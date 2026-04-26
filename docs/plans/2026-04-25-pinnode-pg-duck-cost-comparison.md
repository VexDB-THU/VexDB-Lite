# PG vs DuckDB `PinNode` 成本对照表

日期: 2026-04-25

目的:
- 留档当前 `libvex-core` 统一 HNSW 算法下，PG 与 DuckDB 适配层 `PinNode` 的实现差异
- 解释为什么两边都会有大量逻辑读访问，但 PG 索引构建明显更慢
- 为后续 PG 适配层收敛和瘦身提供优化顺序

边界:
- 不修改 `graph_index` 算法设计
- 不引入全量索引序列化/反序列化搬运
- 结论基于当前代码路径，而不是理想化架构

## 1. 结论摘要

结论:
- DuckDB 和 PostgreSQL 在 HNSW 构建期间都会发生大量逻辑上的只读节点访问
- 差异不在“有没有大量 `pin_read`”，而在“单次 `PinNode` 的成本模型是否足够轻”
- 当前 PG `PinNode` 更接近“读取底层存储并重新 materialize 一个 core 可消费视图”
- 当前 Duck `PinNode` 更接近“拿 allocator handle 和内存指针，再做少量翻译”
- 因此同样是高频读访问，DuckDB 可以承受，而 PG 会被放大成主要瓶颈

已知 PG profiling 数据:
- `pin_read_calls=6836879`
- `pin_read_ms=52456.221`
- `build_ms=64997.5`
- `backlink_ms=42390.002`
- `prune_ms=32043.742`

解释:
- `pin_read_calls` 高并不意外，HNSW 插入路径本来就会反复读节点
- 真正的问题是 PG 单次只读 pin 太重，导致几百万次访问累计成几十秒

## 2. 调用链总览

统一入口:
- core 算法调用 `store_.PinNode(node_id)`
- 适配层 direct backend 转发到 backend binding 的 `PinNode(node_id, false, view)`

公共 direct backend 转发位置:
- [vex_adapter_node_store_common.hpp](/Users/sunji/Work/VexDB-Lite/libvex-core/include/vex/vex_adapter_node_store_common.hpp:136)

PG 实现位置:
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:130)

DuckDB 实现位置:
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:372)

## 3. 共性步骤对照

两边当前实现都不是零成本。

共性动作:
- 校验 `node_id` 是否有效
- 构造一次 pin token
- 获取向量数据句柄或底层向量访问入口
- 把底层 header 字段转成 core 的 `NodeHeader`
- 把邻接信息转换成 core 使用的 `node_id_t` 数组
- 将 view 指针挂到 `out`
- `UnpinNode` 时销毁 token

共性结论:
- Duck 也不是完全零拷贝
- 但两边“重”的位置完全不同

## 4. PG `PinNode` 具体成本

实现位置:
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:130)

### 4.1 每次 pin 都会做的主要动作

1. 分配 `PinnedNodeToken`
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:139)

2. pin 向量 buffer
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:142)

3. 分配临时邻接表和 metadata 容器
- `level0_neighbors`
- `upper_neighbors`
- `metadata`
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:143)
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:147)

4. 组装 header
- 默认字段回填
- 查 `node_to_row_id_` / `upper_chain_by_node_`
- 通过 `get_itempointer(...)` 检查 deleted / empty
- 可能走 `load_node_header_cb`
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:149)
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:336)

5. 读取 base layer，并拷贝到 token
- 构造 `base_raw`
- `base_layer.get_n<ReadLock>(...)`
- 统计有效邻居数
- 再 copy 到 `token->level0_neighbors`
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:156)

6. 逐层读取 upper layer，并拷贝到 token
- 每层构造 `upper_raw`
- 每层 `upper_layer.get_n<ReadLock>(...)`
- 每层统计 count
- 每层 copy 到 `token->upper_neighbors`
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:168)

7. `UnpinNode` 时释放 token
- 可写 pin 还会额外 `FlushNode`
- [core_node_store_bridge_live.hpp](/Users/sunji/Work/VexDB-Lite/vexdb-pg/include/graph_index/core_node_store_bridge_live.hpp:199)

### 4.2 PG 的关键特征

PG 当前 `PinNode` 更像:
- 一次“读取底层存储对象 + 组装临时 core view”

而不是:
- 一次“直接返回底层内存 view”

额外放大项:
- `base_raw` / `upper_raw` 每次 pin 都重复创建
- upper layer 每层重复读、重复计数、重复拷贝
- header deleted 状态不是天然在 core header view 里，需要额外检查

## 5. DuckDB `PinNode` 具体成本

实现位置:
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:372)

### 5.1 每次 pin 都会做的主要动作

1. 获取 node pointer 和 node handle
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:382)
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:386)

2. 直接拿 storage header 指针
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:387)

3. 获取 vector handle
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:395)

4. 分配 token，并把 header 字段直接抄到 token
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:396)
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:399)

5. 从 `storage_header->GetLevel0Neighbors()` 直接读 level0 邻接表
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:409)

6. 如有 upper，直接通过 `upper_alloc->GetHandle(...)` 访问 upper block
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:421)
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:431)

7. 邻居翻译时做 `IndexPointer -> node_id` 映射
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:416)
- [vex_core_node_store_bridge.cpp](/Users/sunji/Work/VexDB-Lite/vexdb-duck/index/vex_core_node_store_bridge.cpp:435)

### 5.2 DuckDB 的关键特征

DuckDB 当前 `PinNode` 更像:
- “先拿 allocator handle，再从内存结构中直接读字段和邻接数据”

虽然 Duck 也会:
- 建 token
- 拷贝邻居
- 做 pointer/node_id 翻译

但它少了 PG 最重的部分:
- 没有 `disk_container get_n<ReadLock>` 这种存储读取路径
- 没有每次构造 `base_raw` / `upper_raw` 再二次 copy
- header 更接近底层原生结构，不需要像 PG 那样动态补齐 deleted/header 状态

## 6. 差异对照表

| 项目 | PG live binding | DuckDB live binding | 对性能影响 |
|---|---|---|---|
| token 分配 | 有 | 有 | 两边都有，不是决定性差异 |
| 向量访问 | `pin_vector_buffer` | `vector_alloc->GetHandle` | 两边都有，Duck 更接近内存句柄 |
| header 来源 | cache + 默认回填 + deleted 检查 + 回调 | storage header 直接读取 | PG 更重 |
| level0 读取 | `base_layer.get_n<ReadLock>` + `base_raw` + copy | `GetLevel0Neighbors()` 直接读 + copy | PG 更重 |
| upper 读取 | 每层 `upper_layer.get_n<ReadLock>` + `upper_raw` + copy | `upper_alloc->GetHandle` 后直接读 + copy | PG 更重 |
| metadata | `vector<uint8_t>` 临时缓冲 | 直接拿 meta handle 指针 | PG 更重 |
| deleted 状态 | 额外 `get_itempointer` 判定 | header 直接带状态 | PG 更重 |
| `UnpinNode` 只读成本 | `delete token` + `vector_buf.release()` | `delete token` | PG 略重 |

## 7. 为什么 DuckDB 构建快得多

原因不是:
- DuckDB 没有大量读访问

真正原因是:
- HNSW 构建本来就会产生大量 `PinNode`
- DuckDB 单次 `PinNode` 足够轻，主要是 allocator handle 级别访问
- PG 单次 `PinNode` 太胖，包含读取、计数、翻译、临时容器分配和多次拷贝

因此:
- 相同量级的逻辑读访问，DuckDB 可以承受
- PG 会在 `SearchLayer / SelectNeighbors / backlink prune` 的高频循环里被放大成主要瓶颈

## 8. 当前性能结论

结合 PG profiling:
- `pin_read_calls=6836879`
- `pin_read_ms=52456.221`
- 平均每次只读 pin 约 `7.7us`

这说明:
- 单次 pin 不是“离谱地慢”
- 但它远远不够轻
- 在几百万次访问下，累计成本已经吞掉大部分构建时间

换言之:
- 当前瓶颈不是“读访问次数异常”
- 而是“PG 只读 pin 的单位成本不适合 HNSW 这种高读放大算法”

## 9. 优化优先级建议

### 9.1 第一优先级

收缩 PG 只读 `PinNode`

目标:
- 让只读 pin 更接近 Duck 的 handle/view 模式

优先项:
- 去掉每次 pin 的 `base_raw` / `upper_raw` 临时容器
- 尽量直接暴露底层邻接 view，而不是重建完整副本
- 缩小 header materialize 范围
- 能直接取指针的地方不再走额外 copy

### 9.2 第二优先级

减少重复读装配

可选方向:
- 热路径 scratch 复用
- 只读 handle 复用
- neighbor view 的轻量缓存

### 9.3 暂不优先

不建议先做:
- 修改 HNSW 算法流程
- 调整 graph_index 设计
- 引入全量快照/序列化传输

原因:
- 当前问题已经明确在适配层单位访问成本

## 10. 后续验证建议

下一批优化后，至少保留以下验证:
- DuckDB 功能测试
- PG smoke 功能测试
- PG `1k` SIFT benchmark
- 比较以下指标是否下降:
  - `pin_read_ms`
  - `backlink_ms`
  - `prune_ms`
  - `build_ms`
