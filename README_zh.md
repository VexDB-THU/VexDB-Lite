<div align="center">
  <h1>VexDB-Lite</h1>
  <p><strong>轻量级高性能向量数据库</strong></p>
  <p>内置 GRAPH_INDEX 向量索引引擎 — 基于 HNSW 增强，支持并行构建、向量去重、PQ 量化、SIMD 加速距离计算</p></invoke>
  <p><a href="README.md">English</a> | <a href="README_zh.md">中文</a></p>
</div>

---

## 简介

VexDB-Lite 是 [VexDB](https://vexdb.com/) 向量数据库家族的轻量级版本，面向嵌入式和边缘部署场景。它将 VexDB 的核心向量检索能力移植到列式分析引擎上，提供完整的 SQL 接口，让向量搜索像普通查询一样简单。

### 与 VexDB 的关系

|              | VexDB                                        | VexDB-Lite                                           |
| ------------ | -------------------------------------------- | ---------------------------------------------------- |
| **定位**     | 企业级分布式向量数据库                       | 轻量级嵌入式向量数据库                               |
| **存储引擎** | 基于 openGauss 行存储引擎                    | 基于 DuckDB 列式分析引擎                             |
| **索引算法** | HNSW 图索引（磁盘页面存储，支持并行 Vacuum） | HNSW 图索引（内存图结构，FixedSizeAllocator 持久化） |
| **图修复**   | 完整的 `RepairGraphElement`，支持并行 Worker | 同步删除 + 图修复（更新入口→清理邻居→修复连接）      |
| **量化器**   | PQ / RaBitQ / 自适应在线更新                 | Product Quantization (PQ)                            |
| **部署模式** | C/S 架构，支持集群                           | 嵌入式进程内，单文件数据库                           |
| **混合索引** | 分区表 + HNSW                                | HYBRID_INDEX（内存分区 HNSW）                        |
| **适用规模** | 亿级向量                                     | 百万级向量                                           |
| **适用场景** | 生产环境、企业级 RAG、大规模推荐             | 边缘设备、移动端、原型验证、开发测试                 |

VexDB-Lite 的 GRAPH_INDEX 核心算法（多层图搜索、邻居选择、图修复）源自 VexDB 的 GRAPH_INDEX 实现，在嵌入式场景下做了适配：

- **全内存图结构**代替 VexDB 的磁盘页面存储，降低 I/O 开销
- **FixedSizeAllocator 序列化**代替 VexDB 的 WAL/页面级持久化，简化存储层
- **同步图修复**代替 VexDB 的延迟 Vacuum Worker，保证即时一致性
- 保留了 VexDB 的核心设计：**分层导航、双向连接、删除后图修复、level 越界保护**

### 与 DuckDB 的关系

VexDB-Lite 基于 [DuckDB](https://duckdb.org/) 构建，DuckDB 是一个进程内分析型数据库。VexDB-Lite 在 DuckDB 之上扩展了向量搜索能力，同时完整保留 DuckDB 生态：

- **完整 SQL 支持** — DuckDB 的所有 SQL 特性（JOIN、CTE、窗口函数等）可与向量搜索无缝配合
- **Python 绑定** — 通过 `pip` 安装，支持 Pandas、Polars、Arrow
- **文件格式兼容** — 原生读写 Parquet、CSV、JSON
- **单文件数据库** — 无服务端、零依赖，一个 `.duckdb` 文件即可

**与 DuckDB vss 扩展的区别**：DuckDB 官方的 [vss 扩展](https://duckdb.org/docs/extensions/vss.html) 提供基础的 HNSW 索引。VexDB-Lite 在此基础上增加了并行构建、向量去重、PQ 量化、混合过滤索引、同步图修复、SIMD 加速距离计算等能力。

## 适用场景

- **RAG 应用** — 语义检索 + 结构化过滤一站式完成
- **推荐系统** — 向量相似度 + 业务属性混合排序
- **图像/音频检索** — 高维特征向量的近邻搜索
- **嵌入式部署** — 零依赖、单文件数据库，适合边缘设备和移动端
- **开发测试** — 本地快速验证向量搜索逻辑，再迁移到 VexDB 生产环境

## 核心特性

### 原生向量类型

```sql
CREATE TABLE documents (
    id INTEGER PRIMARY KEY,
    content VARCHAR,
    embedding FLOATVECTOR(768)
);
```

### GRAPH_INDEX — 增强型 HNSW

GRAPH_INDEX 基于 HNSW（Hierarchical Navigable Small World）算法构建，相比标准 HNSW 做了以下增强：

- **并行构建** — 多线程索引构建，lock-free 搜索 + 节点级自旋锁
- **向量去重** — 相同向量共享图节点，降低内存占用，提升搜索速度
- **PQ 量化** — 可选 Product Quantization，大数据集下显著降低内存开销
- **SIMD 加速** — SSE、AVX2、ARM NEON 优化的距离计算，含通用回退
- **同步图修复** — 删除后立即修复图连接，无需后台 Vacuum
- **暴力搜索自动降级** — 小数据集自动切换精确搜索，保证 100% 召回率

支持 L2、余弦、内积三种距离度量。

```sql
-- 创建索引（默认 L2 距离）
CREATE INDEX idx ON documents USING GRAPH_INDEX (embedding)
    WITH (m = 16, ef_construction = 64);

-- L2 距离搜索（自动走索引）
SELECT id, content
FROM documents
ORDER BY l2_distance(embedding, [0.1, 0.2, ...]::FLOATVECTOR(768))
LIMIT 10;

-- 余弦距离索引（自动归一化，适合文本语义搜索）
CREATE INDEX idx_cos ON documents USING GRAPH_INDEX (embedding)
    WITH (metric = 'cosine');

SELECT id, content
FROM documents
ORDER BY cosine_distance(embedding, [0.1, 0.2, ...]::FLOATVECTOR(768))
LIMIT 10;

-- 内积索引（适合推荐系统评分排序）
CREATE INDEX idx_ip ON documents USING GRAPH_INDEX (embedding)
    WITH (metric = 'ip');

SELECT id, content
FROM documents
ORDER BY inner_product(embedding, [0.1, 0.2, ...]::FLOATVECTOR(768)) DESC
LIMIT 10;
```

### 混合过滤索引

按业务字段分区建立独立 HNSW 图，实现高效的"过滤 + 向量搜索"。

```sql
CREATE INDEX idx ON products USING HYBRID_INDEX (embedding, category)
    WITH (metric = 'cosine');

-- 分区内向量搜索
SELECT * FROM products
WHERE category = 'electronics'
ORDER BY cosine_distance(embedding, ?::FLOATVECTOR(128))
LIMIT 20;
```

### 距离函数

| 函数 / 运算符             | 说明     | 排序方向           | 适用场景                                                              |
| ------------------------- | -------- | ------------------ | --------------------------------------------------------------------- |
| `l2_distance` / `<->`     | 欧氏距离 | ASC（越小越近）    | 通用向量检索                                                          |
| `cosine_distance` / `<=>` | 余弦距离 | ASC（越小越近）    | 文本语义相似度                                                        |
| `inner_product`           | 内积     | DESC（越大越相似） | 推荐系统评分                                                          |
| `<~>`                     | 负内积   | ASC（越小越相似）  | 内积的运算符写法，`vec <~> query` 等价于 `-inner_product(vec, query)` |

### Product Quantization (PQ) 加速

```sql
CREATE INDEX idx ON vectors USING GRAPH_INDEX (vec)
    WITH (quantizer = 'pq', pq_m = 8);
```

PQ 量化将高维向量压缩为紧凑编码，降低内存占用，加速大规模数据集上的搜索。

### 完整的持久化支持

- **Checkpoint 序列化**：索引随数据库一起持久化到磁盘
- **WAL 重放**：未 checkpoint 的修改通过 WAL 日志恢复
- **Restart 安全**：数据库重启后索引自动恢复，无需重建

## 技术架构

```
+---------------------------------------------------+
|                   SQL 接口层                        |
|   FLOATVECTOR 类型 · 距离函数 · ORDER BY 优化器      |
+---------------------------------------------------+
|                    索引层                           |
|   GRAPH_INDEX (HNSW)  ·  HYBRID_INDEX (分区HNSW)   |
|   Product Quantizer   ·  SIMD 距离计算              |
+---------------------------------------------------+
|                  存储引擎层                          |
|   列式存储 · 事务 · Checkpoint · WAL                |
+---------------------------------------------------+
```

## 快速开始

### 编译

```bash
# Release 编译（优化，用于部署和基准测试）
./build.sh release

# Debug 编译（含调试信息和单元测试）
./build.sh dev

# Debug + AddressSanitizer（检测内存问题）
./build.sh dev --asan

# 编译并运行所有 vex 测试
./build.sh test

# 运行匹配特定名称的测试
./build.sh test --filter 'graph_index_dedup'

# 指定并行线程数
./build.sh release -j4

# 清理所有构建目录
./build.sh clean
```

| 命令         | 构建类型   | 默认 target | 输出路径                |
| ------------ | ---------- | ----------- | ----------------------- |
| `dev`        | Debug      | unittest    | `duckdb/build/debug/`   |
| `dev --asan` | Debug+ASan | unittest    | `duckdb/build/asan/`    |
| `release`    | Release    | duckdb      | `duckdb/build/release/` |
| `test`       | Release    | unittest    | 编译后自动运行测试      |

### 使用

```bash
./duckdb/build/release/duckdb
```

```sql
-- 加载 VexDB 向量扩展
LOAD vex;

-- 创建表
CREATE TABLE items (id INTEGER PRIMARY KEY, vec FLOATVECTOR(4));

-- 创建 HNSW 索引
CREATE INDEX idx ON items USING GRAPH_INDEX (vec);

-- 插入数据
INSERT INTO items VALUES
    (1, [1.0, 0.0, 0.0, 0.0]::FLOATVECTOR(4)),
    (2, [0.0, 1.0, 0.0, 0.0]::FLOATVECTOR(4)),
    (3, [0.0, 0.0, 1.0, 0.0]::FLOATVECTOR(4));

-- 向量搜索
SELECT id FROM items
ORDER BY l2_distance(vec, [0.9, 0.1, 0.0, 0.0]::FLOATVECTOR(4))
LIMIT 1;
-- 结果: 1
```

## 索引参数

### GRAPH_INDEX

| 参数              | 默认值 | 范围                   | 说明                                                       |
| ----------------- | ------ | ---------------------- | ---------------------------------------------------------- |
| `metric`          | `l2`   | `l2` / `cosine` / `ip` | 距离度量。`cosine` 自动归一化向量，`ip` 内积（越大越相似） |
| `m`               | 16     | 2-100                  | 每层最大邻居数，越大索引质量越高，内存越大                 |
| `ef_construction` | 64     | 4-1000                 | 构建时搜索宽度，越大构建越慢但质量越高                     |
| `quantizer`       | none   | none/pq                | 量化器类型                                                 |
| `pq_m`            | auto   | -                      | PQ 子空间数量（维度须可被整除）                            |
| `max_dedup`       | 8      | 1-256                  | 向量去重容量（1=禁用），相同向量共享图节点                 |
| `threads`         | 1      | 1-N                    | 并行构建线程数，大数据集建议设为 CPU 核数                  |

### HYBRID_INDEX

| 参数              | 默认值 | 说明                     |
| ----------------- | ------ | ------------------------ |
| `metric`          | `l2`   | 距离度量，同 GRAPH_INDEX |
| `m`               | 16     | 同 GRAPH_INDEX           |
| `ef_construction` | 64     | 同 GRAPH_INDEX           |

## 运行时参数

| 参数                        | 默认值 | 说明                                            |
| --------------------------- | ------ | ----------------------------------------------- |
| `vex_ef_search`             | 40     | 搜索时的扩展因子，越大召回率越高但越慢          |
| `vex_brute_force_threshold` | 64     | 节点数低于此值时自动切换为暴力搜索（100% 召回） |

```sql
-- 提高搜索质量
SET vex_ef_search = 100;

-- 小表强制暴力搜索
SET vex_brute_force_threshold = 1000;

-- 恢复默认值
RESET vex_ef_search;
RESET vex_brute_force_threshold;
```

## 项目结构

```
extension/vex/
├── include/              # 头文件
│   ├── vex_graph_index.hpp
│   ├── vex_graph_index_core.hpp
│   ├── vex_hybrid_index.hpp
│   ├── vex_distance.hpp
│   ├── vex_quantizer.hpp
│   ├── vex_optimizer.hpp
│   ├── vex_types.hpp
│   └── vex_functions.hpp
├── index/                # 索引实现
│   ├── graph_index.cpp        # HNSW 图索引
│   ├── graph_index_core.cpp   # 图核心算法
│   └── hybrid_index.cpp       # 混合过滤索引
├── distance/             # SIMD 距离计算
├── functions/            # SQL 函数注册
├── optimizer/            # 查询优化器（ORDER BY → 索引扫描）
├── quantizer/            # Product Quantizer
├── types/                # FLOATVECTOR 类型
└── vex_extension.cpp     # 扩展入口

test/sql/vex/            # 75 个测试文件
├── types/                # 类型测试
├── functions/            # 距离函数测试
└── index/                # 索引测试（含持久化/压力/模糊/PQ/阈值）
```

## 测试

```bash
# 一键编译并运行所有 vex 测试
./build.sh test

# 运行特定测试
./build.sh test --filter 'graph_index_dedup'

# 或直接调用 unittest
duckdb/build/release/test/unittest "test/sql/vex/*"
duckdb/build/release/test/unittest "test/sql/vex/index/graph_index_basic.test"
```

测试覆盖 **75 个测试文件**，涵盖：

- 基本 CRUD 和搜索正确性
- HNSW 索引持久化与 WAL 重放
- 重度删除后的图修复与搜索质量
- PQ 量化搜索与持久化
- 混合索引多分区操作
- Brute-force / 图搜索阈值边界
- 并发读写、事务回滚
- 向量去重（deduplication）
- Recall-after-delete 大规模质量验证
- 高维向量（128维）
- 多距离度量（L2 / Cosine / Inner Product）
- 非归一化向量、零向量、反向向量等边界场景
- Recall@10 vs 暴力搜索质量验证

## 贡献

欢迎贡献！请先开一个 issue 讨论你的想法，然后再提交 pull request。

## 许可证

本项目采用 [Apache License 2.0](LICENSE) 许可证。底层 DuckDB 引擎采用 [MIT 许可证](LICENSE-DUCKDB)。
