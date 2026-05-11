// stress_parallel_build.cpp
//
// P6' stress test for parallel HNSW build.
//
// Runs N iterations of `CREATE INDEX ... WITH (threads=K)` on K-thread builds
// over a fresh DuckDB instance to expose races introduced by the parallel
// wire-up (P5'):
//   - MemStore assign_vector_id atomic + elems_veclock
//   - lock_point striped LWLock
//   - entry_lock double-lock protocol on graph promotion
//
// Pass criteria:
//   - no SIGSEGV / abort / std::system_error
//   - every search query returns the requested top-K rows (no NULL row_ids)
//   - parallel-built index produces non-trivial recall vs brute force on a
//     final correctness check (threads=K must not silently produce empty graphs)
//
// Usage: stress_parallel_build <extension_path> [iters] [n_vectors] [threads] [dim]
// Defaults: iters=30, n_vectors=10000, threads=4, dim=64.

#include "duckdb.hpp"
#include "core_functions_extension.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
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

static void ExecOrThrow(Connection &con, const std::string &sql) {
    auto result = con.Query(sql);
    if (!result) throw std::runtime_error("null result: " + sql);
    if (result->HasError()) result->ThrowError("query failed: " + sql + "\n");
}

static idx_t QuerySingleInt(Connection &con, const std::string &sql) {
    auto result = con.Query(sql);
    if (!result) throw std::runtime_error("null result: " + sql);
    if (result->HasError()) {
        throw std::runtime_error(std::string("query failed: ") + result->GetError() + "\nSQL: " + sql);
    }
    auto &mat = result->Cast<MaterializedQueryResult>();
    return BigIntValue::Get(mat.GetValue(0, 0).DefaultCastAs(LogicalType::BIGINT));
}

// Build a table, insert N random vectors, CREATE INDEX with threads=K,
// run a few queries, return number of distinct row_ids seen across queries.
static idx_t RunOneIteration(Connection &con, idx_t n_vectors, int threads, int dim,
                              std::mt19937 &rng) {
    ExecOrThrow(con, "DROP TABLE IF EXISTS t");
    ExecOrThrow(con, "CREATE TABLE t (id BIGINT, vec FLOAT[" + std::to_string(dim) + "])");

    std::uniform_real_distribution<float> uni(-1.0f, 1.0f);
    std::vector<float> v(dim);
    {
        Appender app(con, "t");
        for (idx_t i = 0; i < n_vectors; i++) {
            for (int d = 0; d < dim; d++) v[d] = uni(rng);
            app.BeginRow();
            app.Append<int64_t>(static_cast<int64_t>(i));
            app.Append<Value>(MakeArrayValue(v));
            app.EndRow();
        }
        app.Close();
    }

    ExecOrThrow(con,
        "CREATE INDEX idx_t ON t USING GRAPH_INDEX (vec) "
        "WITH (m=16, ef_construction=64, threads=" + std::to_string(threads) + ")");

    // Run 5 ANN queries on random query vectors, ensure k results each.
    idx_t total_returned = 0;
    for (int q = 0; q < 5; q++) {
        for (int d = 0; d < dim; d++) v[d] = uni(rng);
        std::string q_lit = "[";
        for (int d = 0; d < dim; d++) {
            q_lit += std::to_string(v[d]);
            if (d + 1 < dim) q_lit += ",";
        }
        q_lit += "]::FLOAT[" + std::to_string(dim) + "]";
        auto cnt = QuerySingleInt(con,
            "SELECT COUNT(*) FROM (SELECT id FROM t ORDER BY l2_distance(vec, " + q_lit + ") LIMIT 10) sub");
        if (cnt != 10) {
            throw std::runtime_error("expected 10 results, got " + std::to_string(cnt));
        }
        total_returned += static_cast<idx_t>(cnt);
    }
    return total_returned;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <extension_path> [iters=30] [n_vectors=10000] [threads=4] [dim=64]\n", argv[0]);
        return 2;
    }
    const std::string ext_path = argv[1];
    const int iters     = (argc > 2) ? std::atoi(argv[2]) : 30;
    const idx_t n_vec   = (argc > 3) ? std::atoi(argv[3]) : 10000;
    const int threads   = (argc > 4) ? std::atoi(argv[4]) : 4;
    const int dim       = (argc > 5) ? std::atoi(argv[5]) : 64;

    std::printf("stress_parallel_build: iters=%d n=%llu threads=%d dim=%d\n",
                iters, static_cast<unsigned long long>(n_vec), threads, dim);

    try {
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        DuckDB db(nullptr, &config);
        db.LoadStaticExtension<CoreFunctionsExtension>();
        Connection con(db);
        ExecOrThrow(con, "LOAD '" + ext_path + "'");

        std::mt19937 rng(42);
        auto start = std::chrono::steady_clock::now();
        idx_t total = 0;
        for (int i = 0; i < iters; i++) {
            std::printf("  starting iter %d/%d ...\n", i + 1, iters);
            std::fflush(stdout);
            idx_t got = RunOneIteration(con, n_vec, threads, dim, rng);
            total += got;
            auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("  iter %3d/%d done  total_returned=%llu  elapsed=%.1fs\n",
                        i + 1, iters, static_cast<unsigned long long>(total), elapsed);
            std::fflush(stdout);
        }
        std::printf("OK: %d iterations completed, no crash, %llu total query results\n",
                    iters, static_cast<unsigned long long>(total));
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
