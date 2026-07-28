# 编译构建指南

本文档说明如何从源码编译 VexDB 的 PostgreSQL 插件（`vexdb_lite.so`）和 DuckDB 扩展（`vexdb_lite.duckdb_extension`）。

---

## 环境要求

### PostgreSQL 插件

| 工具 | 版本 | 说明 |
|------|------|------|
| CMake | ≥ 3.14 | 构建系统 |
| GCC / G++ | x86: 10.3，ARM: 9.3+ | 仅 Linux；macOS 用 Clang 13+ |
| PostgreSQL | 19（devel） | 需要 server 头文件（`pg_config` 可调用） |
| Boost | vendored 1.91（`thirdparties/boost/`） | 已内置，无需单独安装；Kylin V10 等缺系统 Boost 的机器需额外传 `-DBOOST_FALLBACK_INC` |

### DuckDB 扩展

| 工具 | 版本 | 说明 |
|------|------|------|
| CMake | ≥ 3.28（**< 4.x**） | CMake 4.x 与 DuckDB v1.5.2 `link_threads` 检查不兼容 |
| GCC / G++ | manylinux_2_28 容器内 GCC（自动） | 构建脚本在容器内运行，无需本地安装 |
| DuckDB 源码 | v1.5.2 | 首次构建自动从 GitHub 拉取（~1.5 GB，需数分钟） |

---

## 拉取代码

```bash
git clone https://github.com/VexDB-THU/vexdb_lite.git
cd vexdb_lite
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
# 方式：cmake -D 参数，或通过环境变量
PG_CONFIG=/opt/pg19/bin/pg_config cmake -B build/pg vexdb_pg/
```

**Kylin V10 / 无系统 Boost 环境**，需额外传 `BOOST_FALLBACK_INC`：

```bash
cmake -B build/pg vexdb_pg/ \
    -DBOOST_FALLBACK_INC=/opt/boost_1_90_0
```

### 方式二：顶层 cmake（根目录，仅构建 PG 插件）

```bash
cmake -B build/pg .
cmake --build build/pg -j$(nproc)
```

> 顶层 CMakeLists.txt 是只调 `add_subdirectory(vexdb_pg)` 的薄层分发，**不构建 DuckDB 扩展**。DuckDB 扩展通过 `build_duck.sh` 或 `build.sh` 单独构建。

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
sudo cp vexdb_pg/sql/vexdb_lite--1.0.sql ${PG_SHARE}/vexdb_lite--1.1.sql
sudo cp vexdb_pg/sql/vexdb_lite--1.0--1.1.sql ${PG_SHARE}/
```

### 在 PostgreSQL 中启用

```sql
CREATE EXTENSION vexdb_lite;
SELECT extversion FROM pg_extension WHERE extname = 'vexdb_lite';
```

---

## 编译 DuckDB 扩展

DuckDB 扩展有两套构建路径，用途不同：

### 路径一：build_duck.sh（可加载扩展，生产用）

产出 `vexdb_lite.duckdb_extension`，作为独立文件通过 `LOAD` 加载到任意 DuckDB 实例。

```bash
# Release 构建（首次会自动拉取 DuckDB v1.5.2 源码 ~1.5 GB）
bash build_duck.sh build

# 产物
ls build/duck/build/extension/vexdb_lite/vexdb_lite.duckdb_extension
```

**子命令列表**（不含 `debug`）：

| 子命令 | 说明 |
|--------|------|
| `build` | 编译可加载扩展（Release） |
| `build-unittest` | 编译扩展 + DuckDB unittest runner |
| `unittest [pattern]` | 构建并运行 SQL 测试（自动调 build-unittest） |
| `strip` | 手动执行 strip + footer 重写 |
| `clean` | 清除 `build/duck/build/` |
| `purge` | 清除整个 `build/duck/`（含 DuckDB 源码） |

> **注意**：`build_duck.sh` 没有 debug 子命令，只产出 Release 构建。如需 debug 构建请使用下方 `build.sh`。

**强制全量重建**（增量默认开启，修改代码通常无需全清）：

```bash
bash build_duck.sh clean && bash build_duck.sh build
```

> `CLEAN_DUCK_BUILD=1` 环境变量只在 `scripts/release.sh` 中有效，`build_duck.sh` 本身不识别该变量。

### 路径二：build.sh（DuckDB 整体编译，调试 / 测试用）

将 vexdb_lite 扩展静态链接进 DuckDB 二进制，产出带 vexdb_lite 的 `duckdb` CLI 和 `unittest` runner。适合本地功能调试和运行测试套件。

```bash
# Debug 构建
bash build.sh dev

# Release 构建
bash build.sh release

# 构建并运行所有 vexdb_lite 测试
bash build.sh test

# 只运行匹配 pattern 的测试
bash build.sh test --filter 'graph'

# AddressSanitizer 构建
bash build.sh dev --asan

# 限制并行 job 数
bash build.sh dev -j4
```

产物路径：
- Debug：`build/debug/duckdb`、`build/debug/test/unittest`
- Release：`build/release/duckdb`、`build/release/test/unittest`

运行测试：

```bash
# build.sh test 等价于：
bash build_duck.sh build-unittest
./build/duck/build/test/unittest "test/sql/vex/*"

# 运行特定测试
./build/debug/test/unittest "test/sql/vex/index/graph*"

# 通过 build_duck.sh 快捷方式
bash build_duck.sh unittest 'test/sql/vex/index/graph*'
```

### 加载扩展（路径一产物）

```sql
-- 允许加载未签名扩展（vexdb_lite 扩展当前未签名）
SET allow_unsigned_extensions = true;
LOAD '/path/to/vexdb_lite.duckdb_extension';
SELECT vexdb_version();
```

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
| `vexdb_lite--1.0.sql` | PG 1.0 安装 SQL |
| `vexdb_lite--1.1.sql` | PG 1.1 安装 SQL（默认版本） |
| `vexdb_lite--1.0--1.1.sql` | PG 1.0 到 1.1 升级 SQL |

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

扩展与 DuckDB 版本严格绑定，当前只支持 v1.5.2：

```sql
SELECT version();  -- 确认为 v1.5.2
```

### GLIBCXX 版本不满足

`vexdb_lite.duckdb_extension`（manylinux_2_28 编译）要求 `GLIBCXX_3.4.22`；`vexdb_lite.so`（host GCC 编译）要求 `GLIBCXX_3.4.26`（x86）。CentOS 7 需升级：

```bash
conda install -c conda-forge libstdcxx-ng
```

### Kylin V10 构建报 Boost 找不到

传入系统 Boost 路径作为 fallback：

```bash
cmake -B build/pg vexdb_pg/ -DBOOST_FALLBACK_INC=/opt/boost_1_90_0
```
