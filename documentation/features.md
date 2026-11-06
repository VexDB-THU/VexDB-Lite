# 功能文档

VexDB-Lite 提供三种适配形式，共享图索引、距离计算和量化代码：

- **PostgreSQL 插件**（`vexdb_lite`）：作为 PostgreSQL 扩展提供 `vexdb_graph` 访问方法
- **DuckDB 扩展**（`vexdb_lite`）：作为 DuckDB out-of-tree extension 提供 `GRAPH_INDEX`
- **SQLite 扩展**（`vexdb_lite`）：提供 `GRAPH_INDEX` 虚拟表、距离函数与 VexFS 文件 SQL，支持静态注册和动态加载，详见 [SQLite 说明](../vexdb_sqlite/README.md)

文件命令、版本、快照和挂载详见 [VexFS 使用说明](../agent_files/macos/package/README.md)。本文后续主要介绍 PG 和 DuckDB；当前 CI 与验证范围见 [当前状态](current-status.md)。

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

当前开发分支还提供默认关闭的原向量L2辅助码补漏：

```sql
SELECT vexdb_build_signature('idx_vec'::regclass, 42);
SET vexdb.signature_rescue = on;
SELECT vexdb_signature_status('idx_vec'::regclass);
-- 执行检索后，在同一连接读取本次图扫描信息。
SELECT vexdb_last_search_info();
```

建码和状态查询要求拥有被索引的表。写入、VACUUM或未完成重建会使辅助码失效；开启补漏时，缺失或失效的原向量L2辅助码改走精确枚举，再由PG检查行可见性和SQL条件，代价可能更高。重建辅助码由调用者明确发起。默认关闭时仍沿用原图/PQ路径；PQ不使用这组辅助码。诊断中的`distance_counted=false`表示未统计估距，不能把显示的0当作没有计算。实际验证范围与未完成部分见[公平对照与质量保护](../docs/reports/2026-11-02_search-fairness-and-quality-protection.md)，不表示已发布或任意业务都保证99.9%。

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
-- 使用 duckdb -unsigned 启动，或在连接配置中允许未签名扩展
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
    parallel_workers = 4,      -- 显式 worker 数须为 1..1024；省略时使用 scheduler 线程数
    quantizer       = 'pq',    -- 可选：启用 Product Quantization
    pq_m            = 8,       -- PQ 子量化器数（要求 dim % pq_m == 0）
    memory_mode     = 'full'   -- 'full' 或 'compact'
);
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `metric` | `'l2'` | `'l2'` / `'cosine'` / `'ip'` |
| `m` | `16` | 图索引每节点最大邻居数 |
| `ef_construction` | `64` | 建图时搜索宽度 |
| `parallel_workers` / `threads` | 省略时自动 | 显式值为 1..1024；`parallel_workers` 优先，`1` = 串行，`0` 无效 |
| `quantizer` | `none` | `'pq'` 启用 Product Quantization |
| `pq_m` | — | PQ 子量化器数 |
| `memory_mode` | `'full'` | `'compact'` 仅存 PQ codes（内存节省 ~32×，不支持精确 refine） |

### 运行参数（GUC）

```sql
-- 查询搜索宽度（默认 40）
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
| `vexdb_ef_search` | `40` | 查询候选集大小 |
| `vexdb_brute_force_threshold` | `10000` | 行数低于此值走全量扫描 |
| `vexdb_pq_search_mode` | `'off'` | `'pq_only'` 跳过精确 refine，速度更快但召回略低 |
| `vexdb_pq_refine_k_factor` | `1.0` | refine 倍率；`1.0` = 关闭（只用 PQ 距离排序） |

### WHERE 过滤与精确排序

DuckDB 的 `GRAPH_INDEX` 在**单向量列**上建立。默认 `vexdb_filtered_search_mode='exact'`，有实际 `WHERE` 条件时保留先过滤、再按原向量精确排序的 SQL 路径。可显式设置 `'auto'` 试用标量与原图/PQ 联合执行：在当前事务快照下生成合法行集合，搜索时筛选结果，较小集合及不足返回时精确计算；不使用全局 ANN 的固定倍数候选。近似分支仍须单独验收召回和速度。

auto 遇到存储列等值条件时，还会按列统计和用户搜索预算估计剩余行数。预计规模小或非布尔列统计缺失时保留原生 SQL，让 DuckDB 继续选择标量过滤/ART；联合索引内部尚未直接用 ART 生成合法行集合。

DuckDB 还可显式打开 `vexdb_graph_bidirectional_search=true`，让原图与 PQ 图同时沿已有边反向搜索，补充单向遍历不易取得的候选。默认关闭，额外关系表按索引共用并计入内存；不改变默认精确过滤。20K 商品与 100K 文本的两种子新代理查询通过本轮 99.9% 门槛；完整性能和适用范围见[双向搜索报告](../docs/reports/2026-10-29_bidirectional-search-results.md)，不代表任意业务均保证此召回。

原生精确向量 TopN 还可显式试用 `vexdb_native_topn_fetch=true`，保持筛选和排序，只按所选物理行号读取输出列，减少二次扫描。默认关闭，不符合安全结构时仍用原生计划。密集合法行号的查询临时集合已改用按字节开销选择的位图；商品仍可选择更快且通过本轮质量门槛的原图配置。见[并发优化验收](../docs/reports/2026-10-29_concurrency-optimization-results.md)。

```sql
-- 只在 vec 列建索引
CREATE INDEX idx_vec ON items USING GRAPH_INDEX (vec);

-- 先执行 WHERE，再精确排序；当前不改写为 VEXDB_INDEX_SCAN
SELECT id
FROM items
WHERE category = 'image'
ORDER BY l2_distance(vec, [...]::FLOAT[128])
LIMIT 10;
```

过滤遵循 DuckDB 的 SQL 语义，包括等值（`=`）、BETWEEN、范围（`> / <`）、AND/OR 和 NULL。exact 的自然 SQL 不进入图队列；auto 在元数据筛选后，对搜索/精确回退阶段使用同一产品队列。无法安全解析的关联、易变条件等继续保留原 SQL。auto 尚未通过全部新代理查询的 99.9% 门槛，因而不默认开启；不能将无过滤 PQ 或某组速度直接套用于所有业务。用法及边界见 [DuckDB 元数据过滤](../vexdb_duckdb/README.md#元数据过滤) 和 [向标联合执行报告](../docs/reports/2026-10-29_filtered-hybrid-results.md)。

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

## 共享算法内核

PG 插件和 DuckDB 扩展共享以下核心组件（位于 `common/`）：

| 组件 | 路径 | 说明 |
|------|------|------|
| 图索引算法 | `common/include/graph_index/` | 自研图索引算法、层级结构、邻居选择 |
| 距离计算 | `common/distance/` | SSE / AVX2 / AVX-512 / NEON 运行时分发 |
| Product Quantization | `common/quantizer/` | PQ 编码/解码、K-means 训练 |
| 容器模板 | `common/vtl/` | 内存分配器抽象、FixedSizeAllocator |
