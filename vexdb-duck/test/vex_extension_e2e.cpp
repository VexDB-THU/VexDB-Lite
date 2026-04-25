#include "duckdb.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace duckdb;

namespace {

static unique_ptr<QueryResult> ExecOrThrow(Connection &con, const string &sql) {
    auto result = con.Query(sql);
    if (!result) {
        throw std::runtime_error("query returned null result: " + sql);
    }
    if (result->HasError()) {
        throw std::runtime_error("query failed: " + sql + "\nerror: " + result->GetError());
    }
    return result;
}

static std::vector<string> ReadFirstColumn(Connection &con, const string &sql) {
    auto result = ExecOrThrow(con, sql);
    auto &materialized = result->Cast<MaterializedQueryResult>();
    std::vector<string> values;
    for (idx_t row = 0; row < materialized.RowCount(); row++) {
        values.push_back(materialized.GetValue(0, row).ToString());
    }
    return values;
}

static string ReadExplain(Connection &con, const string &sql) {
    auto result = ExecOrThrow(con, sql);
    auto &materialized = result->Cast<MaterializedQueryResult>();
    string plan;
    for (idx_t row = 0; row < materialized.RowCount(); row++) {
        for (idx_t col = 0; col < materialized.ColumnCount(); col++) {
            plan += materialized.GetValue(col, row).ToString();
            plan += "\n";
        }
    }
    return plan;
}

static void RequireEqual(const std::vector<string> &actual, const std::vector<string> &expected, const string &what) {
    if (actual == expected) {
        return;
    }
    std::cerr << what << " mismatch\nexpected:";
    for (auto &v : expected) {
        std::cerr << " " << v;
    }
    std::cerr << "\nactual:";
    for (auto &v : actual) {
        std::cerr << " " << v;
    }
    std::cerr << std::endl;
    std::exit(1);
}

static void RequireContains(const string &haystack, const string &needle, const string &what) {
    if (haystack.find(needle) != string::npos) {
        return;
    }
    std::cerr << what << " missing token '" << needle << "'\n" << haystack << std::endl;
    std::exit(1);
}

} // namespace

int main() {
    try {
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        DuckDB db(nullptr, &config);
        Connection con(db);

        ExecOrThrow(con, "LOAD '" VEX_E2E_EXTENSION_PATH "'");
        ExecOrThrow(con, "PRAGMA enable_verification");
        ExecOrThrow(con, "PRAGMA explain_output='optimized_only'");

        ExecOrThrow(con, "CREATE TABLE items(id INTEGER, cat INTEGER, vec FLOAT[3])");
        ExecOrThrow(con,
                    "INSERT INTO items VALUES "
                    "(1, 1, [0.0, 0.0, 0.0]::FLOAT[3]),"
                    "(2, 1, [0.1, 0.0, 0.0]::FLOAT[3]),"
                    "(3, 2, [9.0, 0.0, 0.0]::FLOAT[3]),"
                    "(4, 1, [0.2, 0.0, 0.0]::FLOAT[3]),"
                    "(5, 2, [9.2, 0.0, 0.0]::FLOAT[3])");

        ExecOrThrow(con,
                    "CREATE INDEX items_vec_idx ON items USING GRAPH_INDEX (vec, cat) "
                    "WITH (threads=4, m=8, ef_construction=32)");

        auto explain_before = ReadExplain(
            con,
            "EXPLAIN SELECT id FROM items "
            "WHERE cat = 1 "
            "ORDER BY vec <-> [0.05, 0.0, 0.0]::FLOAT[3] "
            "LIMIT 2");
        RequireContains(explain_before, "VEX_INDEX_SCAN", "pre-delete explain plan");

        auto before_delete = ReadFirstColumn(
            con,
            "SELECT id FROM items "
            "WHERE cat = 1 "
            "ORDER BY vec <-> [0.05, 0.0, 0.0]::FLOAT[3] "
            "LIMIT 2");
        RequireEqual(before_delete, {"1", "2"}, "pre-delete ANN result");

        ExecOrThrow(con, "DELETE FROM items WHERE id = 1");

        auto explain_after = ReadExplain(
            con,
            "EXPLAIN SELECT id FROM items "
            "WHERE cat = 1 "
            "ORDER BY vec <-> [0.05, 0.0, 0.0]::FLOAT[3] "
            "LIMIT 2");
        RequireContains(explain_after, "VEX_INDEX_SCAN", "post-delete explain plan");

        auto after_delete = ReadFirstColumn(
            con,
            "SELECT id FROM items "
            "WHERE cat = 1 "
            "ORDER BY vec <-> [0.05, 0.0, 0.0]::FLOAT[3] "
            "LIMIT 2");
        RequireEqual(after_delete, {"2", "4"}, "post-delete ANN result");

        auto non_filtered = ReadFirstColumn(
            con,
            "SELECT id FROM items "
            "ORDER BY vec <-> [9.1, 0.0, 0.0]::FLOAT[3] "
            "LIMIT 2");
        RequireEqual(non_filtered, {"3", "5"}, "post-delete unfiltered ANN result");
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }

    return 0;
}
