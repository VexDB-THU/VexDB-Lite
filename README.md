# VexDB-Lite

**[English](README.en.md)** | **中文**

`VexDB-Lite` 当前包含两条共享算法内核的向量索引实现：

- `vexdb-pg`：PostgreSQL `pg_vexdb` 扩展，提供 `floatvector/halfvector`、距离运算符、`vexdb_graph` HNSW 索引访问方法
- `vexdb-duck`：DuckDB `vex` 扩展，提供 `GRAPH_INDEX`、向量距离函数、优化器 `VEX_INDEX_SCAN` 计划生成

两者尽量复用同一套图索引算法、距离分发和底层模板库，重点目录包括：

- `include/graph_index/`：图索引头文件与共享算法入口
- `distance/`、`src/distance/`：距离函数、ISA 分发、变换模板
- `vtl/`：共享模板容器
- `vexdb-duck/`：DuckDB 扩展层
- `src/`、`include/`、`sql/`：PostgreSQL 扩展层

---

## 1. 组件概览

### 1.1 PostgreSQL：`pg_vexdb`

当前能力：

- `floatvector(N)`、`halfvector(N)` 向量类型
- 距离函数与运算符：
  - L2：`<->`
  - Inner Product：`<#>`
  - Cosine：`<=>`
- `CREATE INDEX ... USING vexdb_graph`
- `m`、`ef_construction`、`parallel_workers` 等索引参数
- `pg_vexdb.ef_search`、`pg_vexdb.vec_architecture` 等运行参数
- 优化器生成 Index Scan，执行器走 ANN 索引检索
- 共享内存向量缓存、并行建索引

### 1.2 DuckDB：`vexdb-duck`

当前能力：

- `FLOAT[N]` 向量列上的 `GRAPH_INDEX`
- 向量距离函数与运算符：
  - `l2_distance`、`<->`
  - `inner_product`、`<#>`
  - `cosine_distance`、`<=>`、`<~>`
- `vector_dims()`、`l2_normalize()`、`vex_version()`、`vex_index_info()`
- `CREATE INDEX ... USING GRAPH_INDEX (vec [, metadata...])`
- DuckDB 优化器生成 `VEX_INDEX_SCAN`
- 支持带 metadata 列的过滤索引语法

当前 Duck 侧运行参数：

- `vex_ef_search`
- `vex_brute_force_threshold`

---

## 2. PostgreSQL 语法示例

### 2.1 安装与建表

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

### 2.2 建索引

```sql
CREATE INDEX idx_items_vec
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (
    m = 16,
    ef_construction = 64
);
```

### 2.3 ANN 查询

```sql
SET pg_vexdb.ef_search = 100;
SET enable_seqscan = off;

SELECT id, vec <-> '[0.15, 0.25, 0.35]' AS dist
FROM items
ORDER BY vec <-> '[0.15, 0.25, 0.35]'
LIMIT 10;
```

### 2.4 其他距离

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

### 2.5 halfvector 示例

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

## 3. DuckDB 语法示例

### 3.1 加载扩展

```sql
LOAD '/path/to/vex.duckdb_extension';
SELECT vex_version();
```

Python 侧常见用法：

```python
import duckdb

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("LOAD '/path/to/vex.duckdb_extension'")
```

### 3.2 建表与建索引

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

### 3.3 ANN 查询

```sql
SET vex_ef_search = 100;

SELECT id
FROM items
ORDER BY l2_distance(vec, [0.15, 0.25, 0.35]::FLOAT[3])
LIMIT 10;
```

### 3.4 过滤索引示例

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

### 3.5 其他距离函数

```sql
SELECT inner_product([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT cosine_distance([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT vector_dims([1.0, 2.0, 3.0]::FLOAT[3]);
SELECT l2_normalize([3.0, 4.0]::FLOAT[2]);
SELECT * FROM vex_index_info();
```

---

## 4. 构建方法

## 4.1 构建 PostgreSQL 版本

### 依赖

- PostgreSQL 19（当前按 `19devel` 适配）
- CMake
- C++17 编译器
- Boost（头文件）

### 编译 PostgreSQL（release 示例）

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

### 编译 `pg_vexdb`

```bash
cd /path/to/VexDB-Lite
mkdir -p build-pg19rel-release
cd build-pg19rel-release

export PG_CONFIG=/opt/postgresql-19rel-install/bin/pg_config
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
make install
```

### 启动前配置

`postgresql.conf` 至少需要：

```conf
shared_preload_libraries = 'pg_vexdb'
```

重启实例后：

```sql
CREATE EXTENSION pg_vexdb;
```

---

## 4.2 构建 DuckDB 版本

`vexdb-duck` 按 DuckDB out-of-tree extension 方式构建。

### 依赖

- DuckDB 源码树
- CMake
- C++17 编译器
- Boost（头文件）

### 在 DuckDB 中注册本地扩展

在 DuckDB 源码目录的 `extension/extension_config_local.cmake` 中加入：

```cmake
duckdb_extension_load(vex
    SOURCE_DIR "/path/to/VexDB-Lite/vexdb-duck"
    INCLUDE_DIR "/path/to/VexDB-Lite/vexdb-duck/include"
)
```

### 构建 loadable extension

```bash
cd /path/to/duckdb/build
cmake .. -DOVERRIDE_GIT_DESCRIBE=v1.5.2
cmake --build . --target vex_loadable_extension -j$(nproc)
```

生成物通常位于：

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

## 5. 测试结果

下面只汇总当前已经落报告的基准结果，详细环境见：

- [x86 PostgreSQL 报告](docs/reports/2026-05-08-x86-pg19-release-benchmark-report.md)
- [ARM PostgreSQL 报告](docs/reports/2026-05-08-arm-pg19-release-benchmark-report.md)
- [DuckDB v1.5.2 报告](docs/reports/2026-04-30-duckdb-v1.5.2-build-and-benchmark-report.md)

### 5.1 PostgreSQL：x86_64 / Intel Xeon E5-2696 v4 / 62 GiB

| 规模 | Load (ms) | Build (ms) | Query (ms) | QPS | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|---:|
| 10k | 454.730 | 2319.690 | 4707.020 | 42.4897 | 0.999500 | 0.995050 |
| 100k | 4499.110 | 29849.700 | 35467.700 | 5.63894 | 0.997500 | 0.974600 |
| 1M cold | 49720.795 | 440295.289 | 118939.861 | 1.682 | 0.986000 | 0.940750 |
| 1M warm | n/a | n/a | 421.385 | 474.626 | 0.986000 | 0.940750 |

### 5.2 PostgreSQL：ARM64 / Kirin 9000C / 15 GiB

| 规模 | Load (ms) | Build (ms) | Query (ms) | QPS | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|---:|
| 10k | 653.710 | 3343.997 | 4221.737 | 47.374 | 0.999500 | 0.995050 |
| 100k | 7190.675 | 50600.905 | 36256.395 | 5.516 | 0.997500 | 0.974600 |
| 1M cold | 80249.436 | 727355.502 | 117733.467 | 1.699 | 0.986000 | 0.940750 |
| 1M warm | n/a | n/a | 565.444 | 353.705 | 0.986000 | 0.940750 |

说明：

- ARM 测试为了完成当前仓库状态下的 PG 编译，临时关闭了 PG 侧 ARM `NEON/SVE` 距离派发，改走 `GENERAL` 路径。
- 因此 ARM 报告代表“当前源码可运行版本”的性能，不代表 ARM SIMD fully enabled 的上限。

### 5.3 DuckDB：Apple M3 Max / 128 GiB / Darwin arm64

测试版本：DuckDB 源码树构建，`OVERRIDE_GIT_DESCRIBE=v1.5.2`

| 规模 | Load (ms) | Build (ms) | Query (ms) | QPS | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|---:|---:|
| 10k | 79.1983 | 4061.74 | 323.747 | 617.767 | 1.000000 | 0.999550 |
| 100k | 715.33 | 59281.3 | 382.046 | 523.498 | 1.000000 | 0.995650 |

说明：

- 当时的 DuckDB 构建环境为 Apple Silicon，本机 `Model Identifier: Mac15,9`，`Chip: Apple M3 Max`，`Memory: 128 GB`
- Duck arm64 测试同样为了当前仓库可编译，走了 `GENERAL` 距离派发

---

## 6. 当前已知限制

### PostgreSQL

- 当前主验证平台是 PostgreSQL 19
- ARM PG 侧 SIMD 还没有完全接回；当前是可运行优先
- 向量存储、buffer/cache、并行构建都已经接通，但 WAL 与量化器仍有待继续完善

### DuckDB

- Duck 扩展当前重点是 `GRAPH_INDEX`、优化器接入和共享算法对齐
- `threads`、`pq_m` 选项目前接受但部分路径仍是兼容保留/未完全实现
- ARM Duck 构建当前也走 `GENERAL` 距离派发

---

## 7. 仓库说明

如果你只关心某一部分：

- PostgreSQL 版本：直接看当前目录下的 `src/`、`include/`、`sql/`
- DuckDB 版本：直接看 [vexdb-duck/README.md](vexdb-duck/README.md) 和 `vexdb-duck/`

如果你关心最近的测试与环境记录：

- `docs/reports/2026-05-08-x86-pg19-release-benchmark-report.md`
- `docs/reports/2026-05-08-arm-pg19-release-benchmark-report.md`
- `docs/reports/2026-04-30-duckdb-v1.5.2-build-and-benchmark-report.md`
