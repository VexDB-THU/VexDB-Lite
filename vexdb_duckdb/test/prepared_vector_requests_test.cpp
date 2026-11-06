// Check real parameter values and transaction visibility on reusable ANN and
// native filtered TopN plans. This is a correctness probe, not a benchmark.
#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/prepared_statement.hpp"

#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace duckdb;

namespace {
size_t checks = 0;

unique_ptr<MaterializedQueryResult> SQL(Connection &con, const std::string &sql) {
    auto result = con.Query(sql);
    if (result->HasError()) throw std::runtime_error(result->GetError());
    return result;
}

std::string Quote(const std::string &text) {
    std::string result = "'";
    for (char ch : text) result += ch == '\'' ? "''" : std::string(1, ch);
    return result + "'";
}

vector<Value> QueryValues(int ordinal, int dimensions) {
    vector<Value> values;
    for (int column = 0; column < dimensions; ++column) {
        values.push_back(Value::FLOAT(float(std::sin((ordinal + 1) * (column + 1) * .0013) + .01 * ordinal)));
    }
    return values;
}

std::string Literal(int ordinal, int dimensions, float offset = 0) {
    auto values = QueryValues(ordinal, dimensions);
    std::ostringstream text;
    text << std::setprecision(9) << '[';
    for (int column = 0; column < dimensions; ++column) {
        if (column) text << ',';
        text << float(values[column].GetValue<float>() + offset);
    }
    text << "]::FLOAT[" << dimensions << ']';
    return text.str();
}

std::string Snapshot(MaterializedQueryResult &result) {
    std::ostringstream text;
    text << result.RowCount() << ':';
    for (idx_t row = 0; row < result.RowCount(); ++row) {
        for (idx_t column = 0; column < result.ColumnCount(); ++column) text << result.GetValue(column, row).ToString() << '|';
    }
    return text.str();
}

void Check(Connection &con, PreparedStatement &statement, const std::string &table,
           int ordinal, int dimensions, bool filtered) {
    const auto filter = filtered ? " WHERE route=" + std::to_string(ordinal % 7) + " AND live" : "";
    SQL(con, "SET vexdb_brute_force_threshold=1000000");
    SQL(con, "SET vexdb_native_topn_fetch=false");
    auto expected = SQL(con, "SELECT rec_id,payload FROM " + table + filter + " ORDER BY l2_distance(vec," +
                             Literal(ordinal, dimensions) + ") LIMIT 10");
    SQL(con, "SET vexdb_brute_force_threshold=0");
    SQL(con, "SET vexdb_native_topn_fetch=true");
    auto literal = SQL(con, "SELECT rec_id,payload FROM " + table + filter + " ORDER BY l2_distance(vec," +
                           Literal(ordinal, dimensions) + ") LIMIT 10");
    vector<Value> parameters{Value::ARRAY(LogicalType::FLOAT, QueryValues(ordinal, dimensions))};
    if (filtered) parameters.push_back(Value::INTEGER(ordinal % 7));
    auto actual = statement.Execute(parameters, false);
    if (actual->HasError()) throw std::runtime_error(actual->GetError());
    const auto actual_rows = Snapshot(actual->Cast<MaterializedQueryResult>());
    if (Snapshot(*literal) != actual_rows)
        throw std::runtime_error("parameter/literal changed: " + table + " ordinal=" + std::to_string(ordinal) +
                                 " literal=" + Snapshot(*literal) + " parameter=" + actual_rows);
    if (Snapshot(*expected) != actual_rows)
        throw std::runtime_error("full-budget exact result changed: " + table + " ordinal=" + std::to_string(ordinal) +
                                 " exact=" + Snapshot(*expected) + " parameter=" + actual_rows);
    ++checks;
}

void Configure(Connection &con) {
    SQL(con, "SET threads=2");
    SQL(con, "SET vexdb_ef_search=2048");
    // This fixture checks exact equality, including order, rather than ANN
    // recall. Refine every code in its 512-row models (k=10). A factor of 1
    // disables refinement and cannot be compared to exact distances.
    SQL(con, "SET vexdb_pq_refine_k_factor=1000");
    SQL(con, "SET vexdb_brute_force_threshold=0");
    SQL(con, "SET vexdb_filtered_search_mode='auto'");
    SQL(con, "SET vexdb_native_topn_fetch=true");
}

void Cases(DuckDB &database, bool create) {
    Connection reader(database), writer(database);
    Configure(reader);
    Configure(writer);
    for (int dimensions : {64, 1536}) {
        for (const auto method : {std::string("raw"), std::string("pq")}) {
            const auto table = "records_" + method + "_" + std::to_string(dimensions);
            if (create) {
                SQL(reader, "CREATE TABLE " + table + "(rec_id INTEGER,route INTEGER,live BOOLEAN,vec FLOAT[" +
                            std::to_string(dimensions) + "],payload VARCHAR)");
                Appender append(reader, table);
                for (int ordinal = 0; ordinal < 512; ++ordinal) {
                    append.BeginRow();
                    append.Append<int32_t>(ordinal);
                    append.Append<int32_t>(ordinal % 7);
                    append.Append<bool>(true);
                    append.Append(Value::ARRAY(LogicalType::FLOAT, QueryValues(ordinal, dimensions)));
                    append.Append(Value("payload-" + std::to_string(ordinal)));
                    append.EndRow();
                }
                append.Close();
                auto options = "metric='l2',m=16,ef_construction=100,threads=2";
                const auto quantizer = method == "pq" ? ",quantizer='pq',pq_m=8,memory_mode='full'" : "";
                SQL(reader, "CREATE INDEX " + table + "_ann ON " + table + " USING GRAPH_INDEX(vec) WITH(" + options + quantizer + ")");
            }
            SQL(reader, "SET vexdb_pq_search_mode=" + Quote(method == "pq" ? "pq_only" : "off"));
            for (bool filtered : {false, true}) {
                const auto filter = filtered ? " WHERE route=$2 AND live" : "";
                auto statement = reader.Prepare("SELECT rec_id,payload FROM " + table + filter +
                    " ORDER BY l2_distance(vec,$1::FLOAT[" + std::to_string(dimensions) + "]) LIMIT 10");
                if (statement->HasError()) throw std::runtime_error(statement->GetError());
                for (int ordinal : {3, 97, 250, 1, 301, 97, 0, 3}) Check(reader, *statement, table, ordinal, dimensions, filtered);

                SQL(reader, "BEGIN");
                SQL(reader, "SELECT count(*) FROM " + table);
                SQL(writer, "BEGIN");
                SQL(writer, "UPDATE " + table + " SET payload='committed' WHERE rec_id=3");
                SQL(writer, "DELETE FROM " + table + " WHERE rec_id=97");
                SQL(writer, "INSERT INTO " + table + " VALUES(10003,3,true," + Literal(3, dimensions, .00025f) + ",'committed insert')");
                SQL(writer, "COMMIT");
                Check(reader, *statement, table, 3, dimensions, filtered);
                SQL(reader, "COMMIT");
                Check(reader, *statement, table, 3, dimensions, filtered);
                SQL(reader, "BEGIN");
                SQL(reader, "DELETE FROM " + table + " WHERE rec_id=3");
                Check(reader, *statement, table, 3, dimensions, filtered);
                SQL(reader, "ROLLBACK");
                Check(reader, *statement, table, 3, dimensions, filtered);
                SQL(writer, "DELETE FROM " + table + " WHERE rec_id=10003");
            }
        }
    }
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 3 || std::filesystem::exists(argv[2])) return 2;
    try {
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.options.load_extensions = false;
        {
            DuckDB database(argv[2], &config);
            ExtensionHelper::LoadExtension(database, "core_functions");
            Connection loader(database);
            SQL(loader, "LOAD " + Quote(argv[1]));
            Cases(database, true);
            SQL(loader, "CHECKPOINT");
        }
        {
            DuckDB database(argv[2], &config);
            ExtensionHelper::LoadExtension(database, "core_functions");
            Connection loader(database);
            SQL(loader, "LOAD " + Quote(argv[1]));
            Cases(database, false);
        }
        std::cout << "PASS " << checks << " prepared vector requests: raw/PQ,64/1536 dimensions, filters, changed parameters, snapshots, rollback, reopen\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
