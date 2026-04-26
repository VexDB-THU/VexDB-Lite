# VexDB-Lite Core Migration File Inventory

日期: 2026-04-24

目的:
- 留档当前多后端架构收敛过程中，哪些文件应保留在适配层，哪些应继续迁入 `libvex-core`
- 明确 DuckDB / PostgreSQL 两侧当前仍然存在的大块重复实现
- 给出后续分批迁移顺序、删除候选、风险点和验证点

边界:
- 不调整 `graph_index_algorithm` 设计
- 不引入全量索引序列化/反序列化搬运
- `MemoryNodeStore` 继续允许后端适配层绑定到底层存储接口

## 1. 目标目录职责

### 1.1 `libvex-core`

职责:
- 通用 HNSW 算法主路径
- 通用 PQ / RabitQ 算法本体
- 通用 bridge runtime
- 通用 distancer / query preprocess / result expansion skeleton

不放:
- DuckDB / PostgreSQL 的索引对象
- planner / optimizer / executor 接入
- 后端磁盘布局、meta page、buffer pin/unpin、锁、缓存

### 1.2 `vexdb-duck`

职责:
- DuckDB 索引对象、DDL 参数解析、序列化/反序列化
- DuckDB optimizer / physical operator 接入
- DuckDB allocator / block manager / storage binding
- DuckDB 结果展开、metadata 提取、Dedup 映射

### 1.3 `vexdb-pg`

职责:
- PostgreSQL index AM / build / scan / insert / vacuum / xlog
- PG meta page、disk store、cluster/page layout
- PG quantizer 元信息、后台任务、缓存
- PG 结果展开到 `ItemPointer/TID`

### 1.4 进入 `libvex-core` 的优化准入规则

可以进入 `libvex-core` 的优化:
- 后端无关的算法本体
- 同时对 DuckDB / PostgreSQL 有潜在收益的性能内核
- 不依赖 `Relation / ItemPointer / Buffer / Block / metapage / allocator / WAL`
- 可由统一的 `NodeStore / Quantizer / Distancer / runtime` 接口承接

优先放进 `libvex-core` 的优化白名单:
- PQ / RabitQ 的 SIMD kernel
  - single-code distance
  - 4 路 / 8 路 batch code distance
  - query distance table 查表累计
- 通用向量距离 SIMD
  - `L2 / IP / Cosine`
  - query preprocess / normalize
- HNSW 通用搜索骨架优化
  - upper-layer / level-0 skeleton
  - candidate heap / visited set
  - rerank / result-limit / expansion skeleton
- cache-friendly 通用内核
  - prefetch helper
  - 对齐访问
  - 紧凑查表布局
- 通用量化训练内核
  - PQ k-means
  - RabitQ estimator / rotator / preprocess
- ISA dispatch
  - AVX2 / AVX512 / NEON / SVE
  - 保留标量 fallback

不应进入 `libvex-core` 的黑名单:
- Duck allocator / row map / dedup_map 持有逻辑
- PG `ItemPointer/TID` 展开
- PG quantizer metadata / `qtcode_block` / cache 生命周期
- page layout / buffer pin-unpin / WAL / metapage 更新
- planner / optimizer / executor / index AM 接线

执行原则:
- 先把“算法核”和“后端胶水”拆开，再迁入 core
- 不为了复用而把后端生命周期代码硬塞进 core
- core 内允许保留多 ISA 实现，但调用面应统一

PQ SIMD 特别说明:
- PQ 的 SIMD 优化路径原则上应进入 `libvex-core`
- 但更适合放在 quant distancer 相关文件，而不是直接塞进 `pq.cpp`
- `product_quantizer.cpp` 继续负责:
  - 训练
  - 编码/解码
  - 距离表构建
  - 序列化
- SIMD 查表累计 / batch code distance 更适合进入:
  - [vex_quant_distancer.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_quant_distancer.hpp)
  - [quant_distancer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/quant_distancer.cpp)
  或后续新增的 `quant_distancer_simd.*`

原因:
- 训练/编码逻辑和 SIMD 查表逻辑应解耦
- Duck 当前已经直接复用 core 的 PQ distancer 路径
- 因此 PQ SIMD 一旦进入 core，Duck 和 PG 都能直接受益

## 2. 必须保留在 `vexdb-duck` 的文件

这些文件本来就是 DuckDB 适配层职责，不应迁到 `libvex-core`。

### 2.1 Duck 索引对象和生命周期

- [graph_index.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index.cpp)
- [vex_graph_index.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_graph_index.hpp)

### 2.2 Duck optimizer / executor 接入

- [vex_optimizer.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/optimizer/vex_optimizer.cpp)
- [vex_physical_index_scan.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/optimizer/vex_physical_index_scan.cpp)
- [vex_physical_create_index.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/optimizer/vex_physical_create_index.cpp)

### 2.3 Duck 存储 binding 和节点布局

- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp)
- [vex_core_node_store_bridge.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_core_node_store_bridge.hpp)
- [vex_hnsw_node.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_hnsw_node.hpp)

### 2.4 Duck 过滤条件 / graph state 翻译辅助

- [vex_filter_predicate.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_filter_predicate.hpp)
- [vex_core_bridge_graph_helpers.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_bridge_graph_helpers.cpp)

## 3. 必须保留在 `vexdb-pg` 的文件

这些文件属于 PG 专属对象模型和存储实现。

### 3.1 PG index AM 和执行入口

- [graph_index_am.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_am.cpp)
- [graph_index_build.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_build.cpp)
- [graph_index_insert.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_insert.cpp)
- [graph_index_scan.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_scan.cpp)
- [graph_index_vacuum.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_vacuum.cpp)
- [graph_index_xlog.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_xlog.cpp)

### 3.2 PG 存储格式和 direct binding

- [graph_index_storage.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/graph_index_storage.h)
- [graph_index_struct.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/graph_index_struct.h)
- [core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/core_node_store_bridge.cpp)
- [core_node_store_bridge.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge.h)

### 3.3 PG quantizer 元信息和后台更新

- [quantizer.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/quantizer.h)
- [quantizer_stubs.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/quantizer_stubs.cpp)
- [guc_config.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/guc_config.cpp)

## 4. 应继续迁到 `libvex-core` 的文件

这些文件仍然携带大量通用算法逻辑，是当前最主要的收敛目标。

### 4.1 Duck 原生 HNSW 主算法

- [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp)
- [vex_graph_index_core.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_graph_index_core.hpp)

内容包括:
- `SearchLayer`
- `SelectNeighbors`
- `InsertNode`
- 并发插入
- brute-force search
- filtered search
- PQ train / encode / search
- dedup

结论:
- 这部分不应长期留在 `vexdb-duck`
- 最终应由 `libvex-core` 承接算法本体，Duck 只保留存储 binding

### 4.2 Duck 本地 PQ 实现

- 已删除的原 Duck 本地 `product_quantizer.cpp`，算法实现已并入 [product_quantizer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/product_quantizer.cpp)
- [vex_quantizer.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_quantizer.hpp)

结论:
- 应与 `libvex-core` 的 `ProductQuantizer` 合并
- 适配层只保留调用和持久化格式处理

### 4.3 PG 的 RabitQ 通用算法部分

- [rabitq.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/rabitq.h)
- [utils.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/utils.h)
- [query.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/query.h)
- [estimator.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/estimator.h)
- [rotator.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/rotator.h)

结论:
- 通用量化数学逻辑应迁入 `libvex-core`
- PG 适配层保留 metadata、cache、distancer 接口

## 5. 迁完后应变薄的文件

这些文件未必删除，但应该明显瘦身，只保留适配层胶水。

### 5.1 Duck bridge runtime

- [vex_core_bridge_runtime.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_bridge_runtime.cpp)

理想职责:
- eligibility 判断
- Duck binding/config 组装
- metadata 回写
- Dedup 展开

### 5.2 PG bridge runtime

- [core_node_store_bridge_runtime.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_runtime.hpp)

理想职责:
- eligibility 判断
- PG binding/config 组装
- `row_id -> ItemPointer/TID` 展开
- vacuum / entry-state 适配

2026-04-24 PG 现状补充:
- PG bridge runtime 已经接入 `build / insert / scan / vacuum entry-state refresh`
- 但当前 bridge 覆盖范围仍然很窄，主要限制包括:
  - `IdType::U32`
  - `DistPrecisionType::FLOAT`
  - `QuantizerType::NONE`
  - `insert` 还要求 `!use_async`
  - `clustered store` 基本不走 bridge
- 这意味着 PG 侧现阶段还不能作为“算法都已经通用化”的证据
- 后续继续从 Duck 抽骨架前，应先以 PG 现状校准“什么才是真通用算法边界”

### 5.3 PG `GraphIndexAlgorithm` 的拆分边界

- [graph_index_algorithm.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/graph_index_algorithm.h)

当前问题:
- 该文件同时混合了:
  - HNSW 搜索/插入骨架
  - `ItemPointer/TID` 结果展开
  - `MemStore/DiskStore` 差异
  - async pending 数据合并
  - clustered store 分支
  - dist cache / cluster maintain traits
- 因此不能整块迁到 `libvex-core`

后续拆分原则:
- 可迁到 `libvex-core` 的部分:
  - 后端无关的 HNSW 搜索骨架
  - PQ / RabitQ 数学逻辑
  - query preprocess / rerank / result-limit skeleton
- 必须留在 `vexdb-pg` 的部分:
  - `ItemPointer/TID` 展开
  - async merge
  - clustered / disk-store / mem-store 调度

## 6. PG 侧未迁移算法清单

这一节只盘点“仍留在 `vexdb-pg`，但本质上属于通用算法、理论上还应继续迁入 `libvex-core`”的部分。

### 6.1 第一优先级：RabitQ 通用数学实现

文件:
- [rabitq.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/rabitq.h)
- [utils.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/utils.h)
- [query.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/query.h)
- [estimator.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/estimator.h)
- [rotator.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/rotator.h)

判断:
- 这批文件主要承载 RabitQ 的量化数学、query preprocess、旋转和估计逻辑
- 与 DuckDB / PostgreSQL 的存储接口没有天然绑定
- 是当前 PG 侧最明确、最值得继续迁入 core 的算法块

迁移原则:
- 先迁纯数学逻辑
- PG 侧保留:
  - [rabitq_cache.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/rabitq_cache.h)
  - [rabitq_distancer.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/rabitq_distancer.h)
  - [rabitq_distancer.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/rabitq_distancer.cpp)
  - PG metadata / cache / load / flush 胶水

### 6.2 第一优先级：PQ 残留算法实现

文件:
- [pq.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/pq.cpp)
- [pq.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/pq.h)
- [horizontal_sum128.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/distance/pq/horizontal_sum128.h)
- [horizontal_sum256.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/distance/pq/horizontal_sum256.h)
- [horizontal_sum512.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/distance/pq/horizontal_sum512.h)
- [pq_endecode.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/distance/pq/pq_endecode.h)
- [transpose_avx2_inl.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/distance/pq/transpose_avx2_inl.h)
- [transpose_avx512_inl.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/distance/pq/transpose_avx512_inl.h)

判断:
- `libvex-core` 已经有:
  - [product_quantizer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/product_quantizer.cpp)
  - [quant_distancer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/quant_distancer.cpp)
- 但 PG 侧仍残留一套 PQ 训练、编码、距离表和 distancer 适配
- 说明 PQ 收敛尚未完成，PG 适配层仍偏厚

迁移原则:
- core 继续承接:
  - 训练
  - 编码/解码
  - 距离表构建
  - SIMD 查表累计
- PG 保留:
  - quantizer 元信息
  - `qtcode_block` 持久化
  - 构建/查询时的加载和 flush 胶水

### 6.3 第一优先级：距离与 SIMD 内核

文件:
- [distance.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/distance/distance.cpp)
- [general.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/distance/general.cpp)
- [sse.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/distance/sse.cpp)
- [avx.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/distance/avx.cpp)
- [avx512.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/distance/avx512.cpp)
- [distance_template.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/distance/distance_template.h)
- [distance_template2.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/distance/distance_template2.h)
- [transform_template.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/distance/transform_template.h)
- [distances_simd_template.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/distance/distances_simd_template.cpp)
- [code_distance_template.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/distance/code_distance_template.cpp)

判断:
- `libvex-core` 已有基础距离实现，但还没有完全覆盖 PG 侧这套 SIMD/模板内核
- 这批内核本身不依赖 PG page / Relation / TID 语义
- 应逐步收敛到 core，形成统一距离核和 ISA dispatch

迁移原则:
- core 承接:
  - 标量实现
  - SSE / AVX2 / AVX512 实现
  - half / int8 / PQ / RabitQ 的通用距离核
  - 统一 dispatch 接口
- PG 保留:
  - `distance_guc` 和运行时架构选择胶水
  - 兼容 PG 编译环境的宏和封装

### 6.4 第二优先级：训练辅助算法

文件:
- [annkmeans.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/annkmeans.h)
- [ann_utils.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/ann_utils.h)
- [ann_utils.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/ann_utils.cpp)

判断:
- 这批代码中如果包含纯采样、归一化、聚类、训练工具，应进入 core
- 但若混有 `Relation` / heap scan / PG row 访问，则不能整块迁移

迁移原则:
- 拆出纯训练数学工具进入 core
- heap sampling、Relation 访问留在 PG

### 6.5 第二优先级：`GraphIndexAlgorithm` 中仍可抽离的通用骨架

文件:
- [graph_index_algorithm.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/graph_index_algorithm.h)

判断:
- 文件整体不能迁
- 但里面仍可能残留:
  - 通用搜索骨架
  - 候选集/visited/剪枝骨架
  - 与 `NodeStore` 接口兼容的后端无关层

迁移原则:
- 只继续抽“真正后端无关”的部分
- 不把以下内容带入 core:
  - `ItemPointer/TID` 展开
  - async merge
  - clustered / disk-store / mem-store 调度

## 7. PG 侧暂不迁移的算法相关文件

这些文件虽然与构建/搜索相关，但本质仍属于 PG 适配层，不应直接迁到 core。

文件:
- [graph_index_build.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_build.cpp)
- [graph_index_insert.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_insert.cpp)
- [graph_index_scan.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_scan.cpp)
- [graph_index_am.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_am.cpp)
- [graph_index_vacuum.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_vacuum.cpp)
- [graph_index_xlog.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_xlog.cpp)
- [graph_index_storage.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/graph_index_storage.h)
- [vector_smgr.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/vector_smgr.cpp)
- [vector_buffer_manager.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/vector_buffer_manager.h)
- [quantizer.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/quantizer.h)
- [quantizer_stubs.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/quantizer_stubs.cpp)

原因:
- 含 PG page / WAL / metapage / Relation / TID / cache 生命周期
- 属于适配层对象模型和持久化实现
- 后续可以瘦身，但不应整体迁入 core

## 8. 后续批次建议顺序

建议顺序:
1. 先收 `rabitq/*` 的纯数学和 query preprocess
2. 再收 `pq.cpp/pq.h` 中残留的通用 PQ 算法核
3. 再收 `distance/*` 的通用 SIMD 距离内核
4. 再拆 `ann_utils/annkmeans` 中纯训练工具
5. 最后再继续从 `graph_index_algorithm.h` 抽纯 HNSW 骨架

原因:
- 前三批更纯、更容易验证
- 风险小于直接硬拆 PG 的 build / scan / storage 混合层
- 更符合“适配层只做调用，通用算法尽量进 core”的路线
  - relation / metapage / buffer / xlog 生命周期
  - quantizer metadata、后台任务、cache 生命周期

结论:
- PG 下一步不应直接重写 `AM / build / scan / insert` 主流程
- 应先把 `graph_index_algorithm.h` 拆成“后端胶水”和“可迁算法核”两个清单
- 之后再继续抽 Duck / PG 共有算法到 `libvex-core`

2026-04-24 函数级拆分清单:

优先视为“可迁算法核候选”的函数:
- `search_upper_layer`
- `search_layer`
- `refine`
- `select_neighbors`
- `get_distance(const Cand &, const Cand &)`
- `get_distance(const PruneNeighbor &, const PruneNeighbor &)`
- `replace_lower_layer_idx`
- `get_neighbors_data`
- `update_reverse_edges`

原因:
- 这些函数主要描述 HNSW / rerank / pruning / graph edge update 的通用骨架
- 和 PG `Relation` / `ItemPointer` / metapage 生命周期没有直接绑定
- 未来更适合作为 Duck / PG 共同复用的 `libvex-core` helper 或算法入口

明确留在 PG 适配层的函数:
- `search`
- `search_async_heap`
- `search_with_async`
- `async_insert`
- `insert`
- `apply_arrangement`
- `insert_range_tid`
- `repair_entry`
- `repair_graph_parallel`
- `repair_graph_remaining`
- `repair_base_range`
- `repair_upper_range`
- `repair_basepoint`
- `repair_upperpoint`
- `get_repair_info`

原因:
- 这些函数直接绑定:
  - `ItemPointer/TID` 展开
  - async pending 数据合并
  - clustered store 路径
  - vacuum / repair 生命周期
  - PG `PointExtensionContext`
  - PG store/page/layout 细节

短期只做边界校准、暂不迁出的函数:
- `insert_new_point`
- `add_upperpoint`
- `add_basepoint`
- `add_first_basepoint`
- `add_first_upperpoint`
- `check_insertable`
- `get_insert_level`

判断:
- 这些函数带有较强 HNSW 算法意味
- 但当前仍直接依赖 PG store 的写入语义、点布局、ID 分配和 layer 结构
- 需要等 PG / Duck 两边 binding 抽象再稳定一轮后，再决定是否继续上收

2026-04-24 未来 core 落点对照:

HNSW 搜索骨架:
- PG 当前来源:
  [graph_index_algorithm.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/graph_index_algorithm.h)
  中
  `search_upper_layer`
  `search_layer`
  `select_neighbors`
  `refine`
- 未来主要落点:
  [vex_graph_algo.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_graph_algo.hpp)
  [graph_algo.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/graph_algo.cpp)
  [vex_adapter_graph_runtime.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_graph_runtime.hpp)

PQ 通用量化逻辑:
- PG 当前来源:
  [pq.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/pq.h)
  [pq.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/pq.cpp)
- 未来主要落点:
  [vex_quantizer.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_quantizer.hpp)
  [product_quantizer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/product_quantizer.cpp)
  [vex_quant_distancer.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_quant_distancer.hpp)
  [quant_distancer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/quant_distancer.cpp)

RabitQ 通用量化逻辑:
- PG 当前来源:
  [rabitq.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/rabitq.h)
  [utils.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/utils.h)
  [query.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/query.h)
  [estimator.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/estimator.h)
  [rotator.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/rotator.h)
- 未来主要落点:
  `libvex-core` 新增 RabitQ 头/实现
  然后由 PG `rabitq_distancer` 只保留 metadata / cache / prepare/process 接口

结果展开 skeleton:
- PG 当前来源:
  `search` / `search_with_async` 中
  `row_id -> ItemPointer/TID` 展开和距离复制
- 现有可复用 core helper:
  [vex_adapter_graph_runtime.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_graph_runtime.hpp)
  的
  `ExpandSearchResults`
- 结论:
  result-limit / expansion skeleton 可以继续复用 core，
  但 `ItemPointer/TID` 具体展开逻辑必须留在 PG

2026-04-24 当前阻塞项:
- PG `search_layer` / `select_neighbors` 仍直接依赖 `Store` 的:
  - `lock_point`
  - `get_point_info`
  - `get_distance_batch`
  - `get_neighbors`
  - `get_neighbor_stats`
- PG `refine` 仍依赖:
  - `fetch_vec_from_heap`
  - `PointExtensionContext`
- PG `insert_new_point` / `add_basepoint` / `add_upperpoint`
  仍依赖:
  - PG store 写入语义
  - ID 分配
  - base/upper layer 具体布局
- 结论:
  批次 C 之后，优先抽“只需要 NodeStore / Quantizer 接口”的部分；
  直接触碰 PG store 写路径的函数继续延后

2026-04-24 PG bridge 接入现状细化:
- [graph_index_scan.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_scan.cpp)
  已支持在受限条件下走 `TrySearchViaCoreBridge`
- [graph_index_insert.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_insert.cpp)
  已支持在受限条件下走 `TryInsertViaCoreBridge`
- [graph_index_build.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_build.cpp)
  已存在 bridge 相关接线和 eligibility 判断
- 但总体仍是“窄桥接”，不是“主算法已全面通用化”
- 因此批次 C 的主要目标不是扩桥，而是先校准通用层边界

## 6. 迁完后可删除的重复实现

前提都是“调用链已经全部切换到 `libvex-core`”。

### 6.1 Duck 可删除候选

- [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp)
- [vex_graph_index_core.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_graph_index_core.hpp)

### 6.2 Duck 可收缩为薄兼容头的候选

- [vex_quantizer.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_quantizer.hpp)
- [vex_distance.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_distance.hpp)

### 6.3 PG 可删除或迁出的候选

- `vexdb-pg/rabitq` 下纯算法头
- `vexdb-pg/src/distance/rabitq_template.cpp` 中可抽象的通用部分

## 7. 短期不要优先动的文件

这些文件虽然大，但不属于当前“重复算法核心”的第一优先级。

### 7.1 PG SIMD 距离模板

- [code_distance_template.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/distance/code_distance_template.cpp)
- [rabitq_template.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/distance/rabitq_template.cpp)

### 7.2 Duck optimizer / executor 管线

- [vex_optimizer.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/optimizer/vex_optimizer.cpp)
- [vex_physical_index_scan.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/optimizer/vex_physical_index_scan.cpp)

原因:
- 这些不是当前主要重复源
- 先收 HNSW / PQ / RabitQ 算法本体，收益更大

## 8. 迁移批次表

### 批次 A: Duck PQ 去重

目标:
- Duck 不再维护一份独立 PQ 算法实现

涉及文件:
- [product_quantizer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/product_quantizer.cpp)
- [vex_quantizer.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_quantizer.hpp)
- [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp)
- [graph_index.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index.cpp)

完成标准:
- PQ train / encode / search 全部走 `libvex-core::ProductQuantizer`
- Duck 本地 PQ 实现文件删除或降为兼容别名

风险点:
- PQ 代码存储格式兼容
- Duck 序列化/反序列化兼容

验证点:
- PQ 建索引
- PQ 搜索
- 旧索引反序列化

2026-04-24 已落地:
- 新增 [vex_adapter_quant_runtime.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_quant_runtime.hpp)
- 收敛 [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp) 中 `TrainPQ` / `EncodeAllPQ` 的公共骨架
- 收敛 [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp) 中 `SearchWithPQ` 的 query prepare / code index / rerank-sort 公共骨架
- 将 Duck 侧重复的并发/配置/visited-set 基础设施切到 core：
  [vex_graph_index_core.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_graph_index_core.hpp)
  现在直接复用 [vex_concurrency.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_concurrency.hpp) 和 [vex_config.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_config.hpp)
- 将 Duck core bridge 的 delete 主算法迁到 core：
  [vex_graph_maintenance.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_graph_maintenance.hpp)
  [graph_maintenance.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/graph_maintenance.cpp)
  适配层现在只保留 row_id / dedup 映射处理和一次调用
- 将 Duck 主入口的 bridge 语义从 `Try/false` 收紧为 `OrThrow`：
  [vex_graph_index.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_graph_index.hpp)
  [graph_index.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index.cpp)
  [vex_core_bridge_runtime.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_bridge_runtime.cpp)
  [vex_core_bridge_graph_helpers.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_bridge_graph_helpers.cpp)
  现在 `VEX_ENABLE_LIBVEX_CORE_BRIDGE=ON` 时，build/search/delete/filtered/pq-search 主入口不再保留 bridge 失败回退分支
- 收敛 [vex_core_bridge_graph_helpers.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_bridge_graph_helpers.cpp) 中 `row_id -> PQ code` 映射构建
- 将 [vex_quantizer.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_quantizer.hpp) 压成 core 兼容头
- 修正 [CMakeLists.txt](/Users/sunji/Work/VexDB-Lite/vexdb-duck/CMakeLists.txt) 中目录改造后的 `libvex-core` 路径引用，并将 core bridge 默认值切为 `ON`
- 将 Duck PQ 实现编译链路统一到 [product_quantizer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/product_quantizer.cpp)
- 删除已脱离编译链路的 Duck 本地 `product_quantizer.cpp`

当前仍保留在 Duck 侧的 PQ 相关文件:
- [graph_index.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index.cpp)
- [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp)
- [vex_graph_index_core.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_graph_index_core.hpp)
- [vex_quantizer.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_quantizer.hpp)

说明:
- `vex_quantizer.hpp` 现在已不再维护本地 `ProductQuantizer` 定义，只保留 Duck 命名空间到 core 类型的兼容别名
- Duck 本地 `product_quantizer.cpp` 已删除，PQ 算法实现单点收敛到 `libvex-core`

剩余未完成项:
- 将 `SearchWithPQ` 进一步切到 `libvex-core::HNSWGraph::SearchWithQuantizedCodes`
- 继续评估 `graph_index_core.cpp` / `vex_graph_index_core.hpp` 中仍然留在适配层的 HNSW/PQ 结构性代码，分离出下一批可迁移骨架
- `GraphCandidate` 以及 `SearchLayer` / `SelectNeighbors` / `InsertNode` / `FilteredSearch` 等主算法实现仍主要留在 Duck，本批尚未动设计层

### 批次 B: Duck HNSW 主算法切换到 core

目标:
- `GraphIndexCore` 不再承载通用 HNSW 算法本体

涉及文件:
- [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp)
- [vex_graph_index_core.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_graph_index_core.hpp)
- [vex_core_bridge_runtime.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_bridge_runtime.cpp)
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp)

完成标准:
- build/search/delete/filtered/PQ 主路径都走 `libvex-core`
- Duck 只保留 allocator、row map、metadata、Dedup、binding

当前状态:
- 以上“主路径走 `libvex-core`”已基本完成
- 当前剩余问题已从“运行时 fallback”转为“源码树里仍保留本地 `GraphIndexCore` 算法实现”
- `GraphIndexCore` 仍直接承载的能力，主要集中在：
  - dedup 逻辑
  - allocator owner / row_id_map / dedup_map / pq_codes
  - 非 bridge 编译下的本地 HNSW/PQ 算法

风险点:
- 并发建索引行为回归
- Dedup 行为一致性
- entry point / delete 清理逻辑一致性

验证点:
- 单线程 build
- 并发 build
- delete + vacuum-like 清理
- filtered search

2026-04-24 继续落地:
- 在 [vex_adapter_graph_runtime.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_graph_runtime.hpp)
  新增 `ResolveLiveEntryPoint` 和 `GreedyDescendUpperLayers`
  两个通用 helper，把“活跃 entry point 选择”和“upper-layer greedy descent”从 Duck 本地搜索路径里抽成 core 侧骨架
- 将 [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp)
  中 `Search` / `SearchWithPQ` / `FilteredSearch` / `TryDedup`
  的重复 upper-layer skeleton 切到以上 core helper
- 将 [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp)
  中 exact / brute-force / filtered 路径重复的
  “distance + row_id” 排序胶水切到已有 core helper
  `RefineAndSortCandidates`
- 将 [graph_index.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index.cpp)
  的 `Build` / `BuildConcurrent` / `BuildParallel` / `Delete` / `Search` / `FilteredSearch`
  主入口完全收敛为 bridge-only；
  同时去掉该文件内序列化/反序列化的 bridge 条件编译死分支，
  不再保留本地 fallback
- 将 `vexdb-duck` 的 bridge 条件编译外壳继续裁掉：
  - [CMakeLists.txt](/Users/sunji/Work/VexDB-Lite/vexdb-duck/CMakeLists.txt)
    不再提供 `VEX_ENABLE_LIBVEX_CORE_BRIDGE` 选项，bridge 源文件直接纳入构建
  - [vex_graph_index.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_graph_index.hpp)
    [vex_graph_index_core.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_graph_index_core.hpp)
    [vex_core_node_store_bridge.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_core_node_store_bridge.hpp)
    [vex_core_bridge_graph_helpers.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/include/vex_core_bridge_graph_helpers.hpp)
    的 `#ifdef VEX_ENABLE_LIBVEX_CORE_BRIDGE` 包装已移除
  - [vex_core_bridge_runtime.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_bridge_runtime.cpp)
    [vex_core_bridge_graph_helpers.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_bridge_graph_helpers.cpp)
    [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp)
    的外层 bridge 宏包裹已移除
  - [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp)
    中 PQ distancer 相关 bridge 条件分支已收敛为单路径
  - 当前 `vexdb-duck` 代码内已不再出现 `VEX_ENABLE_LIBVEX_CORE_BRIDGE`
  - [vexdb-pg/CMakeLists.txt](/Users/sunji/Work/VexDB-Lite/vexdb-pg/CMakeLists.txt)
    和 [Makefile](/Users/sunji/Work/VexDB-Lite/vexdb-pg/Makefile)
    的默认编译设置已切到 `PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE=ON`
  - PG 后续移除开关的收敛顺序:
    1. 先把构建层默认值固定为 `ON`，保证未显式传参时主路径就是 bridge
    2. 再逐批删除 PG 入口文件中的 `#ifdef PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE`
       fallback 外壳，只保留 bridge 主实现
    3. 最后移除 `PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE` 这个构建选项本身，
       让 PG 和 Duck 一样默认只编译 core-bridge 主线
  - 当前已完成 PG 第一步“外圈常驻化”:
    - [vexdb-pg/CMakeLists.txt](/Users/sunji/Work/VexDB-Lite/vexdb-pg/CMakeLists.txt)
      [vexdb-pg/Makefile](/Users/sunji/Work/VexDB-Lite/vexdb-pg/Makefile)
      已把 `libvex-core` 依赖源文件和 `core_node_store_bridge.cpp` 常驻纳入构建
    - [core_node_store_bridge.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge.h)
      [core_node_store_bridge_utils.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_utils.h)
      [core_node_store_bridge_readonly.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_readonly.hpp)
      [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp)
      [core_node_store_bridge_runtime.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_runtime.hpp)
      [core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/core_node_store_bridge.cpp)
      已不再依赖外围 `#ifdef PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE`
    - [graph_index_scan.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_scan.cpp)
      [graph_index_insert.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_insert.cpp)
      [graph_index_inspect.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_inspect.cpp)
      [graph_index_vacuum.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_vacuum.cpp)
      已切到“bridge 常驻，运行时按能力判断是否走 core”单路径
    - [graph_index_build.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_build.cpp)
      里的 core-build/legacy-build 双路径尚未收敛；这部分保留到下一批继续处理

本轮验证:
- 每一步修改后都重编:
  `cmake --build build/standalone-v144 --target vex_loadable_extension -j 4`
- 功能测试:
  [run_extension_function_smoke.sh](/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_extension_function_smoke.sh) 各轮均 4/4 通过
- PQ benchmark:
  [run_pq_sift_benchmark.sh](/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_pq_sift_benchmark.sh) `10k`
  各轮稳定，`PQ (m=32, dsub=4)` 约 `QPS=8047~8080`
- SQL benchmark:
  [run_sift_sql_benchmark.sh](/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_sift_sql_benchmark.sh) `10k`
  全部确认 `uses_vex_index_scan=true`
  复跑稳定样本包括：
  `recall@10=0.999, recall@100=0.9993`
  以及 `recall@10=0.996, recall@100=0.9944`
  中途仍有少量低召回波动样本，当前判断更接近 HNSW 构图随机性波动，而不是本轮 skeleton 抽取引入的确定性回归
- 本次 bridge-only 收敛验证样本:
  `load_ms=81.1801`
  `build_ms=20855.9`
  `query_ms=998.067`
  `qps=200.387`
  `recall@10=1`
  `recall@100=0.9995`
  `uses_vex_index_scan=true`
  PQ benchmark 复测:
  `PQ (m=32, dsub=4) QPS=8069, Recall@100=0.8695`
- 第二轮“构建层/声明层去条件编译”验证样本:
  `load_ms=77.549`
  `build_ms=20831.1`
  `query_ms=1031.9`
  `qps=193.817`
  `recall@10=0.9955`
  `recall@100=0.99445`
  `uses_vex_index_scan=true`
  PQ benchmark:
  `PQ (m=32, dsub=4) QPS=7979, Recall@100=0.8695`
- 第三轮“完全去 bridge 宏痕迹”验证样本:
  `load_ms=65.1638`
  `build_ms=20635.9`
  `query_ms=976.755`
  `qps=204.76`
  `recall@10=1`
  `recall@100=0.99945`
  `uses_vex_index_scan=true`
  PQ benchmark:
  `PQ (m=32, dsub=4) QPS=8086, Recall@100=0.8695`
- 第四轮“dedup 结果展开尾部 helper 化”:
  - 在 [vex_adapter_graph_runtime.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_graph_runtime.hpp)
    新增
    `AppendExtraRowsWithDistance`
    `AppendResultRowWithExtras`

2026-04-24 PG benchmark 补齐:
- 新增 [pg_sift_sql_benchmark.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/test/benchmark/pg_sift_sql_benchmark.cpp)
  作为 PG 侧 SQL benchmark 程序，使用 `libpq` 直连 PostgreSQL，复用 Duck 侧
  `sift_train_{10k,100k}.fbin` / `sift_query_200.fbin` / `sift_gt_*`
  数据集，输出:
  `load_ms`
  `build_ms`
  `query_ms`
  `qps`
  `recall@10`
  `recall@100`
  以及基于 `EXPLAIN` 的索引命中检查结果
- 新增 [run_sift_sql_benchmark.sh](/Users/sunji/Work/VexDB-Lite/vexdb-pg/test/run_sift_sql_benchmark.sh)
  作为 PG 侧统一入口，参数形式:
  `run_sift_sql_benchmark.sh <pguri> <10k|100k|both> <data_dir>`
  默认直接复用 [benchmark/data](/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/benchmark/data)
  以便与 Duck benchmark 直接对齐
  - 将 [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp)
    中 `Search` / `BruteForceSearch` / `SearchWithPQ` /
    `BruteForceFilteredSearch` / `FilteredSearch`
    的 dedup 结果展开尾部统一收敛到上述 core helper
  - 验证样本:
    `load_ms=76.5918`
    `build_ms=20411.2`
    `query_ms=971.54`
    `qps=205.859`
    `recall@10=0.996`
    `recall@100=0.9976`
    `uses_vex_index_scan=true`
    PQ benchmark:
    `PQ (m=32, dsub=4) QPS=8073, Recall@100=0.8695`
  - 复测样本:
    `load_ms=68.6014`
    `build_ms=20832.2`
    `query_ms=998.39`
    `qps=200.322`
    `recall@10=0.998`
    `recall@100=0.9988`
    `uses_vex_index_scan=true`
- 第五轮“level-0 入口骨架 helper 化”:
  - 在 [vex_adapter_graph_runtime.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_graph_runtime.hpp)
    新增 `ResolveLevel0EntryPoint`
  - 将 [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp)
    中 `Search` / `SearchWithPQ` / `TryDedup` /
    `FilteredSearch(post-filter)` / `FilteredSearch(in-graph)`
    的
    `ResolveLiveEntryPoint + GreedyDescendUpperLayers`
    重复骨架统一改成上述 core helper
  - 验证样本:
    `load_ms=68.9365`
    `build_ms=21164.4`
    `query_ms=988.356`
    `qps=202.356`
    `recall@10=0.9995`
    `recall@100=0.999`
    `uses_vex_index_scan=true`
    PQ benchmark:
    `PQ (m=32, dsub=4) QPS=7739, Recall@100=0.8695`

### 批次 C: PG 边界整理与通用性校准

目标:
- 在继续抽 Duck `graph_index_core` 之前，先用 PG 现状校准“什么是后端无关算法”
- 防止把 Duck 特化骨架误抽成 `libvex-core` 公共层

涉及文件:
- [graph_index_algorithm.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/graph_index_algorithm.h)
- [core_node_store_bridge_runtime.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_runtime.hpp)
- [graph_index_scan.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_scan.cpp)
- [graph_index_insert.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_insert.cpp)
- [graph_index_build.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_build.cpp)

完成标准:
- 明确列出 `graph_index_algorithm.h` 中:
  - 可迁入 `libvex-core` 的算法核
  - 必须保留在 PG 适配层的胶水
- 明确 PG bridge 当前能力边界:
  - 支持什么
  - 不支持什么
  - 为什么不支持
- 不直接改动 PG `AM / executor / build / insert` 主流程设计

风险点:
- 把 PG 存储特化逻辑误判为通用层
- 为了追求复用而过早抽象 clustered / async 路径

验证点:
- 文档清单和代码分布一致
- 后续每一批迁移都能对照这份边界清单判断是否越界

2026-04-24 本批次已完成:
- 补充 PG bridge 当前覆盖面和限制条件
- 补充 `graph_index_algorithm.h` 的函数级拆分清单
- 补充 `graph_index_algorithm.h` / `pq.h` / `rabitq.h`
  到 `libvex-core` 现有或未来目标文件的落点映射
- 补充当前阻塞项，明确哪些依赖必须先由 NodeStore / runtime 抽象承接
- 明确批次 C 不直接改 `AM / build / scan / insert` 主流程
- 将后续顺序调整为:
  - 先完成 PG 边界校准
  - 再迁 RabitQ / PQ 等明确通用算法
  - 然后再继续收敛 Duck / PG 共用搜索骨架

### 批次 D: PG RabitQ 算法本体迁移

目标:
- PG 只保留 RabitQ 适配层，不保留主要算法本体

涉及文件:
- [rabitq.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/rabitq.h)
- [utils.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/utils.h)
- [query.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/query.h)
- [estimator.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/estimator.h)
- [rotator.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/rotator.h)
- [rabitq_distancer.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/rabitq/rabitq_distancer.h)
- [rabitq_distancer.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/rabitq_distancer.cpp)

完成标准:
- RabitQ 数学逻辑进入 `libvex-core`
- PG 侧只保留 metadata、cache、distancer 接口和执行入口

风险点:
- quantizer metadata 兼容
- PG cache 生命周期
- SIMD 路径对接

验证点:
- build/scan 时 quantizer 类型识别
- cache 命中/失效
- 距离计算一致性

建议先手顺序:
1. `rabitq/utils.h`
2. `rabitq/query.h`
3. `rabitq/estimator.h`
4. `rabitq/rotator.h`
5. `rabitq/rabitq.h`
6. `rabitq_distancer.h/.cpp`

原因:
- 前四者更接近纯数学/数据变换
- `rabitq.h` 在其之上组装 quantizer
- `rabitq_distancer` 最后再改，能把 PG 侧接口层做得更薄

### 批次 D.1: PG PQ 对齐 core

目标:
- 在继续 RabitQ 之前，盘清 PG `pq.h/.cpp` 与 `libvex-core::ProductQuantizer`
  的重复面，逐步收敛到 core
- 以 `libvex-core` 现有 PQ 实现为准补齐 PG 适配层，不再尝试维护第二份 PG PQ 算法本体

涉及文件:
- [pq.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/pq.h)
- [pq.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/pq.cpp)
- [pq_distancer.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/pq_distancer.h)
- [vex_quantizer.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_quantizer.hpp)
- [product_quantizer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/product_quantizer.cpp)
- [vex_quant_distancer.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_quant_distancer.hpp)
- [quant_distancer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/quant_distancer.cpp)

完成标准:
- PG 不再长期维护独立 PQ 算法本体
- PG 只保留:
  - metadata 持久化
  - distancer prepare/process 接口
  - PG cache / block 读写

2026-04-24 现状判断:
- [pq.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/pq.cpp)
  中 `ProductQuantizer` 和 `PQDistancer` 目前基本都是 stub:
  - `train()` 直接报 `not implemented`
  - `compute_code()` / `compute_distance_table()` / `distance_to_code()` 未实现
  - `prepare()` / `process()` / `flush()` 未实现
- [pq_distancer.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/pq_distancer.h)
  也是 stub 兼容壳
- 因此本批次不是“合并两套成熟 PQ 实现”
- 实际上更接近:
  - 直接以 `libvex-core::ProductQuantizer` / `PQDistancerCore` 为算法单一来源
  - PG 只补 metadata / page IO / cache / distancer 外壳

2026-04-24 字段与接口对照:

PG `ProductQuantizer`:
- `d` -> core `ProductQuantizer::d`
- `M` -> core `ProductQuantizer::m`
- `dsub` -> core `ProductQuantizer::dsub`
- `centroids` -> core `ProductQuantizer::centroids`
- `code_size` / `nbits` / `ksub`
  需要重新评估:
  - core 现实现固定 `KSUB=256`
  - code size 实际等于 `m`
  - PG 头里的 bit-level 通用字段更像历史接口兼容壳

PG `PQDistancer`:
- `dist_table`
  可对齐到 core `PQDistancerCore::dist_table_`
- `prepare/process`
  语义可对齐到 core `LoadQuantizer + PrepareQuery`
- `compute_code`
  可直接转调 core `ProductQuantizer::Encode`
- `get_distance_single/get_distance_batch2`
  应优先转调 core 的距离表逻辑
  SIMD 批量优化若后续需要，再在 PG 适配层单独补

2026-04-24 建议迁移顺序:
1. 保持 PG 现有头文件接口不变，先把内部实现切到 core
2. 在 PG 侧新增最薄兼容层:
   - core PQ 对象持有
   - dist_table 生命周期管理
   - metadata / qtcode block 读写
3. 再评估是否需要保留 PG 自己的 batch/SIMD PQ 壳
4. 最后再收缩 `pq.h` 的历史字段，避免一次改太多接口

当前阻塞项:
- PG quantizer 元数据和 `qtcode_block` 的磁盘格式仍未和 core 序列化格式对齐
- PG `prepare/process/flush` 所需的 page IO 和 cache 生命周期仍是 PG 特有
- 如果要保留 PG SIMD PQ 批处理路径，需要额外决定:
  - 是继续保留 PG 特化优化壳
  - 还是先完全走 core 标准路径，后续再优化

2026-04-24 首个实现批次已落地:
- 在 [vex_quant_distancer.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_quant_distancer.hpp)
  为 `PQDistancerCore` 新增显式 `DistanceBatch` override
- 在 [quant_distancer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/quant_distancer.cpp)
  新增 batch fast-path 结构:
  - `DistanceBatchScalar`
  - `TryDistanceBatchSIMD`
- 当前 SIMD 实现策略:
  - 仅在 `x86 + AVX2` 条件下启用
  - 先支持 `m=4` / `m=8` 的 PQ code distance table 查表累计
  - 其他平台和参数全部自动回退到标量路径
- 当前批次刻意未做:
  - 不改 `product_quantizer.cpp`
  - 不改 PG / Duck 适配层调用面
  - 不改 `qtcode_block` / metadata / cache 生命周期

2026-04-24 第二个实现批次已落地:
- 将 [quant_distancer.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/quant_distancer.cpp)
  中的 AVX2 fast-path 从固定 `m=4/8` 扩展为:
  - 任意 `m` 为 `4` 的倍数可走 SIMD
  - 其中按 `8` 子量化器块优先累计，尾部再处理 `4` 子量化器块
- 这使常见配置 `m=16 / 32` 也能直接复用 core 的 batch fast-path
- 更新 [quant_distancer_smoke.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/tests/quant_distancer_smoke.cpp)
  让其覆盖:
  - `pq_m=2`
  - `pq_m=4`
  - `pq_m=8`
  - `pq_m=16`
  - `pq_m=32`
  并检查 `DistanceBatch` 与 `DistanceSingle` 结果一致

本批次验证:
- `vex_core_quant_distancer_smoke_test`
  通过
- Duck extension 重编:
  通过
- 功能 smoke:
  4/4 通过
- SQL benchmark:
  `load_ms=69.3277`
  `build_ms=20701.4`
  `query_ms=988.106`
  `qps=202.407`
  `recall@10=1`
  `recall@100=0.9995`
  `uses_vex_index_scan=true`
- PQ benchmark:
  `PQ (m=32, dsub=4) QPS=7767`
  `Recall@100=0.8695`

本批次验证:
- `libvex-core` 单测:
  `cmake -S libvex-core -B build/libvex-core-tests -DVEX_CORE_BUILD_TESTS=ON`
  `cmake --build build/libvex-core-tests --target vex_core_quant_distancer_smoke_test -j 4`
  `./build/libvex-core-tests/vex_core_quant_distancer_smoke_test`
  通过
- Duck extension 重编:
  `cmake --build build/standalone-v144 --target vex_loadable_extension -j 4`
  通过
- 功能 smoke:
  [run_extension_function_smoke.sh](/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_extension_function_smoke.sh)
  4/4 通过
- SQL benchmark:
  [run_sift_sql_benchmark.sh](/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_sift_sql_benchmark.sh)
  `10k`
  `load_ms=69.6974`
  `build_ms=21051.8`
  `query_ms=991.264`
  `qps=201.763`
  `recall@10=0.9995`
  `recall@100=0.99915`
  `uses_vex_index_scan=true`
- PQ benchmark:
  [run_pq_sift_benchmark.sh](/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_pq_sift_benchmark.sh)
  `10k`
  `PQ (m=32, dsub=4) QPS=7754`
  `Recall@100=0.8695`

风险点:
- PG 现有 PQ 持久化格式兼容
- batch distance / SIMD 路径兼容
- build 时训练参数一致性

### 批次 E: PG / Duck 通用搜索骨架再收敛

目标:
- 在 PG 边界明确之后，再继续抽 Duck / PG 共同使用的搜索骨架
- 保证迁入 `libvex-core` 的 helper 不是单边后端特化形态

涉及文件:
- [graph_index_core.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/graph_index_core.cpp)
- [graph_index_algorithm.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/graph_index_algorithm.h)
- [vex_adapter_graph_runtime.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_graph_runtime.hpp)
- [vex_graph_algo.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_graph_algo.hpp)
- [graph_algo.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/graph_algo.cpp)

优先抽取对象:
- upper-layer / level-0 入口骨架
- result rerank / limit / expansion skeleton
- filtered search 的后端无关部分

暂不抽取:
- PG `ItemPointer/TID` 展开
- PG async merge
- PG clustered store 特化
- Duck allocator owner / row map / dedup_map 持有逻辑

### 批次 F: bridge runtime 最终瘦身

目标:
- PG / Duck bridge runtime 只剩后端胶水，不再承载算法细节

涉及文件:
- [vex_core_bridge_runtime.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_bridge_runtime.cpp)
- [core_node_store_bridge_runtime.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_runtime.hpp)
- [vex_adapter_graph_runtime.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_graph_runtime.hpp)

完成标准:
- 通用 runtime 逻辑全部集中在 `libvex-core`
- 后端 runtime 只处理 config、binding、结果映射

2026-04-24 继续落地:
- 在 [vex_adapter_quant_runtime.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_quant_runtime.hpp) 新增 `BuildNodeFlatCodePointerIndex`，
  将 `row_id -> flat PQ code -> node_id` 的通用映射构建收敛到 `libvex-core`
- 将 Duck 侧原本在 [vex_core_bridge_graph_helpers.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_bridge_graph_helpers.cpp)
  里的 PQ code 索引构建逻辑改为直接调用 core helper
- 进一步删除 Duck 侧 `BuildCoreBridgePQCodeIndex` 壳方法，
  [vex_core_bridge_runtime.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_bridge_runtime.cpp)
  现在直接调用 core helper
- 将 Duck PQ bridge 查询从“临时构造 `PQDistancerCore`”改为复用
  `graph_.pq_distancer_core`，把 quant distancer 状态归回图对象

本轮验证:
- 功能测试:
  [run_extension_function_smoke.sh](/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_extension_function_smoke.sh) 4/4 通过
- SQL benchmark:
  [run_sift_sql_benchmark.sh](/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_sift_sql_benchmark.sh) `10k`
  复跑确认 `uses_vex_index_scan=true`，稳定样本为 `recall@10=1`、`recall@100=0.99945`
- PQ benchmark:
  [run_pq_sift_benchmark.sh](/Users/sunji/Work/VexDB-Lite/vexdb-duck/test/run_pq_sift_benchmark.sh) `10k`
  指标稳定，`PQ (m=32, dsub=4)` 约 `QPS=8050`

## 9. 当前优先级判断

最高优先级:
1. Duck `graph_index_core`
2. PG `graph_index_algorithm` 边界整理
3. PG `rabitq`
4. PG `pq.h`

原因:
- Duck `graph_index_core` 仍然是最大的本地算法负债
- 但在继续抽 Duck 骨架前，需要先用 PG 校准通用层边界，避免把单边形态固化进 `libvex-core`
- RabitQ / PQ 仍然是 PG 侧最明确的通用算法迁移目标
- 优先处理后，`vexdb-duck` / `vexdb-pg` 会更接近“适配层 + storage binding”，而不是“第二份算法库”
