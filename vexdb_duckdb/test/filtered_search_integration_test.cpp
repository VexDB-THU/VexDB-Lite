#include "duckdb.hpp"

#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace duckdb;
namespace {
unique_ptr<MaterializedQueryResult> Run(Connection &con, const std::string &sql) {
    auto r = con.Query(sql);
    if (!r || r->HasError()) throw std::runtime_error(sql + "\n" + (r ? r->GetError() : "null"));
    return r;
}
const std::string query = "[0.013,0.027,0.039,0.053,0.067,0.079,0.091,0.103]::FLOAT[8]";
std::string Select(const std::string &predicate, int offset = 0, bool scalar = false) {
    return "SELECT id FROM business WHERE " + predicate + " ORDER BY l2_distance(vec," +
           (scalar ? "(SELECT vec FROM query_vectors WHERE id=1)" : query) + ") LIMIT 10 OFFSET " + std::to_string(offset);
}
std::vector<int64_t> IDs(MaterializedQueryResult &result) {
    std::vector<int64_t> ids;
    std::set<int64_t> unique;
    for (idx_t i = 0; i < result.RowCount(); ++i) {
        const auto id = result.GetValue(0, i).GetValue<int64_t>();
        if (!unique.insert(id).second) throw std::runtime_error("duplicate SQL result");
        ids.push_back(id);
    }
    return ids;
}
void Check(Connection &con, const std::string &sql) {
    Run(con, "SET vexdb_brute_force_threshold=1000000");
    const auto expected = IDs(*Run(con, sql));
    Run(con, "SET vexdb_brute_force_threshold=0");
    auto plan = Run(con, "EXPLAIN " + sql)->ToString();
    if (plan.find("VEXDB_INDEX_SCAN") != std::string::npos)
        throw std::runtime_error("filtered query incorrectly used global ANN\n" + plan);
    if (IDs(*Run(con, sql)) != expected) throw std::runtime_error("filtered results differ from exact reference: " + sql);
}
void Suite(Connection &con) {
    for (const auto &mode : {"off", "pq_only", "pq_graph"}) {
        Run(con, "SET vexdb_pq_search_mode='" + std::string(mode) + "'");
        Run(con, "SET vexdb_ef_search=256");
        Run(con, "SET vexdb_pq_refine_k_factor=16");
        for (const auto &predicate : {
                 "id >= 1800", "id % 1000 = 0", "id >= 2045", "id < 0",
                 "tenant IN (3,7) AND (nullable_tag IS NULL OR id % 2 = 0)",
                 "tenant = 9 OR id IN (2044,2046)", "nullable_tag IS NULL", "nullable_tag IS NOT NULL"}) {
            Check(con, Select(predicate));
        }
        Check(con, Select("id >= 1800", 7));
        Check(con, Select("id >= 1800", 0, true));
        auto prepared = con.Prepare("SELECT id FROM business WHERE id >= ? ORDER BY l2_distance(vec," + query + ") LIMIT 10");
        if (prepared->HasError()) throw std::runtime_error(prepared->GetError());
        for (int lower : {1800, 2045, 99999, 0, 1900}) {
            vector<Value> params {Value::INTEGER(lower)};
            auto r = prepared->Execute(params, false);
            if (r->HasError()) throw std::runtime_error(r->GetError());
            auto ids = IDs(r->Cast<MaterializedQueryResult>());
            const size_t count = lower >= 2048 ? 0 : std::min(10, 2048 - lower);
            if (ids.size() != count || (!ids.empty() && ids.front() != lower))
                throw std::runtime_error("prepared parameter reused incorrect predicate");
        }
        Run(con, "SET vexdb_brute_force_threshold=0");
        auto plan = Run(con, "EXPLAIN SELECT id FROM business ORDER BY l2_distance(vec," + query + ") LIMIT 10")->ToString();
        if (plan.find("VEXDB_INDEX_SCAN") == std::string::npos)
            throw std::runtime_error("unfiltered query lost index rewrite");
    }
}
} // namespace
int main(int argc, char **argv) {
    if (argc != 3 || std::filesystem::exists(argv[2])) return 2;
    try {
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        for (int reopen = 0; reopen < 2; ++reopen) {
            DuckDB db(argv[2], &config);
            Connection con(db);
            Run(con, "LOAD '" + StringUtil::Replace(std::filesystem::absolute(argv[1]).string(), "'", "''") + "'");
            Run(con, "SET threads=1");
            if (!reopen) {
                Run(con, "CREATE TABLE business AS SELECT i::INTEGER id, (i%10)::INTEGER tenant, "
                         "CASE WHEN i%3=0 THEN NULL ELSE i%7 END nullable_tag, "
                         "list_transform(range(8),j->(i*0.1+j*0.01)::FLOAT)::FLOAT[8] vec FROM range(2048) t(i)");
                Run(con, "CREATE TABLE query_vectors AS SELECT 1 id," + query + " vec");
                Run(con, "CREATE INDEX business_idx ON business USING GRAPH_INDEX(vec) WITH "
                         "(metric='l2',quantizer='pq',pq_m=2,m=16,threads=1)");
                // Full rows differ, but the first PQ subspace is constant.
                Run(con, "CREATE TABLE sparse AS SELECT i::INTEGER id, "
                         "[0,0,0,0,(i%7)::FLOAT,((i%7)+1)::FLOAT,((i%7)+2)::FLOAT,((i%7)+3)::FLOAT]::FLOAT[8] vec FROM range(1024) t(i)");
                Run(con, "CREATE INDEX sparse_idx ON sparse USING GRAPH_INDEX(vec) WITH "
                         "(metric='l2',quantizer='pq',pq_m=2,threads=1)");
                Run(con, "CREATE TABLE constant_vectors AS SELECT i id,[1,1,1,1,1,1,1,1]::FLOAT[8] vec FROM range(512)t(i)");
                Run(con, "CREATE INDEX constant_idx ON constant_vectors USING GRAPH_INDEX(vec) WITH "
                         "(metric='l2',quantizer='pq',pq_m=2,threads=1)");
            }
            Suite(con);
            if (!reopen) {
                Run(con, "BEGIN");
                Run(con, "DELETE FROM business WHERE id=1800");
                Run(con, "UPDATE business SET tenant=5 WHERE id=1801");
                Check(con, Select("id >= 1800 AND tenant=5"));
                Run(con, "ROLLBACK");
                Run(con, "INSERT INTO business VALUES(9999,9,NULL,[0,0,0,0,0,0,0,0]::FLOAT[8])");
                Check(con, Select("id >= 9000"));
                Run(con, "DELETE FROM business WHERE id=9999");
                Run(con, "CHECKPOINT");
            }
            std::cout << "PASS filtered SQL phase " << reopen << ": all modes, empty/short/offset, AND/OR/IN/NULL, prepared parameters, scalar subquery, unfiltered index\n";
        }
        std::cout << "PASS low-diversity and constant PQ training, transaction/DML and reopen\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
