#include "duckdb.hpp"
#include "vex_search_admission.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace duckdb;
using namespace std::chrono_literals;
static void Require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
static unique_ptr<MaterializedQueryResult> Exec(Connection &c, const std::string &sql) {
    auto r = c.Query(sql);
    if (!r || r->HasError()) throw std::runtime_error(sql + ": " + (r ? r->GetError() : "null result"));
    return r;
}
static std::set<int> IDs(Connection &c, const std::string &sql) {
    auto r = Exec(c, sql);
    std::set<int> ids;
    for (idx_t i = 0; i < r->RowCount(); ++i) ids.insert(r->GetValue(0, i).GetValue<int>());
    Require(ids.size() == r->RowCount(), "duplicate IDs");
    return ids;
}
template <class P> static void Until(P pred) {
    auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) std::abort();
        std::this_thread::sleep_for(1ms);
    }
}
static const std::string distance = "l2_distance(vec, [0.2,0.1,0.7,0.3]::FLOAT[4])";
static const std::string query = "SELECT id FROM items ORDER BY " + distance + " LIMIT 10";
static const std::string exact = "SELECT id FROM items ORDER BY " + distance + ", id LIMIT 10";
static void Settings(Connection &c, const std::string &mode) {
    Exec(c, "SET threads=1");
    Exec(c, "SET vexdb_brute_force_threshold=0");
    Exec(c, "SET vexdb_ef_search=2048");
    Exec(c, "SET vexdb_pq_search_mode='" + mode + "'");
    Exec(c, "SET vexdb_pq_refine_k_factor=1000");
}
static bool IndexPlan(Connection &c, const std::string &sql) {
    return Exec(c, "EXPLAIN " + sql)->ToString().find("VEXDB_INDEX_SCAN") != std::string::npos;
}
static void Idle(const shared_ptr<VexSearchAdmission> &a) {
    auto s = a->queue.GetSnapshot();
    Require(s.active == 0 && s.waiting == 0, "permit/waiter leaked");
}

static void DeleteAndReopen(const std::string &extension, const std::string &path, bool compact) {
    {
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
        DuckDB db(path, &config);
        Connection con(db);
        Exec(con, "LOAD '" + extension + "'");
        Settings(con, "pq_only");
        Exec(con, "CREATE TABLE items AS SELECT i::INTEGER id, "
                  "[sin(i*0.31)::FLOAT,cos(i*0.17)::FLOAT,sin(i*0.73)::FLOAT,cos(i*0.53)::FLOAT]::FLOAT[4] vec "
                  "FROM range(1024)t(i)");
        Exec(con, "CREATE INDEX items_idx ON items USING GRAPH_INDEX(vec) "
                  "WITH(metric='l2',m=8,ef_construction=90,threads=1,quantizer='pq',pq_m=2" +
                  std::string(compact ? ",memory_mode='compact')" : ")"));
        Exec(con, "DELETE FROM items WHERE id>=10");
        Exec(con, "CHECKPOINT");
    }
    DBConfig config;
    config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
    DuckDB db(path, &config);
    Connection con(db);
    Exec(con, "LOAD '" + extension + "'");
    const auto expected = IDs(con, exact);
    Require(expected.size() == 10, "pruned table reference");
    for (const auto &mode : {"off", "pq_graph", "pq_only"}) {
        if (compact && std::string(mode) != "pq_only") continue;
        Settings(con, mode);
        Require(IndexPlan(con, query), "delete/reopen unexpectedly fell back to table scan");
        Require(IDs(con, query) == expected, "deleted PQ codes exhausted backfill before live rows after reopen");
    }
    std::cout << "PASS delete1014/reopen, " << (compact ? "compact PQ" : "all three modes")
              << ", ten visible rows preserved\n";
}

int main(int argc, char **argv) {
    try {
        Require(argc == 3, "usage: test <extension> <new_database>");
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
        DuckDB db(argv[2], &config);
        Connection admin(db), waiting(db), writer(db), observer(db);
        Exec(admin, "LOAD '" + std::string(argv[1]) + "'");
        Settings(admin, "pq_graph");
        Exec(admin, "CREATE TABLE items AS SELECT i::INTEGER id, "
                    "[sin(i*0.31)::FLOAT,cos(i*0.17)::FLOAT,sin(i*0.73)::FLOAT,cos(i*0.53)::FLOAT]::FLOAT[4] vec "
                    "FROM range(1024) t(i)");
        Exec(admin, "CREATE INDEX items_idx ON items USING GRAPH_INDEX(vec) "
                    "WITH (metric='l2', m=8, ef_construction=90, threads=1, quantizer='pq', pq_m=2)");
        auto admission = VexSearchAdmission::Get(*admin.context->db);
        for (const auto &mode : {"off", "pq_graph", "pq_only"}) {
            // The previous mode deliberately writes and rolls back. Its model
            // stays stale until rebuild, so start each mode with a fresh model
            // to exercise PQ admission and invalid-refine validation itself.
            Exec(admin, "PRAGMA vexdb_rebuild_index('items_idx')");
            Settings(admin, mode); Settings(waiting, mode); Settings(writer, mode); Settings(observer, mode);
            Exec(admin, "SET GLOBAL vexdb_search_concurrency=1");
            Exec(waiting, "SET vexdb_search_queue_timeout_ms=0");
            Require(IndexPlan(waiting, query), "expected actual index search");
            const auto expected = IDs(admin, exact);
            Require(IDs(waiting, query) == expected, "uncontended recall");
            std::string error;
            // Hold a real database permit from the harness to deterministically
            // exercise the product wait path without sleeps/debug SQL in product.
            {
                auto held = admission->Acquire(*admin.context);
                Exec(waiting, "BEGIN");
                std::thread task([&] {
                    auto r = waiting.Query(query);
                    error = r->HasError() ? r->GetError() : "unexpected success";
                });
                Until([&] { return admission->queue.GetSnapshot().waiting == 1; });
                waiting.Interrupt();
                task.join();
                Require(error.find("Interrupt") != std::string::npos, "cancel not propagated: " + error);
                Require(admission->queue.GetSnapshot().active == 1, "cancel released holder");
                Exec(waiting, "ROLLBACK");
            }
            Idle(admission);
            Require(IDs(waiting, query) == expected, "cancelled session not reusable");
            {
                auto held = admission->Acquire(*admin.context);
                Exec(waiting, "SET vexdb_search_queue_timeout_ms=30");
                Exec(waiting, "BEGIN");
                const auto begin = std::chrono::steady_clock::now();
                auto r = waiting.Query(query);
                Require(r->HasError() && r->GetError().find("queue wait timed out") != std::string::npos,
                        "timeout not propagated");
                Require(std::chrono::steady_clock::now() - begin >= 30ms, "early queue timeout");
                Exec(waiting, "ROLLBACK");
                // A strong filter uses exact SQL and must not wait for ANN slots.
                const auto filtered = "SELECT id FROM items WHERE id%1000=0 ORDER BY " + distance + " LIMIT 10";
                Require(!IndexPlan(observer, filtered), "filter unexpectedly used ANN queue");
                Require(IDs(observer, filtered).size() == 2, "filtered SQL blocked or lost rows");
            }
            Idle(admission);
            Exec(waiting, "SET vexdb_search_queue_timeout_ms=0");
            Require(IDs(waiting, query) == expected, "timed-out session not reusable");

            // Exceptions thrown after admission release their own permit.
            Exec(waiting, "SET vexdb_pq_refine_k_factor='NaN'");
            if (std::string(mode) != "off") Require(waiting.Query(query)->HasError(), "expected refine failure");
            Settings(waiting, mode);
            Idle(admission);

            // A queued transaction must not stop unrelated DML or hold a graph
            // lock. The optimizer keeps read-your-writes on its exact SQL path.
            Exec(waiting, "BEGIN");
            std::set<int> queued;
            error.clear();
            {
                auto held = admission->Acquire(*admin.context);
                std::thread task([&] {
                    try { queued = IDs(waiting, query); }
                    catch (const std::exception &e) { error = e.what(); }
                });
                Until([&] { return admission->queue.GetSnapshot().waiting == 1; });
                Exec(writer, "BEGIN");
                Exec(writer, "INSERT INTO items VALUES (20000,[10,10,10,10]::FLOAT[4])");
                Exec(writer, "DELETE FROM items WHERE id=20000");
                Exec(writer, "COMMIT");
                Exec(observer, "SELECT count(*) FROM items");
                // Disabling the cap wakes the waiter through its config poll,
                // while the pre-existing holder is still accounted for.
                Exec(admin, "SET GLOBAL vexdb_search_concurrency=0");
                task.join();
            }
            Require(error.empty(), error);
            Require(queued == expected, "queued transaction changed results");
            Exec(waiting, "ROLLBACK");
            Idle(admission);
            Exec(waiting, "BEGIN");
            Exec(waiting, "INSERT INTO items VALUES (20001,[0.2,0.1,0.7,0.3]::FLOAT[4])");
            Require(!IndexPlan(waiting, query), "read-your-writes must use transactional scan");
            Require(IDs(waiting, query).count(20001), "lost own insert");
            Require(!IDs(observer, exact).count(20001), "dirty read");
            Exec(waiting, "ROLLBACK");
            Require(IDs(waiting, query) == expected, "rollback changed search");

            // Keep a snapshot open before another transaction inserts a nearer
            // row. The queued search must still return ten snapshot-visible IDs.
            Exec(admin, "SET GLOBAL vexdb_search_concurrency=1");
            Exec(waiting, "BEGIN");
            Require(IDs(waiting, exact) == expected, "snapshot setup");
            error.clear(); queued.clear();
            {
                auto held = admission->Acquire(*admin.context);
                std::thread task([&] {
                    try { queued = IDs(waiting, query); }
                    catch (const std::exception &e) { error = e.what(); }
                });
                Until([&] { return admission->queue.GetSnapshot().waiting == 1; });
                Exec(writer, "INSERT INTO items SELECT i,[0.2,0.1,0.7,0.3]::FLOAT[4] FROM range(20002,20066) t(i)");
                Exec(admin, "SET GLOBAL vexdb_search_concurrency=0");
                task.join();
            }
            Require(error.empty(), error);
            Require(queued == expected, "queued historical snapshot lost visible neighbors after concurrent insert");
            Exec(waiting, "COMMIT");
            auto new_rows = IDs(observer, query);
            Require(new_rows.size() == 10 && *new_rows.begin() >= 20002, "committed insert invisible to new reader");
            Exec(writer, "DELETE FROM items WHERE id>=20002");
            const auto removed = *expected.begin();
            Exec(writer, "CREATE TEMP TABLE saved_row AS SELECT * FROM items WHERE id=" + std::to_string(removed));
            Exec(admin, "SET GLOBAL vexdb_search_concurrency=1");
            Exec(waiting, "BEGIN");
            Require(IDs(waiting, exact) == expected, "delete snapshot setup");
            queued.clear(); error.clear();
            {
                auto held = admission->Acquire(*admin.context);
                std::thread task([&] {
                    try { queued = IDs(waiting, query); }
                    catch (const std::exception &e) { error = e.what(); }
                });
                Until([&] { return admission->queue.GetSnapshot().waiting == 1; });
                Exec(writer, "DELETE FROM items WHERE id=" + std::to_string(removed));
                Exec(admin, "SET GLOBAL vexdb_search_concurrency=0");
                task.join();
            }
            Require(error.empty(), error);
            Require(queued == expected, "historical snapshot lost concurrently deleted neighbor");
            Exec(waiting, "COMMIT");
            Require(!IDs(observer, query).count(removed), "new reader saw committed delete");
            Exec(writer, "INSERT INTO items SELECT * FROM saved_row");
            Exec(writer, "DROP TABLE saved_row");
            std::cout << "PASS " << mode << " cancel/timeout/reuse/error/DML/transaction/disable\n";
        }
        for (const auto &sql : {"SET SESSION vexdb_search_concurrency=2", "SET vexdb_search_concurrency=2",
                               "SET GLOBAL vexdb_search_concurrency=-2", "SET GLOBAL vexdb_search_concurrency=NULL",
                               "SET vexdb_search_queue_timeout_ms=NULL", "SET vexdb_search_queue_timeout_ms=-1"}) {
            Require(admin.Query(sql)->HasError(), "invalid setting accepted: " + std::string(sql));
        }
        Exec(admin, "SET GLOBAL vexdb_search_concurrency=-1");
        Require(admission->GetLimit(*admin.context->db) == std::max(1u, std::thread::hardware_concurrency()), "auto CPU");
        Exec(admin, "RESET GLOBAL vexdb_search_concurrency");
        Require(admission->GetLimit(*admin.context->db) == 0, "global reset");
        DBConfig second_config;
        second_config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
        DuckDB second(nullptr, &second_config);
        Connection other(second);
        Exec(other, "LOAD '" + std::string(argv[1]) + "'");
        auto independent = VexSearchAdmission::Get(*other.context->db);
        Require(independent.get() != admission.get(), "gate shared across databases");
        Exec(other, "SET GLOBAL vexdb_search_concurrency=3");
        Require(independent->GetLimit(*other.context->db) == 3 && admission->GetLimit(*admin.context->db) == 0,
                "database settings interfere");
        const auto state = Exec(admin, "SELECT vexdb_search_queue_state()")->ToString();
        std::cout << state << "PASS global scope, CPU auto, reset, database isolation\n";
        Idle(admission);
        DeleteAndReopen(argv[1], std::string(argv[2]) + "-pruned-full", false);
        DeleteAndReopen(argv[1], std::string(argv[2]) + "-pruned-compact", true);
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
