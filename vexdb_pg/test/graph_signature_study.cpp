// Disposable native diagnostic. No product registration or cross-query cache.
#include "graph_topology_export.cpp"
#include "../../tests/eval/graph_search/signature_projection.h"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include <ctime>
#include <limits>
extern "C" {
PG_FUNCTION_INFO_V1(test_graph_signature_batch);
Datum test_graph_signature_batch(PG_FUNCTION_ARGS);
}
static double SignatureCpuMs(){timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return t.tv_sec*1000.+t.tv_nsec/1e6;}
struct SignatureStore : DiskStore<uint32> {
    using Base=DiskStore<uint32>;using Base::get_distance;
    float *scores;size_t calls=0;
    SignatureStore(Relation index,Relation heap,Buffer meta,size_t n):Base(index,heap,meta,false){scores=(float*)palloc(n*4);}
    template<typename D> float get_distance(const D&d,const char*q,uint32 id){++calls;float v=Base::get_distance(d,q,id);scores[id]=v;return v;}
    template<typename D,typename IDs>void get_distance_batch(const D&d,const char*q,const IDs&ids,float*out){calls+=ids.size();Base::get_distance_batch(d,q,ids,out);for(size_t i=0;i<ids.size();++i)scores[ids[i]]=out[i];}
    void finish(){pfree(scores);Base::destroy();}
};
struct SignatureWorst {
    bool operator()(const Pair<float,uint32>&a,const Pair<float,uint32>&b)const{return a.first>b.first||(a.first==b.first&&a.second>b.second);}
};
Datum test_graph_signature_batch(PG_FUNCTION_ARGS) {
    Relation index=index_open(PG_GETARG_OID(0),ShareLock);
    Relation heap=table_open(index->rd_index->indrelid,AccessShareLock);
    Buffer meta=ReadBuffer(index,GRAPH_INDEX_METAPAGE_BLKNO);auto mp=GRAPH_INDEX_PAGE_GET_META(BufferGetPage(meta));
    if(mp->use_cluster()||mp->id_type!=IdType::U32||mp->metric!=Metric::L2||mp->precision_type!=DistPrecisionType::FLOAT||mp->quantizer_metainfo.get_type()!=QuantizerType::NONE)elog(ERROR,"signature study requires U32 FLOAT L2 raw graph");
    const uint32 ef=PG_GETARG_INT32(2),bits=PG_GETARG_INT32(3),per_mille=PG_GETARG_INT32(4);const uint64 seed=PG_GETARG_INT64(5);
    if(ef<1||ef>65536||(bits!=256&&bits!=512)||per_mille>100)elog(ERROR,"invalid signature options");
    const size_t n=mp->num_vectors,dim=mp->dimension,padded=signature_rescue::Padded(dim,512),words=bits/64;
    if(!n||n>=uint32(INVALID_VECTOR_ID)||n>MaxAllocSize/(words*8+4))elog(ERROR,"signature study exceeds bounded allocation");
    SignatureStore store(index,heap,meta,n);const double setup_start=SignatureCpuMs();
    auto center=(float*)palloc(dim*4);auto sum=(double*)palloc0(dim*sizeof(double));
    auto signs=(int8_t*)palloc(3*padded);auto order=(uint32*)palloc(padded*4);auto scratch=(float*)palloc(padded*4);auto vec=(float*)palloc(dim*4);
    auto norms=(float*)palloc(n*4);auto codes=(uint64_t*)palloc(n*words*8);auto lut=(float*)palloc((bits+1)*4);
    signature_rescue::Initialize(seed,padded,signs,order);signature_rescue::Cosines(bits,lut);
    for(uint32 id=0;id<n;++id){CHECK_FOR_INTERRUPTS();auto buffer=store.read_data(id);auto data=(float*)buffer.get_vecbuf();for(size_t d=0;d<dim;++d)sum[d]+=data[d];buffer.release();}
    for(size_t d=0;d<dim;++d)center[d]=float(sum[d]/n);
    for(uint32 id=0;id<n;++id){CHECK_FOR_INTERRUPTS();auto buffer=store.read_data(id);memcpy(vec,buffer.get_vecbuf(),dim*4);buffer.release();if(!signature_rescue::Encode(vec,center,dim,padded,bits,signs,order,scratch,codes+size_t(id)*words,norms[id]))elog(ERROR,"nonfinite signature vector");}
    uint64 fingerprint=1469598103934665603ULL;for(size_t i=0;i<n*words;++i){fingerprint^=codes[i];fingerprint*=1099511628211ULL;}
    const double setup_ms=SignatureCpuMs()-setup_start;const size_t model_bytes=n*(bits/8+4)+dim*4+padded*7+48;
    ArrayType *array=PG_GETARG_ARRAYTYPE_P(1);Datum *queries;bool *nulls;int nq;int16 typlen;bool byval;char align;
    get_typlenbyvalalign(ARR_ELEMTYPE(array),&typlen,&byval,&align);deconstruct_array(array,ARR_ELEMTYPE(array),typlen,byval,align,&queries,&nulls,&nq);
    if(nq<1||nq>128)elog(ERROR,"signature study needs1..128 queries");
    StringInfoData out;initStringInfo(&out);
    appendStringInfoString(&out,"query,variant,seed,bits,per_mille,enabled,base_calls,total_calls,code_examined,base_cpu_ms,aux_cpu_ms,init_cpu_ms,model_bytes,model_fingerprint,ids\n");
    DispatchRunner<true,MetricList<Metric::L2>,DistPrecisionTypeList<DistPrecisionType::FLOAT>,DispatcherMode::DEFAULT>::call(mp,[&](auto&distancer){
        for(int qi=0;qi<nq;++qi){
            CHECK_FOR_INTERRUPTS();if(nulls[qi])elog(ERROR,"null signature query");auto query=(FloatVector*)PG_DETOAST_DATUM(queries[qi]);if(query->dim!=dim)elog(ERROR,"signature dimension mismatch");
            std::fill(store.scores,store.scores+n,std::numeric_limits<float>::quiet_NaN());store.calls=0;
            distancer.prepare(index,mp);distancer.process((char*)query->x);const double start=SignatureCpuMs();
            GraphIndexAlgorithm algo(mp,store,distancer);auto points=algo.search_internal(store.template get_entry<false>().first,(char*)query->x,ef);
            const double base_ms=SignatureCpuMs()-start;const size_t base_calls=store.calls;
            const bool enabled=double(n)*(bits/8+4)+padded*4<=.10*base_calls*dim*4;size_t examined=0;
            auto emit=[&](const char*variant,double aux){
                appendStringInfo(&out,"%d,%s,%llu,%u,%u,%d,%zu,%zu,%zu,%.7f,%.7f,%.7f,%zu,%llu,",qi,variant,(unsigned long long)seed,bits,per_mille,int(enabled),base_calls,store.calls,examined,base_ms,aux,setup_ms,model_bytes,(unsigned long long)fingerprint);
                for(auto p:points)appendStringInfo(&out,"%u ",p.id);appendStringInfoChar(&out,'\n');
            };
            emit("baseline",0);const double aux_start=SignatureCpuMs();
            if(enabled){
                uint64_t qcode[8];float qnorm;
                if(!signature_rescue::Encode(query->x,center,dim,padded,bits,signs,order,scratch,qcode,qnorm))elog(ERROR,"nonfinite signature query");
                const size_t limit=base_calls*per_mille/1000;PriorityQueue<Pair<float,uint32>,SignatureWorst> nearest(limit+1);
                if(limit)for(uint32 id=0;id<n;++id){
                    if((id&1023)==0)CHECK_FOR_INTERRUPTS();if(!std::isnan(store.scores[id]))continue;++examined;
                    auto distance=signature_rescue::Hamming(qcode,codes+size_t(id)*words,words);float estimate=signature_rescue::Estimate(qnorm,norms[id],distance,lut);
                    if(nearest.size()<limit)nearest.emplace(estimate,id);else if(estimate<nearest.top().first||(estimate==nearest.top().first&&id<nearest.top().second)){nearest.pop();nearest.emplace(estimate,id);}
                }
                while(!nearest.empty()){uint32 id=nearest.top().second;nearest.pop();float distance=store.get_distance(distancer,(char*)query->x,id);points.emplace_back(id,id,uint32(INVALID_VECTOR_ID),distance,nullptr);}
                nearest.destroy();if(store.calls>base_calls+limit)elog(ERROR,"signature exceeded exact distance budget");
                std::sort(points.begin(),points.end(),[](const auto&a,const auto&b){return a.dist<b.dist;});
            }
            emit("signature",SignatureCpuMs()-aux_start);points.destroy();algo.destroy();
            if((Pointer)query!=DatumGetPointer(queries[qi]))pfree(query);
        }
    });
    pfree(center);pfree(sum);pfree(signs);pfree(order);pfree(scratch);pfree(vec);pfree(norms);pfree(codes);pfree(lut);pfree(queries);pfree(nulls);store.finish();ReleaseBuffer(meta);
    table_close(heap,AccessShareLock);index_close(index,ShareLock);text*result=cstring_to_text_with_len(out.data,out.len);pfree(out.data);PG_FREE_IF_COPY(array,1);PG_RETURN_TEXT_P(result);
}
