// A reusable vector request must not retain statistics-based filter decisions
// after another connection commits rows. This is a small correctness fixture.
#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace duckdb;

namespace {
unique_ptr<MaterializedQueryResult> SQL(Connection &con, const std::string &sql) {
    auto result = con.Query(sql);
    if (result->HasError()) throw std::runtime_error(result->GetError());
    return result;
}
std::string Quote(const std::string &s) {
    std::string result = "'";
    for (char ch : s) result += ch == '\'' ? "''" : std::string(1, ch);
    return result + "'";
}
std::string Snapshot(MaterializedQueryResult &result) {
    std::ostringstream out;
    for (idx_t row = 0; row < result.RowCount(); ++row) {
        out << result.GetValue(0, row).ToString() << ':' << result.GetValue(1, row).ToString() << '|';
    }
    return out.str();
}
void Options(Connection &con, bool pq, bool fetch, bool automatic) {
    SQL(con, "SET threads=2");
    SQL(con, "SET vexdb_brute_force_threshold=0");
    SQL(con, "SET vexdb_ef_search=2048");
    SQL(con, "SET vexdb_pq_refine_k_factor=1000");
    SQL(con, std::string("SET vexdb_pq_search_mode='") + (pq ? "pq_only" : "off") + "'");
    SQL(con, std::string("SET vexdb_filtered_search_mode='") + (automatic ? "auto" : "exact") + "'");
    SQL(con, std::string("SET vexdb_native_topn_fetch=") + (fetch ? "true" : "false"));
}
}

int main(int argc, char **argv) {
    if (argc != 3 || std::filesystem::exists(argv[2])) return 2;
    try {
        DBConfig cfg;
        cfg.SetOptionByName("allow_unsigned_extensions", true);
        cfg.options.load_extensions = false;
        DuckDB db(argv[2], &cfg);
            ExtensionHelper::LoadExtension(db, "core_functions");
        Connection writer(db), reader(db), reference(db);
        SQL(writer, "LOAD " + Quote(argv[1]));
        size_t checks = 0;
        for (bool pq : {false, true}) for (bool fetch : {false, true}) {
            for (bool automatic : {false, true}) for (bool old_snapshot : {false, true}) {
                const std::string table = "pruned_" + std::to_string(checks);
                SQL(writer, "CREATE TABLE " + table + " AS SELECT i::INTEGER id, "
                    "[v,v,v,v,v,v,v,v]::FLOAT[8] vec,14::INTEGER route,true AS live,'body-'||i::VARCHAR body "
                    "FROM (SELECT i,(1+i*.01)::FLOAT v FROM range(512)t(i))s");
                SQL(writer, "CREATE INDEX " + table + "_ann ON " + table + " USING GRAPH_INDEX(vec) WITH "
                    "(m=16,ef_construction=160" + (pq ? ",quantizer='pq',pq_m=4,memory_mode='full'" : "") + ")");
                SQL(writer, "ANALYZE " + table);
                Options(reader, pq, fetch, automatic);
                if (old_snapshot) SQL(reader, "BEGIN");
                const auto request_sql = "SELECT id,body FROM " + table +
                    " WHERE route=$2 AND live ORDER BY l2_distance(vec,$1::FLOAT[8]) LIMIT 10";
                auto present = reader.Prepare(request_sql);
                auto absent = reader.Prepare(request_sql);
                if (present->HasError()) throw std::runtime_error(present->GetError());
                if (absent->HasError()) throw std::runtime_error(absent->GetError());
                auto run = [&](int route) {
                    vector<Value> zero(8, Value::FLOAT(0));
                    vector<Value> parameters{Value::ARRAY(LogicalType::FLOAT, std::move(zero)), Value::INTEGER(route)};
                    auto &statement = route == 14 ? present : absent;
                    auto result = statement->Execute(parameters, false);
                    if (result->HasError()) throw std::runtime_error(result->GetError());
                    return Snapshot(result->Cast<MaterializedQueryResult>());
                };
                auto exact = [&](int route) {
                    // Built-in arithmetic keeps the reference outside the Vex
                    // distance rewrite. All eight coordinates are equal here.
                    auto result = SQL(reference, "SELECT id,body FROM " + table + " WHERE route=" +
                        std::to_string(route) + " AND live ORDER BY "
                        "vec[1]*vec[1]+vec[2]*vec[2]+vec[3]*vec[3]+vec[4]*vec[4]+"
                        "vec[5]*vec[5]+vec[6]*vec[6]+vec[7]*vec[7]+vec[8]*vec[8] LIMIT 10");
                    return Snapshot(*result);
                };
                const auto old_true = exact(14), old_false = exact(15);
                if (run(14) != old_true || run(15) != old_false) throw std::runtime_error("initial parameter mismatch");
                SQL(writer, "BEGIN");
                SQL(writer, "INSERT INTO " + table + " SELECT (10000+i)::INTEGER,[v,v,v,v,v,v,v,v]::FLOAT[8],"
                    "15,true,'body-new-'||i::VARCHAR FROM (SELECT i,(i*.001)::FLOAT v FROM range(20)t(i))s");
                SQL(writer, "UPDATE " + table + " SET live=false WHERE id<10");
                SQL(writer, "COMMIT");
                if (old_snapshot) {
                    if (run(14) != old_true || run(15) != old_false) throw std::runtime_error("old snapshot filter changed");
                    checks += 2;
                    SQL(reader, "COMMIT");
                }
                for (int route : {14, 15}) {
                    const auto expected = exact(route), actual = run(route);
                    if (actual != expected) throw std::runtime_error("stale filter after write: " + table +
                        " route=" + std::to_string(route) + " expected=" + expected + " actual=" + actual);
                    ++checks;
                }
                SQL(writer, "DROP TABLE " + table);
            }
        }
        std::cout << "PASS " << checks << " prepared filter/snapshot checks\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
