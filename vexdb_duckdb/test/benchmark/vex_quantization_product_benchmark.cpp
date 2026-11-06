#include "duckdb.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#endif
#include <sys/resource.h>
#include <unistd.h>

using namespace duckdb;
using Clock = std::chrono::steady_clock;

namespace {
double Milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

uint64_t ResidentBytes() {
#ifdef __APPLE__
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        throw std::runtime_error("cannot read resident size");
    return info.resident_size;
#else
    std::ifstream stat("/proc/self/statm");
    uint64_t total, resident;
    if (!(stat >> total >> resident)) throw std::runtime_error("cannot read /proc/self/statm");
    return resident * uint64_t(sysconf(_SC_PAGESIZE));
#endif
}

unique_ptr<MaterializedQueryResult> Exec(Connection &con, const std::string &sql) {
    auto result = con.Query(sql);
    if (!result || result->HasError()) throw std::runtime_error("SQL failed: " + sql + "\n" + (result ? result->GetError() : "null"));
    return result;
}

std::string Quote(const std::string &s) { return "'" + StringUtil::Replace(s, "'", "''") + "'"; }

template <class T> std::vector<T> Read(const std::string &path, size_t count) {
    std::ifstream file(path, std::ios::binary);
    std::vector<T> data(count);
    if (!file.read(reinterpret_cast<char *>(data.data()), std::streamsize(count * sizeof(T))))
        throw std::runtime_error("short input: " + path);
    return data;
}

std::string QuerySQL(int dim) {
    return "SELECT id FROM vectors ORDER BY l2_distance(vec, ?::FLOAT[" + std::to_string(dim) + "]) LIMIT 10";
}

void Configure(Connection &con, const std::string &mode, int ef, double factor) {
    Exec(con, "SET threads=1");
    Exec(con, "SET vexdb_brute_force_threshold=0");
    Exec(con, "SET vexdb_pq_search_mode=" + Quote(mode));
    Exec(con, "SET vexdb_ef_search=" + std::to_string(ef));
    Exec(con, "SET vexdb_pq_refine_k_factor=" + std::to_string(factor));
}

std::array<int32_t, 10> Search(PreparedStatement &query, const Value &value) {
    vector<Value> parameters {value};
    auto result = query.Execute(parameters, false);
    if (!result || result->HasError()) throw std::runtime_error(result ? result->GetError() : "null query result");
    auto &mat = result->Cast<MaterializedQueryResult>();
    if (mat.RowCount() != 10) throw std::runtime_error("expected ten neighbors");
    std::array<int32_t, 10> ids;
    for (int i = 0; i < 10; ++i) ids[i] = mat.GetValue(0, i).GetValue<int32_t>();
    auto sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) throw std::runtime_error("duplicate output id");
    return ids;
}

void CheckPlan(Connection &con, int dim, const Value &query, const std::filesystem::path &path) {
    auto explain = con.Prepare("EXPLAIN " + QuerySQL(dim));
    if (explain->HasError()) throw std::runtime_error(explain->GetError());
    vector<Value> parameters {query};
    auto result = explain->Execute(parameters, false);
    if (result->HasError()) throw std::runtime_error(result->GetError());
    std::string plan = result->ToString();
    std::ofstream(path) << plan;
    if (plan.find("VEXDB_INDEX_SCAN") == std::string::npos) throw std::runtime_error("query bypassed index: " + plan);
}

struct Sample {
    int repeat, worker, query;
    double ms;
    std::array<int32_t, 10> ids;
};

double Percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, size_t(std::ceil(fraction * values.size()) - 1))];
}

void Measure(DuckDB &db, Connection &control, const std::vector<Value> &queries,
             const std::vector<int32_t> &truth, int dim, const std::string &point,
             const std::string &mode, int ef, double factor, int connections,
             const std::filesystem::path &out, std::ofstream &summary) {
    Configure(control, mode, ef, factor);
    CheckPlan(control, dim, queries[0], out / (point + ".explain.txt"));
    auto prepared = control.Prepare(QuerySQL(dim));
    if (prepared->HasError()) throw std::runtime_error(prepared->GetError());
    auto cold_start = Clock::now();
    Search(*prepared, queries[0]);
    const double first_ms = Milliseconds(cold_start);
    for (size_t q = 0; q < std::min<size_t>(16, queries.size()); ++q) Search(*prepared, queries[q]);
    const uint64_t rss_warm = ResidentBytes();
    std::vector<std::vector<Sample>> samples(connections);
    std::vector<std::string> errors(connections);
    std::atomic<int> ready {0};
    std::atomic<bool> start {false};
    std::vector<std::thread> workers;
    std::vector<double> worker_wall(connections);
    for (int worker = 0; worker < connections; ++worker) {
        workers.emplace_back([&, worker] {
            bool signaled = false;
            try {
                Connection con(db);
                Configure(con, mode, ef, factor);
                auto statement = con.Prepare(QuerySQL(dim));
                if (statement->HasError()) throw std::runtime_error(statement->GetError());
                // Preparation/rebinding warmup is outside the timed workload.
                Search(*statement, queries[worker % queries.size()]);
                ++ready;
                signaled = true;
                while (!start.load()) std::this_thread::yield();
                const auto worker_start = Clock::now();
                for (int repeat = 0; repeat < 3; ++repeat) {
                    std::vector<size_t> order;
                    for (size_t q = worker; q < queries.size(); q += connections) order.push_back(q);
                    std::mt19937 rng(20261027 + repeat);
                    std::shuffle(order.begin(), order.end(), rng);
                    for (auto q : order) {
                        auto begin = Clock::now();
                        auto ids = Search(*statement, queries[q]);
                        const auto ms = Milliseconds(begin);
                        samples[worker].push_back({repeat, worker, int(q), ms, ids});
                    }
                }
                worker_wall[worker] = Milliseconds(worker_start);
            } catch (const std::exception &error) {
                errors[worker] = error.what();
                if (!signaled) ++ready;
            }
        });
    }
    while (ready.load() != connections) std::this_thread::yield();
    const auto wall_start = Clock::now();
    start = true;
    for (auto &worker : workers) worker.join();
    const auto wall_ms = Milliseconds(wall_start);
    for (const auto &error : errors) if (!error.empty()) throw std::runtime_error(error);
    std::ofstream raw(out / (point + "-c" + std::to_string(connections) + ".csv"));
    raw << "repeat,worker,query,ms,hits,ids\n" << std::setprecision(12);
    uint64_t count = 0, hits = 0;
    std::vector<double> latencies;
    for (const auto &group : samples) for (const auto &sample : group) {
        int found = 0;
        for (auto id : sample.ids) for (int k = 0; k < 10; ++k) if (id == truth[sample.query * 10 + k]) { ++found; break; }
        raw << sample.repeat << ',' << sample.worker << ',' << sample.query << ',' << sample.ms << ',' << found << ',';
        for (auto id : sample.ids) raw << id << ' ';
        raw << '\n';
        hits += found;
        ++count;
        latencies.push_back(sample.ms);
    }
    if (count != queries.size() * 3) throw std::runtime_error("incomplete timed query set");
    const auto recall = double(hits) / (count * 10);
    summary << point << ',' << connections << ',' << count << ',' << recall << ','
            << count * 1000.0 / wall_ms << ',' << Percentile(latencies, .5) << ','
            << Percentile(latencies, .95) << ',' << first_ms << ',' << rss_warm << ','
            << ResidentBytes() << ',' << wall_ms << '\n' << std::flush;
    std::ofstream wall(out / (point + "-c" + std::to_string(connections) + "-wall.csv"));
    wall << "worker,ms\n" << std::setprecision(12);
    for (int worker = 0; worker < connections; ++worker) wall << worker << ',' << worker_wall[worker] << '\n';
    std::cout << "POINT " << point << " c=" << connections << " recall=" << recall
              << " p95_ms=" << Percentile(latencies, .95) << " qps=" << count * 1000.0 / wall_ms << std::endl;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 8) {
        std::cerr << "usage: vex_quantization_product_benchmark <extension> <data_dir> <rows> <dim> <query_count> <raw|pq|compact> <new_output>\n";
        return 2;
    }
    try {
        const std::string extension = std::filesystem::absolute(argv[1]).string(), data = argv[2], kind = argv[6];
        const size_t n = std::stoull(argv[3]), dim = std::stoull(argv[4]), nq = std::stoull(argv[5]);
        const std::filesystem::path out = argv[7];
        if (kind != "raw" && kind != "pq" && kind != "compact") throw std::runtime_error("unknown index kind");
        if (!n || !nq || nq > 200 || dim > 65535 || std::filesystem::exists(out)) throw std::runtime_error("invalid sizes or output exists");
        std::filesystem::create_directories(out);
        std::ofstream metadata(out / "metadata.csv");
        auto field = [&](const std::string &key, auto value) { metadata << key << ',' << value << '\n' << std::flush; };
        metadata << std::setprecision(12);
        field("duckdb_version", DuckDB::LibraryVersion());
        field("rows", n); field("dim", dim); field("queries", nq); field("kind", kind);
        field("rss_start", ResidentBytes());
        const auto query_floats = Read<float>(data + "/queries.f32", nq * dim);
        const auto truth = Read<int32_t>(data + "/truth-" + std::to_string(n) + ".i32", nq * 10);
        std::vector<Value> queries;
        for (size_t q = 0; q < nq; ++q) {
            vector<Value> values;
            for (size_t j = 0; j < dim; ++j) values.push_back(Value::FLOAT(query_floats[q * dim + j]));
            queries.push_back(Value::ARRAY(LogicalType::FLOAT, std::move(values)));
        }
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        const auto database = out / "vectors.duckdb";
        DuckDB db(database.string(), &config);
        Connection con(db);
        Exec(con, "LOAD " + Quote(extension));
        Configure(con, "off", 128, 1);
        Exec(con, "SET memory_limit='4GB'");
        Exec(con, "CREATE TABLE vectors (id INTEGER, vec FLOAT[" + std::to_string(dim) + "])");
        const auto load_start = Clock::now();
        {
            std::ifstream base(data + "/base.f32", std::ios::binary);
            Appender app(con, "vectors");
            DataChunk chunk;
            chunk.Initialize(Allocator::DefaultAllocator(), {LogicalType::INTEGER, LogicalType::ARRAY(LogicalType::FLOAT, dim)});
            for (size_t offset = 0; offset < n; offset += STANDARD_VECTOR_SIZE) {
                chunk.Reset();
                const auto count = std::min<size_t>(STANDARD_VECTOR_SIZE, n - offset);
                auto ids = FlatVector::GetData<int32_t>(chunk.data[0]);
                auto vectors = FlatVector::GetData<float>(ArrayVector::GetEntry(chunk.data[1]));
                for (size_t row = 0; row < count; ++row) ids[row] = int32_t(offset + row);
                if (!base.read(reinterpret_cast<char *>(vectors), std::streamsize(count * dim * sizeof(float))))
                    throw std::runtime_error("base file truncated");
                chunk.SetCardinality(count);
                app.AppendDataChunk(chunk);
            }
            app.Close();
        }
        field("load_ms", Milliseconds(load_start));
        Exec(con, "CHECKPOINT");
        field("table_file_bytes", std::filesystem::file_size(database));
        field("rss_table", ResidentBytes());
        auto build_start = Clock::now();
        std::string create = "CREATE INDEX vector_idx ON vectors USING GRAPH_INDEX(vec) WITH (metric='l2', m=16, ef_construction=100, threads=1";
        if (kind != "raw") create += ", quantizer='pq'";
        if (kind == "compact") create += ", memory_mode='compact'";
        Exec(con, create + ")");
        field("build_ms", Milliseconds(build_start));
        field("rss_built", ResidentBytes());
        Exec(con, "CHECKPOINT");
        field("indexed_file_bytes", std::filesystem::file_size(database));
        auto info = Exec(con, "SELECT * FROM vexdb_index_info()");
        std::ofstream(out / "index-info.txt") << info->ToString();
        auto stats = Exec(con, "SELECT memory_bytes,pq_codes_bytes,pq_codebook_bytes,pq_m FROM vexdb_index_info()");
        for (idx_t i = 0; i < 4; ++i) field(std::array<const char *, 4>{"allocator_bytes", "pq_code_bytes", "pq_codebook_bytes", "pq_m"}[i], stats->GetValue(i, 0).ToString());
        std::ofstream summary(out / "points.csv");
        summary << std::setprecision(12) << "point,connections,calls,recall10,qps,p50_ms,p95_ms,first_ms,rss_warm,rss_after,wall_ms\n";
        if (kind == "raw") {
            for (int ef : {40, 128, 512}) for (int connections : {1, 8})
                Measure(db, con, queries, truth, dim, "graph-ef" + std::to_string(ef), "off", ef, 1, connections, out, summary);
        } else {
            const std::vector<int> factors = kind == "compact" ? std::vector<int>{1} : std::vector<int>{1, 4, 16};
            for (int factor : factors) for (int connections : {1, 8})
                Measure(db, con, queries, truth, dim, "pq-r" + std::to_string(factor), "pq_only", 128, factor, connections, out, summary);
        }
        rusage usage;
        getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
        field("peak_rss_bytes", usage.ru_maxrss);
#else
        field("peak_rss_bytes", uint64_t(usage.ru_maxrss) * 1024);
#endif
        field("rss_finished", ResidentBytes());
        std::cout << "COMPLETE " << kind << " n=" << n << " dim=" << dim << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAILED " << error.what() << '\n';
        return 1;
    }
}
