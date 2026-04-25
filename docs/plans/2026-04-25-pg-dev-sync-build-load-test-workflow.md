# PG `vexdb-pg` 修改-同步-编译-替换-加载-测试工作流留底

## 目的

记录当前 `vexdb-pg` 在 111 服务器上的一条可重复开发验证路径，避免后续再次出现:

- 改了本地代码，但远端不是最新
- PG 预加载层与 SQL 扩展层使用不同 `.so`
- 构建产物、installed 扩展元数据、运行实例三者不一致
- 改动后无法快速复现 smoke 和 benchmark

本流程默认面向当前验证环境:

- 远端工作区: `/opt/vexdb-lite-build/VexDB-Lite`
- PostgreSQL:
  - `PG_CONFIG=/usr/pgsql-17/bin/pg_config`
  - `PGDATA=/var/lib/pgsql/vexdb-validation/pgdata-smoke`
  - `port=55432`

## 当前约束

### 1. 开发主工作区

本地源码工作区:

- `/Users/sunji/Work/VexDB-Lite`

远端同步工作区:

- `/opt/vexdb-lite-build/VexDB-Lite`

### 2. 当前生效库路径

当前为了让预加载层与 SQL 扩展层一致，系统库已直接覆盖为最新产物:

- 源构建产物:
  - `/opt/vexdb-lite-build/VexDB-Lite/vexdb-pg/pg_vexdb.so`
- 生效系统库:
  - `/usr/pgsql-17/lib/pg_vexdb.so`

### 3. 运行时加载策略

当前 PG 验证实例使用:

- `shared_preload_libraries=pg_vexdb`

不要再同时使用:

- `shared_preload_libraries=/opt/.../pg_vexdb.so`

否则容易与 SQL 扩展对象的 `$libdir/pg_vexdb` 路径形成双重加载，触发:

- GUC 重复注册
- `_PG_init` 重复执行
- `attempt to redefine parameter "pg_vexdb.ef_search"`

## 推荐工作流

### 步骤 1: 本地修改代码

所有源码修改在本地仓库完成，例如:

- [vexdb-pg/Makefile](/Users/sunji/Work/VexDB-Lite/vexdb-pg/Makefile)
- [vexdb-pg/pg_vexdb.control](/Users/sunji/Work/VexDB-Lite/vexdb-pg/pg_vexdb.control)
- [vexdb-pg/test/run_extension_smoke.sh](/Users/sunji/Work/VexDB-Lite/vexdb-pg/test/run_extension_smoke.sh)
- 以及 `vexdb-pg/src/**`, `vexdb-pg/include/**`

原则:

- 先在本地完成单批修改
- 修改后先做本地静态检查，如:
  - `zsh -n`
  - `rg`
  - 必要的 `git diff`

### 步骤 2: 同步到远端工作区

按文件粒度同步到:

- `/opt/vexdb-lite-build/VexDB-Lite`

常用方式:

- `scp` 单文件同步
- 必要时目录级同步

原则:

- 不直接在远端手写源码修补，除非只是临时诊断
- 以本地仓库为唯一源码真相

### 步骤 3: 远端构建 PG 插件

在远端工作区执行:

```bash
cd /opt/vexdb-lite-build/VexDB-Lite/vexdb-pg
export PG_CONFIG=/usr/pgsql-17/bin/pg_config
export BOOST_INCLUDEDIR=/opt/vexdb-lite-build/third_party/boost-1.90.0/include
make clean
make -j4
```

构建后的核心产物:

- `/opt/vexdb-lite-build/VexDB-Lite/vexdb-pg/pg_vexdb.so`

必要校验:

```bash
nm -C pg_vexdb.so | grep " U "
nm -C pg_vexdb.so | grep -E "pg_vexdb_lock_tranche_id|pg_vexdb_relopt_kind|pg_vexdb_session|pg_yield"
```

目的:

- 确认关键兼容符号已定义
- 避免再把明显残缺的 `.so` 推进到替换阶段

### 步骤 4: 替换系统生效库

当前验证环境下，SQL 扩展对象仍以 `$libdir/pg_vexdb` 为主，所以每次新构建后应覆盖:

```bash
install -m 0755 \
  /opt/vexdb-lite-build/VexDB-Lite/vexdb-pg/pg_vexdb.so \
  /usr/pgsql-17/lib/pg_vexdb.so
```

推荐追加校验:

```bash
sha256sum /opt/vexdb-lite-build/VexDB-Lite/vexdb-pg/pg_vexdb.so /usr/pgsql-17/lib/pg_vexdb.so
```

若本轮改动涉及 SQL 扩展元数据，还应同步:

- `/usr/pgsql-17/share/extension/pg_vexdb.control`
- `/usr/pgsql-17/share/extension/pg_vexdb--1.0.sql`

### 步骤 5: 重启/加载 PostgreSQL 验证实例

保持单一路径加载:

```bash
su - postgres -c "/usr/pgsql-17/bin/pg_ctl -D /var/lib/pgsql/vexdb-validation/pgdata-smoke stop -m fast"
su - postgres -c "/usr/pgsql-17/bin/pg_ctl -D /var/lib/pgsql/vexdb-validation/pgdata-smoke \
  -l /var/lib/pgsql/vexdb-validation/pgdata-smoke/server.log \
  -o \"-c port=55432 -c unix_socket_directories=/run/postgresql,/tmp -c shared_preload_libraries=pg_vexdb\" start"
```

重启后建议检查:

```sql
show shared_preload_libraries;
show dynamic_library_path;
```

当前期望值:

- `shared_preload_libraries = pg_vexdb`
- `dynamic_library_path = $libdir`

### 步骤 6: 先跑功能 smoke

优先执行:

- [run_extension_smoke.sh](/Users/sunji/Work/VexDB-Lite/vexdb-pg/test/run_extension_smoke.sh)

远端示例:

```bash
/opt/vexdb-lite-build/VexDB-Lite/vexdb-pg/test/run_extension_smoke.sh \
  "postgresql:///postgres?host=/run/postgresql&port=55432&user=postgres" \
  "/usr/pgsql-17/lib/pg_vexdb" \
  "vexdb_smoke_script"
```

当前仓库脚本允许第二个参数显式校验 `probin` 路径。若当前验证实例以 `$libdir` 为准，可把预期路径设置为:

- `/usr/pgsql-17/lib/pg_vexdb`

若之后重新切回绝对路径 SQL 绑定，则改回对应绝对路径。

### 步骤 7: 再跑 benchmark

在 smoke 通过后再执行:

- [run_sift_sql_benchmark.sh](/Users/sunji/Work/VexDB-Lite/vexdb-pg/test/run_sift_sql_benchmark.sh)

远端示例:

```bash
cd /opt/vexdb-lite-build/VexDB-Lite/vexdb-pg
PG_CONFIG=/usr/pgsql-17/bin/pg_config \
./test/run_sift_sql_benchmark.sh \
  "postgresql:///postgres?host=/run/postgresql&port=55432&user=postgres" \
  10k \
  /opt/vexdb-lite-build/VexDB-Lite/vexdb-duck/test/benchmark/data
```

建议先跑:

- `10k`

待功能和性能稳定后再考虑:

- `100k`

## 当前推荐执行顺序

每一批 PG 改动建议严格按这个顺序走:

1. 本地修改源码
2. 本地静态检查
3. 同步远端工作区
4. 远端 `make -j4`
5. 校验 `.so` 关键符号
6. 覆盖 `/usr/pgsql-17/lib/pg_vexdb.so`
7. 必要时同步 installed control / sql
8. 重启 PG 验证实例
9. 跑 PG 功能 smoke
10. 跑 PG `10k` benchmark

## 常见坑位

### 1. 预加载层与 SQL 层走不同 `.so`

错误形态:

- `shared_preload_libraries=/opt/.../pg_vexdb.so`
- SQL 函数对象仍然走 `$libdir/pg_vexdb`

后果:

- 同一扩展被双重加载
- GUC 重复注册
- `_PG_init` 重复执行

### 2. 只同步源码，不替换系统库

错误形态:

- `/opt/.../pg_vexdb.so` 是新的
- `/usr/pgsql-17/lib/pg_vexdb.so` 仍是旧的

后果:

- benchmark / 普通 SQL 依然会加载旧库
- 很容易误判成“代码没修好”

### 3. 只重启 PG，不核对运行参数

建议每次重启后都检查:

- `show shared_preload_libraries`
- `show dynamic_library_path`
- 必要时看 `/proc/<postmaster>/maps`

### 4. 不先跑 smoke 就直接跑 benchmark

这样会浪费大量时间在大数据阶段才发现路径或扩展装载问题。

正确顺序是:

- 先 smoke
- 后 benchmark

## 当前最小回归门槛

每一批 PG 改动完成后，至少需要满足:

1. PG 实例可正常启动
2. `CREATE EXTENSION pg_vexdb` 可用
3. 最小 smoke 可通过
4. `10k` benchmark 可完整跑通
5. `EXPLAIN` 确认走 `vexdb_graph` 索引

## 与性能主线的关系

当前这条工作流的目的不是替代性能优化，而是为性能优化提供稳定底座。

只有当这条链路稳定后，后续 PG 构建性能优化结果才有可比性，否则:

- benchmark 会混入路径错误
- smoke 会混入部署错误
- 最终难以区分“算法慢”还是“部署错”
