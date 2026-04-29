# pg_vexdb - HNSW Graph Index Extension for PostgreSQL

**[English](README.en.md)** | **[中文](README.md)**

A high-performance vector similarity search extension for PostgreSQL, providing HNSW (Hierarchical Navigable Small World) graph index for approximate nearest neighbor search. Ported from vexdb with maximum code identity for maintainability.

## Features

### Vector Types
- **floatvector** - Single-precision floating-point vectors (up to 16,384 dimensions)
- **halfvector** - Half-precision floating-point vectors (up to 16,384 dimensions)
- Support for NULL values, TOAST compression, and typmod

### Distance Functions
- **L2 distance** (`<->`) - Euclidean distance
- **Inner product** (`<#>`) - Negative inner product
- **Cosine distance** (`<=>`) - Cosine distance

### SIMD Acceleration
- Auto-detects CPU capabilities (SSE, AVX, AVX512)
- Runtime architecture selection via GUC parameter
- Supports x86_64 and ARM architectures

### Index Access Method
- HNSW graph-based approximate nearest neighbor search
- Configurable M (neighbors per node) and ef_construction
- Support for parallel index build
- Efficient search with configurable ef_search

### GUC Parameters
| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `pg_vexdb.ef_search` | int | 64 | HNSW search ef parameter |
| `pg_vexdb.enable_vec_buffer_manager` | bool | true | Enable vector buffer caching |
| `pg_vexdb.vector_buffers` | int | 262144 | Number of 8KB vector buffers |
| `pg_vexdb.vec_architecture` | string | "" | SIMD architecture selection |

---

## Requirements

### Build Dependencies
- **PostgreSQL 19** (developed against 19devel)
- **C++17** compiler (GCC 8+, Clang 7+)
- **CMake** 3.10+
- **Boost** (for preprocessor macros only)

### Runtime
- PostgreSQL 19
- Linux (x86_64 or ARM64)

---

## Compilation

### 1. Build PostgreSQL (if not already installed)

```bash
./configure --prefix=/path/to/pg-install --enable-debug --enable-cassert \
    --without-icu --without-readline --without-zlib CFLAGS="-O0 -g"
make -j$(nproc)
make install
```

### 2. Build pg_vexdb

```bash
cd pg_vexdb
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
make install
```

### 3. Configure PostgreSQL

Add to `postgresql.conf`:
```
shared_preload_libraries = 'pg_vexdb'
```

Restart PostgreSQL:
```bash
pg_ctl restart -D $PGDATA
```

---

## Usage

### Basic Usage

```sql
-- Create extension
CREATE EXTENSION pg_vexdb;

-- Create table with vector column
CREATE TABLE items (
    id serial PRIMARY KEY,
    embedding floatvector(128)
);

-- Insert vectors
INSERT INTO items (embedding) VALUES 
    ('[0.1, 0.2, 0.3, ...]'),
    ('[0.4, 0.5, 0.6, ...]');

-- Create HNSW index
CREATE INDEX ON items USING vexdb_graph (embedding floatvector_l2_ops)
    WITH (m = 16, ef_construction = 64);

-- Query with index
SELECT * FROM items 
ORDER BY embedding <-> '[0.2, 0.3, 0.4]' 
LIMIT 10;
```

### Index Options

```sql
CREATE INDEX ON items USING vexdb_graph (embedding floatvector_l2_ops)
    WITH (
        m = 32,                    -- Number of neighbors per node (default: 16)
        ef_construction = 128,     -- Build-time search list size (default: 64)
        parallel_workers = 4       -- Parallel build workers (default: 0 = auto)
    );
```

### Query Tuning

```sql
-- Increase ef_search for better recall (higher cost)
SET pg_vexdb.ef_search = 256;

-- Force index scan
SET enable_seqscan = false;

-- Query with distance
SELECT id, embedding <-> '[0.2, 0.3, 0.4]' AS distance
FROM items
ORDER BY embedding <-> '[0.2, 0.3, 0.4]'
LIMIT 10;
```

### SIMD Architecture Selection

```sql
-- Use AVX for all operations
SET pg_vexdb.vec_architecture = 'all:avx';

-- Use AVX512 for float vectors, SSE for half vectors
SET pg_vexdb.vec_architecture = 'float:avx512, half:sse';

-- Reset to auto-detect
SET pg_vexdb.vec_architecture = '';
```

---

## Framework and Code Structure

### Directory Layout

```
pg_vexdb/
├── distance/              # Distance function headers
│   ├── distance.h         # Core distance function declarations
│   ├── distance_dispatcher.h
│   ├── architecture_macro.h
│   └── pq/               # Product quantization
├── include/
│   ├── graph_index/       # Graph index headers
│   ├── floatvector.h      # Float vector type
│   ├── halfvec.h          # Half vector type
│   ├── pg_compat.h        # PostgreSQL compatibility layer
│   └── ...
├── knl/                   # vexdb compatibility layer
│   ├── knl_alloc.cpp     # Memory allocation, _PG_init
│   ├── knl_instance.h    # Global instance structure
│   └── knl_variable.h    # Global variable stubs
├── module/               # Utility modules
│   ├── timer.h          # Timing utilities
│   └── parallel_counter.h
├── quantizer/            # Quantizer headers (stubbed)
├── rabitq/               # RaBitQ headers
├── src/
│   ├── distance/         # Distance implementations
│   ├── graph_index_*.cpp # Graph index implementation
│   └── ...
└── vtl/                  # Vector Template Library
    ├── vector
    ├── hashtable
    ├── disk_container/
    └── ...
```

### Key Components

#### 1. Compatibility Layer (`include/pg_compat.h`)

Provides abstractions for vexdb-specific features:
- Wraps all PostgreSQL headers in `extern "C"`
- Defines `u_sess` → `pg_vexdb_session` for session attributes
- Provides `VECTOR_FORKNUM`, `RM_GRAPH_INDEX_ID` macros

#### 2. Vector Template Library (`vtl/`)

Custom template library compatible with PostgreSQL's memory management:
- Uses PostgreSQL memory contexts (palloc/pfree)
- No STL (incompatible with setjmp/longjmp)
- Provides Vector, HashSet, PriorityQueue, etc.

#### 3. Graph Index (`include/graph_index/`, `src/graph_index*.cpp`)

HNSW implementation:
- `graph_index.h` - Main interface
- `graph_index_algorithm.h` - HNSW algorithm
- `graph_index_storage.h` - Disk storage
- `graph_index_cluster.h` - Clustering support

#### 4. Distance Functions (`distance/`, `src/distance/`)

SIMD-accelerated distance calculations:
- Runtime dispatcher based on CPU capabilities
- SSE, AVX, AVX512 implementations
- Template-based for type flexibility

---

## Implementation Showcase

### 1. Vector Type Implementation

**File:** `src/floatvector.cpp`

The `floatvector` type is a varlena structure stored directly in PostgreSQL:

```cpp
struct FloatVector {
    int32 vl_len_;  /* varlena header */
    int16 dim;      /* number of dimensions */
    int16 unused;   /* reserved */
    float4 x[FLEXIBLE_ARRAY_MEMBER];
};
```

Key functions:
- `floatvector_in()` - Parse text representation `[1,2,3]`
- `floatvector_out()` - Convert to text
- `l2_distance()` - Compute Euclidean distance
- Operators use PostgreSQL's FMGR interface wrapped in `extern "C"`:

```cpp
extern "C" {
PG_FUNCTION_INFO_V1(l2_distance);
Datum l2_distance(PG_FUNCTION_ARGS) {
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = PG_GETARG_FLOATVECTOR_P(1);
    float dist = l2_distance_impl(a, b);
    PG_RETURN_FLOAT4(dist);
}
}
```

### 2. Index Access Method

**File:** `src/graph_index_am.cpp`

PostgreSQL 19's `IndexAmRoutine` requires specific function signatures. We create wrapper functions:

```cpp
// Handler function registered in SQL
PG_FUNCTION_INFO_V1(graph_index_amhandler);
Datum graph_index_amhandler(PG_FUNCTION_ARGS) {
    PG_RETURN_POINTER(graph_index_amroutine());
}

// Build the IndexAmRoutine structure
static IndexAmRoutine *graph_index_amroutine(void) {
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);
    
    amroutine->ambuild = graph_index_ambuild;
    amroutine->aminsert = graph_index_aminsert;
    amroutine->ambeginscan = graph_index_ambeginscan;
    amroutine->amgettuple = graph_index_amgettuple;
    // ... more function pointers
    return amroutine;
}

// Wrapper with correct signature for IndexAmRoutine
static IndexBuildResult *graph_index_ambuild(
    Relation heap, Relation index, IndexInfo *indexInfo) {
    return graph_index_build_internal(heap, index, indexInfo);
}
```

### 3. HNSW Search Algorithm

**File:** `include/graph_index/graph_index_algorithm.h`

The search algorithm uses a priority queue for beam search:

```cpp
template<typename T>
Vector<Cand<T>> search(float *query, size_t ef) {
    MaxHeap<Cand<T>> candidates;
    MinHeap<Cand<T>> results;
    UnorderedSet<T> visited;
    
    // Start from entry point
    T ep = get_entry_point();
    float dist = distance(query, get_vector(ep));
    candidates.emplace(ep, dist);
    visited.insert(ep);
    
    while (!candidates.empty()) {
        Cand<T> cur = candidates.top();
        candidates.pop();
        
        if (results.size() >= ef && cur.dist > results.top().dist)
            break;
        
        results.emplace(cur);
        
        // Explore neighbors
        for (T neighbor : get_neighbors(cur.id)) {
            if (visited.insert(neighbor).second) {
                float d = distance(query, get_vector(neighbor));
                candidates.emplace(neighbor, d);
            }
        }
    }
    
    return results.to_vector();
}
```

### 4. SIMD Dispatch

**File:** `src/distance/architecture.cpp`

Runtime CPU feature detection:

```cpp
static Arch detect_best_arch() {
#if COMPILER_TARGET_X86_64
    unsigned int eax, ebx, ecx, edx;
    
    // Check AVX512
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        if (ebx & bit_AVX512F)
            return Arch::AVX512;
    }
    
    // Check AVX
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if (ecx & bit_AVX)
            return Arch::AVX;
        if (edx & bit_SSE2)
            return Arch::SSE;
    }
#endif
    return Arch::SCALAR;
}
```

### 5. Memory Management

**File:** `knl/knl_alloc.cpp`

All memory uses PostgreSQL contexts:

```cpp
void* mem_align_alloc(size_t alignment, size_t size) {
    return palloc_aligned(size, alignment, 0);
}

// Custom allocator for VTL
template<typename T>
class CtxAllocator {
    MemoryContext ctx;
public:
    T* allocate(size_t n) {
        return (T*)MemoryContextAlloc(ctx, n * sizeof(T));
    }
    void deallocate(T* p) { pfree(p); }
};
```

### 6. GUC Registration

**File:** `src/guc_config.cpp`

Custom GUC parameters with assign hooks:

```cpp
static void assign_ef_search(int newval, void *extra) {
    pg_vexdb_session.attr_storage.ef_search = newval;
}

void pg_vexdb_init_guc(void) {
    DefineCustomIntVariable("pg_vexdb.ef_search",
        "Search list size for HNSW index search.",
        NULL, &pg_vexdb_ef_search, 64, 1, 65535,
        PGC_USERSET, 0, NULL, assign_ef_search, NULL);
}
```

---

## Performance Tips

1. **Index Build**
   - Use higher `ef_construction` for better recall (128-256)
   - Use `parallel_workers` for large datasets
   - Increase `maintenance_work_mem` if possible

2. **Query**
   - Tune `ef_search` based on recall requirements
   - Higher `ef_search` = better recall, slower query
   - Typical values: 64-256

3. **Memory**
   - Adjust `pg_vexdb.vector_buffers` for cache size
   - Default: 262144 buffers = 2GB

---

## Limitations

1. **WAL** - Vector data changes not WAL-logged (deferred)
2. **Quantization** - PQ and RaBitQ not yet implemented
3. **Platform** - Linux only (x86_64, ARM64)

---

## License

Same license as vexdb.

---

## Credits

Ported from vexdb vector index implementation.
