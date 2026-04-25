# Multi-Backend NodeStore Direct Binding Design

**Date:** 2026-04-21  
**Status:** Draft (for review and baseline)

## 1. 背景与问题

当前 `libvex-core` 已具备 `MemoryNodeStore + HNSWGraph` 运行能力，并实现了 `serialize/deserialize` 快照链路。  
该链路有两个结构性问题：

1. 适配层与 core 通过 blob 传递，存在额外内存拷贝。
2. 默认假设索引可整体驻留内存，不适合大索引场景。

目标架构是多后端（DuckDB / PostgreSQL / SQLite）统一算法核心，适配层应可直接对接底层存储接口，而不是先完整反序列化到内存再查询。

## 2. 设计目标

1. 保持 `graph_index` 算法设计不变：
   - 不修改搜索/插入/剪枝策略。
   - 不引入新 ANN 策略。
2. Core 与存储解耦：
   - `HNSWGraph` 仅依赖 `NodeStore` 抽象。
   - 允许不同后端通过各自 `NodeStore` 实现直接访问底层存储。
3. 降低内存峰值与拷贝：
   - 支持按需 pin/read，而非全量加载。
4. 保留快照能力但降级用途：
   - 用于 debug/export/import/offline tool。
   - 不作为线上主查询路径。

## 3. 非目标

1. 不在本阶段优化算法 recall/QPS 策略本身。
2. 不在本阶段定义跨版本二进制快照兼容矩阵。
3. 不在本阶段引入共享内存图结构（DSM）或跨进程统一缓存。

## 4. 方案总览

从“快照传输主路径”调整为“NodeStore 直连主路径”：

1. 适配层实现 `NodeStore`：
   - `DuckDBNodeStore`
   - `PGNodeStore`
   - `SQLiteNodeStore`
2. `HNSWGraph` 通过 `NodeStore` 进行节点访问：
   - 查询和插入仅调用抽象接口。
3. 元数据（entry point / max level / node count / config）由后端元页管理。
4. 快照 API 保留为辅路径。

## 5. NodeStore 契约修订

为避免裸指针长期持有导致后端 buffer 生命周期问题，`NodeStore` 从“直接返回内存地址”演进为“Pin 句柄模型”。

### 5.1 核心抽象

```cpp
class NodeHandle {
public:
    virtual ~NodeHandle() = default;

    virtual const NodeHeader* Header() const = 0;
    virtual const float* Vector() const = 0;

    virtual const node_id_t* Level0Neighbors() const = 0;
    virtual uint16_t Level0Count() const = 0;

    virtual const node_id_t* UpperNeighbors(int level_idx) const = 0;
    virtual uint16_t UpperCount(int level_idx) const = 0;

    virtual const uint8_t* Metadata() const = 0;
};

class MutableNodeHandle : public NodeHandle {
public:
    virtual NodeHeader* MutableHeader() = 0;
    virtual node_id_t* MutableLevel0Neighbors() = 0;
    virtual void SetLevel0Count(uint16_t count) = 0;

    virtual node_id_t* MutableUpperNeighbors(int level_idx) = 0;
    virtual void SetUpperCount(int level_idx, uint16_t count) = 0;
};

class NodeStore {
public:
    virtual ~NodeStore() = default;

    virtual node_id_t AllocateNode(row_id_t row_id, const float* vec, uint32_t dim, uint8_t level) = 0;
    virtual void FreeNode(node_id_t node_id) = 0;

    virtual std::unique_ptr<NodeHandle> PinNode(node_id_t node_id) const = 0;
    virtual std::unique_ptr<MutableNodeHandle> PinNodeForUpdate(node_id_t node_id) = 0;

    virtual uint32_t GetDimension() const = 0;
    virtual int GetM() const = 0;
    virtual uint64_t GetNodeCount() const = 0;

    virtual void ForEachNode(std::function<void(node_id_t)> cb) const = 0;
};
```

### 5.2 约束

1. Handle 生命周期内，返回指针必须有效。
2. Handle 析构时自动 unpin/release。
3. 算法层不持久化 handle 外部指针。

## 6. 核心算法层影响

`HNSWGraph` 只做访问方式替换，不改算法行为：

1. `SearchLayer`: `GetHeader/GetVector/GetNeighbors` -> `PinNode` 读取。
2. `InsertNode`: 更新邻接时使用 `PinNodeForUpdate`。
3. `Search/BruteForce`: 遍历时按需 pin。

行为保持：

1. 层间入口下降策略保持不变。
2. SelectNeighbors 启发式保持不变。
3. 邻接修剪策略保持不变。

## 7. 适配层设计

### 7.1 DuckDBNodeStore

1. 基于 DuckDB BlockManager / BufferManager。
2. `PinNode` 内部获取 buffer handle。
3. 向量、level0、upper 可布局在同页或多页；由适配层决定。

### 7.2 PGNodeStore

1. 基于 shared buffer + page layout。
2. `PinNode` 通过 buffer pin 与 lightweight lock 管理可见性。
3. WAL/redo 仍由 PG 侧写路径负责。

### 7.3 SQLiteNodeStore

1. 基于 pager/btree shadow table。
2. 可选 mmap + 页缓存。
3. 事务语义由 SQLite 虚表层保证。

## 8. 元数据管理

元数据不通过快照外传，主路径由后端管理：

1. index config: `dim, m, ef_construction, metric`
2. graph state: `has_entry_point, entry_point, max_level, node_count`
3. format/version: 每后端有自己的 schema version

core 只接收这些值并驱动算法，不管理持久化细节。

## 9. 快照角色调整

快照保留但降级：

1. 用途：
   - offline export/import
   - debug dump
   - 回归测试基线
2. 非用途：
   - 线上主查询加载路径

## 10. 迁移计划（分批）

### Phase A: 接口改造（不改算法策略）

1. 引入 `NodeHandle/MutableNodeHandle`。
2. `MemoryNodeStore` 适配新接口。
3. `HNSWGraph` 切换为 handle 访问。

### Phase B: DuckDB 直连

1. 新增 `DuckDBNodeStore`。
2. 仅改适配层接线，算法保持不变。
3. 引入对照测试：`MemoryNodeStore` vs `DuckDBNodeStore` 结果一致。

### Phase C: PG/SQLite 直连

1. 新增 `PGNodeStore` / `SQLiteNodeStore`。
2. 各后端完成元数据页 + 邻接/向量页映射。

### Phase D: 清理与收口

1. 快照 API 标注非主路径。
2. 性能回归：拷贝次数、RSS、QPS。

## 11. 验证标准

1. 正确性：
   - 相同数据下，`MemoryNodeStore` 与后端 `NodeStore` TopK 结果一致。
2. 性能：
   - 不出现“整索引反序列化”步骤。
   - 大索引场景 RSS 显著低于全量加载版本。
3. 稳定性：
   - Handle 生命周期内无悬垂指针。
   - 并发查询无 buffer pin 泄漏。

## 12. 风险与缓解

1. 风险：handle 频繁创建销毁导致开销上升。  
   缓解：引入轻量对象池或 small-object 优化（后续优化项）。
2. 风险：不同后端页面布局差异导致实现复杂。  
   缓解：统一最小可用契约，后端内部自定义布局。
3. 风险：写路径锁粒度不当引发性能抖动。  
   缓解：先保证正确性，再按热点路径做细粒度优化。

## 13. 当前决议

1. 主路径采用 NodeStore 直连底层存储。
2. 快照不再作为 core-adapter 主传输协议。
3. 算法设计保持冻结，仅做访问层重构。

## 14. 2026-04-21 执行快照（第 2 批后）

### 14.1 已落地

1. `NodeStore` 已切换为 Pin/Handle 契约，`HNSWGraph` 通过 handle 访问节点（不改算法策略）。
2. DuckDB 与 PG 在 `libvex-core` 内采用统一适配抽象：
   - 公共适配层：`vex_adapter_node_store_common.hpp`
   - 公共 mock 直连 binding：`vex_adapter_low_level_binding_mock.hpp/.cpp`
   - DuckDB/PG 仅保留薄别名与配置差异字段。
3. 新增 PG direct backend smoke test，且与 DuckDB direct/memory 路径共同通过。
4. DuckDB 扩展侧新增 `libvex-core` 直连 bridge skeleton（编译开关控制，默认关闭）。
5. PG 扩展侧新增 `libvex-core` 直连 bridge skeleton（CMake/PGXS 开关控制，默认关闭）。

### 14.2 当前状态

1. 主干运行路径仍为现有适配实现；bridge skeleton 不改变线上行为。
2. 代码库中已形成“统一契约 + 后端薄桥接”的最小闭环，可在后续批次逐步接入真实页布局/元页。
3. 快照能力继续保留为辅路径，不作为主查询载体。

### 14.3 下一批建议（不改算法）

1. 在 DuckDB bridge 内将 `load/store_graph_state_cb` 接到真实元页读写。
2. 在 PG bridge 内将 `load/store_graph_state_cb` 接到 metapage 读写。
3. 完成 graph state 持久化后，再逐步替换节点 pin/unpin 映射路径。

## 15. 2026-04-22 执行快照（第 3 批）

### 15.1 已落地

1. DuckDB bridge 支持 graph state 回调化接口：
   - `DuckDBCoreBindingConfig` 新增 `load_graph_state_cb/store_graph_state_cb`。
   - `LoadGraphState/StoreGraphState` 优先走回调，失败时返回 false。
   - 未配置回调时保持内存 fallback（不影响现有 skeleton 行为）。
2. PG bridge 同步支持 graph state 回调化接口：
   - `PgCoreBindingConfig` 新增 `load_graph_state_cb/store_graph_state_cb`。
   - `LoadGraphState/StoreGraphState` 逻辑与 DuckDB 保持一致。
3. `libvex-core` 新增回调路径 smoke test：
   - `adapter_graph_state_callback_smoke.cpp`
   - 验证 `LoadGraphState/StoreGraphState` 契约行为。

### 15.2 验证

1. `libvex-core` 全部既有测试 + 新增测试通过。
2. `vexdb-pg` 在 bridge 开关开启下完成 CMake configure（`pg_config` 自动发现路径生效）。

### 15.3 下一批建议（不改算法）

1. 先在 DuckDB 侧实现最小 metapage 回调（读写 `entry/max_level/node_count`）。
2. 再在 PG 侧实现同构 metapage 回调。
3. 两侧完成后增加“重启后状态恢复”对照测试，再进入节点 pin/unpin 的真实页映射。

## 16. 2026-04-22 执行快照（第 4 批）

### 16.1 已落地

1. DuckDB bridge 新增“最小 graph state 持久化映射”：
   - `DuckDBCoreBindingConfig` 增加 `storage_options`（指向 `IndexStorageInfo.options`）。
   - 新增 `LoadDuckDBGraphStateFromOptions/StoreDuckDBGraphStateToOptions`。
   - bridge 构造时若提供 `storage_options` 且未显式传 callback，则自动绑定到 options 读写。
2. PG bridge 新增“最小 metapage graph state 读写”：
   - `PgCoreBindingConfig` 增加 `metablkno`（默认 `GRAPH_INDEX_METAPAGE_BLKNO`）。
   - 新增 `LoadPgGraphStateFromMetaPage/StorePgGraphStateToMetaPage`。
   - bridge 构造时若提供 `index_rel` 且未显式传 callback，则自动绑定到 metapage 读写。
3. 上述改动均保持 callback 优先策略：
   - 显式 callback > 自动绑定 > 内存 fallback。

### 16.2 验证

1. `libvex-core` 全量构建和现有测试通过。
2. `vexdb-pg` 在 bridge 开关开启下 CMake configure 通过（`pg_config` 自动发现路径有效）。

### 16.3 下一批建议（不改算法）

1. DuckDB 侧将 `storage_options` 自动绑定接入真实 `GraphIndex` 生命周期（build/deserialize/flush）。
2. PG 侧对 metapage 写路径补 WAL/redo 对齐点（当前为最小写回骨架）。
3. 增加“状态写入 -> 重新加载 -> 状态一致”的适配层回归测试。

## 17. 2026-04-22 执行快照（第 5 批）

### 17.1 已落地

1. DuckDB `GraphIndex` 生命周期已接入 bridge graph-state helper：
   - `SerializeToDisk/SerializeToWAL` 在写入 options 时同步调用 `StoreDuckDBGraphStateToOptions`。
   - `DeserializeFromStorage` 优先调用 `LoadDuckDBGraphStateFromOptions`，再回退旧字段路径。
2. PG 侧增加了 bridge graph-state 的可观测读取接点：
   - `index_inspect` 在 bridge 开关开启时输出 `Core Bridge EntryPoint/MaxLevel/NodeCount`。

### 17.2 验证

1. `libvex-core` 构建 + 全测试通过。
2. `vexdb-pg` bridge 开关开启下 CMake configure 通过。
3. DuckDB extension 因本地未提供 `DUCKDB_SOURCE_DIR`，未完成该目标的编译验证。

### 17.3 下一批建议（不改算法）

1. 补 DuckDB extension 的 bridge-on 编译验证（提供有效 `DUCKDB_SOURCE_DIR`）。
2. PG metapage 写回补 WAL/redo 对齐逻辑。
3. 增加“写入 graph state -> 重启加载 -> 一致性”回归测试（DuckDB/PG 各一条）。

## 18. 2026-04-22 执行快照（第 6 批）

### 18.1 已落地

1. PG bridge metapage 写回已补 WAL 对齐：
   - `StorePgGraphStateToMetaPage` 在 `MarkBufferDirty` 后调用 `GraphIndexXlog`：
     - `update_num_vector`
     - `update_entry`
   - entry 相关字段同步写入：
     - `entrypoint_id`
     - `entry_cur_layer_idx`
     - `entry_level`
2. DuckDB bridge-on 编译验证已完成：
   - 在 DuckDB 1.6-dev 环境，`VEX_ENABLE_LIBVEX_CORE_BRIDGE=ON` 可编译通过。
   - 由于 DuckDB 1.6 优化器 API 差异，需 `VEX_ENABLE_OPTIMIZER=OFF` 才能隔离 bridge 验证。
3. DuckDB 编译兼容补丁（与 bridge 编译连带问题）：
   - 补 `ArrayVector` 头文件包含（`vector_functions/distance_functions/vex_physical_create_index/graph_index`）。
   - 修正 bridge 头里 `duckdb::vex` 与 `::vex` 命名空间混淆。

### 18.2 验证

1. `libvex-core` 构建与全部测试通过。
2. `vexdb-pg` bridge 开关开启下 CMake configure 通过。
3. DuckDB bridge-on 构建通过（no-optimizer 配置）：
   - `cmake -S . -B build/duckdb-bridge-verify-noopt -DDUCKDB_SOURCE_DIR=/Users/sunji/Work/duckdb -DVEX_ENABLE_LIBVEX_CORE_BRIDGE=ON -DVEX_ENABLE_OPTIMIZER=OFF`
   - `cmake --build build/duckdb-bridge-verify-noopt --target vex_loadable_extension -j4`

### 18.3 下一批建议（不改算法）

1. 加入 graph-state 持久化回归测试（写入 -> reload -> 一致性）：
   - DuckDB：`IndexStorageInfo.options` 路径
   - PG：metapage 路径
2. PG 侧补 `redo_meta` 最小实现，覆盖 `UPDATE_NUM_VECTOR/UPDATE_ENTRY_POINT`。
3. 完成后进入节点 pin/unpin 的后端真实页映射。

## 19. 2026-04-22 执行快照（第 7 批）

### 19.1 设计约束再次确认

1. `graph_index` 算法设计继续冻结：
   - 不修改搜索、插入、邻接剪枝与层间遍历策略。
2. `MemoryNodeStore` 允许在适配层有不同实现并直接绑定底层存储：
   - 主路径禁止通过“整索引序列化/反序列化”在 core 与适配层传输。
3. 快照定位保持为辅路径：
   - 仅用于 debug/export/import/offline 基线，不承担线上主查询加载。

### 19.2 本批已落地

1. PG WAL `redo_meta` 路径修正为按 block payload 解析：
   - 从 `XLogRecGetData` 切换为 `XLogRecGetBlockData(record, 0, ...)`。
   - 与 `XLogRegisterBufData` 的写入路径语义对齐，避免恢复阶段读取主记录区导致的数据错位。
2. `redo_meta` 增加最小长度保护：
   - 对 `UPDATE_NUM_VECTOR/UPDATE_ENTRY_POINT/UPDATE_VACUUM_FLAG` 分支分别校验 payload 长度后再应用。
   - 维持“最小可恢复语义”，避免异常记录触发越界读取。

### 19.3 当前验证状态

1. `libvex-core` 重新构建并通过全量测试：
   - `vex_core_capi_roundtrip_test`
   - `vex_core_duckdb_stub_store_smoke_test`
   - `vex_core_duckdb_direct_backend_smoke_test`
   - `vex_core_pg_direct_backend_smoke_test`
   - `vex_core_adapter_graph_state_callback_smoke_test`
2. `vexdb-pg` bridge-on 全量编译在当前环境仍被外部依赖阻塞：
   - 缺少 Boost 头（`boost/preprocessor/seq.hpp`）。
   - 存在与当前 PG 头版本相关的 `PageSetChecksum` 接口差异。
   - 该阻塞与本批 `redo_meta` 修正本身无耦合。

### 19.4 下一批建议（不改算法）

1. 增加 graph-state 持久化最小回归（主目标）：
   - DuckDB：options 写入后重建并读取一致。
   - PG：metapage 写入后读取一致（可先在 bridge helper 层完成闭环）。
2. 在依赖可用后补 PG bridge-on 全量编译验证，确认 WAL 回放路径无编译回归。
3. 进入节点 pin/unpin 与真实页布局映射前，先收口 graph-state 回归用例。
