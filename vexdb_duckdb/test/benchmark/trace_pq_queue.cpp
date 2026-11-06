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


struct QueuedRequest { int client, ordinal; size_t query; int64_t submitted; };
struct QueueSample { int executor, ordinal; size_t query; int64_t submitted, began, ended, cpu; std::vector<int32_t> ids; };
void QueueTrace(DuckDB &db, Connection &control, const std::vector<Value> &queries, int dim,
                const Point &point, int requests, int executors, const std::filesystem::path &out,
                std::ofstream &summary) {
    Settings(control, point);
    const auto sql=SQL(dim, "");
    auto explain=control.Prepare("EXPLAIN "+sql);
    vector<Value> args {queries[0]};
    auto plan=explain->Execute(args,false);
    if (plan->HasError() || plan->ToString().find("VEXDB_INDEX_SCAN")==std::string::npos)
        throw std::runtime_error("queue diagnostic requires index scan");
    std::ofstream(out/(point.name+".plan.txt"))<<plan->ToString();
    std::vector<std::vector<size_t>> orders(point.connections);
    std::vector<std::vector<QueueSample>> samples(point.connections);
    for (int c=0;c<point.connections;++c) {
        orders[c].resize(queries.size());std::iota(orders[c].begin(),orders[c].end(),0);
        std::mt19937 random(20261028u+509u*c);std::shuffle(orders[c].begin(),orders[c].end(),random);
        samples[c].reserve(requests);
    }
    std::deque<QueuedRequest> pending;
    for (int c=0;c<point.connections;++c) pending.push_back({c,0,orders[c][0],0});
    std::mutex mutex;std::condition_variable condition;
    int ready=0,finished=0;bool start=false;std::exception_ptr failure;
    Clock::time_point origin;
    std::vector<std::thread> workers;
    for (int w=0;w<executors;++w) workers.emplace_back([&,w] {
        unique_ptr<Connection> connection;unique_ptr<PreparedStatement> statement;
        try {
            connection=make_uniq<Connection>(db);Settings(*connection,point);
            statement=connection->Prepare(sql);
            if(statement->HasError())throw std::runtime_error(statement->GetError());
            FetchIDs(*statement,queries[w%queries.size()]);
        } catch(...) {std::lock_guard<std::mutex> lock(mutex);if(!failure)failure=std::current_exception();}
        {
            std::unique_lock<std::mutex> lock(mutex);++ready;condition.notify_all();
            condition.wait(lock,[&]{return start;});
            if(failure)return;
        }
        try {
            while(true) {
                QueuedRequest task;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    condition.wait(lock,[&]{return failure || !pending.empty() || finished==point.connections;});
                    if(failure || finished==point.connections)return;
                    task=pending.front();pending.pop_front();
                }
                const auto began=NS(origin,Clock::now());const auto cpu_start=ThreadCPU();
                auto ids=FetchIDs(*statement,queries[task.query]);
                const auto cpu=ThreadCPU()-cpu_start;const auto ended=NS(origin,Clock::now());
                bool all_done=false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    samples[task.client].push_back({w,task.ordinal,task.query,task.submitted,began,ended,cpu,std::move(ids)});
                    if(task.ordinal+1<requests) {
                        const int next=task.ordinal+1;
                        pending.push_back({task.client,next,orders[task.client][next%queries.size()],NS(origin,Clock::now())});
                    } else ++finished;
                    all_done=finished==point.connections;
                }
                if(all_done)condition.notify_all();else condition.notify_one();
            }
        } catch(...) {std::lock_guard<std::mutex> lock(mutex);if(!failure)failure=std::current_exception();condition.notify_all();}
    });
    rusage before{},after{};const auto rss_before=ResidentBytes(),heap_before=HeapLiveBytes();
    {
        std::unique_lock<std::mutex> lock(mutex);condition.wait(lock,[&]{return ready==executors;});
        getrusage(RUSAGE_SELF,&before);origin=Clock::now();start=true;
        std::cout<<"TIMING "<<point.name<<std::endl;
    }
    condition.notify_all();
    for(auto &worker:workers)worker.join();
    const auto wall=NS(origin,Clock::now());getrusage(RUSAGE_SELF,&after);
    if(failure)std::rethrow_exception(failure);
    std::ofstream raw(out/(point.name+".timing.csv"));
    raw<<"worker,ordinal,query,begin_ns,end_ns,thread_cpu_ns,ids,execute_start_ns,executor\n";
    std::vector<double> latencies,cpus,waits;double occupied=0;int64_t latest_start=0,first_finish=wall;
    for(int c=0;c<point.connections;++c) {
        if(samples[c].size()!=size_t(requests))throw std::runtime_error("missing admitted request");
        occupied+=samples[c].back().ended-samples[c].front().submitted;
        latest_start=std::max(latest_start,samples[c].front().submitted);first_finish=std::min(first_finish,samples[c].back().ended);
        for(auto &s:samples[c]) {
            raw<<c<<','<<s.ordinal<<','<<s.query<<','<<s.submitted<<','<<s.ended<<','<<s.cpu<<',';
            for(auto id:s.ids)raw<<id<<' ';
            raw<<','<<s.began<<','<<s.executor<<'\n';
            latencies.push_back((s.ended-s.submitted)/1e6);cpus.push_back(s.cpu/1e6);waits.push_back((s.began-s.submitted)/1e6);
        }
    }
    const auto cpu_ms=CPUms(after)-CPUms(before);
    summary<<point.name<<','<<latencies.size()<<','<<wall<<','<<latencies.size()*1e9/wall<<','
      <<Percentile(latencies,.5)<<','<<Percentile(latencies,.95)<<','<<Percentile(latencies,.99)<<','
      <<Percentile(cpus,.5)<<','<<Percentile(cpus,.95)<<','<<cpu_ms<<','<<cpu_ms*1e6/wall<<','
      <<occupied/wall<<','<<first_finish-latest_start<<','<<after.ru_minflt-before.ru_minflt<<','
      <<after.ru_majflt-before.ru_majflt<<','<<after.ru_nvcsw-before.ru_nvcsw<<','<<after.ru_nivcsw-before.ru_nivcsw<<','
      <<rss_before<<','<<ResidentBytes()<<','<<heap_before<<','<<HeapLiveBytes()<<','<<executors<<','<<Percentile(waits,.95)<<'\n'<<std::flush;
    std::cout<<"DONE "<<point.name<<" qps="<<latencies.size()*1e9/wall<<" p95_including_queue="<<Percentile(latencies,.95)<<std::endl;
}
}
int main(int argc,char **argv) {
    if(argc!=7 || std::filesystem::exists(argv[6]))return 2;
    try {
        const int dim=std::stoi(argv[4]);auto queries=QueryValues(argv[3],dim,512);
        DBConfig config;config.SetOptionByName("allow_unsigned_extensions",true);config.SetOptionByName("access_mode",Value("read_only"));config.SetOptionByName("memory_limit",Value("4GB"));
        DuckDB db(argv[2],&config);Connection con(db);Exec(con,"LOAD "+Quote(std::filesystem::absolute(argv[1]).string()));Exec(con,"SELECT * FROM vexdb_index_info()");
        const std::filesystem::path out=argv[6];std::filesystem::create_directories(out);std::ofstream summary(out/"points.csv");
        summary<<std::setprecision(15)<<"point,calls,wall_ns,qps,p50_ms,p95_ms,p99_ms,cpu_p50_ms,cpu_p95_ms,process_cpu_ms,cpu_cores,mean_active_clients,overlap_ns,minor_faults,major_faults,voluntary_switches,involuntary_switches,rss_before,rss_after,heap_live_before,heap_live_after,executors,queue_p95_ms\n";
        std::ifstream points(argv[5]);if(!points)throw std::runtime_error("points missing");Point p;int requests,executors;
        while(points>>p.name>>p.mode>>p.ef>>p.factor>>p.threshold>>p.filter>>p.connections>>p.expected_scan>>requests>>executors) {
            if(p.connections!=64 || executors<1 || executors>64 || requests!=512 || p.filter!="none")throw std::runtime_error("invalid registered queue load");
            QueueTrace(db,con,queries,dim,p,requests,executors,out,summary);
        }
        return 0;
    } catch(const std::exception &e){std::cerr<<"FAILED "<<e.what()<<'\n';return 1;}
}
