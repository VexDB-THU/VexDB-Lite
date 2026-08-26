# community/ — DuckDB Community Extension 分发

本目录隔离 **DuckDB [Community Extensions](https://duckdb.org/community_extensions/)**
分发所需的全部构建配置。日常开发/发版**不经过这里**（那条路走 `build_duck.sh`
和 `scripts/release.sh`）。

## 这套东西是干嘛的

让用户能用**官方 pip duckdb** 一键安装本扩展：

```sql
INSTALL vexdb_lite FROM community;
LOAD vexdb_lite;
```

构建/签名/托管/随新 DuckDB 版本重建，全部由 DuckDB 官方 CI 完成。

## 文件构成（与隔离设计）

community CI 要求构建入口在**仓库根**，所以根目录只放了 2 个薄文件，其余全隔离在此：

| 文件 | 位置 | 作用 |
|---|---|---|
| `Makefile` | 仓库根 | CI 构建入口；准备 DuckDB checkout，`EXT_CONFIG` 指向本目录 |
| `vcpkg.json` | 仓库根 | 声明唯一依赖 `boost-preprocessor`（header-only） |
| `extension_config.cmake` | `community/` | `duckdb_extension_load(vexdb_lite)` |
| `CMakeLists.txt` | `community/` | 扩展构建定义；**引用** `../vexdb_duckdb` + `../common`，源码不搬 |
| `description.yml` | `community/` | 提交到 `duckdb/community-extensions` 的元数据（**发版前填 TODO**）|

- **源码零搬动**：`CMakeLists.txt` 直接引用仓库现有的 `vexdb_duckdb/` 与 `common/`，
  与 `vexdb_duckdb/CMakeLists.txt`（`build_duck.sh` 用）保持同一份源文件清单。
- **duckdb / extension-ci-tools / vcpkg 不进本仓库**：community CI 会 checkout
  `extension-ci-tools`；根目录 `Makefile` 再按 CI 传入的版本浅拉取 `duckdb/`。不需要
  本仓库带 submodule，也不会把 DuckDB 源码放进发布 tag。

## 发布步骤

1. 填好 `description.yml` 里的 `TODO`：`repo.ref`（发布 tag）、`maintainers`、`version`。
2. 把发布 tag push 到 `github.com/VexDB-THU/VexDB-Lite`。
3. 向 [`duckdb/community-extensions`](https://github.com/duckdb/community-extensions)
   提 PR：在 `extensions/vexdb_lite/` 下放入本目录的 `description.yml`
   （注意：community-extensions repo 里目录名必须等于扩展名 `vexdb_lite`）。
4. CI 全平台构建+签名+托管。合并后用户即可 `INSTALL vexdb_lite FROM community`。

> 背景与可行性验证见 `docs/research/2026-06-04_duckdb-extension-pip-distribution.md`。
