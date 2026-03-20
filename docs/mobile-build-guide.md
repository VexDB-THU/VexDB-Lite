# VexDB-Lite 移动端构建指南

## 概述

VexDB-Lite 支持编译为移动端静态库 `libvexdb.a`，将 DuckDB + VEX 向量搜索引擎打包为单一文件，可直接集成到 iOS/Android/WASM 项目中。

**核心特性：**
- 完整 SQL 查询能力（DuckDB 引擎）
- HNSW 图索引向量搜索
- PQ 量化压缩
- HybridIndex 分区索引
- iOS arm64 strip 后 **~11MB**

## 环境要求

| 平台 | 工具链 |
|------|--------|
| iOS | Xcode + iOS SDK（最低 iOS 15.0） |
| Android | Android NDK（API level 24+） |
| WASM | Emscripten SDK |
| 通用 | CMake 3.5+, Python 3 |

## 快速开始

```bash
cd duckdb

# iOS 设备 (arm64)
bash build.sh ios

# iOS 模拟器 (Apple Silicon)
bash build.sh ios --sim

# Android (arm64-v8a)
bash build.sh android

# Android (x86_64 模拟器)
bash build.sh android --abi x86_64

# WebAssembly
bash build.sh wasm
```

## 构建产出

iOS 构建完成后：

```
build/ios_arm64/libvexdb/
├── libvexdb.a       # 静态库（~139MB .a, 链接 strip 后 ~11MB）
└── include/
    ├── duckdb.hpp    # 主头文件
    └── duckdb/       # DuckDB 内部头文件
```

## 集成到 iOS 项目

### Xcode 集成

1. 将 `libvexdb.a` 和 `include/` 拖入 Xcode 项目
2. Build Settings:
   - **Header Search Paths**: 添加 `include/` 路径
   - **Other Linker Flags**: `-lc++ -framework Foundation`
   - **Dead Code Stripping**: Yes（默认开启）
3. 代码使用：

```cpp
#include "duckdb.hpp"

// 创建内存数据库
duckdb::DuckDB db(nullptr);
duckdb::Connection con(db);

// 或者持久化到文件
duckdb::DuckDB db("/path/to/app/data/vexdb.db");
duckdb::Connection con(db);

// 创建向量表
con.Query("CREATE TABLE docs (id INT, title VARCHAR, embedding FLOATVECTOR(384))");

// 插入数据
con.Query("INSERT INTO docs VALUES (1, 'hello', [0.1, 0.2, ...]::FLOATVECTOR(384))");

// 创建 HNSW 索引
con.Query("CREATE INDEX idx ON docs USING GRAPH_INDEX (embedding)");

// 向量搜索
auto result = con.Query(
    "SELECT id, title FROM docs "
    "ORDER BY l2_distance(embedding, [0.15, 0.25, ...]::FLOATVECTOR(384)) "
    "LIMIT 10"
);

// 向量搜索 + 关系查询混合
auto result = con.Query(
    "SELECT d.title, u.name "
    "FROM docs d JOIN users u ON d.user_id = u.id "
    "ORDER BY l2_distance(d.embedding, ?::FLOATVECTOR(384)) "
    "LIMIT 5"
);
```

### Swift 桥接

```swift
// Bridging-Header.h
#include "duckdb.hpp"
```

或使用 DuckDB 的 C API：

```swift
import Foundation

let db = duckdb_open(nil) // 内存数据库
let con = duckdb_connect(db)
duckdb_query(con, "SELECT 1", nil)
```

## 集成到 Android 项目

### JNI 集成

1. 将 `libvexdb.a` 放入 `app/src/main/jniLibs/arm64-v8a/`
2. CMakeLists.txt:

```cmake
add_library(vexdb STATIC IMPORTED)
set_target_properties(vexdb PROPERTIES IMPORTED_LOCATION
    ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libvexdb.a)

target_link_libraries(your_jni_lib vexdb log dl m)
```

## 构建方案 (Profile)

通过 `--profile` 选择不同功能集：

```bash
bash build.sh ios                       # full（默认）
bash build.sh ios --profile compact     # compact
bash build.sh ios --profile minimal     # minimal
```

| Profile | 功能 | 适用场景 |
|---------|------|---------|
| **full** | HNSW + HybridIndex + Optimizer + core_functions | 需要完整 SQL 分析 + 向量搜索 |
| **compact** | HNSW + core_functions | 只需向量搜索 + 基本 SQL |
| **minimal** | 仅 HNSW | 只需向量搜索，不依赖高级 SQL 函数 |

**体积说明：** 三个 profile 的最终二进制体积差异不大（均 ~11MB），因为链接器 dead strip 会自动移除未引用代码。profile 的意义在于**功能承诺**——compact/minimal 在编译期排除了对应模块，保证不会意外依赖。

## 体积优化详情

移动端构建自动启用以下优化：

| 优化项 | 说明 |
|--------|------|
| `-Oz` | 最小体积优化（比 `-Os` 更激进） |
| `-ffunction-sections -fdata-sections` | 每个函数独立 section，链接器精确删除未引用函数 |
| `-fvisibility=hidden` | 隐藏非必要符号 |
| LTO | 链接时优化，跨模块消除冗余 |
| `SMALLER_BINARY=ON` | DuckDB 内置体积优化（减少特化模板） |
| `DISABLE_THREADS=ON` | 单线程模式，移除线程同步代码 |
| `VEX_MOBILE_MODE=ON` | 自旋锁降级为 mutex（省电） |
| Dead strip | iOS: `-Wl,-dead_strip` / Android: `-Wl,--gc-sections` |

**体积演进：**

| 阶段 | iOS arm64 strip 后 |
|------|-------------------|
| 初始（-Os only） | 29 MB |
| + `-Oz` | 17 MB |
| + function-sections + visibility + LTO + SMALLER_BINARY | **11 MB** |

## 移动端 vs 桌面端差异

| | 桌面端 (`release`) | 移动端 (`ios`/`android`) |
|---|---|---|
| 构建类型 | Release (`-O2`) | MinSizeRel (`-Oz`) |
| 产出 | `libduckdb.dylib` (45MB) + `vex.duckdb_extension` (456KB) | `libvexdb.a` (单文件) |
| 扩展加载 | 动态加载（`LOAD 'vex'`） | 静态链接（启动即可用） |
| 锁实现 | atomic spinlock（高吞吐） | mutex+condvar（省电） |
| 线程 | 多线程并行 | 单线程 |
| 并行构建阈值 | 10000 行 | 1000 行 |

## 支持的距离度量

```sql
-- L2 欧氏距离
ORDER BY l2_distance(vec, query) LIMIT k;

-- 余弦距离
ORDER BY cosine_distance(vec, query) LIMIT k;

-- 内积（负值，越小越相似）
ORDER BY inner_product(vec, query) DESC LIMIT k;
```

## 支持的向量维度

理论上不限（受内存约束），常用：

| 模型 | 维度 | 单向量内存 |
|------|------|-----------|
| MiniLM-L6 | 384 | 1.5 KB |
| BGE-small | 512 | 2 KB |
| OpenAI text-embedding-3-small | 1536 | 6 KB |
| Nomic Embed | 768 | 3 KB |

## 运行时配置

```sql
-- 搜索扩展因子（越高召回越好，越慢）
SET vex_ef_search = 40;  -- 默认 40

-- 暴力搜索阈值（小于此数量的数据用暴力搜索）
SET vex_brute_force_threshold = 64;  -- 默认 64

-- 并行构建阈值
SET vex_parallel_threshold = 1000;  -- 移动端默认 1000
```

## iOS 不支持动态加载

iOS App Store 禁止 `dlopen` 加载非系统库，所有可执行代码必须在构建时签名。因此移动端**必须使用静态链接**，即本方案产出的 `libvexdb.a`。

## 常见问题

### Q: 11MB 对移动端 app 影响大吗？

现代 app 通常 50-500MB+。11MB 的 SQL+向量搜索引擎是合理的。作为对比：SQLite 约 1MB，但没有 ANN 索引；Turso (DiskANN) 约 2MB。DuckDB 的额外体积换来的是完整的列式分析 SQL 能力。

### Q: 向量数据会占多少存储？

以 384 维 float32 为例：
- 1 万条向量：~15 MB（含索引）
- 10 万条向量：~150 MB
- 启用 PQ 量化可减少 4-8 倍

### Q: 支持后台线程构建索引吗？

移动端构建默认单线程（`DISABLE_THREADS=ON`）。如果需要后台多线程构建索引，可在编译时去掉 `DISABLE_THREADS`，但需注意功耗。
