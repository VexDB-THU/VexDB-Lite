# True Memory Build Coreization Plan

日期: 2026-04-26
分支: `vexdb-unify`

## 目标

把 PostgreSQL 插件当前的 true memory build 从：

- `PG legacy GraphIndexAlgorithm + MemStore`

收敛到：

- `libvex-core::MemoryNodeStore + PG flush adapter`

要求：
- 不改 HNSW 算法设计
- 构建阶段真正走 core memory node access
- 最终仍落到现有 PG 磁盘结构
- 现有 scan / insert / vacuum 行为先不动

## 当前状态

当前 true memory build 已经恢复成：
- 在 `mem_store` 中完成构建
- 完成后 `flush(index)` 落盘

问题是：
- 算法侧仍是 PG 本地 `GraphIndexAlgorithm + MemStore`
- 与 disk-like build / scan / insert 已经部分 core 化的方向不一致

## 目标架构

### 构建阶段

使用：
- `vex::MemoryNodeStore`
- `vex::HNSWGraph`
- `vex::AddPointToGraph(...)`
- `vex::PersistGraphRuntime(...)` 仅在需要 graph state 抽取时使用 helper，不直接落 PG meta

说明：
- build 过程中不再经过 PG live bridge
- 不再依赖 PG `MemStore` 的 neighbor / vector 访问接口
- 所有高频 `PinNode` 都落在 core memory store 上

### PG 适配阶段

新增一个 build-only flush adapter，负责把 core memory store 导出到当前 PG 磁盘结构：
- elems
- base layer
- upper layer
- vector / quant code
- metapage entrypoint / num_vectors / graph state

## 数据面映射

### Core `MemoryNodeStore`

现有数据面：
- `headers_`
- `vectors_`
- `neighbors_l0_`
- `upper_neighbors_`
- `metadata_`
- `alive_`

关键接口：
- `AllocateNode(...)`
- `PinNode(...)`
- `PinNodeForUpdate(...)`
- `ForEachNode(...)`

### PG flush 所需数据面

当前 `flush_graph()` 需要：
- `GraphIndexEntryInfo`
- `num_vectors`
- `GraphIndexPoint` elems
- base neighbors
- upper neighbors + lower_layer_idx + id
- vector or quant code
- metapage mutable fields

## 核心设计决策

### 1. Row/TID 保存方式

建议：
- build 期间单独维护 `std::vector<ItemPointerData> heap_tids_by_node_id`
- node_id 作为数组下标
- 不把 `ItemPointerData` 塞进 `MemoryNodeStore::metadata_`

原因：
- 当前 build 阶段只需要 flush 时把 node_id -> tid 写进 `GraphIndexPoint`
- 不需要在算法层频繁读取 heap tid
- 单独数组最简单，避免把 PG payload 强耦合进 core memory store

### 2. Entry state 保存方式

建议：
- 构建结束后从 `HNSWGraph` 抽取 graph state
- 转成 `GraphIndexEntryInfo` / metapage 字段

不要：
- 在 build 阶段维持 PG `GraphIndexEntryInfo` 作为主状态源

### 3. Flush 方向

建议新增：
- `flush_graph_from_core_memory(...)`

而不是强改现有 `flush_graph()` 直接吃 `MemoryNodeStore`

原因：
- 现有 `flush_graph()` 明显是围绕 PG `MemStore` 布局写的
- 直接硬塞 core store 会让函数变得更乱
- 应该先并存两套 flush adapter，再逐步收旧

## 文件级改动建议

### A. `graph_index_build.cpp`

目标：
- 引入新的 true memory build core path

建议改动：
1. 新增 build state runtime 持有：
   - `std::unique_ptr<vex::MemoryNodeStore>`
   - `std::unique_ptr<vex::HNSWGraph>`
   - `std::vector<ItemPointerData> heap_tids_by_node_id`
2. `build_state == MEMORY` 时，不再走 PG `MemStore` 算法路径
3. 新增 memory build callback：
   - 调 `vex::AddPointToGraph(...)`
   - 记录 `heap_tid`
4. 构建结束后调用新的 core flush adapter

### B. 新增一个 PG memory-build core flush adapter 文件

建议新增文件，例如：
- `vexdb-pg/include/graph_index/core_memory_build_flush.hpp`
- 或 `vexdb-pg/src/core_memory_build_flush.cpp`

职责：
- 遍历 `MemoryNodeStore`
- 导出 PG 落盘格式
- 回写 metapage

### C. `libvex-core`

尽量少改。

首选：
- 不改 `MemoryNodeStore` 接口
- 直接通过 `ForEachNode + PinNode` 读取 flush 所需字段

只有在发现缺口时再补：
- `GetTotalSlots()` 之类只读 introspection helper

### D. `graph_index_storage.h`

当前不建议大改。

原因：
- true memory build core 化的目标是减少对 PG `MemStore` 的依赖
- 不是继续扩展 `MemStore`

## 实施批次

### 批次 1

目标：
- 在 `build_state == MEMORY` 下创建并使用 `vex::MemoryNodeStore + HNSWGraph`
- 先完成内存构建，不做 flush
- 暂时只验证内存构建阶段行为和 graph state 提取

### 批次 2

目标：
- 新增 `flush_graph_from_core_memory(...)`
- 打通完整 `CREATE INDEX`
- 对照当前 `mem_store -> flush` 输出结果

### 批次 3

目标：
- benchmark 对比：
  - legacy memory build
  - core memory build
  - disk-like core build

### 批次 4

目标：
- 在验证稳定后，移除旧的 PG legacy memory-build 算法路径
- 保留 flush adapter 和 unsupported-config fallback

## 风险点

### 1. node_id / row_id / heap tid 三者关系

必须确保：
- core graph 的 node_id
- flush 时的 PG vector loc
- elems 中保存的 heap tid

三者一一对应，不允许错位

### 2. upper layer 导出

需要确认：
- core `header.level`
- `UpperCounts()`
- 每层 `UpperNeighbors(level_idx)`

与 PG `GraphIndexDiskUpperPoint` 的：
- `lower_layer_idx`
- `id`
- `neighbors_info`

映射一致

### 3. graph state persistence

必须确保：
- entrypoint
- entry_cur_layer_idx
- entry_level
- num_vectors

和 scan 路径当前读取期望完全一致

## 成功标准

1. `maintenance_work_mem >= 1GB` 时：
- 明确走 core true memory build
2. `10k` benchmark：
- build 成功
- 不崩溃
- explain 仍走 `Index Scan`
3. 结果质量：
- recall 不低于当前 true memory build 已有水平
4. 代码结构：
- memory build 算法侧不再依赖 PG `MemStore`

## 当前建议

下一步直接进入：
- 批次 1

也就是先把 `build_state == MEMORY` 的构建态 runtime 切到：
- `vex::MemoryNodeStore + vex::HNSWGraph`

先不急着删旧 PG memory-build 路径，先并存，把 flush adapter 打通后再收旧。
