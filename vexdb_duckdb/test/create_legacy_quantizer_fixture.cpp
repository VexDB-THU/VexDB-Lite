// Generates the checked-in DuckDB legacy quantizer database fixture.
//
// Build this utility against DuckDB v1.5.2, then load a vexdb_lite extension
// from the revision whose persistence format should be frozen. The resulting
// database is gzip-compressed under test/fixtures and opened by the current
// extension in the compatibility spec.
#include "duckdb.hpp"
#include "core_functions_extension.hpp"

#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace duckdb;

namespace {

void ExecOrThrow(Connection &con, const std::string &sql) {
    auto result = con.Query(sql);
    if (!result) {
        throw std::runtime_error("query returned null: " + sql);
    }
    if (result->HasError()) {
        result->ThrowError("query failed: " + sql + "\n");
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 3) {
        std::cerr << "usage: create_legacy_quantizer_fixture EXTENSION OUTPUT_DB\n";
        return 2;
    }
    const std::string extension_path = argv[1];
    const std::string output_path = argv[2];
    std::remove(output_path.c_str());
    std::remove((output_path + ".wal").c_str());

    try {
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        DuckDB db(output_path, &config);
        db.LoadStaticExtension<CoreFunctionsExtension>();
        Connection con(db);
        ExecOrThrow(con, "LOAD '" + extension_path + "'");
        ExecOrThrow(con, "SET threads=4");
        ExecOrThrow(con, "SET vexdb_brute_force_threshold=0");

        ExecOrThrow(con, "CREATE TABLE legacy_pq(id INTEGER PRIMARY KEY, vec FLOAT[8])");
        ExecOrThrow(con,
            "INSERT INTO legacy_pq SELECT i, ["
            "sin(i*0.11)::FLOAT, cos(i*0.13)::FLOAT, sin(i*0.17)::FLOAT, "
            "cos(i*0.19)::FLOAT, sin(i*0.23)::FLOAT, cos(i*0.29)::FLOAT, "
            "sin(i*0.31)::FLOAT, cos(i*0.37)::FLOAT]::FLOAT[8] "
            "FROM range(512) t(i)");
        ExecOrThrow(con,
            "CREATE INDEX legacy_pq_idx ON legacy_pq USING GRAPH_INDEX(vec) "
            "WITH (metric='l2', quantizer='pq', pq_m=4, m=16, ef_construction=128)");

        ExecOrThrow(con, "CREATE TABLE legacy_rq(id INTEGER PRIMARY KEY, vec FLOAT[8])");
        ExecOrThrow(con, "INSERT INTO legacy_rq SELECT * FROM legacy_pq");
        ExecOrThrow(con,
            "CREATE INDEX legacy_rq_idx ON legacy_rq USING GRAPH_INDEX(vec) "
            "WITH (metric='l2', quantizer='rabitq', memory_mode='compact', "
            "m=16, ef_construction=128)");
        ExecOrThrow(con, "CHECKPOINT");
        std::cout << "legacy quantizer fixture written: " << output_path << '\n';
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
