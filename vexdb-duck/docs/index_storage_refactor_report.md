# Index Storage Refactor Report

## 概述

将 VexDB 向量索引的存储层从堆分配 (`std::vector<unique_ptr<GraphNode>>`) 迁移到 DuckDB 原生 `FixedSizeAllocator` 基础设施，实现了磁盘块级别的持久化和按需加载。

涉及两种索引：
- **GraphIndex** (HNSW 单图)
- **HybridIndex** (分区向量索引 + 标量过滤)

---

## 1. 架构变更

### 1.1 旧方案 (heap-based)

```
GraphNode {
    row_id, level, deleted, vector<float>, vector<vector<GraphNode*>> neighbors
}
std::vector<unique_ptr<GraphNode>> nodes_;
```

- 所有节点数据常驻内存
- 序列化：全量遍历构建 BLOB (`SerializeToBlob`)
- 反序列化：两遍解析 — 分配节点 + 修复邻居指针
- 持久化开销：O(N*M) 序列化 + O(N*M) 反序列化

### 1.2 新方案 (FixedSizeAllocator-based)

```
3-Allocator 设计:
  Allocator 0 (NODE):  HNSWNodeHeader(32B) + IndexPointer level0_neighbors[M*2]
  Allocator 1 (VECTOR): float data[dim]
  Allocator 2 (UPPER):  HNSWUpperLevel(16B) + IndexPointer upper_neighbors[MAX_LEVELS*M]
```

- 数据存储在 DuckDB 磁盘块中，由 buffer pool 按需换入换出
- 序列化：调用 `SerializeBuffers()` 直接写入磁盘块 — O(pages)
- 反序列化：调用 `Init(allocator_info)` 懒加载 — O(1) 启动
- `row_id_map` (unordered_map) 仍在内存，用于 O(1) 节点查找

### 1.3 关键数据结构

| 结构 | 大小 | 说明 |
|------|------|------|
| `HNSWNodeHeader` | 32B | row_id, level, deleted, vector_ptr, upper_ptr |
| `IndexPointer` | 8B | packed (buffer_id + offset) |
| NODE segment | 32 + M*2*8 B | 头 + level-0 邻居 (M=16 → 288B) |
| VECTOR segment | dim * 4 B | 原始浮点向量 (64d → 256B) |
| UPPER segment | 16 + MAX_LEVELS*M*8 B | 上层邻居 (稀疏，仅 ~6% 节点使用) |

---

## 2. HybridIndex 序列化迁移

### 2.1 SerializeToDisk

```
options: m, ef_construction, dimension, num_partitions
partition_meta BLOB: [key_len, key, node_count, max_level, entry_point,
                      num_row_entries, [row_id, IndexPointer]*] × N
allocator_infos: num_partitions × 3 (NODE, VECTOR, UPPER per partition)
```

- 每个分区的 3 个 allocator 通过 `SerializeBuffers()` 写入磁盘块
- `row_id_map` 序列化为 partition_meta 的一部分，确保 restart 后 Delete 可用

### 2.2 SerializeToWAL

- 同 SerializeToDisk，使用 `InitSerializationToWAL()` 获取 buffer data
- `buffers` 数组包含 num_partitions × 3 个 buffer 引用

### 2.3 DeserializeFromStorage

- 从 options 读取元数据
- 解析 partition_meta BLOB 恢复分区结构
- 对每个分区：`InitAllocators()` → `Init(allocator_info)` 实现懒加载
- 恢复 `row_id_map` 和 `row_partition_map_` 确保 Delete 正确性

### 2.4 向后兼容

- `Create()` 优先检测 allocator-based 存储 (`options.count("dimension")`)
- 否则回退到 BLOB 反序列化 (`options.find("hybrid_data")`)
- 旧 BLOB 序列化/反序列化代码保留

---

## 3. 性能对比

### 3.1 内存占用 (M=16, 64维)

| 组件 | 旧方案 (heap) | 新方案 (allocator) |
|------|-------------|-------------------|
| 向量数据 | N × 256B (堆上) | 磁盘块，按需加载 |
| 节点头+邻居 | N × ~300B (堆上) | 磁盘块，按需加载 |
| `row_id_map` | N × 16B | N × 16B (**仍在内存**) |
| 指针数组 | N × 8B | 无 |
| **总计** | **~580B/行 全在内存** | **~16B/行 内存 + 磁盘块按需** |

### 3.2 GraphIndex 基准测试 (10K rows, 64维)

| 指标 | 结果 |
|------|------|
| Build (CREATE INDEX) | ~2.3s |
| Search (top-10) | ~3-7ms |

### 3.3 HybridIndex 基准测试 (10K rows, 64维, 5分区)

| 指标 | 结果 |
|------|------|
| Build (CREATE INDEX) | ~100ms |
| Filtered Search (单分区 2K 行) | ~1-2ms |
| Global Search (全部 5 分区) | ~3ms |

### 3.4 大规模测试

| 规模 | 维度 | Build 时间 | Search 延迟 | 内存 |
|------|------|-----------|------------|------|
| 200K | 64d | 27s | 32ms | 56 MiB |
| 500K | 64d | 29s | 33ms | 140 MiB |
| **1M** | 32d | 33s | **23ms** | **150 MiB** |
| 500K 持久化重启后 | 32d | - | 30ms | **53 MiB** |

### 3.5 数据量上限

- **旧方案**：受限于物理内存，~580B/行 → 8GB 内存约 1400 万行 (64d)
- **新方案**：`row_id_map` ~16B/行 → 8GB 内存约 5 亿行理论上限，实际瓶颈是 HNSW 构建时间 O(N·log(N))
- **无硬编码限制**：已验证 1M 行无任何问题

---

## 4. 修复的关键 Bug

| Bug | 根因 | 修复 |
|-----|------|------|
| SIGSEGV (PQ delete) | Delete Phase 4 在图不一致状态下调用 SearchLayer | 移除 Phase 4，采用 mark-delete |
| SIGFPE (空表 CREATE INDEX) | `EnsureAllocators()` 在 dimension=0 时创建 segment_size=0 的 allocator | 三重守卫：检查 dimension key、require dimension>0、Build 时检测维度 |
| `unsafe_optional_ptr` 编译错误 | `Get<T>()` 返回 `unsafe_optional_ptr`，不能隐式转 `T*` | 使用非模板 `Get()` + `reinterpret_cast` |
| C++17 结构化绑定 | DuckDB 使用 `-std=c++11` | 替换为 `auto &kv` + `.first`/`.second` |
| `SerializeToBlob() const` | allocator `Get()` 设置 dirty flag | 移除 `const` 限定 |

---

## 5. 修改文件清单

| 文件 | 变更 |
|------|------|
| `include/vex_hnsw_node.hpp` | 新增：allocator 段布局定义 |
| `include/vex_graph_index_core.hpp` | 重写：GraphIndexCore 使用 IndexPointer + FixedSizeAllocator |
| `index/graph_index_core.cpp` | 重写：核心算法 (SearchLayer, InsertNode, BruteForceSearch, PQ) |
| `index/graph_index.cpp` | 重写：SerializeToDisk/WAL, DeserializeFromStorage, Delete |
| `include/vex_hybrid_index.hpp` | 新增：DeserializeFromStorage, EnsurePartitionAllocators |
| `index/hybrid_index.cpp` | 重写：Build/Delete/Merge/Serialize 全部使用 allocator API |

---

## 6. 测试覆盖

### 6.1 测试统计

- **总计**: 60 个测试用例，1888 assertions，全部通过
- **types**: 2 tests
- **functions**: 6 tests
- **index**: 52 tests (含 stability, fuzz, persistence, transactions, restart)

### 6.2 HybridIndex 专项测试

| 测试文件 | 场景 | assertions |
|----------|------|------------|
| `hybrid_index_basic.test` | 基础 CRUD，filtered/global search | 8 |
| `hybrid_index_delete.test` | 单行/整分区删除，删后插入 | 12 |
| `hybrid_index_multipartition.test` | 6 分区操作 | 15 |
| `hybrid_index_restart_search.test` | checkpoint + restart 基础 | 4 |
| `hybrid_index_stress.test` | 1000 行 5 分区压力测试 | 14 |
| `hybrid_index_heavy_delete.test_slow` | 800 行多轮删除/更新 | 28 |
| `hybrid_index_persistence.test` | 基础持久化 | 4 |
| `hybrid_index_persistence.test_slow` | 8 阶段完整持久化 (含 WAL replay) | 49 |
| `hybrid_index_allocator_storage.test_slow` | **新增** allocator 序列化专项 | 82 |

### 6.3 新增测试覆盖 (allocator_storage)

| 测试 | 场景 | 验证要点 |
|------|------|----------|
| Test 1 | restart 后 DELETE | `row_id_map`/`row_partition_map_` 从存储正确恢复 |
| Test 2 | restart 后 INSERT + 再 restart | allocator 增量写入在反序列化后正常 |
| Test 3 | 空表 HYBRID_INDEX + restart | dimension=0 不触发异常 |
| Test 4 | 4 次 checkpoint-restart 循环 | 序列化/反序列化幂等性 |
| Test 5 | 50 个分区 (150 allocator_infos) | 大量分区序列化正确性 |

### 6.4 稳定性验证

- 全量测试连续 3 次运行：全部通过
- HybridIndex persistence 测试连续 3 次运行：全部通过
- 新增 allocator_storage 测试连续运行：全部通过
