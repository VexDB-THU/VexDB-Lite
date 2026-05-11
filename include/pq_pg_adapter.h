// PG↔shared quantizer bridge: palloc allocator + ereport rethrow boundary.
#ifndef PQ_PG_ADAPTER_H
#define PQ_PG_ADAPTER_H

#include "pg_compat.h"
#include "quantizer/pq_alloc.h"

namespace vex_pg {

inline void *PgAllocCb(size_t bytes, void * /*user*/) {
    return palloc(bytes);
}

// Huge-allowed allocation. Lets the shared PQ codebook exceed PG's default
// 1GB AllocSet limit (e.g. d=1024, M=64, ksub=256). Falls through pfree on
// release — palloc_extended-allocated chunks are pfree-compatible.
inline void *PgAllocHugeCb(size_t bytes, void * /*user*/) {
    return palloc_extended(bytes, MCXT_ALLOC_HUGE);
}

inline void PgFreeCb(void *p, void * /*user*/) {
    if (p != nullptr) {
        pfree(p);
    }
}

inline ::vex::quantizer::PQAllocator PgPQAllocator() {
    ::vex::quantizer::PQAllocator a;
    a.alloc_fn      = PgAllocCb;
    a.alloc_huge_fn = PgAllocHugeCb;
    a.free_fn       = PgFreeCb;
    a.user          = nullptr;
    return a;
}

// PG's ereport(ERROR) longjmps out of the lambda; allocator state held in
// CurrentMemoryContext is reclaimed by the abort cleanup, so no manual
// rollback is needed in the rethrow path.
template <class Body>
inline void PgQuantizerCall(Body &&body) {
    try {
        body();
    } catch (const ::vex::quantizer::VexQuantizerError &e) {
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("PQ quantizer: %s", e.what())));
    }
}

} // namespace vex_pg

#endif
