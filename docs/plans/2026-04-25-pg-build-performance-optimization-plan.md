# PG 构建性能优化方案

日期: 2026-04-25

目的:
- 为 `vexdb-pg` 当前 bridge-on 路径下的 HNSW 构建性能问题留一份明确的优化方案
- 优化范围严格限定在 PG 适配层 / bridge / 访问路径，不修改 `graph_index` 算法设计
- 为后续每一批代码修改提供可验证的性能目标和回归标准

相关背景文档:
- [2026-04-21-node-store-direct-binding-design.md](/Users/sunji/Work/VexDB-Lite/docs/plans/2026-04-21-node-store-direct-binding-design.md)
- [2026-04-24-core-migration-file-inventory.md](/Users/sunji/Work/VexDB-Lite/docs/plans/2026-04-24-core-migration-file-inventory.md)
- [2026-04-25-pinnode-pg-duck-cost-comparison.md](/Users/sunji/Work/VexDB-Lite/docs/plans/2026-04-25-pinnode-pg-duck-cost-comparison.md)

边界:
- 不调整 `graph_index_algorithm` 设计
- 不改变 HNSW 插入、搜索、剪枝策略
- 不引入全量序列化/反序列化搬运
- 不优先处理非 bridge 路径
- 默认编译路径保持 `PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE=ON`

## 1. 当前问题定义

### 1.1 已确认现象

在 111 服务器上，PG `1k` smoke benchmark 结果如下:
- `build_ms=64997.5`
- `recall@10=1`
- `recall@100=0.9994`
- `uses_vex_index_scan=true`

结论:
- 正确性基本没问题
- 查询路径也已走向量索引
- 当前主要问题是构建性能

### 1.2 已确认瓶颈位置

现有 profiling 已经表明:
- `add_point_total_ms=64708.882`
- `store_graph_state_ms=0.010`
- `pin_read_calls=6836879`
- `pin_read_ms=52456.221`
- `backlink_ms=42390.002`
- `prune_ms=32043.742`

这说明:
- 不是 fallback 慢
- 不是 graph state 持久化慢
- 不是写回 row_id 慢
- 不是查询路径慢
- 主要是 `InsertNode` 过程中的高频只读节点访问太贵

### 1.3 根因判断

当前瓶颈不是“读访问次数异常”。

根因是:
- HNSW 构建本身就会在 `SearchLayer / SelectNeighbors / backlink prune` 中产生大量读节点访问
- PG 适配层当前的 `PinNode(false, ...)` 单次成本过重
- 这些成本被几百万次访问放大后，吞掉了绝大多数构建时间

## 2. 优化目标

### 2.1 总目标

将 PG bridge-on 路径构建成本从“被适配层访问成本主导”收敛到“主要由算法本体决定”。

目标不是:
- 追求一次性做到最优

目标是:
- 先消灭最明显的适配层冗余开销
- 保持 bridge 架构稳定
- 在每一批修改后保持可验证

### 2.2 阶段性目标

第一阶段目标:
- 明显降低 `pin_read_ms`
- 明显降低 `backlink_ms`
- 明显降低 `prune_ms`
- 不引入 recall / 构建正确性回归

第二阶段目标:
- 继续降低 `build_ms`
- 让 PG 只读 `PinNode` 的成本模型尽量接近 DuckDB live binding

第三阶段目标:
- 默认只保留 bridge 主路径
- 逐步移除 bridge-off 分支的存在意义

## 3. 优化原则

### 3.1 不碰算法设计

禁止动作:
- 修改 `SearchLayer`
- 修改 `SelectNeighbors`
- 修改 `InsertNode` 决策逻辑
- 修改 HNSW 的 m / ef / prune 策略

允许动作:
- 优化 PG 适配层 `PinNode / PinNodeForUpdate / UnpinNode`
- 优化 PG 适配层节点布局访问方式
- 优化数据 view 暴露和临时对象构造成本

### 3.2 只优化“单位访问成本”

本阶段的优化对象不是:
- 调用次数本身

本阶段的优化对象是:
- 每一次 `PinNode(false, ...)` 的材料化成本
- 每一次 `UnpinNode` 的释放成本
- 写路径中不必要的读回/重建成本

### 3.3 先只读后可写

原因:
- profiling 表明 `pin_write_ms` 很小
- 当前最大头是只读 pin

顺序:
1. 先瘦身只读 `PinNode`
2. 再评估是否还需要优化 `PinNodeForUpdate`
3. 最后才考虑写回路径细节

## 4. 方案总览

优化分三批推进:

### 批次 P1

只读 `PinNode` 轻量化

目标:
- 去掉明显的临时对象和重复装配

### 批次 P2

邻接视图直出化

目标:
- 尽量避免把底层邻接信息重新 materialize 成临时副本

### 批次 P3

热点读访问复用

目标:
- 对高频读节点访问进一步收缩分配和重复准备成本

### 批次 P4

PG 并行构建接入评估

目标:
- 明确当前 `vexdb-pg` 索引构建仍是单线程
- 在适配层性能热点收敛后，再评估 PostgreSQL 并行 build 接入
- 不把“并行化”误当成当前 `PinNode` 热点问题的替代修复

## 5. 批次 P1 方案

### 5.1 问题

当前 PG 只读 `PinNode` 每次都会:
- `new PinnedNodeToken`
- 分配 `level0_neighbors`
- 分配 `upper_neighbors`
- 分配 `metadata`
- 构造 `base_raw`
- 每层构造 `upper_raw`
- 组装 header
- 再把底层邻接 copy 到 token

这类成本在几百万次调用下不可接受。

### 5.2 目标

优先移除这些最显著的冗余:
- `base_raw` / `upper_raw` 临时容器
- 不必要的 metadata 分配
- 不必要的 header 补齐流程

### 5.3 具体动作

1. 将 `base_raw` / `upper_raw` 改为固定大小栈上缓冲或可复用轻量 scratch
2. 只在真正需要 metadata 时才构造 metadata 访问
3. 只读 pin 路径剥离与写路径共享的多余准备逻辑
4. 明确区分只读 token 和可写 token，避免只读 pin 携带写路径负担

### 5.4 预期收益

优先降低:
- `pin_read_ms`
- `unpin_ms`

同时会间接降低:
- `backlink_ms`
- `prune_ms`

### 5.5 风险

风险:
- scratch 生命周期处理不当导致悬垂指针
- 只读/可写 token 拆分后逻辑分叉变复杂

缓解:
- 保持 handle 生命周期边界不变
- 单元测试和 smoke test 必须覆盖

### 5.6 失败样本与修正

2026-04-25 第一次 `P1` 尝试已经证明一个错误方向:
- 不能用“大固定数组内嵌到 pin state”去替代当前的小对象 + 按需邻接缓冲

失败样本特征:
- 把 `level0_neighbors / upper_neighbors` 改成按 `MAX_M` 上限内嵌的大对象
- 去掉小型动态分配，但引入更大的单对象分配与更差的 cache locality

失败结果:
- `build_ms` 从约 `65.0s` 升到约 `76.1s`
- `pin_read_ms` 从约 `52.5s` 升到约 `65.9s`

结论:
- 当前热点不是“小 vector 分配本身”
- 而是“每次 pin 里反复做存储读取、layout decode 和中间搬运”
- pin state 必须继续保持小对象

## 8. 并行构建 TODO

当前状态:
- 当前 PG 索引构建主流程仍是单线程
- `parallel_workers` reloption 已存在，但 build 路径未真正启用并行 worker
- Index AM 当前也明确关闭了并行建索引能力

代码依据:
- [graph_index_am.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_am.cpp)
  - `amcanparallel = false`
  - `amcanbuildparallel = false`
- [graph_index_build.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/graph_index_build.cpp)
  - `parallel build not yet implemented, using single-threaded`

结论:
- 当前性能分析中的 `pin_read_ms / pin_vector_ms / build_ms`，都是单 backend 构建路径上的热点
- 这意味着现阶段应先把单线程单位访问成本降下来，再讨论并行 build

TODO:
1. 在当前 bridge / vector read 热点收敛后，单独立项评估 PG 并行 build 接入
2. 梳理哪些阶段可并行:
   - heap 扫描 / 采样
   - 训练前数据准备
   - 建图本体是否允许分阶段并行
3. 明确与现有 `graph_index` 算法设计边界
4. 评估 PostgreSQL `CreateParallelContext()` / parallel scan / worker 生命周期接线成本
5. 并行方案应单独验证:
   - 正确性
   - WAL / vacuum / entrypoint 一致性
   - recall 是否回归

### 5.7 P1-rework

新的 `P1` 收敛为更窄的目标:
- 不改邻接缓冲的按需大小策略
- 不再尝试用 `MAX_M` 固定大对象替换小对象
- 只优化中间读取路径

具体动作:
1. 保留小型 `NodePinState`
2. 将 `base_raw / upper_raw` 从每次分配的 `std::vector` 收敛成:
   - 小型栈缓冲，或
   - 绑定实例内可复用 scratch
3. 尽量让 header/deleted 状态准备合并，不做重复补齐
4. 先拆出只读 pin / 可写 pin 的准备路径，避免只读 pin 带着写路径包袱

成功标准:
- 在不改变 recall 和 build 正确性的前提下
- `pin_read_ms` 相对基线有明确下降
- `build_ms` 不高于基线

### 5.8 P1-rework 实测结果

2026-04-25 在 111 服务器上完成了 `P1-rework` 实测。

测试环境:
- 代码目录: `/opt/vexdb-lite-build/VexDB-Lite`
- `pg_config=/usr/pgsql-17/bin/pg_config`
- PostgreSQL: 17.9
- benchmark 命令:
  - `./test/run_sift_sql_benchmark.sh "postgresql:///postgres?host=/run/postgresql&port=55432&user=postgres" 10k /opt/vexdb-lite-build/VexDB-Lite/vexdb-duck/test/benchmark/data 1000`

为保证该轮验证可执行，还补了两项最小构建基线修复:
- `vexdb-pg/Makefile` 中将历史残留的 `src/distance/architecture_minimal.cpp` 修正为当前实际存在的 `src/distance/architecture.cpp`
- 为 PG 构建增加 `BOOST_INCLUDEDIR` 可配置入口，便于在 111 服务器上显式注入 Boost 头文件路径

基线结果:
- `build_ms=64997.5`
- `pin_read_calls=6836879`
- `pin_read_ms=52456.221`
- `backlink_ms=42390.002`
- `prune_ms=32043.742`
- `recall@10=1`
- `recall@100=0.9994`
- `uses_vex_index_scan=true`

本轮结果:
- `build_ms=62107`
- `query_ms=6662.71`
- `qps=30.0178`
- `pin_read_calls=6830887`
- `pin_read_ms=50252.258`
- `backlink_ms=40652.738`
- `prune_ms=30726.740`
- `recall@10=1`
- `recall@100=0.9994`
- `uses_vex_index_scan=true`
- `first_explain_has_index_scan=yes`

对比结论:
- `build_ms` 下降约 `2890.5ms`，约 `4.4%`
- `pin_read_ms` 下降约 `2204.0ms`
- `backlink_ms` 下降约 `1737.3ms`
- `prune_ms` 下降约 `1317.0ms`
- recall 无回退
- planner 未回退，查询仍然走 `VEX_INDEX_SCAN`

结论:
- `P1-rework` 成功
- 最小并发加固没有破坏性能主线
- 该批可以作为后续 PG 只读 `PinNode` 继续瘦身的新的性能基线

## 5A. 并发可见性约束

### 5A.1 新约束

从本批开始，明确以下约束:
- `node_count` 只作为图状态统计量和容量估计量使用
- `node_count` 不能作为节点可见性判断依据
- `node_id < node_count` 不能推出该节点当前仍然存在、未删除、未复用

这条约束适用于:
- `PinNode(...)` 的读路径设计
- 邻居过滤逻辑
- 并发删除 / vacuum / slot 复用场景下的正确性判断

### 5A.2 为什么计数不可靠

在 PG backend 中，`num_vectors` / graph state 中的 `node_count` 更接近:
- 当前图中的活跃节点数量
- 或构建过程中的统计量

它不是:
- 底层 slot 的稳定上界定义
- 更不是节点存活性的证明

因此下面这种判断不成立:
- `raw_id < node_count` -> 节点可安全访问

因为并发下可能发生:
1. 先读出某个邻居 `node_id`
2. 该节点随后被删除
3. 该 slot 被 vacuum 或 free list 复用
4. 再次按同一个 `node_id` 访问时，读到的已经不是原节点

### 5A.3 稳定身份的定义

这里的“稳定身份”是指:
- 一个足以区分“同一个逻辑节点”和“同一个 slot 上后来复用出来的新节点”的标识

典型形式包括:
- `(node_id, generation)`
- `(slot_id, version)`
- backend 可验证的稳定 `storage_key`
- 或更强约束: 在并发访问窗口内不允许 slot 复用

如果没有稳定身份，只有 `node_id`，就会出现典型 ABA 问题:
- 第一次看到的是旧节点 A
- 中途 A 被删除
- 同一 slot 被新节点 B 复用
- 第二次再按同一个 `node_id` 去 pin，拿到的是 B
- 算法层却仍把它当作 A 的后续状态

### 5A.4 设计要求

因此 PG bridge 读路径后续必须遵守:
1. 不再用 `node_count` 做节点可见性过滤
2. 邻居数组中的 `node_id` 只能视为候选引用，不能视为已确认可见
3. 真正访问节点时，必须依赖 backend pin + header/liveness 校验
4. 如果 PG base slot 存在删除后复用，则必须进一步补稳定身份校验；否则并发删除场景仍然存在逻辑漏洞

### 5A.6 最小加固策略

考虑到当前主线仍是性能优化，本阶段不把并发身份问题扩展成大改造，约束如下:
- 不修改 PG on-disk 节点格式
- 不新增节点 `generation/version` 字段
- 不修改 `libvex-core` 邻接接口形态
- 不在邻居复制阶段为每个 neighbor 增加一次额外 `get_itempointer(...)` 校验

原因:
- 这些方案都会明显扩大改动面
- 容易把性能优化主线打断
- 也会引入额外读放大，直接伤到当前 `pin_read_ms` 热点

因此本阶段只做低成本加固:
1. 去掉基于 `node_count` 的可见性判断
2. 仅保留最基本的非法值/物理越界保护，目的只是防止错误访问，而不是证明节点存活
3. 继续依赖 pin 后的 header `deleted/empty` 检查作为当前主防线
4. 保持 copy-then-consume，不直接暴露底层邻接内存

这意味着:
- 本阶段目标是“避免明显错误判断和越界访问”
- 不是“彻底解决 slot 复用下的 ABA 身份问题”

如果后续确认线上并发删除/复用确实构成稳定性问题，再单独立项补节点代际信息；该项不与当前性能批次绑定推进。

### 5A.7 当前代码现状结论

当前 PG 节点本体 `GraphIndexElementBase` 仅包含:
- deleted flag
- 扩展状态位
- heaptid 存储

当前没有现成字段可直接作为:
- node generation
- slot version
- 可复用的稳定代际标识

因此在“不改磁盘格式”的约束下，当前阶段不能把“稳定身份”完整做实，只能先把错误的计数式判断去掉，并把存活性判断收敛到 pin 后校验。

### 5A.5 对当前优化批次的影响

本阶段先做两件事:
- 收紧错误的计数式可见性判断，避免继续把统计量当作 liveness
- 保持 copy-then-consume 语义，不直接暴露底层邻接内存

本阶段暂不做的事:
- 不修改 `graph_index` 算法设计
- 不改变 graph state 存储格式
- 不一次性引入大范围 MVCC / snapshot 机制

后续如确认 PG base id 会复用，则把“稳定身份校验”单列为下一批改造项。

## 6. 批次 P2 方案

### 6.1 问题

当前 PG 邻接数据访问方式太像“解码 + copy”，不像“view”。

### 6.2 目标

尽量让只读 `PinNode` 更接近 DuckDB:
- 拿到底层句柄
- 直接暴露只读邻接视图
- 尽量不重复构建邻接副本

### 6.3 具体动作

1. 评估 base layer 是否可直接暴露只读邻接指针和 count
2. 评估 upper layer 是否可直接暴露按层邻接视图
3. 如无法直接暴露完整 view，则最少也要做到:
   - 一次 pin 只做一次 layout decode
   - 不做额外中间副本搬运
4. 将 `header.deleted` 等状态读取尽量合并到同一份底层 view 解码里

### 6.4 预期收益

这是最可能明显降低构建总时延的一批。

预期直接改善:
- `pin_read_ms`
- `backlink_ms`
- `prune_ms`
- `build_ms`

### 6.5 风险

风险:
- PG 底层布局本身未必天然适合直接 view
- 如果布局和 core 期望不一致，可能引入更多适配复杂度

缓解:
- 不要求一步到位零拷贝
- 先做到“单次 decode，少拷贝”

## 7. 批次 P3 方案

### 7.1 问题

即使单次只读 pin 变轻，热点路径仍会对同一节点反复访问。

### 7.2 目标

减少热路径上的重复准备工作。

### 7.3 具体动作

1. 对热点节点访问引入轻量级只读 token 复用
2. 评估只读 handle 池化或 small-object 优化
3. 对重复访问的邻接数据引入局部 scratch / 微缓存
4. 如需要，再做调用点分桶统计，精确定位 `SearchLayer` 与 `prune` 的读访问占比

### 7.4 预期收益

这一批更偏向精细化收尾。

预期收益:
- 降低长尾开销
- 平滑构建时延

### 7.5 风险

风险:
- 缓存生命周期和一致性处理复杂
- 过早做缓存可能导致结构膨胀

缓解:
- 只在 P1/P2 完成后，再决定是否做 P3

## 8. 明确不做的方案

本阶段不做:

1. 改 HNSW 算法逻辑
2. 改 `graph_index_algorithm` 原始设计
3. 用序列化/反序列化回退到整图内存路径
4. 优先优化查询路径
5. 优先优化 `StoreGraphState`
6. 优先优化 `UpdateNodeRowId`
7. 优先恢复或维护 bridge-off 分支

原因:
- 这些都不是当前构建瓶颈

## 9. 验证策略

### 9.1 每一批都必须跑

1. PG 功能 smoke
2. PG `1k` SIFT benchmark
3. 如相关代码影响到 core 公共逻辑，则补跑 `libvex-core` smoke

### 9.2 每一批必须比较的指标

至少对比:
- `build_ms`
- `add_point_total_ms`
- `pin_read_calls`
- `pin_read_ms`
- `pin_write_ms`
- `unpin_ms`
- `backlink_ms`
- `prune_ms`
- `recall@10`
- `recall@100`
- `uses_vex_index_scan`

### 9.3 成功标准

一批改动可接受的条件:
- recall 不下降
- `uses_vex_index_scan=true`
- 构建成功
- 核心耗时指标至少有一个显著下降

### 9.4 失败判定

以下任一出现应回滚方案方向:
- recall 明显回退
- 构建不稳定
- 代码复杂度明显上升但收益不明显
- 只读 pin 变轻但总 build 没有改善

## 10. 文件落点建议

重点改动文件:
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp)
- [core_node_store_bridge_runtime.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_runtime.hpp)

必要时联动:
- [core_node_store_bridge.h](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge.h)
- [core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/src/core_node_store_bridge.cpp)

如需 core 侧辅助统计:
- [vex_graph_algo.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_graph_algo.hpp)
- [graph_algo.cpp](/Users/sunji/Work/VexDB-Lite/libvex-core/src/graph_algo.cpp)

## 11. 推荐执行顺序

建议顺序:

1. 批次 P1
2. 功能测试 + `1k` benchmark
3. 评估 `pin_read_ms` 是否明显下降
4. 再进入批次 P2
5. 功能测试 + `1k` benchmark
6. 视收益决定是否做 P3

## 12. 当前决议

当前 PG 构建性能优化的主线明确为:
- 不动算法
- 专注瘦身 PG 适配层只读 `PinNode`
- 让 PG 的访问成本模型尽量接近 DuckDB live binding
- 逐批验证，不做大爆改
