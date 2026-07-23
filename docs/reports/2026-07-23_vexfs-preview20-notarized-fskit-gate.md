# VexFS preview.20 公证与真实 FSKit Gate 报告

- 日期：2026-07-23
- 分支：`feature/agent_files`
- 源码 commit：`4f53577884`
- 版本：`0.1.0-preview.20`
- SQLite 合同：`0.9.0`
- runtime ABI：1
- 平台：macOS 26.3.1 / arm64

## 1. 结论

preview.20 已从干净 commit 构建，使用 `Developer ID Application: Jian ming Wu
(BB5VK42K87)` 签名，并通过 Apple 公证、staple、Gatekeeper、安装和真实 FSKit 挂载 Gate。

本机发行 Gate 为 13 passed、0 failed、0 skipped，共 227 checks。另一台 M1
`192.168.130.217` 当前 SSH 端口 22 超时，因此额外干净 Mac 分发验证尚未完成。

## 2. 交付物

- 压缩包：`dist/vexdb-lite/vexdb-lite-0.1.0-preview.20-macos-arm64.zip`
- 独立使用说明：`dist/vexdb-lite/vexdb-lite-0.1.0-preview.20-macos-arm64-使用说明.md`
- SHA-256：`3818155cc0aae66b17b9655b9e2df6f49a84fd1ff75d07fabdaabc03bb01a2ac`
- `source_dirty=false`
- certificate SHA-1：`26C311958B22397631A857D0482CD2F0EA0BF2AA`

## 3. Apple 签名与公证

- App、FSKit extension、CLI 和 `vexdb_lite.dylib` 的 TeamIdentifier 均为 `BB5VK42K87`。
- Apple 公证状态：`Accepted`。
- submission ID：`e2883581-7e4a-488a-9087-8c64d4ccded3`。
- `stapler staple` 与 `stapler validate` 通过。
- `spctl --assess --type execute` 返回 `source=Notarized Developer ID`。
- ZIP 解压后再次通过 App deep codesign、CLI/dylib codesign 和统一 CLI smoke。

## 4. 包和安装验证

- VexDB unified CLI smoke：PASS。
- 文档 smoke：67 checks，PASS。
- `package.unified-install`：1 passed、0 failed、0 skipped、17 checks。
- 安装后 CLI：`vexdb-lite 0.1.0-preview.20 (4f53577884), SQLite 3.45.3`。
- 安装后合同：`0.9.0`。
- FSKit extension：enabled。
- FSKit module URL 指向
  `~/Applications/VexDB Lite.app/Contents/Extensions/VexFSAppEx.appex`，不是 archive、DerivedData
  或 dist 副本。
- 默认旧测试数据库仍为 schema `0.6.0`，被新 CLI 按合同明确拒绝；测试使用全新 `0.9.0`
  数据库，没有删除或迁移旧数据库。

## 5. 真实 FSKit Gate

13 个 macOS 必跑用例逐项启用 `--fail-on-skip`，结果为 13 passed、0 failed、0 skipped、
227 checks，总用时 59.278 秒：

1. `mount.cross-platform-conformance`
2. `mount.timestamps`
3. `mount.concurrent-append`
4. `mount.open-rename-unlink`
5. `mount.read-only-open-lifecycle`
6. `mount.process-locks`
7. `mount.force-unmount`
8. `mount.real-bash`
9. `mount.posix-metadata`
10. `mount.performance`
11. `mount.git-workspace`
12. `mount.real-toolchain-projects`
13. `mount.scale-tree`

测试覆盖 Bash、Git、Python、Node.js、Go、Rust、并发 append、进程锁、hardlink、symlink、
xattr、Unicode、快照恢复、强制卸载、重挂载和 1,000 文件工作区。结束后无残留 VexFS 挂载。

主要报告：

- 一致性合同：`vexdb_sqlite/build/eval/vexfs/20260723T090649.098074Z-quick-20260718/report.json`
- 性能：`vexdb_sqlite/build/eval/vexfs/20260723T090717.944326Z-quick-20260718/report.json`
- 工具链：`vexdb_sqlite/build/eval/vexfs/20260723T090721.320982Z-quick-20260718/report.json`
- 规模：`vexdb_sqlite/build/eval/vexfs/20260723T090737.859441Z-quick-20260718/report.json`

## 6. 本机性能样本

- 8 MiB 顺序写：46.863 MiB/s。
- 100 次随机写：1,041.044 ops/s。
- 1,000 文件创建：5.706 秒，175.253 files/s。
- 同轮 APFS：12,620.380 files/s；VexFS 小文件创建慢约 72.012 倍。
- 普通文件扫描：0.428 秒；同轮 APFS 为 0.018 秒，慢约 23.422 倍。
- 数据库 trigram 索引搜索：0.007449 秒，候选文件 1 个。
- 快照：0.007131 秒；恢复：0.271043 秒。
- 数据库占用：2,342,912 bytes，约 2,342.912 bytes/file。

这说明 chunked-v1 没有破坏正确性或大块顺序写，但小文件创建仍是当前最明显的性能问题。

## 7. 最终完整性

使用已安装 preview.20 创建全新数据库后执行 deep check：

- `ok=true`
- `content_model=chunked-v1`
- `issue_count=0`
- 无 pending handle、retained handle、staging 或残留 mount。

## 8. 尚未完成

1. 另一台没有源码/Xcode 构建记录的干净 Mac 安装验证；当前测试机
   `192.168.130.217:22` 连接超时。
2. 真实 OpenCode 调用仍需单独授权模型外发和额度，不属于自动发行 Gate。
3. 小文件创建性能仍需继续优化。
