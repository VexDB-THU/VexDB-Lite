# VexFS Eval

这套 eval 使用真实构建产物和磁盘 SQLite 数据库，不使用 mock。每个用例独立记录
结果、耗时、断言数和性能指标，失败后仍继续执行其他用例。

## 运行

```bash
# 默认 full：构建后覆盖功能、恢复、并发、备份、性能和平台 adapter 编译
bash build_sqlite.sh eval

# 日常快速回归
bash build_sqlite.sh eval quick

# 指定已有构建目录（评测器会从这里查找 vexdb、扩展和测试产物）
tests/eval/vexfs/python.sh tests/eval/vexfs/run.py --mode quick --build-dir vexdb_sqlite/build

# 最大规模（128 MiB 文件、1 万随机操作、1 万小文件）
bash build_sqlite.sh eval stress

# 只跑某一类或某个用例
tests/eval/vexfs/python.sh tests/eval/vexfs/run.py --mode full --filter recovery
tests/eval/vexfs/python.sh tests/eval/vexfs/run.py --mode full --filter performance.staged-overwrite
tests/eval/vexfs/python.sh tests/eval/vexfs/run.py --mode full --filter performance.mount-contract-small-files

# 真实挂载规模搜索默认最多等待 quick=60、full=300、stress=600 秒；可显式覆盖
VEXFS_EVAL_NATIVE_SEARCH_TIMEOUT_SECONDS=900 \
  tests/eval/vexfs/python.sh tests/eval/vexfs/run.py --mode stress --filter mount.scale-tree \
  --mount-cli /absolute/path/to/vexfs --fail-on-skip

# 固定复现“1000 小文件后写 8 MiB、完整读、重挂载冷读”；失败时保留现场
tests/eval/vexfs/python.sh tests/eval/vexfs/run.py --mode quick \
  --filter mount.scale-read-after-small-files \
  --mount-cli /absolute/path/to/vexdb --fail-on-skip

# 把性能预算也作为硬 Gate；默认只记录指标，避免不同机器误报
tests/eval/vexfs/python.sh tests/eval/vexfs/run.py --mode full --enforce-performance

# 发版前重新打包和签名后，再显式运行交付包测试
VEXDB_LITE_PACKAGE_STAGE=/absolute/path/to/stage \
  tests/eval/vexfs/python.sh tests/eval/vexfs/run.py --mode full --include-package --fail-on-skip

# 真实挂载和跨平台 Gate 必须显式指定本次要验证的挂载 CLI；任何 SKIP 都失败
tests/eval/vexfs/python.sh tests/eval/vexfs/run.py --mode full --filter mount \
  --mount-cli /absolute/path/to/vexfs --fail-on-skip

# macOS 默认 NFS：真实 Bash、hardlink/symlink、fsync、xattr、Git、
# 1000 个小文件、卸载/重挂载、完整快照恢复；文件数硬限制为 1..10000
VEXDB_NFS_FILE_COUNT=1000 \
  bash tests/eval/vexfs/run_macos_nfs_mount.sh \
  /absolute/path/to/vexdb

# Linux AArch64/x86_64：在真实 /dev/fuse 上分别以 root 和 uid 1000
# 运行与 macOS 完全相同的 mount 合同（需要 Docker Desktop 或 Docker Engine）
bash tests/eval/vexfs/run_linux_mount.sh

# 用同一个 SQLite 文件完成 macOS adapter → Linux FUSE → macOS adapter 往返
bash tests/eval/vexfs/run_cross_platform_portability.sh

# 显式调用真实 OpenCode 模型，在挂载项目中自主改代码并跑测试
# 默认不运行，避免日常回归自动消耗模型额度
VEXFS_EVAL_OPENCODE=1 \
  tests/eval/vexfs/python.sh tests/eval/vexfs/run.py --mode quick --filter mount.real-opencode-project

# 可选择模型，默认 openai/gpt-5.4-mini
VEXFS_EVAL_OPENCODE=1 VEXFS_EVAL_OPENCODE_MODEL=openai/gpt-5.4-mini \
  tests/eval/vexfs/python.sh tests/eval/vexfs/run.py --mode quick --filter mount.real-opencode-project
```

## PostgreSQL adapter

PG eval 使用真实 PostgreSQL、真实扩展表、libpq、FSKit 和 FUSE，不使用 mock。先把当前
`vexdb_lite--1.0.sql` 和 `vexdb_lite.control` 安装进测试容器，再按需要执行：

```bash
# 渲染并运行全部 VexFS PG spec
tests/eval/vexfs/python.sh tests/spec/_lib/render.py --engine pg --out build/spec
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
  bash tests/spec/_lib/docker/run_pg.sh test 'pg__vexfs_*'

# SQL adapter、并发写和 expected-head 恢复冲突
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
  bash tests/eval/vexfs/run_pg_adapter_alpha.sh

# 1000 个文件的有界性能基线；参数硬限制为 1..10000
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev VEXDB_PG_PERF_FILES=1000 \
  bash tests/eval/vexfs/run_pg_adapter_performance.sh

# PG 批量历史写入：先用旧单文件接口建立 1000 文件控制组，再验证
# 1k/10k/100k 批量创建。每批最多 1000 个文件、单连接串行执行，容器
# 内存硬限制默认 1 GiB；同时检查一个批次一个 commit、逐路径 change、
# 逐文件 version、共享空 manifest、千/万/十万文件只存一份继承 ACL、
# deep/quick check 和至少 2x 加速。
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
  bash tests/eval/vexfs/run_pg_create_batch_performance.sh

# 16 个连接同时把相同 ACL 写到不同 inode，验证只生成一个不可变集合；
# 再次并发写入必须幂等，快照不得复制 ACL 明细。容器 memory.max 必须
# 不超过 1 GiB，并检查 oom_kill 没有增长。
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
  bash tests/eval/vexfs/run_pg_acl_cow_concurrency.sh

# SQLite 与 PG 各建立 1 万/10 万条无正文 workspace commit，验证 workspace log
# 头页/深分页、快照关联、2 秒查询预算和 PG 1 GiB/OOM 保护线。
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev VEXFS_WORKSPACE_LOG_COMMITS=10000 \
  bash tests/eval/vexfs/run_workspace_log_performance.sh
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
  bash tests/eval/vexfs/run_workspace_log_performance.sh

# PG manifest 根哈希专项：16 MiB 文件每轮只改 4 KiB，发布不得拼完整文件；
# 默认重复 5 次，容器 memory.max 必须不超过 1 GiB，并检查 oom_kill 未增长。
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev VEXDB_PG_DATABASE=test \
  bash tests/eval/vexfs/run_pg_manifest_publish_performance.sh

# libpq HostStore、CLI、handle、ACL、审计、多 gateway 和缓存失效
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
VEXDB_PG_DSN=postgresql://postgres@127.0.0.1:5434/test \
  bash tests/eval/vexfs/run_pg_runtime.sh

# 数据库停止和重启后的句柄、锁、缓存和内容恢复
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
VEXDB_PG_DSN=postgresql://postgres@127.0.0.1:5434/test \
  bash tests/eval/vexfs/run_pg_restart_recovery.sh

# 断电等价 Gate：strict 写入并 fsync、校验 mount source/fsid 和 gateway 进程
# 启动身份、SIGKILL gateway、PG immediate restart、重挂载读取与 deep check；
# 只写一个小文件，容器 memory.max 必须不超过 1 GiB。
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
VEXFS_EVAL_MOUNT_CLI=/absolute/path/to/current/vexdb \
  bash tests/eval/vexfs/run_pg_strict_crash_recovery.sh

# 连续 crash soak；首轮失败立即停止，默认 20 轮、最多 100 轮。
# 每次运行自动使用唯一数据库名，数据库已存在时拒绝执行，不会预先 drop。
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
VEXFS_EVAL_MOUNT_CLI=/absolute/path/to/current/vexdb \
VEXFS_PG_STRICT_CRASH_ROUNDS=20 \
  bash tests/eval/vexfs/run_pg_strict_crash_soak.sh

# 先保留 gateway 的主 libpq/publisher TCP 连接但停止双向转发，再真实关闭两条连接
# 并让新连接进入 blackhole。验证两类故障都在约 5 秒内有界失败、10 秒内恢复，
# 同时覆盖断线期间 visibility 重同步、后台 publisher、唯一数据库、deep check 和 OOM。
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
VEXFS_EVAL_MOUNT_CLI=/absolute/path/to/current/vexdb \
  bash tests/eval/vexfs/run_pg_network_cut_recovery.sh

# 完整数据库 pg_dump/pg_restore
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
  bash tests/eval/vexfs/run_pg_backup_restore.sh

# pg_basebackup 物理克隆
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
  bash tests/eval/vexfs/run_pg_physical_backup_restore.sh

# 16 MiB 有界备份性能 Gate：逻辑备份、物理备份、format v2 和峰值 RSS
# 容器 memory.max 必须不超过 1 GiB，脚本也会检查 oom_kill 没有增加
# 未设置 VEXDB_PG_HOST_PORT 时会读取所选容器发布的 5432/tcp 端口。
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
  bash tests/eval/vexfs/run_pg_backup_performance.sh

# format v2 SQLite → PostgreSQL → SQLite；未设置 VEXDB_PG_DSN 时，
# 脚本会从所选容器的 5432/tcp 发布端口推导本机 DSN，避免连到另一套 PG。
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
  bash tests/eval/vexfs/run_pg_archive_roundtrip.sh

# PostgreSQL 16/17/18/19，按顺序运行且每个容器限制 1 GiB
# PG19 仍是预发布版，没有官方 postgres:19 镜像，因此显式复用已安装当前扩展、
# memory.max 不超过 1 GiB 的 PG19 测试容器。
VEXDB_PG19_MATRIX_CONTAINER=vexdb_pg19-vexfs-dev \
  bash tests/eval/vexfs/run_pg_version_matrix.sh

# macOS：默认通过真实 NFSv3 gateway 运行公共 mount 合同
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
VEXDB_PG_DSN=postgresql://postgres@127.0.0.1:5434/test \
  bash tests/eval/vexfs/run_pg_macos_mount.sh

# PG overlay/publisher 专项：真实挂载性能、1000 小文件后冷读、跨会话缓存失效
VEXFS_MACOS_PG_CASES='mount.performance mount.scale-read-after-small-files mount.external-cache-invalidation' \
VEXDB_PG_DATABASE=test \
VEXDB_PG_DSN=postgresql://postgres@127.0.0.1:5434/test \
  bash tests/eval/vexfs/run_pg_macos_mount.sh

# 强持久化：fsync、强制卸载、重挂载读取；strict 性能数字单独记录
VEXFS_NFS_STRICT_DURABILITY=1 \
VEXFS_MACOS_PG_CASES='mount.force-unmount mount.performance' \
VEXDB_PG_DSN=postgresql://postgres@127.0.0.1:5434/test \
  bash tests/eval/vexfs/run_pg_macos_mount.sh

# Linux：真实 libfuse3，root 和 uid 1000
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
  bash tests/eval/vexfs/run_pg_linux_mount.sh

# 同一 PG 工作区在 macOS FSKit 和 Linux FUSE 间往返
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev \
VEXDB_PG_DSN=postgresql://postgres@127.0.0.1:5434/test \
  bash tests/eval/vexfs/run_pg_cross_gateway.sh

# 两台 Mac：真实 FSKit、extension 崩溃、网络中断和数据库停机恢复。
# 默认通过 SSH reverse tunnel 连接本机 PG，因此不依赖远端 Mac 的本地网络授权。
VEXFS_REMOTE_HOST=<second-mac> VEXFS_REMOTE_USER=<user> \
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev VEXFS_LOCAL_PG_PORT=5434 \
  tests/eval/vexfs/run_pg_remote_macos_faults.sh

# 最初产品场景的 PostgreSQL 版本：Linux PG + 两台 Mac FSKit + 两次真实 OpenCode，
# 串行修改同一个 workspace，列出两个快照并一键恢复。该用例会调用外部模型，必须显式运行。
VEXFS_REMOTE_HOST=<second-mac> VEXFS_REMOTE_USER=<user> \
VEXDB_PG_CONTAINER=vexdb_pg19-vexfs-dev VEXFS_LOCAL_PG_PORT=5434 \
VEXFS_LOCAL_OPENCODE_MODEL=openai/gpt-5.4-mini \
VEXFS_REMOTE_OPENCODE_MODEL=opencode/north-mini-code-free VEXFS_KEEP_WORKSPACE=1 \
  tests/eval/vexfs/run_pg_two_mac_opencode.sh
```

备份恢复 eval 会检查 `vexdb_lite.control` 的默认版本为 `1.0`。这是发布门禁：
`pg_dump` 在目标库安装扩展时使用目标机器的默认版本，所以控制文件与对应版本 SQL 必须
一起安装。

macOS 远程 PG 推荐使用 libpq passfile，文件必须是当前用户拥有的普通文件且权限为 `0600`：

```bash
chmod 0600 ~/.pgpass
vexdb fs --backend pg \
  --dsn 'host=db.example port=5432 dbname=app user=agent passfile=/Users/me/.pgpass' \
  --workspace agent-workspace mount ~/Work/agent-workspace
```

DSN 中的明文密码、符号链接 passfile 和过宽权限会在调用 FSKit 前被拒绝。挂载时的受保护
passfile 副本只在资源仍被挂载时保留，最后卸载或 `vexdb fs doctor` 会清理。可用
`run_pg_macos_credentials.sh` 做自动回归。

两台 Mac 直连局域网 PostgreSQL 前，要分别启用：

1. “系统设置 → 通用 → 登录项与扩展 → 文件系统扩展”中的 VexDB Lite；
2. “系统设置 → 隐私与安全性 → 本地网络”中的 VexDB Lite。

第二项是 macOS 隐私授权，安装脚本不能替用户打开。首次远程 mount 失败时，CLI 会自动打开
VexDB Lite，用目标 host/port 发起不含凭据的真实连接检查，触发授权并在允许后重试。若用户
以前拒绝过，CLI 会明确提示上面的系统设置路径。完成后使用
`run_pg_remote_macos.sh` 执行本机创建、远端直读、远端挂载修改、本机快照恢复、远端最终验证
五个阶段。无人值守 CI 可用 SSH 隧道把远端 PG 暴露为回环地址，验证同一 FSKit/runtime 合同，
但隧道结果不能替代局域网权限验收。

回环隧道下本机和远端端口通常不同。`run_pg_remote_macos.sh` 分别接受
`VEXFS_LOCAL_PG_HOST`/`VEXFS_LOCAL_PG_PORT` 与
`VEXFS_REMOTE_PG_HOST`/`VEXFS_REMOTE_PG_PORT`；本机可以直连数据库发布端口，远端只连接
SSH reverse tunnel，两个 passfile 也按各自 endpoint 生成。

`run_pg_remote_macos_faults.sh` 会让两台 Mac 同时挂载一个 PG workspace，并依次杀死远端
FSKit extension、切断 SSH 隧道、停止 PostgreSQL，再验证没有半文件或静默覆盖。网络恢复后
先用可重试的只读探针建立新连接，再继续写入；中断时失败的写请求不会被隐式重放，因为客户端
无法可靠判断服务端是否已经提交。helper 异常退出后，底层 mountpoint 保持 `0500`，防止 Bash
把内容误写进普通本机目录；显式卸载恢复为 `0700`。

`run_pg_two_mac_opencode.sh` 只发送脚本生成的临时 Python 项目给 OpenCode，不发送仓库源码、
DSN 或凭据。第一台和第二台 Mac 串行工作，暂不测试并发冲突。PG 当前的 `vexfs grep` 是数据库
扫描，`vexfs index status` 返回 `available=false`；因此该用例验证 grep 正确性，但不会把
尚未实现的 PostgreSQL substring 索引宣传成性能提升。
Apple Silicon 默认使用远端 `/opt/homebrew/bin/opencode`；不要误用 Rosetta 下的
`/usr/local/bin/opencode`，后者依赖的 x86_64 Bun 会警告缺少 AVX 并可能崩溃。Intel Mac 可用
`VEXFS_REMOTE_OPENCODE` 显式覆盖。

真实 OpenCode 用例会自动检测当前版本支持的无人值守参数：优先
`--dangerously-skip-permissions`，旧版本回退 `--auto`。两台机器可以分别通过
`VEXFS_LOCAL_OPENCODE_MODEL` 和 `VEXFS_REMOTE_OPENCODE_MODEL` 选择各自已配置的模型；远端默认
使用 `opencode/north-mini-code-free`。`VEXFS_EVAL_OPENCODE_MODEL` 仍可设置第一台的兼容默认值。

报告生成在：

```text
vexdb_sqlite/build/eval/vexfs/<run-id>/report.json
vexdb_sqlite/build/eval/vexfs/<run-id>/report.md
vexdb_sqlite/build/eval/vexfs/latest.json
vexdb_sqlite/build/eval/vexfs-linux-mount/{root,uid-1000}/<run-id>/report.json
```

随机状态机使用固定 seed，可通过 `--seed` 复现或扩充样本。

宿主 eval 要求 Python 3.8+。`tests/eval/vexfs/python.sh` 会优先使用
`VEXDB_LITE_PYTHON`，否则从 PATH、Homebrew、Anaconda 和系统 Python 中选择第一个满足版本
要求的解释器，避免 PATH 中旧 Python 3.6 造成语法错误。容器脚本继续使用镜像内 Python。

评测结果有三种：`PASS` 表示所有选中用例都执行并通过；`PASS_WITH_SKIPS`
表示已执行用例通过，但有环境不满足的用例未执行；`FAIL` 表示至少一个用例失败，
或在 `--fail-on-skip` 下出现了 `SKIP`。没有匹配到任何用例会直接以非零状态退出。

## 覆盖范围

- 现有 Gate：SQL 静态注册、C ABI、CLI smoke、全部 SQLite YAML spec。
- 文件合同：二进制/Unicode、元数据、路径校验、rename replace、递归删除、公开版本历史和指定版本读取。
- 版本恢复：SQL/C ABI/CLI restore、expected-version 冲突、事务回滚、dry-run 不写库、文本 diff 和末尾换行差异。
- 事务：commit、rollback、savepoint、读者快照、目录 verifier。
- 句柄：flags、稀疏写、truncate、幂等指纹、同步、乐观写冲突、发布关闭原子回滚；幂等结果至少
  保留最近 65,536 条，并以 4,096 条为一批清理更早的完成记录。
- 时间戳和生命周期：birth/access/modify/change、`utimens`、并发原子 append、
  `flock`/`fcntl`、打开文件 rename/unlink、普通/强制卸载。
- 恢复：进程中途退出、WAL 重开、integrity check、retained reclaim；
  Linux helper 被 `SIGKILL` 后，底层目录保持 `0500`、普通用户不能产生本地假文件；30 秒
  session/handle 租约到期后重挂载会自动发布保留的暂存写入，显式卸载恢复 `0700`。
- 长期校验：逐块 SHA-256 与有序 manifest 根、单文件恢复和 workspace 恢复别名、commit/snapshot/history/dentry
  引用、staging 缺失、同长度内容损坏注入、损坏读取拒绝、CLI 退出码 8；性能用例分别记录
  quick 元数据检查和 64 KiB 流式 deep hash 的吞吐与 RSS 增长。
- 数据库版本：当前尚未发版，只支持当前 schema，不运行旧 schema 迁移测试。
- 并发：多个真实进程写入、数据库锁和 busy timeout。
- 备份：运行中的 SQLite online backup、恢复、未发布 staging 可见性。
- PostgreSQL 备份性能：有界验证 `pg_dump/pg_restore`、`pg_basebackup` + `pg_verifybackup` +
  克隆启动、format v2 PG → SQLite；同时检查容器 1 GiB 内存上限、`oom_kill` 和 CLI 峰值 RSS。
- PostgreSQL staging：基础 manifest + 64 KiB 脏块、范围读写、truncate 缩短后零填充、
  未变化 chunk 复用、精确 generation claim、逐文件后台事务、断线重试幂等、部分成功清理、
  服务端已提交但客户端不读取结果后的同 generation 收敛、后台发布不持有前台 runtime 锁；
  调度器覆盖全局空闲、4 MiB 脏写、1024 文件、30 秒最长等待和活跃 handle 隔离。
- PostgreSQL 批量历史：`vexfs_create_batch` 用一个 commit/audit/notify 表示一个批次，
  `commit_changes` 继续保留逐路径变化，空文件共享一个不可变 manifest；1 千/1 万/10 万
  文件 eval 与旧逐文件接口对照，并限制 `work_mem`、临时文件和容器总内存。
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
| 挂载合同小文件 | 250 | 10,000 | 100,000 |
| 顺序文件 | 8 MiB | 100 MiB | 128 MiB |
| staging 文件 | 8 MiB | 100 MiB | 128 MiB |
| 随机 4 KiB 写 | 100 | 1,000 | 5,000 |
| 并发进程 | 4 | 8 | 8 |
| 规模目录文件数 | 1,000 | 10,000 | 100,000 |
| 混合重开稳定性 | 5 秒 | 60 秒 | 900 秒 |

`performance.scale-tree` 还会记录无索引数据库 grep、trigram 索引构建和索引搜索。
quick/full/stress 的峰值 RSS 保护线分别为 1 GiB、1.5 GiB、2 GiB；SQLite 临时表使用
磁盘，页缓存限制为 64 MiB。越过保护线会立即失败，不继续扩大测试规模。

真实挂载规模测试即使原生 `rg`/`grep` 超时，也会继续完成数据库搜索、索引、快照恢复和
重挂载，再把超时作为带完整指标的失败报告。失败时临时数据库、挂载描述和分阶段
`eval-checkpoints.jsonl` 会移到该 case 的 `artifacts/preserved-workspace/`，避免数小时测试
只留下一个超时字符串；成功时这些大文件自动清理。搜索等待时间由
`VEXFS_EVAL_NATIVE_SEARCH_TIMEOUT_SECONDS` 控制。

`mount.real-bash` 是 macOS 默认 NFS 环境 Gate。它只使用系统自带 NFS client 和本机
loopback gateway，不要求用户启用 FSKit；会真实运行 `mkdir`、`cat`、`grep`、`cp`、
`mv`、`find`、`rm` 等命令。`run_macos_nfs_mount.sh` 是更完整的发版前 Gate，还覆盖
hardlink、fsync、Git、AppleDouble 隔离、重挂载和快照恢复。条件不满足时必须失败或明确
报告 `SKIP`，不能用 C ABI smoke 冒充真实挂载通过。

`mount.scale-read-after-small-files` 专门覆盖曾经出现过的规模退化：quick 模式先创建
1000 个小文件，再写入并完整读取 8 MiB 文件，随后卸载、重挂载并校验冷读 SHA-256。
full/stress 模式分别提高到 3000/5000 个小文件和 16/32 MiB；所有读取都采用 1 MiB
流式缓冲，不会把整个工作区一次性装进内存。

`mount.external-cache-invalidation` 覆盖缓存优化的正确性：先通过真实挂载填充文件和目录
缓存，再由第二个独立数据库会话覆盖一个文件并创建一个文件；挂载端必须在 5 秒内看到
新内容和新目录项。它防止为了小文件性能把缓存改成永不失效，也会在卸载后通过 CLI 再次
核对数据库权威内容。

`mount.real-linux-bash-git` 是 Linux 环境 Gate。构建中存在 `vexfs-fuse`，且
`/dev/fuse`、`fusermount3` 和 Git 可用时，它会以真实挂载目录验证完整 Git
workspace，并在卸载和重挂载后再次运行 `git status`。

`mount.cross-platform-conformance` 是平台一致性 Gate。Linux 可通过
`run_linux_mount.sh` 自动准备 libfuse3 环境并验证 root/普通用户；macOS 默认使用 NFS，
无需系统扩展授权。只有显式传 `--mount-driver fskit` 时才要求在系统设置中启用 VexDB Lite。
缺少真实挂载条件时结果是 `SKIP`，不会用 C ABI smoke 冒充平台通过。
