# Duck-side parallel HNSW build — implementation status

**Date**: 2026-05-12
**Branch**: `feature/duck-parallel-build`
**Base**: `origin/vexdb-unify` (ca87f3e878)

## Summary

Wired up `CREATE INDEX ... WITH (threads=N)` on the duck side end-to-end.
Prior to this branch the duck adapter parsed `threads=N` but always built
single-threaded (TODO at `vexdb-duck/index/graph_index.cpp:176`).

## What landed

| Phase | Content | Commit |
|---|---|---|
| **P1** | `SimpleRWLock` atomic state machine + unified `unlock()` | `15309089a7` |
| **P2** | `duck_pg_shim.hpp` — LWLock / SpinLock / pg_memory_barrier shims for the duck compile path | `15309089a7` |
| **P3'** | MemStore entry-lock double-lock promotion protocol (`get_entry` / `release_entry_lock`) | `15309089a7` |
| **API port** | DuckDB main API adaptation (75 → 0 errors): `GetReturnType()`, `GetDataMutable<T>`, `BindScalarFunctionInput`, `ResetStorage`, etc. | `9b6b8cb935` |
| **P4'** | MemStore concurrency: 64-stripe `LWLockPadded` per-element locks, atomic id counters, `ReserveCapacity()` pre-allocates ALL allocator slots (fixes FixedSizeAllocator buffers-map race) | `696660af7e` |
| **P5'** | `BuildBulk` wire-up: pre-reserve → serial first base point → `std::thread` pool → exception aggregation | `696660af7e` |
| **P6'** | C++ stress test (`stress_parallel_build.cpp`); **50 iter × 10k vec × threads=4 × dim=128: 5/5 passed** (previously 5/5 SIGSEGV) | `696660af7e` |
| **P7'** | Spec test `graph_index_parallel_build.yaml`; **19 sqllogictest assertions, all passing** | `696660af7e` |

## Verification

```
# P6' stress
$ ./build/duck/bin/stress_parallel_build .../vex.duckdb_extension 50 10000 4 128
OK: 50 iterations completed, no crash, 2500 total query results
# 339s wall, 380-422% CPU = real 4-way parallelism

# P7' spec
$ bash tests/spec/_lib/docker/run_duckdb.sh test '*parallel_build*'
All tests passed (19 assertions in 1 test case)
```

## Default behavior

`build_threads_` default is **1** (serial), matching upstream vexdb main lib
(`HnswOptions::parallel_workers` defaults to 0). Existing `CREATE INDEX`
statements without `WITH (threads=N)` keep their pre-parallel behavior — zero
regression risk for default users.

## Reload bug repair (commits `7a726d68b7` + `f41c023246`)

The pre-existing reload bug (memory `project_vexdb_duck_reload_gap.md`)
is **fully fixed** in this branch.

Root causes (two independent bugs, both pre-dated this branch):

1. **`set_neighbor()` did not maintain `level0_count`** — when
   reverse-edge updates wrote new neighbors via `set_neighbor`, only
   `neighbors[pruned]` was updated; `header->level0_count` stayed at
   its old value. A node could have N real neighbors in slots [0..N)
   but report `level0_count = 0`. After restart, code that trusted
   the count (the original P8' patch + `get_neighbors`'s `max_count`
   fallback) treated the real neighbors as garbage and wiped them.

2. **Atomic id counters not reset on reload** —
   `DeserializeFromStorage` restored `id_to_node_ptr_`, `elems`,
   etc. but left `next_base_id_` / `next_upper_id_` at their default
   value of 0. The next `assign_vector_id<true>()` then returned
   `id = 0`, colliding with the existing node 0. The `add_elem` /
   `add_vector` calls that followed clobbered node 0's segment with
   the new row's data, so the next `search_layer` walked a corrupted
   graph and SIGSEGV'd on garbage neighbor IDs.

Additional defensive improvements:

- **Patch stale unused slots at reload** — slots `[level0_count..m*2)`
  in the disk-backed header could still contain garbage from previously
  freed segments. Zero-pad them to `INVALID_VECTOR_ID` so `is_valid()`
  correctly breaks the iteration.
- **Refill `base_points[i].neighbors` / `vectors[i]`** from disk-backed
  header at reload, for fallback code paths.
- **`get_data(id)` defensive bound check** for any id that slips past
  `is_valid`.

Test results (full restart + persistence suite):

| | Before | After P8' partial | After P8' full |
|---|---|---|---|
| restart+persistence passing | 0/13 | 5/13 | **13/13** |

All previously-failing tests now pass: `restart_insert`,
`restart_update`, `restart_nocheckpoint`, `restart_large_slow`,
`persistence`, `persistence_slow`, `persistence_full`,
`pq_persistence`.

## Key design choices documented in code

- **Pre-allocate all nodes upfront** (`ReserveCapacity` in
  `vex_graph_index_depend_duck.hpp`): DuckDB's `FixedSizeAllocator` is not
  thread-safe (its internal `buffers` unordered_map can rehash on `New()`).
  Holding EXCLUSIVE `elems_veclock` during `New()` only serializes writers;
  concurrent `Get()` readers still race. Pre-allocating eliminates the
  race entirely — `assign_vector_id` fast-path is then lock-free (just
  `fetch_add` on the atomic counter).

- **64 striped locks** (`STRIPE_COUNT=64`): cache-line aligned via
  `LWLockPadded`; chosen so 8KB total fits per layer. Hash via `idx &
  STRIPE_MASK` requires `STRIPE_COUNT` to be a power of 2.

- **Serial first base point**: the empty-graph entry-promotion path in
  `algorithm.insert` reads `entry_level == -1` and sets a fresh entry.
  Multiple workers racing here would all enter that branch — pre-inserting
  one node serially eliminates the race without changing the algorithm.

## Files touched

| File | Change |
|---|---|
| `vexdb-duck/include/vex_simple_rwlock.hpp` | new (P1) |
| `vexdb-duck/include/duck_pg_shim.hpp` | new (P2) |
| `vexdb-duck/include/vex_graph_index_depend_duck.hpp` | P3' entry-lock protocol + P4' striped/atomic/Reserve |
| `vexdb-duck/include/vex_graph_index.hpp` | P5' `build_threads_` member + ctor param |
| `vexdb-duck/index/graph_index.cpp` | P5' BuildBulk thread pool |
| `vexdb-duck/test/cpp/test_simple_rwlock.cpp` | P1 unit test |
| `vexdb-duck/test/cpp/test_duck_pg_shim.cpp` | P2 unit test |
| `vexdb-duck/test/stress_parallel_build.cpp` | P6' stress test |
| `tests/spec/shared/index/graph_index_parallel_build.yaml` | P7' spec |
