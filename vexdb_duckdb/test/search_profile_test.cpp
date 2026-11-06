#include "duckdb.hpp"
#include "vex_search_admission.hpp"
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>

using namespace duckdb;
static void Check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
static unique_ptr<MaterializedQueryResult> Exec(Connection &c, const std::string &sql) {
    auto r = c.Query(sql);
    if (!r || r->HasError()) throw std::runtime_error(sql + ": " + (r ? r->GetError() : "null"));
    return r;
}
static std::set<int> IDs(Connection &c, const std::string &sql) {
    auto r = Exec(c, sql);
    std::set<int> ids;
    for (idx_t i = 0; i < r->RowCount(); ++i) ids.insert(r->GetValue(0, i).GetValue<int>());
    Check(ids.size() == 10 && r->RowCount() == 10, "ten distinct results");
    return ids;
}
using Profile = std::map<std::string, Value>;
static Profile Read(Connection &c) {
    auto r = Exec(c, "SELECT vexdb_last_search_profile()");
    auto v = r->GetValue(0, 0);
    Profile p;
    if (v.IsNull()) return p;
    const auto &values = StructValue::GetChildren(v);
    for (idx_t i = 0; i < values.size(); ++i) p.emplace(StructType::GetChildName(v.type(), i), values[i]);
    return p;
}
static uint64_t N(const Profile &p, const char *name) { return p.at(name).GetValue<uint64_t>(); }
static void Totals(const Profile &p) {
    Check(!p.empty(), "missing previous-query profile");
    Check(N(p, "operators") == 1, "one operator");
    Check(N(p, "output_rows") == 10, "output row count");
    Check(N(p, "operator_ns") >= N(p, "search_ns"), "operator contains search");
    Check(N(p, "search_ns") >= N(p, "retry_search_ns"), "retry nested in search");
    Check(N(p, "search_ns") >= N(p, "pq_distance_ns"), "distance nested in search");
    Check(p.at("status").ToString() == "ok", "successful profile status");
}
int main(int argc, char **argv) {
    try {
        Check(argc == 3, "extension and fresh database path required");
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
        DuckDB db(argv[2], &config);
        Connection c(db), other(db);
        Exec(c, "LOAD '" + std::string(argv[1]) + "'");
        for (auto *con : {&c, &other}) {
            Exec(*con, "SET threads=1");
            Exec(*con, "SET vexdb_brute_force_threshold=0");
            Exec(*con, "SET vexdb_ef_search=2048");
            Exec(*con, "SET vexdb_pq_refine_k_factor=1000");
        }
        Exec(c, "CREATE TABLE items AS SELECT i::INTEGER id, [sin(i*.31)::FLOAT,cos(i*.17)::FLOAT,"
                "sin(i*.73)::FLOAT,cos(i*.53)::FLOAT]::FLOAT[4] vec, i%2=0 active FROM range(1024)t(i)");
        Exec(c, "CREATE INDEX idx ON items USING GRAPH_INDEX(vec) WITH(m=8,ef_construction=90,threads=1,quantizer='pq',pq_m=2)");
        const std::string order = " ORDER BY l2_distance(vec,[.2,.1,.7,.3]::FLOAT[4])";
        const auto sql = "SELECT id FROM items" + order + " LIMIT 10";
        const auto exact = IDs(c, "SELECT id FROM items" + order + ",id LIMIT 10");
        for (const auto &mode : {"off", "pq_graph", "pq_only"}) {
            std::cout << "mode=" << mode << std::endl;
            Exec(c, "SET vexdb_pq_search_mode='" + std::string(mode) + "'");
            Check(IDs(c, sql) == exact, "disabled profile exact IDs");
            Check(Read(c).empty(), "disabled profile must be null");
            Exec(c, "SET vexdb_search_profile=true");
            Check(IDs(c, sql) == exact, "profiling preserves exact IDs");
            auto p = Read(c);
            Totals(p);
            Check(N(p, "search_calls") == 1 && N(p, "retry_calls") == 0, "ordinary read never retries");
            if (std::string(mode) != "off") {
                Check(N(p, "pq_codes_scored") > 0 && N(p, "raw_vectors_refined") > 0, "PQ work counts");
                if (std::string(mode) == "pq_only") Check(N(p, "pq_codes_scored") == 1024, "full PQ scan counts every code");
            }
            Check(Read(c).empty(), "previous statement semantics, no stale profile");
            Check(IDs(other, sql) == exact && Read(other).empty(), "connection-local setting and data");
            Exec(c, "SET vexdb_search_profile=false");
        }
        Exec(c, "SET vexdb_search_profile=true");
        std::cout << "filtered" << std::endl;
        Exec(c, "SET vexdb_filtered_search_mode='auto'");
        const auto filtered = "SELECT id FROM items WHERE active" + order + " LIMIT 10";
        const auto filtered_exact = IDs(c, "SELECT id FROM items WHERE active" + order + ",id LIMIT 10");
        Check(Exec(c, "EXPLAIN " + filtered)->ToString().find("VEXDB_INDEX_SCAN") != std::string::npos,
              "filtered test must actually enter the extension operator");
        Check(IDs(c, filtered) == filtered_exact, "filtered exact fallback IDs");
        auto p = Read(c);
        Totals(p);
        Check(N(p,"metadata_rows") == 1024 && N(p,"allowed_rows") == 512, "metadata and allowed counts");
        Check(N(p,"exact_rows_scored") == 512 && N(p,"exact_selected_calls") == 1, "selected exact route");
        Exec(c, "SET vexdb_pq_search_mode='off'");
        Exec(c, "BEGIN");
        Exec(c, "SELECT count(*) FROM items");
        Exec(other, "INSERT INTO items SELECT (1024+i)::INTEGER,[.2,.1,.7,.3]::FLOAT[4],true FROM range(64)t(i)");
        Check(IDs(c, sql) == exact, "stale PQ model preserves the old snapshot");
        Check(Read(c).empty(), "native exact protection must not report a stale ANN profile");
        Exec(c, "ROLLBACK");
        Exec(other, "DELETE FROM items WHERE id>=1024");
        // Exercise visibility retries on an ordinary graph. A quantized graph
        // is deliberately protected by exact SQL after the concurrent insert,
        // which has no ANN work/profile to count.
        Exec(c, "DROP INDEX idx");
        Exec(c, "CREATE INDEX idx ON items USING GRAPH_INDEX(vec) WITH(m=8,ef_construction=90,threads=1)");
        std::cout << "snapshot" << std::endl;
        Exec(c, "BEGIN");
        Exec(c, "SELECT count(*) FROM items");
        auto snapshot_query = c.Prepare(sql);
        Check(!snapshot_query->HasError(), "prepare raw snapshot query before concurrent insert");
        Exec(other, "INSERT INTO items SELECT (1024+i)::INTEGER,[.2,.1,.7,.3]::FLOAT[4],true FROM range(64)t(i)");
        vector<Value> snapshot_parameters;
        auto snapshot_result = snapshot_query->Execute(snapshot_parameters, false);
        Check(!snapshot_result->HasError(), "execute prepared raw snapshot query");
        std::set<int> snapshot_ids;
        auto &snapshot_rows = snapshot_result->Cast<MaterializedQueryResult>();
        for (idx_t i = 0; i < snapshot_rows.RowCount(); ++i)
            snapshot_ids.insert(snapshot_rows.GetValue(0, i).GetValue<int>());
        Check(snapshot_rows.RowCount() == 10 && snapshot_ids == exact, "old snapshot IDs despite closer concurrent inserts");
        p = Read(c);
        Totals(p);
        Check(N(p,"retry_calls") > 0 && N(p,"snapshot_rejected") > 0 && N(p,"retry_search_ns") > 0,
              "visibility retry work is measured");
        std::cout << "retry_calls=" << N(p,"retry_calls") << " rejected=" << N(p,"snapshot_rejected")
                  << " retry_search_ns=" << N(p,"retry_search_ns") << " search_ns=" << N(p,"search_ns") << '\n';
        Exec(c, "ROLLBACK");
        Exec(c, "SET GLOBAL vexdb_search_concurrency=1");
        std::cout << "timeout" << std::endl;
        Exec(c, "SET vexdb_search_queue_timeout_ms=60");
        auto admission = VexSearchAdmission::Get(*c.context->db);
        {
            auto permit = admission->queue.Acquire([] { return uint64_t(1); }, [] { return false; }, 0);
            auto error = c.Query(sql);
            Check(error->HasError(), "queue timeout");
            p = Read(c);
            Check(p.at("status").ToString() == "error" && N(p,"search_calls") == 0 &&
                  N(p,"queue_wait_ns") >= 50000000, "failed acquisition timing and status");
        }
        Check(admission->queue.GetSnapshot().waiting == 0, "no leaked waiter");
        Exec(c, "SET vexdb_search_queue_timeout_ms=0");
        {
            auto permit = admission->queue.Acquire([] { return uint64_t(1); }, [] { return false; }, 0);
            bool cancelled = false;
            std::thread worker([&] { auto r=c.Query(sql); cancelled=r && r->HasError(); });
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(admission->queue.GetSnapshot().waiting!=1 && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            c.Interrupt();worker.join();
            Check(cancelled,"queue cancellation");
            p=Read(c);
            Check(p.at("status").ToString()=="error" && N(p,"search_calls")==0 && N(p,"queue_wait_ns")>0,
                  "cancelled wait timing and status");
        }
        Check(admission->queue.GetSnapshot().waiting == 0, "no waiter after cancellation");
        Check(IDs(c, sql).size() == 10, "recovery after timeout");
        Totals(Read(c));
        Exec(c,"SELECT 42");
        Check(Read(c).empty(), "native SQL must not reuse an old profile");
        std::cout << "PASS profile off/on, independent exact IDs, stages, counters, isolation, snapshot retries, timeout and recovery\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
