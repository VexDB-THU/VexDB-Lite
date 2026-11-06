// Real prepared SQL. Stages are collected without a second SQL request.
#define main previous_quantization_benchmark_main
#include "vex_quantization_product_benchmark.cpp"
#undef main
#include "vex_search_profile.hpp"
#include <condition_variable>
#include <mutex>
#include <ctime>

namespace {
int64_t StageNs(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count();
}
int64_t StageCpu() {
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)) throw std::runtime_error("thread CPU clock");
    return int64_t(t.tv_sec)*1000000000+t.tv_nsec;
}
struct StageSample {
    size_t query;
    int64_t begin,end,cpu;
    std::array<int32_t,10> ids;
    VexSearchProfileData profile;
};
}
int main(int argc,char **argv) {
    if(argc!=14 || std::filesystem::exists(argv[5])) return 2;
    try {
        const int dim=std::stoi(argv[4]),clients=std::stoi(argv[6]),seconds=std::stoi(argv[7]);
        const bool enabled=std::stoi(argv[8])!=0;
        const std::string shape=argv[9],mode=argv[10];
        const int ef=std::stoi(argv[11]),cap=std::stoi(argv[13]);
        const double factor=std::stod(argv[12]);
        if(clients<1 || clients>64 || seconds<1 || seconds>30) return 2;
        const auto raw=Read<float>(argv[3],size_t(dim)*8);
        std::vector<Value> queries;
        for(size_t q=0;q<8;++q) {
            vector<Value> values;
            for(int d=0;d<dim;++d) values.push_back(Value::FLOAT(raw[q*dim+d]));
            queries.push_back(Value::ARRAY(LogicalType::FLOAT,std::move(values)));
        }
        DBConfig config;
        config.SetOptionByName("allow_unsigned_extensions",true);
        config.SetOptionByName("access_mode",Value("read_only"));
        config.SetOptionByName("memory_limit",Value("4GB"));
        DuckDB db(argv[2],&config);
        Connection admin(db);
        Exec(admin,"LOAD "+Quote(std::filesystem::absolute(argv[1]).string()));
        Exec(admin,"SET GLOBAL vexdb_search_concurrency="+std::to_string(cap));
        Exec(admin,"SELECT * FROM vexdb_index_info()");
        auto setup=[&](Connection &c) {
            Configure(c,mode,ef,factor);
            Exec(c,"SET vexdb_graph_bidirectional_search=true");
            Exec(c,"SET vexdb_filtered_search_mode='auto'");
            Exec(c,std::string("SET vexdb_search_profile=")+(enabled?"true":"false"));
            if(shape=="catalog") {
                Exec(c,"CREATE TEMP VIEW business AS SELECT id,vec,id%5<>0 in_stock,"
                       "(100+id%10000)::INTEGER price_cents,repeat('x',512) body FROM vectors");
            } else Exec(c,"CREATE TEMP VIEW business AS SELECT * FROM vectors");
        };
        std::string predicate;
        if(shape=="broad") predicate="is_active=$2";
        else if(shape=="tenant") predicate="tenant_id=$2 AND is_public";
        else if(shape=="catalog") predicate="in_stock AND price_cents BETWEEN $2 AND $3";
        else throw std::runtime_error("shape");
        const auto sql="SELECT id,body FROM business WHERE "+predicate+
            " ORDER BY l2_distance(vec,$1::FLOAT["+std::to_string(dim)+"]) LIMIT 10";
        auto params=[&](size_t q) {
            vector<Value> p{queries[q]};
            if(shape=="broad") p.push_back(Value::BOOLEAN(true));
            else if(shape=="tenant") p.push_back(Value::INTEGER(int(q%100)));
            else {p.push_back(Value::INTEGER(1000+int(q%5)*100));p.push_back(Value::INTEGER(9000));}
            return p;
        };
        setup(admin);
        std::filesystem::create_directories(argv[5]);
        const std::filesystem::path out=argv[5];
        auto explain=admin.Prepare("EXPLAIN "+sql);
        auto plan_parameters=params(0);
        auto plan=explain->Execute(plan_parameters,false);
        if(plan->HasError())throw std::runtime_error(plan->GetError());
        std::ofstream(out/"plan.txt")<<plan->ToString();
        std::ofstream(out/"query.sql")<<sql<<";\n";
        std::vector<std::vector<StageSample>> samples(clients);
        std::vector<std::string> errors(clients);
        std::mutex mutex;
        std::condition_variable cv;
        int ready=0;bool start=false,abort=false;
        Clock::time_point begin;
        std::vector<std::thread> workers;
        for(int client=0;client<clients;++client)workers.emplace_back([&,client] {
            try {
                Connection c(db);setup(c);
                auto statement=c.Prepare(sql);
                if(statement->HasError())throw std::runtime_error(statement->GetError());
                // Same observer copy/hooks in off/on cases; no SQL between requests.
                auto observer=c.context->registered_state->GetOrCreate<VexSearchProfileState>(VexSearchProfileState::Key());
                for(size_t q=0;q<8;++q) {
                    auto warm_parameters=params((q+client)%8);
                    auto warm=statement->Execute(warm_parameters,false);
                    if(warm->HasError())throw std::runtime_error(warm->GetError());
                }
                {std::unique_lock<std::mutex> l(mutex);++ready;cv.notify_all();cv.wait(l,[&]{return start||abort;});}
                if(abort)return;
                do {
                    for(size_t j=0;j<8;++j) {
                        size_t q=(j+client)%8;
                        auto values=params(q);
                        auto a=Clock::now();auto cpu=StageCpu();
                        auto result=statement->Execute(values,false);
                        if(!result||result->HasError())throw std::runtime_error(result?result->GetError():"null result");
                        auto &m=result->Cast<MaterializedQueryResult>();
                        if(m.RowCount()!=10)throw std::runtime_error("result count");
                        std::array<int32_t,10> ids;
                        for(idx_t r=0;r<10;++r) {
                            ids[r]=m.GetValue(0,r).GetValue<int32_t>();
                            if(m.GetValue(1,r).GetValue<std::string>().empty())throw std::runtime_error("payload");
                        }
                        cpu=StageCpu()-cpu;auto b=Clock::now();
                        samples[client].push_back({q,StageNs(begin,a),StageNs(begin,b),cpu,ids,observer->Completed()});
                    }
                }while(Clock::now()<begin+std::chrono::seconds(seconds));
            }catch(const std::exception &e) {
                std::lock_guard<std::mutex> l(mutex);errors[client]=e.what();abort=true;cv.notify_all();
            }
        });
        {std::unique_lock<std::mutex> l(mutex);cv.wait(l,[&]{return ready==clients||abort;});begin=Clock::now();start=true;}
        std::cout<<"TIMING"<<std::endl;cv.notify_all();
        for(auto &w:workers)w.join();
        const auto wall=StageNs(begin,Clock::now());
        std::ofstream csv(out/"requests.csv");
        csv<<"client,ordinal,query,begin_ns,end_ns,thread_cpu_ns,ids,routes,failed";
#define STAGE_HEADER(name) csv<<"," #name;
        VEX_SEARCH_PROFILE_FIELDS(STAGE_HEADER)
#undef STAGE_HEADER
        csv<<"\n";
        std::vector<double> latency;
        for(int c=0;c<clients;++c)for(size_t i=0;i<samples[c].size();++i) {
            auto &s=samples[c][i];auto &p=s.profile;
            csv<<c<<','<<i<<','<<s.query<<','<<s.begin<<','<<s.end<<','<<s.cpu<<',';
            for(auto id:s.ids)csv<<id<<' ';
            csv<<','<<p.routes<<','<<p.failed;
#define STAGE_VALUE(name) csv<<','<<p.name;
            VEX_SEARCH_PROFILE_FIELDS(STAGE_VALUE)
#undef STAGE_VALUE
            csv<<'\n';latency.push_back((s.end-s.begin)/1e6);
        }
        for(auto &e:errors)if(!e.empty())throw std::runtime_error(e);
        std::ofstream summary(out/"summary.json");
        summary<<std::setprecision(15)<<"{\"calls\":"<<latency.size()<<",\"wall_ns\":"<<wall
               <<",\"qps\":"<<latency.size()*1e9/wall<<",\"p95_ms\":"<<Percentile(latency,.95)
               <<",\"profile\":"<<enabled<<",\"clients\":"<<clients<<"}\n";
        std::cout<<"DONE calls="<<latency.size()<<" p95="<<Percentile(latency,.95)<<std::endl;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
