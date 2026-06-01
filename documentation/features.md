# 功能文档

VexDB 提供两种适配形式，共享同一套 HNSW 图索引算法内核：

- **PostgreSQL 插件**（`vexdb_vector`）：作为 PostgreSQL 扩展提供 `vexdb_graph` 访问方法
- **DuckDB 扩展**（`vex`）：作为 DuckDB out-of-tree extension 提供 `GRAPH_INDEX`

---

## PostgreSQL 插件

### 快速上手

```sql
CREATE EXTENSION vexdb_vector;

CREATE TABLE items (
    id  BIGSERIAL PRIMARY KEY,
    vec floatvector(128)
);

-- 向量字面量使用方括号，与 pgvector 兼容（示意，省略号代表其余维度）
INSERT INTO items (vec) VALUES ('[0.10, 0.20, ..., 0.80]');

CREATE INDEX idx_items_vec
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (m = 16, ef_construction = 64);

-- ANN 查询（优化器自动改写为 Index Scan）
SET vexdb_vector.ef_search = 64;
SELECT id, vec <-> '[0.1, 0.2, ..., 0.8]' AS dist
FROM items
ORDER BY vec <-> '[0.1, 0.2, ..., 0.8]'
LIMIT 10;
```

### 向量类型

| 类型 | 说明 | 示例 |
|------|------|------|
| `floatvector(N)` | 32 位浮点向量，维度 N | `floatvector(128)` |

### 距离运算符

| 运算符 | 距离类型 | 操作符类 | 说明 |
|--------|----------|----------|------|
| `<->` | L2（欧氏）距离 | `floatvector_l2_ops` | 越小越相似 |
| `<#>` | 负内积 | `floatvector_ip_ops` | 越小越相似（内积越大） |
| `<=>` | 余弦距离 | `floatvector_cosine_ops` | 越小越相似 |
| `<~>` | 余弦距离（别名） | `floatvector_cosine_ops` | 同 `<=>` |

### 距离函数

| 函数 | 等价运算符 |
|------|------------|
| `l2_distance(a, b)` | `a <-> b` |
| `inner_product(a, b)` | `-(a <#> b)` |
| `cosine_distance(a, b)` | `a <=> b` |

### 建索引参数

```sql
CREATE INDEX idx_name
ON table_name
USING vexdb_graph (vec_column floatvector_l2_ops)
WITH (
    m                = 16,   -- 每个节点的最大邻居数
    ef_construction  = 64,   -- 建图阶段搜索宽度
    parallel_workers = 4     -- 并行构建 worker 数（0 = 串行）
);
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `m` | `16` | HNSW 每个节点的最大邻居数；越大召回越好，内存消耗越多 |
| `ef_construction` | `64` | 建图时搜索宽度；越大图质量越好，建索引越慢 |
| `parallel_workers` | `0` | 并行构建 worker 数；`0` = 串行 |

### 运行参数（GUC）

```sql
-- 查询时搜索宽度（默认 64，越大召回越好、延迟越高）
SET vexdb_vector.ef_search = 64;

-- 强制指定 SIMD 架构（留空由运行时自动检测；需要 superuser 权限）
-- 格式：'usage:arch[, usage:arch, ...]'
-- usage 取值：all / float / l2 / ip / cos 等
-- arch 取值：SSE / AVX / AVX512 / NEONV8 / SVE2V8 / GENERAL
SET vexdb_vector.vec_architecture = '';        -- 自动（推荐）
-- SET vexdb_vector.vec_architecture = 'all:AVX';  -- 强制 AVX2
```

| 参数 | 默认值 | 权限 | 说明 |
|------|--------|------|------|
| `vexdb_vector.ef_search` | `64` | 普通用户 | 搜索时的候选集大小 |
| `vexdb_vector.vec_architecture` | `''`（自动） | superuser | 强制指定 SIMD 实现；格式 `'usage:arch'` |

### 并行构建

`parallel_workers` 参数启用多 worker 并行建索引：

```sql
CREATE INDEX CONCURRENTLY idx_items_vec
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (m = 16, ef_construction = 64, parallel_workers = 8);
```

**注意**：`parallel_workers` 是 VexDB 自己的索引参数，不依赖 `max_parallel_maintenance_workers`。

---

## DuckDB 扩展

### 快速上手

```sql
-- 加载扩展（每次连接都需要）
SET allow_unsigned_extensions = true;
LOAD '/path/to/vex.duckdb_extension';

CREATE TABLE items (id INTEGER, vec FLOAT[128]);
-- 示意：实际插入时需提供完整 128 维向量
INSERT INTO items VALUES (1, array_fill(0.1::FLOAT, [128])::FLOAT[128]);

CREATE INDEX idx_vec ON items USING GRAPH_INDEX (vec)
    WITH (m = 16, ef_construction = 64);

SET vex_ef_search = 64;
SELECT id
FROM items
ORDER BY l2_distance(vec, array_fill(0.5::FLOAT, [128])::FLOAT[128])
LIMIT 10;
```

### 向量类型

DuckDB 扩展使用原生 `FLOAT[N]` 固定长度数组作为向量：

```sql
CREATE TABLE items (vec FLOAT[128]);
```

### 距离函数与运算符

DuckDB 侧同时支持**函数形式**和**运算符形式**：

| 函数 | 运算符 | 说明 | ANN 方向 |
|------|--------|------|----------|
| `l2_distance(a, b)` | `a <-> b` | L2（欧氏）距离 | 越小越相似 |
| `inner_product(a, b)` | — | 内积 | 越大越相似 |
| `cosine_distance(a, b)` | `a <=> b` / `a <~> b` | 余弦距离（1 - cos） | 越小越相似 |
| `list_negative_inner_product(a, b)` | `a <#> b` | 负内积 | 越小越相似 |

### 建索引参数

```sql
CREATE INDEX idx_name
ON table_name
USING GRAPH_INDEX (vec_column)
WITH (
    metric          = 'l2',    -- 距离指标：'l2' / 'cosine' / 'ip'
    m               = 16,      -- HNSW M 参数
    ef_construction = 64,      -- 建图搜索宽度
    threads         = 0,       -- 并行构建 worker 数（0 = 自动，用 DuckDB scheduler 线程数）
    quantizer       = 'pq',    -- 可选：启用 Product Quantization
    pq_m            = 8,       -- PQ 子量化器数（要求 dim % pq_m == 0）
    memory_mode     = 'full'   -- 'full' 或 'compact'
);
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `metric` | `'l2'` | `'l2'` / `'cosine'` / `'ip'` |
| `m` | `16` | HNSW 每节点最大邻居数 |
| `ef_construction` | `64` | 建图时搜索宽度 |
| `threads` | `0`（自动） | `0` = 使用 DuckDB scheduler 线程数；`1` = 串行 |
| `quantizer` | `none` | `'pq'` 启用 Product Quantization |
| `pq_m` | — | PQ 子量化器数 |
| `memory_mode` | `'full'` | `'compact'` 仅存 PQ codes（内存节省 ~32×，不支持精确 refine） |

### 运行参数（GUC）

```sql
-- 查询搜索宽度（默认 64）
SET vex_ef_search = 64;

-- 暴力搜索阈值：行数 ≤ 此值时走全量扫描（默认 10000）
SET vex_brute_force_threshold = 10000;

-- PQ 搜索模式：'off'（默认）或 'pq_only'
SET vex_pq_search_mode = 'off';

-- PQ refine 倍率（默认 1.0 = 关闭 refine；设 >1.0 开启精确 refine）
SET vex_pq_refine_k_factor = 1.0;
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `vex_ef_search` | `64` | 查询候选集大小 |
| `vex_brute_force_threshold` | `10000` | 行数低于此值走全量扫描 |
| `vex_pq_search_mode` | `'off'` | `'pq_only'` 跳过精确 refine，速度更快但召回略低 |
| `vex_pq_refine_k_factor` | `1.0` | refine 倍率；`1.0` = 关闭（只用 PQ 距离排序） |

### WHERE 过滤与 ANN 查询

`GRAPH_INDEX` 在**单向量列**上建立，优化器会自动将 `WHERE` 条件下推为后置过滤（over-fetch k × factor，然后筛选）：

```sql
-- 只在 vec 列建索引
CREATE INDEX idx_vec ON items USING GRAPH_INDEX (vec);

-- 查询时 WHERE 会被优化器处理为 VEX_INDEX_SCAN + 过滤
SELECT id
FROM items
WHERE category = 'image'
ORDER BY l2_distance(vec, [...]::FLOAT[128])
LIMIT 10;
```

支持的过滤形式：等值（`=`）、BETWEEN、范围（`> / <`）、AND 组合。

### Product Quantization（PQ）

PQ 适合内存敏感场景，用少量 bits 近似表达原始向量：

```sql
-- 启用 PQ：32 个子量化器，每段 4 维（128/32=4）
CREATE INDEX idx_pq
ON items
USING GRAPH_INDEX (vec)
WITH (quantizer = 'pq', pq_m = 32);

-- 紧凑模式：极限内存压缩，不存原始向量
CREATE INDEX idx_compact
ON items
USING GRAPH_INDEX (vec)
WITH (quantizer = 'pq', pq_m = 32, memory_mode = 'compact');
```

### 内省函数

```sql
-- 查看扩展版本
SELECT vex_version();

-- 查看向量维度
SELECT vector_dims([1.0, 2.0, 3.0]::FLOAT[3]);

-- 向量 L2 归一化
SELECT l2_normalize([3.0, 4.0]::FLOAT[2]);  -- [0.6, 0.8]

-- 查看所有 GRAPH_INDEX 详情（无参数，返回每个索引一行）
SELECT * FROM vex_index_info();
-- 过滤特定索引
SELECT * FROM vex_index_info() WHERE index_name = 'idx_vec';
```

### Python 接口

如果使用 `vexdb-lite` Python wheel，可以通过 `vexdb_lite.connect()` 打开连接并自动加载 VEX：

```python
import vexdb_lite

con = vexdb_lite.connect()
```

也可以直接使用 DuckDB Python 包并手动加载扩展：

```python
import duckdb

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("LOAD '/path/to/vex.duckdb_extension'")

con.execute("CREATE TABLE items (id INTEGER, vec FLOAT[128])")
con.execute("CREATE INDEX idx ON items USING GRAPH_INDEX (vec) WITH (m=16)")

query_vec = [0.5] * 128
results = con.execute("""
    SELECT id, l2_distance(vec, ?::FLOAT[128]) AS dist
    FROM items
    ORDER BY dist LIMIT 10
""", [query_vec]).fetchall()
```

---

## 共享算法内核

PG 插件和 DuckDB 扩展共享以下核心组件（位于 `common/`）：

| 组件 | 路径 | 说明 |
|------|------|------|
| 图索引算法 | `common/include/graph_index/` | HNSW 算法、层级结构、邻居选择 |
| 距离计算 | `common/distance/` | SSE / AVX2 / AVX-512 / NEON 运行时分发 |
| Product Quantization | `common/quantizer/` | PQ 编码/解码、K-means 训练 |
| 容器模板 | `common/vtl/` | 内存分配器抽象、FixedSizeAllocator |
