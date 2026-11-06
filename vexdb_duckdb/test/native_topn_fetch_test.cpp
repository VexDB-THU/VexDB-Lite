#define main historical_benchmark_main
#include "benchmark/vex_quantization_product_benchmark.cpp"
#undef main
#include <atomic>
#include <map>

namespace {
std::string Snapshot(MaterializedQueryResult &r) {
    std::ostringstream s;
    s<<r.RowCount()<<':';
    for(idx_t i=0;i<r.RowCount();++i)for(idx_t c=0;c<r.ColumnCount();++c)s<<r.GetValue(c,i).ToString()<<'|';
    return s.str();
}
size_t checked=0,rewritten=0;
std::string Difference(MaterializedQueryResult &before,MaterializedQueryResult &after) {
    std::ostringstream s;s<<"rows "<<before.RowCount()<<" -> "<<after.RowCount();
    for(idx_t i=0;i<std::min(before.RowCount(),after.RowCount());++i) {
        for(idx_t c=0;c<std::min(before.ColumnCount(),after.ColumnCount());++c) {
            const auto a=before.GetValue(c,i).ToString(),b=after.GetValue(c,i).ToString();
            if(a!=b){s<<"; first mismatch row="<<i<<" column="<<c<<" before="<<a.substr(0,160)<<" after="<<b.substr(0,160);return s.str();}
        }
    }
    return s.str();
}
bool SameRowsAndOrderKeys(Connection &con,MaterializedQueryResult &before,
                         MaterializedQueryResult &after,const std::string &key_sql) {
    if(before.RowCount()!=after.RowCount()||before.ColumnCount()!=after.ColumnCount())return false;
    auto keys=Exec(con,key_sql);std::map<int,double> order;
    for(idx_t i=0;i<keys->RowCount();++i) {
        if(!order.emplace(keys->GetValue(0,i).GetValue<int>(),keys->GetValue(1,i).GetValue<double>()).second)
            throw std::runtime_error("tie comparison requires unique reference IDs");
    }
    std::vector<std::vector<std::string>> expected,actual;
    for(idx_t i=0;i<before.RowCount();++i) {
        const auto a=order.find(before.GetValue(0,i).GetValue<int>());
        const auto b=order.find(after.GetValue(0,i).GetValue<int>());
        if(a==order.end()||b==order.end()||a->second!=b->second)return false;
        std::vector<std::string> x,y;
        for(idx_t c=0;c<before.ColumnCount();++c){x.push_back(before.GetValue(c,i).ToString());y.push_back(after.GetValue(c,i).ToString());}
        expected.push_back(std::move(x));actual.push_back(std::move(y));
    }
    std::sort(expected.begin(),expected.end());std::sort(actual.begin(),actual.end());
    return expected==actual;
}
void Check(Connection &con,const std::string &sql,bool required=false,const std::string &key_sql="") {
    Exec(con,"SET vexdb_native_topn_fetch=false"); auto before=Exec(con,sql); auto expected=Snapshot(*before);
    Exec(con,"SET vexdb_native_topn_fetch=true");auto plan=Exec(con,"EXPLAIN "+sql)->ToString();
    bool found=plan.find("VEXDB_NATIVE_TOPN_FETCH")!=std::string::npos;
    if(required&&!found)throw std::runtime_error("expected direct native fetch\n"+plan.substr(0,600));
    auto after=Exec(con,sql);
    const bool same=key_sql.empty()?expected==Snapshot(*after):SameRowsAndOrderKeys(con,*before,*after,key_sql);
    if(!same)throw std::runtime_error("SQL result changed: "+sql+"\n"+Difference(*before,*after)+"\nPLAN "+plan.substr(0,1500));
    ++checked;rewritten+=found;
}
void Options(Connection &con) {Exec(con,"SET threads=4");Exec(con,"SET vexdb_filtered_search_mode='exact'");}
const std::string distance="l2_distance(embedding,[.1,.2,.3,.4]::FLOAT[4])";
}
int main(int argc,char**argv) {
    if(argc!=3||std::filesystem::exists(argv[2]))return 2;
    try {
        DBConfig cfg;cfg.SetOptionByName("allow_unsigned_extensions",true);DuckDB db(argv[2],&cfg);
        Connection con(db);Exec(con,"LOAD "+Quote(argv[1]));Options(con);
        Exec(con,"CREATE TABLE items AS SELECT i::INTEGER item_id,(i%19)::INTEGER department_id, [sin(i*.31)::FLOAT,cos(i*.17)::FLOAT,sin(i*.53)::FLOAT,cos(i*.71)::FLOAT]::FLOAT[4] embedding, repeat('record-'||i::VARCHAR||'-',32) payload, CASE WHEN i%11=0 THEN NULL ELSE i END nullable FROM range(4096)t(i)");
        Exec(con,"CREATE VIEW aliases AS SELECT item_id AS code,embedding AS feature,payload AS content,department_id AS category FROM items");
        for(auto pred:{"department_id=3","department_id>2","nullable IS NULL","nullable IS NOT NULL AND department_id IN (2,7)","item_id>=4093","item_id<0"}) {
            for(auto tail:{"LIMIT 10","LIMIT 7 OFFSET 3"}) Check(con,"SELECT item_id,payload FROM items WHERE "+std::string(pred)+" ORDER BY "+distance+" "+tail);
        }
        Check(con,"SELECT payload,item_id,embedding FROM items WHERE department_id=3 ORDER BY "+distance+" LIMIT 10",true);
        Check(con,"SELECT code,content FROM aliases WHERE category=4 ORDER BY l2_distance(feature,[.1,.2,.3,.4]::FLOAT[4]) LIMIT 10",true);
        Check(con,"SELECT item_id,payload FROM items WHERE department_id>2 ORDER BY inner_product(embedding,[.1,.2,.3,.4]::FLOAT[4]) DESC LIMIT 10");
        Check(con,"SELECT item_id,payload FROM items WHERE department_id>2 ORDER BY cosine_distance(embedding,[.1,.2,.3,.4]::FLOAT[4]) LIMIT 10");
        Check(con,"SELECT item_id,payload FROM items WHERE rowid IN (SELECT rowid FROM items WHERE department_id=3 ORDER BY "+distance+" LIMIT 10) AND item_id%2=0 ORDER BY item_id");
        for(bool on:{false,true}) {
            Exec(con,std::string("SET vexdb_native_topn_fetch=")+(on?"true":"false"));
            auto stmt=con.Prepare("SELECT item_id,payload FROM items WHERE department_id=$2 ORDER BY l2_distance(embedding,$1::FLOAT[4]) LIMIT 10");
            for(int item_id:{3,4,99,0,3}) {
                vector<Value> params{Value::ARRAY(LogicalType::FLOAT,{Value::FLOAT(.1),Value::FLOAT(.2),Value::FLOAT(.3),Value::FLOAT(.4)}),Value::INTEGER(item_id)};
                auto r=stmt->Execute(params,false);if(r->HasError())throw std::runtime_error(r->GetError());
                auto expected=Exec(con,"SELECT item_id,payload FROM items WHERE department_id="+std::to_string(item_id)+" ORDER BY "+distance+" LIMIT 10");
                if(Snapshot(r->Cast<MaterializedQueryResult>())!=Snapshot(*expected))throw std::runtime_error("prepared values stale");++checked;
            }
        }
        Connection writer(db);Options(writer);
        Exec(con,"BEGIN");Exec(con,"SELECT count(*) FROM items");
        Exec(writer,"BEGIN");Exec(writer,"UPDATE items SET payload='updated' WHERE item_id=1");Exec(writer,"DELETE FROM items WHERE item_id=3");Exec(writer,"INSERT INTO items VALUES(5000,3,[.1,.2,.3,.4],'new',NULL)");Exec(writer,"COMMIT");
        Check(con,"SELECT item_id,payload FROM items WHERE department_id=3 ORDER BY "+distance+" LIMIT 10",true);Exec(con,"COMMIT");
        Check(con,"SELECT item_id,payload FROM items WHERE department_id=3 ORDER BY "+distance+" LIMIT 10",true);
        Exec(con,"BEGIN");Exec(con,"INSERT INTO items VALUES(6000,3,[.1,.2,.3,.4],'local',NULL)");
        Check(con,"SELECT item_id,payload FROM items WHERE department_id=3 ORDER BY "+distance+" LIMIT 10",true);Exec(con,"ROLLBACK");
        Exec(con,"SET late_materialization_max_rows=10000");
        Exec(con,"CREATE TABLE many AS SELECT i::INTEGER item_id,(i%19)::INTEGER department_id,[sin(i*.31)::FLOAT,cos(i*.17)::FLOAT,sin(i*.53)::FLOAT,cos(i*.71)::FLOAT]::FLOAT[4] embedding,repeat('large-result-'||i::VARCHAR||'-',32) payload FROM range(50000)t(i)");
        Exec(con,"CHECKPOINT");
        Exec(con,"SET memory_limit='64MB'");
        // Preserve the original single-key plan. Every rank must have the
        // exact same SQL distance and the complete ID/payload multiset must
        // match; SQL permits permutations only within exact distance ties.
        const std::string keys="SELECT item_id,("+distance+")::DOUBLE FROM many";
        Check(con,"SELECT item_id,payload FROM many WHERE department_id>2 ORDER BY "+distance+" LIMIT 2500",true,keys);
        Exec(con,"BEGIN");
        Exec(con,"INSERT INTO many SELECT (50000+i)::INTEGER,(i%19)::INTEGER,[sin(i*.37)::FLOAT,cos(i*.21)::FLOAT,sin(i*.43)::FLOAT,cos(i*.61)::FLOAT]::FLOAT[4],repeat('local-result-'||i::VARCHAR||'-',32) FROM range(2000)t(i)");
        Check(con,"SELECT item_id,payload FROM many WHERE department_id>2 ORDER BY "+distance+" LIMIT 2500",true,keys);
        Exec(con,"ROLLBACK");
        std::vector<std::thread> readers;std::vector<std::string> errors(8);
        for(int i=0;i<8;++i) readers.emplace_back([&,i]{try{Connection reader(db);Options(reader);Exec(reader,"SET vexdb_native_topn_fetch=true");for(int j=0;j<30;++j){auto r=Exec(reader,"SELECT item_id,payload FROM items WHERE department_id="+std::to_string(i)+" ORDER BY "+distance+" LIMIT 10");if(r->RowCount()!=10)throw std::runtime_error("missing row");}}catch(std::exception&e){errors[i]=e.what();}});
        for(auto &r:readers)r.join();for(auto&e:errors)if(!e.empty())throw std::runtime_error(e);
        std::cout<<"PASS "<<checked<<" exact SQL comparisons, "<<rewritten<<" direct fetch plans, prepared values, committed/local snapshots, eight readers\n";
    }catch(std::exception&e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
