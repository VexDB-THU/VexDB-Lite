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
} // namespace
#include <ctime>
#include <deque>
#ifdef __APPLE__
#include <malloc/malloc.h>
#endif

namespace {
uint64_t HeapLiveBytes() {
#ifdef __APPLE__
    malloc_statistics_t stats {};
    malloc_zone_statistics(nullptr, &stats);
    return stats.size_in_use;
#else
    return 0;
#endif
}
int64_t ThreadCPU() {
    timespec value {};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value)) throw std::runtime_error("thread CPU clock unavailable");
    return value.tv_sec * int64_t(1000000000) + value.tv_nsec;
}
struct CPUSample { size_t query; int64_t begin, end, cpu; std::vector<int32_t> ids; };


struct GateSample { size_t query; int64_t begin,end,cpu; std::vector<int32_t> ids; int64_t execute_begin; };
// Diagnostic only: keep64 sessions/threads; FIFO handoff limits simultaneous
// native executions. Waiting is inside the submission-to-result clock.
class FIFOGate {
    struct Waiter { std::condition_variable cv; bool allowed=false; };
    std::mutex mutex;std::deque<Waiter *> pending;int active=0,limit;bool aborted=false;
public:
    explicit FIFOGate(int n):limit(n){}
    void Acquire() {
        std::unique_lock<std::mutex> lock(mutex);
        if(aborted)throw std::runtime_error("gate cancelled");
        if(active<limit && pending.empty()){++active;return;}
        Waiter waiter;pending.push_back(&waiter);
        waiter.cv.wait(lock,[&]{return waiter.allowed || aborted;});
        if(aborted)throw std::runtime_error("gate cancelled");
    }
    void Release() {
        std::lock_guard<std::mutex> lock(mutex);
        if(!pending.empty() && !aborted){auto *next=pending.front();pending.pop_front();next->allowed=true;next->cv.notify_one();}
        else --active;
    }
    void Abort(){std::lock_guard<std::mutex> lock(mutex);aborted=true;for(auto *w:pending)w->cv.notify_one();pending.clear();}
    struct Permit { FIFOGate &gate; explicit Permit(FIFOGate &g):gate(g){gate.Acquire();} ~Permit(){gate.Release();} };
};
void Trace(DuckDB &db, Connection &control, const std::vector<Value> &queries, int dim,
           const Point &point, int requests, int limit, const std::filesystem::path &out, std::ofstream &summary) {
    FIFOGate gate(limit);
    Settings(control, point);
    const auto sql = SQL(dim, Predicate(point.filter));
    auto explained = control.Prepare("EXPLAIN " + sql);
    vector<Value> params {queries[0]};
    auto plan = explained->Execute(params, false);
    if (plan->HasError()) throw std::runtime_error(plan->GetError());
    std::ofstream(out / (point.name + ".plan.txt")) << plan->ToString();
    const bool scan = plan->ToString().find("VEXDB_INDEX_SCAN") != std::string::npos;
    if (point.expected_scan >= 0 && scan != bool(point.expected_scan)) throw std::runtime_error("unexpected plan");
    std::vector<std::vector<GateSample>> samples(point.connections);
    std::vector<std::string> errors(point.connections);
    std::mutex mutex;
    std::condition_variable condition;
    int ready = 0;
    bool start = false, cancel = false;
    Clock::time_point wall_start;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < point.connections; ++worker) workers.emplace_back([&, worker] {
        unique_ptr<Connection> con;
        unique_ptr<PreparedStatement> statement;
        std::vector<size_t> order(queries.size());
        try {
            con = make_uniq<Connection>(db);
            Settings(*con, point);
            statement = con->Prepare(sql);
            if (statement->HasError()) throw std::runtime_error(statement->GetError());
            FetchIDs(*statement, queries[worker % queries.size()]);
            samples[worker].reserve(requests);
            std::iota(order.begin(), order.end(), 0);
            std::mt19937 rng(20261028u + 509u * worker);
            std::shuffle(order.begin(), order.end(), rng);
        } catch (const std::exception &e) { errors[worker] = e.what(); gate.Abort(); }
        {
            std::unique_lock<std::mutex> lock(mutex);
            ++ready;
            condition.notify_all();
            condition.wait(lock, [&] { return start; });
            if (cancel) return;
        }
        try {
            for (int i = 0; i < requests; ++i) {
                const auto q = order[i % order.size()];
                const auto begin = i == 0 ? wall_start : Clock::now();
                int64_t cpu, execute_begin; Clock::time_point end; std::vector<int32_t> ids;
                {
                    FIFOGate::Permit permit(gate);
                    execute_begin = NS(wall_start, Clock::now());
                    const auto cpu_begin = ThreadCPU();
                    ids = FetchIDs(*statement, queries[q]);
                    cpu = ThreadCPU() - cpu_begin;
                    end = Clock::now();
                }
                samples[worker].push_back({q, NS(wall_start, begin), NS(wall_start, end), cpu, std::move(ids), execute_begin});
            }
        } catch (const std::exception &e) { errors[worker] = e.what(); gate.Abort(); }
    });
    rusage before {}, after {};
    const auto rss_before = ResidentBytes();
    const auto heap_before = HeapLiveBytes();
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return ready == point.connections; });
        for (auto &e : errors) cancel |= !e.empty();
        getrusage(RUSAGE_SELF, &before);
        wall_start = Clock::now();
        start = true;
        std::cout << "TIMING " << point.name << std::endl;
    }
    condition.notify_all();
    for (auto &w : workers) w.join();
    const auto wall_ns = NS(wall_start, Clock::now());
    getrusage(RUSAGE_SELF, &after);
    std::ofstream raw(out / (point.name + ".timing.csv"));
    raw << "worker,ordinal,query,begin_ns,end_ns,thread_cpu_ns,ids,execute_start_ns,executor\n";
    std::vector<double> ms, cpu;
    int64_t latest_start = 0, first_finish = wall_ns;
    double occupied = 0;
    for (int worker = 0; worker < point.connections; ++worker) {
        const auto &group = samples[worker];
        if (!group.empty()) {
            latest_start = std::max(latest_start, group.front().begin);
            first_finish = std::min(first_finish, group.back().end);
            occupied += group.back().end - group.front().begin;
        }
        for (size_t i = 0; i < group.size(); ++i) {
            auto &r = group[i];
            raw << worker << ',' << i << ',' << r.query << ',' << r.begin << ',' << r.end << ',' << r.cpu << ',';
            for (auto id : r.ids) raw << id << ' ';
            raw << ',' << r.execute_begin << ',' << worker << '\n';
            ms.push_back((r.end-r.begin)/1e6);
            cpu.push_back(r.cpu/1e6);
        }
    }
    for (auto &e : errors) if (!e.empty()) throw std::runtime_error(e);
    if (ms.size() != size_t(requests*point.connections)) throw std::runtime_error("missing request");
    const auto cpu_ms = CPUms(after)-CPUms(before);
    summary << point.name << ',' << ms.size() << ',' << wall_ns << ',' << ms.size()*1e9/wall_ns << ','
            << Percentile(ms,.5) << ',' << Percentile(ms,.95) << ',' << Percentile(ms,.99) << ','
            << Percentile(cpu,.5) << ',' << Percentile(cpu,.95) << ',' << cpu_ms << ','
            << cpu_ms*1e6/wall_ns << ',' << occupied/wall_ns << ',' << first_finish-latest_start << ','
            << after.ru_minflt-before.ru_minflt << ',' << after.ru_majflt-before.ru_majflt << ','
            << after.ru_nvcsw-before.ru_nvcsw << ',' << after.ru_nivcsw-before.ru_nivcsw << ','
            << rss_before << ',' << ResidentBytes() << ',' << heap_before << ',' << HeapLiveBytes() << ',' << point.connections << ',' << limit << '\n' << std::flush;
    std::cout << "DONE " << point.name << " qps=" << ms.size()*1e9/wall_ns
              << " p95=" << Percentile(ms,.95) << " cpu_p95=" << Percentile(cpu,.95) << std::endl;
}
}

int main(int argc, char **argv) {
    if (argc != 7 || std::filesystem::exists(argv[6])) return 2;
    try {
        const int dim = std::stoi(argv[4]);
        auto queries = QueryValues(argv[3], dim, 512);
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.SetOptionByName("access_mode", Value("read_only"));
        config.SetOptionByName("memory_limit", Value("4GB"));
        DuckDB db(argv[2], &config);
        Connection con(db);
        Exec(con, "LOAD " + Quote(std::filesystem::absolute(argv[1]).string()));
        Exec(con, "SELECT * FROM vexdb_index_info()");
        const std::filesystem::path out = argv[6];
        std::filesystem::create_directories(out);
        std::ofstream summary(out/"points.csv");
        summary << std::setprecision(15) << "point,calls,wall_ns,qps,p50_ms,p95_ms,p99_ms,cpu_p50_ms,cpu_p95_ms,process_cpu_ms,cpu_cores,mean_active_clients,overlap_ns,minor_faults,major_faults,voluntary_switches,involuntary_switches,rss_before,rss_after,heap_live_before,heap_live_after,executors,execution_limit\n";
        std::ifstream points(argv[5]);
        if (!points) throw std::runtime_error("points missing");
        Point p;
        int requests, limit;
        while (points >> p.name >> p.mode >> p.ef >> p.factor >> p.threshold >> p.filter >> p.connections >> p.expected_scan >> requests >> limit) {
            if (p.connections!=64 || requests!=512 || limit<1 || limit>64) throw std::runtime_error("invalid load");
            Trace(db,con,queries,dim,p,requests,limit,out,summary);
        }
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAILED " << e.what() << '\n'; return 1; }
}
