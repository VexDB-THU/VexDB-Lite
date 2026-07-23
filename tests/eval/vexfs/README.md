# VexFS Eval

这套 eval 使用真实构建产物和磁盘 SQLite 数据库，不使用 mock。每个用例独立记录
结果、耗时、断言数和性能指标，失败后仍继续执行其他用例。

## 运行

```bash
# 默认 full：构建后覆盖功能、恢复、并发、备份、性能和 FSKit 编译
bash build_sqlite.sh eval

# 日常快速回归
bash build_sqlite.sh eval quick

# 指定已有构建目录（评测器会从这里查找 vexdb、扩展和测试产物）
python3 tests/eval/vexfs/run.py --mode quick --build-dir vexdb_sqlite/build

# 最大规模（128 MiB 文件、1 万随机操作、1 万小文件）
bash build_sqlite.sh eval stress

# 只跑某一类或某个用例
python3 tests/eval/vexfs/run.py --mode full --filter recovery
python3 tests/eval/vexfs/run.py --mode full --filter performance.staged-overwrite
python3 tests/eval/vexfs/run.py --mode full --filter performance.mount-contract-small-files

# 把性能预算也作为硬 Gate；默认只记录指标，避免不同机器误报
python3 tests/eval/vexfs/run.py --mode full --enforce-performance

# 发版前重新打包和签名后，再显式运行交付包测试
VEXDB_LITE_PACKAGE_STAGE=/absolute/path/to/stage \
  python3 tests/eval/vexfs/run.py --mode full --include-package --fail-on-skip

# 真实挂载和跨平台 Gate 必须显式指定本次要验证的挂载 CLI；任何 SKIP 都失败
python3 tests/eval/vexfs/run.py --mode full --filter mount \
  --mount-cli /absolute/path/to/vexfs --fail-on-skip

# Linux AArch64/x86_64：在真实 /dev/fuse 上分别以 root 和 uid 1000
# 运行与 macOS 完全相同的 mount 合同（需要 Docker Desktop 或 Docker Engine）
bash tests/eval/vexfs/run_linux_mount.sh

# 用同一个 SQLite 文件完成 macOS FSKit → Linux FUSE → macOS FSKit 往返
bash tests/eval/vexfs/run_cross_platform_portability.sh

# 显式调用真实 OpenCode 模型，在挂载项目中自主改代码并跑测试
# 默认不运行，避免日常回归自动消耗模型额度
VEXFS_EVAL_OPENCODE=1 \
  python3 tests/eval/vexfs/run.py --mode quick --filter mount.real-opencode-project

# 可选择模型，默认 openai/gpt-5.4-mini
VEXFS_EVAL_OPENCODE=1 VEXFS_EVAL_OPENCODE_MODEL=openai/gpt-5.4-mini \
  python3 tests/eval/vexfs/run.py --mode quick --filter mount.real-opencode-project
```

报告生成在：

```text
vexdb_sqlite/build/eval/vexfs/<run-id>/report.json
vexdb_sqlite/build/eval/vexfs/<run-id>/report.md
vexdb_sqlite/build/eval/vexfs/latest.json
vexdb_sqlite/build/eval/vexfs-linux-mount/{root,uid-1000}/<run-id>/report.json
```

随机状态机使用固定 seed，可通过 `--seed` 复现或扩充样本。

评测结果有三种：`PASS` 表示所有选中用例都执行并通过；`PASS_WITH_SKIPS`
表示已执行用例通过，但有环境不满足的用例未执行；`FAIL` 表示至少一个用例失败，
或在 `--fail-on-skip` 下出现了 `SKIP`。没有匹配到任何用例会直接以非零状态退出。

## 覆盖范围

- 现有 Gate：SQL 静态注册、C ABI、CLI smoke、全部 SQLite YAML spec。
- 文件合同：二进制/Unicode、元数据、路径校验、rename replace、递归删除、公开版本历史和指定版本读取。
- 版本恢复：SQL/C ABI/CLI restore、expected-version 冲突、事务回滚、dry-run 不写库、文本 diff 和末尾换行差异。
- 事务：commit、rollback、savepoint、读者快照、目录 verifier。
- 句柄：flags、稀疏写、truncate、幂等指纹、同步、乐观写冲突；幂等结果至少
  保留最近 65,536 条，并以 4,096 条为一批清理更早的完成记录。
- 时间戳和生命周期：birth/access/modify/change、`utimens`、并发原子 append、
  `flock`/`fcntl`、打开文件 rename/unlink、普通/强制卸载。
- 恢复：进程中途退出、WAL 重开、integrity check、retained reclaim；
  Linux helper 被 `SIGKILL` 后重挂载会自动发布保留的暂存写入。
- 长期校验：版本 SHA-256、单文件恢复和 workspace 恢复别名、commit/snapshot/history/dentry
  引用、staging 缺失、同长度内容损坏注入、损坏读取拒绝、CLI 退出码 8；性能用例分别记录
  quick 元数据检查和 64 KiB 流式 deep hash 的吞吐与 RSS 增长。
- 数据库版本：当前尚未发版，只支持当前 schema，不运行旧 schema 迁移测试。
- 并发：多个真实进程写入、数据库锁和 busy timeout。
- 备份：运行中的 SQLite online backup、恢复、未发布 staging 可见性。
- 随机模型：真实数据库状态与 Python 参考文件树持续比对。
- 性能：大量小文件、大文件顺序读写、分块 staging、随机 4 KiB 覆盖、备份吞吐；
  `performance.mount-contract-small-files` 额外模拟原子创建、macOS provenance xattr、
  内容发布和最终关闭，检查每文件 commit/version/request 写放大、FULL 屏障和峰值 RSS。
- 文本搜索：`vexdb fs grep` 的 UTF-8 字面匹配、二进制跳过、大小写、文件名/行号输出、
  短查询回退；可选 FTS5 trigram 索引的启用、维护、快照恢复和候选文件性能。
- 规模与稳定性：数据库直连和真实 mount 分别覆盖 1 千/1 万/10 万文件；混合负载
  按 quick/full/stress 持续 5/60/900 秒，期间反复重开、快照和 checkpoint。
- 真实项目：Python、Node.js、Go、Rust 与 Git 在挂载盘中构建和测试，并在重挂载后
  重跑；`mount.real-opencode-project` 显式启用后由真实 OpenCode 自主改代码和跑测试。
- 空间：版本历史增长、幂等请求保留水位、规模树的提交/版本/ACL/xattr 写放大、
  checkpoint/VACUUM 前后的 DB/WAL 体积。
- 平台：Linux/libfuse3 helper 独立 C ABI smoke，并在有 `/dev/fuse` 时执行真实
  mount、普通用户 uid/gid、Bash、可执行脚本、hardlink/symlink、Git 提交、卸载和
  重挂载；Windows/WinFsp 边界独立编译和状态 smoke；FSKit App/extension 无签名
  编译，扩展已启用时执行真实 mount 和 bash 命令。
- 共用挂载合同：`mount.cross-platform-conformance` 不按平台分叉测试步骤；在
  macOS FSKit 与 Linux libfuse3 上统一验证独占创建、范围读写、fsync、append、
  truncate、rename replace、mode/执行、hardlink、symlink、xattr、Unicode、标准
  errno、snapshot restore、卸载与重挂载。新平台 adapter 必须复用这套用例。
- 跨系统往返：`portability.cross-os-roundtrip` 由专用脚本分三段执行，验证同一个
  SQLite 工作区在 macOS 和 Linux 之间保留内容、Unicode、mode、可执行文件、链接、
  xattr、ACL、数字 owner、历史和双向快照恢复。

## 模式规模

| 项目 | quick | full | stress |
|---|---:|---:|---:|
| 随机模型操作 | 300 | 2,000 | 10,000 |
| 小文件 | 250 | 3,000 | 10,000 |
| 挂载合同小文件 | 250 | 1,000 | 1,000 |
| 顺序文件 | 8 MiB | 100 MiB | 128 MiB |
| staging 文件 | 8 MiB | 100 MiB | 128 MiB |
| 随机 4 KiB 写 | 100 | 1,000 | 5,000 |
| 并发进程 | 4 | 8 | 8 |
| 规模目录文件数 | 1,000 | 10,000 | 100,000 |
| 混合重开稳定性 | 5 秒 | 60 秒 | 900 秒 |

`performance.scale-tree` 还会记录无索引数据库 grep、trigram 索引构建和索引搜索。
quick/full/stress 的峰值 RSS 保护线分别为 1 GiB、1.5 GiB、2 GiB；SQLite 临时表使用
磁盘，页缓存限制为 64 MiB。越过保护线会立即失败，不继续扩大测试规模。

`mount.real-bash` 是环境 Gate。macOS、FSKit 和已启用的 VexFS 系统扩展都满足时，
它会真实挂载并运行 `mkdir`、`cat`、`grep`、`cp`、`mv`、`find`、`rm` 等命令；
条件不满足时报告会明确写 `SKIP` 和原因，不会把未执行伪装成通过。

`mount.real-linux-bash-git` 是 Linux 环境 Gate。构建中存在 `vexfs-fuse`，且
`/dev/fuse`、`fusermount3` 和 Git 可用时，它会以真实挂载目录验证完整 Git
workspace，并在卸载和重挂载后再次运行 `git status`。

`mount.cross-platform-conformance` 是平台一致性 Gate。Linux 可通过
`run_linux_mount.sh` 自动准备 libfuse3 环境并验证 root/普通用户；macOS 必须先在
“系统设置 → 通用 → 登录项与扩展 → 文件系统扩展”中启用 VexDB Lite，再运行该
用例。缺少真实挂载条件时结果是 `SKIP`，不会用 C ABI smoke 冒充平台通过。
