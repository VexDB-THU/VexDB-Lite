// Check small-result fetch buffers without changing row order or visibility.
#include "duckdb.hpp"
#include "vex_fetch_utils.hpp"
#include <iostream>
#include <stdexcept>
using namespace duckdb;
static void SQL(Connection &c, const string &sql) { auto r=c.Query(sql); if(r->HasError())throw std::runtime_error(r->GetError()); }
int main() { try {
 DuckDB db(nullptr); Connection con(db); size_t checks=0;
 for (idx_t dim: {idx_t(64),idx_t(1536)}) {
  SQL(con,"CREATE TABLE fetch_test AS SELECT i::BIGINT id, array_resize([i::FLOAT],"+std::to_string(dim)+",i::FLOAT)::FLOAT["+std::to_string(dim)+"] embedding, CASE WHEN i%2=0 THEN NULL ELSE 'body-'||i::VARCHAR END body FROM range(2050)t(i)");
  SQL(con,"CHECKPOINT"); SQL(con,"BEGIN");
  auto &table=Catalog::GetEntry(*con.context,CatalogType::TABLE_ENTRY,INVALID_CATALOG,DEFAULT_SCHEMA,"fetch_test").Cast<DuckTableEntry>();
  vector<ColumnIndex> columns{ColumnIndex(1),ColumnIndex(COLUMN_IDENTIFIER_ROW_ID),ColumnIndex(0),ColumnIndex(2)};
  vector<LogicalType> types{LogicalType::ARRAY(LogicalType::FLOAT,dim),LogicalType::BIGINT,LogicalType::BIGINT,LogicalType::VARCHAR};
  auto check=[&](const vector<row_t> &ids,idx_t limit,idx_t offset,const vector<row_t> &expected,bool checked=false) {
   auto fetched=FetchRowsByRowIds(*con.context,table,columns,types,ids,limit,offset,checked);
   if(fetched->Count()!=expected.size())throw std::runtime_error("count");
   ColumnDataScanState state; fetched->InitializeScan(state); DataChunk chunk; chunk.Initialize(*con.context,types);idx_t seen=0;
   while(true) { chunk.Reset();fetched->Scan(state,chunk);if(!chunk.size())break;
    for(idx_t i=0;i<chunk.size();++i) {auto row=expected.at(seen++);auto id=chunk.GetValue(2,i).GetValue<int64_t>();if(chunk.GetValue(1,i).GetValue<int64_t>()!=row)throw std::runtime_error("rowid/order");
     const auto value=chunk.GetValue(0,i);const auto &coords=ArrayValue::GetChildren(value);if(coords.size()!=dim||coords.front().GetValue<float>()!=float(id)||coords.back().GetValue<float>()!=float(id))throw std::runtime_error("array");
     auto body=chunk.GetValue(3,i);if(body.IsNull()!=(id%2==0)||(!body.IsNull()&&body.ToString()!="body-"+std::to_string(id)))throw std::runtime_error("null/body");
    }
   }
   if(seen!=expected.size())throw std::runtime_error("truncated");++checks;
  };
  for(idx_t count: {idx_t(0),idx_t(1),idx_t(10),idx_t(2048),idx_t(2049)}) {vector<row_t> ids;for(idx_t i=0;i<count;++i)ids.push_back(count-1-i);check(ids,0,0,ids);check(ids,0,0,ids,true);}
  check({11,10,9,8},2,1,{10,9});check({11,10},2,5,{});
  SQL(con,"DELETE FROM fetch_test WHERE id=10");check({11,10,9},0,0,{11,9});
  SQL(con,"INSERT INTO fetch_test VALUES (2050,array_resize([2050::FLOAT],"+std::to_string(dim)+",2050::FLOAT)::FLOAT["+std::to_string(dim)+"],NULL)");
  auto rows=con.Query("SELECT rowid FROM fetch_test WHERE id=2050");if(rows->HasError())throw std::runtime_error(rows->GetError());auto local=rows->GetValue(0,0).GetValue<int64_t>();
  check({11,local,9},0,0,{11,local,9});check({local,11,local},0,0,{local,11,local});
  SQL(con,"ROLLBACK");SQL(con,"DROP TABLE fetch_test");
 }
 std::cout<<"PASS "<<checks<<" fetch capacity/order/visibility checks\n";
} catch(const std::exception &e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
