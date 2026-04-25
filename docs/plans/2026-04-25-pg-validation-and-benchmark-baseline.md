# PG 验证与 Benchmark 基线留底

## 目标

记录 2026-04-25 这轮 `vexdb-pg` 在 PostgreSQL 17.9 验证环境上的关键验证结果，形成后续 PG 适配层与性能优化的基线。

本轮重点不是继续改算法，而是先把以下链路打通并固化:

- PG 预加载路径可用
- SQL 扩展对象与预加载对象不再错绑到不同 `.so`
- 最小功能 smoke 可重复执行
- `10k` SIFT SQL benchmark 能完整跑通，并给出当前性能画像

## 验证环境

- 远端服务器: `root@172.16.203.111`
- 远端工作目录: `/opt/vexdb-lite-build/VexDB-Lite`
- PostgreSQL:
  - `PG_CONFIG=/usr/pgsql-17/bin/pg_config`
  - 版本: `17.9`
- 验证实例:
  - `PGDATA=/var/lib/pgsql/vexdb-validation/pgdata-smoke`
  - `port=55432`
- 当前生效库:
  - `/usr/pgsql-17/lib/pg_vexdb.so`
- 当前源码/构建产物:
  - `/opt/vexdb-lite-build/VexDB-Lite/vexdb-pg/pg_vexdb.so`

## 关键修复结论

### 1. PGXS Makefile 漏编单元问题已清理

在 PGXS `Makefile` 路径中，之前相对 `CMakeLists.txt` 存在多个漏编单元，直接导致 `.so` 装载时持续出现 `undefined symbol`。

最终补齐并验证过的关键编译单元包括:

- `src/guc_config.cpp`
- `src/pq.cpp`
- `src/rabitq_distancer.cpp`
- `src/quantizer_stubs.cpp`
- `src/pg_yield.cpp`
- `src/bulkbuf_smgr.cpp`
- `src/shared_alloc_set.cpp`
- `src/vecbuf_worker.cpp`
- `src/distance/sse_dispatcher.cpp`
- `src/distance/avx_dispatcher.cpp`
- `src/distance/avx512_dispatcher.cpp`

PGXS 构建完成后，目标缺失符号面已清空，后续问题转为部署与运行期路径一致性，而不再是单纯漏编译。

### 2. SQL 层与预加载层曾存在“双路径装载”

曾经同时存在两条库装载路径:

- `shared_preload_libraries=/opt/vexdb-lite-build/VexDB-Lite/vexdb-pg/pg_vexdb.so`
- SQL 扩展对象通过 `MODULE_PATHNAME -> $libdir/pg_vexdb`

这导致:

- 预加载层走 `/opt/.../pg_vexdb.so`
- SQL 函数层走 `/usr/pgsql-17/lib/pg_vexdb.so`

结果是:

- 一边修好的新库可以启动 PG
- 另一边旧库在 `CREATE EXTENSION` / 普通 SQL 时仍然报 `undefined symbol`

### 3. 最终采用的路径对齐方案

本轮为先恢复整体可用性，采用了最直接的收敛方式:

1. 将最新构建产物覆盖到系统库路径:
   - `/opt/vexdb-lite-build/VexDB-Lite/vexdb-pg/pg_vexdb.so`
   - 覆盖到 `/usr/pgsql-17/lib/pg_vexdb.so`
2. 重启 PostgreSQL 验证实例时，将预加载路径切回:
   - `shared_preload_libraries=pg_vexdb`
3. 让 SQL 扩展对象与预加载对象统一走:
   - `$libdir/pg_vexdb`

这样可以避免:

- 同一扩展被绝对路径与 `$libdir` 路径重复加载
- `_PG_init` 中 GUC 重复注册
- `attempt to redefine parameter "pg_vexdb.ef_search"`

## 功能 Smoke 基线

### 新增脚本

新增 PG 轻量功能回归脚本:

- [run_extension_smoke.sh](/Users/sunji/Work/VexDB-Lite/vexdb-pg/test/run_extension_smoke.sh)

脚本覆盖:

- 重建临时测试库
- `CREATE EXTENSION pg_vexdb`
- 校验访问方法 `vexdb_graph`
- 校验 `pg_proc.probin` 指向目标模块路径
- 建最小 `floatvector` 表
- 插入样本
- 建 `vexdb_graph` 索引
- `EXPLAIN` 检查 `Index Scan`
- 查询结果检查 top-k 顺序

### 远端验证命令

```bash
/opt/vexdb-lite-build/VexDB-Lite/vexdb-pg/test/run_extension_smoke.sh \
  "postgresql:///postgres?host=/run/postgresql&port=55432&user=postgres" \
  "/opt/vexdb-lite-build/VexDB-Lite/vexdb-pg/pg_vexdb" \
  "vexdb_smoke_script"
```

### Smoke 实际结果

脚本成功输出:

```text
extension=pg_vexdb
access_method=vexdb_graph
probin=/opt/vexdb-lite-build/VexDB-Lite/vexdb-pg/pg_vexdb
plan_ok=1
result_rows=2,1
```

说明:

- `pg_vexdb` 扩展可创建
- `vexdb_graph` 访问方法存在
- SQL 函数可正确绑定目标 `.so`
- 最小建表、建索引、计划、查询链路可用

## 10k SIFT SQL Benchmark 基线

### 运行命令

```bash
cd /opt/vexdb-lite-build/VexDB-Lite/vexdb-pg
PG_CONFIG=/usr/pgsql-17/bin/pg_config \
./test/run_sift_sql_benchmark.sh \
  "postgresql:///postgres?host=/run/postgresql&port=55432&user=postgres" \
  10k \
  /opt/vexdb-lite-build/VexDB-Lite/vexdb-duck/test/benchmark/data
```

### 实际结果

```text
load_ms=458.744
build_ms=193994
query_ms=1883.6
qps=106.18
recall@10=1
recall@100=0.99505
uses_vex_index_scan=true
first_explain_has_index_scan=yes
```

### benchmark 结论

- benchmark 已能完整跑通
- SQL 查询计划确认走向量索引
- 召回率正常:
  - `recall@10=1`
  - `recall@100=0.99505`
- 当前主要问题已经转为 **PG 构建性能偏慢**

## 当前性能画像

这轮 benchmark 最值得关注的是 PG core bridge 构建统计中的节点读放大:

```text
points=10000
add_point_total_ms=193896.622
add_point_avg_ms=19.390
pin_read_calls=81848816
pin_read_ms=178562.483
pin_write_calls=350439
pin_write_ms=616.657
pin_new_ms=36364.525
lower_search_ms=43375.978
backlink_ms=107061.922
prune_ms=85789.128
```

可见当前 PG 构建瓶颈集中在:

- `pin_read_calls` 极高
- `pin_read_ms` 极高
- `pin_new_ms` 明显偏大
- `backlink` / `prune` / `lower_search` 路径成本叠加明显

这与之前单独分析 `pin_read` 的判断一致，说明后续性能优化应继续围绕:

- 减少节点页重复读取
- 减少构建阶段高频 pin/unpin
- 降低 level0 / upper 层读放大
- 优化 bridge 路径下的构建态访问局部性

## 当前可重复回归入口

### 功能 smoke

- [run_extension_smoke.sh](/Users/sunji/Work/VexDB-Lite/vexdb-pg/test/run_extension_smoke.sh)

### benchmark

- [run_sift_sql_benchmark.sh](/Users/sunji/Work/VexDB-Lite/vexdb-pg/test/run_sift_sql_benchmark.sh)

## 后续建议

当前主线已经从“修到能跑”切到“在 PG 路径下把构建做快”。后续建议按以下顺序推进:

1. 保持 smoke 和 `10k` benchmark 作为每批修改后的回归门槛
2. 继续围绕 `pin_read_calls / pin_read_ms / pin_new_ms` 做 PG 存储访问优化
3. 每次优化后记录 benchmark 数值变化，避免只看局部日志不看总收益
