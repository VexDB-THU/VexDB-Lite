# DuckDB Disk Index Target Design

**Date:** 2026-04-29  
**Repo:** `/Users/sunji/Work/PG_VEXDB`  
**Scope:** 将当前 `vexdb-duck` 从“内存态 graph index”演进为“持久化图结构 + 查询时按需读取节点/向量”的目标方案

## 1. 背景

当前 `vexdb-duck` 已经具备以下能力：

- `CREATE INDEX ... USING GRAPH_INDEX`
- 复用 `pg_vexdb` 共享算法建图和搜索
- optimizer 生成 `VEX_INDEX_SCAN`
- benchmark 查询可以通过索引路径执行

但当前索引实现仍然是**进程内内存态**：

- 建索引时把图结构建到 `runtime_->store`
- 查询时直接访问内存中的 `vectors/base_points/upper_points`
- 没有持久化图结构
- 没有 reopen 后的恢复
- 没有“查询时按需从索引存储读取节点/向量”

用户当前要求是进入下一阶段：

> DuckDB 侧持久化图结构格式，搜索时能够按需读取节点或者向量，图的磁盘存储结构模仿 `pg_vexdb` 来做。

这份报告回答：

1. 目标方案应该长什么样
2. 它和当前实现差在哪
3. 需要新增哪些结构与接口
4. 如何分阶段开发

## 2. 目标结论

目标 DuckDB 方案不是“把当前内存索引简单序列化成一个 blob”。

目标应当是一个**逻辑布局与 `pg_vexdb` 对齐**、同时适配 DuckDB 存储接口的磁盘图索引：

- 元数据单独存储
- base layer 节点单独存储
- upper layer 节点单独存储
- 原始向量单独存储
- 查询时按需读取：
  - 先读少量图节点邻接
  - 再按需读向量
  - 不把全部图结构和全部向量一次性加载到内存

更直接地说：

- **算法仍然使用 `pg_vexdb` 共享算法主体**
- **store 从当前 `DuckMemStore` 扩展为 `DuckDiskStore`**
- **索引磁盘布局模仿 `pg_vexdb` 的 `meta/base/upper/vector` 分层**

## 3. 当前实现 vs 目标实现

### 3.1 当前实现

当前实现可概括为：

- `GraphIndex::BuildBulk(...)`
  - 读取整表向量
  - 调共享算法 `insert(...)`
  - 图结构建在内存 `runtime_->store`
- `GraphIndex::SearchANN(...)`
  - 调共享算法 `search(...)`
  - 直接读取内存邻接和内存向量
- `PhysicalVexIndexScan`
  - 搜索得到 `row_id`
  - 再回表

当前核心数据结构：

- `std::vector<std::vector<char>> vectors`
- `std::vector<BasePointRec> base_points`
- `std::vector<UpperPointRec> upper_points`

这些都属于易失性内存结构。

### 3.2 目标实现

目标实现应改为：

- `GraphIndex::BuildBulk(...)`
  - 共享算法仍然驱动建图
  - 但节点、邻接、向量写入 DuckDB 索引持久化存储
- `GraphIndex::SearchANN(...)`
  - 构造 `DuckDiskStore`
  - 共享算法通过 store 接口按需拉取邻接和向量
- `PhysicalVexIndexScan`
  - 不关心图结构是否在内存
  - 只依赖 `SearchANN(...)` 返回 `row_id`

核心变化不是算法，而是 store 背后的读写介质。

## 4. 要模仿的 `pg_vexdb` 逻辑分层

从 `pg_vexdb` 现有实现看，图索引逻辑至少有这些层：

- `metapage`
- `base_layer`
- `upper_layer`
- `elems`
- `_vec` 向量数据

DuckDB 侧虽然不能照搬 PG 的 `Relation/Buffer/Page/Fork`，但逻辑结构应保持一致。

因此目标 DuckDB 索引磁盘布局建议也拆成四大段：

1. `meta`
2. `base`
3. `upper`
4. `vec`

可选第五段：

5. `rowid map` 或 `elem/tid`

## 5. 目标磁盘布局

### 5.1 Meta 段

用途：

- 全局索引头
- 打开索引时先读这段
- 告诉系统其余段的位置和参数

建议字段：

- magic/version
- dimension
- `m`
- `ef_construction`
- metric
- precision type
- id type
- entrypoint id
- entry level
- entry current layer idx
- num vectors
- base segment offset / size
- upper segment offset / size
- vec segment offset / size
- optional rowid segment offset / size

这部分应在逻辑上对应 PG 的 `GraphIndexMetaPageData`。

### 5.2 Base 段

用途：

- 存 level-0 邻接关系

每个 base record 应包含：

- `neighbors_id[2m]`
- 可选 `dist[2m]`
- 可选邻接统计位图

在 `pg_vexdb` 里，base 邻接和上层节点是分开的；DuckDB 侧也应保留这个分层。

### 5.3 Upper 段

用途：

- 存 level>0 节点

每个 upper record 应包含：

- `lower_layer_idx`
- `id`
- `neighbors_info[2m]`
  - 前半是 neighbor id
  - 后半是 neighbor current layer idx
- 可选 `dist[m]`
- 可选邻接统计位图

这必须与当前共享算法期望的内存布局一致，因为 `graph_index_algorithm.h` 已明确依赖“前半 id、后半 layer idx”的连续布局。

### 5.4 Vec 段

用途：

- 存原始向量

要求：

- 固定长度
- 每个向量大小 = `dimension * sizeof(float)`，再按 `vector_aligned_size` 对齐
- 支持通过 `vector_id -> offset` O(1) 定位

这部分逻辑上对应 PG 的 `_vec` 文件。

### 5.5 RowId / Elem 段

用途：

- `vector_id -> row_t`
- 未来支持去重、多 tid、删除标记时也方便扩展

第一阶段最简可只存单个 `row_t`。

## 6. DuckDB 持久化承载方式

这里有两条候选路径。

### 方案 A：单个 `IndexStorageInfo` blob 分段打包

做法：

- `SerializeToDisk(...)` 生成一个二进制 blob
- blob 内部再划分 `meta/base/upper/vec/rowid`

优点：

- 实现最快
- 不需要深入 DuckDB block manager

缺点：

- 查询时很可能要整体反序列化或大块映射
- 不利于真正的“按需读取”
- 更像“序列化对象”，不像“索引存储层”

### 方案 B：基于 DuckDB index storage 的多段持久化

做法：

- `IndexStorageInfo` 只保存段信息
- 实际数据存为多个持久化段/块
- 查询时按 offset/segment 读取

优点：

- 真正支持按需读取
- 更接近 `pg_vexdb` 的磁盘结构思路
- 后续更容易加入缓存层

缺点：

- 实现复杂度更高
- 需要更深入使用 DuckDB 存储接口

**建议：**  
如果目标是“查询时按需读取节点或向量”，应直接选 **方案 B**。  
方案 A 只能作为临时过渡，不是最终解。

## 7. 查询时按需读取的目标路径

### 7.1 搜索阶段应该怎么读

共享算法需要的核心读操作是：

- 读取 entry info
- 读取某个节点的邻接
- 读取某个向量
- 批量距离计算时读取少量候选向量

因此 `DuckDiskStore` 至少应支持：

- `get_entry(...)`
- `get_point_info<base/upper>(...)`
- `get_data(id)`
- `get_distance_batch(...)`
- `get_itempointer(...)`

这些接口名字最好继续沿用当前共享算法所需的形状。

### 7.2 推荐的 I/O 策略

推荐分两层：

1. **逻辑按需读取**
   - 每次只读当前搜索 frontier 需要的节点/向量
2. **小型页缓存**
   - base/upper/vec 三类段分别做小缓存

这样可以保证：

- 搜索不需要全量载入
- 同时避免每个候选都触发一次底层 I/O

### 7.3 读取粒度

建议读取粒度：

- base / upper：按 record 所在块读取
- vec：按对齐后的固定长度 record 读取

也就是说，不必严格做 PG 那种 `Buffer/Page` API，但逻辑上仍然应该“基于块而非单字段读取”。

## 8. 目标 `DuckDiskStore` 接口

推荐新增：

- `DuckDiskStore`

其接口尽量对齐共享算法需要：

### 必要接口

- `get_entry(...)`
- `release_entry_lock(...)`
- `get_point_info<true>(id)`
- `get_point_info<false>(cur_layer_idx)`
- `get_data(id)`
- `get_distance(...)`
- `get_distance_batch(...)`
- `get_itempointer(id, fn)`
- `fetch_vec_from_heap(...)`

### Build 期接口

- `assign_vector_id<base/upper>()`
- `add_elem(...)`
- `add_vector(...)`
- `add_basepoint(...)`
- `add_upperpoint(...)`
- `set_entrypoint(...)`

### Search 期额外优化接口

- `prefetch_base_block(...)`
- `prefetch_upper_block(...)`
- `prefetch_vec_block(...)`

第一版可以不做显式 prefetch，但接口可以预留。

## 9. Build 策略

### 9.1 不建议直接“先全内存后全量序列化”

如果最终目标是按需读取，那么“建完内存图再整体 dump”会造成两套逻辑：

- 内存 build 逻辑
- 磁盘布局逻辑

更好的方式是：

- 构建时就写入最终持久化布局

### 9.2 推荐的 build 方案

可以采用两阶段：

1. **向量和节点 ID 分配阶段**
   - 为每个向量分配 `vector_id`
   - 写 `vec` 段
   - 初始化 base/upper 空 record
2. **共享算法建边阶段**
   - 共享算法运行时通过 `DuckDiskStore` 更新邻接

这样 build 结束后索引已经是最终磁盘格式，不需要额外重排。

## 10. 缓存策略建议

如果直接每次随机读磁盘，性能会很差。  
因此即便是“磁盘型搜索”，也建议在 DuckDB 侧加一层轻缓存。

推荐缓存：

- `meta cache`
- `base block cache`
- `upper block cache`
- `vec block cache`

缓存原则：

- 块级缓存
- LRU 或 clock 即可
- 不要求复杂并发优化作为第一版目标

## 11. 与当前代码的差异

当前需要改动的核心点：

### `GraphIndex`

当前：

- `runtime_->store` 是内存 store

目标：

- `runtime_` 应能挂两种实现
  - `DuckMemStore`
  - `DuckDiskStore`

建议：

- 先保留 `DuckMemStore`
- 新增 `DuckDiskStore`
- 通过配置或索引状态选择使用哪种 store

### `SearchANN(...)`

当前：

- 直接拿 `runtime_->store`

目标：

- 打开 `DuckDiskStore`
- 从 `meta` 恢复 entry info
- 通过共享算法按需访问 `base/upper/vec`

### `PhysicalVexCreateIndex`

当前：

- `Finalize()` 后内存 build

目标：

- `Finalize()` 后直接构建持久化图结构

## 12. 开发顺序建议

### Phase 1：设计与接口

1. 更新设计文档
2. 定义 `DuckDiskStore` 接口
3. 定义 `meta/base/upper/vec/rowid` 的 record layout

### Phase 2：持久化写入

1. 实现 `SerializeToDisk(...)`
2. Build 期写最终持久化布局
3. 验证 reopen 后可恢复元信息

### Phase 3：按需读取搜索

1. `SearchANN(...)` 切到 `DuckDiskStore`
2. 实现按需读取 `base/upper/vec`
3. 加最小块缓存

### Phase 4：验证

1. 小表 smoke
2. reopen 后查询
3. `10k`
4. `100k`

## 13. 设计判断

如果目标只是“把索引持久化”，可以先做 blob。

如果目标明确是：

> 搜索的时候能够按需读取节点或者向量

那就不应该再走“整包序列化 blob”方案，而应该直接上：

- `meta/base/upper/vec` 分段布局
- `DuckDiskStore`
- 块级按需读取

这才是真正模仿 `pg_vexdb` 存储思路的 DuckDB 版本。

## 14. 直接回答

对你的这条要求：

> DuckDB 侧持久化图结构格式，要求搜索的时候能够按需读取节点或者向量，图的磁盘存储结构模仿 `pg-vexdb` 来做。

我的建议方案是：

1. 在 DuckDB 侧新增持久化 `DuckDiskStore`
2. 磁盘布局拆成：
   - `meta`
   - `base`
   - `upper`
   - `vec`
   - `rowid`
3. `SearchANN()` 通过共享算法按需读取：
   - 节点邻接从 `base/upper` 读
   - 原始向量从 `vec` 读
4. 不走“整个索引 blob 一次性反序列化到内存”的过渡方案作为最终方案

