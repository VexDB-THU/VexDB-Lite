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
| `parallel_workers` / `threads` | 当前 `SET threads` | 图构建 worker 数及 PQ 训练并发上限；同时指定时 `parallel_workers` 优先。PQ 子码本数大于 64 时仍沿用串行训练路径 |
| `quantizer` | `none` | `'pq'` 启用 Product Quantization；`'rabitq'` 启用 RaBitQ 图搜索 |
| `pq_m` | (无) | PQ 子量化器数；要求 `dim % pq_m == 0` |
| `memory_mode` | `'full'` | `'full'` 存索引侧原始向量及量化 code；`'compact'` 支持 PQ 和 RaBitQ，释放索引侧原始向量，无法精确 refine；表中原向量和图等成本仍须计费 |

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

当前索引只接受单个 `FLOAT[N]` 向量列；查询可带 WHERE 条件。不要把标量列附加到索引定义中，当前源码会明确拒绝多列索引。

默认 `vexdb_filtered_search_mode='exact'`，带实际过滤条件的向量 top-k 保留 DuckDB 先过滤、再按原向量精确排序的路径。需要试用标量过滤与向量索引联合执行时，可显式开启：

```sql
SET vexdb_filtered_search_mode='auto';
SET vexdb_pq_search_mode='pq_graph'; -- 索引需已使用 PQ；搜索参数仍须按业务验收
-- 恢复精确过滤语义：新计划保留自然 SQL，已有联合计划执行时也改用精确计算
SET vexdb_filtered_search_mode='exact';
```

`auto` 支持单表及可解析的透明视图：先在当前事务快照下执行完整条件，建立本次查询的合法行集合，再让原图/PQ 搜索只把合法行纳入结果。不合格的图节点仍可用于导航。合法集合较小时直接精确计算，图返回不足时精确回退；不会固定取全局 4 倍候选后过滤。OR/IN/NULL、参数变化、分页及旧快照保留 SQL 语义。易变条件、动态过滤、无法安全解析的关联和向量字段自身的过滤等继续走原 SQL；复杂 JOIN 尚未接入联合索引加速。

`EXPLAIN` 中联合路径标注 `Filter: snapshot-visible scalar prefilter`；`filtered-auto (... / exact)` 表示实际分支还取决于合法行数和搜索预算，不能只凭出现 `VEXDB_INDEX_SCAN` 就认定已经使用 PQ 或获得提速。新增视图改写也需显式开启 auto，避免默认改变已有路径。

联合索引路径通过扫描元数据生成合法集合，尚未直接用 ART 生成集合。对于存储列上的等值条件，auto 按列统计与用户的搜索预算估计筛选规模；预计只剩少量行时保留 DuckDB 原生计划，由它继续选择过滤/ART，避免额外扫描与零散回表。缺少非布尔列统计时也保留原生计划。这是执行成本选择，不按业务字段名或答案判断，统计偏差不会改变原生精确计算的正确性。多列排序及复杂 JOIN 等形式保留自然 SQL。

**auto 包含近似搜索，不保证任意数据和参数都达到 99.9%。** 开发数据通过后，另一批新生成的向量代理查询仍发现宽泛条件下的漏召回，因而默认保留 exact。合法记录数量、权限检查与近邻召回分别验收；不应把“返回够 k 条”当作召回合格。旧精确过滤证据见[过滤验证报告](../docs/reports/2026-10-27_filtered-vector-search-results.md)，本次实现与验收见[向标联合执行报告](../docs/reports/2026-10-29_filtered-hybrid-results.md)。

后续诊断发现，部分区域沿原有有向边无法从其他区域到达，多取候选仍会漏。可显式开启 `SET vexdb_graph_bidirectional_search=true`，在原图及 `pq_graph` 中也沿原边反向搜索。它只缓存节点关系，不缓存查询距离；按图写入/重载失效重建，现有索引不必重建。20K 商品、100K 文本的新两种子查询已达到本轮 99.9% 门槛，默认仍关闭，性能及边界见[双向搜索报告](../docs/reports/2026-10-29_bidirectional-search-results.md)。文本反向关系表约增加 13.60 MB，计入索引内存，原向量存储仍需计费；`pq_only` 不使用这项搜索。

```sql
CREATE INDEX idx_items_meta
ON items
USING GRAPH_INDEX (vec);

SELECT id FROM items
WHERE category = 'book' AND status = 'active'
ORDER BY l2_distance(vec, [...]::FLOAT[128])
LIMIT 10;
```

---

## 产品量化（PQ）

PQ 把向量切成 `pq_m` 段，每段独立 k-means 量化到 256 个 centroid，存储压缩到 `pq_m` 字节。

训练会检查样本和中心的 NaN/Inf、中心数量以及重复中心。只有某段实际训练数据的不同取值不足 256 种时，才允许该段出现重复中心，因此重复、稀疏或常量数据仍可建库；输入足够多样却训练出重复中心会报错。该判断根据实际子向量执行，不按业务或测试数据名称放行。

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
- 原始向量在 PQ 训练后被释放。例如 128 维、`pq_m=16` 时，单条原向量 512 字节、PQ 编码 16 字节；码本、行号和图等开销另计，不能据此推算总内存缩小 32 倍
- 查询自动扫描全部 PQ 编码，无法 refine；数据增多时扫描工作量也会增加
- 适合先按内存预算、召回和实际查询延迟验证的场景，目前没有百亿规模验收数据

### 搜索模式（GUC）

```sql
SET vexdb_pq_search_mode = 'pq_only';   -- 扫描全部 PQ 编码，实际速度需测量
SET vexdb_pq_search_mode = 'off';       -- 原始向量图搜索（compact 模式会自动启用 pq_only）
SET vexdb_pq_refine_k_factor = 4.0;     -- PQ 扫描取最接近的 4×k 个候选，再用原向量精排
```

这里的 4.0 是用户可配置的精排倍数，与已移除的过滤查询固定 4 倍规则不同；带 WHERE 的查询按上文的先过滤、再精确排序执行。

可选的 `pq_graph` 用 PQ 距离沿现有图找候选，减少全量编码扫描。开启精排后，对图返回的全部候选计算原向量距离，避免再按 PQ 分数截断而丢掉真近邻：

```sql
SET vexdb_pq_search_mode = 'pq_graph';
SET vexdb_ef_search = 256;
SET vexdb_pq_refine_k_factor = 16.0;    -- >1 开启精排；k×factor 是搜索宽度下限
```

要求索引使用 PQ、保留原向量，且距离类型为 L2 或 cosine；compact 和内积索引会明确报错。`ef_search` 是搜索宽度，并非实际访问节点数。`EXPLAIN` 在索引路径上分别显示 `pq-graph`、`pq-full-scan` 或 `hnsw`；小表仍受 `vexdb_brute_force_threshold` 控制。

图搜索宽度为 `max(ef_search, k×factor)`（不超过图节点数），精排只处理图实际返回的候选；重复向量按物理节点计算一次距离，再展开有效行。`factor=1` 仍关闭精排；`pq_only` 全扫仍按 `k×factor` 精排。这里的精排量可能明显高于旧版的固定短名单，不能沿用旧版吞吐。原向量与编码均需保留，没有新增索引存储字段。

只读 PQ 遍历在图共享锁内复用已有邻接表副本，并在更省内存时使用查询局部位图去重；原向量精排复用运行时距离分发，查询临时容器按作用域释放。不新增持久缓存或索引字段。

2026-10-28的补测中，既有`ef1536/factor16`设置在两组模拟商品新查询召回均100%，100K文本新查询为99.960938%。实现优化让商品PQ吞吐提高约5%–19%、文本提高24%–35%；商品仍慢于较快的达标原图，文本在这组查询上更快。共同降低到`ef1024`时，新文本仅99.882812%，未达99.9%，没有采纳。参数不按数据集名称自动分流，64连接尾延迟仍需单独验收，详见 [PQ开销与并发延迟结果](../docs/reports/2026-10-28_pq-cost-reduction-results.md)。

以下旧版实验是在二次 PQ 截断仍存在时完成：100K SIFT 和 10K 文本开发查询均为 99.9% Recall@10；编码估距分别少约 96.7% 和 67.5%。小库并发时没有稳定提速。详见 [历史 SQL 基准](../docs/reports/2026-10-27_product-quantization-benchmark.md)。当前修改和新查询验收见 [PQ 召回与延迟定位](../docs/reports/2026-10-28_pq-recall-and-latency-results.md)，默认仍为 `off`，实际业务须同时检查召回和延迟。

补测100K文本后，同一 `ef=256、factor=16` 配置为99.45%召回；预先登记的 `ef=512、factor=16` 为99.80%，尚未达到99.9%。在接近99.4%的召回档，64连接PQ图为2053–2088 QPS，原向量图为1189–1218 QPS；这是12核共享桌面的本地SQL实测，不能直接当作线上容量。全部并发曲线及原图高召回对照见 [10万条文本并发报告](../docs/reports/2026-10-27_text-100k-concurrency-results.md)。

另行冻结的旧版高召回试验使用以下配置，当时只精排160条原向量（`LIMIT 10`）；当前实现会精排全部已返回图候选：

```sql
SET vexdb_pq_search_mode = 'pq_graph';
SET vexdb_ef_search = 1536;
SET vexdb_pq_refine_k_factor = 16;
```

同一100K文本索引上，两个各1024条的新代理查询组分别为99.960938% / 99.951172% Recall@10，16/64连接和两个反序轮次的返回集合一致。这是平均召回，仍有九条查询各漏一个真近邻。扩大搜索范围增加了编码估距工作；原向量仍保留。原登记的原图ef1024/1536/2048对照通过了速度门槛，但更快的ef768补充对照在B组首轮16/64连接均未通过，次轮16连接因磁盘保护缺测，因此没有稳定提速的验收结论。该配置须用业务查询验证，默认仍为 `off`。失败的邻居补查和长度修正原型没有保留为产品模式，完整结果见 [候选补齐报告](../docs/reports/2026-10-27_pq-candidate-rescue-results.md)。

---

原生精确 TopN 可显式试用 `SET vexdb_native_topn_fetch=true`：筛选和距离排序仍由 DuckDB 执行，只把同表物理行号的二次回表改为直接读取所选行；支持事务本地新增行，默认关闭。密集合法行号的临时集合会按内存开销选择位图，稀疏行号仍用哈希集合；不按业务名称切换。质量与速度边界见[并发优化报告](../docs/reports/2026-10-29_concurrency-optimization-results.md)。

## 运行参数（GUC）

| GUC | 默认值 | 说明 |
|---|---|---|
| `vexdb_ef_search` | 40 | 图索引查询搜索宽度。提高 → 召回 ↑ / QPS ↓ |
| `vexdb_graph_bidirectional_search` | false | 同时沿底层边反向搜索，原图/PQ 图共用；增加按索引共享的反向关系表，需按业务验收 |
| `vexdb_native_topn_fetch` | false | 显式试用原生精确向量 TopN 后按行号直接回表，减少二次扫描；复杂结构保留原计划 |
| `vexdb_search_concurrency` | 0 | 数据库范围的向量索引执行槽；0 不限，-1 使用硬件核数提示，正数由部署配置；使用 `SET GLOBAL` |
| `vexdb_search_queue_timeout_ms` | 0 | 等待向量执行槽的超时毫秒数；0 不限等待时间，执行时间另行控制 |
| `vexdb_brute_force_threshold` | 10000 | 行数 ≤ 此值走 brute force 精确路径 |
| `vexdb_filtered_search_mode` | `'exact'` | `'exact'` 保留过滤后的精确 SQL；`'auto'` 显式试用标量与图/PQ 联合执行，小等值条件仍可保留原生计划 |
| `vexdb_pq_search_mode` | `'off'` | `'off'` 原向量图；`'pq_only'` 全扫编码；`'pq_graph'` 使用 PQ 距离沿图查找候选 |
| `vexdb_pq_refine_k_factor` | 1.0 | >1 开启精排；pq_only 取 k×factor，pq_graph 精排全部图候选且 k×factor 是宽度下限；compact 不支持精排 |

```sql
SET vexdb_ef_search = 100;       -- 高召回
SET vexdb_ef_search = 10;        -- 高 QPS

SET vexdb_brute_force_threshold = 0;     -- 关闭全表行数阈值；过滤与安全检查仍可回退
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

### 参数复用与并发

持续检索服务可为每个工作连接准备查询模板，每次绑定新的向量和条件。C++ 示例中 `FLOAT[1536]` 应与实际列维数一致：

```cpp
auto request = con.Prepare(
    "SELECT rec_id,payload FROM records WHERE route=$2 AND live "
    "ORDER BY l2_distance(vec,$1::FLOAT[1536]) LIMIT 10");
if (request->HasError()) throw std::runtime_error(request->GetError());

// 每次请求构造参数；同一连接复用上面的 request。
duckdb::vector<duckdb::Value> values;
for (float value : query) values.push_back(duckdb::Value::FLOAT(value));
duckdb::vector<duckdb::Value> parameters{
    duckdb::Value::ARRAY(duckdb::LogicalType::FLOAT, std::move(values)),
    duckdb::Value::INTEGER(route)};
auto result = request->Execute(parameters, false);
if (result->HasError()) throw std::runtime_error(result->GetError());
```

每次搜索使用当次参数和事务快照。复用模板可减少长向量 SQL 的重复解析和计划开销；新调用方式的召回和并发速度仍须用实际负载验收。DuckDB 的说明见[预备语句调优](https://duckdb.org/docs/1.5/guides/performance/how_to_tune_workloads)。

连接池大小与向量执行槽是两项配置。`vexdb_search_concurrency` 管理 `VEXDB_INDEX_SCAN`，原生精确 TopN 和之前的 SQL 准备由宿主连接池管理；`EXPLAIN` 和 `vexdb_search_queue_state()` 可用于确认实际路径。`-1` 使用硬件核数提示；容器配额或同机其他任务限制了可用 CPU 时，应按部署资源配置并发量，再核对包含入口等待时间的 P95 与吞吐。

连接池应由调用应用配置，同一连接同一时刻只执行一个请求；事务从开始到提交或回滚都应占用同一个连接。不要把本次测试的池大小当成固定产品上限。64个客户端共享较少连接与64条SQL同时执行是不同的负载，应分别记录物理连接数、等待时间、超时和实际返回质量。

[`benchmark_request_invocation.cpp`](test/benchmark/benchmark_request_invocation.cpp) 保存了本轮实际使用的测试客户端，可用同一模型和搜索参数比较literal、参数化与可配置连接池；`pool_connections=0`为每客户端独立连接，正整数为共享池最大连接数，实际连接数不超过客户端数。池的等待、参数构造、执行、完整正文读取与校验、归还都在请求时钟内。统一4连接的试验在24组速度对照中有8组未达标，未推广为产品策略，不能据此承诺所有负载提速。该池是测试工具，不提供生产环境的取消、超时或事务会话管理接口；测试数据里的业务字段和正文也是代理负载，不代表真实客户请求。

只读模型测试需要落盘时，配置文件应显式指定独立、可写的`temp_directory`以及`max_temp_directory_size`；本轮统一临时盘上限为4GiB，不能当成免费内存或实际峰值。临时目录应与`out`结果目录并列，不能放在尚未创建的结果目录内。[`review_request_invocation.py`](test/benchmark/review_request_invocation.py) 按用例清单从实际返回ID与纳秒时钟独立复核召回、过滤、正文大小、QPS、P95和A/A，失败与未执行项仍判失败；它不会从缓存距离推算ANN速度。

[`benchmark_business_load.cpp`](test/benchmark/benchmark_business_load.cpp) 用固定到达率补测正常负载，计时从每个请求的计划到达到完整ID和正文读完，包含调度、会话和SQL等待。配置的`rate`决定业务请求量，`clients`决定实际持久会话数，两者独立；不会把64个会话自动缩成4个。`normal_windows`控制正常窗口，`stress_windows`在最后一个正常窗口前插入单列的闭环压力窗口，保留同一实例和会话来观察压力撤去后的恢复。压力吞吐和尾延迟分别报告，不能当作正常到达率的承诺。该驱动属于测试工具，流量是代理负载，不能证明客户突发流量、冷启动或长期运行表现。

编译时复用匹配的原生DuckDB SDK；驱动关闭SDK扩展自动加载，显式加载`core_functions`和目标模块，并核对加载模式，防止静态旧模块替测。运行配置沿用前一驱动的数据库、查询和搜索选项，另需`cohort`、`rate`、`normal_windows`、`stress_windows`、`stress_seconds`、`drain_seconds`，且`request_mode=parameter`。独立检查入口为[`review_business_load.py`](test/benchmark/review_business_load.py)，读取真实CSV、精确答案和元数据，逐256请求复核召回、过滤、正文大小、完整到达时钟及P95/P99；缺窗口不算通过。固定查询池的重复执行是负载验收，不是新增盲测样本。本轮协议和边界见[验收计划](../docs/plans/2026-11-03_recall-and-pg-io-validation.md)。

参数、过滤、事务和重开回归入口如下，头文件与 SDK 库须匹配扩展的 DuckDB 版本和编译环境：

```bash
bash vexdb_duckdb/test/run_prepared_vector_test.sh \
  "$DUCKDB_SRC_INCLUDE" "$DUCKDB_LIBRARY" "$VEXDB_EXTENSION" build/tests/prepared-vector
bash vexdb_duckdb/test/run_prepared_filter_pruning_test.sh \
  "$DUCKDB_SRC_INCLUDE" "$DUCKDB_LIBRARY" "$VEXDB_EXTENSION" build/tests/prepared-filter-pruning
```

前一个用例在512条合成数据上以满预算精排检查参数和快照，共192项；后一个检查相同参数在写入、统计过滤和快照变化后的语义，共48项。两架构均已原生通过，并接入Linux工作流；GitHub Actions尚未实际运行。大库召回和性能结果见[当前验收报告](../docs/reports/2026-11-03_recall-and-pg-io-validation.md)。

---

## 构建

### 依赖

- 编译器：gcc 9.3+ / clang 10+（gcc 8.5 会撞 DuckDB 主仓 `expression_map_t` 模板问题）
- CMake ≥ 3.28，< 4.x
- Boost 头文件（DuckDB 适配层需要 `boost/preprocessor`；不要求 PG 专用的完整 patched Boost）
- Python 3（DuckDB 主仓构建系统需要）

### 从源码构建

```bash
git clone https://github.com/VexDB-THU/VexDB-Lite.git
cd VexDB-Lite
bash build_duck.sh setup    # clone DuckDB v1.5.2 + cmake configure（首次 ~5 min）
bash build_duck.sh build    # 编 vex 扩展（~20 min）
```

产物：`build/duck/v1.5.2/build/extension/vexdb_lite/vexdb_lite.duckdb_extension`。设置 `DUCKDB_VERSION` 后使用对应版本目录。

### Boost 路径自定义

`vexdb_duckdb/CMakeLists.txt` 用 `find_package(Boost REQUIRED)`。如果 Boost 不在系统默认位置：

```bash
export BOOST_INCLUDEDIR=/path/to/boost
export BOOST_ROOT=/path/to/boost
bash build_duck.sh build
```

### 测试与调试

```bash
python3 -m pip install -r tests/spec/requirements.txt
bash tests/spec/_lib/docker/run_duckdb.sh test
# Debug 构建需先准备源码
bash build_duck.sh setup
bash build.sh dev -j4
```

spec runner 在本地运行，不需要 Docker，并负责先渲染和部署用例。
Debug runner 位于 `build/duck/v1.5.2/duckdb_src/build/debug/test/unittest`。
CI 范围与历史流程归档见 [当前状态](../documentation/current-status.md)。

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

PQ + `memory_mode='compact'` 会在建图和训练完成后释放原始向量；它不能避免构建阶段的原向量内存，最终总内存还包括图、码本和行号等结构。

---

## 测试与 benchmark

详见仓库根的 [README](../README.md) 和 `tests/spec/`、`vexdb_duckdb/test/`。


## 查询排队

### 查询分阶段统计

可按连接打开 `VEXDB_INDEX_SCAN` 的统计，默认关闭，不改变搜索参数和返回结果：

```sql
SET vexdb_search_profile = true;
-- 执行实际的向量检索 SQL 后，紧接着读取，期间不要插入其他 SQL。
SELECT unnest(vexdb_last_search_profile());
SET vexdb_search_profile = false;
```

返回的是**上一条 SQL 内本扩展索引算子的汇总**，不是完整 SQL 耗时；没有执行该算子时返回 NULL，原生精确路径也不会冒用上一条检索的统计。读取统计本身也是 SQL，再次读取会对应刚才的读取语句。状态按连接保存，仅保留当前/上一条语句，不记录 SQL 文本、向量或行号。

主要阶段单位为纳秒：`prepare_ns`、`prefilter_ns`、`queue_wait_ns`、`mask_ns`、`search_ns`、`visibility_ns`、`exact_ns`、`fetch_ns`、`output_ns`。`operator_ns` 包含算子内部总时间，不包含事务启动、规划、原生上游/下游算子和客户端等待。多算子统计是各算子之和，可能有并行重叠。

`graph_lock_ns` 是获取图锁的耗时；`pq_prepare_ns` 是查询归一化和 PQ 查表准备；`pq_navigation_ns`、`pq_distance_ns`、`pq_refine_ns` 分别记录图导航、PQ 单码/批量估距、原向量精排。`raw_navigation_ns` 对应原图搜索。它们是 `search_ns` 的子项，PQ 估距也包含在导航中，**不能相加当总耗时或纯 CPU 时间**。全码路径的 PQ 估距只覆盖估距调用，堆维护等开销仍在 `search_ns` 中。

工作量包括元数据扫描/合法行数、实际搜索次数、重试次数、后续搜索耗时、PQ 估距数、图候选数、原向量精排数、快照排除数和精确路径估距行数。`exact_selected_calls` 与 `exact_fallback_calls` 区分直接选择精确路径和候选不足后的精确回退。`retry_search_ns` 是第二次及后续搜索调用的总耗时，并不等于其中全部工作都重复，也没有计算跨轮次唯一节点数。原图目前不提供独立的逐距离计数。

诊断会增加时钟读取和少量状态管理开销，特别是 PQ 批量估距计时；不要把开启统计的吞吐直接当默认产品性能。请同时记录客户端完整 SQL 耗时、线程 CPU、召回率和资源限制。

测试负载可使用 [`benchmark_request_lifecycle.cpp`](test/benchmark/benchmark_request_lifecycle.cpp)：它按固定到达时刻记录客户端等待、参数构造、SQL 执行和结果读取，保留过期、超时限及未执行请求；[`review_request_lifecycle.py`](test/benchmark/review_request_lifecycle.py) 从原始记录独立重算。这是测试驱动，不是新的产品请求队列；其中请求预算在执行前检查，运行中超限只记账，不能当成逐请求取消保证。配置、实际运行和边界见[完整请求计时报告](../docs/reports/2026-10-30_search-alignment-results.md)。

### 搜索并发额度

DuckDB 的 `VEXDB_INDEX_SCAN` 支持数据库实例范围的可配置排队，原图、`pq_graph` 和 `pq_only` 共用；不按业务数据名称分流。

```sql
SET GLOBAL vexdb_search_concurrency = -1; -- 使用机器逻辑 CPU 数作为上限提示
SET GLOBAL vexdb_search_concurrency = 0;  -- 默认：不限制；也可显式指定任意正整数
SET vexdb_search_queue_timeout_ms = 100;  -- 当前会话最多等待100ms；0为不设等待期限
SELECT vexdb_search_queue_state();
RESET GLOBAL vexdb_search_concurrency;
```

上限必须显式使用 `SET GLOBAL` / `RESET GLOBAL`。`-1` 是硬件提示，不读取容器CPU配额，也不自动保证最佳吞吐；可以根据部署实测指定正整数。等待超时只约束排队，不是整个SQL的执行时限；取消使用宿主的 `Connection::Interrupt()`。搜索已进入内核时，在本次搜索返回后检查取消。

有空位且没有等待者时直接占位，满额才按进入队列的顺序等待。等待发生在图锁之前，名额在回表之前释放；事务、会话与原来的连接保持一致。统计包含暂时预留名额，峰值是实例历史值，不能当作同时使用 CPU 的线程数。默认 exact 以及 auto 主动保留的原生过滤计划不进入图搜索队列；联合索引路径先在图锁之外筛选元数据，再为搜索或精确回退申请名额。

当前功能及取消、超时、事务并发、历史快照、重开与竞争回归已通过，质量检查中商品召回100%、100K文本99.960938%。**性能验收尚未全部通过**：有尾延迟收益，但部分吞吐点下降，旧版A/A复跑也有波动；默认仍关闭，不能把它解释为默认路径已证明完全无额外成本。详见[排队接入与PG构建结果](../docs/reports/2026-10-28_search-admission-results.md)。

本地回归入口（源码头文件须与共享库同版本）：

```bash
bash vexdb_duckdb/test/run_search_admission_test.sh   <duckdb-source/src/include> <libduckdb> <extension> <new-build-dir>
```
