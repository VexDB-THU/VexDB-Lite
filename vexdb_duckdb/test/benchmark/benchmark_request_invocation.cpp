// Native SQL quality collector. Reads vectors/metadata/queries, never answers.
#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/prepared_statement.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <vector>
#include <set>
#include <chrono>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <cmath>
#include <sys/resource.h>
using namespace duckdb;
using Config=std::map<std::string,std::string>;
Config Read(const std::string&p){std::ifstream f(p);if(!f)throw std::runtime_error("config missing");Config c;std::string s;while(std::getline(f,s)){auto i=s.find('=');if(i!=std::string::npos)c[s.substr(0,i)]=s.substr(i+1);}return c;}
std::string Get(const Config&c,const std::string&k,const std::string&d=""){auto i=c.find(k);return i==c.end()?d:i->second;}
std::vector<std::string> Split(const std::string&s){std::vector<std::string>v;std::stringstream f(s);std::string x;while(std::getline(f,x,','))v.push_back(x);return v;}
std::string Body(int id){std::string s="proxy-payload-"+std::to_string(id)+" ",r;for(int i=0;i<32;++i)r+=s;return r;}
unique_ptr<MaterializedQueryResult> Exec(Connection&c,const std::string&s){auto r=c.Query(s);if(r->HasError())throw std::runtime_error(r->GetError()+" SQL="+s.substr(0,160));return r;}
std::string VectorSQL(const std::vector<float>&v,int dim){std::ostringstream s;s<<std::setprecision(9)<<'[';for(int j=0;j<dim;++j){if(j)s<<',';s<<v[j];}s<<"]::FLOAT["<<dim<<']';return s.str();}
std::string Predicate(const std::string&p,int qi){if(p=="none")return "";if(p=="broad")return " WHERE is_active";if(p=="tenant")return " WHERE tenant_id="+std::to_string(qi%100)+" AND is_public";if(p=="catalog")return " WHERE in_stock AND price_cents BETWEEN "+std::to_string(1000+qi%5*100)+" AND 9000";throw std::runtime_error("profile");}


std::string ParamSQL(const std::string &profile,int dim) {
 std::string predicate;
 if(profile=="none")predicate="";
 else if(profile=="broad")predicate=" WHERE is_active";
 else if(profile=="tenant")predicate=" WHERE tenant_id=$2 AND is_public";
 else if(profile=="catalog")predicate=" WHERE in_stock AND price_cents BETWEEN $2 AND 9000";
 else throw std::runtime_error("profile");
 return "SELECT id,body FROM vectors"+predicate+" ORDER BY l2_distance(embedding,$1::FLOAT["+std::to_string(dim)+"]) LIMIT 10";
}
unique_ptr<PreparedStatement> PrepareRequest(Connection &c,const Config &cfg,const std::string &profile,int dim,const std::string &prefix="") {
 auto mode=Get(cfg,"request_mode","literal");if(mode=="literal")return nullptr;if(mode!="parameter")throw std::runtime_error("request_mode");
 auto statement=c.Prepare(prefix+ParamSQL(profile,dim));if(statement->HasError())throw std::runtime_error(statement->GetError());return statement;
}
unique_ptr<MaterializedQueryResult> Request(Connection &c,PreparedStatement *statement,const std::vector<float> &query,const std::string &profile,int qi,const std::string &literal) {
 if(!statement)return Exec(c,literal);
 vector<Value> elements;elements.reserve(query.size());for(float value:query)elements.push_back(Value::FLOAT(value));
 vector<Value> params;params.push_back(Value::ARRAY(LogicalType::FLOAT,std::move(elements)));
 if(profile=="tenant")params.push_back(Value::INTEGER(qi%100));else if(profile=="catalog")params.push_back(Value::INTEGER(1000+qi%5*100));
 auto result=statement->Execute(params,false);if(result->HasError())throw std::runtime_error(result->GetError());
 if(result->type!=QueryResultType::MATERIALIZED_RESULT)throw std::runtime_error("unexpected streaming result");
 return unique_ptr<MaterializedQueryResult>(static_cast<MaterializedQueryResult *>(result.release()));
}
void MeasureDirect(DuckDB &db,const Config &cfg,int dim) {
 using Clock=std::chrono::steady_clock;auto ns=[](auto a,auto b){return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count();};
 int clients=std::stoi(Get(cfg,"clients","1")),seconds=std::stoi(Get(cfg,"seconds","6"));if(clients<1||clients>64||seconds<1||seconds>60)throw std::runtime_error("load bounds");
 std::filesystem::path out=cfg.at("out");if(std::filesystem::exists(out))throw std::runtime_error("output exists");std::filesystem::create_directories(out);
 std::ifstream f(cfg.at("queries"),std::ios::binary);std::vector<std::string>sqls;std::vector<std::vector<float>> query_values;std::vector<float>q(dim);auto profile=Get(cfg,"profile","none");int nq=std::stoi(Get(cfg,"query_count","128"));if(nq<1||nq>4096)throw std::runtime_error("query count");
 for(int qi=0;qi<nq;++qi){if(!f.read(reinterpret_cast<char*>(q.data()),dim*4))throw std::runtime_error("queries truncated");sqls.push_back("SELECT id,body FROM vectors"+Predicate(profile,qi)+" ORDER BY l2_distance(embedding,"+VectorSQL(q,dim)+") LIMIT 10");query_values.push_back(q);}
 auto setup=[&](Connection&c){Exec(c,"SET vexdb_brute_force_threshold=0");Exec(c,"SET vexdb_ef_search="+cfg.at("ef"));Exec(c,"SET vexdb_pq_search_mode='"+Get(cfg,"pq_mode","off")+"'");Exec(c,"SET vexdb_pq_refine_k_factor="+Get(cfg,"refine_factor","1.0"));Exec(c,"SET vexdb_filtered_search_mode='auto'");Exec(c,"SET vexdb_search_profile=false");Exec(c,"SET vexdb_graph_bidirectional_search="+Get(cfg,"bidirectional","false"));Exec(c,"SET vexdb_native_topn_fetch="+Get(cfg,"native_topn_fetch","false"));return PrepareRequest(c,cfg,profile,dim);};
 struct Result{std::vector<int>ids;size_t bytes=0;};auto search=[&](Connection&c,int qi,PreparedStatement *statement){auto r=Request(c,statement,query_values[qi],profile,qi,sqls[qi]);if(r->RowCount()!=10)throw std::runtime_error("row count");Result z;std::set<int>seen;for(idx_t i=0;i<r->RowCount();++i){int id=r->GetValue(0,i).GetValue<int32_t>();auto body=r->GetValue(1,i).GetValue<std::string>();if(body!=Body(id)||!seen.insert(id).second)throw std::runtime_error("body/duplicate");z.ids.push_back(id);z.bytes+=body.size();}return z;};
 Connection admin(db);auto admin_statement=setup(admin);auto explain_statement=PrepareRequest(admin,cfg,profile,dim,"EXPLAIN ");std::ofstream(out/"plan.txt")<<Request(admin,explain_statement.get(),query_values[0],profile,0,"EXPLAIN "+sqls[0])->ToString();for(int i=0;i<8;++i)search(admin,i,admin_statement.get());std::ofstream(out/"query.sql")<<sqls[0]<<';';
 struct Sample{uint64_t ordinal;int qi;int64_t begin,end;Result result;};std::vector<std::vector<Sample>>samples(clients);std::vector<std::string>errors(clients);std::vector<std::thread>threads;std::vector<std::unique_ptr<Connection>> retained_connections(clients);std::vector<unique_ptr<PreparedStatement>> retained_statements(clients);std::mutex mu;std::condition_variable cv;int ready=0;bool go=false,abort=false;uint64_t issued=0;Clock::time_point start;
 for(int client=0;client<clients;++client)threads.emplace_back([&,client]{auto &c=retained_connections[client];auto &statement=retained_statements[client];try{c=std::make_unique<Connection>(db);statement=setup(*c);search(*c,client%nq,statement.get());}catch(std::exception&e){errors[client]=e.what();}{std::unique_lock<std::mutex>l(mu);++ready;cv.notify_all();cv.wait(l,[&]{return go;});if(abort)return;}try{while(true){uint64_t ord;{std::lock_guard<std::mutex>l(mu);if(issued>=uint64_t(clients)&&issued%uint64_t(nq)==0&&Clock::now()-start>=std::chrono::seconds(seconds))break;ord=issued++;}int qi=ord%uint64_t(nq);auto b=Clock::now();auto r=search(*c,qi,statement.get());auto e=Clock::now();samples[client].push_back({ord,qi,ns(start,b),ns(start,e),std::move(r)});}}catch(std::exception&e){errors[client]=e.what();}});
 rusage before{},after{};{std::unique_lock<std::mutex>l(mu);cv.wait(l,[&]{return ready==clients;});for(auto&e:errors)if(!e.empty())abort=true;getrusage(RUSAGE_SELF,&before);start=Clock::now();go=true;}cv.notify_all();std::cout<<"TIMING\n"<<std::flush;for(auto&t:threads)t.join();auto wall=ns(start,Clock::now());getrusage(RUSAGE_SELF,&after);for(auto&e:errors)if(!e.empty())throw std::runtime_error(e);if(abort)throw std::runtime_error("warmup failed");
 auto queue=Exec(admin,"SELECT struct_extract(vexdb_search_queue_state(),'limit'),struct_extract(vexdb_search_queue_state(),'active'),struct_extract(vexdb_search_queue_state(),'waiting'),struct_extract(vexdb_search_queue_state(),'peak_active'),struct_extract(vexdb_search_queue_state(),'admitted'),struct_extract(vexdb_search_queue_state(),'cancelled'),struct_extract(vexdb_search_queue_state(),'timed_out')");
 std::ofstream qs(out/"queue.csv");qs<<"limit,active,waiting,peak_active,admitted,cancelled,timed_out\n";for(idx_t i=0;i<7;++i){if(i)qs<<',';qs<<queue->GetValue(i,0).GetValue<uint64_t>();}qs<<'\n';

 std::ofstream rows(out/"timing.csv");rows<<"client,ordinal,query,begin_ns,end_ns,payload_bytes,ids\n";std::vector<double>lat;int participating=0;size_t calls=0;for(int c=0;c<clients;++c){participating+=!samples[c].empty();for(auto &x:samples[c]){rows<<c<<','<<x.ordinal<<','<<x.qi<<','<<x.begin<<','<<x.end<<','<<x.result.bytes<<',';for(int id:x.result.ids)rows<<id<<' ';rows<<'\n';lat.push_back((x.end-x.begin)/1e6);++calls;}}
 if(calls!=issued||calls%size_t(nq)||participating!=clients||lat.empty())throw std::runtime_error("coverage");std::sort(lat.begin(),lat.end());auto pct=[&](double p){return lat[std::min(lat.size()-1,size_t(std::ceil(p*lat.size())-1))];};auto cpu=[](rusage&r){return (r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000.0+(r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1000.0;};
 std::ofstream point(out/"point.csv");point<<std::setprecision(15)<<"calls,wall_ns,qps,p50_ms,p95_ms,p99_ms,driver_cpu_ms,driver_maxrss_kb,participating_clients\n"<<calls<<','<<wall<<','<<calls*1e9/wall<<','<<pct(.5)<<','<<pct(.95)<<','<<pct(.99)<<','<<cpu(after)-cpu(before)<<','<<after.ru_maxrss<<','<<participating<<'\n';std::cout<<"DONE calls="<<calls<<" qps="<<calls*1e9/wall<<std::endl;
}

// Benchmark host connection pool. Native module and search settings are
// unchanged. All waits are event-driven and included in each request clock.
// This is not a product SQL setting or a production cancellation interface.
class BenchmarkConnectionPool {
 struct Waiter { std::condition_variable ready; };
public:
 struct Session { std::unique_ptr<Connection> connection; unique_ptr<PreparedStatement> statement; };
 struct Stats { uint64_t active,waiting,peak,admitted; };
 class Lease {
 public:
  Lease(const Lease&)=delete;Lease &operator=(const Lease&)=delete;
  ~Lease(){pool.Release(index);}
  Session &Get(){return pool.sessions[index];}
  size_t Index() const{return index;}
 private:
  friend class BenchmarkConnectionPool;
  Lease(BenchmarkConnectionPool &p,size_t i):pool(p),index(i){}
  BenchmarkConnectionPool &pool;size_t index;
 };
 std::vector<Session> sessions;
 explicit BenchmarkConnectionPool(size_t count):sessions(count){for(size_t i=0;i<count;++i)available.push_back(i);}
 Lease Acquire(){
  std::unique_lock<std::mutex> guard(mutex);
  if(pending.empty()&&!available.empty())return Admit();
  Waiter waiter;pending.push_back(&waiter);
  waiter.ready.wait(guard,[&]{return pending.front()==&waiter&&!available.empty();});
  pending.pop_front();return Admit();
 }
 Stats Snapshot(){std::lock_guard<std::mutex> guard(mutex);return{active,pending.size(),peak,admitted};}
private:
 Lease Admit(){size_t i=available.front();available.pop_front();++active;++admitted;peak=std::max(peak,active);Notify();return Lease(*this,i);}
 void Notify(){if(!pending.empty()&&!available.empty())pending.front()->ready.notify_one();}
 void Release(size_t index){std::lock_guard<std::mutex> guard(mutex);available.push_back(index);--active;Notify();}
 std::mutex mutex;std::deque<size_t> available;std::deque<Waiter*> pending;uint64_t active=0,peak=0,admitted=0;
};

void MeasurePooled(DuckDB &db,const Config &cfg,int dim){
 using Clock=std::chrono::steady_clock;auto ns=[](auto a,auto b){return std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count();};
 const int clients=std::stoi(Get(cfg,"clients","1")),seconds=std::stoi(Get(cfg,"seconds","20")),requested=std::stoi(cfg.at("pool_connections"));
 if(clients<1||clients>64||seconds<1||seconds>60||requested<1||requested>64)throw std::runtime_error("load bounds");
 const int physical=std::min(clients,requested),nq=std::stoi(Get(cfg,"query_count","256"));if(nq!=256)throw std::runtime_error("query count");
 const auto profile=Get(cfg,"profile","none");const std::filesystem::path out=cfg.at("out");if(std::filesystem::exists(out))throw std::runtime_error("output exists");std::filesystem::create_directories(out);
 std::ifstream file(cfg.at("queries"),std::ios::binary);std::vector<std::vector<float>> values;std::vector<std::string> sqls;
 for(int qi=0;qi<nq;++qi){std::vector<float> q(dim);if(!file.read(reinterpret_cast<char*>(q.data()),dim*4))throw std::runtime_error("queries truncated");sqls.push_back("SELECT id,body FROM vectors"+Predicate(profile,qi)+" ORDER BY l2_distance(embedding,"+VectorSQL(q,dim)+") LIMIT 10");values.push_back(std::move(q));}
 auto setup=[&](Connection &c){Exec(c,"SET vexdb_brute_force_threshold=0");Exec(c,"SET vexdb_ef_search="+cfg.at("ef"));Exec(c,"SET vexdb_pq_search_mode='"+Get(cfg,"pq_mode","off")+"'");Exec(c,"SET vexdb_pq_refine_k_factor="+Get(cfg,"refine_factor","1.0"));Exec(c,"SET vexdb_filtered_search_mode='auto'");Exec(c,"SET vexdb_search_profile=false");Exec(c,"SET vexdb_graph_bidirectional_search="+Get(cfg,"bidirectional","false"));Exec(c,"SET vexdb_native_topn_fetch="+Get(cfg,"native_topn_fetch","false"));return PrepareRequest(c,cfg,profile,dim);};
 struct Result{std::vector<int>ids;size_t bytes=0;};
 auto search=[&](Connection &c,PreparedStatement *statement,int qi){auto rows=Request(c,statement,values[qi],profile,qi,sqls[qi]);if(rows->RowCount()!=10)throw std::runtime_error("row count");Result result;std::set<int>seen;for(idx_t i=0;i<rows->RowCount();++i){int id=rows->GetValue(0,i).GetValue<int32_t>();auto body=rows->GetValue(1,i).GetValue<std::string>();if(body!=Body(id)||!seen.insert(id).second)throw std::runtime_error("body/duplicate");result.ids.push_back(id);result.bytes+=body.size();}return result;};
 Connection admin(db);auto admin_statement=setup(admin);auto explain=PrepareRequest(admin,cfg,profile,dim,"EXPLAIN ");std::ofstream(out/"plan.txt")<<Request(admin,explain.get(),values[0],profile,0,"EXPLAIN "+sqls[0])->ToString();for(int i=0;i<8;++i)search(admin,admin_statement.get(),i);std::ofstream(out/"query.sql")<<sqls[0]<<';';
 BenchmarkConnectionPool pool(physical);
 for(auto &session:pool.sessions){session.connection=std::make_unique<Connection>(db);session.statement=setup(*session.connection);}
 struct Sample{uint64_t ordinal;int query;int64_t begin,end,wait;size_t session;Result result;};
 std::vector<std::vector<Sample>> samples(clients);std::vector<std::string> errors(clients);std::vector<std::thread> threads;std::mutex mutex;std::condition_variable ready_condition;int ready=0;bool go=false,abort=false;uint64_t issued=0;Clock::time_point start;
 for(int client=0;client<clients;++client)threads.emplace_back([&,client]{
  try{auto lease=pool.Acquire();auto &s=lease.Get();search(*s.connection,s.statement.get(),client%nq);}catch(std::exception &error){errors[client]=error.what();}
  {std::unique_lock<std::mutex> guard(mutex);++ready;ready_condition.notify_all();ready_condition.wait(guard,[&]{return go;});if(abort)return;}
  try{while(true){uint64_t ordinal;{std::lock_guard<std::mutex> guard(mutex);if(issued>=uint64_t(clients)&&issued%nq==0&&Clock::now()-start>=std::chrono::seconds(seconds))break;ordinal=issued++;}
   const int qi=ordinal%nq;const auto begin=Clock::now();Result result;int64_t wait;size_t session;
   {auto lease=pool.Acquire();const auto admitted=Clock::now();session=lease.Index();wait=ns(begin,admitted);auto &s=lease.Get();result=search(*s.connection,s.statement.get(),qi);}
   const auto end=Clock::now();samples[client].push_back({ordinal,qi,ns(start,begin),ns(start,end),wait,session,std::move(result)});
  }}catch(std::exception &error){errors[client]=error.what();}
 });
 rusage before{},after{};BenchmarkConnectionPool::Stats queue_before;
 {std::unique_lock<std::mutex> guard(mutex);ready_condition.wait(guard,[&]{return ready==clients;});for(const auto &error:errors)if(!error.empty())abort=true;queue_before=pool.Snapshot();if(queue_before.active||queue_before.waiting)throw std::runtime_error("warm pool not idle");getrusage(RUSAGE_SELF,&before);start=Clock::now();go=true;}
 ready_condition.notify_all();std::cout<<"TIMING\n"<<std::flush;for(auto &thread:threads)thread.join();const auto wall=ns(start,Clock::now());getrusage(RUSAGE_SELF,&after);for(const auto &error:errors)if(!error.empty())throw std::runtime_error(error);if(abort)throw std::runtime_error("warmup failed");auto queue_after=pool.Snapshot();
 std::ofstream rows(out/"timing.csv");rows<<"client,ordinal,query,begin_ns,end_ns,payload_bytes,ids,connection_wait_ns,physical_session\n";std::vector<double> latency;size_t calls=0;int participating=0;
 for(int client=0;client<clients;++client){participating+=!samples[client].empty();for(const auto &sample:samples[client]){rows<<client<<','<<sample.ordinal<<','<<sample.query<<','<<sample.begin<<','<<sample.end<<','<<sample.result.bytes<<',';for(int id:sample.result.ids)rows<<id<<' ';rows<<','<<sample.wait<<','<<sample.session<<'\n';latency.push_back((sample.end-sample.begin)/1e6);++calls;}}
 if(calls!=issued||calls%nq||participating!=clients||queue_after.active||queue_after.waiting||queue_after.admitted-queue_before.admitted!=calls||queue_after.peak>uint64_t(physical))throw std::runtime_error("coverage or pool ownership");std::sort(latency.begin(),latency.end());auto pct=[&](double p){return latency[std::min(latency.size()-1,size_t(std::ceil(p*latency.size())-1))];};auto cpu=[](rusage &r){return(r.ru_utime.tv_sec+r.ru_stime.tv_sec)*1000.0+(r.ru_utime.tv_usec+r.ru_stime.tv_usec)/1000.0;};
 std::ofstream point(out/"point.csv");point<<std::setprecision(15)<<"calls,wall_ns,qps,p50_ms,p95_ms,p99_ms,driver_cpu_ms,driver_maxrss_kb,participating_clients,physical_connections\n"<<calls<<','<<wall<<','<<calls*1e9/wall<<','<<pct(.5)<<','<<pct(.95)<<','<<pct(.99)<<','<<cpu(after)-cpu(before)<<','<<after.ru_maxrss<<','<<participating<<','<<physical<<'\n';
 std::ofstream state(out/"connection-pool.csv");state<<"configured_capacity,physical_connections,active,waiting,peak_active,timed_acquisitions\n"<<requested<<','<<physical<<','<<queue_after.active<<','<<queue_after.waiting<<','<<queue_after.peak<<','<<queue_after.admitted-queue_before.admitted<<'\n';std::cout<<"DONE calls="<<calls<<" clients="<<clients<<" physical_connections="<<physical<<" qps="<<calls*1e9/wall<<'\n';
}

void Measure(DuckDB &db,const Config &cfg,int dim){
 if(std::stoi(Get(cfg,"pool_connections","0"))>0)MeasurePooled(db,cfg,dim);
 else MeasureDirect(db,cfg,dim);
}

int main(int argc,char**argv){try{
 if(argc!=2)return 2;auto cfg=Read(argv[1]);int dim=std::stoi(cfg.at("dim"));auto task=cfg.at("task");std::filesystem::path dbpath=cfg.at("database");
 if(task=="seed"&&std::filesystem::exists(dbpath))throw std::runtime_error("database already exists");
 DBConfig config;config.options.load_extensions=false;config.SetOptionByName("allow_unsigned_extensions",true);if(task!="seed")config.options.access_mode=AccessMode::READ_ONLY; if(cfg.count("temp_directory")){config.SetOptionByName("temp_directory",Value(cfg.at("temp_directory")));config.SetOptionByName("max_temp_directory_size",Value(Get(cfg,"max_temp_directory_size","4GiB")));}
 DuckDB db(dbpath.string(),&config);ExtensionHelper::LoadExtension(db,"core_functions");Connection c(db);Exec(c,"LOAD '"+cfg.at("extension")+"'");auto loaded_extension=Exec(c,"SELECT install_mode FROM duckdb_extensions() WHERE extension_name='vexdb_lite' AND loaded");if(loaded_extension->RowCount()!=1||loaded_extension->GetValue(0,0).GetValue<std::string>()=="STATICALLY_LINKED")throw std::runtime_error("Requested package was not explicitly loaded; SDK built-in extension cannot serve as package acceptance");Exec(c,"SET threads=2");Exec(c,"SET memory_limit='3GB'");Exec(c,"SET GLOBAL vexdb_search_concurrency="+Get(cfg,"search_concurrency","0"));Exec(c,"SET vexdb_graph_bidirectional_search="+Get(cfg,"bidirectional","false"));Exec(c,"SET vexdb_native_topn_fetch="+Get(cfg,"native_topn_fetch","false"));
 if(task=="seed"){
  int n=std::stoi(cfg.at("rows"));std::ifstream base(cfg.at("base"),std::ios::binary),metadata(cfg.at("metadata"));if(!base||!metadata)throw std::runtime_error("seed inputs");
  std::string line;std::getline(metadata,line);auto head=Split(line);std::map<std::string,int>col;for(size_t i=0;i<head.size();++i)col[head[i]]=i;
  Exec(c,"CREATE TABLE vectors(id INTEGER,embedding FLOAT["+std::to_string(dim)+"],tenant_id INTEGER,is_public BOOLEAN,is_active BOOLEAN,in_stock BOOLEAN,price_cents INTEGER,body VARCHAR)");
  {Appender app(c,"vectors");DataChunk chunk;chunk.Initialize(Allocator::DefaultAllocator(),{LogicalType::INTEGER,LogicalType::ARRAY(LogicalType::FLOAT,dim),LogicalType::INTEGER,LogicalType::BOOLEAN,LogicalType::BOOLEAN,LogicalType::BOOLEAN,LogicalType::INTEGER,LogicalType::VARCHAR});
   for(int offset=0;offset<n;offset+=STANDARD_VECTOR_SIZE){chunk.Reset();auto count=std::min<int>(STANDARD_VECTOR_SIZE,n-offset);auto ids=FlatVector::GetData<int32_t>(chunk.data[0]);auto values=FlatVector::GetData<float>(ArrayVector::GetEntry(chunk.data[1]));if(!base.read(reinterpret_cast<char*>(values),size_t(count)*dim*4))throw std::runtime_error("base truncated");
    for(int row=0;row<count;++row){int id=offset+row;ids[row]=id;if(!std::getline(metadata,line))throw std::runtime_error("metadata truncated");auto fields=Split(line);if(std::stoi(fields.at(col.at("id")))!=id)throw std::runtime_error("metadata row mismatch");
     int at=2;for(const char*key:{"tenant_id","is_public","is_active","in_stock","price_cents"}){auto value=fields.at(col.at(key));if(value.empty())FlatVector::SetNull(chunk.data[at],row,true);else if(at==2||at==6)FlatVector::GetData<int32_t>(chunk.data[at])[row]=std::stoi(value);else FlatVector::GetData<bool>(chunk.data[at])[row]=std::stoi(value)!=0;++at;}
     FlatVector::GetData<string_t>(chunk.data[7])[row]=StringVector::AddString(chunk.data[7],Body(id));
    }chunk.SetCardinality(count);app.AppendDataChunk(chunk);
   }app.Close();}
  Exec(c,"CHECKPOINT");std::string method=Get(cfg,"method","raw");std::string opts="metric='l2',m=16,ef_construction=200,threads=2";
  if(method=="pq"){int parts=std::stoi(cfg.at("pq_m"));if(parts<=0||dim%parts)throw std::runtime_error("pq_m");opts+=",quantizer='pq',pq_m="+std::to_string(parts)+",memory_mode='full'";}else if(method!="raw")throw std::runtime_error("method");
  auto begin=std::chrono::steady_clock::now();Exec(c,"CREATE INDEX vector_ann ON vectors USING GRAPH_INDEX(embedding) WITH("+opts+")");Exec(c,"CHECKPOINT");
  std::cout<<"BUILT "<<n<<" dim="<<dim<<" method="<<method<<" elapsed_ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<" storage_bytes="<<std::filesystem::file_size(dbpath)<<'\n';
  std::ofstream(dbpath.string()+".index-info.txt")<<Exec(c,"SELECT * FROM vexdb_index_info()")->ToString();
 }else if(task=="quality"){
  auto out=std::filesystem::path(cfg.at("out"));if(std::filesystem::exists(out))throw std::runtime_error("output exists");std::filesystem::create_directories(out);
  std::ofstream(out/"index-info.txt")<<Exec(c,"SELECT * FROM vexdb_index_info()")->ToString();
  Exec(c,"SET vexdb_brute_force_threshold=0");Exec(c,"SET vexdb_ef_search="+cfg.at("ef"));Exec(c,"SET vexdb_pq_search_mode='"+Get(cfg,"pq_mode","off")+"'");Exec(c,"SET vexdb_pq_refine_k_factor="+Get(cfg,"refine_factor","1.0"));Exec(c,"SET vexdb_filtered_search_mode='auto'");Exec(c,"SET vexdb_search_profile=true");
  std::ifstream f(cfg.at("queries"),std::ios::binary);if(!f)throw std::runtime_error("queries missing");int nq=std::stoi(Get(cfg,"query_count","128"));std::string profile=Get(cfg,"profile","none");auto statement=PrepareRequest(c,cfg,profile,dim);auto explain_statement=PrepareRequest(c,cfg,profile,dim,"EXPLAIN ");
  std::ofstream rows(out/"results.csv"),profiles(out/"profile.txt"),sqls(out/"query.sql");rows<<"query,payload_bytes,ids\n";
  std::vector<float>q(dim);
  for(int qi=0;qi<nq;++qi){if(!f.read(reinterpret_cast<char*>(q.data()),dim*4))throw std::runtime_error("queries truncated");auto sql="SELECT id,body FROM vectors"+Predicate(profile,qi)+" ORDER BY l2_distance(embedding,"+VectorSQL(q,dim)+") LIMIT 10";
   if(qi==0)std::ofstream(out/"plan.txt")<<Request(c,explain_statement.get(),q,profile,qi,"EXPLAIN "+sql)->ToString();auto result=Request(c,statement.get(),q,profile,qi,sql);if(result->RowCount()!=10)throw std::runtime_error("row count");std::set<int>seen;size_t bytes=0;std::string idtext;
   for(idx_t row=0;row<10;++row){int id=result->GetValue(0,row).GetValue<int32_t>();auto body=result->GetValue(1,row).GetValue<std::string>();if(body!=Body(id)||!seen.insert(id).second)throw std::runtime_error("body/duplicate");bytes+=body.size();idtext+=std::to_string(id)+' ';}
   rows<<qi<<','<<bytes<<','<<idtext<<'\n';rows.flush();profiles<<qi<<' '<<Exec(c,"SELECT vexdb_last_search_profile()")->ToString()<<'\n';sqls<<sql<<";\n";
  }
  std::cout<<"QUALITY_RECORDS "<<nq<<'\n';
 }else if(task=="measure"){Measure(db,cfg,dim); }else throw std::runtime_error("task");
 return 0;
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
