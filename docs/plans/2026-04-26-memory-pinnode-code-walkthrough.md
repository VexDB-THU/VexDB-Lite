# Memory Build `PinNode` Code Walkthrough

## Scope

本文只说明 `libvex-core` 在 **内存构建** 场景下，`MemoryNodeStore::PinNode` / `PinNodeForUpdate` 的真实代码行为。

不覆盖：

- PG / DuckDB 适配层里的底层 page pin 语义
- `PinNode` 在 bridge / low-level binding 中的行为差异
- 性能优化方案

重点回答：

1. `PinNode` 到底返回什么
2. 它是否真的“pin 住”底层存储
3. 在 HNSW 内存构建过程中它被如何使用
4. 这个语义的边界是什么

## Interface

`NodeStore` 抽象定义在：

- [vex_node_store.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_node_store.hpp)

其中两组接口是核心：

- `std::unique_ptr<NodeHandle> PinNode(node_id_t node_id) const`
- `std::unique_ptr<MutableNodeHandle> PinNodeForUpdate(node_id_t node_id)`

`NodeHandle` 只暴露只读访问：

- `Header()`
- `Vector()`
- `Level0Neighbors()`
- `UpperNeighbors()`
- `Metadata()`

`MutableNodeHandle` 在此基础上再开放可写访问：

- `MutableHeader()`
- `MutableLevel0Neighbors()`
- `MutableUpperNeighbors()`
- `SetLevel0Count()`
- `SetUpperCount()`

这套接口的设计意图是：让图算法统一写成“先取句柄，再从句柄看节点内容”，而不是让算法代码直接了解底层存储布局。

## Memory Store Implementation

内存实现定义在：

- [vex_node_store_memory.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_node_store_memory.hpp)
- [node_store_memory.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/node_store_memory.cpp)

`MemoryNodeStore` 内部用几块大数组保存节点内容：

- `headers_`
- `vectors_`
- `neighbors_l0_`
- `upper_neighbors_`
- `metadata_`
- `alive_`

节点分配时：

- `AllocateNode(...)` 会向这些数组尾部追加新槽位
- `headers_` 追加一个 `NodeHeader`
- `vectors_` 追加 `dim_` 个 `float`
- `neighbors_l0_` 追加 `m * 2` 个邻居槽位
- `upper_neighbors_` 追加一个 `UpperBlock`
- `metadata_` 追加元数据区

也就是说，内存构建阶段的节点数据并不是 page/block 形式，而是数组切片。

## `PinNode` Real Behavior

实现位置：

- [node_store_memory.cpp:217](/Users/sunji/Work/VexDB-Lite/libvex-core/src/node_store_memory.cpp:217)

代码行为非常直接：

1. 检查 `node_id` 是否有效
2. 如果无效，返回 `nullptr`
3. 如果有效，构造一个 `MemoryNodeHandle`
4. 用 `unique_ptr<NodeHandle>` 返回

`PinNodeForUpdate` 也是同样模式，只是返回 `MutableNodeHandle`。

这意味着：

- **没有锁**
- **没有引用计数**
- **没有底层 page pin**
- **没有 buffer 生命周期管理**
- **没有并发保护**

它的真实语义不是“把底层节点 pin 住”，而是：

- “给当前 `node_id` 返回一个短生命周期的节点视图句柄”

## What The Handle Reads

`MemoryNodeHandle` 本身也很薄，只保存：

- `store_`
- `node_id_`

然后每次访问时按 `node_id_` 去 `MemoryNodeStore` 对应数组里取切片。

关键读路径：

- `Header()`:
  - `&store_->headers_[node_id_]`
- `Vector()`:
  - `store_->vectors_.data() + node_id_ * dim_`
- `Level0Neighbors()`:
  - `store_->neighbors_l0_.data() + node_id_ * (m * 2)`
- `UpperNeighbors(level_idx)`:
  - `store_->upper_neighbors_[node_id_].neighbors.data() + level_idx * m`
- `Metadata()`:
  - `store_->metadata_.data() + node_id_ * metadata_size_`

所以 `PinNode` 返回的不是稳定的存储页句柄，而是“数组索引入口”。

## Update Handle Behavior

`PinNodeForUpdate` 返回的仍然是 `MemoryNodeHandle`，只是以 `MutableNodeHandle` 身份暴露可写方法。

可写行为包括：

- 修改 `NodeHeader`
- 修改 level0 邻居数组
- 修改 upper 层邻居数组
- 修改邻居计数

依然没有：

- 锁
- 版本检查
- 并发冲突检测

所以它更像：

- “允许通过同一个句柄改内存数组”

而不是：

- “获得受保护的写 pin”

## How Graph Algo Uses It

主要调用点在：

- [graph_algo.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/graph_algo.cpp)

典型模式有三种。

### 1. 搜索入口点

在 `SearchLayer(...)` 里：

- `store_.PinNode(ep)`
- 读取 entry point 的 header / vector
- 算 entry point 到 query 的距离

这是典型的“拿句柄，立刻消费”。

### 2. 读取当前节点及其邻居

同样在 `SearchLayer(...)`：

- `store_.PinNode(current.node_id)`
- 读取当前节点邻居列表
- 对每个邻居再 `PinNode(neighbor_id)`
- 取邻居向量算距离

所以搜索路径里的 `PinNode` 调用频率很高，但每次只是短暂读取。

### 3. 插入和反向边更新

在 `InsertNode(...)`、`SelectNeighbors(...)` 等路径里：

- `PinNode(new_node_id)` 或 `PinNodeForUpdate(new_node_id)`
- 读取或修改新节点邻居
- 再 `PinNodeForUpdate(sel_id)` 修改已有节点反向边

更新路径也遵循同一个模式：

- 句柄短生命周期
- 拿到就用
- 用完即释放

## Why This Is Fast

对 `MemoryNodeStore` 来说，`PinNode` 的成本很低：

1. 一个 `node_id` 有效性检查
2. 一次小对象分配
3. 后续通过数组偏移取数据

所以在内存构建阶段，大量 `PinNode` 调用通常不会表现出类似 PG buffer pin 的重开销。

这也是为什么把 core memory build 的 `PinNode` 行为直接类比到 PG 适配层会产生偏差。

## Semantic Boundary

这里的边界很重要。

`MemoryNodeStore::PinNode` 成立的前提是：

- 句柄是**短生命周期**
- 调用方不会长期缓存 `Vector()` / `Neighbors()` 返回的裸指针
- 调用方不会把这些裸指针跨越复杂重分配周期长期保存

当前 `graph_algo.cpp` 基本满足这个前提：

- `PinNode`
- 立刻读/改
- 离开作用域后释放 `unique_ptr`

所以它当前是安全的。

但如果以后有人把 `PinNode` 取得的内部指针拿出去长期保存，那么这个语义就不再可靠。

## Difference From Backend Pin

这也是它和 PG / DuckDB 适配层的根本区别：

- `MemoryNodeStore::PinNode`
  - 是“内存数组视图句柄”
- backend adapter 的 `PinNode`
  - 才更接近“向底层存储申请当前节点访问权”

因此不能把内存实现的 `PinNode` 误认为通用后端协议。

从架构上说，`PinNode` 在 core 里更像算法访问抽象，不是统一的底层存储一致性模型。

## Practical Summary

在 `libvex-core` 内存构建阶段，`PinNode` 的实际行为可以一句话概括：

- 它不是 page pin，也不是 lock；它只是返回一个基于 `node_id` 的短生命周期节点视图句柄，后续所有读写都直接落在 `MemoryNodeStore` 的数组切片上。

这也是为什么：

- 调用次数很多
- 单次开销低
- 但语义依赖“拿到即用，不长期持有内部裸指针”

## Related Files

- [vex_node_store.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_node_store.hpp)
- [vex_node_store_memory.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_node_store_memory.hpp)
- [node_store_memory.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/node_store_memory.cpp)
- [graph_algo.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/graph_algo.cpp)
