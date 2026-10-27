#include "duckdb.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace duckdb;
using Rows = std::vector<std::pair<int64_t, double>>;
using ExactDistances = std::unordered_map<int64_t, double>;
static std::string pq_mode = "pq_only";

static unique_ptr<MaterializedQueryResult> Exec(Connection &con, const std::string &sql) {
    auto result = con.Query(sql);
    if (!result || result->HasError()) {
        throw std::runtime_error("query failed: " + sql + "\n" + (result ? result->GetError() : "null result"));
    }
    return result;
}

static std::string QueryVector(int query) {
    std::ostringstream out;
    out << std::setprecision(9) << '[';
    for (int j = 0; j < 16; ++j) {
        if (j) out << ',';
        out << float(std::sin((query + 0.731) * (j + 0.3)) + std::cos((query + 0.731) * 0.001 * (j + 2)));
    }
    out << "]::FLOAT[16]";
    return out.str();
}

static std::string Query(int query, bool reference) {
    auto sql = "SELECT id, l2_distance(vec, " + QueryVector(query) + ") AS d FROM pq_test ORDER BY d";
    // The extension only rewrites a single ORDER BY key. A second key gives an
    // independent full-vector reference; assert both plans below.
    if (reference) sql += ", id";
    return sql + " LIMIT 10";
}

static Rows Fetch(Connection &con, const std::string &sql) {
    auto result = Exec(con, sql);
    Rows rows;
    for (idx_t row = 0; row < result->RowCount(); ++row) {
        rows.emplace_back(result->GetValue(0, row).GetValue<int64_t>(),
                          result->GetValue(1, row).GetValue<double>());
    }
    return rows;
}

static void RequireIndexPlan(Connection &con, const std::string &sql, bool expected) {
    auto result = Exec(con, "EXPLAIN " + sql);
    std::string plan;
    for (idx_t row = 0; row < result->RowCount(); ++row) {
        plan += result->GetValue(1, row).ToString();
    }
    if ((plan.find("VEXDB_INDEX_SCAN") != std::string::npos) != expected) {
        throw std::runtime_error("unexpected query plan:\n" + plan);
    }
}

static void Configure(Connection &con, double factor) {
    Exec(con, "SET vexdb_brute_force_threshold=0");
    Exec(con, "SET vexdb_pq_search_mode='" + pq_mode + "'");
    if (pq_mode == "pq_graph") Exec(con, "SET vexdb_ef_search=65535");
    Exec(con, "SET vexdb_pq_refine_k_factor=" + std::to_string(factor));
}

static ExactDistances FetchExactDistances(Connection &con, int query, const std::string &table = "pq_test") {
    // No LIMIT or distance ORDER BY: this cannot become an index top-k scan.
    const auto rows = Fetch(con, "SELECT id, l2_distance(vec, " + QueryVector(query) + ") FROM " + table);
    return ExactDistances(rows.begin(), rows.end());
}

static void Compare(const Rows &actual, const Rows &expected, const ExactDistances &raw_distances) {
    if (actual.size() != 10 || expected.size() != 10) throw std::runtime_error("expected ten neighbors");
    std::set<int64_t> ids;
    for (size_t i = 0; i < actual.size(); ++i) {
        if (!ids.insert(actual[i].first).second) throw std::runtime_error("duplicate result row id");
        const auto raw = raw_distances.find(actual[i].first);
        if (raw == raw_distances.end() ||
            std::abs(actual[i].second - raw->second) > 2e-5 * (1 + std::abs(raw->second))) {
            throw std::runtime_error("returned row id and its actual vector distance disagree");
        }
        // Identical vectors may have different row ids at a tied top-k boundary.
        if (std::abs(actual[i].second - expected[i].second) > 2e-5 * (1 + std::abs(expected[i].second))) {
            throw std::runtime_error("neighbor distance differs from reference at rank " + std::to_string(i));
        }
    }
}

static void ConcurrentQueries(DuckDB &db, Connection &control, const std::string &phase, bool full_refine) {
    constexpr int workers = 8, queries = 8;
    std::vector<Rows> expected;
    std::vector<ExactDistances> raw_distances;
    for (int query = 0; query < queries; ++query) {
        expected.push_back(Fetch(control, Query(query, full_refine)));
        raw_distances.push_back(FetchExactDistances(control, query));
    }
    std::atomic<int> ready {0};
    std::atomic<bool> start {false};
    std::vector<std::thread> readers;
    std::vector<std::string> errors(workers);
    for (int worker = 0; worker < workers; ++worker) {
        readers.emplace_back([&, worker] {
            try {
                Connection con(db);
                Configure(con, full_refine ? 1000 : 4);
                ++ready;
                while (!start.load()) std::this_thread::yield();
                for (int repeat = 0; repeat < 2; ++repeat) {
                    for (int query = 0; query < queries; ++query) {
                        Compare(Fetch(con, Query(query, false)), expected[query], raw_distances[query]);
                    }
                }
            } catch (const std::exception &error) {
                errors[worker] = error.what();
                // A setup failure still releases the coordinator's barrier.
                if (!start.load()) ++ready;
            }
        });
    }
    while (ready.load() != workers) std::this_thread::yield();
    start = true;
    for (auto &reader : readers) reader.join();
    for (const auto &error : errors) if (!error.empty()) throw std::runtime_error(phase + ": " + error);
    std::cout << "PASS " << phase << ": 8 connections, 128 queries\n";
}

static void RequireRowCoverage(Connection &con) {
    const auto table = Exec(con, "SELECT count(*) FROM pq_test")->GetValue(0, 0).GetValue<int64_t>();
    const auto index = Exec(con, "SELECT row_id_map_size FROM vexdb_index_info() WHERE index_name='pq_test_idx'")
                           ->GetValue(0, 0).GetValue<int64_t>();
    if (table != index) throw std::runtime_error("live table rows and restored index coverage differ");
}

static void RequireSurvivingDuplicate(Connection &con) {
    const auto sql = "SELECT id FROM pq_test ORDER BY l2_distance(vec, "
                     "[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]::FLOAT[16]) LIMIT 1";
    RequireIndexPlan(con, sql, true);
    if (Exec(con, sql)->GetValue(0, 0).GetValue<int64_t>() != 10000) {
        throw std::runtime_error("duplicate of the deleted primary row was lost");
    }
}

static void RequireSingleRowLayout(Connection &con) {
    const auto sql = "SELECT id, l2_distance(vec, " + QueryVector(0) + ") AS d FROM unique_test ORDER BY d";
    RequireIndexPlan(con, sql + " LIMIT 10", true);
    Compare(Fetch(con, sql + " LIMIT 10"), Fetch(con, sql + ", id LIMIT 10"),
            FetchExactDistances(con, 0, "unique_test"));
    const auto covered = Exec(con, "SELECT row_id_map_size FROM vexdb_index_info() WHERE index_name='unique_test_idx'")
                             ->GetValue(0, 0).GetValue<int64_t>();
    if (covered != 512) throw std::runtime_error("node-header-only layout failed to reload");
}

static void RequireGraphModeGuards(Connection &con) {
    auto reject = [&](const std::string &sql, const std::string &expected) {
        auto result = con.Query(sql);
        if (!result || !result->HasError() || result->GetError().find(expected) == std::string::npos) {
            throw std::runtime_error("expected rejection containing " + expected);
        }
    };
    Exec(con, "SET vexdb_pq_search_mode='invalid'");
    reject(Query(0, false), "vexdb_pq_search_mode must");
    Configure(con, 4);
    Exec(con, "SET vexdb_pq_refine_k_factor='NaN'");
    reject(Query(0, false), "vexdb_pq_refine_k_factor must");
    Configure(con, 1000);
    for (const auto &kind : {std::string("raw"), std::string("compact"), std::string("ip"), std::string("cosine")}) {
        const auto table = "guard_" + kind;
        Exec(con, "CREATE TABLE " + table + " AS SELECT * FROM unique_test");
        auto options = "metric='" + (kind == "ip" ? std::string("ip") : kind == "cosine" ? std::string("cosine") : std::string("l2")) + "', m=8, ef_construction=90, threads=1";
        if (kind != "raw") options += ", quantizer='pq', pq_m=4";
        if (kind == "compact") options += ", memory_mode='compact'";
        Exec(con, "CREATE INDEX " + table + "_idx ON " + table + " USING GRAPH_INDEX(vec) WITH (" + options + ")");
        const auto distance = kind == "ip" ? "inner_product" : kind == "cosine" ? "cosine_distance" : "l2_distance";
        const auto sql = "SELECT id, " + std::string(distance) + "(vec," + QueryVector(0) + ") AS d FROM " + table +
                         " ORDER BY d" + (kind == "ip" ? " DESC" : "");
        if (kind == "cosine") {
            RequireIndexPlan(con, sql + " LIMIT 10", true);
            const auto all = Fetch(con, "SELECT id, cosine_distance(vec," + QueryVector(0) + ") FROM " + table);
            Compare(Fetch(con, sql + " LIMIT 10"), Fetch(con, sql + ", id LIMIT 10"), ExactDistances(all.begin(), all.end()));
        } else {
            reject(sql + " LIMIT 10", kind == "raw" ? "requires the index" : "non-compact L2 or cosine");
        }
    }
    std::cout << "PASS pq_graph rejects compact/non-PQ/IP/invalid settings; cosine matches exact reference\n";
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: pq_refine_integration_test <extension_path> <new_database_path> [pq_only|pq_graph]\n";
        return 2;
    }
    const std::string extension = std::filesystem::absolute(argv[1]).string();
    const std::string database = argv[2];
    if (argc == 4) pq_mode = argv[3];
    if (pq_mode != "pq_only" && pq_mode != "pq_graph") return 2;
    if (std::filesystem::exists(database) || std::filesystem::exists(database + ".wal")) {
        std::cerr << "refusing to overwrite an existing database\n";
        return 2;
    }
    try {
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        {
            DuckDB db(database, &config);
            Connection con(db);
            Exec(con, "LOAD '" + StringUtil::Replace(extension, "'", "''") + "'");
            Configure(con, 1000);
            Exec(con, "SET threads=2");
            Exec(con, "CREATE TABLE pq_test AS SELECT i::INTEGER AS id, "
                      "list_transform(range(16), j -> (sin(i*(j+0.3))+cos(i*0.001*(j+2)))::FLOAT)::FLOAT[16] AS vec "
                      "FROM range(4096) t(i)");
            // Duplicates during the initial bulk build leave reserved, unused
            // node slots. They must not become extra row ids after a checkpoint.
            Exec(con, "INSERT INTO pq_test SELECT id+20000, vec FROM pq_test WHERE id>=100 AND id<108");
            Exec(con, "CREATE INDEX pq_test_idx ON pq_test USING GRAPH_INDEX(vec) WITH (metric='l2', quantizer='pq', pq_m=4, threads=1)");
            // This index has no duplicate-row metadata, exercising the existing
            // node-header format alongside the optional new field.
            Exec(con, "CREATE TABLE unique_test AS SELECT * FROM pq_test WHERE id<512");
            Exec(con, "CREATE INDEX unique_test_idx ON unique_test USING GRAPH_INDEX(vec) WITH (metric='l2', quantizer='pq', pq_m=4, threads=1)");
            RequireIndexPlan(con, Query(0, false), true);
            RequireIndexPlan(con, Query(0, true), false);
            ConcurrentQueries(db, con, "first refinement after build", true);
            Configure(con, 4);
            ConcurrentQueries(db, con, "normal factor 4 matches sequential results", false);
            Exec(con, "INSERT INTO pq_test SELECT i::INTEGER, "
                      "list_transform(range(16), j -> (sin(i*(j+0.3))+cos(i*0.001*(j+2)))::FLOAT)::FLOAT[16] "
                      "FROM range(4096,4224) t(i)");
            Exec(con, "INSERT INTO pq_test SELECT id+10000, vec FROM pq_test WHERE id<8");
            Exec(con, "DELETE FROM pq_test WHERE id%13=0");
            ConcurrentQueries(db, con, "first refinement after insert and delete", true);
            Configure(con, 1000);
            RequireSurvivingDuplicate(con);
            Exec(con, "CHECKPOINT");
        }
        {
            DuckDB db(database, &config);
            Connection con(db);
            Exec(con, "LOAD '" + StringUtil::Replace(extension, "'", "''") + "'");
            Configure(con, 1000);
            RequireRowCoverage(con);
            RequireIndexPlan(con, Query(0, false), true);
            ConcurrentQueries(db, con, "first refinement after database reopen", true);
            RequireSurvivingDuplicate(con);
            RequireSingleRowLayout(con);
            if (pq_mode == "pq_graph") RequireGraphModeGuards(con);
            Exec(con, "CHECKPOINT");
        }
        {
            DuckDB db(database, &config);
            Connection con(db);
            Exec(con, "LOAD '" + StringUtil::Replace(extension, "'", "''") + "'");
            Configure(con, 1000);
            RequireRowCoverage(con);
            RequireIndexPlan(con, Query(0, false), true);
            ConcurrentQueries(db, con, "second reopen preserves surviving duplicates", true);
            RequireSurvivingDuplicate(con);
            RequireSingleRowLayout(con);
            if (pq_mode == "pq_graph") {
                const auto sql = "SELECT id, cosine_distance(vec," + QueryVector(0) + ") AS d FROM guard_cosine ORDER BY d";
                RequireIndexPlan(con, sql + " LIMIT 10", true);
                const auto all = Fetch(con, "SELECT id, cosine_distance(vec," + QueryVector(0) + ") FROM guard_cosine");
                Compare(Fetch(con, sql + " LIMIT 10"), Fetch(con, sql + ", id LIMIT 10"), ExactDistances(all.begin(), all.end()));
                const auto options = Exec(con, "SELECT m, ef_construction, metric FROM vexdb_index_info() WHERE index_name='guard_cosine_idx'");
                if (options->GetValue(0, 0).GetValue<int>() != 8 || options->GetValue(1, 0).GetValue<int>() != 90 ||
                    options->GetValue(2, 0).ToString() != "cosine") throw std::runtime_error("saved graph parameters were lost");
                std::cout << "PASS reopened cosine and non-default m/ef_construction\n";
            }
        }
        std::cout << "PASS actual extension PQ refinement, 640 concurrent SQL queries total\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "pq_refine_integration_test: " << error.what() << '\n';
        return 1;
    }
}
