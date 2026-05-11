# PG VexDB x86 服务器测试报告

## 1. 测试摘要

- 测试日期：`2026-05-08`
- 测试对象：当前 `VexDB-Lite` 工作区中的 `pg_vexdb` PostgreSQL 插件
- 测试服务器：`root@8.152.168.4:10022`
- CPU 架构：`x86_64`
- PostgreSQL 运行版本：`19devel`
- PostgreSQL 编译优化：`-O3 -DNDEBUG`
- 插件编译优化：`Release`，`-O3 -DNDEBUG`
- 测试数据集：SIFT，包含 `10k`、`100k`、`1M`
- 距离类型：L2
- 索引类型：`USING vexdb_graph (vec floatvector_l2_ops)`

## 2. 硬件与系统环境

### 2.1 CPU

- 型号：`Intel(R) Xeon(R) CPU E5-2696 v4 @ 2.20GHz`
- 拓扑：`1 socket / 22 cores / 44 threads`
- L3 Cache：`55 MiB`
- `lscpu` 可见 SIMD 指令集：`sse`、`sse2`、`ssse3`、`sse4_1`、`sse4_2`、`avx`、`avx2`、`fma`

### 2.2 内存

- 总内存：`62 GiB`
- 采集环境信息时可用内存约：`55 GiB`
- Swap：`31 GiB`

### 2.3 操作系统

- 内核：`Linux localhost.localdomain 5.14.0-587.el9.x86_64`

### 2.4 磁盘

- 根文件系统：`70G` 总容量，采集时 `69G` 已用，`1.8G` 可用
- 说明：剩余磁盘空间偏紧，多次重复执行 1M 级别全量重建时需要留意空间水位

## 3. 软件与构建信息

### 3.1 源码版本

- 本地工作区同步参考提交：`e4223a3dd64031f18b86ce32c5dc96597ebffd95`
- 提交标题：`fix bugs`
- 提交时间：`2026-05-08 10:02:26 +0800`
- 说明：服务器上的 `/opt/vexdb-lite-build/VexDB-Lite` 是同步后的工作目录，不是带 `.git` 的完整仓库，因此服务器侧无法直接查询 commit hash。报告中以上述本地提交作为同步参考版本。

### 3.2 PostgreSQL 构建信息

- 安装路径：`/opt/postgresql-19rel-install`
- `pg_config --version`：`PostgreSQL 19devel`
- `pg_config --configure`：

```text
--prefix=/opt/postgresql-19rel-install --without-icu --without-readline --without-zlib CFLAGS=-O3 -DNDEBUG
```

### 3.3 pg_vexdb 插件构建信息

- 构建目录：`/opt/vexdb-lite-build/VexDB-Lite/build-pg19rel-release`
- `CMAKE_BUILD_TYPE=Release`
- `CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG`
- `CMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG`

### 3.4 PostgreSQL 运行时配置

- 端口：`55435`
- `shared_preload_libraries = 'pg_vexdb'`
- `shared_buffers = 2GB`
- 集群级 `maintenance_work_mem = 64MB`
- Benchmark 会话级覆盖：
  - 标准 `10k/100k` benchmark：使用 benchmark 程序内部默认会话配置
  - 自定义 `1M` 全量 benchmark：显式设置 `maintenance_work_mem = '2GB'`
  - `1M` 查询脚本：显式设置 `enable_seqscan = off`、`enable_bitmapscan = on`、`pg_vexdb.ef_search = 100`

## 4. 测试数据

### 4.1 数据文件

- 标准小规模 benchmark 文件：
  - `vexdb-duck/test/benchmark/data/sift_train_10k.fbin`
  - `vexdb-duck/test/benchmark/data/sift_train_100k.fbin`
  - `vexdb-duck/test/benchmark/data/sift_query_200.fbin`
  - `vexdb-duck/test/benchmark/data/sift_gt_10k_200q.ibin`
  - `vexdb-duck/test/benchmark/data/sift_gt_100k_200q.ibin`
- SIFT1M 文件：
  - `/opt/vexdb-lite-build/VexDB-Lite/vexdb-duck/test/benchmark/data/sift_base.fvecs`
  - `/opt/vexdb-lite-build/VexDB-Lite/vexdb-duck/test/benchmark/data/sift_query.fvecs`
  - `/opt/vexdb-lite-build/VexDB-Lite/vexdb-duck/test/benchmark/data/sift_groundtruth.ivecs`

### 4.2 数据规模

- `10k`：`10,000` 条 base vector，`200` 条 query
- `100k`：`100,000` 条 base vector，`200` 条 query
- `1M`：`1,000,000` 条 base vector，`200` 条 query
- 向量维度：`128`
- 召回率指标：
  - `Recall@10`
  - `Recall@100`

## 5. 测试方法

### 5.1 标准 SQL Benchmark

`10k` 与 `100k` 使用服务器上的标准 benchmark 二进制：

```bash
/opt/vexdb-lite-build/VexDB-Lite/build/pg/pg_sift_sql_benchmark \
  "postgresql:///postgres?host=/run/postgresql&port=55435&user=postgres" \
  <scale> \
  /opt/vexdb-lite-build/VexDB-Lite/vexdb-duck/test/benchmark/data
```

该 benchmark 的执行流程为：

- 创建全新测试表
- 批量导入 base vectors
- 以 `m = 16`、`ef_construction = 64` 创建 `vexdb_graph` 索引
- 执行 `200` 条 ANN 查询
- 与 ground truth 对比，输出召回率
- 记录并输出 `load_ms`、`build_ms`、`query_ms`、`qps`、`recall@10`、`recall@100`

### 5.2 SIFT1M 自定义脚本

`1M` 使用服务器上已有的两个脚本：

- 全量构建与查询：`/tmp/pg_sift1m_bench.py`
- 已有索引上的 query-only：`/tmp/pg_sift1m_query_only.py`

脚本参数与行为：

- 表名：`sift_pg_1m`
- 索引名：`idx_sift_pg_1m_vec`
- 建索引参数：`m = 16`、`ef_construction = 64`
- 查询会话参数：`enable_seqscan = off`、`pg_vexdb.ef_search = 100`
- 全量脚本额外设置：`maintenance_work_mem = '2GB'`

## 6. 测试结果

### 6.1 标准 benchmark 结果

| 规模 | Base Rows | Query 数 | Load (ms) | Build (ms) | Query (ms) | QPS | Recall@10 | Recall@100 | 是否走索引 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 10k | 10,000 | 200 | 454.730 | 2319.690 | 4707.020 | 42.4897 | 0.999500 | 0.995050 | 是 |
| 100k | 100,000 | 200 | 4499.110 | 29849.700 | 35467.700 | 5.63894 | 0.997500 | 0.974600 | 是 |

### 6.2 SIFT1M 结果

| 模式 | Base Rows | Query 数 | Load (ms) | Build (ms) | Query (ms) | QPS | Recall@10 | Recall@100 | 是否走索引 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 全量重建后首轮查询 | 1,000,000 | 200 | 49720.795 | 440295.289 | 118939.861 | 1.682 | 0.986000 | 0.940750 | 是 |
| 已有索引热态 query-only 复测 | 1,000,000 | 200 | n/a | n/a | 421.385 | 474.626 | 0.986000 | 0.940750 | 是 |

## 7. 结果解读

- 当前 x86 服务器上的 PostgreSQL 与 `pg_vexdb` 都已经确认使用完整 `Release/-O3/-DNDEBUG` 构建。
- `10k` 规模下召回率已经接近饱和，说明当前插件在这台机器上的基础构图与查询路径是正常的。
- `100k` 规模下召回率仍然较高，`Recall@10 = 0.9975`，`Recall@100 = 0.9746`，但查询耗时增长明显，吞吐下降到 `5.64 QPS`。
- `1M` 全量脚本下，装载耗时约 `49.7s`，建索引耗时约 `440.3s`，构建完成后的首轮 `200` 条查询耗时约 `118.9s`。
- 同一个 `1M` 索引，紧接着再次执行 query-only 时，`200` 条查询只用了 `421.385ms`，召回率与首轮完全一致。这说明当前环境下存在非常明显的冷态/热态查询性能差异。
- `1M` 规模下，`Recall@10 = 0.986000`，`Recall@100 = 0.940750`，在 `ef_search = 100` 的设置下表现稳定。
- 当前根分区仅剩 `1.8G` 可用空间，建议避免在同一台机器上高频重复做 1M 级别全量 DROP/COPY/CREATE INDEX，避免空间与 I/O 抖动对结果造成额外影响。

## 8. 口径说明

- `10k` 与 `100k` 结果来自标准 benchmark，可视为“新表 + 新索引 + 首轮查询”的统一口径。
- `1M` 全量结果来自 `/tmp/pg_sift1m_bench.py`，同样属于“新表 + 新索引 + 首轮查询”的口径。
- `1M` 的 query-only 复测结果是“已有索引上的热态查询”口径，不能直接与首轮冷态查询时间混为一谈，但它非常适合用来反映缓存预热后的 ANN 查询上限。

## 9. 原始输出

### 9.1 10k

```text
load_ms=454.73
build_ms=2319.69
query_ms=4707.02
qps=42.4897
recall@10=0.9995
recall@100=0.99505
uses_vex_index_scan=true
first_explain_has_index_scan=yes
```

### 9.2 100k

```text
load_ms=4499.11
build_ms=29849.7
query_ms=35467.7
qps=5.63894
recall@10=0.9975
recall@100=0.9746
uses_vex_index_scan=true
first_explain_has_index_scan=yes
```

### 9.3 1M 全量重建

```text
load_ms=49720.795
build_ms=440295.289
query_ms=118939.861
qps=1.682
recall@10=0.986000
recall@100=0.940750
uses_index_scan=true
```

### 9.4 1M 热态 query-only 复测

```text
query_ms=421.385
qps=474.626
recall@10=0.986000
recall@100=0.940750
uses_index_scan=true
```
