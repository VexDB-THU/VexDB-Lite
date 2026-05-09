# PG VexDB ARM NEON Retest Report

## 1. Scope

Retest `pg_vexdb` on the ARM server `192.168.130.66` after the repository added ARM SIMD support:

- baseline branch: `vexdb-unify`
- baseline head during retest: `358dcde9bd63e9431e8928f52ac0ab0cb52abb1e`
- focus: verify that PostgreSQL now builds/runs with ARM NEON enabled, then rerun the same `10k / 100k / 1M` SIFT benchmark flow used in the previous ARM report

This retest was executed on `2026-05-09`.

## 2. Test Environment

### 2.1 Hardware

- Server: `aaa@192.168.130.66`
- CPU: `HUAWEI Kirin 9000C`
- Architecture: `aarch64`
- CPU count: `12`
- Memory: `15 GiB`
- OS: `Kylin V10 SP1`
- Kernel: `Linux 5.10.97-23-9000c`

### 2.2 PostgreSQL

- Install path: `/opt/postgresql-19rel-install`
- Version: `PostgreSQL 19devel`
- Configure flags:

```text
--prefix=/opt/postgresql-19rel-install --without-icu --without-readline --without-zlib CFLAGS=-O3 -DNDEBUG
```

### 2.3 Plugin Build

- Source tree on server: `/opt/vexdb-lite-build/VexDB-Lite`
- Build dir: `/opt/vexdb-lite-build/VexDB-Lite/build-pg19rel-release`
- Build type: `Release`
- Release flags:
  - `CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG`
  - `CMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG`

### 2.4 Database Runtime

- Data dir: `/home/aaa/vexdb-validation/pgdata-pg19rel-arm`
- Socket dir: `/home/aaa/vexdb-validation/run`
- Port: `55435`
- `shared_preload_libraries = 'pg_vexdb'`
- `shared_buffers = 2GB`
- Benchmark session overrides:
  - `enable_seqscan = off`
  - `enable_bitmapscan = on`
  - `pg_vexdb.ef_search = 100`
  - `maintenance_work_mem = '2GB'` in the bulk benchmark scripts

## 3. What Changed Versus The Previous ARM Report

The previous ARM report on `2026-05-08` ran with PostgreSQL ARM distance dispatch forced back to `GENERAL`.

In this retest:

- the current branch already included:
  - `feat(distance): integrate NEONv8 dispatch path for ARM builds`
  - `build(distance): align SIMD compile flags with openGauss baseline`
- during validation, `pg_vexdb` still failed to load on ARM because the new NEON path did not fully cover `INT8` template specializations used by the generic dispatcher
- a local compatibility patch was applied in:
  - [src/distance/core/neon_dispatcher.cpp](/Users/sunji/Work/VexDB-Lite/src/distance/core/neon_dispatcher.cpp:1)

That patch does two things:

- limits NEON explicit template instantiation to the actually used PG vector precisions: `FLOAT` and `HALF`
- forwards residual `INT8` NEON dispatcher references to the `GENERAL` implementation so that `pg_vexdb.so` can link and preload cleanly

This means:

- `FLOAT` and `HALF` queries can use `NEONV8`
- `INT8` remains compatibility-fallback only in this retest

## 4. Verification That NEON Is Really Active

After building and restarting PostgreSQL successfully, `index_inspect()` was used to inspect the created indexes.

Observed result:

```text
Architecture Usage | NEONV8
```

This was confirmed on:

- `idx_sift_pg_10k_vec`
- `idx_sift_pg_1m_vec`

So this retest is not running on the old `GENERAL` fallback path.

## 5. Benchmark Method

The same Python benchmark scripts from the previous ARM run were reused:

- `/home/aaa/vexdb-validation/scripts/pg_sift_small_bench.py`
- `/home/aaa/vexdb-validation/scripts/pg_sift1m_bench.py`
- `/home/aaa/vexdb-validation/scripts/pg_sift1m_query_only.py`

Data set:

- `10k`: `sift_train_10k.fbin`
- `100k`: `sift_train_100k.fbin`
- `1M`: `sift_base.fvecs`
- query set: `200` queries
- ground truth:
  - `sift_gt_10k_200q.ibin`
  - `sift_gt_100k_200q.ibin`
  - `sift_groundtruth.ivecs`

Index settings:

- `m = 16`
- `ef_construction = 64`
- query-time `pg_vexdb.ef_search = 100`

## 6. Retest Results

### 6.1 10k

```text
load_ms=675.492
build_ms=3621.935
query_ms=4012.295
qps=49.847
recall@10=0.999500
recall@100=0.995050
uses_index_scan=true
first_explain_has_index_scan=yes
```

### 6.2 100k

```text
load_ms=6036.217
build_ms=51100.431
query_ms=36182.889
qps=5.527
recall@10=0.997500
recall@100=0.974600
uses_index_scan=true
first_explain_has_index_scan=yes
```

### 6.3 1M Full Rebuild

```text
load_ms=69513.162
build_ms=711103.568
query_ms=118598.167
qps=1.686
recall@10=0.986000
recall@100=0.940750
uses_index_scan=true
```

### 6.4 1M Warm Query-Only Rerun

```text
query_ms=487.352
qps=410.381
recall@10=0.986000
recall@100=0.940750
uses_index_scan=true
```

## 7. Comparison With The Previous ARM GENERAL-Path Report

Previous report:

- [2026-05-08-arm-pg19-release-benchmark-report.md](/Users/sunji/Work/VexDB-Lite/docs/reports/2026-05-08-arm-pg19-release-benchmark-report.md)

### 7.1 Headline Comparison

| Scale | Metric | 2026-05-08 ARM GENERAL | 2026-05-09 ARM NEON | Delta |
|---|---|---:|---:|---:|
| 10k | Load (ms) | 653.710 | 675.492 | slower |
| 10k | Build (ms) | 3343.997 | 3621.935 | slower |
| 10k | Query (ms) | 4221.737 | 4012.295 | faster |
| 10k | QPS | 47.374 | 49.847 | higher |
| 100k | Load (ms) | 7190.675 | 6036.217 | faster |
| 100k | Build (ms) | 50600.905 | 51100.431 | similar |
| 100k | Query (ms) | 36256.395 | 36182.889 | similar |
| 100k | QPS | 5.516 | 5.527 | similar |
| 1M cold | Load (ms) | 80249.436 | 69513.162 | faster |
| 1M cold | Build (ms) | 727355.502 | 711103.568 | faster |
| 1M cold | Query (ms) | 117733.467 | 118598.167 | similar |
| 1M warm | Query (ms) | 565.444 | 487.352 | faster |
| 1M warm | QPS | 353.705 | 410.381 | higher |

### 7.2 Recall Stability

Recall stayed unchanged across the retest:

- `10k`: `Recall@10 = 0.999500`, `Recall@100 = 0.995050`
- `100k`: `Recall@10 = 0.997500`, `Recall@100 = 0.974600`
- `1M`: `Recall@10 = 0.986000`, `Recall@100 = 0.940750`

This is the most important signal:

- enabling the NEON path did not change ANN quality in the tested workload
- the changes are performance-path changes, not algorithm-path changes

## 8. Conclusions

- PostgreSQL on the ARM server can now build, install, preload, and execute `pg_vexdb` with `NEONV8` selected for the tested float-vector path.
- The current branch as-is was still not directly loadable on ARM; a small local patch in `src/distance/core/neon_dispatcher.cpp` was required to make the new NEON path compatible with the existing `INT8` dispatcher references.
- After that compatibility patch:
  - `Architecture Usage = NEONV8` was confirmed
  - `10k / 100k / 1M` SIFT benchmarks all completed successfully
  - recall remained identical to the previous ARM `GENERAL`-path run
- Performance improved modestly rather than dramatically:
  - most visible benefit was the `1M` warm query-only run
  - bulk build time also improved somewhat at `1M`
  - `100k` query time stayed roughly unchanged

## 9. Follow-Up

The NEON path is now testable, but the repository still has a structural gap:

- `INT8` NEON dispatch is not fully implemented end-to-end
- the current local patch only provides a compatibility bridge so PostgreSQL ARM can load and run the float/half path cleanly

Recommended next step:

1. make `INT8` NEON distance/transform support complete in the shared dispatcher layer
2. remove the local compatibility bridge
3. rerun the same ARM benchmark again to validate that the final ARM SIMD implementation is self-contained
