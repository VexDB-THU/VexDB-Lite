# `PinNode` Semantics Cross-Path Compare

## Purpose

本文对比 4 条常见路径里 `PinNode` 的真实语义差异：

1. `libvex-core` 纯内存构建：`MemoryNodeStore`
2. `libvex-core` 直连适配后端：`AdapterDirectBackend`
3. PG live bridge：`PgCoreLiveBinding`
4. PG readonly bridge / skeleton fallback：`PgCoreReadonlyBinding` / `PgCoreLowLevelBindingSkeleton`

目标是把这些问题说清楚：

- `PinNode` 到底是不是“真实 pin”
- 是否会分配临时对象
- 是否会拷贝数据
- `UnpinNode` 会不会回写
- 算法层能不能长期持有返回的裸指针

## One-Line Summary

4 条路径里，只有“直连后端 + bridge”语义上更接近“向底层存储申请访问权”；`MemoryNodeStore` 本质上只是**短生命周期视图句柄**，而 readonly / skeleton fallback 则更像**临时快照视图**。

## Path 1: `MemoryNodeStore`

实现位置：

- [vex_node_store_memory.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_node_store_memory.hpp)
- [node_store_memory.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/node_store_memory.cpp)

入口：

- `MemoryNodeStore::PinNode`
- `MemoryNodeStore::PinNodeForUpdate`

### Real behavior

`PinNode(node_id)` 做的事情非常少：

1. 检查 `node_id` 是否有效
2. new 一个 `MemoryNodeHandle`
3. 返回 `unique_ptr<NodeHandle>`

它不会：

- 不会上锁
- 不会做页 pin
- 不会做引用计数
- 不会做并发保护

### Data source

句柄读的是 `MemoryNodeStore` 内部数组切片：

- `headers_`
- `vectors_`
- `neighbors_l0_`
- `upper_neighbors_`
- `metadata_`

### Update semantics

`PinNodeForUpdate` 仍然只是返回一个 handle，只是这个 handle 暴露可写指针：

- `MutableHeader()`
- `MutableLevel0Neighbors()`
- `MutableUpperNeighbors()`

### Unpin cost

没有真正的 unpin 行为。`unique_ptr` 释放句柄对象本身而已。

### Safety boundary

安全前提是：

- 拿到句柄后立即读/写
- 不长期缓存内部裸指针

这是一种“数组视图”语义，不是底层存储一致性语义。

## Path 2: `AdapterDirectBackend`

实现位置：

- [vex_adapter_node_store_common.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_node_store_common.hpp)

入口：

- `AdapterDirectBackend::PinNode`
- `AdapterDirectBackend::PinNodeForUpdate`

### Real behavior

这里的 `PinNode` 不再直接碰内存数组，而是调用：

- `binding_->PinNode(node_id, false, view)`
- 或 `binding_->PinNode(node_id, true, view)`

也就是说，真正的 pin 语义被下推到底层 `AdapterLowLevelBinding`。

### Data source

返回的是一个 `AdapterNodeLayoutView`：

- `header`
- `vector`
- `level0_neighbors`
- `upper_neighbors_base`
- `metadata`
- `opaque`

这些指针是否直接指向底层存储，取决于绑定实现。

### Unpin semantics

`DirectNodeHandle` 的析构函数会调用：

- `binding_->UnpinNode(view_)`

所以这里的 `UnpinNode` 具有真实的后端资源释放语义。

### Update semantics

如果 `for_update=true`，后端应返回可写缓冲；是否立即写回、延迟写回、还是在 `UnpinNode` 时提交，也由 binding 决定。

### Practical meaning

这条路径才是真正意义上的“抽象 pin 接口”，算法层看到的是统一 handle，底层后端决定 pin/unpin 的真实成本和一致性边界。

## Path 3: PG Live Bridge

实现位置：

- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp)

入口：

- `PgCoreLiveBinding::PinNode`
- `PgCoreLiveBinding::UnpinNode`

### Real behavior

这里的 `PinNode` 会创建一个 `NodePinState`，并做一系列动作：

1. `store_.pin_vector_buffer(node_id)`
2. 分配临时数组保存：
   - `level0_neighbors`
   - `upper_neighbors`
   - `upper_counts`
   - `metadata`
3. 从 PG 存储层加载：
   - header
   - metadata
   - level0 neighbors
   - upper neighbors
4. 把这些地址填进 `PGNodeLayoutView`
5. `opaque = NodePinState*`

### Data source

这是一个“**部分直连 + 部分复制**”的混合语义：

- `vector` 来自 `pin_vector_buffer`
- 邻居、header、metadata 被拷贝进 `NodePinState`

也就是说，返回给算法层的不是全量原地映射，而是 bridge 层组装出来的一份节点视图。

### Unpin semantics

`UnpinNode` 会：

1. 如果是 writable pin，则 `FlushNode(*pin_state)`
2. delete `NodePinState`
3. 清空 view

所以：

- read pin: 更像临时读取快照
- write pin: 带回写语义

### Practical meaning

这条路径是真正和 PG 后端存储打交道的 live adapter。它的 `PinNode` 成本明显高于纯内存路径，因为它会 pin vector buffer、拷贝邻居和元数据，并可能在 unpin 时 flush。

## Path 4: PG Readonly Bridge / Skeleton Fallback

实现位置：

- [core_node_store_bridge_readonly.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_readonly.hpp)
- [core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/core_node_store_bridge.cpp)

### 4.1 Readonly bridge

入口：

- `PgCoreReadonlyBinding::PinNode`
- `PgCoreReadonlyBinding::UnpinNode`

行为：

1. `for_update` 一律拒绝
2. `PinNode` 分配一个 `PinnedNodeToken`
3. 从 PG 磁盘层读取：
   - vector buffer
   - base neighbors
   - upper neighbors
4. 把 header / counts / neighbors 组织成只读 view
5. `opaque = token.release()`

`UnpinNode` 只做：

- delete token
- 清空 view

这条路径是只读快照语义，没有回写。

### 4.2 Skeleton fallback

入口：

- `PgCoreLowLevelBindingSkeleton::PinNode`
- `PgCoreLowLevelBindingSkeleton::UnpinNode`

行为：

当走本地 fallback storage 时：

- `PinNode` 只是从 `local_nodes_` 里取指针并直接填 view
- `opaque = nullptr`
- `UnpinNode` 只是清空 view

这条路径在语义上更接近“轻量本地对象视图”，类似简化版的 `MemoryNodeStore`。

## Cross-Path Table

| Path | Real pin? | Data returned | Copy behavior | Unpin side effects | Writeback |
|---|---|---|---|---|---|
| `MemoryNodeStore` | No | Internal array slices | No extra copy for view | Destroy handle only | Direct in-memory mutation |
| `AdapterDirectBackend` | Backend-defined | `AdapterNodeLayoutView` | Backend-defined | Calls `binding->UnpinNode` | Backend-defined |
| `PgCoreLiveBinding` | Yes, bridge-managed | Mixed: pinned vector + copied neighbors/header/meta | Yes, bridge assembles node view | Delete `NodePinState`, optional flush | Yes for writable pins |
| `PgCoreReadonlyBinding` | Yes, readonly bridge token | Readonly snapshot view | Yes | Delete token only | No |
| `PgCoreLowLevelBindingSkeleton` fallback | No real backend pin | Local fallback object fields | No extra copy beyond local store | Clear view only | Local object mutation semantics |

## What Algorithm Code Should Assume

算法层最安全的假设应该是：

1. `PinNode` 返回的是**短生命周期句柄**
2. 句柄里的裸指针只在句柄存活期间有效
3. 不应该跨阶段长期持有 `Vector()` / `Neighbors()` 返回的地址
4. `UnpinNode` 可能只是释放小对象，也可能是一次真正的后端回写/解 pin

所以：

- 对 `MemoryNodeStore` 来说，这个约束显得“保守但安全”
- 对 PG live bridge / direct backend 来说，这是必须遵守的

## Practical Takeaway

最容易出错的地方在于把 `MemoryNodeStore::PinNode` 的轻量语义误当成所有路径的共同语义。

真实情况是：

- 在 core memory build 里，`PinNode` 接近“数组视图句柄”
- 在 adapter / PG live bridge 里，`PinNode` 接近“桥接层分配的一次节点访问会话”

从工程上说，后续如果要做：

- pin 次数统计
- pin 热点优化
- 生命周期压缩
- 零拷贝改造

必须先明确自己讨论的是哪一条路径。

## Related Files

- [vex_node_store.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_node_store.hpp)
- [vex_node_store_memory.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_node_store_memory.hpp)
- [node_store_memory.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/node_store_memory.cpp)
- [vex_adapter_node_store_common.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_node_store_common.hpp)
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp)
- [core_node_store_bridge_readonly.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_readonly.hpp)
- [core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/core_node_store_bridge.cpp)
