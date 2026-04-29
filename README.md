# pg_vexdb - PostgreSQL HNSW 图索引扩展

**[English](README.en.md)** | **[中文](README.md)**

高性能向量相似度搜索 PostgreSQL 扩展，提供 HNSW（分层可导航小世界）图索引用于近似最近邻搜索。从 vexdb 移植，保持最大代码一致性以利于维护。

## 特性

### 向量类型
- **floatvector** - 单精度浮点向量（最高 16,384 维）
- **halfvector** - 半精度浮点向量（最高 16,384 维）
- 支持 NULL 值、TOAST 压缩和 typmod

### 距离函数
- **L2 距离** (`<->`) - 欧几里得距离
- **内积** (`<#>`) - 负内积
- **余弦距离** (`<=>`) - 余弦距离

### SIMD 加速
- 自动检测 CPU 能力（SSE、AVX、AVX512）
- 通过 GUC 参数运行时选择架构
- 支持 x86_64 和 ARM 架构

### 索引访问方法
- 基于 HNSW 图的近似最近邻搜索
- 可配置 M（每节点邻居数）和 ef_construction
- 支持并行索引构建
- 可配置 ef_search 高效搜索

### GUC 参数
| 参数 | 类型 | 默认值 | 描述 |
|------|------|--------|------|
| `pg_vexdb.ef_search` | int | 64 | HNSW 搜索 ef 参数 |
| `pg_vexdb.enable_vec_buffer_manager` | bool | true | 启用向量缓冲缓存 |
| `pg_vexdb.vector_buffers` | int | 262144 | 8KB 向量缓冲区数量 |
| `pg_vexdb.vec_architecture` | string | "" | SIMD 架构选择 |

---

## 环境要求

### 编译依赖
- **PostgreSQL 19**（基于 19devel 开发）
- **C++17** 编译器（GCC 8+、Clang 7+）
- **CMake** 3.10+
- **Boost**（仅用于预处理器宏）

### 运行环境
- PostgreSQL 19
- Linux（x86_64 或 ARM64）

---

## 编译

### 1. 编译 PostgreSQL（如未安装）

```bash
./configure --prefix=/path/to/pg-install --enable-debug --enable-cassert \
    --without-icu --without-readline --without-zlib CFLAGS="-O0 -g"
make -j$(nproc)
make install
```

### 2. 编译 pg_vexdb

```bash
cd pg_vexdb
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
make install
```

### 3. 配置 PostgreSQL

在 `postgresql.conf` 中添加：
```
shared_preload_libraries = 'pg_vexdb'
```

重启 PostgreSQL：
```bash
pg_ctl restart -D $PGDATA
```

---

## 使用

### 基本用法

```sql
-- 创建扩展
CREATE EXTENSION pg_vexdb;

-- 创建带向量列的表
CREATE TABLE items (
    id serial PRIMARY KEY,
    embedding floatvector(128)
);

-- 插入向量
INSERT INTO items (embedding) VALUES 
    ('[0.1, 0.2, 0.3, ...]'),
    ('[0.4, 0.5, 0.6, ...]');

-- 创建 HNSW 索引
CREATE INDEX ON items USING vexdb_graph (embedding floatvector_l2_ops)
    WITH (m = 16, ef_construction = 64);

-- 使用索引查询
SELECT * FROM items 
ORDER BY embedding <-> '[0.2, 0.3, 0.4]' 
LIMIT 10;
```

### 索引选项

```sql
CREATE INDEX ON items USING vexdb_graph (embedding floatvector_l2_ops)
    WITH (
        m = 32,                    -- 每节点邻居数（默认：16）
        ef_construction = 128,     -- 构建时搜索列表大小（默认：64）
        parallel_workers = 4       -- 并行构建工作进程数（默认：0=自动）
    );
```

### 查询调优

```sql
-- 增加 ef_search 提高召回率（成本更高）
SET pg_vexdb.ef_search = 256;

-- 强制索引扫描
SET enable_seqscan = false;

-- 带距离的查询
SELECT id, embedding <-> '[0.2, 0.3, 0.4]' AS distance
FROM items
ORDER BY embedding <-> '[0.2, 0.3, 0.4]'
LIMIT 10;
```

### SIMD 架构选择

```sql
-- 所有操作使用 AVX
SET pg_vexdb.vec_architecture = 'all:avx';

-- float 向量用 AVX512，half 向量用 SSE
SET pg_vexdb.vec_architecture = 'float:avx512, half:sse';

-- 重置为自动检测
SET pg_vexdb.vec_architecture = '';
```

---

## 框架与代码结构

### 目录布局

```
pg_vexdb/
├── distance/              # 距离函数头文件
│   ├── distance.h         # 核心距离函数声明
│   ├── distance_dispatcher.h
│   ├── architecture_macro.h
│   └── pq/               # 乘积量化
├── include/
│   ├── graph_index/       # 图索引头文件
│   ├── floatvector.h      # 浮点向量类型
│   ├── halfvec.h          # 半精度向量类型
│   ├── pg_compat.h        # PostgreSQL 兼容层
│   └── ...
├── knl/                   # vexdb 兼容层
│   ├── knl_alloc.cpp     # 内存分配、_PG_init
│   ├── knl_instance.h    # 全局实例结构
│   └── knl_variable.h    # 全局变量存根
├── module/               # 工具模块
│   ├── timer.h          # 计时工具
│   └── parallel_counter.h
├── quantizer/            # 量化器头文件（存根）
├── rabitq/               # RaBitQ 头文件
├── src/
│   ├── distance/         # 距离实现
│   ├── graph_index_*.cpp # 图索引实现
│   └── ...
└── vtl/                  # 向量模板库
    ├── vector
    ├── hashtable
    ├── disk_container/
    └── ...
```

### 核心组件

#### 1. 兼容层（`include/pg_compat.h`）

提供 vexdb 特性的抽象：
- 将所有 PostgreSQL 头文件包裹在 `extern "C"` 中
- 定义 `u_sess` → `pg_vexdb_session` 用于会话属性
- 提供 `VECTOR_FORKNUM`、`RM_GRAPH_INDEX_ID` 宏

#### 2. 向量模板库（`vtl/`）

兼容 PostgreSQL 内存管理的自定义模板库：
- 使用 PostgreSQL 内存上下文（palloc/pfree）
- 不使用 STL（与 setjmp/longjmp 不兼容）
- 提供 Vector、HashSet、PriorityQueue 等

#### 3. 图索引（`include/graph_index/`、`src/graph_index*.cpp`）

HNSW 实现：
- `graph_index.h` - 主接口
- `graph_index_algorithm.h` - HNSW 算法
- `graph_index_storage.h` - 磁盘存储
- `graph_index_cluster.h` - 聚类支持

#### 4. 距离函数（`distance/`、`src/distance/`）

SIMD 加速的距离计算：
- 基于 CPU 能力的运行时分发
- SSE、AVX、AVX512 实现
- 基于模板的类型灵活性

---

## 实现详解

### 1. 向量类型实现

**文件：** `src/floatvector.cpp`

`floatvector` 类型是直接存储在 PostgreSQL 中的 varlena 结构：

```cpp
struct FloatVector {
    int32 vl_len_;  /* varlena 头 */
    int16 dim;      /* 维度数 */
    int16 unused;   /* 保留 */
    float4 x[FLEXIBLE_ARRAY_MEMBER];
};
```

关键函数：
- `floatvector_in()` - 解析文本表示 `[1,2,3]`
- `floatvector_out()` - 转换为文本
- `l2_distance()` - 计算欧几里得距离
- 运算符使用包裹在 `extern "C"` 中的 PostgreSQL FMGR 接口：

```cpp
extern "C" {
PG_FUNCTION_INFO_V1(l2_distance);
Datum l2_distance(PG_FUNCTION_ARGS) {
    FloatVector *a = PG_GETARG_FLOATVECTOR_P(0);
    FloatVector *b = PG_GETARG_FLOATVECTOR_P(1);
    float dist = l2_distance_impl(a, b);
    PG_RETURN_FLOAT4(dist);
}
}
```

### 2. 索引访问方法

**文件：** `src/graph_index_am.cpp`

PostgreSQL 19 的 `IndexAmRoutine` 需要特定函数签名。我们创建包装函数：

```cpp
// 在 SQL 中注册的处理函数
PG_FUNCTION_INFO_V1(graph_index_amhandler);
Datum graph_index_amhandler(PG_FUNCTION_ARGS) {
    PG_RETURN_POINTER(graph_index_amroutine());
}

// 构建 IndexAmRoutine 结构
static IndexAmRoutine *graph_index_amroutine(void) {
    IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);
    
    amroutine->ambuild = graph_index_ambuild;
    amroutine->aminsert = graph_index_aminsert;
    amroutine->ambeginscan = graph_index_ambeginscan;
    amroutine->amgettuple = graph_index_amgettuple;
    // ... 更多函数指针
    return amroutine;
}

// IndexAmRoutine 签名的包装器
static IndexBuildResult *graph_index_ambuild(
    Relation heap, Relation index, IndexInfo *indexInfo) {
    return graph_index_build_internal(heap, index, indexInfo);
}
```

### 3. HNSW 搜索算法

**文件：** `include/graph_index/graph_index_algorithm.h`

搜索算法使用优先队列进行束搜索：

```cpp
template<typename T>
Vector<Cand<T>> search(float *query, size_t ef) {
    MaxHeap<Cand<T>> candidates;
    MinHeap<Cand<T>> results;
    UnorderedSet<T> visited;
    
    // 从入口点开始
    T ep = get_entry_point();
    float dist = distance(query, get_vector(ep));
    candidates.emplace(ep, dist);
    visited.insert(ep);
    
    while (!candidates.empty()) {
        Cand<T> cur = candidates.top();
        candidates.pop();
        
        if (results.size() >= ef && cur.dist > results.top().dist)
            break;
        
        results.emplace(cur);
        
        // 探索邻居
        for (T neighbor : get_neighbors(cur.id)) {
            if (visited.insert(neighbor).second) {
                float d = distance(query, get_vector(neighbor));
                candidates.emplace(neighbor, d);
            }
        }
    }
    
    return results.to_vector();
}
```

### 4. SIMD 分发

**文件：** `src/distance/architecture.cpp`

运行时 CPU 特性检测：

```cpp
static Arch detect_best_arch() {
#if COMPILER_TARGET_X86_64
    unsigned int eax, ebx, ecx, edx;
    
    // 检查 AVX512
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        if (ebx & bit_AVX512F)
            return Arch::AVX512;
    }
    
    // 检查 AVX
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if (ecx & bit_AVX)
            return Arch::AVX;
        if (edx & bit_SSE2)
            return Arch::SSE;
    }
#endif
    return Arch::SCALAR;
}
```

### 5. 内存管理

**文件：** `knl/knl_alloc.cpp`

所有内存使用 PostgreSQL 上下文：

```cpp
void* mem_align_alloc(size_t alignment, size_t size) {
    return palloc_aligned(size, alignment, 0);
}

// VTL 的自定义分配器
template<typename T>
class CtxAllocator {
    MemoryContext ctx;
public:
    T* allocate(size_t n) {
        return (T*)MemoryContextAlloc(ctx, n * sizeof(T));
    }
    void deallocate(T* p) { pfree(p); }
};
```

### 6. GUC 注册

**文件：** `src/guc_config.cpp`

带赋值钩子的自定义 GUC 参数：

```cpp
static void assign_ef_search(int newval, void *extra) {
    pg_vexdb_session.attr_storage.ef_search = newval;
}

void pg_vexdb_init_guc(void) {
    DefineCustomIntVariable("pg_vexdb.ef_search",
        "HNSW 索引搜索的搜索列表大小。",
        NULL, &pg_vexdb_ef_search, 64, 1, 65535,
        PGC_USERSET, 0, NULL, assign_ef_search, NULL);
}
```

---

## 性能调优

1. **索引构建**
   - 使用更高的 `ef_construction` 获得更好的召回率（128-256）
   - 大数据集使用 `parallel_workers`
   - 如果可能，增加 `maintenance_work_mem`

2. **查询**
   - 根据召回率要求调整 `ef_search`
   - 更高的 `ef_search` = 更好的召回率，更慢的查询
   - 典型值：64-256

3. **内存**
   - 调整 `pg_vexdb.vector_buffers` 控制缓存大小
   - 默认：262144 个缓冲区 = 2GB

---

## 限制

1. **WAL** - 向量数据变更未记录 WAL（延迟）
2. **量化** - PQ 和 RaBitQ 尚未实现
3. **平台** - 仅 Linux（x86_64、ARM64）

---

## 许可证

与 vexdb 相同的许可证。

---

## 致谢

从 vexdb 向量索引实现移植到 PostgreSQL。
