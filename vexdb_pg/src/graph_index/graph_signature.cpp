#include "pg_compat.h"
#include "graph_signature.h"
#include "graph_index/graph_index_storage.h"
extern "C" {
#include "commands/defrem.h"
#include "utils/acl.h"
#include "catalog/pg_class.h"
#include "utils/builtins.h"
}

using SignatureBytes=disk_container::DiskVector<unsigned char>;
using signature_rescue::Header;

bool graph_signature_supported(GraphIndexMetaPage m) {
    return !m->use_cluster() && m->id_type==IdType::U32 && m->metric==Metric::L2 &&
           m->precision_type==DistPrecisionType::FLOAT &&
           m->quantizer_metainfo.get_type()==QuantizerType::NONE;
}
bool PgGraphSignature::load(Relation index,GraphIndexMetaPage meta) {
    state="unsupported";
    if(!graph_signature_supported(meta))return false;
    state="missing";
    if(!BlockNumberIsValid(meta->qtcode_block))return false;
    state="corrupt";
    root=meta->qtcode_block;SignatureBytes bytes(index,root,false);
    if(bytes.size()<sizeof(header)){bytes.destroy();return false;}
    bytes.get_n<disk_container::AccessorLockType::ReadLock>(0,sizeof(header),(unsigned char*)&header);
    if(header.magic==Header{}.magic && header.version==1 && header.valid!=1) {
        switch(header.valid) {
        case 2:state="write_invalid";break;
        case 3:state="vacuum_invalid";break;
        case 4:state="building_or_interrupted";break;
        default:state="invalid";break;
        }
        bytes.destroy();return false;
    }
    const uint64_t size=sizeof(header)+uint64_t(meta->dimension)*4+header.count*signature_rescue::record_bytes;
    if(!signature_rescue::ValidHeader(header,meta->dimension,meta->num_vectors) || size>bytes.size()) {
        bytes.destroy();return false;
    }
    center=(float*)palloc(header.dim*4);
    bytes.get_n<disk_container::AccessorLockType::ReadLock>(sizeof(header),header.dim*4,(unsigned char*)center);bytes.destroy();
    signs=(int8_t*)palloc(3*header.padded);order=(uint32_t*)palloc(header.padded*4);
    cosines=(float*)palloc(257*4);
    signature_rescue::Initialize(header.seed,header.padded,signs,order);
    signature_rescue::Cosines(256,cosines);state="ready";return true;
}
void PgGraphSignature::destroy() {
    if(center)pfree(center);if(signs)pfree(signs);if(order)pfree(order);if(cosines)pfree(cosines);
    center=nullptr;signs=nullptr;order=nullptr;cosines=nullptr;
}
// A first version deliberately invalidates on any insertion/vacuum, including
// aborted writes. Stale or recycled node codes must never be used. Rebuild is
// explicit; an enabled rescue query uses exact search until then.
void graph_signature_invalidate(Relation index,GraphIndexMetaPage meta,GraphSignatureInvalidation reason) {
    if(!graph_signature_supported(meta) || !BlockNumberIsValid(meta->qtcode_block))return;
    SignatureBytes bytes(index,meta->qtcode_block,true);Header h;
    if(bytes.size()>=sizeof(h)) {
        bytes.get_n<disk_container::AccessorLockType::ReadLock>(0,sizeof(h),(unsigned char*)&h);
        if(h.magic==Header{}.magic && h.valid) {
            uint32_t invalid=uint32_t(reason);
            bytes.set_n<disk_container::AccessorLockType::WriteLock>(offsetof(Header,valid),sizeof(invalid),(unsigned char*)&invalid);
        }
    }
    bytes.destroy();
}

extern "C" {
PG_FUNCTION_INFO_V1(vexdb_build_signature);
PG_FUNCTION_INFO_V1(vexdb_signature_status);
Datum vexdb_build_signature(PG_FUNCTION_ARGS);
Datum vexdb_signature_status(PG_FUNCTION_ARGS);
}
Datum vexdb_signature_status(PG_FUNCTION_ARGS) {
    Relation index=index_open(PG_GETARG_OID(0),AccessShareLock);
    if(index->rd_rel->relam!=get_am_oid("vexdb_graph",false))elog(ERROR,"signature requires a vexdb_graph index");
    if(!object_ownercheck(RelationRelationId,index->rd_index->indrelid,GetUserId()))
        ereport(ERROR,(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),errmsg("must own the indexed table")));
    Buffer buffer=ReadBuffer(index,GRAPH_INDEX_METAPAGE_BLKNO);
    auto meta=GRAPH_INDEX_PAGE_GET_META(BufferGetPage(buffer));
    PgGraphSignature model;model.load(index,meta);
    text *result=cstring_to_text(model.state);model.destroy();
    ReleaseBuffer(buffer);index_close(index,AccessShareLock);PG_RETURN_TEXT_P(result);
}
Datum vexdb_build_signature(PG_FUNCTION_ARGS) {
    PreventCommandIfReadOnly("vexdb_build_signature");
    PreventCommandDuringRecovery("vexdb_build_signature");
    PreventCommandIfParallelMode("vexdb_build_signature");
    Oid oid=PG_GETARG_OID(0);
    // Lock heap first, like CREATE INDEX: block writers while reading its index.
    Relation probe=index_open(oid,AccessShareLock);
    if(probe->rd_rel->relam!=get_am_oid("vexdb_graph",false))elog(ERROR,"signature requires a vexdb_graph index");
    Oid heapoid=probe->rd_index->indrelid;index_close(probe,AccessShareLock);
    if(!object_ownercheck(RelationRelationId,heapoid,GetUserId()))
        ereport(ERROR,(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),errmsg("must own the indexed table")));
    Relation heap=table_open(heapoid,ShareLock);
    Relation index=index_open(oid,AccessExclusiveLock);
    Buffer meta=ReadBuffer(index,GRAPH_INDEX_METAPAGE_BLKNO);
    auto m=GRAPH_INDEX_PAGE_GET_META(BufferGetPage(meta));
    if(graph_index_get_enable_async_insert(index))
        elog(ERROR,"signature build requires synchronous graph insertion");
    if(!graph_signature_supported(m) || !m->num_vectors || m->num_vectors>=uint32(INVALID_VECTOR_ID))
        elog(ERROR,"signature requires a nonempty raw FLOAT L2 graph with 32-bit IDs");
    Header h;h.dim=m->dimension;h.padded=signature_rescue::Padded(h.dim,512);
    h.seed=uint64_t(PG_GETARG_INT64(1));h.count=m->num_vectors;h.valid=4;
    graph_signature_invalidate(index,m,GraphSignatureInvalidation::Build);
    BlockNumber root=m->qtcode_block;
    if(!BlockNumberIsValid(root))root=SignatureBytes::get_disk_vector(index,true);
    SignatureBytes bytes(index,root,true);
    const size_t offset=sizeof(h)+size_t(h.dim)*4,total=offset+h.count*signature_rescue::record_bytes;
    bytes.extend(total);
    bytes.set_n<disk_container::AccessorLockType::WriteLock>(0,sizeof(h),(unsigned char*)&h); // invalid until finished
    auto sum=(double*)palloc0(h.dim*sizeof(double));auto center=(float*)palloc(h.dim*4);
    auto signs=(int8_t*)palloc(3*h.padded);auto order=(uint32_t*)palloc(h.padded*4);
    auto scratch=(float*)palloc(h.padded*4);auto vec=(float*)palloc(h.dim*4);
    signature_rescue::Initialize(h.seed,h.padded,signs,order);
    DiskStore<uint32> store(index,heap,meta,false);
    for(uint32 id=0;id<h.count;++id) {
        CHECK_FOR_INTERRUPTS();auto b=store.read_data(id);auto v=(float*)b.get_vecbuf();
        for(size_t d=0;d<h.dim;++d)sum[d]+=v[d];b.release();
    }
    for(size_t d=0;d<h.dim;++d)center[d]=float(sum[d]/h.count);
    bytes.set_n<disk_container::AccessorLockType::WriteLock>(sizeof(h),h.dim*4,(unsigned char*)center);
    constexpr size_t chunk=1024,stride=signature_rescue::record_bytes;
    auto buffer=(unsigned char*)palloc(chunk*stride);
    for(size_t first=0;first<h.count;first+=chunk) {
        size_t count=std::min(chunk,size_t(h.count)-first);
        for(size_t i=0;i<count;++i) {
            CHECK_FOR_INTERRUPTS();auto b=store.read_data(uint32(first+i));
            memcpy(vec,b.get_vecbuf(),h.dim*4);b.release();uint64_t code[4];float norm;
            if(!signature_rescue::Encode(vec,center,h.dim,h.padded,256,signs,order,scratch,code,norm))
                elog(ERROR,"signature requires finite vectors");
            memcpy(buffer+i*stride,&norm,4);memcpy(buffer+i*stride+4,code,32);
        }
        bytes.set_n<disk_container::AccessorLockType::WriteLock>(offset+first*stride,count*stride,buffer);
    }
    store.destroy();h.valid=1;
    bytes.set_n<disk_container::AccessorLockType::WriteLock>(0,sizeof(h),(unsigned char*)&h);bytes.destroy();
    // Publish the root with generic WAL while holding the metapage lock.
    LockBuffer(meta,BUFFER_LOCK_EXCLUSIVE);
    auto wal=GenericXLogStart(index);auto page=GenericXLogRegisterBuffer(wal,meta,0);
    GRAPH_INDEX_PAGE_GET_META(page)->qtcode_block=root;GenericXLogFinish(wal);
    LockBuffer(meta,BUFFER_LOCK_UNLOCK);ReleaseBuffer(meta);
    pfree(sum);pfree(center);pfree(signs);pfree(order);pfree(scratch);pfree(vec);pfree(buffer);
    index_close(index,NoLock);table_close(heap,NoLock); // keep exclusion until commit
    PG_RETURN_INT64(total);
}
