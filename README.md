# VexDB

**English** | **[中文](README.zh.md)**

`VexDB-Lite` is a vector similarity search engine for PostgreSQL, DuckDB, and SQLite. The backends share the same graph index algorithm, SIMD distance dispatch, and PQ/RaBitQ quantization kernels.

> See [vexdb_duckdb/README.md](vexdb_duckdb/README.md) for the DuckDB extension docs.  
> This root README is a project-level overview and build guide.

**Latest release: [v0.0.17](https://github.com/VexDB-THU/VexDB-Lite/releases/tag/v0.0.17)**

- PostgreSQL 16–19 packages for Linux x86_64 and AArch64
- DuckDB v1.5.2 packages for Linux x86_64 and AArch64
- SQLite packages for Linux, macOS, iOS, Android, and WASM
- `SHA256SUMS.txt` covering all 30 release archives

---

## 1. Components

### 1.1 PostgreSQL: `vexdb_lite`

Current functionality:

- `floatvector(N)` type
- Distance functions and operators:
  - `l2_distance` (`<->`)
  - `cosine_distance` (`<=>`)
  - `inner_product` (use `<~>` for negative inner product / MIPS)
- Scalar helpers: `vector_dims()`, `vector_norm()`, `l2_normalize()`, `vexdb_index_info()`
- `CREATE INDEX ... USING vexdb_graph`
- Index options: `m`, `ef_construction`, `parallel_workers` (parallel build), `quantizer` (`pq` / `rabitq`), `pq_m`, and `memory_mode`
- PQ and RaBitQ code-only storage; `memory_mode='compact'` makes quantizer activation mandatory
- Optimizer rewrite into an ANN Index Scan
- Shared-memory vector buffer cache and parallel index build
- Runtime settings: `vexdb.ef_search`, `vexdb.vec_architecture`

### 1.2 DuckDB: `vexdb_lite`

Current functionality:

- `GRAPH_INDEX` on `FLOAT[N]` vector columns
- Distance functions and operators:
  - `l2_distance` (`<->`)
  - `cosine_distance` (`<=>`)
  - `inner_product` (use `<~>` for negative inner product / MIPS)
  > Note: `<#>` is unavailable in DuckDB (`#` clashes with its comment syntax); use `<~>` for negative inner product — same meaning as in PG.
- Scalar helpers: `vector_dims()`, `l2_normalize()`, `vexdb_version()`, `vexdb_index_info()`
- `CREATE INDEX ... USING GRAPH_INDEX (vec [, metadata...])` with metadata filtering
- Index options: `m`, `ef_construction`, `parallel_workers` (parallel build), `quantizer` (`pq` / `rabitq`) and `pq_m`
- Both PQ and RaBitQ support `memory_mode='compact'`; compact RaBitQ still traverses the graph using quantized codes
- Optimizer rewrite into `VEXDB_INDEX_SCAN`
- Vector buffer cache and parallel index build
- Runtime settings: `vexdb_ef_search`, `vexdb_brute_force_threshold`, `vexdb_pq_search_mode`, `vexdb_pq_refine_k_factor`

### 1.3 SQLite: `vexdb_lite`

See [vexdb_sqlite/README.md](vexdb_sqlite/README.md) for the full API and build guide. Current functionality:

- `GRAPH_INDEX` virtual tables with shadow-table persistence
- L2, cosine, and inner-product search with JSON or float32 BLOB vectors
- Incremental insert, update, delete, transaction rollback, and reopen recovery
- Metadata filtering, `LIMIT` pushdown, and parallel graph construction
- PQ and RaBitQ in full or compact memory mode
- Loadable desktop extension plus static registration for iOS, Android, and WASM

---

## 2. Capability Matrix
### 2.1 PG Extension Comparison (PGVector vs VexDB-Lite vs VexDB)

| Category | Feature | Description | PGVector | VexDB-Lite (open-source) | VexDB (commercial) |
|---|---|---|:---:|:---:|:---:|
| Graph Index | graph_index | A fully self-developed high-performance graph index that merges the advantages of various graph indexes and works seamlessly across all scenarios.| ❌ | ✅ | ✅ |
| Distance | Distance function dispatch | Inlined distance functions, compile-time optimized | ❌ | ✅ | ✅ |
| Cache | vector buffer | General vector cache, all scenarios | ❌ | ✅ | ✅ |
| Cache | bulk buffer | Full in-memory vector cache for max throughput | ❌ | ❌ | ✅ |
| Cache | Async I/O cache | Accelerated disk-to-cache reads under memory pressure | ❌ | ❌ | ✅ |
| Data types | floatvector | Standard float32 vector type | ✅ | ✅ | ✅ |
| Data types | halfvector | Float16 vector type | ✅ | 🟡 | ✅ |
| Data types | int8vector | Int8 vector type | ❌ | 🟡 | ✅ |
| Quantization | PQ quantization | Maximum compression, QPS close to raw vectors | ❌ | 🟡 | ✅ |
| Quantization | RaBitQ quantization | Quantized graph traversal with code-only index storage | ❌ | ✅ | ✅ |
| Quantization | Auto quantization | Background auto-enable, supports empty-table index build | ❌ | ❌ | ✅ |
| Graph index enhancement | Async insert | Fast ingestion for write-heavy workloads | ❌ | ❌ | ✅ |
| Graph index enhancement | Graph sharding | Large-scale vectors on small-memory machines | ❌ | ❌ | ✅ |
| Graph index enhancement | Subgraph index build | Continue using memory for index building even in low-memory scenarios to accelerate build speed | ❌ | ❌ | ✅ |
| HA | Primary-replica HA | Synchronous replication and backup restore | ✅ | ❌ | ✅ |
| Maintenance | Parallel vacuum | Parallel index cleanup and reclaim | ❌ | ❌ | ✅ |


### 2.2 DuckDB Extension Comparison (DuckDB VSS vs VexDB-Lite)

| Category | Feature | Description | DuckDB VSS | VexDB-Lite (`vexdb_lite`) |
|---|---|---|:---:|:---:|
| Index | Graph index | VSS: HNSW; VexDB: graph_index (self-developed hybrid) | ✅ | ✅ |
| Distance | SIMD dispatch | Inlined distance functions, compile-time optimized | ❌ | ✅ |
| Quantization | PQ | Vector compression for memory-constrained scenarios | ❌ | ✅ |
| Quantization | RaBitQ | Shared quantized graph traversal with optional compact index-side storage | ❌ | ✅ |
| Cache | Buffer management | Disk-to-memory vector caching | ❌ | ✅ |
| Maintenance | Index compaction | Reclaim space from soft-deleted entries | ✅ | ❌ |
| Search | Filtered ANN search | WHERE filter with automatic oversampling | ❌ | ✅ |
| Persistence | Disk-backed index | Index survives database restart without rebuild | ✅† | ✅ |

† VSS persistence is experimental — WAL recovery is not implemented, unexpected shutdowns may cause index corruption. VexDB-Lite persists via DuckDB's standard serialization.

---

✅ Supported · 🟡 Coming soon · ❌ Not included in open-source edition

## 3. PostgreSQL Syntax Examples

### 3.1 Install and Create Table

```sql
CREATE EXTENSION vexdb_lite;

CREATE TABLE items (
    id  BIGSERIAL PRIMARY KEY,
    vec floatvector(128)
);

INSERT INTO items (vec) VALUES
    ('[0.10, 0.20, 0.30]'),
    ('[0.40, 0.50, 0.60]');
```

### 3.2 Build Index

```sql
CREATE INDEX idx_items_vec
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (
    m = 16,
    ef_construction = 64
);
```

#### 3.2.1 PQ Quantized Index (v1)

PQ reduces vector storage by encoding indexed vectors with a trained codebook:

```sql
SET maintenance_work_mem = '2GB';   -- required; low memory falls back to a plain graph index

CREATE INDEX idx_pq
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (quantizer = 'pq', pq_m = 4);
```

Current v1 behavior:

- If there are fewer than 256 training samples, or the memory build budget is too low, PQ falls back to a plain graph index with a NOTICE.
- PQ indexes support post-build `INSERT` / `UPDATE` / `DELETE`; new vectors are encoded with the trained codebook.
- For `parallel_workers > 0`, PQ training still runs in the leader process; PG parallel workers only participate in the disk-build phase.

Check the active index state:

```sql
SELECT indexname, use_pq, pq_m
FROM vexdb_index_info()
WHERE indexname = 'idx_pq';
```

#### 3.2.2 RaBitQ Index

```sql
CREATE INDEX idx_rabitq
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (quantizer = 'rabitq', m = 16, ef_construction = 100);
```

PG trains RaBitQ only when at least 10,000 samples fit in `maintenance_work_mem`. In the default `full` mode, an unavailable quantizer emits a NOTICE and falls back to a plain graph. With `memory_mode='compact'`, activation is mandatory and the build fails instead of writing raw vectors:

```sql
CREATE INDEX idx_rabitq_compact
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (quantizer = 'rabitq', memory_mode = 'compact');
```

PG quantized indexes already store only PQ/RaBitQ codes in the index vector fork; they do not keep a second raw-vector mirror. `compact` therefore acts as a strict storage contract rather than a second compression pass. Omitting `quantizer` in compact mode selects PQ; `quantizer='none'` is rejected. Compact indexes are not supported on unlogged tables. `ALTER INDEX ... SET (memory_mode='compact')` does not rewrite an existing raw index; use `REINDEX` and verify the effective state through `vexdb_index_info().memory_mode` plus `index_inspect()` attributes `Working Quantizer` and `Vector Storage`.

### 3.3 ANN Query

```sql
SET vexdb.ef_search = 100;
SET enable_seqscan = off;

SELECT id, vec <-> '[0.15, 0.25, 0.35]' AS dist
FROM items
ORDER BY vec <-> '[0.15, 0.25, 0.35]'
LIMIT 10;
```

### 3.4 Other Metrics

```sql
SELECT id
FROM items
ORDER BY vec <~> '[0.15, 0.25, 0.35]'
LIMIT 10;

SELECT id
FROM items
ORDER BY vec <=> '[0.15, 0.25, 0.35]'
LIMIT 10;
```

---

## 4. DuckDB Syntax Examples

### 4.1 Load Extension

```sql
LOAD '/path/to/vexdb_lite.duckdb_extension';
SELECT vexdb_version();
```

Typical Python usage:

```python
import duckdb

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("LOAD '/path/to/vexdb_lite.duckdb_extension'")
```

### 4.2 Create Table and Index

```sql
CREATE TABLE items (
    id       INTEGER,
    category VARCHAR,
    vec      FLOAT[128]
);

CREATE INDEX idx_items_vec
ON items
USING GRAPH_INDEX (vec)
WITH (
    metric = 'l2',
    m = 16,
    ef_construction = 64
);
```

### 4.3 ANN Query

```sql
SET vexdb_ef_search = 100;

SELECT id
FROM items
ORDER BY l2_distance(vec, [0.15, 0.25, 0.35]::FLOAT[3])
LIMIT 10;
```

### 4.4 Filtered Index Example

```sql
CREATE INDEX idx_items_vec_meta
ON items
USING GRAPH_INDEX (vec, category);

SELECT id
FROM items
WHERE category = 'book'
ORDER BY l2_distance(vec, [0.15, 0.25, 0.35]::FLOAT[3])
LIMIT 10;
```

### 4.5 Other Functions

```sql
SELECT inner_product([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT cosine_distance([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT vector_dims([1.0, 2.0, 3.0]::FLOAT[3]);
SELECT l2_normalize([3.0, 4.0]::FLOAT[2]);
SELECT * FROM vexdb_index_info();
```

---

## 5. Build

**Prebuilt packages are recommended.** Download the archive for your database, platform, and architecture from the [v0.0.17 release](https://github.com/VexDB-THU/VexDB-Lite/releases/tag/v0.0.17), then verify it with the published checksum manifest:

```bash
shasum -a 256 -c SHA256SUMS.txt
```

SQLite package names include `linux-x86_64`, `linux-aarch64`, `macos-arm64`, `macos-x86_64`, `ios-xcframework`, `android-arm64-v8a`, `android-x86_64`, and `wasm`. Windows prebuilt packages are not available in v0.0.17.

### 5.1 Build the PostgreSQL Variant

### Dependencies

- PostgreSQL 16 ~ 19 (PG 16/17/18/19 supported; primary validation target is `19devel`)
- CMake ≥ 3.14
- C++17 compiler (GCC 9+ or Clang 10+)

### Build PostgreSQL (release example)

```bash
cd /path/to/postgresql-19-source
./configure \
  --prefix=/opt/postgresql-19rel-install \
  --without-icu \
  --without-readline \
  --without-zlib \
  CFLAGS="-O3 -DNDEBUG"
make -j$(nproc)
make install
```

### Build `vexdb_lite`

```bash
cd /path/to/VexDB
mkdir -p build-pg19rel-release
cd build-pg19rel-release

export PG_CONFIG=/opt/postgresql-19rel-install/bin/pg_config
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
make install
```

### PostgreSQL Configuration

At minimum:

```conf
shared_preload_libraries = 'vexdb_lite'
```

Then restart PostgreSQL and run:

```sql
CREATE EXTENSION vexdb_lite;
```

---

### 5.2 Build the DuckDB Variant

**Recommended: use `build_duck.sh`** — it handles DuckDB clone, cmake configuration, compilation, and metadata processing in one command.

```bash
bash build_duck.sh setup   # First time: clone DuckDB v1.5.2 and cmake configure
bash build_duck.sh build   # Compile the extension (incremental)
```

Output: `build/duck/build/extension/vexdb_lite/vexdb_lite.duckdb_extension`

### Dependencies

- CMake ≥ 3.28 and < 4.x
- C++17 compiler (GCC 9+ or Clang 10+)
- Git

### Why a shell script and not plain cmake?

DuckDB extensions must be compiled inside DuckDB's source tree — you cannot run `cmake -B build vexdb_duckdb/` standalone. `build_duck.sh` automates:
1. Cloning DuckDB v1.5.2
2. Writing `extension_config_local.cmake` to register the vexdb_lite extension
3. Running `cmake` + `cmake --build`
4. Appending the extension metadata footer (required by DuckDB's release format)

---

## 6. Running Tests

### DuckDB Extension Tests

```bash
bash build_duck.sh build          # Build the extension
bash tests/spec/_lib/docker/run_duckdb.sh test  # Run full spec tests (requires Docker)
```

### PostgreSQL Plugin Tests

```bash
bash tests/spec/_lib/docker/run_pg.sh test      # Run PG spec tests (requires Docker + PG19)
```

### SQLite Extension Tests

```bash
bash build_sqlite.sh test
bash tests/spec/_lib/docker/run_sqlite.sh test
```

Tests are driven by a YAML spec DSL; test files live under `tests/spec/`.

The v0.0.17 release passed 127/127 DuckDB tests on Linux x86_64 and AArch64, 88/88 PostgreSQL 19 specs on AArch64, and 32/32 SQLite specs against SQLite 3.46.0 and 3.53.4. The iOS Simulator suite also passed M0, M1, SIMD, M2, M3, and M3+ scenarios, including the 40,000-vector parallel-build test with recall@10 of 1.0000.

---

## 7. Benchmark Results

Dataset: SIFT-1M 128-dim, `m=16`, `ef_construction=128`. Columns: `QPS (reads=1)` / `QPS (reads=16)` / `Recall@10`.

Test environment: Intel Core Ultra 7-265K (20c/20t, 3.9 GHz) / 16 GB DDR5 / x86_64 Linux

### 7.1 Comparison with pgvector / VSS (x86_64)

**ef_search = 50**

| System | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 507.9 | 7153.5 | 96.22% |
| **vexdb_lite (PostgreSQL)** | **994.7** | **12084.6** | 95.97% |
| **vexdb_lite (DuckDB)** | **717.5** | **8667.8** | 95.06% |
| duckdb-vss | 496.1 | 5360.9 | 94.07% |

**ef_search = 100**

| System | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 313.4 | 4272.5 | 98.82% |
| **vexdb_lite (PostgreSQL)** | **618.5** | **7883.1** | 98.62% |
| **vexdb_lite (DuckDB)** | **547.2** | **5379.1** | 98.40% |
| duckdb-vss | 405.2 | 4433.3 | 98.04% |

**ef_search = 200**

| System | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 193.1 | 2694.1 | 99.66% |
| **vexdb_lite (PostgreSQL)** | **421.3** | **5038.0** | 99.58% |
| **vexdb_lite (DuckDB)** | **383.6** | **4298.8** | 99.53% |
| duckdb-vss | 321.9 | 3809.3 | 99.42% |

---

## 8. Known Limitations

### PostgreSQL

- Supports PostgreSQL 16 ~ 19; primary validation target is PostgreSQL 19

### DuckDB

- `threads` and `pq_m` options are compatibility placeholders on some code paths
- ARM Duck builds currently use scalar (`GENERAL`) distance dispatch without SIMD acceleration

### SQLite and release packages

- iOS, Android, and WASM use static registration; runtime `.load` is for desktop and server builds
- v0.0.17 does not include Windows prebuilt packages

## 9. Repository Structure

| Directory | Description |
|---|---|
| `common/` | Shared core: graph index algorithm, SIMD distance dispatch, quantizer (PQ/RaBitQ), template containers |
| `vexdb_pg/` | PostgreSQL extension: index AM, build, search, DML, WAL, distance entry |
| `vexdb_duckdb/` | DuckDB extension: index lifecycle, optimizer rewrite, distance functions → [README](vexdb_duckdb/README.md) |
| `vexdb_sqlite/` | SQLite extension: virtual table, shadow-table persistence, PQ/RaBitQ → [README](vexdb_sqlite/README.md) |
| `documentation/` | Feature docs, build guide |
| `tests/spec/` | YAML-based spec tests (shared / pg / duckdb / sqlite) |
| `scripts/` | Build, release, and packaging scripts |
| `thirdparties/` | Vendored dependencies (patched Boost) |

---

## Community

| Channel | Description |
|---|---|
| [GitHub Issues](https://github.com/VexDB-THU/VexDB-Lite/issues) | Bug reports and feature requests |
| [GitHub Discussions](https://github.com/VexDB-THU/VexDB-Lite/discussions) | Questions, proposals and general discussion |
| [Discord](https://discord.gg/Ge4kaFak) | Real-time chat and Q&A |
| WeChat Group | Scan QR code at [vexdb.com/community](https://vexdb.com/community) · Chinese community |

---

## License

MIT License. See [LICENSE](LICENSE).
