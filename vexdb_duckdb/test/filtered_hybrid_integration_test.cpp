// Retain the historical exact-filter regression unchanged and exercise both
// the explicit exact route and the new scalar/graph integration separately.
#define main historical_filtered_regression_main
#include "filtered_search_integration_test.cpp"
#undef main
#include "vex_search_admission.hpp"
#include <atomic>
#include <chrono>
#include <thread>

namespace {
bool test_bidirectional = false;
void CheckHybrid(Connection &con, const std::string &sql, bool require_index = true) {
    Run(con, "SET vexdb_filtered_search_mode='exact'");
    const auto expected = IDs(*Run(con, sql));
    Run(con, "SET vexdb_filtered_search_mode='auto'");
    const auto plan = Run(con, "EXPLAIN " + sql)->ToString();
    const bool indexed = plan.find("VEXDB_INDEX_SCAN") != std::string::npos;
    if ((require_index && !indexed) || (indexed && plan.find("snapshot-visible") == std::string::npos))
        throw std::runtime_error("missing scalar-aware index plan: " + sql + "\n" + plan);
    const auto actual = IDs(*Run(con, sql));
    if (actual != expected) {
        std::cerr << "EXPECTED"; for (auto id : expected) std::cerr << ' ' << id;
        std::cerr << " ACTUAL"; for (auto id : actual) std::cerr << ' ' << id;
        std::cerr << '\n' << Run(con, "SELECT current_setting('vexdb_pq_search_mode')")->ToString();
        throw std::runtime_error("hybrid result differs from exact SQL: " + sql);
    }
}
void HybridSuite(Connection &con) {
    Run(con, "SET threads=4");
    Run(con, "SET vexdb_brute_force_threshold=0");
    // Match the previously qualified global width. The ef256 diagnostic is
    // retained separately: approximate navigation is not an exact SQL oracle.
    Run(con, "SET vexdb_ef_search=1536");
    Run(con, "SET vexdb_pq_refine_k_factor=16");
    for (const auto &mode : {"off", "pq_only", "pq_graph"}) {
        Run(con, "SET vexdb_pq_search_mode='" + std::string(mode) + "'");
        for (const auto &predicate : {"id>=1800", "id%2=0", "id>=2045", "id<0",
                 "tenant IN (3,7) AND (nullable_tag IS NULL OR id%2=0)",
                 "tenant=9 OR id IN (2044,2046)", "nullable_tag IS NULL", "nullable_tag IS NOT NULL"})
            CheckHybrid(con, Select(predicate), std::string(predicate) != "id<0");
        CheckHybrid(con, Select("id%2=0", 7));
        CheckHybrid(con, Select("id>=1800", 0, true), false);
        CheckHybrid(con, "SELECT id FROM business_view WHERE account=9 OR nullable_tag IS NULL ORDER BY l2_distance(embedding," + query + ") LIMIT 10");
        Run(con, "SET vexdb_filtered_search_mode='auto'");
        auto p = con.Prepare("SELECT id FROM business WHERE tenant=? AND id>=? ORDER BY l2_distance(vec," + query + ") LIMIT 10");
        if (p->HasError()) throw std::runtime_error(p->GetError());
        for (const auto &values : {std::pair<int,int>{9,0},{2,2000},{4,2045},{99,0},{0,1900},{9,0}}) {
            vector<Value> params {Value::INTEGER(values.first), Value::INTEGER(values.second)};
            auto r = p->Execute(params, false);
            if (r->HasError()) throw std::runtime_error(r->GetError());
            const auto actual = IDs(r->Cast<MaterializedQueryResult>());
            Run(con, "SET vexdb_filtered_search_mode='exact'");
            const auto expected = IDs(*Run(con, Select("tenant=" + std::to_string(values.first) + " AND id>=" + std::to_string(values.second))));
            Run(con, "SET vexdb_filtered_search_mode='auto'");
            if (actual != expected) throw std::runtime_error("prepared predicate reused old values");
        }
    }
    Run(con, "SET vexdb_filtered_search_mode='auto'");
    const auto view_plan = Run(con, "EXPLAIN SELECT id FROM business_view ORDER BY l2_distance(embedding," + query + ") LIMIT 10")->ToString();
    if (view_plan.find("VEXDB_INDEX_SCAN") == std::string::npos) throw std::runtime_error("transparent view missed index");
    for (const auto &sql : {Select("id>=1800") + "", "SELECT id FROM business WHERE id>=1800 ORDER BY l2_distance(vec," + query + ") NULLS FIRST LIMIT 10"})
        CheckHybrid(con, sql, sql.find("NULLS FIRST") == std::string::npos);
    Run(con, "SET vexdb_filtered_search_mode='exact'");
    if (Run(con, "EXPLAIN SELECT id FROM business_view ORDER BY l2_distance(embedding," + query + ") LIMIT 10")->ToString().find("VEXDB_INDEX_SCAN") != std::string::npos)
        throw std::runtime_error("default/exact mode unexpectedly enabled the new view rewrite");
    if (Run(con, "EXPLAIN " + Select("id%2=0"))->ToString().find("VEXDB_INDEX_SCAN") != std::string::npos)
        throw std::runtime_error("explicit exact mode used ANN");
    const auto expected = IDs(*Run(con, Select("id%2=0")));
    Run(con, "SET vexdb_filtered_search_mode='auto'");
    Run(con, "SET vexdb_ef_search=40");
    auto cached = con.Prepare(Select("id%2=0"));
    Run(con, "SET vexdb_filtered_search_mode='exact'");
    vector<Value> no_parameters;
    auto result = cached->Execute(no_parameters, false);
    if (result->HasError() || IDs(result->Cast<MaterializedQueryResult>()) != expected)
        throw std::runtime_error("prepared plan ignored exact override");
    Run(con, "SET vexdb_ef_search=1536");
}

void NativeEqualitySuite(Connection &con, bool create) {
    // Stored-column equalities should keep native filtering (including ART)
    // when their estimated result set fits the existing search budget.
    if (create) Run(con, "ANALYZE business");
    for (int phase = 0; phase < (create ? 2 : 1); ++phase) {
        if (create && phase) Run(con, "CREATE INDEX business_scalar_idx ON business(tenant)");
        for (int key : {0, 5, 9, 99}) {
            const auto sql = Select("tenant=" + std::to_string(key));
            CheckHybrid(con, sql, false);
            const auto plan = Run(con, "EXPLAIN " + sql)->ToString();
            if (plan.find("VEXDB_INDEX_SCAN") != std::string::npos)
                throw std::runtime_error("selective stored equality bypassed native scalar plan");
        }
    }
    std::cout << "PASS selective stored equality preserves native filtering with/without ART\n";
}

void MetricAndErrorSuite(DuckDB &db, Connection &con, bool create) {
    if (create) {
        Run(con, "CREATE TABLE metric_source AS SELECT i::INTEGER id,[sin(i*.31)::FLOAT,cos(i*.17)::FLOAT,sin(i*.53)::FLOAT,1::FLOAT]::FLOAT[4] vec FROM range(512)t(i)");
        for (const auto &metric : {"cosine", "ip"}) {
            const std::string table = "hybrid_" + std::string(metric);
            Run(con, "CREATE TABLE " + table + " AS SELECT * FROM metric_source");
            Run(con, "CREATE INDEX " + table + "_idx ON " + table + " USING GRAPH_INDEX(vec) WITH(metric='" + metric + "',quantizer='pq',pq_m=1,threads=1)");
        }
        Run(con, "CREATE TABLE metric_large_ip AS SELECT i::INTEGER id,[(i+1)::FLOAT,0::FLOAT,0::FLOAT,0::FLOAT]::FLOAT[4] vec FROM range(8192)t(i)");
        Run(con, "CREATE INDEX metric_large_ip_idx ON metric_large_ip USING GRAPH_INDEX(vec) WITH(metric='ip',quantizer='pq',pq_m=1,threads=1)");
    }
    for (const auto &metric : {"cosine", "ip"}) {
        const auto distance = std::string(metric) == "ip" ? "inner_product" : "cosine_distance";
        const std::string sql = "SELECT id FROM hybrid_" + std::string(metric) + " WHERE id%2=0 ORDER BY " + distance + "(vec,[.2,.3,.5,.7]::FLOAT[4])" + (std::string(metric) == "ip" ? " DESC" : " ASC") + " LIMIT 10";
        for (const auto &mode : {"off", "pq_only", "pq_graph"}) {
            Run(con, "SET vexdb_pq_search_mode='" + std::string(mode) + "'");
            CheckHybrid(con, sql);
        }
    }
    Run(con, "SET vexdb_pq_search_mode='pq_only'");
    CheckHybrid(con, "SELECT id FROM metric_large_ip WHERE id%2=0 ORDER BY inner_product(vec,[1,0,0,0]::FLOAT[4]) DESC LIMIT 10");
    Run(con, "SET vexdb_pq_search_mode='pq_graph'");
    Run(con, "SET vexdb_filtered_search_mode='auto'");
    auto error = con.Query("SELECT id FROM business WHERE CAST('bad-'||id::VARCHAR AS INTEGER)>0 ORDER BY l2_distance(vec," + query + ") LIMIT 10");
    if (!error->HasError()) throw std::runtime_error("predicate conversion error was swallowed");
    auto admission = VexSearchAdmission::Get(*con.context->db);
    if (admission->queue.GetSnapshot().active || admission->queue.GetSnapshot().waiting) throw std::runtime_error("predicate error leaked queue state");
    CheckHybrid(con, Select("id%2=0"));
    std::cout << "PASS filtered cosine/IP, error preservation, prepared exact override\n";
}

void DenseAndConcurrent(DuckDB &db, Connection &con, Connection &writer, bool create) {
    if (create) {
        Run(con, "CREATE TABLE dense_business AS SELECT i::INTEGER id,(i%10)::INTEGER tenant,"
                 "list_transform(range(8),j->(i*0.1+j*0.01)::FLOAT)::FLOAT[8] vec FROM range(8192)t(i)");
        Run(con, "CREATE INDEX dense_idx ON dense_business USING GRAPH_INDEX(vec) WITH(quantizer='pq',pq_m=2,threads=1)");
    } else {
        Run(con, "PRAGMA vexdb_rebuild_index('dense_idx')");
    }
    const auto sql = "SELECT id FROM dense_business WHERE tenant IN(1,2,3,4,5,6,7,8,9) AND id%2=0 ORDER BY l2_distance(vec," + query + ") LIMIT 10";
    Run(con, "SET vexdb_pq_search_mode='pq_graph'");
    CheckHybrid(con, sql);
    Run(con, "BEGIN"); Run(con, "SELECT count(*) FROM dense_business");
    Run(writer, "UPDATE dense_business SET tenant=9 WHERE id%3=0");
    CheckHybrid(con, sql, false);
    Run(writer, "DELETE FROM dense_business WHERE id<50");
    CheckHybrid(con, sql, false);
    Run(con, "COMMIT"); CheckHybrid(con, sql, false);
    std::vector<std::string> errors(8);
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) readers.emplace_back([&, i] {
        try {
            Connection reader(db);
            Run(reader, "SET vexdb_graph_bidirectional_search=" + std::string(test_bidirectional ? "true" : "false"));
            Run(reader, "SET threads=2"); Run(reader, "SET vexdb_ef_search=1536");
            Run(reader, "SET vexdb_brute_force_threshold=0"); Run(reader, "SET vexdb_pq_search_mode='pq_graph'");
            Run(reader, "SET vexdb_pq_refine_k_factor=16");
            for (int n = 0; n < 8; ++n) {
                Run(reader, "BEGIN"); Run(reader, "SELECT count(*) FROM dense_business");
                CheckHybrid(reader, sql, false); Run(reader, "COMMIT");
            }
        } catch (const std::exception &e) { errors[i] = e.what(); }
    });
    for (int n = 0; n < 16; ++n) Run(writer, "UPDATE dense_business SET tenant=(tenant+1)%10 WHERE id%17=" + std::to_string(n));
    for (auto &reader : readers) reader.join();
    for (const auto &error : errors) if (!error.empty()) throw std::runtime_error(error);
    std::cout << "PASS fresh dense PQ graph and stale-model exact protection: old snapshots and 8 readers with writer\n";
}
template <class F> void Wait(F predicate) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > until) std::abort();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
}

int main(int argc, char **argv) {
    if ((argc != 3 && argc != 4) || std::filesystem::exists(argv[2])) return 2;
    try {
        if (argc == 4 && std::string(argv[3]) != "bidirectional") return 2;
        test_bidirectional = argc == 4;
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        for (int reopen = 0; reopen < 2; ++reopen) {
            DuckDB db(argv[2], &config);
            Connection con(db), writer(db);
            Run(con, "LOAD " + std::string("'") + StringUtil::Replace(std::filesystem::absolute(argv[1]).string(), "'", "''") + "'");
            Run(con, "SET vexdb_graph_bidirectional_search=" + std::string(test_bidirectional ? "true" : "false"));
            if (!reopen) {
                Run(con, "CREATE TABLE business AS SELECT CASE WHEN i%3=0 THEN NULL ELSE i%7 END nullable_tag, (i%10)::INTEGER tenant, "
                         "list_transform(range(8),j->(i*0.1+j*0.01)::FLOAT)::FLOAT[8] vec,i::INTEGER id FROM range(2048)t(i)");
                Run(con, "CREATE INDEX business_idx ON business USING GRAPH_INDEX(vec) WITH(quantizer='pq',pq_m=2,m=16,threads=1)");
                Run(con, "CREATE TABLE query_vectors AS SELECT 1 id," + query + " vec");
                Run(con, "CREATE VIEW business_view AS SELECT tenant account,nullable_tag,vec embedding,id FROM business");
            } else {
                Run(con, "PRAGMA vexdb_rebuild_index('business_idx')");
            }
            HybridSuite(con);
            NativeEqualitySuite(con, !reopen);
            MetricAndErrorSuite(db, con, !reopen);
            DenseAndConcurrent(db, con, writer, !reopen);
            Run(con, "SET vexdb_filtered_search_mode='auto'");
            Run(con, "BEGIN");
            Run(con, "SELECT count(*) FROM business");
            Run(writer, "UPDATE business SET tenant=8 WHERE id%10=9 AND id<100");
            CheckHybrid(con, Select("tenant=9"), false);
            Run(con, "COMMIT");
            CheckHybrid(con, Select("tenant=9"), false);
            Run(writer, "UPDATE business SET tenant=(id%10)::INTEGER");
            Run(con, "BEGIN");
            Run(con, "UPDATE business SET tenant=9 WHERE id=0");
            CheckHybrid(con, Select("tenant=9"), false);
            Run(con, "ROLLBACK");
            Run(con, "PRAGMA vexdb_rebuild_index('business_idx')");
            auto admission = VexSearchAdmission::Get(*con.context->db);
            Run(con, "SET GLOBAL vexdb_search_concurrency=1");
            for (bool timeout : {false, true}) {
                Connection waiter(db);
                Run(waiter, "SET vexdb_filtered_search_mode='auto'");
                Run(waiter, "SET vexdb_graph_bidirectional_search=" + std::string(test_bidirectional ? "true" : "false"));
                Run(waiter, "SET vexdb_brute_force_threshold=0");
                Run(waiter, "SET vexdb_pq_search_mode='pq_graph'");
                Run(waiter, "SET vexdb_ef_search=1536");
                Run(waiter, "SET vexdb_pq_refine_k_factor=16");
                Run(waiter, "SET vexdb_search_queue_timeout_ms=" + std::string(timeout ? "100" : "0"));
                std::string error;
                {
                    auto held = admission->Acquire(*con.context);
                    std::thread task([&] { auto r = waiter.Query(Select("id%2=0")); error = r->HasError() ? r->GetError() : "unexpected success"; });
                    Wait([&] { return admission->queue.GetSnapshot().waiting == 1; });
                    if (!timeout) waiter.Interrupt();
                    task.join();
                }
                if (error.find(timeout ? "timed out" : "Interrupt") == std::string::npos) throw std::runtime_error(error);
                if (admission->queue.GetSnapshot().active || admission->queue.GetSnapshot().waiting) throw std::runtime_error("hybrid leaked permit");
                CheckHybrid(waiter, Select("id%2=0"));
            }
            Run(con, "SET GLOBAL vexdb_search_concurrency=0");
            Run(con, "CHECKPOINT");
            std::cout << "PASS hybrid SQL phase=" << reopen << " filters, modes, bindings, parameters, snapshot, cancel/timeout\n";
        }
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
