# `PinNode` Data Source Map

## Purpose

本文只聚焦 PG live bridge 路径里，一次 `PinNode` 返回给算法层的各块数据分别来自哪里：

1. `header`
2. `vector`
3. `level0 neighbors`
4. `upper neighbors`
5. `metadata`

重点回答：

- 哪些字段来自 bridge 层状态
- 哪些字段来自底层 store
- 哪些字段只是 bridge 层拼装结果
- `LoadHeader / LoadLevel0Neighbors / LoadUpperNeighbors` 各自到底调用了什么

## The Returned View

PG live bridge 最终返回的是一个 `PGNodeLayoutView`。

定义在：

- [vex_adapter_node_store_common.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_node_store_common.hpp)

核心字段：

- `header`
- `vector`
- `level0_neighbors`
- `level0_count`
- `upper_neighbors_base`
- `upper_counts`
- `metadata`
- `opaque`

这说明算法层拿到的不是一个“单块节点对象”，而是一个由 bridge 层组装出来的多段视图。

## `PinNode` Top-Level Flow

PG live bridge 入口在：

- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp)

`PinNode(...)` 的顺序是：

1. `pin_vector_buffer(...)`
2. `LoadHeader(...)`
3. `LoadMetadata(...)`
4. `LoadLevel0Neighbors(...)`
5. `LoadUpperNeighbors(...)`

也就是说：

- `vector` 先拿
- `header` 单独组装
- 邻接表再单独读取并拷贝

## `header` Data Source

入口：

- [core_node_store_bridge_live.hpp:392](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:392)

`LoadHeader(node_id, header)` 的数据来源顺序如下。

### 1. 默认构造值

`LoadHeader()` 先构造默认 header：

- `row_id = node_id`
- `level = 0`
- `deleted = 0`
- `level0_count = 0`
- 其余字段清零

这说明 `LoadHeader()` 不是“直接从物理页读取 header struct”，而是“先有一个逻辑默认值”。

### 2. `node_to_row_id_`

接着会查：

```cpp
auto row_it = node_to_row_id_.find(node_id);
header.row_id = row_it == node_to_row_id_.end()
    ? static_cast<vex::row_id_t>(node_id)
    : row_it->second;
```

也就是说，`row_id` 当前来自 bridge 维护的行映射，而不是独立 header cache。

### 3. `upper_chain_by_node_`

然后它会用：

```cpp
auto chain_it = upper_chain_by_node_.find(node_id);
header.level = ...
```

重新修正 `level`。

所以 `level` 的来源不是某个 header cache，而是 bridge 维护的 upper chain 映射。

### 4. `store_.get_itempointer(...)`

```cpp
store_.get_itempointer(...)
```

通过当前元素状态修正：

- `header.deleted`

这一步是 `LoadHeader()` 里少数真的去底层 store 看“当前状态”的地方。

### 5. `load_node_header_cb`

如果配置了：

```cpp
cfg_.load_node_header_cb(node_id, header)
```

就允许外部回调继续覆盖 header。

所以 `header` 的本质是：

- **默认值 + bridge state + store state + callback** 共同组装出来的逻辑视图

而不是某个单独的“物理 header 块”。

## `vector` Data Source

入口：

- [core_node_store_bridge_live.hpp:160](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:160)

调用链：

1. `store_.pin_vector_buffer(ToStoreId(node_id))`
2. `DiskStore::pin_vector_buffer(...)`
3. `vec_read_buffer(...)`
4. `VecBufMgr->get_buffer(...)`

具体位置：

- [graph_index_storage.h:1087](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/graph_index_storage.h:1087)
- [vector_smgr.cpp:796](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/vector_smgr.cpp:796)

所以 `vector` 的来源是：

- **vector buffer manager 缓存**
- miss 时才会底层读取

这条路径和 `header` 完全不同：

- `vector` 是底层 buffer cache 语义
- `header` 是 bridge 现组装语义

## `level0 neighbors` Data Source

入口：

- [core_node_store_bridge_live.hpp:336](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:336)

核心逻辑：

```cpp
store_.base_layer.get_n<ReadLock>(...)
pin_state.header.level0_count = PgBridgeCountValidNeighbors(...)
pin_state.level0_neighbors[...] = ...
```

这说明：

- level0 邻接表不是从 `header` 里出来的
- 也不走单独 cache map
- 而是每次 `PinNode` 时从底层 `base_layer` 读取一条记录
- 再由 bridge 复制到 `pin_state.level0_neighbors`

所以：

- `level0_count` 最终值是 `LoadLevel0Neighbors(...)` 覆盖出来的
- 不是 `LoadHeader()` 决定的

## `upper neighbors` Data Source

入口：

- [core_node_store_bridge_live.hpp:353](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:353)

过程分两段：

### 1. 先查 `upper_chain_by_node_`

```cpp
auto chain_it = upper_chain_by_node_.find(node_id);
```

这决定了：

- 这个节点有几层 upper levels
- 每一层对应哪个 upper storage slot

### 2. 再从 `upper_layer` 逐层读

```cpp
store_.upper_layer.get_n<ReadLock>(...)
```

然后：

- 统计该层有效邻居数
- 拷贝邻居到 `pin_state.upper_neighbors`
- 计数写入 `pin_state.upper_counts`

所以：

- `upper neighbors` 既依赖 bridge 维护的 upper-chain 映射
- 又依赖底层 `upper_layer` 数据读取

## `metadata` Data Source

入口：

- [core_node_store_bridge_live.hpp:429](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:429)

逻辑：

1. 先分配/扩容 `metadata`
2. 先清零
3. 如果有 `load_node_metadata_cb`，走回调现读

所以 `metadata` 走的是：

- **bridge 临时缓冲 + optional callback**
- 不是 vector buffer cache

## Summary Table

| Data block | Main entry | Primary source | Cache layer | Notes |
|---|---|---|---|---|
| `header` | `LoadHeader` | 默认初始化 + `node_to_row_id_` + `upper_chain_by_node_` + `store_.get_itempointer` + optional callback | 无独立 header cache | 逻辑组装，不是直接页读 |
| `vector` | `pin_vector_buffer` | `VecBufMgr->get_buffer` | `VecBufferManager` | 命中时直接复用 buffer |
| `level0 neighbors` | `LoadLevel0Neighbors` | `store_.base_layer.get_n` | none at bridge layer | 每次现取现拷贝 |
| `upper neighbors` | `LoadUpperNeighbors` | `upper_chain_by_node_` + `store_.upper_layer.get_n` | none at bridge layer | 每层现取现拷贝 |
| `metadata` | `LoadMetadata` | 临时缓冲 + optional callback | 无独立 metadata cache | 现分配现加载 |

## Key Takeaway

一次 `PinNode` 返回给算法层的数据并不是来自同一个物理来源：

- `vector` 走底层 buffer cache
- `header` / `metadata` 都是 bridge 现组装
- `neighbors` 走底层层级结构现取现拷贝

所以当前 `PinNode` 更接近“**多源拼装节点视图**”，而不是“统一节点块 pin”。

这也是为什么：

- `pin_vector_ms`
- `pin_header_ms`
- `pin_level0_ms`
- `pin_upper_ms`

会被分开统计，因为它们本来就是不同来源、不同语义的访问成本。
