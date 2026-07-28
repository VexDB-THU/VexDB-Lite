# 功能文档

VexDB 提供三种适配形式，共享同一套自研图索引算法内核：

- **PostgreSQL 插件**（`vexdb_lite`）：作为 PostgreSQL 扩展提供 `vexdb_graph` 访问方法
- **DuckDB 扩展**（`vexdb_lite`）：作为 DuckDB out-of-tree extension 提供 `GRAPH_INDEX`
- **SQLite 扩展**（`vexdb_lite`）：以虚拟表和 shadow table 提供 `GRAPH_INDEX`

---

## PostgreSQL 插件

### 快速上手

```sql
CREATE EXTENSION vexdb_lite;

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
SET vexdb.ef_search = 64;
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
| `<#>` | 负内积 | `floatvector_ip_ops` | 越小越相似（内积越大）；pgvector 兼容写法 |
| `<=>` | 余弦距离 | `floatvector_cosine_ops` | 越小越相似 |
| `<~>` | 负内积 | `floatvector_ip_ops` | 同 `<#>`，跨引擎统一写法（与 DuckDB `<~>` 一致），同样走 IP 索引 |

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
| `m` | `16` | 图索引每个节点的最大邻居数；越大召回越好，内存消耗越多 |
| `ef_construction` | `64` | 建图时搜索宽度；越大图质量越好，建索引越慢 |
| `parallel_workers` | `0` | 并行构建 worker 数；`0` = 串行 |

### 运行参数（GUC）

```sql
-- 查询时搜索宽度（默认 64，越大召回越好、延迟越高）
SET vexdb.ef_search = 64;

-- 强制指定 SIMD 架构（留空由运行时自动检测；需要 superuser 权限）
-- 格式：'usage:arch[, usage:arch, ...]'
-- usage 取值：all / float / l2 / ip / cos 等
-- arch 取值：SSE / AVX / AVX512 / NEONV8 / SVE2V8 / GENERAL
SET vexdb.vec_architecture = '';        -- 自动（推荐）
-- SET vexdb.vec_architecture = 'all:AVX';  -- 强制 AVX2
```

| 参数 | 默认值 | 权限 | 说明 |
|------|--------|------|------|
| `vexdb.ef_search` | `64` | 普通用户 | 搜索时的候选集大小 |
| `vexdb.vec_architecture` | `''`（自动） | superuser | 强制指定 SIMD 实现；格式 `'usage:arch'` |

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
LOAD '/path/to/vexdb_lite.duckdb_extension';

CREATE TABLE items (id INTEGER, vec FLOAT[128]);
-- 示意：实际插入时需提供完整 128 维向量
INSERT INTO items VALUES (1, array_fill(0.1::FLOAT, [128])::FLOAT[128]);

CREATE INDEX idx_vec ON items USING GRAPH_INDEX (vec)
    WITH (m = 16, ef_construction = 64);

SET vexdb_ef_search = 64;
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

### 距离函数

DuckDB 侧支持**函数形式**，同时运算符 `<->` / `<=>` / `<~>` 也可用（`<#>` 因 `#` 与 DuckDB 注释符冲突而不可用，负内积用 `<~>`）：

| 函数 | 说明 | ANN 方向 |
|------|------|----------|
| `l2_distance(a, b)` | L2（欧氏）距离 | 越小越相似 |
| `inner_product(a, b)` | 内积 | 越大越相似 |
| `cosine_distance(a, b)` | 余弦距离（1 - cos） | 越小越相似 |
| `list_negative_inner_product(a, b)` | 负内积 | 越小越相似 |

### 建索引参数

```sql
CREATE INDEX idx_name
ON table_name
USING GRAPH_INDEX (vec_column)
WITH (
    metric          = 'l2',    -- 距离指标：'l2' / 'cosine' / 'ip'
    m               = 16,      -- 图索引 M 参数
    ef_construction = 64,      -- 建图搜索宽度
    threads         = 0,       -- 并行构建 worker 数（0 = 自动，用 DuckDB scheduler 线程数）
    quantizer       = 'pq',    -- 可选：'pq' 或 'rabitq'
    pq_m            = 8,       -- PQ 子量化器数（要求 dim % pq_m == 0）
    memory_mode     = 'full'   -- 'full' 或 'compact'
);
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `metric` | `'l2'` | `'l2'` / `'cosine'` / `'ip'` |
| `m` | `16` | 图索引每节点最大邻居数 |
| `ef_construction` | `64` | 建图时搜索宽度 |
| `threads` | `0`（自动） | `0` = 使用 DuckDB scheduler 线程数；`1` = 串行 |
| `quantizer` | `none` | `'pq'` 启用 Product Quantization；`'rabitq'` 启用 RaBitQ 图搜索 |
| `pq_m` | — | PQ 子量化器数 |
| `memory_mode` | `'full'` | `'compact'` 不保留索引侧原始向量镜像；支持 PQ 和 RaBitQ |

### 运行参数（GUC）

```sql
-- 查询搜索宽度（默认 64）
SET vexdb_ef_search = 64;

-- 暴力搜索阈值：行数 ≤ 此值时走全量扫描（默认 10000）
SET vexdb_brute_force_threshold = 10000;

-- PQ 搜索模式：'off'（默认）或 'pq_only'
SET vexdb_pq_search_mode = 'off';

-- PQ refine 倍率（默认 1.0 = 关闭 refine；设 >1.0 开启精确 refine）
SET vexdb_pq_refine_k_factor = 1.0;
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `vexdb_ef_search` | `64` | 查询候选集大小 |
| `vexdb_brute_force_threshold` | `10000` | 行数低于此值走全量扫描 |
| `vexdb_pq_search_mode` | `'off'` | `'pq_only'` 跳过精确 refine，速度更快但召回略低 |
| `vexdb_pq_refine_k_factor` | `1.0` | refine 倍率；`1.0` = 关闭（只用 PQ 距离排序） |

### WHERE 过滤与 ANN 查询

`GRAPH_INDEX` 在**单向量列**上建立，优化器会自动将 `WHERE` 条件下推为后置过滤（over-fetch k × factor，然后筛选）：

```sql
-- 只在 vec 列建索引
CREATE INDEX idx_vec ON items USING GRAPH_INDEX (vec);

-- 查询时 WHERE 会被优化器处理为 VEXDB_INDEX_SCAN + 过滤
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

### RaBitQ

RaBitQ 使用公共量化器和 code-aware 图遍历，支持 L2、cosine 和 inner product：

```sql
CREATE INDEX idx_rabitq
ON items
USING GRAPH_INDEX (vec)
WITH (metric = 'cosine', quantizer = 'rabitq', m = 16, ef_construction = 160);
```

RaBitQ 不能与 `pq_m` 同时使用。设置 `memory_mode='compact'` 后，表中的原始向量不变，但索引不再保留一份原始向量镜像；查询、增量写入和重启恢复仍使用 RaBitQ code 遍历图。

PostgreSQL 的量化索引向量文件本身就是 code-only：`full` 与 `compact` 激活量化器后都不保存索引侧 raw mirror。PG 的 `compact` 额外保证量化器必须激活；样本或内存不足时建索引报错，不会回退成 raw。compact 未指定量化器时自动选择 PQ，unlogged 表不支持 compact。修改已有索引的 reloption 不会重写存储，需 `REINDEX` 后再用 `vexdb_index_info()` 和 `index_inspect()` 确认实际状态。

### 内省函数

```sql
-- 查看扩展版本
SELECT vexdb_version();

-- 查看向量维度
SELECT vector_dims([1.0, 2.0, 3.0]::FLOAT[3]);

-- 向量 L2 归一化
SELECT l2_normalize([3.0, 4.0]::FLOAT[2]);  -- [0.6, 0.8]

-- 查看所有 GRAPH_INDEX 详情（无参数，返回每个索引一行）
SELECT * FROM vexdb_index_info();
-- 过滤特定索引
SELECT * FROM vexdb_index_info() WHERE index_name = 'idx_vec';
```

### Python 接口

```python
import duckdb

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("LOAD '/path/to/vexdb_lite.duckdb_extension'")

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

## SQLite 扩展

SQLite 通过虚拟表直接保存业务向量和 ANN 索引：

```sql
.load ./vexdb_lite

CREATE VIRTUAL TABLE idx USING GRAPH_INDEX(
    embedding FLOAT[128],
    metric=cosine,
    quantizer=pq,
    pq_m=32,
    memory_mode=compact,
    m=16,
    ef_construction=160,
    ef_search=160,
    brute_force_threshold=0
);

INSERT INTO idx(rowid, embedding) VALUES (1, :json_or_f32_blob);
SELECT rowid, distance
FROM idx
WHERE embedding MATCH :query AND k = 10;
```

SQLite 支持 `quantizer=none|pq|rabitq`。PQ 的训练、编码、ADC 和 SIMD 距离来自 `common/`；SQLite 只处理虚拟表、事务、缓存和持久化。`pq_m` 必须是向量维度的正因数。compact 未指定量化器时自动选择 PQ，显式 `quantizer=none` 会报错。

PQ 和 RaBitQ 的 fixed data/code 分别写入 `%_graph` 的 kind 5/6。full 额外保存 kind 4 原始向量镜像；compact 不写 kind 4，但用户数据仍完整保存在 `%_vectors`。PQ 会按 `ef_search × 1.25` 扩展候选，再用 `%_vectors` 中的原向量精确重排。低内存 DiskStore 下，base、raw mirror 和量化 code 共用 `graph_memory_limit` 缓存预算。

---

## 共享算法内核

PG、DuckDB 和 SQLite 共享以下核心组件（位于 `common/`）：

| 组件 | 路径 | 说明 |
|------|------|------|
| 图索引算法 | `common/include/graph_index/` | 自研图索引算法、层级结构、邻居选择 |
| 距离计算 | `common/distance/` | SSE / AVX2 / AVX-512 / NEON 运行时分发 |
| Product Quantization | `common/quantizer/` | PQ 编码/解码、K-means 训练 |
| 容器模板 | `common/vtl/` | 内存分配器抽象、FixedSizeAllocator |
