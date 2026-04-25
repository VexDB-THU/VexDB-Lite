# Multi Backend Architecture Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将 VexDB-Lite 从单一 DuckDB 深耦合实现演进到 `libvex-core + adapter` 多后端架构，首阶段完成共享核心骨架与基础模块抽离。

**Architecture:** 核心算法与通用模块进入 `libvex-core`（零 DB 依赖），DuckDB/PG 保持现有行为作为适配层；先完成 Phase 0/1（骨架+无依赖模块），再推进 Phase 2（NodeStore + 算法重构），最后改造各适配器。

**Tech Stack:** C++17, CMake, DuckDB extension API, PostgreSQL extension API, HNSW, SIMD distance, PQ.

---

### Task 1: 建立 libvex-core 骨架与构建入口

**Files:**
- Create: `libvex-core/CMakeLists.txt`
- Modify: `build.sh`
- Test: `cmake -S libvex-core -B build/core -DCMAKE_BUILD_TYPE=Release && cmake --build build/core -j4`

**Step 1: 创建独立静态库构建定义**

- `vex_core` 目标包含 `distance.cpp/product_quantizer.cpp/node_store_memory.cpp/vex_core_capi.cpp`
- 安装 include/lib 输出规则

**Step 2: 增加 build.sh 的 core 子命令**

Run: `./build.sh core Release`
Expected: 仅构建 `libvex-core`，输出 `libvex_core.a`

**Step 3: 验证不影响既有 DuckDB 构建**

Run: `DUCKDB_SOURCE_DIR=/path/to/duckdb ./build.sh build Release`
Expected: 现有扩展路径不变

### Task 2: 抽离基础类型、配置、节点与并发原语

**Files:**
- Create: `libvex-core/include/vex/vex_types.h`
- Create: `libvex-core/include/vex/vex_config.hpp`
- Create: `libvex-core/include/vex/vex_node.hpp`
- Create: `libvex-core/include/vex/vex_concurrency.hpp`
- Test: `cmake --build build/core -j4`

**Step 1: 定义跨后端公共类型**

- row_id/node_id/type_id
- C/C++ 双接口可见

**Step 2: 提取 GraphIndexConfig 常量与层配置函数**

- 保持与现有 DuckDB 参数一致

**Step 3: 定义 NodeHeader（去 IndexPointer）**

- 使用 `uint32_t` 偏移/句柄表达

**Step 4: 抽离 SimpleRWLock/VisitedSet**

- 保留 mobile/non-mobile 两种路径

### Task 3: 抽离 distance 与 PQ 到 libvex-core

**Files:**
- Create: `libvex-core/include/vex/vex_distance.hpp`
- Create: `libvex-core/include/vex/vex_quantizer.hpp`
- Create: `libvex-core/src/distance.cpp`
- Create: `libvex-core/src/product_quantizer.cpp`
- Test: `cmake --build build/core -j4`

**Step 1: 迁移 DuckDB 可用实现并改 namespace**

- `duckdb::vex` -> `vex`
- include 替换为 `vex/...`

**Step 2: 修正 API 命名一致性**

- `Metric` 枚举与解析/序列化 API 对齐

**Step 3: 保持行为一致**

- SIMD dispatch 路径与 PQ 编解码逻辑不变

### Task 4: 定义 NodeStore 接口与 MemoryNodeStore 默认实现

**Files:**
- Create: `libvex-core/include/vex/vex_node_store.hpp`
- Create: `libvex-core/include/vex/vex_node_store_memory.hpp`
- Create: `libvex-core/src/node_store_memory.cpp`
- Test: `cmake --build build/core -j4`

**Step 1: 按架构文档定义 NodeStore 抽象接口**

- Allocate/Free
- GetVector/GetNeighbors/GetMetadata
- ForEachNode

**Step 2: 实现 flat-array MemoryNodeStore**

- headers_/vectors_/neighbors_l0_/upper_neighbors_/metadata_
- `node_id` 即数组索引

**Step 3: 保留 Phase 2 可扩展点**

- PrepareParallelAccess/FinishParallelAccess 默认空实现

### Task 5: 增加 C API 骨架

**Files:**
- Create: `libvex-core/include/vex/vex_core.h`
- Create: `libvex-core/src/vex_core_capi.cpp`
- Test: `cmake --build build/core -j4`

**Step 1: 定义 C API 结构与错误码**

- index config / result / error enum

**Step 2: 提供最小可用实现**

- create/destroy/add/add_batch 可工作
- search/serialize 先返回占位成功或空结果

**Step 3: 与 NodeStore 绑定**

- C API 内部句柄持有 `MemoryNodeStore`

### Task 6: 过滤谓词抽离并去 DB 类型依赖

**Files:**
- Create: `libvex-core/include/vex/vex_filter.hpp`
- Test: `cmake --build build/core -j4`

**Step 1: 引入 `vex::TypeId` 替代 `LogicalTypeId`**

**Step 2: 保留 Equality/Range/InList/Conjunction 行为一致**

### Task 7: 阶段收口与差异基线

**Files:**
- Modify: `README.md`（后续补）
- Modify: `README_zh.md`（后续补）
- Test: `git diff --stat`

**Step 1: 记录 Phase 0/1 已完成项与未完成项**

**Step 2: 明确 Phase 2 输入输出与风险控制点**

- 算法迁移从 `graph_index_core.cpp` 到 `graph_algo.cpp`
- 首个目标：单元一致性 + recall 基线

