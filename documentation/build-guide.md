# 编译构建指南

本文档说明 PostgreSQL、DuckDB、SQLite 三个适配层的实际构建和测试入口。当前 CI 范围见 [当前状态](current-status.md)。根目录没有 CMakeLists.txt，不能在根目录执行 `cmake -S .`。

---

## 环境要求

### PostgreSQL 插件

| 工具 | 版本 | 说明 |
|------|------|------|
| CMake | ≥ 3.14 | 构建系统 |
| GCC / G++ | x86: 10.3，ARM: 9.3+ | 仅 Linux；macOS 用 Clang 13+ |
| PostgreSQL | 16 / 17 / 18 / 19 | 需要对应版本的 server 头文件（`pg_config` 可调用） |
| Boost | vendored 1.91（`thirdparties/boost/`） | 已内置，无需单独安装；Kylin V10 等缺系统 Boost 的机器需额外传 `-DBOOST_FALLBACK_INC` |

### DuckDB 扩展

| 工具 | 版本 | 说明 |
|------|------|------|
| CMake | ≥ 3.28（**< 4.x**） | CMake 4.x 与 DuckDB v1.5.2 `link_threads` 检查不兼容 |
| C++17 编译器 | GCC / Clang | `build_duck.sh` 在本机运行，需要本机编译器与 Boost 头文件 |
| DuckDB 源码 | v1.5.2 | 首次构建自动从 GitHub 拉取（~1.5 GB，需数分钟） |

---

## 拉取代码

```bash
git clone https://github.com/VexDB-THU/VexDB-Lite.git
cd VexDB-Lite
```

---

## 编译 PostgreSQL 插件

### 方式一：直接 cmake（推荐）

```bash
cmake -B build/pg vexdb_pg/ -DCMAKE_BUILD_TYPE=Release
cmake --build build/pg -j$(nproc)

# 产物
ls build/pg/vexdb_lite.so
```

CMake 自动调用 `pg_config` 查找 PostgreSQL 头文件。PG 在非标准路径时手动指定：

```bash
# 通过环境变量指定实际 pg_config
PG_CONFIG=/opt/pg19/bin/pg_config cmake -B build/pg vexdb_pg/
```

**Kylin V10 / 无系统 Boost 环境**，需额外传 `BOOST_FALLBACK_INC`：

```bash
cmake -B build/pg vexdb_pg/ \
    -DBOOST_FALLBACK_INC=/opt/boost_1_90_0
```

### 安装到 PG 扩展目录

```bash
# 推荐：cmake --install 自动用构建时配置的路径
sudo cmake --install build/pg

# 或手动拷贝
PG_PKGLIB=$(pg_config --pkglibdir)
PG_SHARE=$(pg_config --sharedir)/extension
sudo cp build/pg/vexdb_lite.so ${PG_PKGLIB}/
sudo cp vexdb_pg/vexdb_lite.control ${PG_SHARE}/
sudo cp vexdb_pg/sql/vexdb_lite--1.0.sql ${PG_SHARE}/
```

### 在 PostgreSQL 中启用

```sql
-- 先在 postgresql.conf 中配置 shared_preload_libraries = 'vexdb_lite' 并重启
CREATE EXTENSION vexdb_lite;
SELECT extversion FROM pg_extension WHERE extname = 'vexdb_lite';
```

---

## 编译 DuckDB 扩展

DuckDB 扩展有两套构建路径，用途不同：

### 路径一：build_duck.sh（可加载扩展，生产用）

产出 `vexdb_lite.duckdb_extension`，作为独立文件通过 `LOAD` 加载到相同版本的 DuckDB 实例。

```bash
# Release 构建（首次会自动拉取 DuckDB v1.5.2 源码 ~1.5 GB）
bash build_duck.sh build

# 产物
ls build/duck/v1.5.2/build/extension/vexdb_lite/vexdb_lite.duckdb_extension
```

**子命令列表**（不含 `debug`）：

| 子命令 | 说明 |
|--------|------|
| `build` | 编译可加载扩展（Release） |
| `build-unittest` | 编译扩展 + DuckDB unittest runner |
| `unittest [pattern]` | 构建并运行 SQL 测试（自动调 build-unittest） |
| `strip` | 手动执行 strip + footer 重写 |
| `clean` | 清除 `build/duck/v1.5.2/build/` |
| `purge` | 清除当前版本的 `build/` 和 `duckdb_src/`，保留共享数据 |

> **注意**：`build_duck.sh` 没有 debug 子命令，只产出 Release 构建。如需 debug 构建请使用下方 `build.sh`。

**强制全量重建**（增量默认开启，修改代码通常无需全清）：

```bash
bash build_duck.sh clean && bash build_duck.sh build
```

> `CLEAN_DUCK_BUILD=1` 环境变量只在 `scripts/release.sh` 中有效，`build_duck.sh` 本身不识别该变量。

### Debug 构建与测试

`build.sh` 使用 `build_duck.sh setup` 准备的源码树，Debug 构建入口是：

```bash
bash build_duck.sh setup
bash build.sh dev -j4
# Debug runner：build/duck/v1.5.2/duckdb_src/build/debug/test/unittest
```

功能回归统一使用 spec runner，避免直接运行未部署用例的 unittest：

```bash
python3 -m pip install -r tests/spec/requirements.txt
bash tests/spec/_lib/docker/run_duckdb.sh test
bash tests/spec/_lib/docker/run_duckdb.sh test '*graph_index_ann*'
```

DuckDB runner 在本地运行，不需要 Docker。它会渲染、部署用例并构建
`build/duck/v1.5.2/build/test/unittest`，默认执行 `[spec_run]`。
设置 `DUCKDB_VERSION` 后，源码和产物路径中的版本也相应变化。

### 加载扩展（路径一产物）

```sql
-- 使用 duckdb -unsigned 启动，或在客户端连接配置中允许未签名扩展
LOAD '/path/to/vexdb_lite.duckdb_extension';
SELECT vexdb_version();
```

---

## 编译 SQLite 与 VexFS

需要 CMake ≥ 3.14、C++17 编译器和 Boost 头文件。脚本会下载并校验固定版本的 SQLite
amalgamation。Linux 安装 `libfuse3-dev` 可额外构建 FUSE helper；桌面连接 PostgreSQL
后端还需要 libpq 开发文件。

```bash
bash build_sqlite.sh build
bash build_sqlite.sh test
# 产物目录：vexdb_sqlite/build/
# 动态扩展：vexdb_lite.so（Linux）或 vexdb_lite.dylib（macOS）
# 静态库：libvexdb_lite_static.a
# 统一命令：vexdb，兼容文件入口：vexfs
```

`test` 包括向量、VexFS、CLI 冒烟和 SQLite spec。已有构建可单独运行 spec：

```bash
bash tests/spec/_lib/docker/run_sqlite.sh
```

SQLite runner 同样不需要 Docker。macOS 预览包、Android 和 iOS 的入口分别为
`agent_files/macos/package_preview.sh`、`bash build_sqlite.sh android` 和
`bash build_sqlite.sh ios`；交叉编译结果需要独立进行设备测试。

## PostgreSQL spec

```bash
python3 -m pip install -r tests/spec/requirements.txt
python3 tests/spec/_lib/render.py --engine pg --out build/spec
# 使用 PG 19 稳定分支，避免 master 后续切换到下一个大版本
BUILDX_EXTRA_ARGS='--build-arg PG_GIT_TAG=REL_19_STABLE' \
  bash tests/spec/_lib/docker/run_pg.sh build
bash tests/spec/_lib/docker/run_pg.sh test
bash tests/spec/_lib/docker/run_pg.sh down
```

只有 PG runner 使用 Docker。完整命令和过滤方式见 [测试框架](../tests/spec/README.md)。

---

## 发版构建（x86 + ARM 双平台，远程构建机）

`scripts/release.sh` 是全自动双平台发版流水线，**通过 SSH 远程连接 x86 / ARM 构建机**，在 manylinux_2_28 容器内编译，产出跨发行版兼容包。**需要有构建机的 SSH 访问权限**。

三个阶段相互独立，必须按顺序执行：

```bash
# 阶段 1：构建 x86 + ARM 双平台 PG + DuckDB 插件
# （SSH 到远程机，容器内编译，产物同步回 dist/）
MANYLINUX=1 bash scripts/release.sh build

# 阶段 2：在远程机运行 spec 测试套件（依赖阶段 1 产物）
bash scripts/release.sh test

# 阶段 3：打包 dist/ 下产物为 tar.gz + SHA256SUMS（依赖阶段 1 产物）
bash scripts/release.sh package
```

> `test` 和 `package` 均**不会**自动触发 `build`。在 `dist/` 没有产物时直接调用会失败。

### 产物清单

每个 `dist/<arch>-linux/` 目录包含：

| 文件 | 说明 |
|------|------|
| `vexdb_lite.duckdb_extension` | DuckDB 可加载扩展（stripped） |
| `vexdb_lite.duckdb_extension.unstripped` | 含调试符号的未 strip 版本 |
| `vexdb_lite.so` | PG 插件（stripped，含 `.gnu_debuglink`） |
| `vexdb_lite.so.debug` | PG 插件的独立 debug symbols（split-debug） |
| `vexdb_lite.control` | PG 控制文件 |
| `vexdb_lite--1.0.sql` | PG 安装 SQL |

打包后的 Release tarballs（`dist/release/`）：

```
vex-duckdb-linux-x86_64.tar.gz
vex-duckdb-linux-aarch64.tar.gz
vex-duckdb-debugsymbols-linux-x86_64.tar.gz
vex-duckdb-debugsymbols-linux-aarch64.tar.gz
vexdb_lite-linux-x86_64-pg19.tar.gz
vexdb_lite-linux-aarch64-pg19.tar.gz
vexdb_lite-debugsymbols-linux-x86_64-pg19.tar.gz
vexdb_lite-debugsymbols-linux-aarch64-pg19.tar.gz
SHA256SUMS.txt
```

---

## 常见问题

### `pg_config: command not found`

将 PostgreSQL bin 目录加入 PATH：

```bash
export PATH=/opt/pg19/bin:$PATH
```

### `fatal error: postgres.h: No such file or directory`

未安装 PostgreSQL 开发头文件（Ubuntu/Debian）：

```bash
apt install postgresql-server-dev-19
```

### DuckDB 扩展加载报 `IO Error: The file is not a DuckDB extension`

扩展与 DuckDB 版本严格绑定，默认构建目标是 v1.5.2。切换版本必须重新构建对应产物：

```sql
SELECT version();  -- 确认为 v1.5.2
```

### GLIBCXX 版本不满足

以下是历史发布产物的记录，当前 CI artifact 不承诺同等兼容性，实际以发布校验为准。

`vexdb_lite.duckdb_extension`（manylinux_2_28 编译）要求 `GLIBCXX_3.4.22`；`vexdb_lite.so`（host GCC 编译）要求 `GLIBCXX_3.4.26`（x86）。CentOS 7 需升级：

```bash
conda install -c conda-forge libstdcxx-ng
```

### Kylin V10 构建报 Boost 找不到

传入系统 Boost 路径作为 fallback：

```bash
cmake -B build/pg vexdb_pg/ -DBOOST_FALLBACK_INC=/opt/boost_1_90_0
```
