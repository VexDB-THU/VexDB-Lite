// Test-only native graph candidate study. Not registered by the extension.
// Reads a frozen graph under a relation lock. Batch setup and algorithm CPU
// are separate diagnostics; neither is a product SQL throughput measurement.
#include "graph_topology_export.cpp"
#include "utils/array.h"
#include "utils/lsyscache.h"
#include <ctime>
#include <limits>

extern "C" {
PG_FUNCTION_INFO_V1(test_graph_completion_batch);
Datum test_graph_completion_batch(PG_FUNCTION_ARGS);
}

struct CompletionLess {
    bool operator()(const Pair<float,uint32> &a,const Pair<float,uint32> &b) const {
        return a.first<b.first || (a.first==b.first && a.second<b.second);
    }
};
using CompletionHeap=PriorityQueue<Pair<float,uint32>,CompletionLess>;

struct CompletionDiskStore : DiskStore<uint32> {
    using Base=DiskStore<uint32>;
    using Base::get_distance;
    float *scores;
    uint8 *expanded;
    size_t calls=0;
    CompletionDiskStore(Relation index,Relation heap,Buffer meta,size_t n)
        : Base(index,heap,meta,false) {
        scores=(float*)palloc(sizeof(float)*n);expanded=(uint8*)palloc(n);
    }
    template<typename D> float get_distance(const D &d,const char *q,uint32 id) {
        ++calls;float value=Base::get_distance(d,q,id);scores[id]=value;return value;
    }
    template<typename D,typename IDs> void get_distance_batch(const D &d,const char *q,const IDs &ids,float *out) {
        calls+=ids.size();Base::get_distance_batch(d,q,ids,out);
        for(size_t i=0;i<ids.size();++i)scores[ids[i]]=out[i];
    }
    template<bool base> auto get_point_info(uint32 id) {
        if constexpr(base)expanded[id]=1;
        return Base::template get_point_info<base>(id);
    }
    void finish() {pfree(scores);pfree(expanded);Base::destroy();}
};
static double CompletionCpuMs() {
    timespec t{};clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);
    return t.tv_sec*1000.0+t.tv_nsec/1e6;
}

Datum test_graph_completion_batch(PG_FUNCTION_ARGS) {
    Relation index=index_open(PG_GETARG_OID(0),ShareLock);
    Relation heap=table_open(index->rd_index->indrelid,AccessShareLock);
    Buffer meta=ReadBuffer(index,GRAPH_INDEX_METAPAGE_BLKNO);
    auto metap=GRAPH_INDEX_PAGE_GET_META(BufferGetPage(meta));
    if(metap->id_type!=IdType::U32 || metap->quantizer_metainfo.get_type()!=QuantizerType::NONE ||
       metap->precision_type!=DistPrecisionType::FLOAT || metap->metric!=Metric::L2)
        elog(ERROR,"completion study requires a U32 float L2 original graph");
    const uint32 ef=PG_GETARG_INT32(2),pct=PG_GETARG_INT32(3),rank=PG_GETARG_INT32(4);
    const float gamma=PG_GETARG_FLOAT4(5);
    if(ef<1||ef>65536||pct>10||rank<1||rank>ef||!std::isfinite(gamma)||gamma<0||gamma>1)
        elog(ERROR,"invalid completion study options");
    const size_t n=metap->num_vectors,degree=size_t(metap->m)*2;
    if(!n||n>=uint32(INVALID_VECTOR_ID)||n>MaxAllocSize/(degree*sizeof(uint32)))
        elog(ERROR,"completion study graph exceeds bounded allocation");
    ArrayType *array=PG_GETARG_ARRAYTYPE_P(1);Datum *queries;bool *nulls;int nq;
    int16 typlen;bool typbyval;char typalign;
    get_typlenbyvalalign(ARR_ELEMTYPE(array),&typlen,&typbyval,&typalign);
    deconstruct_array(array,ARR_ELEMTYPE(array),typlen,typbyval,typalign,&queries,&nulls,&nq);
    if(nq<1||nq>1024)elog(ERROR,"completion study needs1..1024 queries");
    CompletionDiskStore store(index,heap,meta,n);
    elog(NOTICE,"completion setup begin n=%zu",n);
    const double init_start=CompletionCpuMs();
    auto outgoing=(uint32*)palloc(n*degree*sizeof(uint32));
    std::fill(outgoing,outgoing+n*degree,uint32(INVALID_VECTOR_ID));
    auto offsets=(size_t*)palloc0((n+1)*sizeof(size_t));
    Vector<GraphIndexCandidate<uint32>> neighbors;
    for(uint32 id=0;id<n;++id) {
        CHECK_FOR_INTERRUPTS();neighbors.clear();
        GraphIndexCandidate<uint32> point(id,id,uint32(INVALID_VECTOR_ID),0,nullptr);
        store.template lock_point<true,true>(id);
        store.template get_neighbors<true>(neighbors,point);
        if(neighbors.size()>degree)elog(ERROR,"invalid graph degree");
        for(size_t j=0;j<neighbors.size();++j) {
            uint32 target=neighbors[j].id;if(target>=n)elog(ERROR,"graph changed during study");
            outgoing[id*degree+j]=target;++offsets[target+1];
        }
        store.template unlock_point<true,true>(id);
    }
    neighbors.destroy();
    for(size_t i=0;i<n;++i)offsets[i+1]+=offsets[i];
    auto reverse=(uint32*)palloc(offsets[n]*sizeof(uint32));
    auto cursor=(size_t*)palloc(n*sizeof(size_t));memcpy(cursor,offsets,n*sizeof(size_t));
    for(uint32 id=0;id<n;++id)for(size_t j=0;j<degree;++j) {
        uint32 target=outgoing[id*degree+j];if(target==uint32(INVALID_VECTOR_ID))break;
        reverse[cursor[target]++]=id;
    }
    pfree(cursor);
    const double init_ms=CompletionCpuMs()-init_start;
    elog(NOTICE,"completion setup done cpu_ms=%.3f",init_ms);
    const size_t graph_bytes=(n*degree+offsets[n])*sizeof(uint32)+(n+1)*sizeof(size_t);
    StringInfoData out;initStringInfo(&out);
    appendStringInfoString(&out,"query,variant,base_calls,total_calls,extra_reads,peak_pending,limit_hit,base_cpu_ms,completion_cpu_ms,init_cpu_ms,graph_bytes,ids\n");
    DispatchRunner<true,MetricList<Metric::L2>,DistPrecisionTypeList<DistPrecisionType::FLOAT>,DispatcherMode::DEFAULT>::call(metap,[&](auto &distancer) {
        for(int qi=0;qi<nq;++qi) {
            elog(NOTICE,"completion query=%d begin",qi);
            CHECK_FOR_INTERRUPTS();if(nulls[qi])elog(ERROR,"null study query");
            auto query=(FloatVector*)PG_DETOAST_DATUM(queries[qi]);
            if(query->dim!=metap->dimension)elog(ERROR,"completion dimension mismatch");
            std::fill(store.scores,store.scores+n,std::numeric_limits<float>::quiet_NaN());
            memset(store.expanded,0,n);store.calls=0;
            distancer.prepare(index,metap);distancer.process((char*)query->x);
            const double start=CompletionCpuMs();
            GraphIndexAlgorithm algo(metap,store,distancer);
            auto points=algo.search_internal(store.template get_entry<false>().first,(char*)query->x,ef);
            const size_t baseline_calls=store.calls,original_size=points.size();
            const double baseline_ms=CompletionCpuMs()-start;
            elog(NOTICE,"completion query=%d baseline cpu_ms=%.3f calls=%zu",qi,baseline_ms,baseline_calls);
            size_t reads=0,peak=0;bool limit_hit=false;
            auto emit=[&](const char *variant,double completion_ms) {
                appendStringInfo(&out,"%d,%s,%zu,%zu,%zu,%zu,%d,%.6f,%.6f,%.6f,%zu,",qi,variant,baseline_calls,store.calls,reads,peak,int(limit_hit),baseline_ms,completion_ms,init_ms,graph_bytes);
                for(auto p:points)appendStringInfo(&out,"%u ",p.id);
                appendStringInfoChar(&out,'\n');
            };
            emit("baseline",0);
            const double extra_start=CompletionCpuMs();
            const float bound=points.back().dist;
            auto included=(uint8*)palloc0(n);auto enqueued=(uint8*)palloc(n);
            for(auto p:points)included[p.id]=1;
            auto add=[&](float distance,uint32 id) {
                if(distance<=bound&&!included[id]) {
                    included[id]=1;points.emplace_back(id,id,uint32(INVALID_VECTOR_ID),distance,nullptr);
                }
            };
            { // Forward continuation from already scored, unexpanded nodes.
                CompletionHeap pending;memcpy(enqueued,store.expanded,n);
                const float frontier=std::max(bound,(1+gamma)*(1+gamma)*points[rank-1].dist);
                for(uint32 id=0;id<n;++id)if(!enqueued[id]&&!std::isnan(store.scores[id])&&store.scores[id]<=frontier) {
                    pending.emplace(store.scores[id],id);enqueued[id]=1;add(store.scores[id],id);
                }
                const size_t start_calls=store.calls,limit=baseline_calls*pct/100;
                size_t expansions=0;
                while(!pending.empty()&&expansions<ef) {
                    CHECK_FOR_INTERRUPTS();auto point=pending.top();pending.pop();++expansions;++reads;bool stop=false;
                    for(size_t j=0;j<degree;++j) {
                        uint32 id=outgoing[point.second*degree+j];if(id==uint32(INVALID_VECTOR_ID))break;
                        if(enqueued[id])continue;enqueued[id]=1;float distance=store.scores[id];
                        if(std::isnan(distance)) {
                            if(store.calls-start_calls>=limit){limit_hit=true;stop=true;break;}
                            distance=store.get_distance(distancer,(char*)query->x,id);
                        }
                        if(distance<=frontier){pending.emplace(distance,id);add(distance,id);}
                    }
                    peak=std::max(peak,pending.size());if(stop)break;
                }
                pending.destroy();
            }
            { // Reverse completion with the remaining common distance budget.
                CompletionHeap pending;memset(enqueued,0,n);
                for(size_t i=0;i<std::min<size_t>(metap->m,original_size);++i) {
                    pending.emplace(points[i].dist,points[i].id);enqueued[points[i].id]=1;
                }
                const size_t start_calls=store.calls,limit=baseline_calls*(10-pct)/100;
                size_t expansions=0;
                while(limit&&!pending.empty()&&expansions<ef) {
                    CHECK_FOR_INTERRUPTS();auto point=pending.top();pending.pop();++expansions;++reads;bool stop=false;
                    auto neighbor=[&](uint32 id) {
                        if(enqueued[id])return;enqueued[id]=1;float distance=store.scores[id];
                        if(std::isnan(distance)) {
                            if(store.calls-start_calls>=limit){limit_hit=true;stop=true;return;}
                            distance=store.get_distance(distancer,(char*)query->x,id);
                        }
                        if(distance<=bound){pending.emplace(distance,id);add(distance,id);}
                    };
                    for(size_t j=offsets[point.second];j<offsets[point.second+1];++j){neighbor(reverse[j]);if(stop)break;}
                    if(!stop)for(size_t j=0;j<degree;++j) {
                        uint32 id=outgoing[point.second*degree+j];if(id==uint32(INVALID_VECTOR_ID))break;
                        neighbor(id);if(stop)break;
                    }
                    peak=std::max(peak,pending.size());if(stop)break;
                }
                pending.destroy();
            }
            if(store.calls>baseline_calls+baseline_calls/10)elog(ERROR,"completion exceeded its distance budget");
            std::sort(points.begin(),points.end(),[](const auto&a,const auto&b){return a.dist<b.dist;});
            emit("frozen-completion",CompletionCpuMs()-extra_start);
            elog(NOTICE,"completion query=%d done extra_cpu_ms=%.3f calls=%zu",qi,CompletionCpuMs()-extra_start,store.calls);
            pfree(enqueued);pfree(included);points.destroy();algo.destroy();
            if((Pointer)query!=DatumGetPointer(queries[qi]))pfree(query);
        }
    });
    pfree(outgoing);pfree(offsets);pfree(reverse);store.finish();ReleaseBuffer(meta);
    table_close(heap,AccessShareLock);index_close(index,ShareLock);
    text *result=cstring_to_text_with_len(out.data,out.len);pfree(out.data);
    pfree(queries);pfree(nulls);PG_FREE_IF_COPY(array,1);
    PG_RETURN_TEXT_P(result);
}
