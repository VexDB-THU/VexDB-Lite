# VexDB-Duck Design V2

**Date:** 2026-04-28  
**Repo:** `/Users/sunji/Work/PG_VEXDB`  
**Target subtree:** `vexdb-duck/`

## 1. Goal

在当前 `pg_vexdb` 仓库内新增一个独立子树 `vexdb-duck/`，实现一个 DuckDB 向量 `GRAPH_INDEX` 插件，满足以下要求：

- 复用当前仓库已有的 `pg_vexdb` HNSW/graph-index 算法与距离计算代码。
- 禁止修改任何现有 `pg_vexdb` 文件。
- 禁止依赖 `VexDB-Lite` 内部现成索引实现逻辑作为核心算法来源。
- 允许借鉴 `VexDB-Lite/vexdb-duck` 的 DuckDB 扩展接线方式、优化器改写模式、benchmark 入口。
- 支持 DuckDB 优化器生成索引计划。
- 支持 DuckDB 执行器通过索引执行向量 ANN 检索。
- 能通过 `/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_sift_sql_benchmark.sh` 进行验证。

额外强约束：

- `GraphIndex` 的 build/search/connect/neighbor selection 等核心路径必须使用当前仓库 `pg_vexdb` 里的共享算法。
- `VexDB-Lite` 的 `GraphIndex`、`graph_index_core`、bridge helper、node store、allocator、search/build 逻辑不得进入本实现。

本设计文档是后续实现的唯一执行基线。

## 2. 核心约束

### 2.1 不可破坏约束

1. 不修改仓库根目录下现有 `include/`、`src/`、`knl/`、`distance/`、`vtl/` 等 `pg_vexdb` 已有实现。
2. 不直接搬运 `VexDB-Lite/vexdb-duck/index/graph_index_core.cpp` 或其同类 HNSW 核心逻辑。
3. DuckDB 子树必须自洽，所有新代码放在 `vexdb-duck/` 下。
4. 编译验证必须增量进行，不能最后一次性集成。

### 2.2 允许的复用方式

1. 直接复用当前仓库的 `pg_vexdb` 算法/距离/模板容器代码，保持单一实现源，不在 `vexdb-duck/` 下复制。
2. 在 `vexdb-duck/` 中新增 DuckDB 适配层，并通过统一依赖入口头 + 编译宏隔离来满足现有算法头的依赖。
3. 复用当前仓库的 `vtl/` 模板库，优先通过 include 路径直接引用。
4. 借鉴 `VexDB-Lite` 的：
   - DuckDB `BoundIndex` 接口实现模式
   - `IndexType` 注册方式
   - optimizer rewrite 的识别逻辑
   - `PhysicalCreateIndex` / `PhysicalIndexScan` 的执行框架
   - benchmark 驱动与 SQL 约定

## 3. 设计原则

### 3.1 最大代码同构

`pg_vexdb` 现有算法代码应保持为仓库中的单一实现源，不在 `vexdb-duck/` 内重复拷贝。  
DuckDB 适配优先通过“薄兼容层 + 统一依赖入口头 + 编译宏隔离”完成，而不是复制算法主体。

### 3.2 分层明确

DuckDB 接入层和图索引核心层必须严格分开：

- DuckDB 层负责 planner/executor/index lifecycle。
- core 层只负责 build/search/serialize 的图索引行为。

### 3.3 最小可用优先

第一阶段只做 benchmark 需要的最小闭环：

- `FLOAT[N]`
- `metric='l2'`
- `CREATE INDEX ... USING GRAPH_INDEX(vec)`
- `ORDER BY l2_distance(vec, query) LIMIT k`
- `EXPLAIN` 出现自定义索引扫描算子
- `10k` 和 `100k` SIFT 跑通

以下内容默认不纳入第一阶段：

- halfvector
- quantizer/PQ/RaBitQ
- vacuum/delete 完整语义
- WAL/recovery 等价
- 并发写入优化
- DuckDB 持久化格式的长期兼容承诺

## 4. 参考代码边界

### 4.1 来自 `pg_vexdb` 的复用来源

优先复用以下代码族：

- `include/graph_index/graph_index_algorithm.h`
- `include/graph_index/graph_index_struct.h` 中与候选点、entry info 相关的轻量结构
- `src/graph_index_build.cpp` 中的 build 组织方式
- `src/graph_index_scan.cpp` 中的 query/search 组织方式
- `distance/` 与 `src/distance/` 的 L2 距离分发链路
- `vtl/` 容器和并发基础设施

说明：

- `graph_index_algorithm.h` 直接复用。
- `graph_index_storage.h` 不直接复用实现，而是由 DuckDB 侧提供同接口 `DuckMemStore` 兼容实现。
- `graph_index_cluster.h` 不直接复用实现，而是由 DuckDB 侧提供 rowid/结果展开兼容层。

### 4.2 来自 `VexDB-Lite` 的允许借鉴范围

只允许借鉴以下非核心图算法部分：

- `vex_extension.cpp` 的扩展注册结构
- `optimizer/vex_optimizer.cpp` 的 planner pattern match 思路
- `optimizer/vex_physical_create_index.cpp` 的 DuckDB build pipeline 接线
- `optimizer/vex_physical_index_scan.cpp` 的执行器接线
- `test/run_sift_sql_benchmark.sh`
- `test/benchmark/vex_sift_sql_benchmark.cpp`

不允许将其 HNSW graph core 作为实现来源。
不允许把其 `GraphIndex` 内部数据结构、build/search 代码路径、bridge/runtime helper 带入 `vexdb-duck/index/graph_index.cpp`。

## 5. 总体架构

`vexdb-duck/` 分四层：

### 5.1 Layer A: DuckDB 扩展入口层

负责：

- 注册扩展
- 注册索引类型 `GRAPH_INDEX`
- 注册距离函数与配置项
- 安装 optimizer extension

主要文件：

- `vexdb-duck/vex_extension.cpp`
- `vexdb-duck/include/vex_extension.hpp`

实现细节：

- 使用 DuckDB C++ extension entrypoint，在 `LoadInternal()` 中集中完成注册。
- `LoadInternal()` 需要依次完成：
  1. 注册向量距离函数与辅助函数。
  2. 注册 `GRAPH_INDEX` 的 `IndexType`。
  3. 注册 optimizer extension。
  4. 注册配置项 `vex_ef_search`、`vex_brute_force_threshold`。
- `IndexType` 的关键绑定为：
  - `name = "GRAPH_INDEX"`
  - `create_instance = GraphIndex::Create`
  - `create_plan = GraphIndex::CreatePlan`
- 第一阶段不做复杂自动安装和外部依赖探测，扩展入口只负责最小注册。

### 5.2 Layer B: DuckDB Index 接入层

负责：

- `BoundIndex` 子类实现
- `CreateIndexInput` 参数解释
- 构建期数据接收和 finalize
- 暴露 `Search()` 接口给 physical scan

主要文件：

- `vexdb-duck/index/graph_index.cpp`
- `vexdb-duck/include/vex_graph_index.hpp`
- `vexdb-duck/optimizer/vex_physical_create_index.cpp`

实现细节：

- `GraphIndex` 继承 `duckdb::BoundIndex`，内部至少持有：
  - `m`
  - `ef_construction`
  - `metric`
  - `dimension`
  - `Distancer` 选择信息
  - `DuckMemStore` 或其持有者
  - row-id map / entry info / node count
- `GraphIndex::Create(CreateIndexInput &input)` 负责：
  - 校验第一列是 `FLOAT[N]`
  - 解析 `WITH (...)` 参数
  - 构造新的内存索引对象
- `GraphIndex::CreatePlan(PlanIndexInput &input)` 负责：
  - 生成 `PhysicalVexCreateIndex`
  - 接住 table scan child
  - 将 build 所需 metadata 传入 physical operator
- 第一阶段优先保障 bulk build finalize 路径，增量 `Insert/Append` 只需满足最小正确性。
- `GraphIndex` 内部的真正索引行为约束如下：
  - build 走 `pg_vexdb` 共享算法
  - ANN search 走 `pg_vexdb` 共享算法
  - 邻居裁剪、逐层下降、entry point 更新都走 `pg_vexdb` 共享算法
  - DuckDB 层只负责把 `row_t`、vector 数据和查询表达式接到共享算法上
  - 不允许在 `GraphIndex` 中引入 `VexDB-Lite` 的 graph core、bridge graph helper 或等价自有实现

### 5.3 Layer C: Planner/Executor 层

负责：

- 识别 `ORDER BY l2_distance(vec, query) LIMIT k`
- 校验目标表存在 `GRAPH_INDEX`
- 将逻辑计划替换为自定义逻辑算子
- 生成物理索引扫描算子
- 执行 ANN 搜索并按 row_id 回表

主要文件：

- `vexdb-duck/optimizer/vex_optimizer.cpp`
- `vexdb-duck/optimizer/vex_physical_index_scan.cpp`
- `vexdb-duck/include/vex_optimizer.hpp`
- `vexdb-duck/include/vex_physical_index_scan.hpp`

实现细节：

- optimizer 使用 DuckDB extension hook，而不修改 DuckDB 主源码。
- 新增：
  - `LogicalVexIndexScan`
  - `PhysicalVexIndexScan`
- `LogicalVexIndexScan` 至少保存：
  - 目标表引用
  - 目标 `GraphIndex` 引用
  - query vector expression
  - `k`
  - 输出列与回表所需 column ids
- `PhysicalVexIndexScan` 采用“一次搜索，分批吐 chunk”的执行模型：
  - 首次 `Execute()` 执行 ANN search 和回表
  - 结果暂存到 `ColumnDataCollection`
  - 后续 `Execute()` 从缓存集合持续输出

### 5.4 Layer D: Shared Core Reuse 层

负责：

- 图节点存储
- 向量存储
- 邻接关系存储
- HNSW 插入与搜索
- 距离计算
- 内存态索引状态管理

主要文件：

- 直接复用仓库根目录下的：
  - `include/graph_index/graph_index_algorithm.h`
  - `distance/`
  - `src/distance/`
  - `vtl/`
- `vexdb-duck/include/vex/vex_duckdb_compat.hpp`
- `include/graph_index/graph_index_depend.h`
- `vexdb-duck/include/vex_graph_index_depend_duck.hpp`

## 6. 目录结构

建议目录如下：

```text
vexdb-duck/
  CMakeLists.txt
  README.md
  vex_extension.cpp
  include/
    vex_extension.hpp
    vex_graph_index.hpp
    vex_optimizer.hpp
    vex_physical_create_index.hpp
    vex_physical_index_scan.hpp
    vex_distance.hpp
    vex/
      vex_duckdb_compat.hpp
      vex_allocator.hpp
      vex_storage_types.hpp
      vex_storage_io.hpp
      vex_rowid.hpp
      vex_graph_params.hpp
  functions/
    distance_functions.cpp
    vector_functions.cpp
    ann_search_functions.cpp
    index_info_function.cpp
  optimizer/
    vex_optimizer.cpp
    vex_physical_create_index.cpp
    vex_physical_index_scan.cpp
  index/
    graph_index.cpp
  test/
    benchmark/
      vex_sift_sql_benchmark.cpp
```

说明：

- `index/`、`optimizer/`、`functions/` 保存 DuckDB-facing 代码。
- `include/vex/` 放兼容层和环境替换类型。
- 根目录 `include/graph_index/graph_index_depend.h` 作为共享算法唯一依赖入口。
- `vexdb-duck/` 提供 DuckDB 专属依赖实现，但不提供同名覆盖头。

## 7. 兼容层设计

为了让 `pg_vexdb` 算法尽量少改，新增一个 DuckDB 兼容层：

### 7.1 类型替换

PostgreSQL 中的若干概念，在 `vexdb-duck` 中映射为：

- `ItemPointerData` -> `row_t` 或封装后的 `DuckRowId`
- `Datum` -> DuckDB `Value` 或运行时解码后的原始向量指针
- `MemoryContextAlloc/palloc/pfree` -> 本地 allocator 包装
- `elog(ERROR)` -> `throw duckdb::InvalidInputException` / `InternalException`
- PG 锁/原子/轻量同步 -> `std::mutex` / `std::shared_mutex` / `std::atomic`

### 7.2 宏与工具替换

新建 `include/vex/vex_duckdb_compat.hpp`，负责提供：

- `Assert` 替代
- 对齐宏
- 内存分配薄封装
- 最小日志/错误辅助
- 与 `vtl` 兼容的基础 typedef

同时新增统一依赖入口：

- `include/graph_index/graph_index_depend.h`

它内部通过编译宏分发到不同宿主环境：

- `PG_VEXDB_TARGET_PG`
- `PG_VEXDB_TARGET_DUCK`

DuckDB 侧具体实现入口建议为：

- `vexdb-duck/include/vex_graph_index_depend_duck.hpp`

### 7.3 适配策略

优先做以下模式：

1. 兼容头提供同名/近似接口。
2. `graph_index_algorithm.h` 直接从根目录复用，不复制到 `vexdb-duck/`。
3. 通过 `graph_index_depend.h` 把 `Store`、`PointExtensionContext`、`ItemPointerData` 等依赖分发到 PG 或 DuckDB 实现。
4. 不在 DuckDB 层重新实现 HNSW 数学与 graph connect 逻辑。

## 8. 图存储模型

### 8.1 逻辑结构要求

虽然 DuckDB 不能直接复用 PostgreSQL relation/page/fork 模型，但逻辑结构必须和 `pg_vexdb` 对齐：

- 元数据
- 向量池
- base layer 邻接池
- upper layer 邻接池
- row_id 映射

### 8.2 第一阶段状态形式

第一阶段只实现单进程内存态索引，不实现 `Serialize/Deserialize`。

也就是说：

- `CREATE INDEX` 完成后索引可立即用于查询
- 当前进程生命周期内可用于 smoke test 与 benchmark
- 不承诺 checkpoint/reload 后恢复索引内容

持久化在后续阶段单独设计，不在第一阶段范围内。

### 8.3 存储接口仿真

为了满足“模仿存储接口和存储结构”，在 core 内部定义本地 store 接口，保留类似概念：

- `vector_pool`
- `basepoint_pool`
- `upperpoint_pool`
- `entry_info`
- `get_neighbors_data()`
- `get_itempointer()` 的 DuckDB 变体

也就是说：

- 外部持久化介质第一阶段暂不实现
- 内部逻辑分层仍然保持与 `pg_vexdb` 对齐

## 9. 算法内核设计

### 9.1 选型

核心算法直接复用 `GraphIndexAlgorithm<Store, Distancer>`，保留：

- 逐层下降搜索
- `search_layer`
- 邻居选择
- 插入连接
- entry point 更新

### 9.2 第一阶段 store 选择

第一阶段只实现一个 DuckDB 专用内存 store：

- `DuckMemStore`

它模仿 `MemStore` 的结构，而不是先实现 `DiskStore`。  
它通过 `graph_index_depend.h` 暴露与 `graph_index_algorithm.h` 兼容的接口，而不是复制 `MemStore` 原文件。

原因：

1. benchmark 生命周期是“建索引后立即查询”。
2. DuckDB 插件先实现稳定的 in-memory/search 路径更可控。
3. 以后若要做更强持久化，可以在不动算法主体的情况下新增持久化 store。

### 9.3 距离能力范围

第一阶段只要求：

- float32
- L2

保留扩展点：

- cosine
- inner product
- SIMD dispatcher

距离代码同样优先直接复用根目录 `distance/` 与 `src/distance/`，仅在必要时通过兼容层解决环境依赖。  
如果某些 SIMD 分发单元迁移成本过高，则先让 L2 scalar 路径跑通，后续补 SIMD。

## 10. DuckDB 索引对象设计

`GraphIndex : public BoundIndex` 负责把 DuckDB 生命周期映射到 core。

### 10.1 必要职责

- 解析 `CREATE INDEX ... WITH (...)`
- 保存 `m`、`ef_construction`、`metric`、`dimension`
- 在 build finalize 时构建 core graph
- 在 query 时执行 ANN search
- 为 optimizer 提供索引元信息

额外约束：

- `GraphIndex` 不直接依赖 PostgreSQL 头或符号。
- `GraphIndex` 对共享算法暴露的 store/point 语义尽量与 `pg_vexdb` 对齐。
- `GraphIndex` 对外接口需清晰区分：
  - `BuildBulk(...)`
  - `SearchANN(...)`
- `GraphIndex` 的算法实现来源必须单一：
  - 允许来源：本仓库 `pg_vexdb` 共享算法
  - 不允许来源：`VexDB-Lite` 的 `GraphIndex`/`graph_index_core`/相关 helper

### 10.2 第一阶段支持的方法

必须稳定实现：

- `Create`
- `CreatePlan`
- `Append` / `Insert` 的最小版本
- `CommitDrop`
- `GetInMemorySize`
- `Verify` / `ToString` 的最小版本

每个关键方法的实现要求：

- `Create`
  - 校验 schema
  - 解析 options
  - 构造新索引或恢复已有索引
- `CreatePlan`
  - 只负责 create-index 物理计划
- `GetInMemorySize`
  - 汇总 vectors、nodes、neighbor pools、row-id map
- `Verify` / `ToString`
  - 提供最小一致性检查与摘要输出

### 10.3 低优先级方法

可以先给最小实现或受限实现：

- `Delete`
- `Vacuum`
- `MergeIndexes`
- delta index 相关能力

前提是不会影响 benchmark 执行。

### 10.4 `GraphIndex::Create` 详细流程

1. 从 `input.unbound_expressions[0]` 读取向量列类型。
2. 校验 `LogicalTypeId::ARRAY` 且 child type 为 `FLOAT`。
3. 提取 `dimension = ArrayType::GetSize(type)`。
4. 解析 index options：
   - `m`
   - `ef_construction`
   - `metric`
5. 创建 `GraphIndex` 对象。
6. 返回 `unique_ptr<BoundIndex>`。

### 10.5 `GraphIndex::CreatePlan` 详细流程

1. 接收 DuckDB planner 生成的 table scan。
2. 生成 create-index 所需的 projection/filter child。
3. 构造 `PhysicalVexCreateIndex`。
4. 将 table scan 作为 child 挂入 create-index operator。
5. 后续由 `PhysicalVexCreateIndex` 驱动 sink/combine/finalize。

## 11. Build Pipeline 设计

DuckDB 的 `CREATE INDEX` 构建管线采用两段式：

### 11.1 Sink 阶段

`PhysicalVexCreateIndex` 收集输入表的：

- `row_id`
- 向量列

先累积到本地内存缓冲：

- `std::vector<float> all_vectors`
- `std::vector<row_t> all_row_ids`

实现细节：

- `PhysicalVexCreateIndex` 需要定义：
  - `GlobalSinkState`
  - `LocalSinkState`
- `LocalSinkState` 持有线程本地缓冲，避免每行都争用全局锁。
- `Sink()` 中执行：
  1. 从 `DataChunk` 提取 vector column
  2. 从 row chunk 提取 `row_id`
  3. 将向量按扁平 `float` 数组写入 local buffer
- `Combine()` 中把 local buffer 合并到 global buffer。

### 11.2 Finalize 阶段

调用 `GraphIndex` 的 bulk build：

1. 初始化 `DuckMemStore`
2. 分配节点与向量存储
3. 按 `pg_vexdb` HNSW build 流程插入
4. 建立 entry point

第一阶段不强制做多线程 build；如果工作量可控，再对齐 `pg_vexdb` 的 parallel build 思路。

实现细节：

- `Finalize()` 中调用 `GraphIndex::BuildBulk(all_vectors, all_row_ids, dimension)`。
- `BuildBulk()` 内部流程：
  1. 初始化 `DuckMemStore`
  2. 构造 distancer
  3. 为每个向量生成插入上下文
  4. 调用共享 `GraphIndexAlgorithm` 完成 build
  5. 固化 entry info、row-id map、统计信息
- 构建完成后，将 `global_index` 返回给 DuckDB finalize 路径。

### 11.3 为什么优先使用 `CreatePlan`

DuckDB 同时支持：

- `IndexType.build_*` 回调族
- `BoundIndex::CreatePlan` 自定义物理计划

本项目第一阶段优先选择 `CreatePlan + PhysicalVexCreateIndex`，原因是：

1. 与现有 `VexDB-Lite` DuckDB 插件的组织方式更接近。
2. 更容易精确控制 `row_id` 与 vector 列采集。
3. 更利于后续接 parallel build。

## 12. Optimizer 设计

### 12.1 识别目标 SQL 形态

第一阶段只支持：

```sql
SELECT ...
FROM t
ORDER BY l2_distance(vec, [query]) 
LIMIT k
```

也兼容：

- `array_distance`
- `list_distance`
- `<->`

前提是它们都映射到 L2。

### 12.2 改写条件

只有满足以下条件才触发索引计划：

1. 目标表存在 `GRAPH_INDEX(vec)`
2. 向量列与索引列一致
3. 距离函数与索引 metric 匹配
4. `LIMIT` 存在
5. 排序方向正确

### 12.3 逻辑计划替换

optimizer 执行：

1. 识别 `ORDER BY distance(...) LIMIT k`
2. 解析 query vector
3. 构造 `LogicalVexIndexScan`
4. 替换默认 table scan + sort + limit 路径

### 12.4 Pattern Match 详细规则

第一阶段只识别如下结构：

- `LogicalLimit(LogicalOrder(LogicalGet(table)))`
- `LogicalTopN(LogicalGet(table))`

并在 `ORDER BY` 中识别：

- `l2_distance(vec_col, const_vec)`
- `l2_distance(const_vec, vec_col)`
- `<->`

其中：

- `vec_col` 必须来自单表 `LogicalGet`
- `const_vec` 可以是常量、cast 后常量，或 `list_value(...)` 常量表达式

### 12.5 失败回退策略

以下场景不做 rewrite，直接回退 DuckDB 原计划：

- 没有 `LIMIT`
- 距离函数不匹配
- query vector 既不是常量，也不是可安全求值的单值表达式
- 表上不存在匹配列的 `GRAPH_INDEX`
- 多表 join 场景下无法明确下推到单表 ANN 扫描

## 13. 执行器设计

`PhysicalVexIndexScan` 负责：

1. 计算或提取 query vector
2. 获取 `vex_ef_search`
3. 调用 `GraphIndex::Search()`
4. 拿到有序 `row_id`
5. 通过 DuckDB 表扫描/回表机制取回结果行

第一阶段只要求支持 benchmark 中的：

- 返回 `id`
- 依据 `row_id` 取基表记录

### 13.1 `PhysicalVexIndexScan` 执行流程

1. 初始化 `OperatorState`。
2. 第一次 `Execute()`：
   - 计算 query vector expression
   - 读取 `vex_ef_search`
   - 调用 `GraphIndex::SearchANN()`
   - 得到 `vector<row_t>` 与 `vector<float>`
   - 通过 DuckDB 表访问接口批量回表
   - 将结果写入 `ColumnDataCollection`
3. 后续 `Execute()`：
   - 从 `ColumnDataCollection` 顺序吐出 chunk

### 13.2 为什么先物化到 `ColumnDataCollection`

原因：

1. DuckDB physical operator 的 chunk 模型更适合“先搜完、后分批吐出”。
2. 可以把 ANN 搜索和回表逻辑解耦。
3. 第一阶段先追求 correctness，避免流式回表把执行模型做复杂。

### 13.3 回表接口

第一阶段采用最直接方式：

- 用 row_id 结果列表驱动表访问
- 只回 benchmark 需要的列
- 保持回表结果顺序与 ANN 搜索结果顺序一致

### 13.4 第一阶段生命周期约束

由于不实现 `Serialize/Deserialize`，第一阶段生命周期约束明确为：

- 索引以进程内内存对象形式存在
- 重点验证 `CREATE INDEX` 后立即查询
- smoke test 与 benchmark 在同一进程生命周期内完成

## 14. 运行时配置

新增 DuckDB 扩展配置项：

- `vex_ef_search`：默认 64
- `vex_brute_force_threshold`：默认 64 或与图规模相关的保守值

第一阶段不引入过多配置项，避免行为面过宽。

## 15. 编译与集成方案

### 15.1 构建模式

采用 DuckDB out-of-tree extension 构建方式。

依赖源码树：

- DuckDB: `/Users/sunji/Work/duckdb`

### 15.2 目标产物

期望产物：

- loadable extension：`vex.duckdb_extension`

### 15.3 CMake 原则

- `vexdb-duck/CMakeLists.txt` 独立存在
- 优先对接 DuckDB `build_loadable_extension(...)`
- include path 显式引入：
  - DuckDB headers
  - repo root
  - `vtl/`
  - `distance/`
  - `vexdb-duck/include`

### 15.4 DuckDB 插件编译宏

DuckDB 目标编译时定义：

- `PG_VEXDB_TARGET_DUCK`

PostgreSQL 目标编译时定义：

- `PG_VEXDB_TARGET_PG`

共享头通过该宏选择依赖入口：

- PG 分支保留现有语义
- Duck 分支包含 `vex_graph_index_depend_duck.hpp`

### 15.5 第一阶段必须落地的文件

第一阶段至少需要这些文件是真正有实现的，而不是占位：

- `vexdb-duck/vex_extension.cpp`
- `vexdb-duck/index/graph_index.cpp`
- `vexdb-duck/optimizer/vex_optimizer.cpp`
- `vexdb-duck/optimizer/vex_physical_create_index.cpp`
- `vexdb-duck/optimizer/vex_physical_index_scan.cpp`
- `vexdb-duck/functions/distance_functions.cpp`
- `vexdb-duck/include/vex_graph_index_depend_duck.hpp`
- `vexdb-duck/include/vex/vex_duckdb_compat.hpp`
- `vexdb-duck/include/vex/vex_duck_memstore.hpp`
- `vexdb-duck/include/vex/vex_duck_point.hpp`

## 16. 验证策略

### 16.1 编译阶段

每完成一个子系统就编译一次：

1. 先空扩展可编译
2. 再 optimizer/executor 骨架可编译
3. 再 core 层可编译
4. 最后整体 link 成功

### 16.2 功能 smoke

最小 SQL：

```sql
LOAD '.../vex.duckdb_extension';
CREATE TABLE t (id INTEGER, vec FLOAT[3]);
INSERT INTO t VALUES (1, [1,2,3]), (2, [4,5,6]), (3, [7,8,9]);
CREATE INDEX idx ON t USING GRAPH_INDEX (vec) WITH (metric='l2');
EXPLAIN SELECT id FROM t ORDER BY l2_distance(vec, [1,2,3]::FLOAT[3]) LIMIT 2;
SELECT id FROM t ORDER BY l2_distance(vec, [1,2,3]::FLOAT[3]) LIMIT 2;
```

检查点：

- 扩展可加载
- 建索引成功
- `EXPLAIN` 出现 `VEX_INDEX_SCAN`
- 查询返回正确的近邻顺序

### 16.3 Benchmark

使用：

`/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_sift_sql_benchmark.sh`

先跑：

- `10k`

再跑：

- `100k`

验收关注：

- benchmark 能完成
- explain 走索引
- recall 不明显异常

## 17. 实施顺序

### Phase 1: 设计与脚手架

1. 新增本设计文档
2. 新建 `vexdb-duck/` 目录
3. 接入 DuckDB 扩展构建骨架

### Phase 2: DuckDB 外壳

1. 扩展入口
2. 距离函数注册
3. `GRAPH_INDEX` 类型注册
4. optimizer / physical operator 骨架

### Phase 3: 核心移植

1. 接入根目录 `pg_vexdb` 核心算法和必要头文件
2. 建立 DuckDB 兼容层
3. 跑通最小 build/search

### Phase 4: 计划与执行接通

1. `CREATE INDEX` build pipeline
2. optimizer 识别
3. physical index scan
4. row_id 回表

### Phase 5: 验证

1. smoke test
2. 10k SIFT benchmark
3. 100k SIFT benchmark

## 18. 风险与应对

### 风险 1: `pg_vexdb` 核心依赖 PostgreSQL 过深

应对：

- 先只迁移 `MemStore` 路径
- 建立兼容头而不是全面改写算法

### 风险 2: DuckDB `BoundIndex` 生命周期要求比预期更复杂

应对：

- 先确保 benchmark 所需路径稳定
- 非关键方法提供保守实现

### 风险 3: SIMD 分发迁移成本高

应对：

- 第一阶段允许先用 scalar L2 跑通
- 后续再逐步接入 SSE/AVX

### 风险 4: optimizer 改写对 DuckDB 版本敏感

应对：

- 严格对齐本机 `/Users/sunji/Work/duckdb` 当前头文件接口
- 优先使用现有 `VexDB-Lite` 中已验证的 planner/executor 壳层模式

## 19. 定义完成

满足以下条件即视为第一阶段完成：

1. 仓库中新增 `vexdb-duck/` 子树。
2. 现有 `pg_vexdb` 文件零改动。
3. 能编译出可加载的 DuckDB 扩展。
4. `CREATE INDEX ... USING GRAPH_INDEX(vec)` 可执行。
5. `EXPLAIN` 对 benchmark 风格 SQL 显示自定义索引扫描算子。
6. 执行器实际通过索引完成 ANN 检索。
7. `/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_sift_sql_benchmark.sh` 至少 `10k` 跑通，并继续推进到 `100k`。
