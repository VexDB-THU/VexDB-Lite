# VexDB-Duck FixedSizeAllocator Migration Plan

**Date:** 2026-04-29  
**Goal:** Replace MemStore's `std::vector` storage with `FixedSizeAllocator` for persistent storage  
**Approach:** Keep MemStore interface unchanged, translate between `T id` (uint32) and `IndexPointer` internally

---

## 1. Overview

### Current State
- MemStore uses `std::vector` for all internal storage
- Data is lost after process exit unless explicitly serialized to BLOB
- No integration with DuckDB's checkpoint/WAL mechanisms

### Target State
- MemStore uses `FixedSizeAllocator` internally
- Data persists through DuckDB's native checkpoint/WAL
- Interface remains unchanged for `GraphIndexAlgorithm` compatibility

### Key Decisions
1. **Neighbor storage:** Stored as `T` (uint32) in node segments, NOT `IndexPointer` (Phase 2 can migrate)
2. **Entry point:** Stored as `T id` in `entry_info`, unchanged
3. **Upper layer metadata:** Embedded in upper segment (lower_layer_idx, id stored in segment)
4. **Distance cache:** Kept in memory (`level0_dists_`, `upper_dists_`)
5. **ID→Pointer mapping:** Serialized as BLOB in `IndexStorageInfo.options["id_ptr_map"]`

---

## 2. Data Structure Design

### 2.1 Node Segment Layout (Allocator 0)

```
+-------------------+
| HNSWNodeHeader    |  40 bytes
|   row_id          |  8B
|   level           |  1B
|   deleted         |  1B
|   level0_count    |  2B
|   extra_row_count |  2B
|   reserved        |  2B
|   vector_ptr      |  8B (IndexPointer)
|   upper_ptr       |  8B (IndexPointer)
+-------------------+
| level0_neighbors  |  M*2 * sizeof(T) = M*2 * 4 bytes
|   [T, T, T, ...]  |  (stored as uint32 IDs)
+-------------------+

Segment size = 40 + M*2*4 bytes
```

### 2.2 Vector Segment Layout (Allocator 1)

```
+-------------------+
| float[dimension]  |  dimension * 4 bytes
+-------------------+
```

### 2.3 Upper Segment Layout (Allocator 2)

```
+-------------------+
| HNSWUpperLevel    |
|   counts[8]       |  16 bytes (HNSW_MAX_UPPER_LEVELS = 8)
+-------------------+
| lower_layer_idx   |  4 bytes (T)
| id                |  4 bytes (T)
+-------------------+
| neighbors         |  8*M * sizeof(T) = 8*M*4 bytes
|   [level1: M Ts]  |
|   [level2: M Ts]  |
|   ...             |
+-------------------+

Segment size = 16 + 4 + 4 + 8*M*4 bytes
```

### 2.4 ID ↔ IndexPointer Mapping

```
id_to_node_ptr_:    vector<IndexPointer>   // id → node pointer (index is id)
node_ptr_to_id_:    unordered_map<idx_t, T> // node_ptr.Get() → id (for reverse lookup)
upper_idx_to_ptr_:  vector<IndexPointer>   // upper_idx → upper pointer
```

---

## 3. Files to Create/Modify

### 3.1 New Files

| File | Purpose |
|------|---------|
| `vexdb-duck/include/vex_hnsw_node.hpp` | HNSWNodeHeader, HNSWUpperLevel structures |

### 3.2 Modified Files

| File | Changes |
|------|---------|
| `vexdb-duck/include/vex_graph_index_depend_duck.hpp` | MemStore allocator storage, all method implementations |
| `vexdb-duck/include/vex_graph_index.hpp` | Add serialization method declarations |
| `vexdb-duck/index/graph_index.cpp` | Call InitAllocators, update serialization |
| `vexdb-duck/index/graph_index_disk.cpp` | Update serialization to use allocators |

---

## 4. Implementation Phases

### Phase 1: Data Structures and Allocator Members

**Files:** `vex_hnsw_node.hpp`, `vex_graph_index_depend_duck.hpp`

**Tasks:**
1. Create `vex_hnsw_node.hpp` with HNSWNodeHeader and HNSWUpperLevel structures
2. Add allocator members to MemStore:
   - `unique_ptr<FixedSizeAllocator> node_alloc_`
   - `unique_ptr<FixedSizeAllocator> vector_alloc_`
   - `unique_ptr<FixedSizeAllocator> upper_alloc_`
3. Add ID↔Pointer mapping vectors:
   - `std::vector<IndexPointer> id_to_node_ptr_`
   - `std::vector<IndexPointer> upper_idx_to_ptr_`
   - `unordered_map<idx_t, T> node_ptr_to_id_`
4. Add upper layer metadata storage:
   - Embed `lower_layer_idx` and `id` in upper segment (not separate vectors)
5. Add distance cache vectors:
   - `std::vector<std::vector<float>> level0_dists_`
   - `std::vector<std::vector<float>> upper_dists_`

**Verification:** Code compiles

---

### Phase 2: Allocator Initialization

**Files:** `vex_graph_index_depend_duck.hpp`, `vex_graph_index.cpp`

**Tasks:**
1. Add `InitAllocators(BlockManager&)` method to MemStore
2. Calculate segment sizes based on M and dimension
3. Reserve slot 0 in each allocator (null sentinel)
4. Call `InitAllocators` from `GraphIndex::BuildBulk` and `GraphIndex::Create`

**Verification:** Allocators initialize without error

---

### Phase 3: Node Operations

**Files:** `vex_graph_index_depend_duck.hpp`

**Tasks:**
1. Implement `assign_vector_id<true>()` - allocate node and vector segments
2. Implement `assign_vector_id<false>()` - allocate upper segment
3. Implement `add_vector(T id, const char* query)` - store vector data
4. Implement `add_elem(...)` - store row_id in header
5. Implement `get_data(T id)` - retrieve vector data
6. Implement `set_entrypoint(...)` - unchanged, uses T id

**Verification:** Nodes allocate correctly, vectors stored and retrieved

---

### Phase 4: Neighbor Operations

**Files:** `vex_graph_index_depend_duck.hpp`

**Tasks:**
1. Implement `get_point_info<true>()` - return level 0 neighbors
2. Implement `get_point_info<false>()` - return upper neighbors
3. Implement `set_base_neighbors(T id, const T* neighbors)` - write to segment
4. Implement `set_upper_neighbors(T idx, const T* neighbors_info)` - write to segment
5. Implement `add_basepoint(...)` - wrapper for set_base_neighbors
6. Implement `add_upperpoint(...)` - allocate if needed, set metadata and neighbors
7. Implement `get_neighbors<is_base_layer>()` - read from segment
8. Implement `set_neighbor<is_base_layer>()` - update single neighbor
9. Implement `get_neighbor_stats<is_base_layer>()` - return distance array and bit span

**Verification:** Neighbor operations work correctly

---

### Phase 5: Row ID Operations

**Files:** `vex_graph_index_depend_duck.hpp`

**Tasks:**
1. Implement `get_itempointer(T id, Func&& func)` - retrieve row_id(s) from header
2. Implement `apply_elem(T id, Func&& func)` - apply function to point_type
3. Add dedup support:
   - `TryDedup(...)` - check for duplicate vectors
   - Store extra row_ids in `dedup_map_`

**Verification:** Row IDs correctly stored and retrieved

---

### Phase 6: Serialization

**Files:** `vex_graph_index.hpp`, `vex_graph_index.cpp`, `graph_index_disk.cpp`

**Tasks:**
1. Implement `SerializeToDisk(...)`:
   - Store metadata in `options`:
     - dimension, m, node_count, upper_count
     - entry_id, entry_cur_layer_idx, entry_level
   - Serialize `id_to_node_ptr_` as BLOB in `options["id_ptr_map"]`
   - Serialize `upper_idx_to_ptr_` as BLOB
   - Serialize row_ids as BLOB
   - Serialize dedup_map as BLOB
   - Call `allocator->SerializeBuffers(...)` for each allocator
   - Store allocator_infos
2. Implement `SerializeToWAL(...)` - similar but use `InitSerializationToWAL`
3. Implement deserialization helper:
   - Restore allocators with `allocator->Init(info)`
   - Restore id→pointer mappings from BLOB
   - Restore entry point
   - Restore row_ids and dedup_map

**Verification:** Index persists through checkpoint, reloads correctly

---

### Phase 7: Deserialization

**Files:** `vex_graph_index.cpp`

**Tasks:**
1. Add `DeserializeFromStorage(const IndexStorageInfo& info)` to GraphIndex
2. Restore allocator state from `allocator_infos`
3. Restore id→pointer mappings from BLOB
4. Restore entry point metadata
5. Restore row_ids and dedup_map
6. Update `GraphIndex::Create` to call deserialization when storage info is valid

**Verification:** Index usable after database restart

---

### Phase 8: Cleanup and Testing

**Tasks:**
1. Remove old vector-based storage code
2. Remove BLOB-based serialization from `graph_index_disk.cpp`
3. Add smoke tests:
   - Create index, checkpoint, restart, query
4. Run SIFT benchmark to verify correctness

**Verification:** All tests pass

---

## 5. Serialization Format

### 5.1 Metadata Options

```
options["dimension"]         = UINTEGER
options["m"]                 = INTEGER
options["node_count"]        = UBIGINT
options["upper_count"]       = UBIGINT
options["entry_id"]          = UBIGINT (T id)
options["entry_cur_layer_idx"] = UBIGINT
options["entry_level"]       = INTEGER
options["max_dedup"]         = USMALLINT
```

### 5.2 ID→Pointer Map BLOB

```
uint64_t count
struct {
    uint64_t ptr_value  // IndexPointer.Get()
} [count entries]
```

### 5.3 Row IDs BLOB

```
uint64_t count
row_t row_ids[count]
```

### 5.4 Dedup Map BLOB

```
uint64_t entry_count
struct {
    uint64_t node_key
    uint64_t extra_count
    row_t extras[extra_count]
} [entry_count entries]
```

---

## 6. Testing Checklist

| Test | Description |
|------|-------------|
| Compile | Extension builds without errors |
| Create Index | CREATE INDEX succeeds |
| Insert | Vectors inserted correctly |
| Search | ANN search returns correct results |
| Checkpoint | Index data written to disk |
| Restart | Database restarts successfully |
| Reload | Index reloads from disk |
| Search after reload | ANN search still returns correct results |
| SIFT 10k | Benchmark passes |
| SIFT 100k | Benchmark passes |

---

## 7. Future Work (Phase 2)

After this migration is complete:

1. **Replace T with IndexPointer** in MemStore interface
   - Change `using T = uint32` to `using T = IndexPointer`
   - Update `GraphIndexAlgorithm` template instantiation
   - Remove id→pointer translation layer

2. **Optimize neighbor storage**
   - Store neighbors as `IndexPointer` instead of `T`
   - Remove reverse lookup map

3. **Add parallel build support**
   - Implement buffer pointer cache like reference
   - Add striped spinlocks for concurrent access

---

## 8. References

- Reference implementation: `/home/mingwei6/workspace/referencce/extension/vex/`
- DuckDB FixedSizeAllocator: `duckdb/execution/index/fixed_size_allocator.hpp`
- DuckDB IndexPointer: `duckdb/execution/index/index_pointer.hpp`
- Design doc: `docs/plans/2026-04-28-vexdb-duck-design-v2.md`
