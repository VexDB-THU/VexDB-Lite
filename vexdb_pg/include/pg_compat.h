#ifndef PG_COMPAT_H
#define PG_COMPAT_H

#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cfloat>

#ifdef __cplusplus
extern "C" {
#endif

/* Core PostgreSQL headers */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"

/* Storage */
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lmgr.h"
#include "storage/itemptr.h"
#include "storage/smgr.h"
#include "storage/fd.h"
#include "storage/block.h"
#include "storage/lwlock.h"
#include "storage/ipc.h"
#include "storage/md.h"
#include "storage/proc.h"
#if PG_VERSION_NUM < 170000
#include "storage/backendid.h"
#else
#include "storage/procnumber.h"
#endif
#include "storage/freespace.h"
#include "storage/latch.h"

/* Memory */
#include "utils/palloc.h"
#include "utils/memutils.h"

/* Access methods */
#include "access/amapi.h"
#include "access/heapam.h"
#include "access/genam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/heaptoast.h"
#include "access/detoast.h"
#include "access/reloptions.h"
#include "access/hash.h"
#include "access/tableam.h"

/* Catalog */
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "catalog/pg_class.h"
#include "catalog/storage_xlog.h"

/* Utils */
#include "utils/relcache.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/spccache.h"
#include "utils/selfuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/numeric.h"
#include "utils/guc.h"
#include "utils/syscache.h"
#include "utils/sortsupport.h"
#include "utils/fmgroids.h"
#include "utils/ps_status.h"
#include "utils/wait_event.h"

/* FuncAPI */
#include "funcapi.h"

/* Commands */
#include "commands/vacuum.h"
#include "commands/tablespace.h"

/* Libpq */
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"

/* Parser */
#include "parser/parse_type.h"
#include "parser/scansup.h"

/* Common */
#include "common/relpath.h"

/* Port */
#include "port.h"

/* Statistics */
#include "pgstat.h"

/* PG18 compatibility shims */
#if PG_VERSION_NUM < 190000
#define PageSetChecksum(page, blkno) PageSetChecksumInplace((page), (blkno))
#endif

/* PG19 merged init_size and max_size into a single nelems parameter */
#if PG_VERSION_NUM >= 190000
#define VexShmemInitHash(name, nelems, infoP, flags) \
    ShmemInitHash(name, nelems, infoP, flags)
#else
#define VexShmemInitHash(name, nelems, infoP, flags) \
    ShmemInitHash(name, nelems, nelems, infoP, flags)
#endif

#ifndef TupleDescFinalize
#define TupleDescFinalize(tupdesc) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#if PG_VERSION_NUM < 170000
typedef BackendId ProcNumber;
#define INVALID_PROC_NUMBER InvalidBackendId
#endif

/* The index does not need a visibility map, so its standard vector pages use
 * the otherwise-unused VM fork. */
#define VECTOR_FORKNUM VISIBILITYMAP_FORKNUM

/* WAL resource manager - extensions can't register custom ones */
#define RM_GRAPH_INDEX_ID RM_XLOG_ID

/* Reloption kind - will be initialized at runtime */
extern "C" relopt_kind vexdb_lite_relopt_kind;
#define RELOPT_KIND_GRAPH_INDEX vexdb_lite_relopt_kind

/* Session attributes - only vexdb_lite specific settings */
typedef struct {
    int ef_search;
    uint16 float_l2_arch;
    uint16 float_ip_arch;
    uint16 float_cos_arch;
    uint16 half_l2_arch;
    uint16 half_ip_arch;
    uint16 half_cos_arch;
    uint16 int8_l2_arch;
    uint16 int8_ip_arch;
    uint16 int8_cos_arch;
} PgVexdbAttrStorage;

typedef struct {
    PgVexdbAttrStorage attr_storage;
} PgVexdbSessionAttrs;

extern PgVexdbSessionAttrs vexdb_lite_session;
#define u_sess (&vexdb_lite_session)

/* Base object for palloc-based allocation */
class BaseObject {
public:
    void* operator new(size_t size, void *res) { return res; }
    void *operator new(size_t size) { return palloc(size); }
    void operator delete(void *ptr) { pfree(ptr); }
};

/* LWLock tranches - initialized at runtime */
extern "C" int vexdb_lite_lock_tranche_id;
#define LWTRANCHE_EXTEND vexdb_lite_lock_tranche_id

/* Inline macros for performance-critical functions */
#define FORCE_INLINE __attribute__((always_inline)) inline
#define INLINE_PROP FORCE_INLINE

/* Yield function - implemented in pg_yield.cpp */
extern "C" void pg_yield(unsigned int k);

/* PostgreSQL list API - openGauss uses different signature */
#define list_head(l) ((l) ? (ListCell *)((l)->elements) : NULL)

/* Memory context macros for openGauss compatibility */
#define MEMORY_CONTEXT_OPTIMIZER 0
#define SESS_GET_MEM_CXT_GROUP(ctx) CurrentMemoryContext

#endif
