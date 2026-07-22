# vexdb_duckdb

`vexdb_duckdb` 是 VexDB-Lite 在 DuckDB 端的实现：作为 DuckDB out-of-tree extension 提供向量类型、距离函数、自研图索引（`GRAPH_INDEX`）和优化器 rewrite（`VEXDB_INDEX_SCAN`）。

**目录**

- [一分钟上手](#一分钟上手)
- [安装](#安装)
- [向量类型](#向量类型)
- [距离函数](#距离函数)
- [图索引（GRAPH_INDEX）](#图索引graph_index)
- [产品量化（PQ）](#产品量化pq)
- [运行参数（GUC）](#运行参数guc)
- [内省函数](#内省函数)
- [Python 用法](#python-用法)
- [构建](#构建)
- [故障排查](#故障排查)

---

## 一分钟上手

```sql
LOAD '/path/to/vexdb_lite.duckdb_extension';

CREATE TABLE items (id INTEGER, vec FLOAT[128]);
INSERT INTO items SELECT i, list_value(...)::FLOAT[128] FROM range(100000) t(i);

CREATE INDEX idx_vec ON items USING GRAPH_INDEX (vec)
  WITH (m = 16, ef_construction = 64);

SET vexdb_ef_search = 40;
SELECT id FROM items
  ORDER BY l2_distance(vec, [0.5, 0.5, ...]::FLOAT[128])
  LIMIT 10;
```

---

## 安装

### 加载预编译的扩展

DuckDB 默认禁止加载未签名扩展，需要：

```sql
-- CLI
duckdb -unsigned

-- Python
import duckdb
con = duckdb.connect(config={"allow_unsigned_extensions": "true"})

-- 任意客户端
SET allow_unsigned_extensions = true;
LOAD '/path/to/vexdb_lite.duckdb_extension';
SELECT vexdb_version();
```

`vexdb_version()` 返回当前扩展版本号，验证加载成功。

### 平台与产物

| 架构 | 平台 | 文件 |
|---|---|---|
| `aarch64` Linux (Kylin/Ubuntu) | `vexdb_lite.duckdb_extension` | `dist/aarch64-linux/` |
| `x86_64` Linux | `vexdb_lite.duckdb_extension` | 见 [构建](#构建) |
| `x86_64` / `arm64` macOS | `vexdb_lite.duckdb_extension` | 见 [构建](#构建) |

### 加载到持久数据库

```sql
ATTACH '/path/to/db.duckdb' AS my_db;
USE my_db;
LOAD '/path/to/vexdb_lite.duckdb_extension';
```

每次打开数据库都要 `LOAD` 一次（DuckDB 不会持久化扩展自动加载状态）。

---

## 向量类型

vexdb_duckdb 使用 DuckDB 原生的 `FLOAT[N]` 固定长度数组类型作为向量：

```sql
CREATE TABLE items (vec FLOAT[128]);
INSERT INTO items VALUES ([0.1, 0.2, ...]::FLOAT[128]);
```

**为什么不引入 `floatvector(N)` 自定义类型？**
DuckDB extension API 暂未暴露稳定的自定义类型接口；`FLOAT[N]` 已经覆盖大多数需求，且零序列化开销。

**维度约束**
索引创建时维度从首个非 NULL 向量推断。后续插入若维度不匹配会报错。

---

## 距离函数

DuckDB 侧支持**函数形式**，同时 `<->`、`<=>`、`<~>` 运算符也可用（`<#>` 因 `#` 与 DuckDB 注释符冲突不可用，负内积用 `<~>`）：

| 函数 | 等价数学 | 备注 |
|---|---|---|
| `l2_distance(a, b)` | √Σ(aᵢ-bᵢ)² | 欧氏距离 |
| `inner_product(a, b)` | Σaᵢ·bᵢ | 内积；越大越相似 |
| `cosine_distance(a, b)` | 1 - cos(a,b) | 余弦距离；0 = 同向 |
| `list_negative_inner_product(a, b)` | -Σaᵢ·bᵢ | 负内积；ANN-friendly（越小越相似） |
| `array_distance(a, b)` / `list_distance(a, b)` | 同 `l2_distance` | DuckDB 内置别名 |

**示例**

```sql
SELECT l2_distance([1.0, 0.0]::FLOAT[2], [0.0, 1.0]::FLOAT[2]);  -- 1.4142135
SELECT cosine_distance([1.0, 0.0]::FLOAT[2], [1.0, 0.0]::FLOAT[2]);  -- 0
SELECT inner_product([1.0, 2.0]::FLOAT[2], [3.0, 4.0]::FLOAT[2]);  -- 11
```

---

## 图索引（GRAPH_INDEX）

### 语法

```sql
CREATE INDEX idx_name
ON table_name
USING GRAPH_INDEX (vec_column [, filter_column...])
WITH (
    metric = 'l2',           -- 距离指标
    m = 16,                  -- 图索引 M 参数（邻居数上限）
    ef_construction = 64,    -- 建图时搜索宽度
    threads = 1,             -- 并行 worker 数（>1 启用并行构建）
    quantizer = 'pq',        -- 可选：'pq' 或 'rabitq'
    pq_m = 8,                -- PQ 子量化器数（每段 dim/pq_m 维度）
    memory_mode = 'compact'  -- 可选：不保留索引侧原始向量镜像
);
```

### 参数详解

| 参数 | 默认值 | 说明 |
|---|---|---|
| `metric` | `'l2'` | `'l2'` / `'cosine'` / `'ip'` |
| `m` | `16` | 图索引每个节点的最大邻居数 |
| `ef_construction` | `64` | 建图阶段搜索宽度，越大召回越好建索引越慢 |
| `threads` | `1` | 并行构建 worker 数；`1` = 串行；>1 启用 striped lock 并发 |
| `quantizer` | `none` | `'pq'` 启用 Product Quantization；`'rabitq'` 启用 RaBitQ 图搜索 |
| `pq_m` | (无) | PQ 子量化器数；要求 `dim % pq_m == 0` |
| `memory_mode` | `'full'` | `'full'` 存索引侧原始向量 + 量化 code；`'compact'` 支持 PQ 和 RaBitQ |

### 索引 vs 暴力搜索

`vexdb_duckdb` 内置 small-data fast path：当索引行数 ≤ `vexdb_brute_force_threshold`（默认 10000）时，优化器**跳过图索引**直接走 SEQ_SCAN + ORDER BY + LIMIT 精确扫描。

- 小表（n < 10k）：自动 brute force，100% 召回，无索引开销
- 大表（n ≥ 10k）：图索引路径，~99% 召回，毫秒响应

需要绕开 fast path（如测优化器、测 ANN 路径）：`SET vexdb_brute_force_threshold = 0;`

### ANN 查询

```sql
SET vexdb_ef_search = 40;  -- 查询时搜索宽度，越大召回越好越慢

SELECT id
FROM items
ORDER BY l2_distance(vec, [0.5, ...]::FLOAT[128])
LIMIT 10;
```

优化器识别 `ORDER BY <distance_func>(vec, query) LIMIT k` 模式后会改写为 `VEXDB_INDEX_SCAN`，可通过 EXPLAIN 验证：

```sql
EXPLAIN SELECT id FROM items
  ORDER BY l2_distance(vec, [...]::FLOAT[128])
  LIMIT 10;
-- physical_plan 应包含 VEXDB_INDEX_SCAN
```

### 元数据过滤

支持在索引里附带标量列做 pre-filter：

```sql
CREATE INDEX idx_items_meta
ON items
USING GRAPH_INDEX (vec, category, status);

SELECT id FROM items
WHERE category = 'book' AND status = 'active'
ORDER BY l2_distance(vec, [...]::FLOAT[128])
LIMIT 10;
```

---

## 产品量化（PQ）

PQ 把向量切成 `pq_m` 段，每段独立 k-means 量化到 256 个 centroid，存储压缩到 `pq_m` 字节。

### 启用

```sql
CREATE INDEX idx_vec_pq ON items
USING GRAPH_INDEX (vec)
WITH (quantizer = 'pq', pq_m = 16);
```

**约束**：`dim % pq_m == 0`，常见配置 dim=128/pq_m=16 或 dim=768/pq_m=64。

### compact 模式

```sql
CREATE INDEX idx_vec_compact ON items
USING GRAPH_INDEX (vec)
WITH (quantizer = 'pq', pq_m = 16, memory_mode = 'compact');
```

`memory_mode='compact'` 时：

- 表中的原始向量不变，索引侧的原始向量镜像在训练后被释放。
- PQ 查询自动走 code 路径，无法使用原始向量 refine。
- RaBitQ 查询仍通过 code-aware store 遍历原图，增量向量先编码再插入图。
- PQ 与 RaBitQ 的 compact 状态都会持久化，checkpoint 或重启后无需重建。

## RaBitQ

RaBitQ 使用公共量化器和 code-aware 图遍历，支持 L2、cosine 和 inner product：

```sql
CREATE INDEX idx_vec_rabitq ON items
USING GRAPH_INDEX (vec)
WITH (metric = 'cosine', quantizer = 'rabitq', m = 16, ef_construction = 160);
```

RaBitQ compact 示例：

```sql
CREATE INDEX idx_vec_rabitq_compact ON items
USING GRAPH_INDEX (vec)
WITH (metric = 'cosine', quantizer = 'rabitq', memory_mode = 'compact',
      m = 16, ef_construction = 160);
```

当前限制：

- `quantizer='rabitq'` 不能与 `pq_m` 同时使用。
- 当前版本优先保证统一算法、持久化和召回正确性；小规模数据上不保证比 plain 图索引更快。

### 搜索模式（GUC）

```sql
SET vexdb_pq_search_mode = 'pq_only';   -- 仅用 PQ codes，最快
SET vexdb_pq_search_mode = 'off';       -- PQ 默认路径（compact PQ 会自动启用 pq_only）
SET vexdb_pq_refine_k_factor = 4.0;     -- 图索引找 4k 个候选后用原向量精排
```

---

## 运行参数（GUC）

| GUC | 默认值 | 说明 |
|---|---|---|
| `vexdb_ef_search` | 40 | 图索引查询搜索宽度。提高 → 召回 ↑ / QPS ↓ |
| `vexdb_brute_force_threshold` | 10000 | 行数 ≤ 此值走 brute force 精确路径 |
| `vexdb_pq_search_mode` | `'off'` | PQ 搜索模式：`'off'` / `'pq_only'` |
| `vexdb_pq_refine_k_factor` | 1.0 | PQ pq_only 时取 k×factor 候选用原向量精排 |

```sql
SET vexdb_ef_search = 100;       -- 高召回
SET vexdb_ef_search = 10;        -- 高 QPS

SET vexdb_brute_force_threshold = 0;     -- 强制走图索引（调试用）
SET vexdb_brute_force_threshold = 100000;-- 大表也走 brute force（重 build 后召回未恢复时临时绕过）
```

---

## 内省函数

```sql
SELECT vexdb_version();           -- 扩展版本号
SELECT vexdb_simd_arch();         -- 当前 SIMD 路径（'AVX512' / 'NEONV8' / 'GENERAL' 等）

-- 索引统计
SELECT * FROM vexdb_index_info();
-- 列：index_name, table_name, dimension, metric, m, ef_construction, node_count,
--     use_pq, pq_m, pq_codebook_bytes, pq_codes_bytes, ...

-- 工具函数
SELECT vector_dims([1.0, 2.0, 3.0]::FLOAT[3]);  -- 3
SELECT l2_normalize([3.0, 4.0]::FLOAT[2]);       -- [0.6, 0.8]
SELECT vector_norm([3.0, 4.0]::FLOAT[2]);        -- 5.0
SELECT vector_add(a, b);
SELECT vector_sub(a, b);
```

---

## Python 用法

```python
import duckdb
import numpy as np

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("LOAD '/path/to/vexdb_lite.duckdb_extension'")

# 建表 + 插入
con.execute("CREATE TABLE items (id INTEGER, vec FLOAT[128])")
vecs = np.random.rand(100000, 128).astype(np.float32)
con.executemany(
    "INSERT INTO items VALUES (?, ?)",
    [(i, v.tolist()) for i, v in enumerate(vecs)],
)

# 建索引
con.execute("""
    CREATE INDEX idx_vec ON items USING GRAPH_INDEX (vec)
    WITH (m = 16, ef_construction = 64)
""")

# 查询
q = np.random.rand(128).astype(np.float32)
result = con.execute("""
    SELECT id FROM items
    ORDER BY l2_distance(vec, ?::FLOAT[128])
    LIMIT 10
""", [q.tolist()]).fetchall()
```

---

## 构建

### 依赖

- 编译器：gcc 9.3+ / clang 10+（gcc 8.5 会撞 DuckDB 主仓 `expression_map_t` 模板问题）
- CMake 3.20+
- Boost 头文件（需含 `concurrent_flat_map`，Boost 1.84+）
- Python 3（DuckDB 主仓构建系统需要）

### 从源码构建

```bash
git clone <vexdb-lite-repo> && cd vexdb_lite
bash build_duck.sh setup    # clone DuckDB v1.5.2 + cmake configure（首次 ~5 min）
bash build_duck.sh build    # 编 vex 扩展（~20 min）
```

产物：`build/duck/build/extension/vexdb_lite/vexdb_lite.duckdb_extension`

### Boost 路径自定义

`vexdb_duckdb/CMakeLists.txt` 用 `find_package(Boost REQUIRED)`。如果 Boost 不在系统默认位置：

```bash
export BOOST_INCLUDEDIR=/path/to/boost
export BOOST_ROOT=/path/to/boost
bash build_duck.sh build
```

### 调试构建

```bash
cd build/duck/build
cmake --build . --target unittest -j8
./test/unittest "test/sql/vex/*"
```

---

## 故障排查

### `Extension "vex" is not loaded` 或 `function not found`

未加载扩展或加载失败：

```sql
LOAD '/path/to/vexdb_lite.duckdb_extension';
SELECT vexdb_version();  -- 应返回版本号
```

### `Could not load library "vexdb_lite.duckdb_extension"`

通常是 DuckDB 版本不匹配。扩展必须用 build 时的 DuckDB 版本加载。检查：

```sql
SELECT version();  -- 当前 DuckDB 版本
```

需要与扩展 build 时的 DuckDB 版本一致（一般是 v1.5.2 或同 commit）。

### `IO Error: Extension ... must be signed`

```sql
-- 启动时
duckdb -unsigned

-- 或运行时（先 SET 再 LOAD）
SET allow_unsigned_extensions = true;
LOAD '...';
```

Python：`config={"allow_unsigned_extensions": "true"}`

### EXPLAIN 显示 SEQ_SCAN 而不是 VEXDB_INDEX_SCAN

可能原因：

1. **行数 < brute_force_threshold**（默认 10000）— 小表自动走精确扫描，是 feature 不是 bug
2. **`ORDER BY` 函数不匹配** — 必须用 `l2_distance` / `cosine_distance` / `inner_product` 之一
3. **没有对应 metric 的索引** — `metric='l2'` 索引服务不了 `cosine_distance` 查询
4. **`LIMIT` 缺失** — vex 优化器要求 `LIMIT k` 模式

调试：

```sql
SET vexdb_brute_force_threshold = 0;  -- 排除原因 1
EXPLAIN SELECT id FROM items ORDER BY l2_distance(vec, ...) LIMIT 10;
```

### ANN 查询结果与暴力扫描不同（recall < 1）

图索引（近似最近邻），召回 < 100% 是预期。提高召回：

```sql
SET vexdb_ef_search = 200;  -- 默认 40，提到 200 召回大幅提升
```

或直接用 brute force（小数据集已经自动这样做）：

```sql
SET vexdb_brute_force_threshold = 99999999;
```

### `Parser Error: syntax error at or near "#>"`

DuckDB 支持 `<->`（L2）、`<=>`（cosine）、`<~>`（负内积）运算符；仅 `<#>` 因 `#` 与注释符冲突不可用，负内积改用 `<~>` 或函数形式：

```sql
-- 错（<#> 不可用）
SELECT id FROM items ORDER BY vec <#> [...]::FLOAT[3] LIMIT 10;

-- 对
SELECT id FROM items ORDER BY vec <~> [...]::FLOAT[3] LIMIT 10;
-- 或函数形式
SELECT id FROM items ORDER BY list_negative_inner_product(vec, [...]::FLOAT[3]) LIMIT 10;
```

### Index build OOM

`m × ef_construction` 调小，或分批 INSERT 后 `CREATE INDEX`。
PQ 或 RaBitQ + `memory_mode='compact'` 可避免索引内再保留一份原始向量。

---

## 测试与 benchmark

详见仓库根的 [README](../README.md) 和 `tests/spec/`、`vexdb_duckdb/test/`。
