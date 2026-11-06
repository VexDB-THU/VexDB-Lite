// Fixed-arrival actual SQL diagnostic. No nearest-neighbor answers are read.
// Keep arrival/connection waiting in latency, including overload and draining.
#define main previous_quantization_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main
#include "vex_search_profile.hpp"
#include <condition_variable>
#include <mutex>
#include <map>
#include <ctime>

namespace {
using Settings = std::map<std::string, std::string>;
Settings SettingsFile(const char *path) {
    Settings result; std::ifstream input(path); std::string line;
    if (!input) throw std::runtime_error("settings missing");
    while (std::getline(input, line)) {
        auto split = line.find('=');
        if (split != std::string::npos) result.emplace(line.substr(0, split), line.substr(split+1));
    }
    return result;
}
int64_t Nanos(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count();
}
int64_t ThreadCPU() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)) throw std::runtime_error("CPU clock");
    return int64_t(t.tv_sec)*1000000000+t.tv_nsec;
}
struct Request {
    int client=-1;
    int64_t scheduled=0,bind_begin=0,execute_begin=0,execute_end=0,read_end=0,cpu=0;
    std::string status="not_started", error;
    std::vector<int32_t> ids;
    VexSearchProfileData stages;
};
std::string CleanError(std::string error) {
    for (auto &c : error) if (c=='\t'||c=='\r'||c=='\n') c=' ';
    return error.substr(0,1024);
}
}

int main(int argc, char **argv) {
    if (argc!=2) return 2;
    try {
        const auto cfg=SettingsFile(argv[1]);
        const auto out=std::filesystem::path(cfg.at("out"));
        if (std::filesystem::exists(out)) throw std::runtime_error("output exists");
        const int dim=std::stoi(cfg.at("dim")),clients=std::stoi(cfg.at("clients"));
        const int seconds=std::stoi(cfg.at("seconds")),pool=std::stoi(cfg.at("query_count"));
        const int drain=std::stoi(cfg.at("drain_seconds")),budget=std::stoi(cfg.at("request_budget_ms"));
        const double rate=std::stod(cfg.at("rate"));
        const bool profile=cfg.at("profile")=="1";
        if(dim<1||clients<1||clients>64||seconds<1||seconds>600||pool<1||pool>1024||
           drain<1||drain>120||budget<1||!std::isfinite(rate)||rate<=0||rate*seconds>200000)
            throw std::runtime_error("invalid bounded workload");
        const size_t count=size_t(std::ceil(rate*seconds));
        const auto raw=Read<float>(cfg.at("queries"),size_t(dim)*pool);
        std::vector<Value> queries;
        for(int i=0;i<pool;++i) {
            vector<Value> values;
            for(int d=0;d<dim;++d) values.push_back(Value::FLOAT(raw[size_t(i)*dim+d]));
            queries.push_back(Value::ARRAY(LogicalType::FLOAT,std::move(values)));
        }
        DBConfig dbconfig;
        dbconfig.SetOptionByName("allow_unsigned_extensions",true);
        dbconfig.SetOptionByName("access_mode",Value("read_only"));
        dbconfig.SetOptionByName("memory_limit",Value("4GB"));
        DuckDB db(cfg.at("database"),&dbconfig); Connection admin(db);
        Exec(admin,"LOAD "+Quote(std::filesystem::absolute(cfg.at("extension")).string()));
        Exec(admin,"SET GLOBAL vexdb_search_concurrency="+cfg.at("cap"));
        Exec(admin,"SELECT * FROM vexdb_index_info()");
        const std::string shape=cfg.at("shape");
        if(shape!="broad"&&shape!="tenant"&&shape!="catalog") throw std::runtime_error("shape");
        const std::string predicate=shape=="broad"?"is_active=$2":shape=="tenant"?
            "tenant_id=$2 AND is_public":"in_stock AND price_cents BETWEEN $2 AND $3";
        const std::string sql="SELECT id,body FROM business WHERE "+predicate+
            " ORDER BY l2_distance(vec,$1::FLOAT["+std::to_string(dim)+"]) LIMIT 10";
        auto setup=[&](Connection &c) {
            Configure(c,cfg.at("mode"),std::stoi(cfg.at("ef")),std::stod(cfg.at("factor")));
            Exec(c,"SET vexdb_graph_bidirectional_search=true");
            Exec(c,"SET vexdb_filtered_search_mode='auto'");
            Exec(c,std::string("SET vexdb_search_profile=")+(profile?"true":"false"));
            if(shape=="catalog") Exec(c,"CREATE TEMP VIEW business AS SELECT id,vec,id%5<>0 in_stock,"
                "(100+id%10000)::INTEGER price_cents,repeat('x',512) body FROM vectors");
            else Exec(c,"CREATE TEMP VIEW business AS SELECT * FROM vectors");
        };
        auto parameters=[&](size_t q) {
            vector<Value> values{queries[q]};
            if(shape=="broad") values.push_back(Value::BOOLEAN(true));
            else if(shape=="tenant") values.push_back(Value::INTEGER(int(q%100)));
            else { values.push_back(Value::INTEGER(1000+int(q%5)*100));values.push_back(Value::INTEGER(9000)); }
            return values;
        };
        std::filesystem::create_directories(out);
        setup(admin); auto explain=admin.Prepare("EXPLAIN "+sql);auto p=parameters(0);
        auto plan=explain->Execute(p,false);if(plan->HasError())throw std::runtime_error(plan->GetError());
        std::ofstream(out/"plan.txt")<<plan->ToString();std::ofstream(out/"query.sql")<<sql<<'\n';
        std::vector<Request> requests(count);
        for(size_t i=0;i<count;++i) requests[i].scheduled=int64_t(double(i)*1e9/rate);
        std::vector<std::shared_ptr<Connection>> connections(clients);
        std::vector<std::string> setup_errors(clients);
        std::vector<std::thread> workers;
        std::mutex mutex;std::condition_variable cv;
        int ready=0;bool go=false,setup_failed=false,finished=false;
        std::atomic<size_t> next{0};std::atomic<bool> stop{false};Clock::time_point start;
        const auto initialization=Clock::now();
        for(int client=0;client<clients;++client) workers.emplace_back([&,client] {
            unique_ptr<PreparedStatement> statement;
            shared_ptr<VexSearchProfileState> observer;
            try {
                auto c=std::make_shared<Connection>(db);setup(*c);statement=c->Prepare(sql);
                if(statement->HasError())throw std::runtime_error(statement->GetError());
                observer=c->context->registered_state->GetOrCreate<VexSearchProfileState>(VexSearchProfileState::Key());
                // Preparation and warmup are recorded outside offered requests.
                for(int q=0;q<std::min(pool,8);++q) {auto v=parameters(q);auto r=statement->Execute(v,false);
                    if(!r||r->HasError())throw std::runtime_error(r?r->GetError():"null warmup");}
                connections[client]=std::move(c);
            } catch(const std::exception &e) {setup_errors[client]=e.what();}
            { std::unique_lock<std::mutex> lock(mutex);++ready;cv.notify_all();cv.wait(lock,[&]{return go;}); }
            if(setup_failed)return;
            while(true) {
                const size_t ordinal=next.fetch_add(1);if(ordinal>=count)break;
                auto &s=requests[ordinal];s.client=client;
                auto scheduled=start+std::chrono::nanoseconds(s.scheduled);
                if(Clock::now()<scheduled)std::this_thread::sleep_until(scheduled);
                s.bind_begin=Nanos(start,Clock::now());
                if(stop) {s.status="not_started_drain_limit";s.read_end=s.bind_begin;continue;}
                if(s.bind_begin-s.scheduled>int64_t(budget)*1000000) {s.status="expired_before_start";s.read_end=s.bind_begin;continue;}
                const auto cpu=ThreadCPU();
                try {
                    auto values=parameters(ordinal%pool);s.execute_begin=Nanos(start,Clock::now());
                    auto result=statement->Execute(values,false);s.execute_end=Nanos(start,Clock::now());
                    if(!result||result->HasError())throw std::runtime_error(result?result->GetError():"null result");
                    auto &materialized=result->Cast<MaterializedQueryResult>();
                    if(materialized.RowCount()!=10)throw std::runtime_error("result count");
                    for(idx_t row=0;row<10;++row) {
                        s.ids.push_back(materialized.GetValue(0,row).GetValue<int32_t>());
                        if(materialized.GetValue(1,row).GetValue<std::string>().empty())throw std::runtime_error("empty payload");
                    }
                    s.read_end=Nanos(start,Clock::now());
                    s.status=s.read_end-s.scheduled>int64_t(budget)*1000000?"ok_late":"ok";
                } catch(const std::exception &e) {
                    s.read_end=Nanos(start,Clock::now());s.status=stop?"error_drain_limit":"error";s.error=CleanError(e.what());
                }
                s.cpu=ThreadCPU()-cpu;s.stages=observer->Completed();
            }
        });
        {std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return ready==clients;});
            for(auto &e:setup_errors)if(!e.empty())setup_failed=true;
            start=Clock::now()+std::chrono::milliseconds(100);go=true;}
        cv.notify_all();std::cout<<"TIMING offered="<<count<<std::endl;
        std::thread guard([&] {
            std::unique_lock<std::mutex> lock(mutex);
            if(cv.wait_until(lock,start+std::chrono::seconds(seconds+drain),[&]{return finished;}))return;
            stop=true;
            // One global drain boundary: no later SQL may start, so an idle
            // connection interrupt cannot accidentally affect a future request.
            for(auto &c:connections)if(c)c->Interrupt();
        });
        for(auto &w:workers)w.join();const auto end=Clock::now();
        {std::lock_guard<std::mutex> lock(mutex);finished=true;}cv.notify_all();guard.join();
        std::ofstream csv(out/"requests.tsv");
        csv<<"ordinal\tclient\tquery\tscheduled_ns\tbind_begin_ns\texecute_begin_ns\texecute_end_ns\tread_end_ns\tthread_cpu_ns\tstatus\terror\tids";
#define REQUEST_HEADER(name) csv<<"\t" #name;
        VEX_SEARCH_PROFILE_FIELDS(REQUEST_HEADER)
#undef REQUEST_HEADER
        csv<<'\n';size_t completed=0,expired=0,errors=0;
        for(size_t i=0;i<count;++i) {auto &s=requests[i];
            if(s.status=="ok"||s.status=="ok_late")++completed;
            else if(s.status=="expired_before_start")++expired;else ++errors;
            csv<<i<<'\t'<<s.client<<'\t'<<i%pool<<'\t'<<s.scheduled<<'\t'<<s.bind_begin<<'\t'<<s.execute_begin
               <<'\t'<<s.execute_end<<'\t'<<s.read_end<<'\t'<<s.cpu<<'\t'<<s.status<<'\t'<<s.error<<'\t';
            for(auto id:s.ids)csv<<id<<' ';
#define REQUEST_VALUE(name) csv<<'\t'<<s.stages.name;
            VEX_SEARCH_PROFILE_FIELDS(REQUEST_VALUE)
#undef REQUEST_VALUE
            csv<<'\n';
        }
        std::ofstream summary(out/"summary.json");
        summary<<std::setprecision(17)<<"{\"offered\":"<<count<<",\"completed\":"<<completed
            <<",\"expired_before_start\":"<<expired<<",\"errors_or_not_started\":"<<errors
            <<",\"offering_seconds\":"<<seconds<<",\"wall_ns\":"<<Nanos(start,end)
            <<",\"initialization_ns\":"<<Nanos(initialization,start)<<",\"drain_interrupted\":"<<int(stop)
            <<",\"profile\":"<<int(profile)<<",\"request_budget_semantics\":\"expire before start; record running deadline misses without per-request interruption\"}\n";
        for(auto &e:setup_errors)if(!e.empty())std::cerr<<CleanError(e)<<'\n';
        return setup_failed?1:0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
