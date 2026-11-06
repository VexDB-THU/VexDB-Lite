// Diagnostic variant; unprofiled product gates remain separate.
// Native prepared business SQL. Timed windows end only after each client has
// completed its full, identical query cycle. No external admission scheduler.
#define main historical_product_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main
#include <condition_variable>
#include <mutex>
#include <set>
#include <ctime>

namespace {
struct Point {
    std::string name, filter_mode, pq_mode, profile;
    int ef, clients, seconds;
    double factor;
    int cap;
};
int64_t Ns(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count();
}
int64_t ThreadCpu() {
    timespec t {};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) throw std::runtime_error("thread CPU unavailable");
    return t.tv_sec * int64_t(1000000000) + t.tv_nsec;
}
double CpuMs(const rusage &r) { return (r.ru_utime.tv_sec + r.ru_stime.tv_sec) * 1000.0 + (r.ru_utime.tv_usec + r.ru_stime.tv_usec) / 1000.0; }
void Settings(Connection &con, const Point &p, bool bidirectional) {
    Configure(con, p.pq_mode, p.ef, p.factor);
    if (p.filter_mode != "legacy") Exec(con, "SET vexdb_filtered_search_mode=" + Quote(p.filter_mode));
    if (bidirectional) Exec(con, "SET vexdb_graph_bidirectional_search=true");
}
void CreateBusinessView(Connection &con, bool stored) {
    if (stored) {
        Exec(con, "CREATE TEMP VIEW business AS SELECT id,vec,tenant_id,is_public,is_active,in_stock,price_cents,body FROM vectors");
        return;
    }
    Exec(con, "CREATE TEMP VIEW business AS SELECT id,vec,(id%100)::INTEGER tenant_id,id%7<>0 is_public,"
              "id%2=0 is_active,id%5<>0 in_stock,(100+id%10000)::INTEGER price_cents,"
              "repeat('proxy-payload-'||id::VARCHAR||' ',32) body FROM vectors");
}
std::string BusinessSql(int dim, const std::string &profile) {
    std::string predicate;
    if (profile == "broad-acl") predicate = "is_active=$2";
    else if (profile == "tenant-acl") predicate = "tenant_id=$2 AND is_public";
    else if (profile == "catalog-available-price") predicate = "in_stock AND price_cents BETWEEN $2 AND $3";
    else throw std::runtime_error("unknown registered business profile");
    return "SELECT id,body FROM business WHERE " + predicate +
        " ORDER BY l2_distance(vec,$1::FLOAT[" + std::to_string(dim) + "]) LIMIT 10";
}
struct ResultIDs { std::vector<int32_t> ids; size_t payload_bytes = 0; int64_t execute_ns=0, read_ns=0; };
ResultIDs SearchBusiness(PreparedStatement &statement, const Value &query, size_t q, const std::string &profile, bool verify_body = false) {
    const auto request_start=Clock::now();
    vector<Value> params {query};
    if (profile == "broad-acl") params.push_back(Value::BOOLEAN(true));
    else if (profile == "tenant-acl") params.push_back(Value::INTEGER(int(q % 100)));
    else { params.push_back(Value::INTEGER(1000 + int(q % 5) * 100)); params.push_back(Value::INTEGER(9000)); }
    auto result = statement.Execute(params, false);
    if (!result || result->HasError()) throw std::runtime_error(result ? result->GetError() : "null result");
    const auto execute_end=Clock::now();
    auto &mat = result->Cast<MaterializedQueryResult>();
    ResultIDs output;
    std::set<int32_t> unique;
    for (idx_t r = 0; r < mat.RowCount(); ++r) {
        const auto id = mat.GetValue(0, r).GetValue<int32_t>();
        if (!unique.insert(id).second) throw std::runtime_error("duplicate business result");
        const auto body = mat.GetValue(1, r).GetValue<std::string>();
        if (body.empty()) throw std::runtime_error("missing result body");
        if (verify_body) {
            const auto part = "proxy-payload-" + std::to_string(id) + " ";
            std::string expected;
            for (int i = 0; i < 32; ++i) expected += part;
            if (body != expected) throw std::runtime_error("fixture payload does not match returned ID");
        }
        output.payload_bytes += body.size();
        output.ids.push_back(id);
    }
    if (output.ids.size() != 10) throw std::runtime_error("unexpected business result count");
    output.execute_ns=Ns(request_start,execute_end);
    output.read_ns=Ns(execute_end,Clock::now());
    return output;
}
struct BusinessSample { size_t q; int64_t begin, end, cpu; ResultIDs result; };
void MeasureBusiness(DuckDB &db, Connection &admin, const std::vector<Value> &queries, int dim,
                     const Point &point, const std::filesystem::path &out, std::ofstream &summary, bool stored, bool bidirectional) {
    Settings(admin, point, bidirectional);
    Exec(admin, "SET GLOBAL vexdb_search_concurrency=" + std::to_string(point.cap));
    const auto sql = BusinessSql(dim, point.profile);
    std::ofstream(out / (point.name + ".sql")) << sql << ";\n";
    auto explained = admin.Prepare("EXPLAIN " + sql);
    vector<Value> params {queries[0]};
    if (point.profile == "broad-acl") params.push_back(Value::BOOLEAN(true));
    else if (point.profile == "tenant-acl") params.push_back(Value::INTEGER(0));
    else { params.push_back(Value::INTEGER(1000)); params.push_back(Value::INTEGER(9000)); }
    auto plan = explained->Execute(params, false);
    if (plan->HasError()) throw std::runtime_error(plan->GetError());
    std::ofstream(out / (point.name + ".plan.txt")) << plan->ToString();
    const bool indexed = plan->ToString().find("VEXDB_INDEX_SCAN") != std::string::npos;
    // auto may deliberately preserve a cheaper native scalar/ART plan. Keep
    // that decision visible and require exact/legacy controls to remain exact.
    std::ofstream(out / (point.name + ".route.txt")) << (indexed ? (bidirectional ? "filtered-index-bidirectional" : "filtered-index") : "native-scalar") << '\n';
    if (indexed && point.filter_mode != "auto") throw std::runtime_error("unexpected business path");
    auto body_check = admin.Prepare(sql);
    if (body_check->HasError()) throw std::runtime_error(body_check->GetError());
    for (size_t q = 0; q < queries.size(); ++q) SearchBusiness(*body_check, queries[q], q, point.profile, true);
    std::vector<std::vector<BusinessSample>> samples(point.clients);
    std::vector<std::string> errors(point.clients);
    std::mutex mutex;
    std::condition_variable condition;
    int ready = 0;
    bool start = false, abort = false;
    Clock::time_point started;
    std::vector<std::thread> workers;
    for (int client = 0; client < point.clients; ++client) workers.emplace_back([&, client] {
        unique_ptr<Connection> con;
        unique_ptr<PreparedStatement> statement;
        std::vector<size_t> order(queries.size());
        std::iota(order.begin(), order.end(), 0);
        std::mt19937 rng(20261029u + uint32_t(client));
        std::shuffle(order.begin(), order.end(), rng);
        try {
            con = make_uniq<Connection>(db);
            CreateBusinessView(*con, stored);
            Settings(*con, point, bidirectional);
            statement = con->Prepare(sql);
            if (statement->HasError()) throw std::runtime_error(statement->GetError());
            SearchBusiness(*statement, queries[client % queries.size()], client % queries.size(), point.profile);
            samples[client].reserve(2048);
        } catch (const std::exception &e) { errors[client] = e.what(); }
        {
            std::unique_lock<std::mutex> lock(mutex);
            ++ready;
            condition.notify_all();
            condition.wait(lock, [&] { return start; });
            if (abort) return;
        }
        try {
            do {
                for (auto q : order) {
                    const auto begin = Clock::now();
                    const auto cpu = ThreadCpu();
                    auto result = SearchBusiness(*statement, queries[q], q, point.profile);
                    const auto used = ThreadCpu() - cpu;
                    const auto end = Clock::now();
                    samples[client].push_back({q, Ns(started, begin), Ns(started, end), used, std::move(result)});
                }
                std::shuffle(order.begin(), order.end(), rng);
            } while (Clock::now() - started < std::chrono::seconds(point.seconds));
        } catch (const std::exception &e) { errors[client] = e.what(); }
    });
    rusage before {}, after {};
    const auto rss_before = ResidentBytes();
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return ready == point.clients; });
        for (auto &error : errors) abort |= !error.empty();
        getrusage(RUSAGE_SELF, &before);
        started = Clock::now();
        start = true;
        std::cout << "TIMING " << point.name << std::endl;
    }
    condition.notify_all();
    for (auto &worker : workers) worker.join();
    const auto wall = Ns(started, Clock::now());
    getrusage(RUSAGE_SELF, &after);
    std::ofstream raw(out / (point.name + ".timing.csv"));
    raw << "client,ordinal,query,begin_ns,end_ns,thread_cpu_ns,payload_bytes,execute_ns,read_ns,ids\n";
    std::vector<double> latency;
    double occupied = 0;
    int64_t overlap_begin = 0, overlap_end = wall;
    for (int client = 0; client < point.clients; ++client) {
        const auto &group = samples[client];
        if (!group.empty()) {
            occupied += group.back().end - group.front().begin;
            overlap_begin = std::max(overlap_begin, group.front().begin);
            overlap_end = std::min(overlap_end, group.back().end);
        }
        for (size_t n = 0; n < group.size(); ++n) {
            const auto &s = group[n];
            raw << client << ',' << n << ',' << s.q << ',' << s.begin << ',' << s.end << ',' << s.cpu << ',' << s.result.payload_bytes << ',' << s.result.execute_ns << ',' << s.result.read_ns << ',';
            for (auto id : s.result.ids) raw << id << ' ';
            raw << '\n';
            latency.push_back((s.end - s.begin) / 1e6);
        }
    }
    for (auto &error : errors) if (!error.empty()) throw std::runtime_error(error);
    if (latency.empty()) throw std::runtime_error("missing timing");
    summary << point.name << ',' << latency.size() << ',' << wall << ',' << latency.size() * 1e9 / wall << ','
            << Percentile(latency, .5) << ',' << Percentile(latency, .95) << ',' << Percentile(latency, .99) << ','
            << CpuMs(after) - CpuMs(before) << ',' << occupied / wall << ',' << overlap_end - overlap_begin << ','
            << rss_before << ',' << ResidentBytes() << ',' << after.ru_minflt-before.ru_minflt << ',' << after.ru_majflt-before.ru_majflt << ',' << after.ru_nvcsw-before.ru_nvcsw << ',' << after.ru_nivcsw-before.ru_nivcsw << '\n' << std::flush;
    std::cout << "DONE " << point.name << " calls=" << latency.size() << " qps=" << latency.size() * 1e9 / wall << std::endl;
    std::ofstream(out / (point.name + ".index-info.txt")) << Exec(admin, "SELECT * FROM vexdb_index_info()")->ToString();
}
}

int main(int argc, char **argv) {
    if (argc < 7 || argc > 9 || std::filesystem::exists(argv[6])) return 2;
    try {
        const bool stored = argc >= 8 && std::string(argv[7]) == "stored";
        if (argc >= 8 && !stored) throw std::runtime_error("metadata source must be stored");
        const bool bidirectional = argc == 9 && std::string(argv[8]) == "bidirectional";
        if (argc == 9 && !bidirectional) throw std::runtime_error("invalid graph direction");
        const int dim = std::stoi(argv[4]);
        auto input = Read<float>(std::string(argv[3]) + "/queries.f32", size_t(dim) * 8);
        std::vector<Value> queries;
        for (int q = 0; q < 8; ++q) {
            vector<Value> values;
            for (int d = 0; d < dim; ++d) values.push_back(Value::FLOAT(input[q * size_t(dim) + d]));
            queries.push_back(Value::ARRAY(LogicalType::FLOAT, std::move(values)));
        }
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions", true);
        config.SetOptionByName("access_mode", Value("read_only"));
        config.SetOptionByName("memory_limit", Value("4GB"));
        DuckDB db(argv[2], &config);
        Connection admin(db);
        Exec(admin, "LOAD " + Quote(std::filesystem::absolute(argv[1]).string()));
        Exec(admin, "SELECT * FROM vexdb_index_info()");
        CreateBusinessView(admin, stored);
        const std::filesystem::path out = argv[6];
        std::filesystem::create_directories(out);
        std::ofstream summary(out / "points.csv");
        summary << std::setprecision(15) << "point,calls,wall_ns,qps,p50_ms,p95_ms,p99_ms,process_cpu_ms,mean_active_clients,overlap_ns,rss_before,rss_after,minor_faults,major_faults,voluntary_switches,involuntary_switches\n";
        std::ifstream points(argv[5]);
        if (!points) throw std::runtime_error("point file missing");
        Point p;
        while (points >> p.name >> p.filter_mode >> p.pq_mode >> p.ef >> p.factor >> p.clients >> p.seconds >> p.profile >> p.cap) {
            if (p.clients < 1 || p.clients > 256 || p.seconds < 1 || p.seconds > 60) throw std::runtime_error("invalid load");
            MeasureBusiness(db, admin, queries, dim, p, out, summary, stored, bidirectional);
        }
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
