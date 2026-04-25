# pg_vexdb - HNSW Graph Index Extension for PostgreSQL

## Project Goal

Port vexdb graph index (HNSW vector similarity search) to a PostgreSQL extension. The key principle is **maximum code identity** - keep files as identical as possible to vexdb source so future changes can be trivially applied to both codebases.

**Target:** PostgreSQL 19devel
**Principle:** Copy/modify from vexdb, don't write from scratch. Complete each phase fully before moving on.

## Build Commands

```bash
cd /home/mingwei6/workspace/pg_vexdb/build
cmake .. && make -j4     # Build extension
make install              # Install to PostgreSQL
```

PostgreSQL installation: `/home/mingwei6/workspace/postgres/pg-install/`
OpenGauss source: `/home/mingwei6/workspace/opengauss/`

**PostgreSQL must be configured with:**
```bash
./configure --prefix=/home/mingwei6/workspace/postgres/pg-install --enable-debug --enable-cassert --without-icu --without-readline --without-zlib CFLAGS="-O0 -g"
make -j$(nproc) && make install
```

**PostgreSQL must run with shared_preload_libraries:**
```bash
echo "shared_preload_libraries = 'pg_vexdb'" >> $PGDATA/postgresql.conf
```

---

## Current Status

| Component | Status | Notes |
|-----------|--------|-------|
| Build system (CMake) | ✅ Done | |
| CREATE EXTENSION | ✅ Done | Requires `shared_preload_libraries` |
| floatvector type | ✅ Done | Input, output, operators |
| halfvector type | ✅ Done | Input, output, operators |
| Distance functions | ✅ Done | L2, IP, Cosine |
| CREATE INDEX | ✅ Done | HNSW graph builds successfully |
| Index scan | ✅ Done | Works correctly |
| Vector buffer manager | ✅ Working | Shared-memory manager/worker path stable, inspect shows active pools |
| GUC parameters | ✅ Done | 5 GUCs: ef_search, enable_vec_buffer_manager, vector_buffers, vector_buffer_workers, vec_architecture |
| RelOpts | ✅ Done | m, ef_construction, parallel_workers, quantizer, cluster_rate, enable_async_insert |
| SIMD distance | ✅ Working | Auto-detects CPU capabilities (SSE/AVX/AVX512) |
| Quantizer support | ❌ Deferred | Stubbed out |

---

## Architecture

### Language & Build
- C++17 for all code, treating PostgreSQL interfaces as `extern "C"`
- CMake build system (not PGXS due to C++ requirements)
- Boost preprocessor for enum generation (distance types, metrics)

### Key Directories

| Directory | Purpose | Source (vexdb) |
|-----------|---------|-------------------|
| `vtl/` | Template library (Vector, HashSet, PriorityQueue, etc.) | `src/include/templates/vtl/` |
| `include/graph_index/` | Graph index header files | `src/include/access/graph_index/` |
| `distance/` | Distance functions (L2, IP, Cosine, SIMD) | `src/include/access/annvector/distance/` |
| `include/` | Vector types (half, floatvector) | `src/include/access/annvector/` |
| `knl/` | vexdb global state stubs | N/A - created for PG port |
| `module/` | Utility modules (timer, parallel_counter) | `src/include/access/annvector/module/` |
| `quantizer/` | Quantizer headers (stubbed) | `src/include/access/annvector/quantizer/` |
| `rabitq/` | RaBitQ headers | `src/include/access/annvector/rabitq/` |
| `src/` | Implementation files | `src/gausskernel/storage/access/graph_index/` |

### Vector Storage Design

**vexdb:** Uses `VECTOR_FORKNUM` (fork 5) to store vector data in separate `_vec` files.

**PostgreSQL Limitation:** Extensions cannot register new fork types.

**Solution:** Store vector data in separate files named `{relpath}_vec`:
- Path: Same directory as index, filename = `{relfilenode}_vec`
- Format: 1GB blocks (same as vexdb)
- I/O: Thread-safe `pread`/`pwrite` (no extra locking)
- WAL: Deferred

---

## GUC Parameters

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `pg_vexdb.ef_search` | int | 64 | HNSW search ef parameter |
| `pg_vexdb.enable_vec_buffer_manager` | bool | true | Enable vector buffer caching |
| `pg_vexdb.vector_buffers` | int | 262144 | Number of 8KB vector buffers |
| `pg_vexdb.vector_buffer_workers` | int | 1 | Number of vector buffer background workers |
| `pg_vexdb.vec_architecture` | string | "" | SIMD architecture selection |

### vec_architecture Syntax

```
pg_vexdb.vec_architecture = 'usage:arch[, usage:arch, ...]'

usage := all | float | half | int8 | l2 | ip | cos | 
         float_l2 | float_ip | float_cos | half_l2 | half_ip | half_cos |
         int8_l2 | int8_ip | int8_cos

arch := scalar | sse | avx | avx512 | neonv8 | svev8 | ...
```

**Examples:**
```sql
SET pg_vexdb.vec_architecture = 'all:avx';
SET pg_vexdb.vec_architecture = 'float:avx512, half:sse';
SET pg_vexdb.vec_architecture = 'l2:avx, ip:scalar';
```

Empty string (default) means auto-detect best available SIMD.

---

## Index RelOpts

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `m` | int | 16 | Number of neighbors per node |
| `ef_construction` | int | 64 | Dynamic candidate list size during build |
| `parallel_workers` | int | 0 | Parallel build workers (0 = auto) |
| `quantizer` | string | NULL | Quantizer type (none, pq, rabitq) |
| `cluster_rate` | int | 0 | Cluster rate for quantization |
| `enable_async_insert` | bool | false | Enable async insert |

---

## Key Fixes Applied

### 1. extern "C" Wrapping

All PostgreSQL headers must be wrapped in `extern "C"`:
```cpp
#ifdef __cplusplus
extern "C" {
#endif
#include "postgres.h"
#include "fmgr.h"
// ... other PG headers
#ifdef __cplusplus
}
#endif
```

`pg_compat.h` wraps all common PostgreSQL headers. Additional headers needed in specific files should also be wrapped.

### 2. IndexAmRoutine Function Signatures

PostgreSQL 19's `IndexAmRoutine` expects function pointers with specific signatures, not `PG_FUNCTION_ARGS`. See `src/graph_index_am.cpp` for the wrapper pattern.

### 3. LWLock Registration

PostgreSQL 19 requires LWLocks be registered in `shmem_request_hook`. VecBufferManager initialization must happen in `shmem_startup_hook` (after LWLock registration):
```cpp
static void pg_vexdb_shmem_request(void) {
    RequestNamedLWLockTranche("vector_buffer", 1);
}

static void pg_vexdb_shmem_startup(void) {
    pg_vexdb_preloaded = true;
    init_vector_smgr();  // Initialize VecBufferManager here, not in _PG_init
}

void _PG_init(void) {
    shmem_request_hook = pg_vexdb_shmem_request;
    shmem_startup_hook = pg_vexdb_shmem_startup;
}
```

### 4. GUC-to-Session Wiring

`ef_search` uses an assign hook to sync GUC value to session struct:
```cpp
static void assign_ef_search(int newval, void *extra) {
    pg_vexdb_session.attr_storage.ef_search = newval;
}
```

### 5. Type OID Runtime Lookup

Type OIDs are looked up at runtime, not hardcoded:
```cpp
Oid get_floatvector_oid(void) {
    static Oid cached_oid = InvalidOid;
    if (cached_oid == InvalidOid) {
        cached_oid = TypenameGetTypid("floatvector");
    }
    return cached_oid;
}
```

### 6. Buffer Locking for MarkBufferDirty

`MarkBufferDirty()` requires exclusive lock on the buffer before calling.

### 7. Page Checksums

When `data_checksums = on`, use `PageSetChecksum((Page)page, blkno)` before `smgrextend()`.

### 8. IndexScanDesc ORDER BY Fields

Must initialize `xs_orderbyvals` and `xs_orderbynulls` in `beginscan`.

### 9. Reset neighbors_val_pool

`get_neighbors_data()` must call `store.reset_neighbors_val_pool()` before populating.

### 10. pg_yield Implementation

Spinlock yield function copied from vexdb with CPU-specific pause instructions:
- x86: `rep; nop` (pause)
- ARMv8: `yield` instruction
- Falls back to `sched_yield()` and `pg_usleep(1)` for longer waits

### 11. VecBufferManager Initialization Timing

VecBufferManager must be initialized in `shmem_startup_hook`, not `_PG_init()`:
- `_PG_init()` runs first and sets hooks
- `shmem_request_hook` runs next to register LWLocks
- `shmem_startup_hook` runs last - this is when VecBufferManager can safely use `GetNamedLWLockTranche()`
- If initialized in `_PG_init()`, the LWLock tranche isn't registered yet, causing NULL pointer crash

### 12. Shared VecBuffer State and Worker Registration

VecBuffer manager state is created in main shared memory and attached by all processes:
- Shared state struct: `VecBufSharedState` (`include/vecbuf_shared.h`)
- Shared allocator context: `SharedAllocSet` (`include/shared_alloc_set.h`, `src/shared_alloc_set.cpp`)
- Shared allocator adapter: `vtl/shared_allocator`
- Background worker: `src/vecbuf_worker.cpp`

Worker registration rules:
- Register in `_PG_init()` only when `process_shared_preload_libraries_in_progress` is true
- Gate worker registration by `pg_vexdb.enable_vec_buffer_manager`
- Worker count comes from `pg_vexdb.vector_buffer_workers`

Memory sizing rule:
- `RequestAddinShmemSpace()` includes vector page buffers plus metadata overhead derived from `pg_vexdb.vector_buffers`

---

## Code Conventions

### Replaced Macros (inline in code instead)

| Old Macro | Replacement |
|-----------|-------------|
| `ItemPointerEqualsNoCheck(a, b)` | `ItemPointerEquals(a, b)` |
| `(Item)` cast | `(const void*)` |
| `page_index_tuple_overwrite` | `PageIndexTupleOverwrite` |
| `mem_align_free(ptr)` | `pfree(ptr)` |
| `STORAGE_SPACE_OPERATION(rel, size)` | `((void)0)` |
| `RelationOpenSmgr(rel)` | `RelationGetSmgr(rel)` |

### Headers in pg_compat.h

All commonly needed PostgreSQL headers are in `pg_compat.h` wrapped in `extern "C"`. Additional headers (like `optimizer/*.h`) should be wrapped locally:
```cpp
extern "C" {
#include "optimizer/optimizer.h"
#include "optimizer/cost.h"
}
```

---

## Testing

```sql
CREATE EXTENSION pg_vexdb;
CREATE TABLE t (id serial, v floatvector(3));
INSERT INTO t (v) VALUES ('[1,2,3]'), ('[4,5,6]'), ('[7,8,9]');
CREATE INDEX ON t USING vexdb_graph (v floatvector_l2_ops) WITH (m = 16, ef_construction = 64);
SET enable_seqscan = false;
SELECT * FROM t ORDER BY v <-> '[3,4,5]' LIMIT 2;
```

---

## Key Constraints

1. **No C++ exceptions** across PostgreSQL boundary - use `elog(ERROR)` only
2. **All memory** via PostgreSQL memory contexts (palloc/pfree)
3. **No STL** - incompatible with PostgreSQL's setjmp/longjmp exception handling
4. **Vector storage** in separate `_vec` files (not in fork)
5. **WAL** via `generic_xlog` for index pages, deferred for vector storage
6. **shared_preload_libraries** required for LWLock registration
7. **extern "C"** required for all PostgreSQL header includes and function declarations

---

## Files Created for PostgreSQL Port

| File | Purpose |
|------|---------|
| `include/pg_compat.h` | Compatibility layer, wraps PG headers |
| `knl/knl_alloc.cpp` | Memory allocation, `_PG_init`, GUC registration |
| `knl/knl_variable.h` | Global instance stub declarations |
| `knl/knl_instance.h` | Global instance structure |
| `src/session_compat.cpp` | Session attributes struct |
| `src/guc_config.cpp` | GUC parameters and reloptions |
| `include/vecbuf_shared.h` | Shared vec buffer state in main shmem |
| `include/shared_alloc_set.h` | Shared memory MemoryContext declaration |
| `src/shared_alloc_set.cpp` | Shared memory MemoryContext implementation |
| `src/vecbuf_worker.cpp` | Vector buffer background worker |
| `vtl/shared_allocator` | VTL allocator for shared-memory containers |
| `src/pg_yield.cpp` | Spinlock yield implementation |
| `src/quantizer_stubs.cpp` | Empty stubs for quantizer |
| `CMakeLists.txt` | CMake build configuration |

---

## Directory Cleanup (Apr 2026)

### Removed Directories
- `include/access/` - Duplicates, unused
- `include/store/` - Duplicates of `include/*.h`
- `include/distance/` - Empty
- `include/module/` - Duplicate of `module/`
- `utils/` - Unused `rel_utils.h`

### Removed Files
- `include/graph_index_param.h` - Merged into `include/graph_index/graph_index_param.h`
- `include/graph_index_config.h` - Values merged into `graph_index_param.h`
- `include/pg_version_defs.h` - Unused
- `src/ann_utils_stub.cpp` - Unused
- `src/distance/architecture.cpp` - Replaced by renamed `architecture_minimal.cpp`

### Renamed Files
- `src/distance/architecture_minimal.cpp` → `src/distance/architecture.cpp`

---

## Future Work

1. **WAL support** - Currently deferred for vector storage
2. **Quantizer support** - PQ, RaBitQ (currently stubbed)
3. **Parallel build** - Needs testing
4. **Performance testing** - Large datasets (100K+ vectors)
5. **Memory management** - Verify no leaks under load
