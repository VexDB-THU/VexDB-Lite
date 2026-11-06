// Fixed-arrival SQL acceptance and a separate closed-loop SQL stress phase.
// Nearest-neighbor answers are read only by the independent Python reviewer.
#define main invocation_collector_main
#include "benchmark_request_invocation.cpp"
#undef main
#include <atomic>

namespace {
using LoadClock = std::chrono::steady_clock;
int64_t LoadNanos(LoadClock::time_point a, LoadClock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count();
}
struct LoadSample {
    uint64_t ordinal=0;
    int client=-1, query=0;
    int64_t scheduled=0, begin=0, end=0;
    size_t bytes=0;
    std::vector<int> ids;
    std::string status="not_started", error;
};
std::string LoadError(std::string s) {
    for (auto &c:s) if(c=='\n'||c=='\r'||c=='\t'||c==',') c=' ';
    return s.substr(0,1024);
}
}

int main(int argc,char **argv) {
    if(argc!=2) return 2;
    try {
        auto cfg=Read(argv[1]);
        const int dim=std::stoi(cfg.at("dim")), nq=std::stoi(cfg.at("query_count"));
        const int clients=std::stoi(cfg.at("clients"));
        const int seconds=std::stoi(cfg.at("seconds")), drain=std::stoi(cfg.at("drain_seconds"));
        const int normals=std::stoi(Get(cfg,"normal_windows","3"));
        const int stresses=std::stoi(Get(cfg,"stress_windows","0"));
        const int stress_seconds=std::stoi(Get(cfg,"stress_seconds","20"));
        const double rate=std::stod(cfg.at("rate"));
        if(dim<1||nq<1||nq>4096||clients<1||clients>64||seconds<1||seconds>300||
           drain<1||drain>120||normals<1||normals>4||stresses<0||stresses>2||
           stress_seconds<1||stress_seconds>60||!std::isfinite(rate)||rate<=0||rate*seconds>100000)
            throw std::runtime_error("bounded workload settings");
        const size_t count=size_t(std::ceil(rate*seconds/nq))*nq;
        const auto out=std::filesystem::path(cfg.at("out"));
        if(std::filesystem::exists(out)) throw std::runtime_error("output exists");
        std::filesystem::create_directories(out);
        std::ifstream input(cfg.at("queries"),std::ios::binary);
        std::vector<std::vector<float>> queries(nq,std::vector<float>(dim));
        for(auto &q:queries) if(!input.read(reinterpret_cast<char *>(q.data()),dim*4))
            throw std::runtime_error("truncated query input");
        const auto profile=cfg.at("profile");
        DBConfig config;
        config.options.load_extensions=false;
        config.options.access_mode=AccessMode::READ_ONLY;
        config.SetOptionByName("allow_unsigned_extensions",true);
        config.SetOptionByName("temp_directory",Value(cfg.at("temp_directory")));
        config.SetOptionByName("max_temp_directory_size",Value(Get(cfg,"max_temp_directory_size","4GiB")));
        DuckDB db(cfg.at("database"),&config);
        ExtensionHelper::LoadExtension(db,"core_functions");
        Connection admin(db);
        Exec(admin,"LOAD '"+cfg.at("extension")+"'");
        auto loaded=Exec(admin,"SELECT install_mode FROM duckdb_extensions() WHERE extension_name='vexdb_lite' AND loaded");
        if(loaded->RowCount()!=1||loaded->GetValue(0,0).GetValue<std::string>()!="NOT_INSTALLED")
            throw std::runtime_error("explicit requested module was not loaded");
        std::ofstream(out/"extension.txt")<<loaded->ToString();
        Exec(admin,"SET threads=2"); Exec(admin,"SET memory_limit='3GB'");
        Exec(admin,"SET GLOBAL vexdb_search_concurrency="+Get(cfg,"search_concurrency","0"));
        auto setup=[&](Connection &c) {
            Exec(c,"SET vexdb_brute_force_threshold=0");
            Exec(c,"SET vexdb_ef_search="+cfg.at("ef"));
            Exec(c,"SET vexdb_pq_search_mode='"+Get(cfg,"pq_mode","off")+"'");
            Exec(c,"SET vexdb_pq_refine_k_factor="+Get(cfg,"refine_factor","1"));
            Exec(c,"SET vexdb_graph_bidirectional_search="+Get(cfg,"bidirectional","false"));
            Exec(c,"SET vexdb_native_topn_fetch="+Get(cfg,"native_topn_fetch","false"));
            Exec(c,"SET vexdb_filtered_search_mode='auto'");
            Exec(c,"SET vexdb_search_profile=false");
            return PrepareRequest(c,cfg,profile,dim);
        };
        auto search=[&](Connection &c,PreparedStatement *statement,int qi,LoadSample &s) {
            auto r=Request(c,statement,queries[qi],profile,qi,"");
            if(r->RowCount()!=10) throw std::runtime_error("result row count");
            std::set<int> seen;
            for(idx_t row=0;row<10;++row) {
                int id=r->GetValue(0,row).GetValue<int32_t>();
                auto body=r->GetValue(1,row).GetValue<std::string>();
                if(body!=Body(id)||!seen.insert(id).second) throw std::runtime_error("body or duplicate");
                s.bytes+=body.size(); s.ids.push_back(id);
            }
        };
        auto admin_statement=setup(admin);
        auto explain=PrepareRequest(admin,cfg,profile,dim,"EXPLAIN ");
        std::ofstream(out/"plan.txt")<<Request(admin,explain.get(),queries[0],profile,0,"")->ToString();
        std::ofstream(out/"query.sql")<<ParamSQL(profile,dim)<<';';
        // Warm the complete fixed query set once, then keep every session alive.
        for(int qi=0;qi<nq;++qi) { LoadSample s;search(admin,admin_statement.get(),qi,s); }
        std::vector<std::unique_ptr<Connection>> connections;
        std::vector<unique_ptr<PreparedStatement>> statements;
        for(int c=0;c<clients;++c) {
            connections.push_back(std::make_unique<Connection>(db));
            statements.push_back(setup(*connections.back()));
            LoadSample s;search(*connections.back(),statements.back().get(),c%nq,s);
        }
        std::ofstream manifest(out/"windows.csv");
        manifest<<"name,kind,calls,wall_ns,offering_ns,interrupted,errors,participating_clients\n";
        bool any_error=false;
        std::vector<std::string> kinds;
        for(int i=0;i<normals;++i) {
            if(i==normals-1) for(int j=0;j<stresses;++j) kinds.push_back("stress");
            kinds.push_back("normal");
        }
        int normal_index=0,stress_index=0;
        for(const auto &kind:kinds) {
            const bool normal=kind=="normal";
            const auto name=kind+"-"+std::to_string(normal?normal_index++:stress_index++);
            auto folder=out/name;std::filesystem::create_directory(folder);
            std::vector<std::vector<LoadSample>> samples(clients);
            std::mutex mu;std::condition_variable cv;
            int ready=0;bool go=false,done=false;
            uint64_t issued=0;std::atomic<bool> stop{false};
            LoadClock::time_point start;
            std::vector<std::thread> workers;
            for(int c=0;c<clients;++c) workers.emplace_back([&,c] {
                {std::unique_lock<std::mutex> lock(mu);++ready;cv.notify_all();cv.wait(lock,[&]{return go;});}
                std::this_thread::sleep_until(start);
                for(;;) {
                    uint64_t ordinal;
                    {std::lock_guard<std::mutex> lock(mu);
                        if(normal ? issued>=count :
                           (stop || (issued>=uint64_t(clients)&&issued%uint64_t(nq)==0&&LoadClock::now()-start>=std::chrono::seconds(stress_seconds)))) break;
                        ordinal=issued++;
                    }
                    LoadSample s;s.ordinal=ordinal;s.client=c;s.query=int(ordinal%nq);
                    auto scheduled=normal?start+std::chrono::nanoseconds(int64_t(double(ordinal)*1e9/rate)):LoadClock::now();
                    s.scheduled=LoadNanos(start,scheduled);
                    if(LoadClock::now()<scheduled)std::this_thread::sleep_until(scheduled);
                    s.begin=LoadNanos(start,LoadClock::now());
                    if(stop)s.status="not_started_drain_limit";
                    else try {
                        search(*connections[c],statements[c].get(),s.query,s);s.status="ok";
                    } catch(const std::exception &e) {s.status="sql_error";s.error=LoadError(e.what());}
                    s.end=LoadNanos(start,LoadClock::now());samples[c].push_back(std::move(s));
                }
            });
            {std::unique_lock<std::mutex> lock(mu);cv.wait(lock,[&]{return ready==clients;});
                start=LoadClock::now()+std::chrono::milliseconds(100);go=true;}
            cv.notify_all();std::cout<<"TIMING "<<name<<std::endl;
            const int64_t offering=normal?int64_t(double(count)*1e9/rate):int64_t(stress_seconds)*1000000000;
            std::thread guard([&] {
                std::unique_lock<std::mutex> lock(mu);
                if(cv.wait_until(lock,start+std::chrono::nanoseconds(offering)+std::chrono::seconds(drain),[&]{return done;}))return;
                stop=true;for(auto &c:connections)c->Interrupt();
            });
            for(auto &w:workers)w.join();
            // A normal window includes the entire offered interval even when the last SQL finishes early.
            if(normal&&LoadClock::now()<start+std::chrono::nanoseconds(offering))
                std::this_thread::sleep_until(start+std::chrono::nanoseconds(offering));
            const auto wall=LoadNanos(start,LoadClock::now());
            {std::lock_guard<std::mutex> lock(mu);done=true;}cv.notify_all();guard.join();
            std::ofstream csv(folder/"requests.csv");
            csv<<"client,ordinal,query,scheduled_ns,begin_ns,end_ns,payload_bytes,status,error,ids\n";
            size_t calls=0,errors=0;int participating=0;
            for(int c=0;c<clients;++c) {
                participating+=!samples[c].empty();
                for(const auto &s:samples[c]) {
                    ++calls;errors+=s.status!="ok";
                    csv<<c<<','<<s.ordinal<<','<<s.query<<','<<s.scheduled<<','<<s.begin<<','<<s.end<<','<<s.bytes<<','<<s.status<<','<<s.error<<',';
                    for(auto id:s.ids)csv<<id<<' ';csv<<'\n';
                }
            }
            manifest<<name<<','<<kind<<','<<calls<<','<<wall<<','<<offering<<','<<int(stop)<<','<<errors<<','<<participating<<'\n';manifest.flush();
            std::cout<<"DONE "<<name<<" calls="<<calls<<" errors="<<errors<<std::endl;
            any_error=any_error||stop||errors||calls!=issued||(normal&&calls!=count);
            if(stop)break;
        }
        return any_error?1:0;
    } catch(const std::exception &e) {std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}
}
