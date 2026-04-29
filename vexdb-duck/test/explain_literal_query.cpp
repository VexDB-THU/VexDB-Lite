#include "duckdb.hpp"
#include "core_functions_extension.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace duckdb;

static Value MakeArrayValue(const std::vector<float> &vec) {
    std::vector<Value> values;
    values.reserve(vec.size());
    for (auto v : vec) {
        values.emplace_back(Value::FLOAT(v));
    }
    return Value::ARRAY(LogicalType::FLOAT, std::move(values));
}

static unique_ptr<QueryResult> ExecOrThrow(Connection &con, const std::string &sql) {
    auto result = con.Query(sql);
    if (!result) {
        throw std::runtime_error("query returned null result: " + sql);
    }
    if (result->HasError()) {
        result->ThrowError("query failed: " + sql + "\n");
    }
    return result;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "usage: explain_literal_query <extension_path>\n";
        return 2;
    }

    try {
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        DuckDB db(nullptr, &config);
        db.LoadStaticExtension<CoreFunctionsExtension>();
        Connection con(db);

        ExecOrThrow(con, "LOAD '" + std::string(argv[1]) + "'");
        ExecOrThrow(con, "CREATE TABLE t (id INTEGER, vec FLOAT[3])");
        {
            Appender app(con, "t");
            app.BeginRow();
            app.Append(1);
            app.Append(MakeArrayValue({1.0f, 2.0f, 3.0f}));
            app.EndRow();
            app.BeginRow();
            app.Append(2);
            app.Append(MakeArrayValue({4.0f, 5.0f, 6.0f}));
            app.EndRow();
            app.BeginRow();
            app.Append(3);
            app.Append(MakeArrayValue({7.0f, 8.0f, 9.0f}));
            app.EndRow();
            app.Close();
        }
        ExecOrThrow(con, "CREATE INDEX idx_t_vec ON t USING GRAPH_INDEX (vec) WITH (metric='l2')");
        auto result = ExecOrThrow(con, "EXPLAIN SELECT id FROM t ORDER BY l2_distance(vec, [1,2,3]::FLOAT[3]) LIMIT 1");
        auto &mat = result->Cast<MaterializedQueryResult>();
        for (idx_t row = 0; row < mat.RowCount(); row++) {
            for (idx_t col = 0; col < mat.ColumnCount(); col++) {
                std::cout << mat.GetValue(col, row).ToString() << "\n";
            }
        }
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "explain_literal_query: fail: " << ex.what() << "\n";
        return 1;
    }
}
