// Private lifecycle/audit helpers, never registered by the product extension.
#include "pg_compat.h"
#include "graph_signature.h"
#include "utils/builtins.h"
extern "C" {
PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(test_graph_signature_ready);
PG_FUNCTION_INFO_V1(test_graph_signature_dump);
Datum test_graph_signature_ready(PG_FUNCTION_ARGS);
Datum test_graph_signature_dump(PG_FUNCTION_ARGS);
}
Datum test_graph_signature_ready(PG_FUNCTION_ARGS) {
    Relation index=index_open(PG_GETARG_OID(0),AccessShareLock);
    Buffer buffer=ReadBuffer(index,GRAPH_INDEX_METAPAGE_BLKNO);
    auto meta=GRAPH_INDEX_PAGE_GET_META(BufferGetPage(buffer));
    PgGraphSignature model;bool ready=model.load(index,meta);model.destroy();
    ReleaseBuffer(buffer);index_close(index,AccessShareLock);PG_RETURN_BOOL(ready);
}
Datum test_graph_signature_dump(PG_FUNCTION_ARGS) {
    Relation index=index_open(PG_GETARG_OID(0),AccessShareLock);
    Buffer buffer=ReadBuffer(index,GRAPH_INDEX_METAPAGE_BLKNO);
    auto meta=GRAPH_INDEX_PAGE_GET_META(BufferGetPage(buffer));
    if(!graph_signature_supported(meta)||!BlockNumberIsValid(meta->qtcode_block))elog(ERROR,"no signature model");
    disk_container::DiskVector<unsigned char> bytes(index,meta->qtcode_block,false);
    size_t size=bytes.size();if(size>MaxAllocSize-VARHDRSZ)elog(ERROR,"signature dump too large");
    bytea *out=(bytea*)palloc(size+VARHDRSZ);SET_VARSIZE(out,size+VARHDRSZ);
    bytes.get_n<disk_container::AccessorLockType::ReadLock>(0,size,(unsigned char*)VARDATA(out));
    bytes.destroy();ReleaseBuffer(buffer);index_close(index,AccessShareLock);PG_RETURN_BYTEA_P(out);
}
