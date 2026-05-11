# PG VexDB ARM 服务器测试报告

## 1. 测试摘要

- 测试日期：`2026-05-08`
- 测试对象：当前 `VexDB-Lite` 工作区中的 `pg_vexdb` PostgreSQL 插件
- 测试服务器：`aaa@192.168.130.66`
- CPU 架构：`aarch64`
- PostgreSQL 运行版本：`19devel`
- PostgreSQL 编译优化：`-O3 -DNDEBUG`
- 插件编译优化：`Release`，`-O3 -DNDEBUG`
- 测试数据集：SIFT，包含 `10k`、`100k`、`1M`
- 距离类型：L2
- 索引类型：`USING vexdb_graph (vec floatvector_l2_ops)`

## 2. 硬件与系统环境

### 2.1 CPU

- 型号：`HUAWEI Kirin 9000C`
- 架构：`aarch64`
- 拓扑：`3 sockets / 2 cores per socket / 12 CPUs / 1 thread per core`
- 主频：`335 MHz - 2300 MHz`
- `lscpu` 可见 ARM 指令特性：`asimd`、`sve`、`i8mm`、`sha*`、`sm3`、`sm4`、`crc32`

### 2.2 内存

- 总内存：`15 GiB`
- 采集报告时已用：`8.7 GiB`
- 可用内存：约 `4.6 GiB`
- Swap：`20 GiB`，已用约 `3.9 GiB`

### 2.3 操作系统

- 内核：`Linux aaa-pc 5.10.97-23-9000c`
- 发行版：`Kylin V10 SP1`

### 2.4 磁盘

- 根文件系统：`94G` 总容量，采集时 `40G` 已用，`49G` 可用
- `/home`：`916G` 总容量，采集时 `369G` 已用，`501G` 可用

## 3. 软件与构建信息

### 3.1 源码版本

- 本地工作区基线提交：`e4223a3dd64031f18b86ce32c5dc96597ebffd95`
- 提交标题：`fix bugs`
- 提交时间：`2026-05-08 10:02:26 +0800`

### 3.2 本次 ARM 测试所需兼容性修复

本次 ARM 服务器测试不是直接拿基线提交原样编译，而是在当前工作区上追加了最小 ARM 编译兼容修复：

- `include/vector_buffer_manager.h`、`src/vector_smgr.cpp`
  - 在包含 Boost 并发容器头之前临时解除 PostgreSQL 的 `snprintf/vsnprintf` 宏污染，避免与 Boost 1.90 冲突。
- `include/graph_index/graph_index_algorithm.h`
  - 为 `Distancer::transform_type` 补充依赖模板的 `template` 关键字，满足 GCC/aarch64 语义要求。
- `src/distance/pg/distance.cpp`
  - 同样补充依赖模板的 `template` 关键字。
- `include/halfutils.h`
  - 在 ARM `half = float16_t` 的情况下，通过原始 `16-bit` 比特位来做 `NaN/Inf/Zero` 判定。
- `distance/core/architecture_macro.h`
  - 对 `PG_VEXDB_TARGET_PG + ARM` 暂时关闭 `NEON/SVE` 派发，强制 PG 侧距离实现走 `GENERAL` 路径，避免链接缺失的 ARM SIMD 符号。

### 3.3 PostgreSQL 构建信息

- 安装路径：`/opt/postgresql-19rel-install`
- `pg_config --version`：`PostgreSQL 19devel`
- `pg_config --configure`：

```text
--prefix=/opt/postgresql-19rel-install --without-icu --without-readline --without-zlib CFLAGS=-O3 -DNDEBUG
```

### 3.4 pg_vexdb 插件构建信息

- 构建目录：`/opt/vexdb-lite-build/VexDB-Lite/build-pg19rel-release`
- `CMAKE_BUILD_TYPE=Release`
- `CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG`
- `CMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG`

### 3.5 PostgreSQL 运行时配置

- 数据目录：`/home/aaa/vexdb-validation/pgdata-pg19rel-arm`
- Socket 目录：`/home/aaa/vexdb-validation/run`
- 端口：`55435`
- `shared_preload_libraries = 'pg_vexdb'`
- `shared_buffers = 2GB`
- 集群级 `maintenance_work_mem = 64MB`
- Benchmark 会话级覆盖：
  - 所有 benchmark 脚本均显式设置 `enable_seqscan = off`
  - 所有 benchmark 脚本均显式设置 `pg_vexdb.ef_search = 100`
  - 全量脚本显式设置 `maintenance_work_mem = '2GB'`

## 4. 测试数据

### 4.1 数据文件

- 标准小规模 benchmark 文件：
  - `vexdb-duck/test/benchmark/data/sift_train_10k.fbin`
  - `vexdb-duck/test/benchmark/data/sift_train_100k.fbin`
  - `vexdb-duck/test/benchmark/data/sift_query_200.fbin`
  - `vexdb-duck/test/benchmark/data/sift_gt_10k_200q.ibin`
  - `vexdb-duck/test/benchmark/data/sift_gt_100k_200q.ibin`
- SIFT1M 文件：
  - `vexdb-duck/test/benchmark/data/sift_base.fvecs`
  - `vexdb-duck/test/benchmark/data/sift_query.fvecs`
  - `vexdb-duck/test/benchmark/data/sift_groundtruth.ivecs`

### 4.2 数据规模

- `10k`：`10,000` 条 base vector，`200` 条 query
- `100k`：`100,000` 条 base vector，`200` 条 query
- `1M`：`1,000,000` 条 base vector，`200` 条 query
- 向量维度：`128`
- 召回率指标：
  - `Recall@10`
  - `Recall@100`

## 5. 测试方法

### 5.1 脚本口径

由于当前工作区没有可直接用于 PostgreSQL 的现成 benchmark 二进制，本次 ARM 测试统一使用 Python + `psycopg2` 脚本执行，脚本位于 ARM 服务器：

- `/home/aaa/vexdb-validation/scripts/pg_sift_small_bench.py`
- `/home/aaa/vexdb-validation/scripts/pg_sift1m_bench.py`
- `/home/aaa/vexdb-validation/scripts/pg_sift1m_query_only.py`

### 5.2 执行流程

`10k/100k` 的脚本流程：

- 创建全新测试表
- 批量导入 base vectors
- 以 `m = 16`、`ef_construction = 64` 创建 `vexdb_graph` 索引
- 执行 `200` 条 ANN 查询
- 与 ground truth 对比，输出召回率

`1M` 的全量脚本流程：

- 创建全新测试表
- 从 `sift_base.fvecs` 顺序导入 `1,000,000` 条向量
- 以 `m = 16`、`ef_construction = 64` 创建 `vexdb_graph` 索引
- 执行 `200` 条查询并计算 `Recall@10/Recall@100`

`1M query-only` 脚本流程：

- 复用已构建好的 `sift_pg_1m / idx_sift_pg_1m_vec`
- 直接执行 `200` 条查询
- 记录热态查询时间与召回率

## 6. 测试结果

### 6.1 小规模与中规模

| 规模 | Base Rows | Query 数 | Load (ms) | Build (ms) | Query (ms) | QPS | Recall@10 | Recall@100 | 是否走索引 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 10k | 10,000 | 200 | 653.710 | 3343.997 | 4221.737 | 47.374 | 0.999500 | 0.995050 | 是 |
| 100k | 100,000 | 200 | 7190.675 | 50600.905 | 36256.395 | 5.516 | 0.997500 | 0.974600 | 是 |

### 6.2 SIFT1M

| 模式 | Base Rows | Query 数 | Load (ms) | Build (ms) | Query (ms) | QPS | Recall@10 | Recall@100 | 是否走索引 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 全量重建后首轮查询 | 1,000,000 | 200 | 80249.436 | 727355.502 | 117733.467 | 1.699 | 0.986000 | 0.940750 | 是 |
| 已有索引热态 query-only 复测 | 1,000,000 | 200 | n/a | n/a | 565.444 | 353.705 | 0.986000 | 0.940750 | 是 |

## 7. 与 x86 结果对照

相对于同日 x86 报告：

- 召回率基本一致：
  - `10k`: `0.9995 / 0.99505`
  - `100k`: `0.9975 / 0.9746`
  - `1M`: `0.9860 / 0.94075`
- 这说明当前 ARM 服务器上的图构建与 ANN 搜索结果在正确性上与 x86 保持一致。
- 时间维度上，ARM 明显慢于 x86：
  - `10k build`：ARM `3.34s`，x86 `2.32s`
  - `100k build`：ARM `50.60s`，x86 `29.85s`
  - `1M build`：ARM `727.36s`，x86 `440.30s`
- `1M` 热态查询也明显慢于 x86：
  - ARM：`565.444 ms / 200q`
  - x86：`421.385 ms / 200q`

## 8. 结果解读

- 当前 ARM 环境下，`pg_vexdb` 的索引构建、优化器计划生成、执行器索引扫描、召回率表现都已经跑通。
- `10k/100k/1M` 三组数据的召回率与 x86 报告一致，说明算法与搜索逻辑没有因为 ARM 兼容修复而发生偏移。
- 当前 ARM PostgreSQL 插件构建为了规避缺失的 `neonv8/sve` PG 链接单元，强制使用了 `GENERAL` 距离派发路径。因此，这份报告代表的是“ARM 上当前可编译可运行版本”的性能，而不是“ARM SIMD fully enabled”的上限性能。
- `1M` 的冷态与热态差异依然明显：
  - 首轮全量脚本查询：`117.7s / 200q`
  - 紧接着 query-only 复测：`565.444ms / 200q`
- 这一点与 x86 的现象一致，说明主要差异仍然来自缓存预热状态，而不是召回率退化。

## 9. 原始输出

### 9.1 10k

```text
load_ms=653.710
build_ms=3343.997
query_ms=4221.737
qps=47.374
recall@10=0.999500
recall@100=0.995050
uses_index_scan=true
first_explain_has_index_scan=yes
```

### 9.2 100k

```text
load_ms=7190.675
build_ms=50600.905
query_ms=36256.395
qps=5.516
recall@10=0.997500
recall@100=0.974600
uses_index_scan=true
first_explain_has_index_scan=yes
```

### 9.3 1M 全量重建

```text
load_ms=80249.436
build_ms=727355.502
query_ms=117733.467
qps=1.699
recall@10=0.986000
recall@100=0.940750
uses_index_scan=true
```

### 9.4 1M 热态 query-only 复测

```text
query_ms=565.444
qps=353.705
recall@10=0.986000
recall@100=0.940750
uses_index_scan=true
```
