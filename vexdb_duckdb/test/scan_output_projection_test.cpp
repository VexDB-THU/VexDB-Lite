#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_helper.hpp"
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace duckdb;

static unique_ptr<MaterializedQueryResult> SQL(Connection &c, const string &s) {
    auto r = c.Query(s);
    if (r->HasError()) throw std::runtime_error(r->GetError());
    return r;
}

static string Quote(const string &s) {
    string result = "'";
    for (char c : s) result += c == '\'' ? "''" : string(1, c);
    return result + "'";
}

int main(int argc, char **argv) {
    if (argc != 3 || std::filesystem::exists(argv[2])) return 2;
    try {
        DBConfig cfg;
        cfg.options.load_extensions = false;
        cfg.SetOptionByName("allow_unsigned_extensions", true);
        DuckDB db(argv[2], &cfg);
        ExtensionHelper::LoadExtension(db, "core_functions");
        Connection run(db), ref(db);
        SQL(run, "LOAD " + Quote(argv[1]));
        SQL(run, "SET threads=2");
        SQL(run, "SET vexdb_brute_force_threshold=0");
        SQL(run, "SET vexdb_ef_search=8192");
        SQL(run, "SET vexdb_pq_refine_k_factor=1000");
        SQL(run, "SET vexdb_filtered_search_mode='auto'");
        SQL(ref, "SET vexdb_brute_force_threshold=1000000");
        const string query = "list_transform(range(64),j->0::FLOAT)::FLOAT[64]";
        idx_t checks = 0;
        for (bool pq : {false, true}) {
            SQL(run, "CREATE TABLE vectors AS SELECT i::INTEGER id,"
                     "list_transform(range(64),j->(i*.001+j*.0001)::FLOAT)::FLOAT[64] embedding,"
                     "i%3<>0 live,CASE WHEN i%7=0 THEN NULL ELSE 'body-'||i::VARCHAR END body "
                     "FROM range(4097)t(i)");
            SQL(run, string("CREATE INDEX ann ON vectors USING GRAPH_INDEX(embedding) WITH "
                            "(metric='l2',m=16,ef_construction=160") +
                     (pq ? ",quantizer='pq',pq_m=8,memory_mode='full'" : "") + ")");
            SQL(run, string("SET vexdb_pq_search_mode='") + (pq ? "pq_only" : "off") + "'");
            for (bool view : {false, true}) {
                const string source = view ? "(SELECT id,upper(body) body,embedding,live FROM vectors WHERE live)v" : "vectors";
                for (const string &tail : {string(" LIMIT 10 OFFSET 3"), string(" LIMIT 2500 OFFSET 3")}) {
                    auto text = "SELECT id,upper(body) AS payload_text,l2_distance(embedding," + query + ") d,"
                                "embedding[1] AS vector_first FROM " + source +
                                " WHERE live AND id%5<>0 ORDER BY l2_distance(embedding," + query + ")" + tail;
                    auto actual = SQL(run, text);
                    auto expected = SQL(ref, text);
                    std::cout << "CASE pq=" << pq << " view=" << view << tail
                              << " actual=" << actual->RowCount() << " expected=" << expected->RowCount()
                              << '\n' << std::flush;
                    if (actual->RowCount() != expected->RowCount()) {
                        std::cerr << text << '\n' << SQL(run, "EXPLAIN " + text)->ToString()
                                  << "ACTUAL\n" << actual->ToString() << "EXPECTED\n" << expected->ToString();
                        throw std::runtime_error("row count differs");
                    }
                    for (idx_t row = 0; row < actual->RowCount(); ++row)
                        for (idx_t col = 0; col < actual->ColumnCount(); ++col)
                            if (!Value::NotDistinctFrom(actual->GetValue(col, row), expected->GetValue(col, row)))
                                throw std::runtime_error("projected scalar/null/offset value differs");
                    if (tail.find("2500") != string::npos && actual->RowCount() <= STANDARD_VECTOR_SIZE)
                        throw std::runtime_error("test did not cover multiple fetch batches");
                    auto plan = SQL(run, "EXPLAIN " + text)->ToString();
                    if (plan.find("VEXDB_INDEX_SCAN") == string::npos ||
                        plan.find("projected inside scan") == string::npos)
                        throw std::runtime_error("expected fused scan not selected");
                    ++checks;
                }
            }
            auto empty = SQL(run, "SELECT id,upper(body) FROM vectors WHERE live AND id<0 "
                                  "ORDER BY l2_distance(embedding," + query + ") LIMIT 10");
            if (empty->RowCount()) throw std::runtime_error("empty predicate returned rows");
            ++checks;
            SQL(run, "DROP TABLE vectors");
        }
        std::cout << "PASS " << checks << " fused scalar/null/view/offset/multiple-batch/empty checks\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
