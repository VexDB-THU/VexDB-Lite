# 数据库适配版本

本文档列出 VexDB 各产物所支持的数据库版本、操作系统和 CPU 架构。

> 2026-10-26 校正：数据库目标和当前 CI 范围如下。历史系统、架构和库依赖表保留为既有产物记录，不能代表当前提交重新通过了全部平台验收。当前 CI artifact 与正式 Release 的兼容性承诺分开，详见 [当前状态](current-status.md)。

---

## 支持矩阵

### PostgreSQL 插件（`vexdb_lite.so`）

| 数据库 | 支持版本 | 说明 |
|--------|----------|------|
| PostgreSQL | **16 / 17 / 18 / 19** | 已适配，主验证平台为 PG 19 |
| openGauss | — | 参见 VexDB 主库 |

### DuckDB 扩展（`vexdb_lite.duckdb_extension`）

| 数据库 | 支持版本 | 说明 |
|--------|----------|------|
| DuckDB | **v1.5.2** | 默认构建与当前 spec CI 目标 |
| DuckDB | v1.5.5 | 历史记录有 macOS arm64 构建与冒烟，完整跨平台 spec 未确认 |
| DuckDB | 其他版本 | 需要分别构建和验证，不能复用其他版本扩展 |

> DuckDB 扩展与运行时版本严格绑定。加载前请通过 `SELECT version()` 确认与构建目标完全相同。

### SQLite 扩展与统一 CLI

默认 vendor SQLite 3.45.3；动态扩展宿主最低 3.24。SQLite 构建有 Linux、macOS、Android 和 iOS 入口，但构建入口存在不等于设备与发布验收完成。
Android CI 编译 arm64-v8a/x86_64 库；iOS 使用本地静态库构建。
原生 Windows 工作流已归档，尚无支持承诺。

---

## 操作系统兼容性

### Linux

| 发行版 | 架构 | 状态 | 说明 |
|--------|------|------|------|
| Kylin V10 SP1 / SP3 | aarch64 | ✅ 已验证 | 主要测试平台（ARM） |
| Ubuntu 22.04 | aarch64 | ✅ 已验证 | ARM 性能基准测试平台 |
| Ubuntu 22.04 | x86_64 | ✅ 已验证 | x86 性能基准测试平台 |
| CentOS 8 | x86_64 | ✅ 已验证 | manylinux_2_28 兼容 |
| CentOS 7 | x86_64 | ⚠️ 需升级 libstdc++ | 默认 GLIBCXX 3.4.19，需升级 |
| 其他 glibc ≥ 2.28 | x86_64 / aarch64 | ✅ 应兼容 | 未逐一验证 |

### macOS

| 版本 | 架构 | 状态 | 说明 |
|------|------|------|------|
| macOS 14 / 15 | arm64 (Apple Silicon) | 🔧 源码构建 | 开发者本地构建可用，不在 Release 中发布 |
| macOS 13 | x86_64 | 🔧 源码构建 | 同上 |

> 上表保留的是 PG/DuckDB 的历史说明。SQLite/VexFS 另有 macOS 统一技术预览包，默认 NFS 路径最低目标为 macOS 13，FSKit 为 macOS 26+ 的可选路径。实际安装、签名与公证状态以对应包的 MANIFEST 和验证记录为准；本次未查询远端 Release。

---

## CPU 架构

| 架构 | SIMD 优化 | 最低 CPU 要求 | 说明 |
|------|-----------|--------------|------|
| x86_64 | SSE4.1 / AVX2 / AVX-512 | SSE4.1 | 运行时动态检测最优实现 |
| AArch64 | NEON（ARMv8.2 dotprod） | **ARMv8.2+** | 运行时动态检测；ARMv8.0（如 Cortex-A53/A57）因 LSE 指令不兼容，会触发 SIGILL |
| AArch64 SVE | 代码已就位，默认构建未启用 | — | 默认 `-march=armv8.2-a+lse+fp16+dotprod+crc`，无 `+sve` |

> 距离计算采用运行时 SIMD dispatch 机制，无需重新编译即可利用当前 CPU 的最高 SIMD 级别。

---

## 系统库要求

### glibc

| 产物 | 最低 glibc |
|------|-----------|
| DuckDB 扩展（Linux） | **2.28** |
| PG 插件（Linux） | **2.28** |

上表对应历史发布链路。当前 Linux CI 使用 Ubuntu 24.04，上传的 CI artifact 不承诺可在 glibc 2.28 上运行；正式分发需走发布脚本并核对实际依赖。

### libstdc++ / GLIBCXX

| 产物 | GLIBCXX 依赖上限 | 编译工具链 |
|------|-----------------|------------|
| `vexdb_lite.duckdb_extension` | `GLIBCXX_3.4.22` | manylinux_2_28 容器 GCC；硬校验，超出即拦截 |
| `vexdb_lite.so`（x86） | `GLIBCXX_3.4.26` | x86 host GCC 10.3 |
| `vexdb_lite.so`（ARM） | `GLIBCXX_3.4.26`（或更低） | ARM host GCC 9.3（Kylin V10 SP1 系统） |

> 上表是历史工具链记录。发布脚本中的 DuckDB GLIBCXX 上限校验仍需执行；PG、SQLite 和具体 Release 的实际依赖应检查对应产物，不能从 CI 编译成功推断。

CentOS 7 默认 libstdc++ 为 GLIBCXX_3.4.19，需手动升级：

```bash
conda install -c conda-forge libstdcxx-ng
```

---

## 已知不兼容场景

| 场景 | 状态 | 原因 |
|------|------|------|
| DuckDB 运行时与构建版本不同 | ❌ 不支持 | 扩展 API 与 metadata footer 随版本变化，必须分别构建 |
| PostgreSQL < 16 或 > 19 | ❌ 不支持 | 仅适配 PG 16-19 |
| ARMv8.0 及更早（如 Cortex-A53/A57） | ❌ 不支持 | 构建使用 `+lse` 扩展指令，旧核触发 SIGILL |
| Windows | ❌ 暂不支持 | 原生适配未完成，旧工作流已归档 |
| CentOS 7 默认环境 | ⚠️ 需手动升级 libstdc++ | GLIBCXX 版本低 |
| CMake 4.x 构建 DuckDB 扩展 | ❌ 不兼容 | CMake 4.x 与 DuckDB v1.5.2 `link_threads` 检查冲突；请用 3.28.x |
