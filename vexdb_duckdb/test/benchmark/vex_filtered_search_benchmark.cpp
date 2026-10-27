// Separate from the frozen unfiltered experiments. Reuse only their native
// SDK I/O, loading and timing helpers; variable-length results are kept below.
#define main historical_product_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main

#include <condition_variable>
#include <map>
#include <mutex>
#include <set>

namespace {
std::string Predicate(const std::string &name) {
    if (name == "none") return "";
    if (name == "half") return "id % 2 = 0";
    if (name == "one-percent") return "id % 100 = 0";
    if (name == "tenth-percent") return "id % 1000 = 0";
    if (name == "hundredth-percent") return "id % 10000 = 0";
    if (name == "opposed-category") return "id >= 18000";
    throw std::runtime_error("unknown registered filter " + name);
}
std::string SQL(int dim, const std::string &predicate) {
    return "SELECT id FROM vectors" + (predicate.empty() ? std::string() : " WHERE " + predicate) +
           " ORDER BY l2_distance(vec, ?::FLOAT[" + std::to_string(dim) + "]) LIMIT 10";
}
std::vector<int32_t> FetchIDs(PreparedStatement &statement, const Value &query) {
    vector<Value> params {query};
    auto result = statement.Execute(params, false);
    if (!result || result->HasError()) throw std::runtime_error(result ? result->GetError() : "null result");
    auto &materialized = result->Cast<MaterializedQueryResult>();
    std::vector<int32_t> ids;
    std::set<int32_t> unique;
    for (idx_t row = 0; row < materialized.RowCount(); ++row) {
        const auto id = materialized.GetValue(0, row).GetValue<int32_t>();
        if (!unique.insert(id).second) throw std::runtime_error("duplicate returned ID");
        ids.push_back(id);
    }
    if (ids.size() > 10) throw std::runtime_error("LIMIT exceeded");
    return ids;
}
std::vector<Value> QueryValues(const std::string &data, size_t dim, size_t count) {
    auto floats = Read<float>(data + "/queries.f32", dim * count);
    std::vector<Value> values;
    for (size_t q = 0; q < count; ++q) {
        vector<Value> coordinates;
        for (size_t d = 0; d < dim; ++d) coordinates.push_back(Value::FLOAT(floats[q * dim + d]));
        values.push_back(Value::ARRAY(LogicalType::FLOAT, std::move(coordinates)));
    }
    return values;
}
struct Point {
    std::string name, mode, filter;
    int ef, connections, expected_scan;
    double factor;
    int64_t threshold;
};
void Settings(Connection &con, const Point &point) {
    Configure(con, point.mode, point.ef, point.factor);
    Exec(con, "SET vexdb_brute_force_threshold=" + std::to_string(point.threshold));
}
struct Request { size_t query; int64_t begin, end; std::vector<int32_t> ids; };
double CPUms(const rusage &r) {
    return 1000.0 * (r.ru_utime.tv_sec + r.ru_stime.tv_sec) +
           (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000.0;
}
int64_t NS(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}
void Measure(DuckDB &db, Connection &control, const std::vector<Value> &queries, int dim,
             const Point &point, const std::filesystem::path &out, bool quality,
             std::ofstream &summary) {
    Settings(control, point);
    const auto sql = SQL(dim, Predicate(point.filter));
    auto explain = control.Prepare("EXPLAIN " + sql);
    if (explain->HasError()) throw std::runtime_error(explain->GetError());
    vector<Value> params {queries[0]};
    auto explained = explain->Execute(params, false);
    if (explained->HasError()) throw std::runtime_error(explained->GetError());
    const auto plan = explained->ToString();
    const bool scan = plan.find("VEXDB_INDEX_SCAN") != std::string::npos;
    std::ofstream(out / (point.name + ".plan.txt")) << plan;
    if (point.expected_scan >= 0 && scan != bool(point.expected_scan))
        throw std::runtime_error("unexpected plan for " + point.name + ": " + plan);
    auto prepared = control.Prepare(sql);
    if (prepared->HasError()) throw std::runtime_error(prepared->GetError());
    if (quality) {
        std::ofstream raw(out / (point.name + ".quality.csv"));
        raw << "query,ids\n";
        for (size_t q = 0; q < queries.size(); ++q) {
            auto ids = FetchIDs(*prepared, queries[q]);
            raw << q << ',';
            for (auto id : ids) raw << id << ' ';
            raw << '\n';
        }
    }
    for (size_t q = 0; q < 8; ++q) FetchIDs(*prepared, queries[q]);
    const auto rss_before = ResidentBytes();
    std::vector<std::vector<Request>> samples(point.connections);
    std::vector<std::string> errors(point.connections);
    std::mutex mutex;
    std::condition_variable condition;
    int ready = 0;
    bool start = false, cancel = false;
    Clock::time_point wall_start;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < point.connections; ++worker) {
        workers.emplace_back([&, worker] {
            unique_ptr<Connection> con;
            unique_ptr<PreparedStatement> statement;
            std::vector<size_t> order(32);
            try {
                con = make_uniq<Connection>(db);
                Settings(*con, point);
                statement = con->Prepare(sql);
                if (statement->HasError()) throw std::runtime_error(statement->GetError());
                FetchIDs(*statement, queries[worker % queries.size()]);
                samples[worker].reserve(32);
                std::iota(order.begin(), order.end(), 0);
                std::mt19937 rng(20261027u + 509u * worker);
                std::shuffle(order.begin(), order.end(), rng);
            } catch (const std::exception &e) { errors[worker] = e.what(); }
            {
                std::unique_lock<std::mutex> lock(mutex);
                ++ready;
                condition.notify_all();
                condition.wait(lock, [&] { return start; });
                if (cancel) return;
            }
            try {
                for (auto q : order) {
                    const auto begin = Clock::now();
                    auto ids = FetchIDs(*statement, queries[q]);
                    const auto end = Clock::now();
                    samples[worker].push_back({q, NS(wall_start, begin), NS(wall_start, end), std::move(ids)});
                }
            } catch (const std::exception &e) { errors[worker] = e.what(); }
        });
    }
    rusage before {}, after {};
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return ready == point.connections; });
        for (auto &e : errors) cancel |= !e.empty();
        getrusage(RUSAGE_SELF, &before);
        wall_start = Clock::now();
        start = true;
    }
    condition.notify_all();
    for (auto &worker : workers) worker.join();
    const auto wall_end = Clock::now();
    getrusage(RUSAGE_SELF, &after);
    const auto wall_ns = NS(wall_start, wall_end);
    std::ofstream raw(out / (point.name + ".timing.csv"));
    raw << "worker,query,begin_ns,end_ns,ids\n";
    std::vector<double> ms;
    int64_t latest_start = 0, first_finish = wall_ns;
    double occupied = 0;
    size_t returned = 0;
    for (int w = 0; w < point.connections; ++w) {
        const auto &group = samples[w];
        if (!group.empty()) {
            latest_start = std::max(latest_start, group.front().begin);
            first_finish = std::min(first_finish, group.back().end);
            occupied += group.back().end - group.front().begin;
        }
        for (auto &r : group) {
            raw << w << ',' << r.query << ',' << r.begin << ',' << r.end << ',';
            for (auto id : r.ids) raw << id << ' ';
            raw << '\n';
            returned += r.ids.size();
            ms.push_back((r.end - r.begin) / 1e6);
        }
    }
    // Preserve partial calls and plans even when a point fails.
    for (auto &e : errors) if (!e.empty()) throw std::runtime_error(e);
    if (ms.size() != 32 * size_t(point.connections)) throw std::runtime_error("incomplete point");
    const double wall_ms = wall_ns / 1e6, cpu_ms = CPUms(after) - CPUms(before);
    summary << point.name << ',' << point.mode << ',' << point.filter << ',' << point.connections << ','
            << scan << ',' << ms.size() << ',' << returned << ',' << ms.size() * 1000.0 / wall_ms << ','
            << Percentile(ms, .5) << ',' << Percentile(ms, .95) << ',' << Percentile(ms, .99) << ','
            << wall_ns << ',' << cpu_ms << ',' << cpu_ms / wall_ms << ',' << occupied / wall_ns << ','
            << first_finish - latest_start << ',' << rss_before << ',' << ResidentBytes() << '\n' << std::flush;
    std::cout << "POINT " << point.name << " qps=" << ms.size() * 1000.0 / wall_ms
              << " p95=" << Percentile(ms, .95) << " returned=" << returned << std::endl;
}
} // namespace

int main(int argc, char **argv) {
    // build <extension> <data> <rows> <dim> <new_output>
    if (argc == 7 && std::string(argv[1]) == "build") {
        const std::filesystem::path out = argv[6];
        if (std::filesystem::exists(out)) return 2;
        std::filesystem::create_directories(out);
        try {
            const auto n = std::stoull(argv[4]), dim = std::stoull(argv[5]);
            DBConfig config; config.SetOptionByName("allow_unsigned_extensions", true);
            DuckDB db((out / "vectors.duckdb").string(), &config);
            Connection con(db);
            Exec(con, "LOAD " + Quote(std::filesystem::absolute(argv[2]).string()));
            Exec(con, "SET threads=1");
            Exec(con, "CREATE TABLE vectors(id INTEGER, vec FLOAT[" + std::to_string(dim) + "])");
            auto data = Read<float>(std::string(argv[3]) + "/base.f32", n * dim);
            {
                Appender app(con, "vectors");
                for (size_t row = 0; row < n; ++row) {
                    vector<Value> values;
                    for (size_t d = 0; d < dim; ++d) values.push_back(Value::FLOAT(data[row * dim + d]));
                    app.BeginRow(); app.Append(int32_t(row));
                    app.Append(Value::ARRAY(LogicalType::FLOAT, std::move(values))); app.EndRow();
                }
                app.Close();
            }
            Exec(con, "CREATE INDEX vector_idx ON vectors USING GRAPH_INDEX(vec) WITH "
                      "(metric='l2',quantizer='pq',m=16,ef_construction=100,threads=1)");
            Exec(con, "CHECKPOINT");
            std::ofstream(out / "index-info.txt") << Exec(con, "SELECT * FROM vexdb_index_info()")->ToString();
            std::cout << "COMPLETE fixture\n";
            return 0;
        } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
    }
    // run <extension> <read_only_database> <data> <dim> <quality_queries> <points> <new_output>
    if (argc != 9 || std::string(argv[1]) != "run") return 2;
    const std::filesystem::path out = argv[8];
    if (std::filesystem::exists(out)) return 2;
    std::filesystem::create_directories(out);
    try {
        const int dim = std::stoi(argv[5]), nq = std::stoi(argv[6]);
        if (nq < 32 || nq > 200) throw std::runtime_error("quality query bound");
        auto queries = QueryValues(argv[4], dim, nq);
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.SetOptionByName("access_mode", Value("read_only"));
        config.SetOptionByName("memory_limit", Value("4GB"));
        DuckDB db(argv[3], &config);
        Connection con(db);
        Exec(con, "LOAD " + Quote(std::filesystem::absolute(argv[2]).string()));
        std::ofstream(out / "index-info.txt") << Exec(con, "SELECT * FROM vexdb_index_info()")->ToString();
        std::ofstream summary(out / "points.csv");
        summary << std::setprecision(15) << "point,mode,filter,connections,index_scan,calls,returned,qps,p50_ms,p95_ms,p99_ms,wall_ns,cpu_ms,cpu_cores,mean_active_clients,overlap_ns,rss_before,rss_after\n";
        std::ifstream points(argv[7]);
        if (!points) throw std::runtime_error("missing points file");
        Point point;
        std::set<std::string> quality, names;
        while (points >> point.name >> point.mode >> point.ef >> point.factor >> point.threshold >> point.filter >> point.connections >> point.expected_scan) {
            if (point.connections < 1 || point.connections > 64 || !names.insert(point.name).second)
                throw std::runtime_error("invalid/duplicate point");
            std::cout << "START " << point.name << std::endl;
            const auto key = point.mode + ":" + std::to_string(point.threshold) + ":" + point.filter;
            Measure(db, con, queries, dim, point, out, quality.insert(key).second, summary);
        }
        std::cout << "COMPLETE filtered workload\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAILED " << e.what() << '\n'; return 1; }
}
