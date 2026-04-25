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
        result->ThrowError("query failed: " + sql + "\n");
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
    throw std::runtime_error(what + " mismatch");
}

static void RequireContains(const string &haystack, const string &needle, const string &what) {
    if (haystack.find(needle) != string::npos) {
        return;
    }
    throw std::runtime_error(what + " missing token '" + needle + "'\nplan:\n" + haystack);
}

static Connection OpenWithExtension(DuckDB &db, const string &extension_path) {
    Connection con(db);
    ExecOrThrow(con, "LOAD '" + extension_path + "'");
    ExecOrThrow(con, "PRAGMA enable_verification");
    ExecOrThrow(con, "PRAGMA explain_output='optimized_only'");
    ExecOrThrow(con, "SET threads TO 4");
    return con;
}

static void TestLoadAndBasicQuery(const string &extension_path) {
    DBConfig config;
    config.SetOptionByName("allow_unsigned_extensions", true);
    DuckDB db(nullptr, &config);
    auto con = OpenWithExtension(db, extension_path);

    ExecOrThrow(con, "CREATE TABLE items(id INTEGER, vec FLOAT[3])");
    ExecOrThrow(con,
                "INSERT INTO items VALUES "
                "(1, [0.0, 0.0, 0.0]::FLOAT[3]),"
                "(2, [0.1, 0.0, 0.0]::FLOAT[3]),"
                "(3, [1.0, 0.0, 0.0]::FLOAT[3])");

    auto rows = ReadFirstColumn(
        con,
        "SELECT id FROM items "
        "ORDER BY l2_distance(vec, [0.05, 0.0, 0.0]::FLOAT[3]) "
        "LIMIT 2");
    RequireEqual(rows, {"1", "2"}, "basic query result");
}

static void TestCreateIndexAndANN(const string &extension_path) {
    DBConfig config;
    config.SetOptionByName("allow_unsigned_extensions", true);
    DuckDB db(nullptr, &config);
    auto con = OpenWithExtension(db, extension_path);

    ExecOrThrow(con, "CREATE TABLE items(id INTEGER, vec FLOAT[3])");
    ExecOrThrow(con,
                "INSERT INTO items VALUES "
                "(1, [0.0, 0.0, 0.0]::FLOAT[3]),"
                "(2, [0.1, 0.0, 0.0]::FLOAT[3]),"
                "(3, [9.0, 0.0, 0.0]::FLOAT[3]),"
                "(4, [0.2, 0.0, 0.0]::FLOAT[3]),"
                "(5, [9.2, 0.0, 0.0]::FLOAT[3])");

    ExecOrThrow(con, "CREATE INDEX items_vec_idx ON items USING GRAPH_INDEX (vec)");

    auto rows = ReadFirstColumn(
        con,
        "SELECT id FROM items "
        "ORDER BY l2_distance(vec, [0.05, 0.0, 0.0]::FLOAT[3]) "
        "LIMIT 3");
    RequireEqual(rows, {"1", "2", "4"}, "indexed ANN result");
}

static void TestExplainANNPlan(const string &extension_path) {
    DBConfig config;
    config.SetOptionByName("allow_unsigned_extensions", true);
    DuckDB db(nullptr, &config);
    auto con = OpenWithExtension(db, extension_path);

    ExecOrThrow(con, "CREATE TABLE items(id INTEGER, cat INTEGER, vec FLOAT[3])");
    ExecOrThrow(con,
                "INSERT INTO items VALUES "
                "(1, 1, [0.0, 0.0, 0.0]::FLOAT[3]),"
                "(2, 1, [0.1, 0.0, 0.0]::FLOAT[3]),"
                "(3, 2, [9.0, 0.0, 0.0]::FLOAT[3]),"
                "(4, 1, [0.2, 0.0, 0.0]::FLOAT[3]),"
                "(5, 2, [9.2, 0.0, 0.0]::FLOAT[3])");
    ExecOrThrow(con, "CREATE INDEX items_vec_idx ON items USING GRAPH_INDEX (vec, cat)");

    ExecOrThrow(con, "PRAGMA explain_output='all'");
    auto plan = ReadExplain(
        con,
        "EXPLAIN SELECT id FROM items "
        "WHERE cat = 1 "
        "ORDER BY vec <-> [0.05, 0.0, 0.0]::FLOAT[3] "
        "LIMIT 2");
    RequireContains(plan, "VEX_INDEX_SCAN", "explain ANN plan");
}

static void TestInsertDeleteUpdateRegression(const string &extension_path) {
    DBConfig config;
    config.SetOptionByName("allow_unsigned_extensions", true);
    DuckDB db(nullptr, &config);
    auto con = OpenWithExtension(db, extension_path);

    ExecOrThrow(con, "CREATE TABLE items(id INTEGER, cat INTEGER, vec FLOAT[3])");
    ExecOrThrow(con,
                "INSERT INTO items VALUES "
                "(1, 1, [0.0, 0.0, 0.0]::FLOAT[3]),"
                "(2, 1, [0.1, 0.0, 0.0]::FLOAT[3]),"
                "(3, 2, [9.0, 0.0, 0.0]::FLOAT[3]),"
                "(4, 1, [0.2, 0.0, 0.0]::FLOAT[3])");
    ExecOrThrow(con, "CREATE INDEX items_vec_idx ON items USING GRAPH_INDEX (vec, cat)");

    ExecOrThrow(con, "INSERT INTO items VALUES (5, 1, [0.15, 0.0, 0.0]::FLOAT[3])");
    ExecOrThrow(con, "DELETE FROM items WHERE id = 1");
    ExecOrThrow(con, "UPDATE items SET vec = [9.2, 0.0, 0.0]::FLOAT[3] WHERE id = 3");

    auto filtered_rows = ReadFirstColumn(
        con,
        "SELECT id FROM items "
        "WHERE cat = 1 "
        "ORDER BY vec <-> [0.05, 0.0, 0.0]::FLOAT[3] "
        "LIMIT 3");
    RequireEqual(filtered_rows, {"2", "5", "4"}, "mutation filtered ANN result");

    auto far_rows = ReadFirstColumn(
        con,
        "SELECT id FROM items "
        "ORDER BY vec <-> [9.1, 0.0, 0.0]::FLOAT[3] "
        "LIMIT 2");
    RequireEqual(far_rows, {"3", "4"}, "mutation unfiltered ANN result");
}

static int RunOne(const string &test_name, const string &extension_path) {
    if (test_name == "load_and_basic_query") {
        TestLoadAndBasicQuery(extension_path);
        return 0;
    }
    if (test_name == "create_index_and_ann") {
        TestCreateIndexAndANN(extension_path);
        return 0;
    }
    if (test_name == "explain_ann_plan") {
        TestExplainANNPlan(extension_path);
        return 0;
    }
    if (test_name == "insert_delete_update_regression") {
        TestInsertDeleteUpdateRegression(extension_path);
        return 0;
    }
    throw std::runtime_error("unknown test: " + test_name);
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <test_name> <extension_path>" << std::endl;
        return 2;
    }

    try {
        RunOne(argv[1], argv[2]);
        std::cout << "PASS " << argv[1] << std::endl;
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "FAIL " << argv[1] << ": " << ex.what() << std::endl;
        return 1;
    }
}
