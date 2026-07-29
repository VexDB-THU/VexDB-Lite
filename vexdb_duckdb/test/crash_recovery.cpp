#include "duckdb.hpp"
#include "core_functions_extension.hpp"
#include "duckdb/main/materialized_query_result.hpp"
#include "vexdb_lite_extension.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace duckdb;

namespace {

static constexpr idx_t DIM = 8;

struct ScenarioResult {
    bool completed_before_kill = false;
};

struct ScenarioFailure {
    std::string name;
    std::string message;
};

enum class IndexScanExpectation {
    Require,
    Forbid,
    Allow
};

static bool Exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static std::string JoinPath(const std::string &dir, const std::string &file) {
    if (!dir.empty() && dir.back() == '/') {
        return dir + file;
    }
    return dir + "/" + file;
}

static void TouchMarker(const std::string &path) {
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("open marker failed: " + path + ": " + std::strerror(errno));
    }
    const char payload[] = "ready\n";
    if (write(fd, payload, sizeof(payload) - 1) < 0) {
        close(fd);
        throw std::runtime_error("write marker failed: " + path + ": " + std::strerror(errno));
    }
    fsync(fd);
    close(fd);
}

static void RemoveTree(const std::string &path) {
    DIR *dir = opendir(path.c_str());
    if (!dir) {
        unlink(path.c_str());
        return;
    }
    while (auto *entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") {
            continue;
        }
        auto child = JoinPath(path, name);
        struct stat st;
        if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            RemoveTree(child);
        } else {
            unlink(child.c_str());
        }
    }
    closedir(dir);
    rmdir(path.c_str());
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

static std::string QueryToString(Connection &con, const std::string &sql) {
    auto result = ExecOrThrow(con, sql);
    auto &mat = result->Cast<MaterializedQueryResult>();
    std::string out;
    for (idx_t row = 0; row < mat.RowCount(); row++) {
        for (idx_t col = 0; col < mat.ColumnCount(); col++) {
            out += mat.GetValue(col, row).ToString();
            out += '\n';
        }
    }
    return out;
}

static int64_t QuerySingleInt64(Connection &con, const std::string &sql) {
    auto result = ExecOrThrow(con, sql);
    auto &mat = result->Cast<MaterializedQueryResult>();
    if (mat.RowCount() != 1 || mat.ColumnCount() != 1) {
        throw std::runtime_error("expected 1x1 result for query: " + sql);
    }
    auto value = mat.GetValue(0, 0);
    if (value.IsNull()) {
        throw std::runtime_error("unexpected NULL result for query: " + sql);
    }
    return value.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
}

static void WithConnection(const std::string &db_path, const std::string &extension_path,
                           const std::function<void(Connection &)> &fn) {
    DBConfig config;
    config.SetOptionByName("allow_unsigned_extensions", true);
    DuckDB db(db_path, &config);
    db.LoadStaticExtension<CoreFunctionsExtension>();
    db.LoadStaticExtension<VexdbLiteExtension>();
    Connection con(db);
    (void)extension_path;
    ExecOrThrow(con, "SET vexdb_brute_force_threshold=1");
    // Make crash-recovery assertions about durability, not HNSW recall. All
    // scenarios stay below this row count, so this effectively searches the
    // whole graph while still exercising VEXDB_INDEX_SCAN.
    ExecOrThrow(con, "SET vexdb_ef_search=65535");
    fn(con);
}

static std::string VectorLiteral(int64_t id) {
    double vals[DIM] = {
        static_cast<double>(id),
        static_cast<double>(id) * 0.5,
        static_cast<double>(id) * 0.25,
        static_cast<double>(id) * 0.125,
        static_cast<double>(id % 97),
        static_cast<double>(id % 101),
        static_cast<double>(id % 103),
        static_cast<double>(id % 107),
    };
    std::string out = "[";
    for (idx_t i = 0; i < DIM; i++) {
        if (i) {
            out += ", ";
        }
        out += std::to_string(vals[i]) + "::FLOAT";
    }
    out += "]::FLOAT[8]";
    return out;
}

static std::string UnitVectorLiteral() {
    return "[1.0::FLOAT, 0.0::FLOAT, 0.0::FLOAT, 0.0::FLOAT, "
           "0.0::FLOAT, 0.0::FLOAT, 0.0::FLOAT, 0.0::FLOAT]::FLOAT[8]";
}

static std::string CompactUpdateVectorLiteral() {
    return "[-7777.0::FLOAT, 13.0::FLOAT, 29.0::FLOAT, 31.0::FLOAT, "
           "37.0::FLOAT, 41.0::FLOAT, 43.0::FLOAT, 47.0::FLOAT]::FLOAT[8]";
}

static std::string PopulateSql(int64_t first_id, int64_t last_id_inclusive) {
    return "INSERT INTO vectors SELECT i, ["
           "i::FLOAT, "
           "(i * 0.5)::FLOAT, "
           "(i * 0.25)::FLOAT, "
           "(i * 0.125)::FLOAT, "
           "(i % 97)::FLOAT, "
           "(i % 101)::FLOAT, "
           "(i % 103)::FLOAT, "
           "(i % 107)::FLOAT]::FLOAT[8] "
           "FROM range(" +
           std::to_string(first_id) + ", " + std::to_string(last_id_inclusive + 1) + ") tbl(i)";
}

static void CreateBaseDatabase(const std::string &db_path, const std::string &extension_path, int64_t rows,
                               bool with_index, const std::string &metric = "l2",
                               const std::string &extra_index_options = "") {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        ExecOrThrow(con, "CREATE TABLE vectors (id BIGINT PRIMARY KEY, vec FLOAT[8])");
        ExecOrThrow(con, PopulateSql(1, rows));
        if (with_index) {
            std::string options = "metric='" + metric + "'";
            if (!extra_index_options.empty()) {
                options += ", " + extra_index_options;
            }
            ExecOrThrow(con, "CREATE INDEX idx_vectors ON vectors USING GRAPH_INDEX (vec) WITH (" + options + ")");
        }
        ExecOrThrow(con, "PRAGMA force_checkpoint");
        ExecOrThrow(con, "CHECKPOINT");
    });
}

static bool IndexExists(Connection &con) {
    return QuerySingleInt64(con, "SELECT count(*) FROM duckdb_indexes() WHERE index_name='idx_vectors'") == 1;
}

static void ValidateL2IndexQuery(Connection &con, int64_t expected_id, const std::string &literal,
                                 IndexScanExpectation index_scan_expectation = IndexScanExpectation::Require) {
    ExecOrThrow(con, "SET vexdb_brute_force_threshold=1000000");
    auto exact = QuerySingleInt64(con, "SELECT id FROM vectors ORDER BY l2_distance(vec, " + literal + ") LIMIT 1");
    if (exact != expected_id) {
        throw std::runtime_error("exact nearest-id mismatch after recovery: expected " + std::to_string(expected_id) +
                                 ", got " + std::to_string(exact));
    }

    ExecOrThrow(con, "SET vexdb_brute_force_threshold=1");
    auto explain = QueryToString(con, "EXPLAIN SELECT id FROM vectors ORDER BY l2_distance(vec, " + literal + ") LIMIT 1");
    bool uses_index = explain.find("VEXDB_INDEX_SCAN") != std::string::npos;
    if (index_scan_expectation == IndexScanExpectation::Require && !uses_index) {
        throw std::runtime_error("expected VEXDB_INDEX_SCAN after recovery, got:\n" + explain);
    }
    if (index_scan_expectation == IndexScanExpectation::Forbid && uses_index) {
        throw std::runtime_error("expected stale index to be bypassed after recovery, got:\n" + explain);
    }
    if (!uses_index) {
        return;
    }
    auto found = QuerySingleInt64(con, "SELECT id FROM vectors ORDER BY l2_distance(vec, " + literal + ") LIMIT 1");
    if (found != expected_id) {
        auto index_info = QueryToString(con,
            "SELECT node_count, row_id_map_size, max_level, m FROM vexdb_index_info() "
            "WHERE index_name='idx_vectors'");
        auto rowid_info = QueryToString(con, "SELECT count(*), max(rowid) FROM vectors");
        throw std::runtime_error("nearest-id mismatch after recovery: expected " + std::to_string(expected_id) +
                                 ", exact scan got " + std::to_string(exact) +
                                 ", index scan got " + std::to_string(found) +
                                 "\nindex_info:\n" + index_info +
                                 "rowid_info:\n" + rowid_info);
    }
}

static void ValidateIndexQuery(Connection &con, int64_t expected_id,
                               IndexScanExpectation index_scan_expectation = IndexScanExpectation::Require) {
    ValidateL2IndexQuery(con, expected_id, VectorLiteral(expected_id), index_scan_expectation);
}

static void ValidateCosineIndexQuery(Connection &con, int64_t expected_id,
                                     IndexScanExpectation index_scan_expectation = IndexScanExpectation::Require) {
    auto literal = UnitVectorLiteral();
    ExecOrThrow(con, "SET vexdb_brute_force_threshold=1000000");
    auto exact = QuerySingleInt64(con, "SELECT id FROM vectors ORDER BY cosine_distance(vec, " + literal + ") LIMIT 1");
    if (exact != expected_id) {
        throw std::runtime_error("exact cosine nearest-id mismatch after recovery: expected " +
                                 std::to_string(expected_id) + ", got " + std::to_string(exact));
    }

    ExecOrThrow(con, "SET vexdb_brute_force_threshold=1");
    auto explain = QueryToString(con, "EXPLAIN SELECT id FROM vectors ORDER BY cosine_distance(vec, " + literal +
                                      ") LIMIT 1");
    bool uses_index = explain.find("VEXDB_INDEX_SCAN") != std::string::npos;
    if (index_scan_expectation == IndexScanExpectation::Require && !uses_index) {
        auto index_info = QueryToString(con,
            "SELECT metric, node_count, row_id_map_size, max_level, m FROM vexdb_index_info() "
            "WHERE index_name='idx_vectors'");
        auto rowid_info = QueryToString(con, "SELECT count(*), max(rowid) FROM vectors");
        throw std::runtime_error("expected cosine VEXDB_INDEX_SCAN after recovery, got:\n" + explain +
                                 "\nindex_info:\n" + index_info +
                                 "rowid_info:\n" + rowid_info);
    }
    if (index_scan_expectation == IndexScanExpectation::Forbid && uses_index) {
        throw std::runtime_error("expected stale cosine index to be bypassed after recovery, got:\n" + explain);
    }
    if (!uses_index) {
        return;
    }
    auto found = QuerySingleInt64(con, "SELECT id FROM vectors ORDER BY cosine_distance(vec, " + literal + ") LIMIT 1");
    if (found != expected_id) {
        throw std::runtime_error("cosine nearest-id mismatch after recovery: expected " +
                                 std::to_string(expected_id) + ", exact scan got " + std::to_string(exact) +
                                 ", index scan got " + std::to_string(found));
    }
}

static void ValidateDatabase(const std::string &db_path, const std::string &extension_path, int64_t expected_count,
                             bool require_index, int64_t expected_nearest_id,
                             IndexScanExpectation index_scan_expectation = IndexScanExpectation::Require) {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        auto count = QuerySingleInt64(con, "SELECT count(*) FROM vectors");
        if (count != expected_count) {
            throw std::runtime_error("row-count mismatch after recovery: expected " + std::to_string(expected_count) +
                                     ", got " + std::to_string(count));
        }
        bool has_index = IndexExists(con);
        if (require_index && !has_index) {
            throw std::runtime_error("expected idx_vectors to survive recovery");
        }
        if (has_index) {
            ValidateIndexQuery(con, expected_nearest_id, index_scan_expectation);
        }
    });
}

static void RebuildAndValidateL2Index(const std::string &db_path, const std::string &extension_path,
                                      int64_t expected_count, int64_t expected_nearest_id,
                                      const std::string &literal) {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        ExecOrThrow(con, "PRAGMA vexdb_rebuild_index('idx_vectors')");
        auto count = QuerySingleInt64(con, "SELECT row_id_map_size FROM vexdb_index_info() "
                                          "WHERE index_name='idx_vectors'");
        if (count != expected_count) {
            throw std::runtime_error("rebuild row_id_map_size mismatch: expected " +
                                     std::to_string(expected_count) + ", got " + std::to_string(count));
        }
        ExecOrThrow(con, "PRAGMA force_checkpoint");
        ExecOrThrow(con, "CHECKPOINT");
    });
    WithConnection(db_path, extension_path, [&](Connection &con) {
        ValidateL2IndexQuery(con, expected_nearest_id, literal, IndexScanExpectation::Require);
    });
}

static void RebuildAndValidateIndex(const std::string &db_path, const std::string &extension_path,
                                    int64_t expected_count, int64_t expected_nearest_id) {
    RebuildAndValidateL2Index(db_path, extension_path, expected_count, expected_nearest_id,
                              VectorLiteral(expected_nearest_id));
}

static void RebuildAndValidateCosineIndex(const std::string &db_path, const std::string &extension_path,
                                          int64_t expected_count, int64_t expected_nearest_id) {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        ExecOrThrow(con, "PRAGMA vexdb_rebuild_index('idx_vectors')");
        auto count = QuerySingleInt64(con, "SELECT row_id_map_size FROM vexdb_index_info() "
                                          "WHERE index_name='idx_vectors'");
        if (count != expected_count) {
            throw std::runtime_error("cosine rebuild row_id_map_size mismatch: expected " +
                                     std::to_string(expected_count) + ", got " + std::to_string(count));
        }
        ExecOrThrow(con, "PRAGMA force_checkpoint");
        ExecOrThrow(con, "CHECKPOINT");
    });
    WithConnection(db_path, extension_path, [&](Connection &con) {
        ValidateCosineIndexQuery(con, expected_nearest_id, IndexScanExpectation::Require);
    });
}

static void ValidateCreateIndexCrash(const std::string &db_path, const std::string &extension_path, int64_t rows) {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        auto count = QuerySingleInt64(con, "SELECT count(*) FROM vectors");
        if (count != rows) {
            throw std::runtime_error("table row-count mismatch after create-index crash");
        }
        if (IndexExists(con)) {
            ValidateIndexQuery(con, 42);
        } else {
            // If the process died before CREATE INDEX committed, the index must
            // be absent but the catalog/table must remain usable.
            ExecOrThrow(con, "CREATE INDEX idx_vectors ON vectors USING GRAPH_INDEX (vec) WITH (metric='l2')");
            ValidateIndexQuery(con, 42);
        }
    });
}

[[noreturn]] static void SleepUntilKilled() {
    while (true) {
        pause();
    }
}

static void ChildCreateIndex(const std::string &db_path, const std::string &extension_path, const std::string &marker) {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        TouchMarker(marker);
        ExecOrThrow(con, "CREATE INDEX idx_vectors ON vectors USING GRAPH_INDEX (vec) WITH (metric='l2')");
        TouchMarker(marker + ".done");
        SleepUntilKilled();
    });
    SleepUntilKilled();
}

static void ChildTxnBeforeCommit(const std::string &db_path, const std::string &extension_path,
                                 const std::string &marker) {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        TouchMarker(marker + ".opened");
        ExecOrThrow(con, "BEGIN TRANSACTION");
        TouchMarker(marker + ".begun");
        ExecOrThrow(con, "INSERT INTO vectors VALUES (6001, " + VectorLiteral(6001) + ")");
        TouchMarker(marker + ".inserted");
        ExecOrThrow(con, "DELETE FROM vectors WHERE id = 7");
        TouchMarker(marker + ".deleted");
        TouchMarker(marker);
        SleepUntilKilled();
    });
    SleepUntilKilled();
}

static void ChildTxnAfterCommit(const std::string &db_path, const std::string &extension_path,
                                const std::string &marker) {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        TouchMarker(marker + ".opened");
        ExecOrThrow(con, "BEGIN TRANSACTION");
        TouchMarker(marker + ".begun");
        ExecOrThrow(con, "INSERT INTO vectors VALUES (6001, " + VectorLiteral(6001) + ")");
        TouchMarker(marker + ".inserted");
        ExecOrThrow(con, "DELETE FROM vectors WHERE id = 7");
        TouchMarker(marker + ".deleted");
        ExecOrThrow(con, "COMMIT");
        TouchMarker(marker + ".committed");
        TouchMarker(marker);
        SleepUntilKilled();
    });
    SleepUntilKilled();
}

static void ChildDeleteAfterCommit(const std::string &db_path, const std::string &extension_path,
                                   const std::string &marker) {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        TouchMarker(marker + ".opened");
        ExecOrThrow(con, "BEGIN TRANSACTION");
        TouchMarker(marker + ".begun");
        ExecOrThrow(con, "DELETE FROM vectors WHERE id = 5000");
        TouchMarker(marker + ".deleted");
        ExecOrThrow(con, "COMMIT");
        TouchMarker(marker + ".committed");
        TouchMarker(marker);
        SleepUntilKilled();
    });
    SleepUntilKilled();
}

static void ChildCosineUpdateAfterCommit(const std::string &db_path, const std::string &extension_path,
                                         const std::string &marker) {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        TouchMarker(marker + ".opened");
        ExecOrThrow(con, "BEGIN TRANSACTION");
        TouchMarker(marker + ".begun");
        ExecOrThrow(con, "UPDATE vectors SET vec = " + UnitVectorLiteral() + " WHERE id = 1234");
        TouchMarker(marker + ".updated");
        ExecOrThrow(con, "COMMIT");
        TouchMarker(marker + ".committed");
        TouchMarker(marker);
        SleepUntilKilled();
    });
    SleepUntilKilled();
}

static void ChildCompactPQUpdateAfterCommit(const std::string &db_path, const std::string &extension_path,
                                            const std::string &marker) {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        TouchMarker(marker + ".opened");
        ExecOrThrow(con, "BEGIN TRANSACTION");
        TouchMarker(marker + ".begun");
        ExecOrThrow(con, "UPDATE vectors SET vec = " + CompactUpdateVectorLiteral() + " WHERE id = 1234");
        TouchMarker(marker + ".updated");
        ExecOrThrow(con, "COMMIT");
        TouchMarker(marker + ".committed");
        TouchMarker(marker);
        SleepUntilKilled();
    });
    SleepUntilKilled();
}

static void ChildCheckpoint(const std::string &db_path, const std::string &extension_path, const std::string &marker) {
    WithConnection(db_path, extension_path, [&](Connection &con) {
        ExecOrThrow(con, "INSERT INTO vectors VALUES (65001, " + VectorLiteral(65001) + ")");
        ExecOrThrow(con, "DELETE FROM vectors WHERE id = 11");
        TouchMarker(marker);
        ExecOrThrow(con, "CHECKPOINT");
        TouchMarker(marker + ".done");
        SleepUntilKilled();
    });
    SleepUntilKilled();
}

static void ChildRepeatedTxn(const std::string &db_path, const std::string &extension_path, const std::string &marker,
                             int64_t iteration) {
    const auto insert_id = 20000 + iteration;
    const auto delete_id = 100 + iteration;
    WithConnection(db_path, extension_path, [&](Connection &con) {
        ExecOrThrow(con, "BEGIN TRANSACTION");
        ExecOrThrow(con, "INSERT INTO vectors VALUES (" + std::to_string(insert_id) + ", " + VectorLiteral(insert_id) +
                             ")");
        ExecOrThrow(con, "DELETE FROM vectors WHERE id = " + std::to_string(delete_id));
        ExecOrThrow(con, "COMMIT");
        if (iteration % 2 == 0) {
            ExecOrThrow(con, "CHECKPOINT");
        }
        TouchMarker(marker);
        SleepUntilKilled();
    });
    SleepUntilKilled();
}

static int ChildMain(int argc, char **argv) {
    if (argc < 6) {
        std::cerr << "usage: crash_recovery --child <scenario> <db_path> <extension_path> <marker> [iteration]\n";
        return 2;
    }
    std::string scenario = argv[2];
    std::string db_path = argv[3];
    std::string extension_path = argv[4];
    std::string marker = argv[5];

    try {
        if (scenario == "create-index") {
            ChildCreateIndex(db_path, extension_path, marker);
        } else if (scenario == "txn-before-commit") {
            ChildTxnBeforeCommit(db_path, extension_path, marker);
        } else if (scenario == "txn-after-commit") {
            ChildTxnAfterCommit(db_path, extension_path, marker);
        } else if (scenario == "delete-after-commit") {
            ChildDeleteAfterCommit(db_path, extension_path, marker);
        } else if (scenario == "cosine-update-after-commit") {
            ChildCosineUpdateAfterCommit(db_path, extension_path, marker);
        } else if (scenario == "compact-pq-update-after-commit") {
            ChildCompactPQUpdateAfterCommit(db_path, extension_path, marker);
        } else if (scenario == "checkpoint") {
            ChildCheckpoint(db_path, extension_path, marker);
        } else if (scenario == "repeated-txn") {
            if (argc < 7) {
                throw std::runtime_error("repeated-txn requires iteration");
            }
            ChildRepeatedTxn(db_path, extension_path, marker, std::stoll(argv[6]));
        } else {
            throw std::runtime_error("unknown child scenario: " + scenario);
        }
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "child scenario '" << scenario << "' failed: " << ex.what() << "\n";
        return 1;
    }
}

static ScenarioResult KillScenario(const std::string &self, const std::string &scenario, const std::string &db_path,
                                   const std::string &extension_path, const std::string &marker,
                                   int kill_delay_ms = 25, const std::vector<std::string> &extra_args = {}) {
    unlink(marker.c_str());
    unlink((marker + ".done").c_str());

    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error("fork failed: " + std::string(std::strerror(errno)));
    }
    if (pid == 0) {
        std::vector<std::string> args = {self, "--child", scenario, db_path, extension_path, marker};
        args.insert(args.end(), extra_args.begin(), extra_args.end());
        std::vector<char *> c_args;
        c_args.reserve(args.size() + 1);
        for (auto &arg : args) {
            c_args.push_back(const_cast<char *>(arg.c_str()));
        }
        c_args.push_back(nullptr);
        execv(self.c_str(), c_args.data());
        std::cerr << "execv failed: " << std::strerror(errno) << "\n";
        _exit(127);
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int status = 0;
    while (!Exists(marker)) {
        auto done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            throw std::runtime_error("child exited before crash marker for scenario " + scenario);
        }
        if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            throw std::runtime_error("timeout waiting for crash marker for scenario " + scenario);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kill_delay_ms));
    ScenarioResult result;
    result.completed_before_kill = Exists(marker + ".done");

    kill(pid, SIGKILL);
    if (waitpid(pid, &status, 0) != pid) {
        throw std::runtime_error("waitpid failed for scenario " + scenario);
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        throw std::runtime_error("child was not killed by SIGKILL for scenario " + scenario);
    }
    return result;
}

static void PrintPass(const std::string &name, const ScenarioResult *result = nullptr) {
    std::cout << "[pass] " << name;
    if (result) {
        std::cout << " (" << (result->completed_before_kill ? "operation completed before kill"
                                                            : "operation interrupted before completion")
                  << ")";
    }
    std::cout << "\n";
}

static void PrintFail(const std::string &name, const std::exception &ex) {
    std::cout << "[fail] " << name << ": " << ex.what() << "\n";
}

static int ParentMain(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        std::cerr << "usage: crash_recovery <extension_path> [work_dir]\n";
        return 2;
    }

    std::string extension_path = argv[1];
    std::string tmp_dir;
    bool cleanup = argc == 2;
    if (argc == 3) {
        tmp_dir = argv[2];
        if (mkdir(tmp_dir.c_str(), 0755) != 0 && errno != EEXIST) {
            throw std::runtime_error("mkdir failed: " + tmp_dir + ": " + std::strerror(errno));
        }
    } else {
        char templ[] = "/tmp/vexdb_duck_crash_recovery_XXXXXX";
        char *created = mkdtemp(templ);
        if (!created) {
            throw std::runtime_error("mkdtemp failed: " + std::string(std::strerror(errno)));
        }
        tmp_dir = created;
    }

    std::cout << "crash_recovery work_dir=" << tmp_dir << "\n";

    std::vector<ScenarioFailure> failures;
    auto run_scenario = [&](const std::string &name, const std::function<void()> &fn) {
        try {
            fn();
        } catch (const std::exception &ex) {
            PrintFail(name, ex);
            failures.push_back({name, ex.what()});
        }
    };

    try {
        run_scenario("kill during CREATE INDEX", [&]() {
            auto db = JoinPath(tmp_dir, "create_index.duckdb");
            CreateBaseDatabase(db, extension_path, 20000, false);
            auto marker = JoinPath(tmp_dir, "create_index.marker");
            auto result = KillScenario(argv[0], "create-index", db, extension_path, marker, 15);
            ValidateCreateIndexCrash(db, extension_path, 20000);
            PrintPass("kill during CREATE INDEX", &result);
        });

        run_scenario("kill before transaction COMMIT", [&]() {
            auto db = JoinPath(tmp_dir, "txn_before_commit.duckdb");
            CreateBaseDatabase(db, extension_path, 5000, true);
            auto marker = JoinPath(tmp_dir, "txn_before_commit.marker");
            KillScenario(argv[0], "txn-before-commit", db, extension_path, marker, 5);
            ValidateDatabase(db, extension_path, 5000, true, 42);
            PrintPass("kill before transaction COMMIT rolls back index/table changes");
        });

        run_scenario("kill after transaction COMMIT", [&]() {
            auto db = JoinPath(tmp_dir, "txn_after_commit.duckdb");
            CreateBaseDatabase(db, extension_path, 5000, true);
            auto marker = JoinPath(tmp_dir, "txn_after_commit.marker");
            KillScenario(argv[0], "txn-after-commit", db, extension_path, marker, 5);
            WithConnection(db, extension_path, [&](Connection &con) {
                auto deleted = QuerySingleInt64(con, "SELECT count(*) FROM vectors WHERE id=7");
                if (deleted != 0) {
                    throw std::runtime_error("committed delete did not survive crash");
                }
            });
            ValidateDatabase(db, extension_path, 5000, true, 6001, IndexScanExpectation::Forbid);
            RebuildAndValidateIndex(db, extension_path, 5000, 6001);
            PrintPass("kill after transaction COMMIT falls back when index is stale, then rebuild restores it");
        });

        run_scenario("kill after delete-only COMMIT", [&]() {
            auto db = JoinPath(tmp_dir, "delete_after_commit.duckdb");
            CreateBaseDatabase(db, extension_path, 5000, true);
            auto marker = JoinPath(tmp_dir, "delete_after_commit.marker");
            KillScenario(argv[0], "delete-after-commit", db, extension_path, marker, 5);
            WithConnection(db, extension_path, [&](Connection &con) {
                auto deleted = QuerySingleInt64(con, "SELECT count(*) FROM vectors WHERE id=5000");
                if (deleted != 0) {
                    throw std::runtime_error("committed delete-only transaction did not survive crash");
                }
            });
            ValidateDatabase(db, extension_path, 4999, true, 4999, IndexScanExpectation::Forbid);
            RebuildAndValidateIndex(db, extension_path, 4999, 4999);
            PrintPass("kill after delete-only COMMIT falls back when index is stale, then rebuild restores it");
        });

        run_scenario("kill after cosine UPDATE COMMIT", [&]() {
            auto db = JoinPath(tmp_dir, "cosine_update_after_commit.duckdb");
            CreateBaseDatabase(db, extension_path, 5000, true, "cosine");
            auto marker = JoinPath(tmp_dir, "cosine_update_after_commit.marker");
            KillScenario(argv[0], "cosine-update-after-commit", db, extension_path, marker, 5);
            WithConnection(db, extension_path, [&](Connection &con) {
                auto count = QuerySingleInt64(con, "SELECT count(*) FROM vectors");
                if (count != 5000) {
                    throw std::runtime_error("cosine update changed row count after recovery");
                }
                ValidateCosineIndexQuery(con, 1234, IndexScanExpectation::Allow);
            });
            RebuildAndValidateCosineIndex(db, extension_path, 5000, 1234);
            PrintPass("kill after cosine UPDATE COMMIT returns the recovered vector, then rebuild keeps it queryable");
        });

        run_scenario("kill after compact PQ UPDATE COMMIT", [&]() {
            auto db = JoinPath(tmp_dir, "compact_pq_update_after_commit.duckdb");
            CreateBaseDatabase(db, extension_path, 5000, true, "l2",
                               "quantizer='pq', pq_m=4, memory_mode='compact'");
            auto marker = JoinPath(tmp_dir, "compact_pq_update_after_commit.marker");
            KillScenario(argv[0], "compact-pq-update-after-commit", db, extension_path, marker, 5);
            WithConnection(db, extension_path, [&](Connection &con) {
                auto count = QuerySingleInt64(con, "SELECT count(*) FROM vectors");
                if (count != 5000) {
                    throw std::runtime_error("compact PQ update changed row count after recovery");
                }
                ValidateL2IndexQuery(con, 1234, CompactUpdateVectorLiteral(), IndexScanExpectation::Allow);
            });
            RebuildAndValidateL2Index(db, extension_path, 5000, 1234, CompactUpdateVectorLiteral());
            PrintPass("kill after compact PQ UPDATE COMMIT returns the recovered vector, then rebuild restores it");
        });

        run_scenario("kill during CHECKPOINT/index serialization", [&]() {
            auto db = JoinPath(tmp_dir, "checkpoint.duckdb");
            CreateBaseDatabase(db, extension_path, 60000, true);
            auto marker = JoinPath(tmp_dir, "checkpoint.marker");
            auto result = KillScenario(argv[0], "checkpoint", db, extension_path, marker, 1);
            WithConnection(db, extension_path, [&](Connection &con) {
                auto deleted = QuerySingleInt64(con, "SELECT count(*) FROM vectors WHERE id=11");
                if (deleted != 0) {
                    throw std::runtime_error("checkpoint-crash delete did not survive recovery");
                }
            });
            ValidateDatabase(db, extension_path, 60000, true, 65001, IndexScanExpectation::Forbid);
            RebuildAndValidateIndex(db, extension_path, 60000, 65001);
            PrintPass("kill during CHECKPOINT/index serialization, then rebuild restores stale index", &result);
        });

        run_scenario("five repeated crash/recovery cycles", [&]() {
            auto db = JoinPath(tmp_dir, "repeated.duckdb");
            CreateBaseDatabase(db, extension_path, 12000, true);
            int64_t expected_count = 12000;
            for (int64_t i = 1; i <= 5; i++) {
                auto marker = JoinPath(tmp_dir, "repeated_" + std::to_string(i) + ".marker");
                KillScenario(argv[0], "repeated-txn", db, extension_path, marker, 5, {std::to_string(i)});
                // One insert and one delete of an existing row per iteration.
                ValidateDatabase(db, extension_path, expected_count, true, 20000 + i, IndexScanExpectation::Allow);
            }
            PrintPass("five repeated crash/recovery cycles");
        });

        if (!failures.empty()) {
            std::string message = "crash_recovery had " + std::to_string(failures.size()) + " failing scenario(s)";
            for (auto &failure : failures) {
                message += "\n- " + failure.name + ": " + failure.message;
            }
            throw std::runtime_error(message);
        }

        if (cleanup) {
            RemoveTree(tmp_dir);
        }
        std::cout << "crash_recovery: ok\n";
        return 0;
    } catch (...) {
        std::cerr << "crash_recovery retained work_dir=" << tmp_dir << "\n";
        throw;
    }
}

} // namespace

int main(int argc, char **argv) {
    try {
        std::cout << std::unitbuf;
        if (argc >= 2 && std::string(argv[1]) == "--child") {
            return ChildMain(argc, argv);
        }
        return ParentMain(argc, argv);
    } catch (const std::exception &ex) {
        std::cerr << "crash_recovery: fail: " << ex.what() << "\n";
        return 1;
    }
}
