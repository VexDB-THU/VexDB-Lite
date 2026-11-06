# VexFS chunk + manifest 与逻辑归档 v2 验证报告

- 日期：2026-07-23
- 分支：`feature/agent_files`
- SQLite 合同：`0.9.0`
- 内容模型：`chunked-v1`
- 逻辑归档格式：`.vexfs` format v2

## 1. 结论

SQLite 已从“每个 canonical 版本保存一个完整 BLOB”切换为不可变 manifest + 64 KiB 物理
chunk。挂载打开和发布不再额外组装一份完整文件；随机覆盖只创建受影响的块，未变化的块在
同一 inode 的版本之间复用。版本恢复继续使用 alias，不重复创建 manifest 或 chunk。

这是未正式发版的新 schema，不提供旧实验 schema 迁移；版本不匹配时明确拒绝打开。

## 2. 当前数据模型

- `_vexfs_file_versions`：canonical 版本保存 `manifest_id`；恢复版本保存
  `source_file_version_id`，两者不能同时存在。
- `_vexfs_manifests`：保存 inode、文件长度、固定块大小、块数量和整文件 SHA-256。
- `_vexfs_chunks`：不可变物理块，保存 inode、内容长度、BLOB 和逐块 SHA-256。
- `_vexfs_manifest_chunks`：保存 manifest 中连续的块序号和物理 chunk 引用。

复用范围有意限制在同一 inode。当前不做跨 inode、跨文件通用去重，避免仅凭内容 hash 把
不同文件的生命周期、安全边界和回收规则绑在一起。

## 3. 读写和恢复

- 读取 canonical 版本时依次读取 manifest 的 chunk，校验块顺序、长度、逐块 SHA-256 和
  整文件 SHA-256；alias 会先解析到同 inode 的 canonical 版本。
- mount 打开文件时，以 64 KiB 为单位把已发布内容写入 staging。
- mount publish 两遍扫描 staging：第一遍计算文件信息，第二遍建立 manifest 和物理块；
  工作缓冲固定为 64 KiB。
- 原子 append 在发布锁内刷新到当前 inode 版本，再追加本次后缀，避免两个旧 handle 静默覆盖。
- staging 当前仍是每个 handle 一个可增长 SQLite BLOB；它是未发布临时状态，不是最终内容模型。

## 4. check、GC、配额和归档

- quick check 验证 manifest 引用、块数量、连续顺序、长度和可达性。
- deep check 再验证每个物理块和整文件 SHA-256。
- GC 先删除可回收版本和 manifest 映射，只删除没有任何 manifest 引用的物理块；共享块不会
  因删除一个版本而被误删。
- retention 的 stored/live/reclaimable bytes 按物理 chunk 统计，不把同一块的多个引用重复计费。
- `.vexfs` format v2 显式保存 manifest/chunk 逻辑记录；verify 逐块校验，import 重建 ID 和
  块引用，深度 check 通过后才把目标 workspace 原子切换为 active。

## 5. 有界性能样本

测试文件为 8 MiB，连续执行 100 次 4 KiB 随机覆盖，再发布为新版本：

| 指标 | 结果 |
|---|---:|
| 随机覆盖次数 | 100 |
| 受影响的 64 KiB 块 | 71 |
| 从上一版本复用的块 | 57 |
| 两个版本最终物理块数 | 72 |
| 100 次 staging 覆盖耗时 | 0.074700 s |
| publish 耗时 | 0.370492 s |
| staging 覆盖吞吐 | 1,338.684 ops/s |

这组数据证明“未变化块确实被复用”，不是完整设备性能基准。

最终全量 quick eval：75 个场景中 57 passed、0 failed、18 skipped，共 2,863 checks，耗时
24.824 秒，结果为 `PASS_WITH_SKIPS`。18 个 skip 都有明确平台条件：当前环境没有已安装并启用
的 FSKit extension、当前不是 Linux，或没有显式开启真实 OpenCode 模型调用。

- JSON：`vexdb_sqlite/build/eval/vexfs/20260723T085638.564798Z-quick-20260718/report.json`
- Markdown：`vexdb_sqlite/build/eval/vexfs/20260723T085638.564798Z-quick-20260718/report.md`
- SQLite spec：1 passed、0 failed
- runtime smoke：PASS
- VexFS CLI smoke：PASS
- VexDB unified CLI smoke：PASS

## 6. 仍然存在的边界

1. SQL 直接函数 `vexfs_append` 仍会在内存中组装当前完整内容；mount handle append 已走流式发布。
2. staging 仍是每个 handle 一个可增长 BLOB，极大未发布文件会让 SQLite staging BLOB 很大。
3. 当前没有跨文件去重、自动 repair、自动 GC 或在线 `VACUUM`。
4. PostgreSQL 和 DuckDB 还没有 format v2 consumer，也没有 VexFS adapter。
5. 当前安装的 FSKit 预览包不是基于合同 `0.9.0` 重新签名、公证和安装的发行证据。

## 7. 下一步

从干净 commit 重新构建 macOS 包，完成 Developer ID 签名、公证、staple 和启用 FSKit 的真实
mount Gate；随后在另一台干净 Mac 按随包文档验证安装、Bash、Git、卸载和恢复。完成 Phase 1
发行证明后，再进入 PostgreSQL 远程共享工作区，不提前并行扩展三个数据库。
