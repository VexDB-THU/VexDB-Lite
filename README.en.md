# VexDB-Lite

**English** | **[中文](README.md)**

`VexDB-Lite` currently contains two vector-index integrations that share the same core graph algorithm and distance stack:

- `vexdb-pg`: PostgreSQL extension `pg_vexdb`
- `vexdb-duck`: DuckDB extension `vex`

Shared core directories:

- `include/graph_index/`: graph index headers and shared HNSW logic
- `distance/`, `src/distance/`: distance functions, ISA dispatch, transform templates
- `vtl/`: shared template/container layer
- `vexdb-duck/`: DuckDB integration layer
- `src/`, `include/`, `sql/`: PostgreSQL integration layer

---

## 1. Components

### 1.1 PostgreSQL: `pg_vexdb`

Current functionality:

- `floatvector(N)` and `halfvector(N)` types
- Distance operators/functions:
  - L2: `<->`
  - Inner product: `<#>`
  - Cosine: `<=>`
- `CREATE INDEX ... USING vexdb_graph`
- HNSW options such as `m`, `ef_construction`, `parallel_workers`
- runtime settings such as `pg_vexdb.ef_search`, `pg_vexdb.vec_architecture`
- optimizer/executor ANN index scan path
- shared-memory vector buffer manager and parallel build support

### 1.2 DuckDB: `vexdb-duck`

Current functionality:

- `GRAPH_INDEX` on `FLOAT[N]` vector columns
- Vector distance functions/operators:
  - `l2_distance`, `<->`
  - `inner_product`, `<#>`
  - `cosine_distance`, `<=>`, `<~>`
- `vector_dims()`, `l2_normalize()`, `vex_version()`, `vex_index_info()`
- `CREATE INDEX ... USING GRAPH_INDEX (vec [, metadata...])`
- optimizer rewrite into `VEX_INDEX_SCAN`
- filtered vector index syntax with metadata columns

Duck runtime settings:

- `vex_ef_search`
- `vex_brute_force_threshold`

---

## 2. PostgreSQL Syntax Examples

### 2.1 Install and Create Table

```sql
CREATE EXTENSION pg_vexdb;

CREATE TABLE items (
    id  BIGSERIAL PRIMARY KEY,
    vec floatvector(128)
);

INSERT INTO items (vec) VALUES
    ('[0.10, 0.20, 0.30]'),
    ('[0.40, 0.50, 0.60]');
```

### 2.2 Build Index

```sql
CREATE INDEX idx_items_vec
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (
    m = 16,
    ef_construction = 64
);
```

### 2.3 ANN Query

```sql
SET pg_vexdb.ef_search = 100;
SET enable_seqscan = off;

SELECT id, vec <-> '[0.15, 0.25, 0.35]' AS dist
FROM items
ORDER BY vec <-> '[0.15, 0.25, 0.35]'
LIMIT 10;
```

### 2.4 Other Metrics

```sql
SELECT id
FROM items
ORDER BY vec <#> '[0.15, 0.25, 0.35]'
LIMIT 10;

SELECT id
FROM items
ORDER BY vec <=> '[0.15, 0.25, 0.35]'
LIMIT 10;
```

### 2.5 `halfvector` Example

```sql
CREATE TABLE half_items (
    id  BIGSERIAL PRIMARY KEY,
    vec halfvector(128)
);

CREATE INDEX idx_half_items_vec
ON half_items
USING vexdb_graph (vec halfvector_l2_ops);
```

---

## 3. DuckDB Syntax Examples

### 3.1 Load Extension

```sql
LOAD '/path/to/vex.duckdb_extension';
SELECT vex_version();
```

Typical Python usage:

```python
import duckdb

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("LOAD '/path/to/vex.duckdb_extension'")
```

### 3.2 Create Table and Index

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

### 3.3 ANN Query

```sql
SET vex_ef_search = 100;

SELECT id
FROM items
ORDER BY l2_distance(vec, [0.15, 0.25, 0.35]::FLOAT[3])
LIMIT 10;
```

### 3.4 Filtered Index Example

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

### 3.5 Other Functions

```sql
SELECT inner_product([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT cosine_distance([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT vector_dims([1.0, 2.0, 3.0]::FLOAT[3]);
SELECT l2_normalize([3.0, 4.0]::FLOAT[2]);
SELECT * FROM vex_index_info();
```

---

## 4. Build

## 4.1 Build the PostgreSQL Variant

### Dependencies

- PostgreSQL 19 (currently aligned to `19devel`)
- CMake
- C++17 compiler
- Boost headers

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

### Build `pg_vexdb`

```bash
cd /path/to/VexDB-Lite
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
shared_preload_libraries = 'pg_vexdb'
```

Then restart PostgreSQL and run:

```sql
CREATE EXTENSION pg_vexdb;
```

---

## 4.2 Build the DuckDB Variant

`vexdb-duck` is built as an out-of-tree DuckDB extension.

### Dependencies

- DuckDB source tree
- CMake
- C++17 compiler
- Boost headers

### Register the Local Extension in DuckDB

Add this to DuckDB's `extension/extension_config_local.cmake`:

```cmake
duckdb_extension_load(vex
    SOURCE_DIR "/path/to/VexDB-Lite/vexdb-duck"
    INCLUDE_DIR "/path/to/VexDB-Lite/vexdb-duck/include"
)
```

### Build the Loadable Extension

```bash
cd /path/to/duckdb/build
cmake .. -DOVERRIDE_GIT_DESCRIBE=v1.5.2
cmake --build . --target vex_loadable_extension -j$(nproc)
```

The artifact is typically:

```bash
/path/to/duckdb/build/extension/vex/vex.duckdb_extension
```

### Smoke / Benchmark

```bash
cd /path/to/VexDB-Lite
vexdb-duck/test/run_extension_function_smoke.sh /path/to/duckdb/build

vexdb-duck/test/run_sift_sql_benchmark.sh \
  /path/to/duckdb/build \
  10k \
  /path/to/VexDB-Lite/vexdb-duck/test/benchmark/data
```

---

## 5. Benchmark Results

Detailed reports:

- [x86 PostgreSQL report](docs/reports/2026-05-08-x86-pg19-release-benchmark-report.md)
- [ARM PostgreSQL GENERAL report](docs/reports/2026-05-08-arm-pg19-release-benchmark-report.md)
- [ARM PostgreSQL NEON retest report](docs/reports/2026-05-09-arm-pg19-neon-retest-report.md)
- [DuckDB v1.5.2 report](docs/reports/2026-04-30-duckdb-v1.5.2-build-and-benchmark-report.md)

### 5.1 PostgreSQL on x86_64 / Intel Xeon E5-2696 v4 / 62 GiB

| Scale | Load (ms) | Build (ms) | Query (ms) | QPS | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|---:|
| 10k | 454.730 | 2319.690 | 4707.020 | 42.4897 | 0.999500 | 0.995050 |
| 100k | 4499.110 | 29849.700 | 35467.700 | 5.63894 | 0.997500 | 0.974600 |
| 1M cold | 49720.795 | 440295.289 | 118939.861 | 1.682 | 0.986000 | 0.940750 |
| 1M warm | n/a | n/a | 421.385 | 474.626 | 0.986000 | 0.940750 |

### 5.2 PostgreSQL on ARM64 / Kirin 9000C / 15 GiB (NEONV8)

| Scale | Load (ms) | Build (ms) | Query (ms) | QPS | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|---:|
| 10k | 675.492 | 3621.935 | 4012.295 | 49.847 | 0.999500 | 0.995050 |
| 100k | 6036.217 | 51100.431 | 36182.889 | 5.527 | 0.997500 | 0.974600 |
| 1M cold | 69513.162 | 711103.568 | 118598.167 | 1.686 | 0.986000 | 0.940750 |
| 1M warm | n/a | n/a | 487.352 | 410.381 | 0.986000 | 0.940750 |

Notes:

- The default ARM numbers shown on the front page are now the `2026-05-09` `NEONV8` retest.
- `index_inspect()` confirmed `Architecture Usage = NEONV8` on the tested indexes.
- A small `INT8 -> GENERAL` compatibility bridge is still present so the current branch can preload cleanly on PostgreSQL ARM; see `2026-05-09-arm-pg19-neon-retest-report.md` for details.
- For the earlier pure-`GENERAL` ARM run, see `2026-05-08-arm-pg19-release-benchmark-report.md`.

### 5.3 DuckDB on Apple M3 Max / 128 GiB / Darwin arm64

Tested against a DuckDB source build with `OVERRIDE_GIT_DESCRIBE=v1.5.2`.

| Scale | Load (ms) | Build (ms) | Query (ms) | QPS | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|---:|
| 10k | 79.1983 | 4061.74 | 323.747 | 617.767 | 1.000000 | 0.999550 |
| 100k | 715.33 | 59281.3 | 382.046 | 523.498 | 1.000000 | 0.995650 |

Notes:

- Test machine: `MacBook Pro`, `Model Identifier: Mac15,9`, `Chip: Apple M3 Max`, `Memory: 128 GB`
- The Duck arm64 build also ran with `GENERAL` distance dispatch for the current repository state

---

## 6. Known Limitations

### PostgreSQL

- Primary validation target is PostgreSQL 19
- ARM PG SIMD is not fully wired back yet; current state prioritizes correctness/buildability
- WAL/quantizer work is still incomplete compared to the full roadmap

### DuckDB

- Current focus is `GRAPH_INDEX`, optimizer integration, and shared-algorithm alignment
- Some accepted options such as `threads` and `pq_m` are currently compatibility placeholders on parts of the path
- ARM Duck builds also currently rely on `GENERAL` distance dispatch

---

## 7. Where To Look Next

- PostgreSQL implementation: `src/`, `include/`, `sql/`
- DuckDB implementation: [vexdb-duck/README.md](vexdb-duck/README.md) and `vexdb-duck/`
- Benchmark/environment records:
  - `docs/reports/2026-05-08-x86-pg19-release-benchmark-report.md`
  - `docs/reports/2026-05-08-arm-pg19-release-benchmark-report.md`
  - `docs/reports/2026-05-09-arm-pg19-neon-retest-report.md`
  - `docs/reports/2026-04-30-duckdb-v1.5.2-build-and-benchmark-report.md`
