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

## Known issue (out of scope for this branch)

`index__graph_index_restart_large_slow.test` SIGSEGVs in
`test_sqllogictest.cpp:41`. This is **not** introduced by parallel build —
the failure is in deserialization after restart (`DeserializeFromStorage`
only recovers metadata, missing elems/邻居 data). Tracked in memory
`project_vexdb_duck_reload_gap.md` as a pre-existing reload bug.

This bug pre-dates the parallel build work and applies equally to single-
threaded builds (default behavior). Fixing it requires a separate
work-stream on the persistence layer (P8' in the design doc).

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
