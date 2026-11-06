# 历史工作流归档

归档日期：2026-10-26。按用户选择保留原文；GitHub Actions 只读取 `.github/workflows/`，不会执行这里的文件。

| 文件 | 归档原因 | 当前替代入口 |
|---|---|---|
| `build-linux.yml` | 旧根 `duckdb/`、退役测试路径、缺失 Java/Node 打包脚本 | 当前 `build-linux.yml`、`_linux-tests.yml` |
| `build-mobile.yml` | 旧 DuckDB fork 构建和头文件目录 | 当前 SQLite Android 库工作流 |
| `build-wheels.yml` | OEM wheel 依赖已删除的 fork 源码和 `packaging/` | 当前独立 DuckDB 扩展构建；不产出 OEM wheel |
| `build-windows.yml` | 根目录没有 CMakeLists.txt，原生 Windows 适配未完成 | 暂无；保留为待恢复能力 |
| `_extension_client_tests.yml` | 假定本仓库是 DuckDB 源码树，调用不存在的 Python client target | 当前 spec 回归 |
| `_extension_distribution.yml` | 把本仓库提交当作 DuckDB 提交，调用上游部署配置 | 当前 Community `Makefile` 和 `community/` |
| `_manual_extension_deploy.yml` | 引用不存在的配置、上传脚本和 DuckDB 专用部署凭证 | 正式发布入口 `scripts/release.sh`；Community 分发另行验证 |

现有 Java/Node 包、OEM wheel、原生 Windows 二进制和自动 AWS 部署不能被这些归档文件证明仍然可用。
恢复时应先补源码与测试，再新增适合本仓库的工作流。
