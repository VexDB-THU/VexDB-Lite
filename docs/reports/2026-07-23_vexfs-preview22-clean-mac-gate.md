# VexFS preview.22 无源码 M1 真机 Gate 报告

- 日期：2026-07-23
- 分支：`feature/agent_files`
- 源码 commit：`8a8b0c139a`
- 版本：`0.1.0-preview.22`
- SQLite 合同：`0.9.0`
- runtime ABI：1
- 测试机：Apple M1，macOS 26.5.2

## 1. 结论

preview.22 已完成 Developer ID 签名、Apple 公证、staple、Gatekeeper、包内安装检查，
并在另一台没有 VexDB 源码、只有 Command Line Tools 的 M1 上覆盖安装和真实 FSKit 挂载。

无源码真机 smoke 最终为 `PASS (30 checks)`。覆盖 Bash、Git、Python、普通文件、目录、
hardlink、symlink、可执行权限、xattr、快照创建与恢复、索引 grep、卸载、deep check、
archive v2 导出、独立校验、导入和导入后 deep check。结束后没有残留挂载。

preview.20 和 preview.21 只保留为发现问题的中间证据，不再作为推荐交付包。

## 2. 最终交付物

- ZIP：`dist/vexdb-lite/vexdb-lite-0.1.0-preview.22-macos-arm64.zip`
- 独立说明：`dist/vexdb-lite/vexdb-lite-0.1.0-preview.22-macos-arm64-使用说明.md`
- SHA-256：`773b0c05e47908dfe2b8f890be4a16f04f10ff3c1937843e9e2aea1c3aa25d1c`
- `source_dirty=false`
- certificate SHA-1：`26C311958B22397631A857D0482CD2F0EA0BF2AA`
- Apple submission ID：`406b904b-6b01-4ae7-8283-4db6c909c5cc`
- Apple 状态：`Accepted`
- Gatekeeper：`source=Notarized Developer ID`

## 3. 真机发现并修复的问题

### 3.1 未发布的新文件进入历史

Git 创建 `.git/index.lock` 后，另一个目录操作可能先产生 commit。旧实现会把尚未首次发布、
`current_version=0` 的 inode 和 dentry 写入不可变历史，但 version 0 没有 manifest，最终 deep
check 报 `VEXFS_HISTORY_VERSION_MISSING`。

修复：历史刷写跳过未发布的 version-0 文件，并保留其 dirty inode、dentry、xattr 和 ACL；
首次发布推进为 version 1 时再原子写入历史。该修复没有增加空版本或额外 commit。

回归用例：`contract.unpublished-create-history`，8 checks；运行时合同还覆盖了
`handle_create -> unrelated mkdir -> first publish` 的顺序。

### 3.2 快照 archive 带入已删除 inode 的当前 ACL

preview.21 的 deep check 已通过，但导出 `remote-baseline` 快照后，archive verify 发现 13 条
当前 ACL 指向快照中已经删除的 Git 临时 inode。历史 ACL 合法，问题是导出器把它们错误地
重建成当前 ACL。

修复：archive 的当前 `xattrs` 和 `acl_entries` 只保留所选快照中仍存活的 inode；历史
`xattr_states` 和 `acl_states` 不删除。`backup.logical-export-import` 新增已删除 inode 的
xattr/ACL 场景，现为 21 checks。

使用 M1 原始失败数据库重新导出同一个 commit 111 后，结果为：49 versions、27,199 content
bytes，archive verify、导入和导入后 deep check 全部通过，`issue_count=0`。

## 4. 本地回归

- `VEXFS RUNTIME SMOKE: PASS`。
- 100 文件 mount-contract 基准：467.757 files/s，仍为每文件 1 commit、1 version。
- 完整 quick eval：58 passed、0 failed、18 skipped、2,499 checks，25.859 秒。
- 18 个 skip 是当前源码构建未注册 FSKit、Linux 专用、跨系统专用或需要真实模型授权的用例；
  本次无源码 M1 Gate 单独验证了已安装 FSKit。
- 文档 smoke：67 checks，PASS。
- `package.unified-install`：17 checks，PASS。

## 5. 无源码 M1 证据

测试机没有 VexDB 源码目录，也没有完整 Xcode。安装后的关键结果：

- CLI：`vexdb-lite 0.1.0-preview.22 (8a8b0c139a), SQLite 3.45.3`。
- 合同：`0.9.0`。
- FSKit：`mount_ready=true`、`extension=enabled`。
- module URL：`~/Applications/VexDB Lite.app/Contents/Extensions/VexFSAppEx.appex`。
- App：Gatekeeper accepted，Notarized Developer ID。
- 无源码 smoke：30 checks，PASS。
- deep check：`content_model=chunked-v1`、`issue_count=0`。
- archive：format v2 verify、导入、导入后 deep check 全部通过。

### 5.1 签名复验环境说明

最终 ZIP 已在 M1 macOS 26.5.2 上从 Downloads 全新解压，不经过安装目录或系统缓存，再次完成：

- ZIP SHA-256 校验；
- App deep codesign；
- CLI 和 dylib codesign；
- Gatekeeper `Notarized Developer ID`；
- manifest 的 source commit、clean source、notarization 状态和 submission ID 核对。

构建机当前是 macOS 26.3.1(a)，但 Xcode 只有 macOS 26.5 SDK。完成构建后，该机器的
Code Signing 子系统会对同一 ZIP 返回 `internal error` 或 `invalid signature`，同时
`xcodebuild` 报系统缓存目录 I/O error；同一 ZIP 在 26.5.2 上持续验证有效。因此本报告把
26.5.2 真机结果作为有效签名证据，不把 26.3.1(a) 的瞬时缓存结果算作通过。

manifest 的 deployment target 仍是 26.0，但目前真实发行验证只证明 26.5.2；26.0–26.4
兼容性需要在系统状态正常、对应版本的独立 Mac 上补测。

## 6. 当前边界

preview.22 证明 macOS 26.5.2 arm64 的安装、FSKit、Bash/Git workspace、版本恢复和离线
archive 已形成完整闭环。它不代表 macOS 26.0–26.4、x86_64 macOS、Linux 安装包或
PostgreSQL/DuckDB 文件插件已经达到同一交付状态；这些仍按路线图分别推进。
