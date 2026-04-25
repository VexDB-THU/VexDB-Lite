# VexDB-Lite 多数据库后端架构 - 实施计划

## Context

VexDB-Lite 当前是基于 DuckDB v1.5.0 fork 的 HNSW 向量搜索扩展（~10,200 行）。为覆盖移动端/嵌入式（SQLite）、桌面分析（DuckDB）、服务端/云（PostgreSQL）三个部署场景，需要将核心算法提取为独立库 `libvex-core`，并为三个数据库分别实现薄适配层。

核心问题：当前代码与 DuckDB 深度耦合——FixedSizeAllocator（内存管理）、IndexPointer（节点寻址）、BoundIndex（索引生命周期）、OptimizerExtension（查询重写）贯穿全部代码。直接迁移不可行，必须先解耦。

经陪审团评审（6 票有条件通过）和技术调研（pgvector/sqlite-vec 参考实现），确认独立核心库是唯一可行架构。

---

## 架构总览

```
┌─────────────────────────────────────────────┐
│          libvex-core (静态库 .a)              │
│  Layer 0: 纯算法（零 DB 依赖）               │
│  ├── HNSW: SearchLayer/InsertNode/SelectNeighbors │
│  ├── SIMD 距离: SSE/AVX2/NEON/WASM           │
│  ├── PQ 量化: 编码/解码/训练                  │
│  ├── 并发: SimpleRWLock/SpinLock/VisitedSet   │
│  └── 过滤: EqualityFilter/RangeFilter/InList  │
│                                              │
│  Layer 1: 运行时                              │
│  ├── NodeStore 接口 (抽象类)                  │
│  ├── MemoryNodeStore (默认实现, flat array)    │
│  ├── 序列化/反序列化 (二进制 BLOB)            │
│  └── C API (extern "C")                      │
├──────────┬──────────┬────────────────────────┤
│  DuckDB  │  SQLite  │  PostgreSQL            │
│  Adapter │  Adapter │  Adapter               │
│ ~600 行  │ ~800 行  │  ~1200 行              │
└──────────┴──────────┴────────────────────────┘
```

---

## 核心接口设计

### NodeStore 接口（替代 FixedSizeAllocator + IndexPointer）

关键决策：`IndexPointer`（64位 buffer_id+offset 编码）→ `uint32_t node_id`（数组索引）

```cpp
// libvex-core/include/vex/vex_node_store.hpp
namespace vex {

class NodeStore {
public:
    virtual ~NodeStore() = default;

    // 节点生命周期
    virtual uint32_t AllocateNode(int64_t row_id, const float* vec,
                                   uint32_t dim, uint8_t level) = 0;
    virtual void FreeNode(uint32_t node_id) = 0;

    // 搜索热路径（必须 O(1)）
    virtual const float* GetVector(uint32_t node_id) = 0;
    virtual uint32_t* GetLevel0Neighbors(uint32_t node_id) = 0;
    virtual uint16_t GetLevel0Count(uint32_t node_id) = 0;
    virtual void SetLevel0Count(uint32_t node_id, uint16_t count) = 0;

    // 节点头
    virtual NodeHeader* GetHeader(uint32_t node_id) = 0;

    // 上层邻居（仅 ~1/M 节点有 level>0）
    virtual uint32_t* GetUpperNeighbors(uint32_t node_id, int level_idx) = 0;
    virtual uint16_t GetUpperCount(uint32_t node_id, int level_idx) = 0;
    virtual void SetUpperCount(uint32_t node_id, int level_idx, uint16_t count) = 0;

    // 元数据（过滤搜索）
    virtual const uint8_t* GetMetadata(uint32_t node_id) = 0;

    // 配置
    virtual uint32_t GetDimension() const = 0;
    virtual int GetM() const = 0;
    virtual uint64_t GetNodeCount() const = 0;

    // 迭代
    virtual void ForEachNode(std::function<void(uint32_t)> cb) const = 0;

    // 并行构建（可选重写）
    virtual void PrepareParallelAccess() {}
    virtual void FinishParallelAccess() {}
};

} // namespace vex
```

### MemoryNodeStore（默认实现）

4 个 flat vector 存储所有数据，node_id 就是数组索引：

```
headers_[node_id]                     → NodeHeader
vectors_[node_id * dim ... +dim]      → float 向量
neighbors_l0_[node_id * M*2 ... +M*2] → level-0 邻居
upper_neighbors_[node_id]             → UpperBlock（按需分配）
```

`GetVector(id)` = `vectors_.data() + id * dim`，与现有 BufferPtrCache 的 `origin + offset * segment_size` 性能等价。

### C API

```c
// libvex-core/include/vex/vex_core.h
vex_index* vex_index_create(const vex_index_config_t* config);
void       vex_index_destroy(vex_index* idx);
int        vex_index_add(vex_index* idx, int64_t row_id, const float* vec, uint32_t dim);
int        vex_index_add_batch(vex_index* idx, const int64_t* row_ids,
                               const float* vecs, uint32_t dim, uint32_t count, int threads);
int        vex_index_search(vex_index* idx, const float* query, uint32_t dim,
                            uint32_t k, int ef, vex_result_t* results, uint32_t* count);
int        vex_index_serialize(vex_index* idx, void** data, size_t* size);
vex_index* vex_index_deserialize(const void* data, size_t size, const vex_index_config_t* cfg);
```

---

## 分阶段实施

### Phase 0: 构建骨架（1 周）

创建目录结构，不修改现有代码行为。

| 任务 | 产出 |
|------|------|
| 创建 `libvex-core/` 目录结构和 CMakeLists.txt | 静态库构建骨架 |
| 定义 `vex_types.h`（基础类型替代 DuckDB 的 idx_t/row_t/IndexPointer） | ~60 行 |
| 提取 `vex_config.hpp`（GraphIndexConfig 参数定义） | ~40 行 |
| `build.sh` 添加 `core` 构建目标 | ~30 行改动 |

### Phase 1: 零依赖模块迁移（2 周）

将无 DuckDB 依赖的模块搬入 libvex-core，现有扩展通过 include 引用。

| 模块 | 源 → 目标 | 行数 | 改动 |
|------|-----------|------|------|
| SIMD 距离 | `distance/distance.cpp` → `libvex-core/src/distance.cpp` | 705 | namespace 改 `vex` |
| PQ 量化 | `quantizer/product_quantizer.cpp` → `libvex-core/src/` | 335 | namespace 改 |
| 并发原语 | 从 `vex_graph_index_core.hpp` 提取 → `vex_concurrency.hpp` | 260 | 独立头文件 |
| 过滤谓词 | `vex_filter_predicate.hpp` → `vex_filter.hpp` | 248 | `LogicalTypeId` → `vex::TypeId` |
| 节点结构 | `vex_hnsw_node.hpp` → `vex_node.hpp` | 93 | `IndexPointer` → `uint32_t` |

**验证**：libvex-core.a 独立编译通过，零 DuckDB 头文件依赖。DuckDB 扩展通过 include 路径引用 libvex-core，所有现有测试继续通过。

### Phase 2: NodeStore + 算法重构（3 周）⭐ 最关键阶段

将 HNSW 算法从 FixedSizeAllocator 解耦为 NodeStore 接口调用。

| 任务 | 文件 | 行数 |
|------|------|------|
| NodeStore 接口定义 | `vex_node_store.hpp` | ~120 |
| MemoryNodeStore 实现 | `node_store_memory.cpp/hpp` | ~430 |
| HNSW 算法重构 | `graph_algo.cpp/hpp`（从 `graph_index_core.cpp` 重写） | ~1,650 |
| C API 实现 | `vex_core_capi.cpp` | ~250 |
| 核心测试 | `tests/test_*.cpp` | ~400 |

核心转换模式（`graph_index_core.cpp` 的 1,836 行逐行重构）：

```
旧: auto *header = GetNode(ptr);                    // FixedSizeAllocator
    float *vec = GetVector(header->vector_ptr);       // 二次间接
新: const float* vec = store->GetVector(node_id);     // 数组索引 O(1)

旧: IndexPointer node_ptr = node_alloc->New();       // 分配器
新: uint32_t node_id = store->AllocateNode(...);      // NodeStore

旧: visited.Insert(ptr.Get());                       // 64位 IndexPointer
新: visited.Insert(node_id);                          // 32位 node_id

旧: GraphCandidate{IndexPointer, float}
新: GraphCandidate{uint32_t node_id, float}
```

**验证**：libvex-core 独立 SIFT-1K 召回率测试通过（recall@10 ≥ 0.95）。C API 测试通过。MemoryNodeStore 的搜索 QPS 不低于现有 BufferPtrCache 路径的 85%。

### Phase 3: DuckDB 适配器改造（2 周）

现有 DuckDB 扩展改为使用 libvex-core。

| 任务 | 文件 | 变更 |
|------|------|------|
| GraphIndex 改造 | `graph_index.cpp/hpp` | `GraphIndexCore graph_` → `vex::HNSWGraph* graph_` + `vex::MemoryNodeStore* store_` |
| Serialize/Deserialize 重写 | `graph_index.cpp` 中的序列化部分 | MemoryNodeStore BLOB 序列化 |
| 优化器/函数适配 | `vex_optimizer.cpp`, `functions/*.cpp` | 类型引用更新 |
| 向后兼容 | `DeserializeFromStorage()` | 检测旧格式走 legacy 路径 |

**验证**：`bash build.sh test` 全绿。现有 `.duckdb` 文件通过 `REINDEX ALL` 迁移后正常工作。

### Phase 4: SQLite 适配器（3 周）

| 文件 | 行数 | 职责 |
|------|------|------|
| `vex_sqlite_module.c` | ~100 | `sqlite3_vex_init` 入口，注册模块和函数 |
| `vex_vtab.c` | ~500 | Virtual Table（xCreate/xConnect/xBestIndex/xFilter/xUpdate） |
| `vex_tvf.c` | ~250 | `vex_search()` Table-Valued Function |
| `vex_shadow.c` | ~200 | Shadow table（_config, _data）管理 |
| `vex_functions.c` | ~150 | `vex_vector()`/`vex_config()`/距离函数 |

SQL API 设计：
```sql
-- 创建索引（Virtual Table 管理向量数据）
CREATE VIRTUAL TABLE vec_items USING vex0(
    embedding FLOAT32[128], metric='l2', m=16
);
INSERT INTO vec_items(rowid, embedding) VALUES (1, X'...');

-- 搜索（TVF）
SELECT rowid, distance FROM vex_search('vec_items', ?query, 10);

-- 配置
SELECT vex_config('ef_search', 40);

-- 辅助
SELECT vex_vector_to_json(embedding) FROM vec_items;
```

存储策略：MemoryNodeStore + BLOB 序列化到 `_data` shadow table。iOS/Android 静态链接（`sqlite3_auto_extension`）。

**验证**：SQLite CLI 加载扩展→创建→插入→搜索。iOS Simulator + Android Emulator 交叉编译通过。

### Phase 5: PostgreSQL 适配器（4 周）

| 文件 | 行数 | 职责 |
|------|------|------|
| `vex_pg_module.c` | ~80 | `_PG_init` + `PG_MODULE_MAGIC` |
| `vex_type.c` | ~300 | `vector` 类型 I/O（text→binary→text） |
| `vex_operator.c` | ~200 | `<->`/`<=>`/`<~>` 距离运算符 |
| `vex_amhandler.c` | ~150 | `IndexAmRoutine` 注册，`amcanorderbyop=true` |
| `vex_ambuild.c` | ~350 | 索引构建（heap scan → HNSWGraph → 序列化到页面） |
| `vex_amscan.c` | ~250 | 索引扫描（反序列化 → search → 逐行返回） |
| `vex_amvacuum.c` | ~150 | bulk delete + vacuum cleanup |
| `vex--0.1.sql` | ~150 | CREATE TYPE/OPERATOR/OPCLASS/ACCESS METHOD |

SQL API 设计（与 pgvector 语法对齐）：
```sql
CREATE EXTENSION vex;
CREATE TABLE docs (id serial, embedding vector(128));
CREATE INDEX ON docs USING vex(embedding vex_l2_ops) WITH (m=16);
SELECT * FROM docs ORDER BY embedding <-> '[1,2,3,...]' LIMIT 10;
```

存储策略：MemoryNodeStore 序列化分割为 BLCKSZ 页面，通过 Generic WAL 保证崩溃安全。Per-process 反序列化缓存。

**验证**：`CREATE EXTENSION` → `CREATE INDEX USING vex` → `ORDER BY <-> LIMIT k` 正确走索引扫描。`pg_regress` 测试通过。

### Phase 6: 集成测试 + 优化（2 周）

- 三后端 SIFT-1K 召回率一致性验证
- ann-benchmarks 性能对比（使用 `/Users/Four/JobProjects/vexdb/ann-benchmark-main`）
- MemoryNodeStore prefetch 调优
- SQLite 懒加载（首次查询才反序列化）
- PG per-process 缓存优化

---

## 代码量总结

| 模块 | 行数 | 说明 |
|------|------|------|
| libvex-core | ~3,500 | 算法+内存实现+C API（一次写，三处用） |
| DuckDB adapter | ~600 | 从现有代码重构 |
| SQLite adapter | ~1,200 | 新增 |
| PG adapter | ~1,630 | 新增 |
| 测试 | ~800 | 新增核心测试 + 适配器测试 |
| **总计** | **~7,730** | vs 现有 ~10,200 只支持 DuckDB |

---

## 目录结构

```
vexdb_lite/
├── libvex-core/
│   ├── CMakeLists.txt
│   ├── include/vex/
│   │   ├── vex_core.h              # C 公共 API
│   │   ├── vex_types.h             # 基础类型
│   │   ├── vex_distance.h          # 距离 C API
│   │   ├── vex_node_store.hpp      # NodeStore 接口
│   │   ├── vex_node_store_memory.hpp
│   │   ├── vex_graph_algo.hpp      # HNSWGraph
│   │   ├── vex_distance.hpp
│   │   ├── vex_quantizer.hpp
│   │   ├── vex_filter.hpp
│   │   ├── vex_concurrency.hpp
│   │   └── vex_config.hpp
│   ├── src/
│   │   ├── distance.cpp
│   │   ├── product_quantizer.cpp
│   │   ├── graph_algo.cpp          # HNSW 核心（从 graph_index_core.cpp 重构）
│   │   ├── node_store_memory.cpp
│   │   └── vex_core_capi.cpp
│   └── tests/
├── adapters/
│   ├── duckdb/                     # 现有扩展改造
│   │   ├── include/ + src/         # graph_index, optimizer, functions
│   │   └── CMakeLists.txt
│   ├── sqlite/
│   │   ├── include/ + src/         # vtab, tvf, shadow, functions
│   │   └── CMakeLists.txt
│   └── postgresql/
│       ├── src/                    # type, operator, amhandler, build, scan
│       ├── sql/vex--0.1.sql
│       └── Makefile (PGXS)
├── duckdb/                         # 现有 DuckDB fork（保留）
├── test/                           # 现有测试 + 新增
└── build.sh                        # 扩展支持 core/sqlite/pg 目标
```

---

## 关键风险与缓解

| 风险 | 级别 | 缓解 |
|------|------|------|
| NodeStore 虚函数开销导致搜索性能下降 | 高 | 模板化搜索路径 `SearchLayer<MemoryNodeStore>()` 编译期去虚化；阈值：QPS 下降 >15% 则切换 |
| PG 多进程模型下 per-process 反序列化内存占用大 | 高 | 初版接受（服务器内存充裕），后续可改 DSM 共享 |
| SQLite xRollback 需撤销索引修改 | 中 | xBegin 创建 MemoryNodeStore 快照，xRollback 恢复 |
| DuckDB 现有 .duckdb 文件序列化格式不兼容 | 中 | DeserializeFromStorage 检测旧格式走 legacy 路径 + REINDEX 迁移 |
| graph_index_core.cpp 重构引入算法 bug | 中 | SIFT-1K recall 回归测试 + 逐函数对比验证 |

---

## 时间线

| 阶段 | 周数 | 里程碑 |
|------|------|--------|
| Phase 0: 构建骨架 | 1 | libvex-core 目录和 CMake 就绪 |
| Phase 1: 零依赖迁移 | 2 | distance/PQ/concurrency/filter 独立编译 |
| Phase 2: NodeStore + 算法重构 | 3 | libvex-core.a 独立工作，C API 可用 |
| Phase 3: DuckDB adapter | 2 | DuckDB 扩展全测试通过 |
| Phase 4: SQLite adapter | 3 | 移动端可用 |
| Phase 5: PG adapter | 4 | 服务端可用 |
| Phase 6: 集成 + 优化 | 2 | 三后端性能对齐 |
| **总计** | **17** | |

Phase 0-3（8 周）是核心路径。Phase 4 和 5 可部分并行。

---

## 验证方案

1. **每阶段验证**：现有 `bash build.sh test` 始终全绿（不破坏 DuckDB 功能）
2. **核心库验证**：libvex-core 独立 SIFT-1K recall@10 ≥ 0.95
3. **性能验证**：MemoryNodeStore 搜索 QPS ≥ 现有 BufferPtrCache 的 85%
4. **跨后端一致性**：同一数据集三个后端 recall 差异 < 1%
5. **移动端验证**：iOS Simulator + Android Emulator 编译运行通过
6. **PG 验证**：`pg_regress` 标准回归测试通过

## Critical Files

- `duckdb/extension/vexdb-duck/index/graph_index_core.cpp` (1,836 行) — 算法核心，需逐行重构为 NodeStore 调用
- `duckdb/extension/vexdb-duck/include/vex_graph_index_core.hpp` (607 行) — 类定义+并发原语，需拆分
- `duckdb/extension/vexdb-duck/distance/distance.cpp` (647 行) — SIMD，零改动迁移
- `duckdb/extension/vexdb-duck/index/graph_index.cpp` (1,763 行) — DuckDB 适配层主体，需改造
- `duckdb/extension/vexdb-duck/include/vex_hnsw_node.hpp` (93 行) — 节点结构，需去 IndexPointer
- `duckdb/extension/vexdb-duck/include/vex_filter_predicate.hpp` (248 行) — 过滤，需去 LogicalTypeId
- `duckdb/extension/vexdb-duck/quantizer/product_quantizer.cpp` (335 行) — PQ，零改动迁移
