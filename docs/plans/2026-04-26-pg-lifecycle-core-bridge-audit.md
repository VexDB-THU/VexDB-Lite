# PG `graph_index` Lifecycle Core-Bridge Audit

日期: 2026-04-26
分支: `vexdb-unify`

## 目标

梳理 PostgreSQL 插件 `graph_index` 全生命周期中：

1. 哪些步骤已经实质复用 `libvex-core`
2. 哪些步骤还可以继续向 `libvex-core` 收敛
3. 在 `PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE=ON` 为默认前提下，哪些旧代码已经被屏蔽或弱化，可以进入移除候选列表

## 生命周期总表

| 生命周期阶段 | 当前主入口 | 当前是否走 core | 当前状态判断 |
|---|---|---|---|
| Build (disk-like) | `graph_index_build.cpp` + `try_build_single_thread_core()` | 是，部分 | 已走 core HNSW build，但仅限 `U32 + FLOAT + no quantizer + supported metric` |
| Build (true memory) | `graph_index_build.cpp` + `mem_store -> flush()` | 否 | 现在已恢复成真正 memory build，但算法仍走 PG legacy `GraphIndexAlgorithm` + `MemStore` |
| Insert | `graph_index_insert.cpp` | 是，部分 | 支持配置下会先尝试 `TryInsertViaCoreBridge()` |
| Scan | `graph_index_scan.cpp` | 是，部分 | 支持配置下会先尝试 `TrySearchViaCoreBridge()` |
| Vacuum bulkdelete | `graph_index_vacuum.cpp` | 否，主体未走 | 只在 entry-point 刷新阶段借了 core bridge helper |
| Vacuum cleanup | `graph_index_vacuum.cpp` | 否 | 纯 PG store 路径 |
| Inspect | `graph_index_inspect.cpp` | 是，极少量 | 只复用 graph-state meta 页转换 helper |
| XLOG/meta persistence | `graph_index_xlog.h/.cpp`, `core_node_store_bridge.cpp` | 否 | 仍是 PG 适配层职责 |
| Buildempty | `graph_index_build.cpp` | 否 | 仍是 PG metapage / disk-structure 初始化 |

## 已经走 core 的步骤

### 1. Disk-like build 已走 core HNSW

文件:
- [graph_index_build.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_build.cpp)
- [core_node_store_bridge_runtime.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_runtime.hpp)

当前行为:
- `try_build_single_thread_core()` 进入 `CoreBridgeBuildRuntime`
- `CreateGraphRuntime(...)`
- `AddPointToGraph(...)`
- `PersistGraphRuntime(...)`

说明:
- 算法本体已经是 `libvex-core`
- PG 适配层主要负责：
  - graph state 落 meta page
  - node/tid 映射
  - live binding

结论:
- 这是当前 PG 生命周期里 core 化最深的一段

### 2. Insert 已经有 core bridge fast path

文件:
- [graph_index_insert.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_insert.cpp)
- [core_node_store_bridge_runtime.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_runtime.hpp)

当前行为:
- `graph_index_insert_internal()` 里先尝试 `TryInsertViaCoreBridge()`
- 命中条件:
  - `!use_async`
  - `U32`
  - `FLOAT`
  - `QuantizerType::NONE`

说明:
- 满足条件时，单条插入已经走 core HNSW runtime
- 不满足时仍回退到 legacy `GraphIndexAlgorithm`

### 3. Scan 已经有 core bridge read-only path

文件:
- [graph_index_scan.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_scan.cpp)
- [core_node_store_bridge_runtime.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_runtime.hpp)

当前行为:
- `graph_index_gettuple_internal()` 里先尝试 `TrySearchViaCoreBridge()`
- 命中条件:
  - `U32`
  - `FLOAT`
  - `QuantizerType::NONE`

说明:
- 搜索算法已经是 core HNSW 搜索
- 结果展开回 `ItemPointerData` 仍在 PG 适配层完成

### 4. Vacuum 后的 entry-point 刷新已借助 core helper

文件:
- [graph_index_vacuum.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_vacuum.cpp)
- [core_node_store_bridge_runtime.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_runtime.hpp)

当前行为:
- bulkdelete 后调用 `RefreshEntryStateAfterVacuum()`
- 通过 live binding + graph state helper 重算并回写 entry point

说明:
- 这里只是局部借助 core bridge
- bulkdelete 主体仍不是 core 化删除维护

## 还可以继续 core 化的步骤

### A. True memory build

现状:
- 现在已经恢复成“真正 memory build 在 `mem_store` 中完成，再 flush 到磁盘”
- 但算法本体仍是 PG 本地 `GraphIndexAlgorithm + MemStore`

文件:
- [graph_index_build.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_build.cpp)
- [graph_index_storage.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/graph_index_storage.h)

建议:
- 这是接下来最值得继续 core 化的一段
- 目标应是 build-only `libvex-core::MemoryNodeStore` + PG flush adapter
- 这样才能让 true memory build 的算法侧也完全摆脱 PG legacy node access

### B. Insert legacy fallback

现状:
- 当前 insert 在 supported config 下已走 core
- 但 async insert / half / quantized / 其他不满足条件配置仍走 legacy

建议:
- 可继续扩大 `TryInsertViaCoreBridge()` 支持矩阵
- 优先级次于 true memory build

### C. Scan legacy fallback

现状:
- 当前 scan 在 supported config 下已走 core read-only path
- quantized / half / clustered 等仍走 legacy search

建议:
- 可继续扩大 core scan 覆盖范围
- 对 quantized 路径尤其关键

### D. Vacuum graph maintenance

现状:
- 目前只在 entry point 刷新阶段用了 core helper
- bulkdelete / deleted-node cleanup / graph maintenance 仍是 PG store 逻辑

建议:
- 这里理论上可以继续向 `libvex-core` 的 graph maintenance 能力收敛
- 但要先确认 PG 删除、可见性、TID 生命周期语义如何映射

## 在 `PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE=ON` 下可进入移除候选的内容

### 1. `graph_index_build.cpp` 中的 `#ifdef PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE`

结论:
- 宏本身已经没有长期保留价值
- 方向上可以逐步去掉编译开关，直接保留 ON 分支

但注意:
- 不能连同 runtime fallback 一起删
- 因为当前还有：
  - true memory build legacy path
  - unsupported config legacy path

可移除的是:
- 预处理宏分叉

暂时不能移除的是:
- runtime fallback 行为本身

### 2. `PgCoreLowLevelBindingSkeleton` / `CreatePgCoreNodeStoreSkeleton`

文件:
- [core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/core_node_store_bridge.cpp)
- [core_node_store_bridge.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge.h)

结论:
- 生产主链当前没有走这个 skeleton fallback
- 更像测试/占位脚手架

建议:
- 这是一个明显的清理候选
- 但要先确认没有测试或工具代码依赖

### 3. Core bridge supported-config 下的 legacy search/insert branches

结论:
- 对于 `U32 + FLOAT + QuantizerType::NONE` 这条主路径，legacy 分支现在已经基本只是在兜底
- 但因为 runtime 仍允许 fallback，所以现在不能直接删

建议:
- 等支持矩阵进一步扩大后，再按配置维度删除 legacy 分支
- 现在只能标记为“后续候选”，不能立即删

## 现在还不能删的旧代码

### 1. `graph_index_build.cpp` 的 legacy memory-build 路径

原因:
- 现在 true memory build 仍完全依赖它
- 如果删掉，`maintenance_work_mem >= 1GB` 时就没有真正 memory build 了

### 2. `graph_index_insert.cpp` 的 legacy insert 路径

原因:
- async insert / 非 `U32 + FLOAT + NONE` 组合仍靠它

### 3. `graph_index_scan.cpp` 的 legacy search 路径

原因:
- quantized / half / clustered 等路径仍靠它

### 4. `graph_index_vacuum.cpp` 主体

原因:
- 当前 bulkdelete / cleanup 并没有完整 core 版本替代

### 5. `graph_index_inspect.cpp` 中的 `DiskStore::inspect()`

原因:
- 当前 inspect 输出仍依赖 PG 磁盘结构统计
- core 还没有等价的 inspect adapter 输出

## 优先级建议

### 第一优先级
- 把 true memory build 的算法侧也迁到 `libvex-core`
- 也就是从 `PG legacy GraphIndexAlgorithm + MemStore` 进一步收敛到 `core MemoryNodeStore + flush adapter`

### 第二优先级
- 扩大 insert / scan 的 core bridge 覆盖矩阵
- 尽量减少 supported config 下落回 legacy 的概率

### 第三优先级
- 移除编译期开关分叉
- 保留 runtime fallback，先去掉 `#ifdef PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE` 这种编译层噪音

### 第四优先级
- 评估并清理 `PgCoreLowLevelBindingSkeleton` 等脚手架代码

## 当前判断

最值得继续推进的不是 scan / insert 小修小补，而是:

- **true memory build 的算法层彻底 core 化**

因为现在生命周期里最大的结构性不一致就是：

- disk-like build 已 core 化
- scan 已部分 core 化
- insert 已部分 core 化
- 但 true memory build 仍停留在 PG legacy path

这正是下一阶段最该收敛的点。
