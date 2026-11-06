# 当前代码与 CI 状态

更新日期：2026-10-26。本文按当前工作区的源码和工作流记录；远端 CI、PR、tag 和 Release 状态需要单独查询。

## 项目范围

| 模块 | 当前代码入口 | 构建或测试入口 |
|---|---|---|
| 共享内核 | `common/`：图索引、SIMD 距离计算、量化与容器 | 由三个数据库适配层分别编译 |
| PostgreSQL | `vexdb_pg/`：类型、索引、DML、并行构建、WAL | `cmake -S vexdb_pg -B build/pg`；PG Docker spec runner |
| DuckDB | `vexdb_duckdb/`：GRAPH_INDEX、查询改写、持久化、PQ | `build_duck.sh`；DuckDB spec runner |
| SQLite | `vexdb_sqlite/`：GRAPH_INDEX 虚拟表、静态/动态扩展、VexFS SQL | `build_sqlite.sh test` |
| VexFS | `agent_files/`：统一 CLI、版本、快照、运行接口、平台挂载 | SQLite/VexFS 冒烟；`tests/eval/vexfs/` 场景验证 |
| Community 分发 | 根 `Makefile` 和 `community/` | `tests/community/test_makefile_checkout.sh`；正式分发由 DuckDB Community CI 验证 |

`common/` 中有量化实现，不表示三个适配层已经开放完全相同的量化功能。
SQLite 本文只确认 GRAPH_INDEX 与距离函数入口；PG/DuckDB 的 PQ 选项见各自说明。
RaBitQ 源码存在也不能当作三个适配层均已通过验收。

## 版本与目录

- PostgreSQL 适配范围是 16–19。当前 PR CI 选用 `REL_19_STABLE` 构建 PG 测试镜像，不使用会随大版本变化的 `master`。
- DuckDB 默认目标是 `v1.5.2`。源码在 `build/duck/v1.5.2/duckdb_src/`，产物在 `build/duck/v1.5.2/build/`；改变 `DUCKDB_VERSION` 后路径随之变化。
- DuckDB C++ 扩展与运行时版本严格绑定。历史记录有 v1.5.2/v1.5.5 的 macOS arm64 本地构建冒烟；这不代表其他版本、平台或完整 spec 已通过。
- SQLite 默认 vendor 版本是 3.45.3，构建产物在 `vexdb_sqlite/build/`。动态扩展加载宿主的硬下限是 3.24；移动端使用静态注册，详见 [SQLite 说明](../vexdb_sqlite/README.md)。
- 根目录没有 CMakeLists.txt。根 `Makefile` 是 Community CI 入口，需要 `extension-ci-tools/`；日常构建使用各自脚本或子目录 CMake。

## 当前工作流

| 文件 | 触发方式 | 实际工作 |
|---|---|---|
| `.github/workflows/pr-regression.yml` | PR、main push、手工 | actionlint、五种目标的 spec 渲染、Community 检出测试，然后运行三个数据库的 Linux 回归 |
| `.github/workflows/_linux-tests.yml` | 被其他工作流调用 | DuckDB 构建/spec、SQLite 构建/冒烟/spec，可选 PG 镜像构建/spec |
| `.github/workflows/build-linux.yml` | v* tag、手工 | 复用上述工作流，在 Linux x86_64 和 aarch64 分别构建并测试 DuckDB、SQLite |
| `.github/workflows/build-mobile.yml` | v* tag、手工 | 从 `vexdb_sqlite/` 交叉编译 Android arm64-v8a、x86_64 的动态扩展与静态库，附公共头文件 |
| `.github/workflows/duckdb-crash-recovery.yml` | 手工 | 执行已有 `build_duck.sh crash-recovery` harness |

Linux、Android 工作流上传的是 CI artifact，不自动创建或修改 GitHub Release。
Linux CI 使用 Ubuntu 工具链，不能据此声明产物满足 manylinux 或既有 Release 的 GLIBCXX 上限。
正式发布仍走 `scripts/release.sh`，并执行该脚本的产物与兼容性检查。

默认 shell 显式设为 bash，让命令或管道失败能够使步骤失败。构建产物不存在时上传步骤会失败，PG 诊断日志不存在时只给提示。
Android 流程只编译库，不代表模拟器或真机运行验证；iOS 仍使用本地构建入口。
仓库工作流存在，也不表示 GitHub 分支保护已把它设为必需检查。

## 归档范围

旧流程原文保存在 [`.github/legacy-workflows/`](../.github/legacy-workflows/README.md)，GitHub Actions 不执行该目录：

- Java/Node 打包和 OEM Python wheel 依赖已删除的 `packaging/` 或 DuckDB fork 源码；本轮按用户选择归档。
- Windows 旧流程在仓库根执行 CMake；当前缺少原生 Windows 适配与验证，因此归档，不能当作当前支持平台。
- 旧 DuckDB 上游 client/deploy 工作流依赖不存在的根 `duckdb/`、配置和上传脚本，也一并归档。
- 原 Linux、mobile 工作流保留副本；当前流程已按本仓库的三个适配层组织。

重新开放这些流程时，需要恢复源码、打包入口和实际测试。不能只把归档文件移回去。

## 检查与剩余事项

本轮本地验证记录：

| 检查 | 结果与边界 |
|---|---|
| actionlint 1.7.7、YAML、各 run 步骤的 bash 语法、脚本与可复用工作流路径 | 通过 |
| Community Makefile 检出测试 | 通过；只验证本地测试仓库，不证明远端 Community CI 已通过 |
| 全目标 spec 渲染 | 通过，生成 689 个文件 |
| macOS SQLite/VexFS 构建与冒烟 | 通过；SQLite spec 为 33 passed、0 failed |
| Ubuntu 24.04 aarch64、GCC 13、Release SQLite | 完整构建/冒烟入口通过，包含 FUSE helper 自检、两个 CLI 冒烟；SQLite spec 为 33 passed、0 failed |
| Android NDK r27、API 24、arm64-v8a/x86_64 | 动态扩展与静态库交叉编译通过；未执行设备测试 |

实跑还修正了两类测试问题：ANN 冒烟原本误要求精确 top1，现在保持原有 0.9 召回门槛，
逐条核对返回 rowid 的标量距离、排序和重复行；CLI 冒烟改用跨 macOS/Linux 的
mktemp/stat 写法，并避免 `grep -q` 在 pipefail 下引起偶发 SIGPIPE。
新距离检查在临时注入错误距离时会失败；本次没有修改检索算法或搜索参数。
PG、DuckDB 的完整构建/spec 与远端 GitHub Actions 本轮尚未运行。

当前 spec 物理目录包含 219 份业务 YAML：shared 26、duckdb 88、pg 74、sqlite 30、opengauss 1。
渲染器为 DuckDB、PG、openGauss、SQLite、Python 收集的数量分别为 114、100、101、56、114；
这些数量包含跨引擎复用和 skip，不是运行通过数。最新执行结果以本轮报告和实际 CI 为准。

- 本次本地修复记录保存在 `docs/reports/2026-07-13_project-gap-analysis.md`；该目录按项目规则只在本机保留，本文单独记录可随仓库交付的状态。
- 旧计划中的 `scripts/pq_recall_bench.py` 在当前分支不存在，不能继续记为已交付。
- 远端 Community PR、跨平台完整 spec、性能和真实挂载验收，不由本次路径修复推断。

完整命令见 [构建指南](build-guide.md) 和 [测试框架](../tests/spec/README.md)。
