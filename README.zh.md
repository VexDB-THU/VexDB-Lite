# VexDB

**[English](README.md)** | **[中文](README.zh.md)** | **[日本語](README.ja.md)**

`VexDB` 当前包含两条共享算法内核的向量索引实现：

- **`vexdb_pg`** (PostgreSQL `vexdb_vector` 扩展)：`floatvector`、距离运算符（`<->` `<#>` `<=>`）、`vexdb_graph` HNSW 索引访问方法
- **`vexdb_duckdb`** (DuckDB `vex` 扩展) → [详细文档](vexdb_duckdb/README.md)：`GRAPH_INDEX`、`l2_distance` / `cosine_distance` / `inner_product` 等函数、优化器 `VEX_INDEX_SCAN` 计划生成

> 子 README 包含完整的安装/建表/查询/PQ/GUC/Python/故障排查；本根 README 只做项目级综述与构建总览。

两者尽量复用同一套图索引算法、距离分发和底层模板库，重点目录包括：

- `include/graph_index/`：图索引头文件与共享算法入口
- `distance/`、`src/distance/`：距离函数、ISA 分发、变换模板
- `vtl/`：共享模板容器
- `vexdb_duckdb/`：DuckDB 扩展层
- `src/`、`include/`、`sql/`：PostgreSQL 扩展层

---

## 1. 组件概览

### 1.1 PostgreSQL：`vexdb_vector`

当前能力：

- `floatvector(N)` 向量类型
- 距离函数与运算符：
  - L2：`<->`
  - Inner Product：`<#>`
  - Cosine：`<=>`
- `CREATE INDEX ... USING vexdb_graph`
- `m`、`ef_construction`、`parallel_workers` 等索引参数
- `vexdb_vector.ef_search`、`vexdb_vector.vec_architecture` 等运行参数
- 优化器生成 Index Scan，执行器走 ANN 索引检索
- 共享内存向量缓存、并行建索引

### 1.2 DuckDB：`vexdb_duckdb`

详见 [vexdb_duckdb/README.md](vexdb_duckdb/README.md)。当前能力：

- `FLOAT[N]` 向量列上的 `GRAPH_INDEX`
- 距离函数：`l2_distance` / `inner_product` / `cosine_distance` / `list_negative_inner_product`
  > 注：DuckDB parser 不支持 pgvector 风格的 `<->` 操作符；统一用函数形式
- 标量工具：`vector_dims()`、`l2_normalize()`、`vex_version()`、`vex_index_info()`
- `CREATE INDEX ... USING GRAPH_INDEX (vec [, metadata...])` 含元数据过滤
- DuckDB 优化器生成 `VEX_INDEX_SCAN`
- 产品量化 PQ + compact 模式（百亿级内存优化）

运行参数：`vex_ef_search`、`vex_brute_force_threshold`、`vex_pq_search_mode`、`vex_pq_refine_k_factor`

---

## 2. 产品能力矩阵

| 分类 | 功能 | 描述 | vexdb_vector（开源版） | VexDB（商用版） |
|---|---|---|:---:|:---:|
| 向量检索图索引 | graph_index 图索引 | 完全自研高性能图索引，融合多种图结构优势，全场景通用 | ✅ | ✅ |
| 距离计算 | 距离计算函数模板分发 | 内联距离计算函数，编译时优化 | ✅ | ✅ |
| 缓存 | vector buffer | 通用向量缓存，全场景通用 | ✅ | ✅ |
| 缓存 | bulk buffer | 全内存向量缓存，内存充足场景下最大加速 | ❌ | ✅ |
| 缓存 | 缓存异步 IO | 内存受限场景下加速磁盘到缓存数据读取 | ❌ | ✅ |
| 数据类型 | floatvector | 标准 float32 向量类型 | ✅ | ✅ |
| 数据类型 | halfvector | 半精度 float16 向量类型 | 🟡 | ✅ |
| 数据类型 | int8vector | int8 向量类型 | 🟡 | ✅ |
| 量化 | PQ 量化 | 向量压缩比最大，QPS 与原始向量相近 | ✅ | ✅ |
| 量化 | RaBitQ 量化 | 向量压缩比中等，QPS 优于原始向量 | 🟡 | ✅ |
| 量化 | 量化自动开启 | 后台自动开启量化，支持空表建量化索引 | ❌ | ✅ |
| 图索引增强 | 图索引异步插入 | 多写少读场景下快速入库 | ❌ | ✅ |
| 图索引增强 | 图挂桶功能 | 小规格机器承载大规模向量检索 | ❌ | ✅ |
| 高可用 | 主备高可用 | 支持主备同步与备份恢复 | ❌ | ✅ |
| 运维 | 并行 vacuum | 并行加速索引清理回收 | ❌ | ✅ |

✅ 已支持 · 🟡 即将支持 · ❌ 开源版不含

---

## 3. PostgreSQL 语法示例

### 2.1 安装与建表

```sql
CREATE EXTENSION vexdb_vector;

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

#### 2.2.1 PQ 量化索引（v1）

启用 PQ 可将索引存储压缩约 16×。当前 v1 版本的使用规约：

```sql
SET maintenance_work_mem = '2GB';   -- 必需，低于 1GB 会自动回落 plain HNSW
CREATE INDEX idx_pq ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (quantizer = 'pq', pq_m = 4);
```

**v1 已知限制**：

- `maintenance_work_mem < 1GB` 时 PQ 自动回落 plain HNSW（带 NOTICE 提示）
- **PQ 索引在 build 后是只读的**：`INSERT` / `UPDATE` / `DELETE` 触发的 aminsert 会被拒绝
  ```
  ERROR:  DML on a PQ-enabled vexdb_graph index is not yet supported
  HINT:   Drop and recreate the index after data changes, or use an index
          without quantizer='pq'.
  ```
  推荐工作流：**先批量写数据 → CREATE INDEX → 只读查询**。数据变更后 DROP + CREATE 重建索引。这与 FAISS 等向量库的 "build-once index" 模式一致。
- parallel build × PQ：走单线程（与 plain HNSW 行为一致）

**核对索引状态**：

```sql
SELECT indexname, use_pq, pq_m FROM vex_index_info()
WHERE indexname = 'idx_pq';
```

### 2.3 ANN 查询

```sql
SET vexdb_vector.ef_search = 100;
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
SET vexdb_vector.ef_search = 100;

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

> **预编译产物（推荐）**：见 [GitHub Releases](https://github.com/VexDB-THU/vexdb_lite/releases) 下载 `vex-duckdb-linux-<arch>.tar.gz` / `vexdb_vector-linux-<arch>-pg19.tar.gz`，无需本地编译。
>
> **从源码构建**：每个子项目的 README 有详细步骤：
> - DuckDB 扩展：[vexdb_duckdb/README.md#构建](vexdb_duckdb/README.md#构建)
> - PG 扩展：[编译构建指南](documentation/build-guide.md)
>
> **跨架构批量打包**（项目内部发版用）：
> ```bash
> bash scripts/release.sh build              # 远程 x86 + ARM 各 build 一份 → dist/
> bash scripts/release.sh package            # 打 tarball + SHA256SUMS → dist/release/
> bash scripts/release.sh upload v0.1.0      # gh release upload
> ```

## 4.1 构建 PostgreSQL 版本

### 依赖

- PostgreSQL 16 ~ 19（已适配 PG 16/17/18/19；主验证平台为 `19devel`）
- CMake ≥ 3.14
- C++17 编译器（GCC 9+ / Clang）
- Boost 头文件：仓库已自带打了 **PG 多进程并发 patch** 的 boost（`thirdparties/`，`concurrent_flat_map`
  的内部锁分片改用 `getpid()`，避免 fork 下所有 backend 聚到同一把锁），CMake 优先用它。它只 trim
  了核心模块（unordered / lockfree），`config` / `preprocessor` 等基础库靠**系统 boost 兜底**；
  系统无 boost 的 build 机用 `-DBOOST_FALLBACK_INC=/path/to/boost_root` 指定一份完整 boost 兜底。

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

### 编译 `vexdb_vector`

```bash
cd /path/to/VexDB
mkdir -p build-pg19rel-release
cd build-pg19rel-release

export PG_CONFIG=/opt/postgresql-19rel-install/bin/pg_config
cmake -DCMAKE_BUILD_TYPE=Release ..
# 系统无 boost(如 Kylin build 机)时,补一份完整 boost 兜底 vendored trim 掉的基础库:
#   cmake -DCMAKE_BUILD_TYPE=Release -DBOOST_FALLBACK_INC=/path/to/boost_root ..
make -j$(nproc)
make install
```

> **构建说明**：本仓已从旧 PGXS `Makefile` 统一到 CMake（`Makefile` 已删除）。`thirdparties/`
> 的 patched boost 优先于系统 boost；ISA 走运行时 dispatch（`GetBestArch` 按 CPU 选
> SSE/AVX/AVX-512），**不使用 `-march=native`**，保证发版产物跨 CPU 可移植。

### 启动前配置

`postgresql.conf` 至少需要：

```conf
shared_preload_libraries = 'vexdb_vector'
```

重启实例后：

```sql
CREATE EXTENSION vexdb_vector;
```

---

## 4.2 构建 DuckDB 版本

**推荐方式：使用 `build_duck.sh`**（封装了 DuckDB clone、cmake 配置、编译、元数据处理全流程）

```bash
bash build_duck.sh setup   # 首次：clone DuckDB v1.5.2 并 cmake configure
bash build_duck.sh build   # 编译扩展（增量）
```

生成物：`build/duck/build/extension/vex/vex.duckdb_extension`

### 依赖

- CMake 3.14+
- C++17 编译器（GCC 9+ 或 Clang 10+）
- Git

### 说明

DuckDB 扩展需嵌入 DuckDB 源码树编译，无法单独 `cmake -B build vexdb_duckdb/`。`build_duck.sh` 自动处理以下步骤：
1. clone DuckDB v1.5.2 源码
2. 写入 `extension_config_local.cmake` 注册 vex 扩展
3. 运行 `cmake` + `cmake --build`
4. 处理扩展元数据 footer（DuckDB 发版格式要求）

---

## 运行测试

### DuckDB 扩展测试

```bash
bash build_duck.sh build          # 构建扩展
bash tests/spec/_lib/docker/run_duckdb.sh test  # 运行全量 spec 测试（需 Docker）
```

### PostgreSQL 插件测试

```bash
bash tests/spec/_lib/docker/run_pg.sh test      # 运行 PG spec 测试（需 Docker + PG19）
```

测试框架基于 YAML spec DSL，测试文件位于 `tests/spec/`。

---

## 5. 测试结果

数据集：SIFT-1M 128 维，`m=16`，`ef_construction=128`。列含义：`QPS（reads=1）` / `QPS（reads=16）` / `Recall@10`。

测试环境：Intel Core Ultra 7-265K（20c/20t，3.9 GHz）/ 16 GB DDR5 / x86_64 Linux

### 5.1 与 pgvector / VSS 对比（x86_64）

**ef_search = 50**

| 系统 | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 507.9 | 7153.5 | 96.22% |
| **vexdb_vector (PostgreSQL)** | **994.7** | **12084.6** | 95.97% |
| **vexdb_vector (DuckDB)** | **717.5** | **8667.8** | 95.06% |
| duckdb-vss | 496.1 | 5360.9 | 94.07% |

**ef_search = 100**

| 系统 | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 313.4 | 4272.5 | 98.82% |
| **vexdb_vector (PostgreSQL)** | **618.5** | **7883.1** | 98.62% |
| **vexdb_vector (DuckDB)** | **547.2** | **5379.1** | 98.40% |
| duckdb-vss | 405.2 | 4433.3 | 98.04% |

**ef_search = 200**

| 系统 | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 193.1 | 2694.1 | 99.66% |
| **vexdb_vector (PostgreSQL)** | **421.3** | **5038.0** | 99.58% |
| **vexdb_vector (DuckDB)** | **383.6** | **4298.8** | 99.53% |
| duckdb-vss | 321.9 | 3809.3 | 99.42% |

---

## 6. 当前已知限制

详见 [docs/known-limitations/](docs/known-limitations/)。

### PostgreSQL

- 支持 PostgreSQL 16 ~ 19，当前主验证平台是 PostgreSQL 19
- ARM PG 侧 SIMD 还没有完全接回；当前是可运行优先
- 向量存储、buffer/cache、并行构建都已经接通，但 WAL 仍有待继续完善
- **PQ 量化（v1）**：CREATE INDEX + SELECT 端到端可用；要求 `maintenance_work_mem ≥ 1GB`；PQ 索引在 build 后只读，DML 会被拒绝（见 2.2.1）

### DuckDB

- Duck 扩展当前重点是 `GRAPH_INDEX`、优化器接入和共享算法对齐
- `threads`、`pq_m` 选项目前接受但部分路径仍是兼容保留/未完全实现
- ARM Duck 构建当前也走 `GENERAL` 距离派发

---

## 7. 仓库说明

如果你只关心某一部分：

- PostgreSQL 版本：直接看当前目录下的 `src/`、`include/`、`sql/`
- DuckDB 版本：直接看 [vexdb_duckdb/README.md](vexdb_duckdb/README.md) 和 `vexdb_duckdb/`

---

## 社区

| 渠道 | 说明 |
|---|---|
| [GitHub Issues](https://github.com/VexDB-THU/VexDB-Lite/issues) | Bug 反馈与功能建议 |
| [GitHub Discussions](https://github.com/VexDB-THU/VexDB-Lite/discussions) | 提问、提案与功能讨论 |
| [Discord](https://discord.gg/vexdb) | 实时交流、答疑解惑 |
| 官方微信社群 | 扫码加入，见 [vexdb.com/community](https://vexdb.com/community)，中文社区互助 |

---

## License

MIT License. 详见 [LICENSE](LICENSE)。

> 产品能力矩阵见 [product-capability-matrix.md](product-capability-matrix.md)。
