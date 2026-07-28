#include "duckdb.hpp"
#include "core_functions_extension.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

using namespace duckdb;

namespace {

constexpr idx_t DIM = 32;
constexpr idx_t K = 10;
constexpr idx_t PQ_M = 16;
constexpr double MIN_RECALL = 0.80;
constexpr double MIN_QPS_RATIO = 0.70;
constexpr double MAX_BUILD_RATIO = 3.0;

struct Timer {
    std::chrono::steady_clock::time_point started;
    void Start() { started = std::chrono::steady_clock::now(); }
    double Ms() const {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - started)
            .count();
    }
};

struct Stats {
    double build_ms = 0;
    double qps = 0;
    double update_qps = 0;
    double recall = 0;
    bool uses_index = false;
    bool quantizer_active = false;
    int64_t memory_bytes = 0;
    int64_t code_bytes = 0;
    int64_t resident_bytes = 0;
};

struct Mode {
    const char *name;
    const char *quantizer;
    const char *memory_mode;
};

double Median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    if (values.size() % 2 != 0) return values[middle];
    return (values[middle - 1] + values[middle]) / 2.0;
}

Stats MedianStats(const std::vector<Stats> &runs) {
    std::vector<double> build_ms;
    std::vector<double> qps;
    std::vector<double> recall;
    std::vector<double> update_qps;
    build_ms.reserve(runs.size());
    qps.reserve(runs.size());
    recall.reserve(runs.size());
    update_qps.reserve(runs.size());
    Stats result;
    result.uses_index = true;
    result.quantizer_active = true;
    for (const auto &run : runs) {
        build_ms.push_back(run.build_ms);
        qps.push_back(run.qps);
        recall.push_back(run.recall);
        update_qps.push_back(run.update_qps);
        result.uses_index = result.uses_index && run.uses_index;
        result.quantizer_active = result.quantizer_active && run.quantizer_active;
    }
    result.build_ms = Median(std::move(build_ms));
    result.qps = Median(std::move(qps));
    result.recall = Median(std::move(recall));
    result.update_qps = Median(std::move(update_qps));
    std::vector<double> memory_bytes;
    std::vector<double> code_bytes;
    std::vector<double> resident_bytes;
    memory_bytes.reserve(runs.size());
    code_bytes.reserve(runs.size());
    resident_bytes.reserve(runs.size());
    for (const auto &run : runs) {
        memory_bytes.push_back(static_cast<double>(run.memory_bytes));
        code_bytes.push_back(static_cast<double>(run.code_bytes));
        resident_bytes.push_back(static_cast<double>(run.resident_bytes));
    }
    result.memory_bytes = static_cast<int64_t>(Median(std::move(memory_bytes)));
    result.code_bytes = static_cast<int64_t>(Median(std::move(code_bytes)));
    result.resident_bytes = static_cast<int64_t>(Median(std::move(resident_bytes)));
    return result;
}

int64_t CurrentResidentBytes() {
#if defined(__APPLE__)
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<int64_t>(info.resident_size);
    }
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    uint64_t total_pages = 0;
    uint64_t resident_pages = 0;
    if (statm >> total_pages >> resident_pages) {
        const long page_size = sysconf(_SC_PAGESIZE);
        if (page_size > 0 &&
            resident_pages <= static_cast<uint64_t>(INT64_MAX) /
                                  static_cast<uint64_t>(page_size)) {
            return static_cast<int64_t>(resident_pages *
                                        static_cast<uint64_t>(page_size));
        }
    }
#endif
    return 0;
}

unique_ptr<QueryResult> ExecOrThrow(Connection &con, const string &sql) {
    auto result = con.Query(sql);
    if (!result) {
        throw std::runtime_error("query returned null: " + sql);
    }
    if (result->HasError()) {
        result->ThrowError("query failed: " + sql + "\n");
    }
    return result;
}

Value MakeArrayValue(const float *vec) {
    std::vector<Value> values;
    values.reserve(DIM);
    for (idx_t d = 0; d < DIM; d++) {
        values.emplace_back(Value::FLOAT(vec[d]));
    }
    return Value::ARRAY(LogicalType::FLOAT, std::move(values));
}

string MakeArrayLiteral(const float *vec) {
    std::ostringstream out;
    out << '[' << std::setprecision(9);
    for (idx_t d = 0; d < DIM; d++) {
        if (d) out << ',';
        out << vec[d];
    }
    out << "]::FLOAT[" << DIM << ']';
    return out.str();
}

std::vector<int32_t> ReadTopIds(Connection &con, const string &sql) {
    auto result = ExecOrThrow(con, sql);
    auto &mat = result->Cast<MaterializedQueryResult>();
    std::vector<int32_t> ids;
    ids.reserve(K);
    for (idx_t row = 0; row < mat.RowCount() && row < K; row++) {
        ids.push_back(IntegerValue::Get(
            mat.GetValue(0, row).DefaultCastAs(LogicalType::INTEGER)));
    }
    return ids;
}

bool ExplainUsesIndex(Connection &con, const string &sql) {
    auto result = ExecOrThrow(con, "EXPLAIN " + sql);
    auto &mat = result->Cast<MaterializedQueryResult>();
    for (idx_t row = 0; row < mat.RowCount(); row++) {
        for (idx_t col = 0; col < mat.ColumnCount(); col++) {
            if (mat.GetValue(col, row).ToString().find("VEXDB_INDEX_SCAN") !=
                string::npos) {
                return true;
            }
        }
    }
    return false;
}

void ReadIndexInfo(Connection &con, const Mode &mode, Stats &stats) {
    auto result = ExecOrThrow(
        con,
        "SELECT quantizer, memory_mode, memory_bytes, "
        "pq_codes_bytes + pq_codebook_bytes, "
        "rabitq_codes_bytes + rabitq_fixed_bytes "
        "FROM vexdb_index_info() WHERE index_name = 'idx_bench'");
    auto &mat = result->Cast<MaterializedQueryResult>();
    if (mat.RowCount() != 1) {
        return;
    }
    const auto actual_quantizer = mat.GetValue(0, 0).ToString();
    const auto actual_memory_mode = mat.GetValue(1, 0).ToString();
    stats.memory_bytes = BigIntValue::Get(mat.GetValue(2, 0));
    const auto pq_bytes = BigIntValue::Get(mat.GetValue(3, 0));
    const auto rabitq_bytes = BigIntValue::Get(mat.GetValue(4, 0));
    stats.code_bytes = pq_bytes + rabitq_bytes;
    const bool code_ok = std::string(mode.quantizer) == "none" || stats.code_bytes > 0;
    stats.quantizer_active = actual_quantizer == mode.quantizer &&
                             actual_memory_mode == mode.memory_mode && code_ok;
}

float L2Squared(const float *a, const float *b) {
    float distance = 0;
    for (idx_t d = 0; d < DIM; d++) {
        const float delta = a[d] - b[d];
        distance += delta * delta;
    }
    return distance;
}

std::vector<int32_t> ExactTopK(const std::vector<float> &data,
                               const float *query) {
    const idx_t count = data.size() / DIM;
    std::vector<std::pair<float, int32_t>> distances;
    distances.reserve(count);
    for (idx_t i = 0; i < count; i++) {
        distances.emplace_back(L2Squared(data.data() + i * DIM, query),
                               static_cast<int32_t>(i));
    }
    std::partial_sort(distances.begin(), distances.begin() + K, distances.end());
    std::vector<int32_t> result;
    result.reserve(K);
    for (idx_t i = 0; i < K; i++) result.push_back(distances[i].second);
    return result;
}

double Recall(const std::vector<int32_t> &actual,
              const std::vector<int32_t> &expected) {
    idx_t hits = 0;
    for (auto id : actual) {
        if (std::find(expected.begin(), expected.end(), id) != expected.end()) {
            hits++;
        }
    }
    return static_cast<double>(hits) / static_cast<double>(K);
}

Stats RunMode(const string &extension_path, const std::vector<float> &data,
              const std::vector<float> &queries,
              const std::vector<std::vector<int32_t>> &truth,
              const Mode &mode, idx_t requested_updates) {
    DBConfig config;
    config.SetOptionByName("allow_unsigned_extensions", true);
    DuckDB db(nullptr, &config);
    db.LoadStaticExtension<CoreFunctionsExtension>();
    Connection con(db);
    ExecOrThrow(con, "LOAD '" + extension_path + "'");
    ExecOrThrow(con, "SET threads=4");
    ExecOrThrow(con, "SET vexdb_brute_force_threshold=0");
    ExecOrThrow(con, "SET vexdb_ef_search=100");
    if (std::string(mode.quantizer) == "pq") {
        ExecOrThrow(con, "SET vexdb_pq_search_mode='pq_only'");
        ExecOrThrow(con, "SET vexdb_pq_refine_k_factor=1.0");
    }
    ExecOrThrow(con, "CREATE TABLE bench(id INTEGER, vec FLOAT[32])");

    {
        Appender app(con, "bench");
        for (idx_t i = 0; i < data.size() / DIM; i++) {
            app.BeginRow();
            app.Append(static_cast<int32_t>(i));
            app.Append(MakeArrayValue(data.data() + i * DIM));
            app.EndRow();
        }
        app.Close();
    }

    string create =
        "CREATE INDEX idx_bench ON bench USING GRAPH_INDEX (vec) WITH "
        "(metric='l2', m=16, ef_construction=128, quantizer='" +
        string(mode.quantizer) + "', memory_mode='" + mode.memory_mode + "'";
    if (std::string(mode.quantizer) == "pq") {
        create += ", pq_m=" + std::to_string(PQ_M);
    }
    create += ")";
    Timer timer;
    timer.Start();
    ExecOrThrow(con, create);
    Stats stats;
    stats.build_ms = timer.Ms();
    ReadIndexInfo(con, mode, stats);

    std::vector<string> sql;
    sql.reserve(queries.size() / DIM);
    for (idx_t q = 0; q < queries.size() / DIM; q++) {
        sql.push_back("SELECT id FROM bench ORDER BY l2_distance(vec," +
                      MakeArrayLiteral(queries.data() + q * DIM) + ") LIMIT 10");
    }
    stats.uses_index = ExplainUsesIndex(con, sql.front());
    ReadTopIds(con, sql.front());

    double recall_sum = 0;
    timer.Start();
    for (idx_t q = 0; q < sql.size(); q++) {
        recall_sum += Recall(ReadTopIds(con, sql[q]), truth[q]);
    }
    const double query_ms = timer.Ms();
    stats.qps = static_cast<double>(sql.size()) / (query_ms / 1000.0);
    stats.recall = recall_sum / static_cast<double>(sql.size());

    // Measure committed row replacement separately from index build/search.
    // DuckDB implements UPDATE as secondary-index DELETE + INSERT. A row-id
    // lookup that scans every graph node makes this rate fall linearly with N.
    const idx_t update_count = std::min<idx_t>(requested_updates, data.size() / DIM);
    timer.Start();
    for (idx_t i = 0; i < update_count; i++) {
        std::vector<float> updated(data.begin() + i * DIM,
                                   data.begin() + (i + 1) * DIM);
        updated[0] += 10.0f;
        ExecOrThrow(con, "UPDATE bench SET vec=" + MakeArrayLiteral(updated.data()) +
                         " WHERE id=" + std::to_string(i));
    }
    const double update_ms = timer.Ms();
    stats.update_qps = static_cast<double>(update_count) / (update_ms / 1000.0);
    // Capture while the database and index are still alive. This is the real
    // process RSS after build/query/update, separate from the index's own
    // accounting and from the build-phase peak reported by /usr/bin/time -l.
    stats.resident_bytes = CurrentResidentBytes();
    return stats;
}

void PrintStats(const char *name, const Stats &stats) {
    std::cout << std::left << std::setw(10) << name << " build_ms=" << std::fixed
              << std::setprecision(2) << stats.build_ms << " qps=" << stats.qps
              << " update_qps=" << stats.update_qps
              << " recall@10=" << std::setprecision(4) << stats.recall
              << " index_scan=" << (stats.uses_index ? "yes" : "no")
              << " quantizer_active=" << (stats.quantizer_active ? "yes" : "no")
              << " memory_mib=" << std::setprecision(2)
              << static_cast<double>(stats.memory_bytes) / (1024.0 * 1024.0)
              << " code_mib=" << static_cast<double>(stats.code_bytes) / (1024.0 * 1024.0)
              << " rss_mib=" << static_cast<double>(stats.resident_bytes) /
                                     (1024.0 * 1024.0)
              << '\n';
}

} // namespace

int main(int argc, char **argv) {
    const string extension_path = argc > 1
        ? argv[1]
        : "build/duck/v1.5.2/build/extension/vexdb_lite/vexdb_lite.duckdb_extension";
    const idx_t count = argc > 2 ? std::stoul(argv[2]) : 20000;
    const idx_t query_count = argc > 3 ? std::stoul(argv[3]) : 200;
    const idx_t repetitions = argc > 4 ? std::stoul(argv[4]) : 5;
    const idx_t update_count = argc > 5 ? std::stoul(argv[5]) : 64;
    const string mode_filter = argc > 6 ? argv[6] : "";
    if (count < K || query_count == 0 || repetitions == 0 || update_count == 0) {
        std::cerr << "count must be >= 10; query_count, repetitions and updates must be > 0\n";
        return 2;
    }

    try {
        std::vector<float> data(count * DIM);
        for (idx_t i = 0; i < count; i++) {
            for (idx_t d = 0; d < DIM; d++) {
                data[i * DIM + d] =
                    std::sin(static_cast<double>(i + 1) * 0.011 * (d + 1)) +
                    0.5f * std::cos(static_cast<double>(i + 3) * 0.007 + d * 0.17);
            }
        }
        std::vector<float> queries(query_count * DIM);
        std::vector<std::vector<int32_t>> truth;
        truth.reserve(query_count);
        for (idx_t q = 0; q < query_count; q++) {
            const idx_t source = (q * 199 + 17) % count;
            for (idx_t d = 0; d < DIM; d++) {
                queries[q * DIM + d] = data[source * DIM + d] +
                    0.002f * std::sin(static_cast<double>((q + 1) * (d + 1)));
            }
            truth.push_back(ExactTopK(data, queries.data() + q * DIM));
        }

        const std::vector<Mode> available_modes = {
            {"plain", "none", "full"},
            {"pq-full", "pq", "full"},
            {"pq-compact", "pq", "compact"},
            {"rabitq-full", "rabitq", "full"},
            {"rabitq-compact", "rabitq", "compact"},
        };
        std::vector<Mode> modes;
        for (const auto &mode : available_modes) {
            if (mode_filter.empty() || mode_filter == mode.name) {
                modes.push_back(mode);
            }
        }
        if (modes.empty()) {
            throw std::invalid_argument(
                "unknown mode filter; expected plain, pq-full, pq-compact, "
                "rabitq-full, or rabitq-compact");
        }
        std::cout << "synthetic_l2 vectors=" << count << " dim=" << DIM
                  << " queries=" << query_count << " k=" << K
                  << " pq_m=" << PQ_M << " repetitions=" << repetitions
                  << " updates=" << update_count;
        if (!mode_filter.empty()) std::cout << " mode=" << mode_filter;
        std::cout << '\n';
        std::vector<std::vector<Stats>> all_runs(modes.size());
        for (auto &runs : all_runs) runs.reserve(repetitions);
        for (idx_t run = 0; run < repetitions; run++) {
            std::vector<idx_t> order;
            order.reserve(modes.size());
            for (idx_t i = 0; i < modes.size(); i++) order.push_back(i);
            if (run % 2 != 0) std::reverse(order.begin(), order.end());
            std::rotate(order.begin(), order.begin() + (run % order.size()), order.end());
            std::cout << "run " << (run + 1) << '\n';
            for (auto mode_index : order) {
                auto stats = RunMode(extension_path, data, queries, truth,
                                     modes[mode_index], update_count);
                PrintStats(modes[mode_index].name, stats);
                all_runs[mode_index].push_back(stats);
            }
        }
        std::cout << "median\n";
        std::vector<Stats> medians;
        medians.reserve(modes.size());
        for (idx_t i = 0; i < modes.size(); i++) {
            medians.push_back(MedianStats(all_runs[i]));
            PrintStats(modes[i].name, medians.back());
        }
        bool accepted = true;
        const auto plain_it = std::find_if(
            modes.begin(), modes.end(), [](const Mode &mode) {
                return std::string(mode.name) == "plain";
            });
        const bool has_plain = plain_it != modes.end();
        const idx_t plain_index = has_plain
            ? static_cast<idx_t>(std::distance(modes.begin(), plain_it)) : 0;
        for (idx_t i = 0; i < modes.size(); i++) {
            accepted = accepted && medians[i].uses_index && medians[i].quantizer_active &&
                       medians[i].recall >= MIN_RECALL;
            if (has_plain) {
                const auto build_ratio =
                    medians[i].build_ms / medians[plain_index].build_ms;
                const auto qps_ratio = medians[i].qps / medians[plain_index].qps;
                std::cout << modes[i].name << "/plain build_ratio=" << build_ratio
                          << " qps_ratio=" << qps_ratio << '\n';
                accepted = accepted && build_ratio <= MAX_BUILD_RATIO &&
                           qps_ratio >= MIN_QPS_RATIO;
            }
        }
        if (!accepted) {
            std::cerr << "benchmark acceptance failed\n";
            return 1;
        }
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
