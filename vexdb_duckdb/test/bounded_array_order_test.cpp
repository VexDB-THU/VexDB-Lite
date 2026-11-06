// Compare the bounded ARRAY plan with the engine's exact SQL ordering.
#include "duckdb.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension_helper.hpp"
#include <filesystem>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
using namespace duckdb;
static unique_ptr<MaterializedQueryResult> SQL(Connection &c,const string &s){auto r=c.Query(s);if(r->HasError())throw std::runtime_error(r->GetError());return r;}
static string Quote(const string &s){string q="'";for(char c:s)q+=c=='\''?"''":string(1,c);return q+"'";}
static string Snapshot(MaterializedQueryResult &r){std::ostringstream s;for(idx_t i=0;i<r.RowCount();i++){const auto v=r.GetValue(2,i);const auto &a=ArrayValue::GetChildren(v);s<<r.GetValue(0,i).ToString()<<':'<<r.GetValue(1,i).ToString()<<':'<<a.front().ToString()<<':'<<a.back().ToString()<<'|';}return s.str();}
int main(int argc,char**argv){if(argc!=3||std::filesystem::exists(argv[2]))return 2;try{
 DBConfig cfg;cfg.SetOptionByName("allow_unsigned_extensions",true);
        cfg.options.load_extensions = false;DuckDB db(argv[2],&cfg);
            ExtensionHelper::LoadExtension(db, "core_functions");Connection run(db),ref(db);SQL(run,"LOAD "+Quote(argv[1]));SQL(run,"SET threads=2");SQL(run,"SET memory_limit='3GB'");
 size_t comparisons=0,plans=0;
 for(int dim:{64,1536})for(string metric:{"l2","cosine","ip"})for(bool pq:{false,true}){
  SQL(run,"CREATE TABLE bounded_rows AS SELECT i::INTEGER id,list_transform(range("+std::to_string(dim)+"),j->(sin(i*.17+j*.013)+cos(i*.031*j*.001))::FLOAT)::FLOAT["+std::to_string(dim)+"] embedding,i%3<>0 live,'body-'||i::VARCHAR body FROM range(512)t(i)");
  SQL(run,"CREATE INDEX bounded_ann ON bounded_rows USING GRAPH_INDEX(embedding) WITH (metric='"+metric+"',m=16,ef_construction=160"+(pq?",quantizer='pq',pq_m=8,memory_mode='full'":"")+")");
  SQL(run,"SET vexdb_brute_force_threshold=0");SQL(run,"SET vexdb_ef_search=2048");SQL(run,"SET vexdb_pq_refine_k_factor=1000");SQL(run,string("SET vexdb_pq_search_mode='")+(pq?"pq_only":"off")+"'");SQL(run,"SET vexdb_filtered_search_mode='auto'");SQL(ref,"SET vexdb_brute_force_threshold=1000000");SQL(ref,"SET vexdb_native_topn_fetch=false");
  std::ostringstream q;q<<std::setprecision(9)<<'[';for(int j=0;j<dim;j++){if(j)q<<',';q<<float(std::cos(j*.07));}q<<"]::FLOAT["<<dim<<']';
  string function=metric=="l2"?"l2_distance":metric=="cosine"?"cosine_distance":"inner_product";string order=" ORDER BY "+function+"(embedding,"+q.str()+")"+(metric=="ip"?" DESC":" ASC");
  auto check=[&](const string &filter,int k,int offset){auto sql="SELECT id,body,embedding FROM bounded_rows"+filter+order+" LIMIT "+std::to_string(k)+" OFFSET "+std::to_string(offset);auto a=SQL(run,sql);auto reference_sql=sql;
   if(filter.empty()) {
    // ANN chooses candidates; this unit checks SQL sorting/offset within that
    // same candidate set. Global recall has a separate exact-oracle gate.
    auto candidates=SQL(run,"SELECT id,body,embedding FROM bounded_rows"+order+" LIMIT "+std::to_string(k+offset));
    string ids;for(idx_t i=0;i<candidates->RowCount();++i){if(i)ids+=",";ids+=candidates->GetValue(0,i).ToString();}
    reference_sql="SELECT id,body,embedding FROM bounded_rows WHERE id IN ("+ids+")"+order+" LIMIT "+std::to_string(k)+" OFFSET "+std::to_string(offset);
   }
   auto b=SQL(ref,reference_sql);if(Snapshot(*a)!=Snapshot(*b))throw std::runtime_error("order/offset mismatch: "+metric+" dim="+std::to_string(dim));++comparisons;return sql;};
  for(string filter:{""," WHERE live"})for(int offset:{0,3})check(filter,10,offset);
  check(" WHERE live AND id<0",10,0);check(" WHERE live",2049,0);
  auto sql=check(" WHERE live",10,3);auto plan=SQL(run,"EXPLAIN "+sql)->ToString();if(plan.find("VEXDB_INDEX_SCAN")==string::npos)throw std::runtime_error("ANN plan was not selected");++plans;
  auto native=SQL(ref,"EXPLAIN "+sql)->ToString();if(native.find("VEXDB_INDEX_SCAN")!=string::npos)throw std::runtime_error("reference did not bypass ANN");++plans;
  SQL(run,"DROP TABLE bounded_rows");
 }
 std::cout<<"PASS "<<comparisons<<" SQL ordering/offset/empty/large-limit comparisons and "<<plans<<" plan checks\n";
}catch(const std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
