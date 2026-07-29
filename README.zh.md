# VexDB

**[English](README.md)** | **中文**

`VexDB-Lite` 是一个高性能向量检索系统，提供 PostgreSQL、DuckDB 和 SQLite 三种适配形式，共享同一套 graph_index 图索引算法、SIMD 距离分发和 PQ/RaBitQ 量化内核。

> DuckDB 扩展详见 [vexdb_duckdb/README.md](vexdb_duckdb/README.md)。  
> 本根 README 只做项目级综述与构建总览。

**最新版本：[v0.0.17](https://github.com/VexDB-THU/VexDB-Lite/releases/tag/v0.0.17)**

- PostgreSQL 16–19：Linux x86_64、AArch64
- DuckDB v1.5.2：Linux x86_64、AArch64
- SQLite：Linux、macOS、iOS、Android、WASM
- `SHA256SUMS.txt` 覆盖全部 30 个发布压缩包

---

## 1. 组件概览

### 1.1 PostgreSQL：`vexdb_lite`

当前能力：

- `floatvector(N)` 向量类型
- 距离函数与运算符：
  - `l2_distance`（`<->`）
  - `cosine_distance`（`<=>`）
  - `inner_product`（负内积/最大内积检索用 `<~>`）
- 标量工具：`vector_dims()`、`vector_norm()`、`l2_normalize()`、`vexdb_index_info()`
- `CREATE INDEX ... USING vexdb_graph`
- 索引参数：`m`、`ef_construction`、`parallel_workers`（并行构建）、`quantizer`（`pq` / `rabitq`）、`pq_m` 和 `memory_mode`
- PQ、RaBitQ code-only 存储；`memory_mode='compact'` 强制量化器必须成功激活
- 优化器生成 Index Scan，执行器走 ANN 索引检索
- 共享内存向量缓存、并行建索引
- 运行参数：`vexdb.ef_search`、`vexdb.vec_architecture`

### 1.2 DuckDB：`vexdb_lite`

详见 [vexdb_duckdb/README.md](vexdb_duckdb/README.md)。当前能力：

- `FLOAT[N]` 向量列上的 `GRAPH_INDEX`
- 距离函数与运算符：
  - `l2_distance`（`<->`）
  - `cosine_distance`（`<=>`）
  - `inner_product`（负内积/最大内积检索用 `<~>`）
- 标量工具：`vector_dims()`、`l2_normalize()`、`vexdb_version()`、`vexdb_index_info()`
- `CREATE INDEX ... USING GRAPH_INDEX (vec [, metadata...])`，支持元数据过滤
- 索引参数：`m`、`ef_construction`、`parallel_workers`（并行构建）、`quantizer`（`pq` / `rabitq`）和 `pq_m`
- PQ 和 RaBitQ 都支持 `memory_mode='compact'`；RaBitQ 在 compact 下仍走量化 code 图遍历
- 优化器生成 `VEXDB_INDEX_SCAN`
- 向量缓存、并行建索引
- 运行参数：`vexdb_ef_search`、`vexdb_brute_force_threshold`、`vexdb_pq_search_mode`、`vexdb_pq_refine_k_factor`

### 1.3 SQLite：`vexdb_lite`

完整 API 和构建方法见 [vexdb_sqlite/README.md](vexdb_sqlite/README.md)。当前能力：

- `GRAPH_INDEX` 虚拟表和 shadow table 持久化
- L2、cosine、inner product 检索，支持 JSON 和 float32 BLOB 向量
- 增量插入、更新、删除、事务回滚和关库重开恢复
- metadata 过滤、`LIMIT` 下推和并行建图
- PQ、RaBitQ 的 full / compact 模式
- 桌面端可加载扩展，以及 iOS、Android、WASM 静态注册

---

## 2. 产品能力矩阵
### 2.1 PostgreSQL 扩展对比（pgvector vs VexDB-Lite vs VexDB）

| 分类 | 功能 | 描述 | pgvector | vexdb-lite（开源版） | VexDB（商用版） |
|---|---|---|:---:|:---:|:---:|
| Graph Index | graph_index | 完全自研高性能图索引，融合多种图索引优势，全场景适用 | ❌ | ✅ | ✅ |
| 距离计算 | 距离计算函数模板分发 | 内联距离计算函数，编译时优化 | ❌ | ✅ | ✅ |
| 缓存 | vector buffer | 通用向量缓存，全场景适用 | ❌ | ✅ | ✅ |
| 缓存 | bulk buffer | 全内存向量缓存，内存充足场景下最大加速 | ❌ | ❌ | ✅ |
| 缓存 | 缓存异步 IO | 内存受限场景下加速磁盘到缓存数据读取 | ❌ | ❌ | ✅ |
| 数据类型 | floatvector | 标准 float32 向量类型 | ✅ | ✅ | ✅ |
| 数据类型 | halfvector | 半精度 float16 向量类型 | ✅ | 🟡 | ✅ |
| 数据类型 | int8vector | int8 向量类型 | ❌ | 🟡 | ✅ |
| 量化 | PQ 量化 | 向量压缩比最大，QPS 与原始向量相近 | ❌ | 🟡 | ✅ |
| 量化 | RaBitQ 量化 | 量化距离参与图遍历，索引只保存 code | ❌ | ✅ | ✅ |
| 量化 | 量化自动开启 | 后台自动开启量化，支持空表建量化索引 | ❌ | ❌ | ✅ |
| 图索引增强 | 图索引异步插入 | 多写少读场景下快速入库 | ❌ | ❌ | ✅ |
| 图索引增强 | 图挂桶功能 | 小规格机器承载大规模向量检索 | ❌ | ❌ | ✅ |
| 图索引增强 | 子图构建索引 | 内存不足场景下仍保持使用内存构建索引，加快构建速度 | ❌ | ❌ | ✅ |
| 高可用 | 主备高可用 | 支持主备同步与备份恢复 | ✅ | ❌ | ✅ |
| 运维 | 并行 vacuum | 并行加速索引清理回收 | ❌ | ❌ | ✅ |

### 2.2 DuckDB 扩展对比（DuckDB VSS vs VexDB-Lite）

| 分类 | 功能 | 描述 | DuckDB VSS | VexDB-Lite (`vexdb_lite`) |
|---|---|---|:---:|:---:|
| 索引 | 图索引 | VSS：HNSW；VexDB：graph_index（自研融合图索引） | ✅ | ✅ |
| 距离计算 | SIMD 分发 | 内联距离计算函数，编译时优化 | ❌ | ✅ |
| 量化 | PQ 量化 | 内存受限场景下的向量压缩 | ❌ | ✅ |
| 量化 | RaBitQ 量化 | 共享量化图遍历，支持不保留索引侧原始向量镜像的 compact 模式 | ❌ | ✅ |
| 缓存 | 缓存管理 | 磁盘到内存向量缓存 | ❌ | ✅ |
| 运维 | 索引压缩 | 回收软删除条目的空间 | ✅ | ❌ |
| 搜索 | 过滤 ANN 搜索 | WHERE 过滤 + 自动过采样 | ❌ | ✅ |
| 持久化 | 磁盘持久化 | 重启后无需重建索引 | ✅† | ✅ |

† VSS 持久化为实验性功能——WAL 恢复未实现，异常关机可能导致索引损坏。VexDB-Lite 通过 DuckDB 标准序列化机制持久化。

---

✅ 已支持 · 🟡 即将支持 · ❌ 开源版不含

## 3. PostgreSQL 语法示例

### 3.1 安装与建表

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

### 3.2 建索引

```sql
CREATE INDEX idx_items_vec
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (
    m = 16,
    ef_construction = 64
);
```

#### 3.2.1 PQ 量化索引（v1）

启用 PQ 可将索引存储压缩约 16×。当前 v1 版本的使用规约：

```sql
SET maintenance_work_mem = '2GB';   -- 必需，低于 1GB 会自动回落 普通图索引
CREATE INDEX idx_pq ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (quantizer = 'pq', pq_m = 4);
```

**v1 已知限制**：

- `maintenance_work_mem < 1GB` 时 PQ 自动回落 普通图索引（带 NOTICE 提示）
- PQ 索引支持 build 后的 `INSERT` / `UPDATE` / `DELETE` 路径，新增向量会按已训练 codebook 写入 PQ code
- parallel build × PQ：PG parallel worker 只在磁盘构建阶段参与，PQ 训练仍在 leader 进程内完成

**核对索引状态**：

```sql
SELECT indexname, use_pq, pq_m FROM vexdb_index_info()
WHERE indexname = 'idx_pq';
```

#### 3.2.2 RaBitQ 索引

```sql
CREATE INDEX idx_rabitq
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (quantizer = 'rabitq', m = 16, ef_construction = 100);
```

PG 至少需要 10000 条训练样本，并且样本要能放进 `maintenance_work_mem`。默认 `full` 模式下，条件不足时会输出 NOTICE 并回退为普通图索引；`memory_mode='compact'` 则要求量化器必须成功激活，失败时直接终止建索引，不会写入原始向量：

```sql
CREATE INDEX idx_rabitq_compact
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (quantizer = 'rabitq', memory_mode = 'compact');
```

PG 的量化索引原本就只在索引向量文件中保存 PQ/RaBitQ code，不会额外保留原始向量镜像，所以 `compact` 是严格存储契约，不是再压缩一次。compact 未指定 `quantizer` 时自动使用 PQ；显式设置 `quantizer='none'` 会报错；unlogged 表暂不支持 compact。`ALTER INDEX ... SET (memory_mode='compact')` 不会重写已有 raw 索引，需要执行 `REINDEX`，并通过 `vexdb_index_info().memory_mode`、`index_inspect()` 的 `Working Quantizer` 和 `Vector Storage` 核对实际状态。

### 3.3 ANN 查询

```sql
SET vexdb.ef_search = 100;
SET enable_seqscan = off;

SELECT id, vec <-> '[0.15, 0.25, 0.35]' AS dist
FROM items
ORDER BY vec <-> '[0.15, 0.25, 0.35]'
LIMIT 10;
```

### 3.4 其他距离

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

## 4. DuckDB 语法示例

### 4.1 加载扩展

```sql
LOAD '/path/to/vexdb_lite.duckdb_extension';
SELECT vexdb_version();
```

Python 侧常见用法：

```python
import duckdb

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("LOAD '/path/to/vexdb_lite.duckdb_extension'")
```

### 4.2 建表与建索引

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

### 4.3 ANN 查询

```sql
SET vexdb_ef_search = 100;

SELECT id
FROM items
ORDER BY l2_distance(vec, [0.15, 0.25, 0.35]::FLOAT[3])
LIMIT 10;
```

### 4.4 过滤索引示例

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

### 4.5 其他距离函数

```sql
SELECT inner_product([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT cosine_distance([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT vector_dims([1.0, 2.0, 3.0]::FLOAT[3]);
SELECT l2_normalize([3.0, 4.0]::FLOAT[2]);
SELECT * FROM vexdb_index_info();
```

---

## 5. 构建方法

> **预编译产物（推荐）**：从 [v0.0.17 Release](https://github.com/VexDB-THU/VexDB-Lite/releases/tag/v0.0.17) 下载对应数据库、平台和架构的压缩包。SQLite 提供 Linux、macOS、iOS XCFramework、Android 和 WASM 产物；本版本暂不提供 Windows 预编译包。
>
> **从源码构建**：每个子项目的 README 有详细步骤：
> - DuckDB 扩展：[vexdb_duckdb/README.md#构建](vexdb_duckdb/README.md#构建)
> - PG 扩展：[编译构建指南](documentation/build-guide.md)
>
> **跨架构批量打包**（项目内部发版用）：
> ```bash
> bash scripts/release.sh build              # 远程 x86 + ARM 各 build 一份 → dist/
> bash scripts/release.sh package            # 打 tarball + SHA256SUMS → dist/release/
> bash scripts/release.sh upload v0.0.17     # gh release upload
> ```

下载后使用发布页中的校验文件检查产物：

```bash
shasum -a 256 -c SHA256SUMS.txt
```

### 5.1 构建 PostgreSQL 版本

#### 依赖

- PostgreSQL 16 ~ 19（已适配 PG 16/17/18/19；主验证平台为 `19devel`）
- CMake ≥ 3.14
- C++17 编译器（GCC 9+ / Clang 10+）

#### 编译 PostgreSQL（release 示例）

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

#### 编译 `vexdb_lite`

```bash
cd /path/to/VexDB
mkdir -p build-pg19rel-release
cd build-pg19rel-release

export PG_CONFIG=/opt/postgresql-19rel-install/bin/pg_config
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
make install
```


#### 启动前配置

`postgresql.conf` 至少需要：

```conf
shared_preload_libraries = 'vexdb_lite'
```

重启实例后：

```sql
CREATE EXTENSION vexdb_lite;
```

---

### 5.2 构建 DuckDB 版本

**推荐方式：使用 `build_duck.sh`**（封装了 DuckDB clone、cmake 配置、编译、元数据处理全流程）

```bash
bash build_duck.sh setup   # 首次：clone DuckDB v1.5.2 并 cmake configure
bash build_duck.sh build   # 编译扩展（增量）
```

生成物：`build/duck/build/extension/vexdb_lite/vexdb_lite.duckdb_extension`

#### 依赖

- CMake ≥ 3.28 且 < 4.x
- C++17 编译器（GCC 9+ 或 Clang 10+）
- Git

#### 说明

DuckDB 扩展需嵌入 DuckDB 源码树编译，无法单独 `cmake -B build vexdb_duckdb/`。`build_duck.sh` 自动处理以下步骤：
1. clone DuckDB v1.5.2 源码
2. 写入 `extension_config_local.cmake` 注册 vexdb_lite 扩展
3. 运行 `cmake` + `cmake --build`
4. 处理扩展元数据 footer（DuckDB 发版格式要求）

---

## 6. 运行测试

#### DuckDB 扩展测试

```bash
bash build_duck.sh build          # 构建扩展
bash tests/spec/_lib/docker/run_duckdb.sh test  # 运行全量 spec 测试（需 Docker）
```

#### PostgreSQL 插件测试

```bash
bash tests/spec/_lib/docker/run_pg.sh test      # 运行 PG spec 测试（需 Docker + PG19）
```

#### SQLite 扩展测试

```bash
bash build_sqlite.sh test
bash tests/spec/_lib/docker/run_sqlite.sh test
```

测试框架基于 YAML spec DSL，测试文件位于 `tests/spec/`。

v0.0.17 发版验证结果：DuckDB 在 Linux x86_64 和 AArch64 上各通过 127/127；PostgreSQL 19 AArch64 通过 88/88；SQLite 在 3.46.0 和 3.53.4 上通过 32/32。iOS Simulator 通过 M0、M1、SIMD、M2、M3、M3+，其中 4 万向量并行建图的 recall@10 为 1.0000。

---

## 7. 测试结果

数据集：SIFT-1M 128 维，`m=16`，`ef_construction=128`。列含义：`QPS（reads=1）` / `QPS（reads=16）` / `Recall@10`。

测试环境：Intel Core Ultra 7-265K（20c/20t，3.9 GHz）/ 16 GB DDR5 / x86_64 Linux

### 7.1 与 pgvector / VSS 对比（x86_64）

**ef_search = 50**

| 系统 | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 507.9 | 7153.5 | 96.22% |
| **vexdb_lite (PostgreSQL)** | **994.7** | **12084.6** | 95.97% |
| **vexdb_lite (DuckDB)** | **717.5** | **8667.8** | 95.06% |
| duckdb-vss | 496.1 | 5360.9 | 94.07% |

**ef_search = 100**

| 系统 | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 313.4 | 4272.5 | 98.82% |
| **vexdb_lite (PostgreSQL)** | **618.5** | **7883.1** | 98.62% |
| **vexdb_lite (DuckDB)** | **547.2** | **5379.1** | 98.40% |
| duckdb-vss | 405.2 | 4433.3 | 98.04% |

**ef_search = 200**

| 系统 | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 193.1 | 2694.1 | 99.66% |
| **vexdb_lite (PostgreSQL)** | **421.3** | **5038.0** | 99.58% |
| **vexdb_lite (DuckDB)** | **383.6** | **4298.8** | 99.53% |
| duckdb-vss | 321.9 | 3809.3 | 99.42% |

---

## 8. 当前已知限制

### PostgreSQL

- 支持 PostgreSQL 16 ~ 19，当前主验证平台是 PostgreSQL 19

### DuckDB

- `threads`、`pq_m` 选项目前接受但部分路径仍是兼容保留/未完全实现
- ARM Duck 构建当前也走 `GENERAL` 距离派发

### SQLite 与发布产物

- iOS、Android、WASM 使用静态注册；运行时 `.load` 只适用于桌面端和服务端
- v0.0.17 暂不提供 Windows 预编译包

## 9. 仓库结构

| 目录 | 说明 |
|---|---|
| `common/` | 双端共享内核：图索引算法、SIMD 距离分发、量化器（PQ/RaBitQ）、模板容器 |
| `vexdb_pg/` | PostgreSQL 扩展：索引 AM、构建、搜索、DML、WAL、距离分发入口 |
| `vexdb_duckdb/` | DuckDB 扩展：索引生命周期、优化器改写、距离函数 → [README](vexdb_duckdb/README.md) |
| `vexdb_sqlite/` | SQLite 扩展：虚拟表、shadow table 持久化、PQ/RaBitQ → [README](vexdb_sqlite/README.md) |
| `documentation/` | 功能文档、构建指南 |
| `tests/spec/` | 基于 YAML 的 spec 测试（shared / pg / duckdb / sqlite） |
| `scripts/` | 构建、发版、打包脚本 |
| `thirdparties/` | 第三方依赖（patched Boost） |

---

## 社区

| 渠道 | 说明 |
|---|---|
| [GitHub Issues](https://github.com/VexDB-THU/VexDB-Lite/issues) | Bug 反馈与功能建议 |
| [GitHub Discussions](https://github.com/VexDB-THU/VexDB-Lite/discussions) | 提问、提案与功能讨论 |
| [Discord](https://discord.gg/Ge4kaFak) | 实时交流、答疑解惑 |
| 官方微信社群 | 扫码加入，见 [vexdb.com/community](https://vexdb.com/community)，中文社区互助 |

---

## License

MIT License. 详见 [LICENSE](LICENSE)。
