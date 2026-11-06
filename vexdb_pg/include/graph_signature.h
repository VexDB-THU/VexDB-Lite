#ifndef VEXDB_PG_GRAPH_SIGNATURE_H
#define VEXDB_PG_GRAPH_SIGNATURE_H

#include "graph_index/signature_search.h"
#include "graph_index/graph_index_struct.h"
#include <vtl/disk_container/diskvector.hpp>

// An optional raw L2 graph auxiliary; PQ's qtcode pages are never interpreted
// as signatures. All allocations belong to the caller's PG memory context.
bool graph_signature_supported(GraphIndexMetaPage meta);
enum class GraphSignatureInvalidation : uint32_t { Write = 2, Vacuum = 3, Build = 4 };
void graph_signature_invalidate(Relation index, GraphIndexMetaPage meta,
    GraphSignatureInvalidation reason = GraphSignatureInvalidation::Write);
struct PgGraphSignature {
    const char *state = "missing";
    signature_rescue::Header header;
    BlockNumber root = InvalidBlockNumber;
    float *center = nullptr, *cosines = nullptr;
    int8_t *signs = nullptr;
    uint32_t *order = nullptr;
    bool load(Relation index, GraphIndexMetaPage meta);
    void destroy();
    template<typename Store,typename Dister,typename Candidates>
    size_t complete(Relation index, Store &store, Dister &dister, const char *query,
                  signature_rescue::Observer observer, Candidates &points) {
        const size_t n=header.count,dim=header.dim,padded=header.padded,calls=*observer.calls;
        if (!signature_rescue::WorthScanning(n,dim,padded,calls)) return 0;
        size_t limit=calls*25/1000;
        if (!limit || limit>MaxAllocSize/sizeof(signature_rescue::Nominee)) return 0;
        auto scratch=(float*)palloc(padded*4);uint64_t code[4];float norm;
        bool ok=signature_rescue::Encode(reinterpret_cast<const float*>(query),center,dim,padded,256,
                    signs,order,scratch,code,norm);
        pfree(scratch);if(!ok)return 0;
        auto heap=(signature_rescue::Nominee*)palloc(limit*sizeof(signature_rescue::Nominee));
        signature_rescue::Selector selector{heap,0,limit,observer,code,norm,cosines};
        disk_container::DiskVector<unsigned char> bytes(index,root,false);
        constexpr size_t chunk=1024, stride=signature_rescue::record_bytes;
        auto buffer=(unsigned char*)palloc(chunk*stride);
        const size_t start=sizeof(header)+dim*4;
        for(size_t first=0;first<n;first+=chunk) {
            CHECK_FOR_INTERRUPTS();size_t count=std::min(chunk,n-first);
            bytes.template get_n<disk_container::AccessorLockType::ReadLock>(start+first*stride,count*stride,buffer);
            for(size_t i=0;i<count;++i)selector.offer(uint32_t(first+i),buffer+i*stride);
        }
        bytes.destroy();pfree(buffer); // release every code-page pin before vector I/O
        for(size_t i=0;i<selector.count;++i) {
            CHECK_FOR_INTERRUPTS();auto id=heap[i].id;
            float distance=store.get_distance(dister,query,id);
            points.emplace_back(id,id,uint32_t(INVALID_VECTOR_ID),distance,nullptr);
        }
        const size_t refined=selector.count;
        pfree(heap);
        std::sort(points.begin(),points.end(),[](const auto&a,const auto&b){return a.dist<b.dist;});
        return refined;
    }
};
#endif
