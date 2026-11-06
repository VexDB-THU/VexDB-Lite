// Separately versioned workload: allow up to 4096 queries for the frozen new-query check.
#include "vex_graph_index.hpp"

// Keep the historical timer/build program unchanged. Reuse its SQL binding,
// materialization and returned-ID checks in this separately versioned workload.
#define main quantization_build_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/data_table_info.hpp"

#include <condition_variable>
#include <ctime>
#include <mutex>

namespace {
int64_t Nanoseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count();
}

double CPUTimeMS(const rusage &usage) {
    return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
           (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
}

struct ConcurrentSample {
    int repeat, query;
    int64_t begin_ns, end_ns;
    std::array<int32_t, 10> ids;
};

void MeasureConcurrent(DuckDB &db, Connection &control, const std::vector<Value> &queries,
                       const std::vector<int32_t> &truth, int dim, const std::string &point,
                       const std::string &mode, int ef, double factor, int connections,
                       int repeats, const std::filesystem::path &out, std::ofstream &summary) {
    const auto name = point + "-c" + std::to_string(connections);
    if (connections < 1 || connections > 256) throw std::runtime_error("invalid connection count");
    Configure(control, mode, ef, factor);
    CheckPlan(control, dim, queries[0], out / (name + ".explain.txt"));
    auto statement = control.Prepare(QuerySQL(dim));
    if (statement->HasError()) throw std::runtime_error(statement->GetError());
    auto first_start = Clock::now();
    Search(*statement, queries[0]);
    const double first_ms = Milliseconds(first_start);
    for (size_t q = 0; q < std::min<size_t>(16, queries.size()); ++q) Search(*statement, queries[q]);
    const uint64_t rss_before = ResidentBytes();

    std::vector<std::vector<ConcurrentSample>> samples(connections);
    std::vector<std::string> errors(connections);
    std::vector<std::thread> workers;
    std::mutex gate;
    std::condition_variable changed;
    int ready = 0;
    bool start = false, cancel = false;
    Clock::time_point wall_start;
    // Every connection executes every query once per pass, in independently
    // shuffled order. This keeps quality weights equal and sustains load at 64
    // clients, unlike partitioning just 600 global requests among all clients.
    for (int worker = 0; worker < connections; ++worker) {
        workers.emplace_back([&, worker] {
            unique_ptr<Connection> con;
            unique_ptr<PreparedStatement> prepared;
            std::vector<std::vector<size_t>> orders(repeats);
            try {
                con = make_uniq<Connection>(db);
                Configure(*con, mode, ef, factor);
                prepared = con->Prepare(QuerySQL(dim));
                if (prepared->HasError()) throw std::runtime_error(prepared->GetError());
                Search(*prepared, queries[worker % queries.size()]);
                samples[worker].reserve(queries.size() * repeats);
                for (int repeat = 0; repeat < repeats; ++repeat) {
                    auto &order = orders[repeat];
                    order.resize(queries.size());
                    std::iota(order.begin(), order.end(), 0);
                    std::mt19937 rng(20261027u + 1009u * repeat + 509u * worker);
                    std::shuffle(order.begin(), order.end(), rng);
                }
            } catch (const std::exception &e) {
                errors[worker] = e.what();
            }
            {
                std::unique_lock<std::mutex> lock(gate);
                ++ready;
                changed.notify_all();
                changed.wait(lock, [&] { return start; });
                if (cancel || !errors[worker].empty()) return;
            }
            try {
                for (int repeat = 0; repeat < repeats; ++repeat) {
                    for (auto q : orders[repeat]) {
                        const auto begin = Clock::now();
                        auto ids = Search(*prepared, queries[q]);
                        const auto end = Clock::now();
                        samples[worker].push_back({repeat, int(q), Nanoseconds(wall_start, begin),
                                                   Nanoseconds(wall_start, end), ids});
                    }
                }
            } catch (const std::exception &e) {
                errors[worker] = e.what();
            }
        });
    }
    rusage usage_before {}, usage_after {};
    {
        std::unique_lock<std::mutex> lock(gate);
        changed.wait(lock, [&] { return ready == connections; });
        for (const auto &error : errors) cancel |= !error.empty();
        getrusage(RUSAGE_SELF, &usage_before);
        wall_start = Clock::now();
        start = true;
    }
    changed.notify_all();
    for (auto &worker : workers) worker.join();
    const auto wall_end = Clock::now();
    getrusage(RUSAGE_SELF, &usage_after);
    const auto wall_ns = Nanoseconds(wall_start, wall_end);
    const double wall_ms = wall_ns / 1e6;
    const double cpu_ms = CPUTimeMS(usage_after) - CPUTimeMS(usage_before);

    std::ofstream raw(out / (name + ".csv"));
    std::ofstream worker_stats(out / (name + "-workers.csv"));
    raw << "repeat,worker,query,begin_ns,end_ns,ms,hits,ids\n" << std::setprecision(15);
    worker_stats << "worker,calls,first_begin_ns,last_end_ns\n";
    uint64_t hits = 0, count = 0;
    int64_t latest_begin = 0, first_finish = wall_ns;
    double occupied_ns = 0;
    std::vector<double> latencies;
    for (int worker = 0; worker < connections; ++worker) {
        const auto &group = samples[worker];
        const int64_t first = group.empty() ? 0 : group.front().begin_ns;
        const int64_t last = group.empty() ? 0 : group.back().end_ns;
        worker_stats << worker << ',' << group.size() << ',' << first << ',' << last << '\n';
        latest_begin = std::max(latest_begin, first);
        first_finish = std::min(first_finish, last);
        occupied_ns += last - first;
        for (const auto &sample : group) {
            int found = 0;
            for (auto id : sample.ids) {
                for (int k = 0; k < 10; ++k) {
                    if (id == truth[sample.query * 10 + k]) { ++found; break; }
                }
            }
            const auto ms = (sample.end_ns - sample.begin_ns) / 1e6;
            raw << sample.repeat << ',' << worker << ',' << sample.query << ',' << sample.begin_ns << ','
                << sample.end_ns << ',' << ms << ',' << found << ',';
            for (auto id : sample.ids) raw << id << ' ';
            raw << '\n';
            hits += found;
            ++count;
            latencies.push_back(ms);
        }
    }
    // Preserve partial call evidence before failing an incomplete point.
    for (const auto &error : errors) if (!error.empty()) throw std::runtime_error(error);
    if (count != queries.size() * repeats * connections) throw std::runtime_error("incomplete concurrent workload");
    if (first_finish <= latest_begin) throw std::runtime_error("workers did not overlap");
    const auto recall = double(hits) / (count * 10);
    summary << point << ',' << mode << ',' << ef << ',' << factor << ',' << connections << ',' << count << ','
            << recall << ',' << count * 1000.0 / wall_ms << ',' << Percentile(latencies, .5) << ','
            << Percentile(latencies, .95) << ',' << Percentile(latencies, .99) << ',' << first_ms << ','
            << rss_before << ',' << ResidentBytes() << ',' << wall_ns << ',' << cpu_ms << ','
            << cpu_ms / wall_ms << ',' << occupied_ns / wall_ns << ',' << first_finish - latest_begin << ','
            << usage_after.ru_majflt - usage_before.ru_majflt << ','
            << usage_after.ru_minflt - usage_before.ru_minflt << '\n' << std::flush;
    std::cout << "POINT " << name << " recall=" << recall << " p95_ms=" << Percentile(latencies, .95)
              << " p99_ms=" << Percentile(latencies, .99) << " qps=" << count * 1000.0 / wall_ms
              << " calls=" << count << " active_clients=" << occupied_ns / wall_ns << std::endl;
}

void CountPQ(Connection &con, const std::vector<float> &queries, size_t nq, size_t dim,
             const std::string &point, const std::string &mode, int ef, double factor, std::ofstream &out) {
    con.BeginTransaction();
    auto &table = Catalog::GetEntry(*con.context, CatalogType::TABLE_ENTRY, INVALID_CATALOG,
                                    DEFAULT_SCHEMA, "vectors").Cast<TableCatalogEntry>().Cast<DuckTableEntry>();
    auto &indexes = table.GetStorage().GetDataTableInfo()->GetIndexes();
    bool found = false;
    for (auto &index : indexes.Indexes()) {
        if (!index.IsBound() || index.GetIndexType() != GraphIndex::TYPE_NAME) continue;
        auto &graph = index.Cast<BoundIndex>().Cast<GraphIndex>();
        for (size_t q = 0; q < nq; ++q) {
            std::vector<row_t> ids;
            std::vector<float> distances;
            PQSearchMetrics metrics;
            graph.SearchPQ(queries.data() + q * dim, 10, ids, distances, factor, mode == "pq_graph" ? ef : 0, &metrics);
            out << point << ',' << q << ',' << metrics.codes_scored << ',' << metrics.raw_vectors_refined << ',';
            for (auto id : ids) out << id << ' ';
            out << '\n';
        }
        found = true;
    }
    con.Rollback();
    if (!found) throw std::runtime_error("bound graph index missing for native audit");
}
} // namespace

int main(int argc, char **argv) {
    if (argc == 5 && std::string(argv[1]) == "build") {
        // The existing build program uses one worker, product-default PQ M,
        // m=16/ef_construction=100. Its single-query probes are preparation
        // checks only and are excluded from concurrent performance results.
        std::vector<std::string> args {argv[0], argv[2], argv[3], "100000", "1536", "1", "pq", argv[4]};
        std::vector<char *> params;
        for (auto &arg : args) params.push_back(arg.data());
        return quantization_build_benchmark_main(int(params.size()), params.data());
    }
    if (argc != 11 || std::string(argv[1]) != "run") {
        std::cerr << "usage: benchmark build <extension> <data_dir> <new_fixture>\n"
                  << "       benchmark run <extension> <saved_db> <data_dir> <rows> <dim> <queries> <repeats> <points_file> <new_output>\n";
        return 2;
    }
    try {
        const auto extension = std::filesystem::absolute(argv[2]).string();
        const std::string database = argv[3], data = argv[4];
        const size_t n = std::stoull(argv[5]), dim = std::stoull(argv[6]), nq = std::stoull(argv[7]);
        const int repeats = std::stoi(argv[8]);
        const std::filesystem::path out = argv[10];
        if (!n || !nq || nq > 4096 || repeats < 1 || repeats > 100 || std::filesystem::exists(out)) {
            throw std::runtime_error("invalid sizes or output exists");
        }
        std::filesystem::create_directories(out);
        const auto floats = Read<float>(data + "/queries.f32", nq * dim);
        const auto truth = Read<int32_t>(data + "/truth-" + std::to_string(n) + ".i32", nq * 10);
        std::vector<Value> queries;
        for (size_t q = 0; q < nq; ++q) {
            vector<Value> values;
            for (size_t j = 0; j < dim; ++j) values.push_back(Value::FLOAT(floats[q * dim + j]));
            queries.push_back(Value::ARRAY(LogicalType::FLOAT, std::move(values)));
        }
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.SetOptionByName("access_mode", Value("read_only"));
        config.SetOptionByName("memory_limit", Value("4GB"));
        DuckDB db(database, &config);
        Connection con(db);
        Exec(con, "LOAD " + Quote(extension));
        auto info = Exec(con, "SELECT * FROM vexdb_index_info()");
        std::ofstream(out / "index-info.txt") << info->ToString();
        auto coverage = Exec(con, "SELECT node_count, row_id_map_size, dimension, use_pq FROM vexdb_index_info()");
        if (coverage->RowCount() != 1 || coverage->GetValue(1, 0).GetValue<int64_t>() != int64_t(n) ||
            coverage->GetValue(2, 0).GetValue<int32_t>() != int32_t(dim) || !coverage->GetValue(3, 0).GetValue<bool>()) {
            throw std::runtime_error("saved index coverage or layout mismatch");
        }
        std::ofstream summary(out / "points.csv"), counts(out / "counts.csv");
        summary << std::setprecision(15)
                << "point,mode,ef,factor,connections,calls,recall10,qps,p50_ms,p95_ms,p99_ms,first_ms,rss_before,rss_after,wall_ns,cpu_ms,cpu_cores,mean_active_clients,overlap_ns,major_faults,minor_faults\n";
        counts << "point,query,codes_scored,raw_vectors_refined,ids\n";
        std::ifstream input(argv[9]);
        std::set<std::string> audited, observed;
        std::string point, mode;
        int ef, connections;
        double factor;
        if (!input) throw std::runtime_error("cannot read point configuration");
        while (input >> point >> mode >> ef >> factor >> connections) {
            if (!observed.insert(point + "-c" + std::to_string(connections)).second) {
                throw std::runtime_error("duplicate point configuration");
            }
            std::cout << "START " << point << " c=" << connections << " epoch=" << std::time(nullptr) << std::endl;
            MeasureConcurrent(db, con, queries, truth, dim, point, mode, ef, factor, connections, repeats, out, summary);
            if (mode != "off" && audited.insert(point).second) CountPQ(con, floats, nq, dim, point, mode, ef, factor, counts);
        }
        std::cout << "COMPLETE saved-index concurrency benchmark\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAILED " << e.what() << '\n';
        return 1;
    }
}
