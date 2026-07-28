# VexFS 完整产品需求文档

- 文档类型：产品需求基线
- 产品名：`VexFS`（暂定）
- 所属项目：VexDB-Lite
- 日期：2026-07-16
- 分支：`feature/agent_files`
- 文档版本：2.0
- 状态：SQLite `0.9.0`、macOS 默认 NFS、可选 FSKit、Linux libfuse3 和 arm64 真机交付已实现；
PostgreSQL `0.4.0-alpha.1` 的数据库合同、format v2、ACL、审计、libpq HostStore、
  macOS/Linux 真实 mount、双 Mac 串行 Agent 工作区和备份恢复已实现；SQLite 与 PostgreSQL
  已提供数据库批量 `vexfs_find`，并通过 CLI、C ABI、ACL 和 10 万文件性能 Gate
- 适用范围：VexFS 完整产品定义；开发顺序以 `docs/plans/2026-07-21_vexfs-final-goal-and-roadmap.md` 为准

## 0. 阅读结论

### 0.0 2026-07-24 macOS 默认挂载决策

macOS 0.1 默认使用系统内置 NFS 客户端连接本机 VexFS NFS gateway。FSKit 已完成的实现和
真机证据继续保留，但降为后续原生增强，不再是首次安装、默认挂载或 0.1 发布 Gate。

这个决定只改变 macOS mount adapter，不改变数据库权威核心：NFS gateway、后续 FSKit、
Linux FUSE 和 Windows WinFsp 必须共用同一 Workspace Engine、mount runtime、数据库合同和
一致性 eval，不能形成两套文件、版本、权限或备份逻辑。

默认 NFS 路径的产品要求是：

- 用户不需要在“文件系统扩展”中寻找和启用 VexFS；
- `vexdb fs mount` 自动启动当前用户的本机 gateway，并调用系统 NFS 客户端挂载；
- NFS 服务只绑定 loopback，不作为局域网或公网文件服务器；
- mount/unmount/status/doctor 管理 gateway 生命周期和 stale mount；
- FSKit 的签名、entitlement、启用和公证不再阻塞 NFS 版 0.1，但普通 macOS 发行物仍需
  Developer ID 签名与公证，避免 Gatekeeper 阻止 CLI/gateway；
- NFS 协议版本、mount 权限提示、锁、xattr、缓存一致性、sleep/wake 和 gateway crash 必须
  通过真机 Gate 后，才能宣传“安装后无缝 Bash”。

2026-07-24 已完成连接 SQLite mount runtime 的 localhost NFSv3 adapter，并设为 macOS CLI
默认 driver。gateway 只监听 loopback，由 `mount/unmount/status/doctor` 管理独立 PID、端口、
日志和受保护状态目录。真实 macOS 26.3.1 arm64 eval 共 45 项，覆盖 Bash、hardlink、symlink、
mode、fsync、xattr、原子替换、open-then-unlink、单机跨进程 `flock`/`fcntl`、轻量 npm/Cargo、Git、gateway `SIGKILL` 恢复、卸载/重挂载和完整 workspace 快照恢复；
package smoke 30 项也通过。公开版 `nfsserve` 缺少 NFSv3 LINK/COMMIT，当前仓库固定维护一个
小型 fork，LINK 调用数据库 hardlink 合同，COMMIT 调用强同步并返回同一 server verifier。
另有独立的真实 OpenCode Gate：OpenCode 1.18.3 使用 `openai/gpt-5.4-mini` 在默认 NFS
挂载项目中修复代码并运行测试，13 项检查在 28.349 秒内通过；卸载和重挂载后的数据库内容、
Git diff 与单测结果保持一致。

默认挂载使用 bounded soft NFS：固定 10 秒重传间隔、最多 4 次，并设 60 秒 dead timeout。
它让 gateway 崩溃时的文件调用最终返回错误而不是永久卡死，同时覆盖 runtime 最长 30 秒的
数据库 busy timeout。应用必须把返回的 I/O 错误当成失败，不能假定写入成功。

macOS 写入的 AppleDouble 由 gateway 吸收为目标 inode 的
`io.vexdb.macos.appledouble` xattr，不进入用户目录、dentry、grep 或快照树。1000 个 2-byte
小文件创建为 4.862788 秒、205.643 files/s；同轮 APFS 为 0.059030 秒、16940.431 files/s，
约慢 82.4 倍。这个吞吐与此前 FSKit 约 175 files/s 同级，满足“不比现有默认入口明显退化”的
0.1 入口条件，但远未接近原生盘，列为 0.2 批量提交、写回合并和 metadata cache 优化项。

后文中的 FSKit 完成状态仍是历史事实；凡是把 FSKit 写成“默认、MVP 主入口或当前发布前置”的
旧表述，均由本节替代。

VexFS 是由 PostgreSQL、DuckDB 或 SQLite 加载和管理的数据库文件管理扩展。三个数据库共享文件合同，但不要求同时完成。macOS + SQLite 和 Linux + SQLite 的真实挂载预览已经跑通；PostgreSQL `0.4.0-alpha.1` 已完成数据库内合同、format v2、真实 role、ACL、审计、跨 gateway 锁与缓存失效、libpq HostStore、macOS FSKit、Linux FUSE、数据库重启以及逻辑和物理备份恢复。两台真实 Mac 已通过回环隧道串行挂载同一 PG workspace，各运行一次真实 OpenCode，完成创建、继承修改、双快照和整工作区一键恢复；局域网直连 FSKit 只需要用户完成一次 macOS“本地网络”授权。Windows 使用 WinFsp 接入同一 mount runtime；DuckDB 后续先提供 SQL、CLI 和 worktree。

它在数据库中提供文件、目录、路径、版本、快照、权限、审计、配额、完整性检查和逻辑导入导出。文件修改可以和普通业务 SQL 在同一个事务中提交或回滚。

用户安装并完成一次数据库连接配置后，VexFS 把工作区挂载为真实操作系统目录。人和模型进入该目录后，可以直接使用 Bash、`ls`、`cat`、`rg`、`sed`、`python`、`git`、编译器和 `apply_patch`，不需要给命令增加 `vexfs` 前缀。

VexFS 同时保留两种回退入口：

1. 用 `vexfs ls/cat/find/grep/cp/mv/rm` 等直接命令操作数据库中的文件；
2. 在容器、端侧或不能挂载的环境中，用 `vexfs shell` 或 `vexfs exec` 建立临时 worktree，最后把差异事务性提交回数据库。

最重要的边界是：

> **数据库管理 VexFS，VexFS 管理数据库中的文件；mount gateway 只把这些文件展示给 Bash。**

数据库扩展是权威核心。mount gateway 是随产品安装的非权威访问进程：它可以保存当前 handle 和未发布写入的临时状态，但不能把这些状态当作已经提交的文件，也不能绕过数据库事务、权限和版本检查。

VexFS 有两种不同的事务入口，文档和产品界面必须明确区分：

1. SQL 调用可以参加调用者当前数据库事务，实现“文件 + 业务表”原子提交；
2. mount 中的普通 Bash 操作由 gateway 建立短事务，不能自动加入应用已经打开的数据库事务，多条 Bash 命令也不会自动合成一个事务。

macOS 默认挂载改为系统 NFS client + 本机用户态 gateway，不再受 FSKit V2
`FSPathURLResource` 的 macOS 26.0 最低版本约束；默认 NFS core 已完成 macOS 13.0 deployment
target 构建，Intel/Apple Silicon 的完整支持范围仍由发行包真机验证决定。现有 macOS 26.0+ Apple Silicon FSKit 实现作为后续可选
原生 adapter 保留。Linux 已有 libfuse3 预览，Windows 后续使用 WinFsp。
打包时必须同时写入 `minimum_macos=13.0` 和 `fskit_minimum_macos=26.0`，并保证
NFS gateway 同时能在压缩包目录和 `~/.local` 安装目录找到随包的 PostgreSQL runtime。

### 0.1 最终目标和开发顺序

VexFS 的不变最终目标、阶段边界、完成条件和当前下一步统一记录在：

`docs/plans/2026-07-21_vexfs-final-goal-and-roadmap.md`

本产品规格负责定义“产品做什么”，不再单独维护另一套阶段顺序。其他文档与路线图冲突时，以路线图为准。

## 1. 优先级定义

| 优先级 | 含义 |
|---|---|
| MVP Gate | 首个 macOS + SQLite 垂直闭环必须具备，用来决定是否继续投入 |
| P0 | 完整产品没有该能力则核心价值不成立，但不要求全部进入首个技术闭环 |
| P1 | 产品可用和可推广需要，允许在 P0 后完成 |
| P2 | 增强能力，不阻塞首个可用版本 |

文档中的“必须”表示对应阶段的验收硬要求；“应该”表示默认实现；“可以”表示可选能力。需求表中的 P0 是产品优先级，不等于全部进入 Phase 1；每个阶段的实际范围和完成条件以路线图为准。

### 1.1 CLI 评审保留项和阶段边界

2026-07-17 CLI 评审中保留以下长期合同：

- v1 的 workspace 使用线性历史，不引入 branch、DAG 或 detached HEAD；
- file version、workspace commit、snapshot 和 worktree 是不同对象；
- restore 产生新 commit，不删除、不重写旧历史；
- restore、worktree commit 和 changeset publish 使用 expected-head/base version 防止静默覆盖；
- request-id 用于结果不确定时的安全重试；
- commit 创建后不可修改，只允许追加 annotation；
- 原生 `grep`/`rg` 经过 mount 时按文件扫描，不能透明改写成数据库查询。

这些是完整产品合同，不全部进入当前阶段。功能依赖顺序仍是：

1. 先保证真实挂载、基础 Bash、同步、重开和故障正确性；
2. 再保证可重建的 workspace 历史、snapshot、restore 和 expected-head；
3. 再补数据库身份、ACL、审计、配额、保留策略和备份恢复；
4. 最后增加 worktree、`txn run` 和可选的普通文本索引。

这只是能力依赖，不是另一套发布阶段。当前实施顺序以路线图中的 Phase 1–5 为准。

文本索引不是 VexFS 核心 schema 的前置条件。BM25、向量索引、Embedding 和语义检索不进入 VexFS 文件系统核心，也不阻塞任何文件系统阶段。

SQLite MVP 没有数据库用户认证。该阶段的 principal 只能来自受签名 App/extension、当前 macOS 用户、security-scoped 数据库目录和数据库文件权限共同建立的本地身份边界；不能把 PostgreSQL 角色语义直接套到 SQLite。

### 1.2 当前实现状态（2026-07-24）

SQLite 合同为 `0.9.0`，runtime ABI 为 1。除单文件版本外，当前实现已经提供完整 workspace 时间点恢复：

- 每个 workspace commit 增量保存 inode、dentry 和 xattr 状态；文件内容继续复用不可变 file version；
- snapshot 是“名称 → 固定 workspace commit”的不可变书签，创建不复制目录树或文件内容；
- `snapshot create/list/show/diff/drop/restore` 已同时进入 SQL、C ABI 和 CLI；
- restore 覆盖目录创建、删除和移动，以及文件内容、mode、symlink target 和 xattr，并生成新的 workspace commit；
- restore 必须带 expected-head；旧 head、并发 restore 和打开/retained handle 都会被拒绝，不能静默覆盖；
- 产品尚未正式发版，当前只接受 `0.9.0` schema；其他版本明确拒绝且不改写数据库，不保留历史迁移代码；
- SQLite Online Backup 会同时保存 snapshot、历史树和未发布 staging；数据库重开后仍可 diff 和 restore；
- 3,000 文件实测 snapshot create 约 0.16 ms、单文件变化 diff 约 0.65 s、完整 restore 约 1.47 s；10,000 文件压力档分别约 0.17 ms、6.83 s 和 15.42 s。

现有数据库合同和已接入平台的 POSIX 兼容能力包括：

- 文件和目录创建时保存 `0000..0777` mode，`chmod` 后重新挂载仍保留，可执行脚本可以直接运行；
- 符号链接使用独立 inode，target 原样保存，支持相对链接、绝对链接和 dangling link；
- FSKit、C ABI、SQL 和 SQLite schema 对 mode/symlink 使用同一套合同；
- Git 真机探测为 `core.filemode=true`、`core.symlinks=true`，init/add/commit/checkout/gc 已通过；
- xattr 已由数据库保存，不需要把它退化为普通文件内容；
- hardlink、owner/group 元数据和便携 ACL 已进入 SQLite schema、SQL/C ABI/CLI 和 eval；
- Linux libfuse3 已接入 hardlink、chown 和 xattr，并通过 root、普通用户、Git 工作区和重挂载真机测试。
- macOS FSKit 与 Linux libfuse3 已共用 mount 一致性 eval；四类时间戳、原子并发
  `O_APPEND`、最小 `flock`/`fcntl`、打开文件 rename/unlink、强制卸载和 helper 崩溃恢复已验证；
- 同一 SQLite 数据库已完成 macOS → Linux → macOS 往返，内容、Unicode、mode、链接、
  xattr、便携 ACL、数字 UID/GID、历史和跨系统快照恢复均通过；
- macOS `preview.22` 已完成 Developer ID 签名、Apple 公证、staple、本机安装和真实 FSKit
  回归，另一台没有源码和完整 Xcode 的 M1 Mac 也已完成安装、挂载、Git、OpenCode 和恢复
  验证。`preview.36` 又修复了重签后 DER entitlement 与原子升级事务；本机已经允许
  `preview.37` 的 FSKit module，`doctor` 连续确认 `extension=enabled` 和 `mount_ready=true`；
  用户再次唤醒系统后，本机 PostgreSQL FSKit 13/13 场景、247 项检查已全部通过，凭据生命周期
  9 项、macOS↔Linux 跨 gateway 47 项、两台 Mac 文件/快照往返 50 项、双机故障恢复 25 项，
  以及双 Mac 串行真实 OpenCode 的 7 + 9 + 21 条检查也通过。后者覆盖第一台创建、第二台继承
  同一 Git workspace、第一台回看、两个完整快照和一键恢复。PG 当前 grep 仍是数据库扫描，
  不宣称索引加速。该包仍没有 Apple 公证票据，不能作为正式分发
  结论；安装器已经修复旧 extension UUID 刷新问题，必须从干净提交构建、公证并 staple
  `preview.38` 后再做最终发行复测。Linux
  x86_64/AArch64 `preview.9`
  包已完成原生无 root 安装、扩展加载、自检、文件读写和卸载回归。
- Python、Node.js、Go、Rust 和 Git 已在 FSKit 挂载盘中运行并通过重挂载复测；SQLite
  直连已通过 10 万文件，FSKit 已通过 1 万文件；10 万文件已完成创建/遍历/抽样读取，
  但普通 `rg` 在 3,600 秒内未完成；900 秒混合重开稳定性完成 594,000
  次操作、2,970 次重开、297 个快照和 118 次 checkpoint，未留下 staging；
- FSKit 对不超过 1 MiB 的只读普通文件使用打开期内容快照，但不启用 kernel data
  cache；写打开和 truncate 会失效快照，大文件继续使用数据库 handle。
- 幂等结果至少保留最近 65,536 条，每 4,096 条完成记录批量清理一次；显式
  `vexfs_init()` 始终完整检查 schema，普通热路径在同一连接内复用已验证状态。
- `vexdb fs grep` 已提供数据库内批量扫描；可显式开启 FTS5 trigram 索引。索引默认关闭，
  不增加普通写入成本；开启后写入、版本恢复和工作区快照恢复都维护索引。少于 3 个字符
  的查询自动回退扫描，宿主没有 FTS5 时也回退扫描并标记索引待重建。
- 1 万文件索引查询为 0.013 秒，构建为 0.024 秒，峰值 RSS 50.9 MiB。10 万完整
  mount 流程尚未用新索引包复测，不能写成通过。

已有的单文件版本能力包括：

- 普通写入生成不可变 manifest 和递增版本；manifest 按 64 KiB 块引用数据库内的物理 chunk；
- `vexfs_history` 支持有上限的分页查询，`vexfs_read_version` 读取指定版本；
- `vexfs_restore_version` 创建新版本，但复用目标版本的不可变内容，不复制大 BLOB；
- 四参数恢复带 `expected_version`，当前版本已变化时返回冲突；
- CLI 已提供 `history`、`show`、`diff`、`restore` 和 `restore --dry-run`；
- SQL 事务回滚会同时撤销版本、commit 和 workspace head；两个进程并发恢复时只有一个成功；
- mount session 使用独占租约，崩溃遗留 staging 只能显式恢复，不能被新进程静默发布；
- `doctor` 不创建、不迁移旧数据库，并能报告合同兼容性和升级需要；
- SQLite 主库、WAL 和 SHM 默认限制为当前用户读写，CLI 提供稳定错误码和退出码。
- workspace 配额已覆盖当前文件数、当前内容总字节和单文件字节。计数由数据库触发器维护，
  写入、句柄 publish 和快照恢复在修改前检查，失败不会产生半个版本或 commit；hardlink 不重复计数；
- retention 已支持最近版本数和天数。GC 只能显式分批执行，保护当前版本、快照、打开或保留
  句柄以及恢复别名引用的 canonical 内容；活动挂载和 `gc pause` 都会阻止删除；
- `vexdb fs export/import` 已提供 format v2 逻辑包。逻辑包使用独立 SQLite 容器保存跨后端
  记录，不是源数据库副本；它包含 workspace 元数据、commit、inode/dentry 历史、文件版本、
  manifest、chunk、快照、xattr、ACL 和 principal 表。每条记录、每个 chunk、整文件和整包都有
  SHA-256；导入先在
  `importing` 状态完成映射和深度检查，再在同一事务中原子发布；
- export、`archive verify` 和 import 都逐个处理不超过 64 KiB 的 chunk，校验和导入
  使用同一个事务快照。HEAD 导出遇到未发布句柄时拒绝，指定快照
  导出不受后续写入影响。当前 SQLite 和 PostgreSQL 已能读写该逻辑格式；DuckDB 消费端尚未实现。

当前已经是完整 workspace 时间点恢复，但还不是完整 POSIX 文件系统。SQLite 文件版本
SHA-256、只读 check、live quota、retention、显式 GC、逻辑导入导出和 `chunked-v1` 内容模型
已实现；PostgreSQL 还完成了真实 role、路径 ACL 授权与继承、审计、跨 gateway 锁和权限/内容
失效。SQLite 仍只保存便携 ACL，不执行完整路径授权。产品整体尚未实现
history/staging/index/total quota、跨文件通用去重、setuid/setgid/sticky bit 和特殊文件。
hardlink、owner/group 和便携 ACL 已进入公共合同和 macOS/Linux mount 测试，但不能把这些
能力扩大描述为所有平台的原生 ACL 完全等价。GC 回收的是数据库可复用页，不会自动执行
`VACUUM` 缩小 SQLite 文件。snapshot 解决误改和任务检查点，SQLite Backup 或 PostgreSQL
原生备份才解决数据库灾难恢复，两者不能混为一谈。

当前符号链接采用 POSIX 行为：target 是原样字符串，可以指向挂载目录外部。它解决兼容性，但不提供路径隔离；需要隔离时仍由 Agent sandbox 负责。未来可以增加可选的 `internal` 策略，但当前版本没有实现该策略。

## 2. 背景与问题

业务系统和 Agent 经常同时保存结构化记录与文件：

- 任务状态和生成报告；
- 客户记录和合同附件；
- Agent 输入资料和中间结果；
- 分析任务和导出文件；
- 配置、日志、代码和构建结果；
- 历史版本和审计证据。

常见做法是数据库保存记录，文件系统或对象存储保存文件。这会产生以下问题：

1. 业务记录提交成功，但文件写入失败。
2. 文件写入成功，但数据库事务回滚。
3. 数据库和文件服务维护两套身份与权限。
4. 数据库备份没有包含文件，恢复时无法对齐。
5. 文件版本和业务记录版本无法对应。
6. 多个 Agent 同时修改时容易静默覆盖。
7. Agent 需要学习另一套服务协议和工具。
8. SQLite、DuckDB 和 PostgreSQL 使用三套文件方案。

VexFS 的目标是把普通文件管理放进数据库事务、权限、版本和备份边界，同时保留模型熟悉的终端工作方式。

## 3. 产品定义

### 3.1 一句话定义

> **VexFS 是数据库原生的版本化 Agent 文件工作区。它让模型使用正常终端工具工作，并把最终文件变更按数据库权限和事务提交。**

### 3.2 产品组成

VexFS 由三部分组成：

1. **数据库扩展**：权威状态、SQL 合同、事务、权限、版本、快照、审计和备份兼容。
2. **mount gateway**：通过数据库公开合同把 workspace 显示为真实操作系统目录；macOS
   默认使用 localhost NFS gateway，FSKit 是后续可选 adapter；Linux 使用 libfuse3，Windows
   后续使用 WinFsp。
3. **配套 CLI**：安装、连接、mount/unmount、直接文件命令、worktree、shell、exec、导入导出和运维入口。

数据库扩展是权威核心。没有 mount gateway 或 CLI 时，全部数据能力仍可通过 SQL 使用；mount gateway 崩溃或卸载不能损坏数据库中已经提交的文件。

### 3.3 与 VexDB-Lite 的关系

- VexFS 位于 VexDB-Lite 仓库和产品品牌下；
- VexFS 与向量索引共享多数据库适配、构建、测试和发布经验；
- VexFS 文件核心和现有向量图核心相互独立；
- 不安装或不启用向量索引时，VexFS 必须完整可用；
- VexFS 不做 Embedding、语义搜索或内容理解。

## 4. 产品目标

### 4.1 MVP Gate 目标

- 首个默认发行只支持经真机矩阵确认的 macOS 版本和 CPU、SQLite、单 gateway、单 mount 和
  单数据库 principal；
- 采用系统 NFS client + 只监听 loopback 的当前用户 gateway，把一个 workspace 挂载为真实目录；
- 支持普通文件和目录的最小调用集，以及 O_CREAT、O_EXCL、O_TRUNC 和 O_APPEND；
- 明确采用 close-to-open 一致性，不宣传完整 POSIX 强一致；
- NFS attribute/data cache 使用可测试的有界 TTL、COMMIT/fsync 和主动失效规则，权限与正确性优先；
- 允许在 gateway 内对 metadata 和小型只读文件做有边界的缓存，但必须覆盖并通过写打开、
  truncate、rename、unlink、同名重建和大文件回退测试；
- NFS `WRITE`、`COMMIT`、close、fsync 和 unmount 的成功含义及故障恢复规则可测试；
- sync 成功后的内容在 gateway 被终止和数据库 reopen 后仍完整；
- `._*` AppleDouble 不能作为普通用户文件进入路径、历史、快照、配额和 grep；需要保留的
  macOS xattr 必须映射到统一 VexFS xattr 合同；
- 完成签名并公证的 macOS CLI、gateway 和 SQLite 扩展组合包，以及版本握手、doctor 和卸载流程；
- Bash、Python 和基础 Git 流程不需要 VexFS 专用命令；
- 1,000 个文件、1 GiB 当前内容和 100 MiB 单文件通过正确性测试。

MVP Gate 不等于完整产品。它只回答三个问题：数据库能否保持唯一权威、真实 Bash 是否可用、故障后是否不产生伪成功和半文件。

### 4.2 P0 目标

- 在数据库中提供稳定的文件和目录模型；
- 文件修改参加当前数据库事务；
- 文件和普通业务表可以原子提交；
- 提供不可变文件版本、工作区 commit 和快照；
- 提供数据库身份驱动的路径权限；
- 数据库原生备份包含全部 VexFS 状态；
- macOS 用户完成一次系统扩展授权和 context 配置后，可以把 workspace 挂载为真实目录；
- 模型可以在挂载目录中直接用普通终端工具完成文件任务；
- mount 的每次文件操作都经过数据库身份、权限、配额、版本和审计检查；
- 单个文件系统操作不能产生半状态；
- 多文件 changeset 和业务 SQL 可以显式原子提交；
- 不能挂载时，worktree 差异可以全部提交或全部回滚；
- 并发冲突不能静默覆盖；
- 三端共享相同的逻辑文件合同。

### 4.3 P1 目标

- 工作区逻辑导入导出；
- 路径权限继承、审计和配额；
- 保留策略和分批垃圾回收；
- 跨 SQLite、PostgreSQL 和 DuckDB 迁移；
- 完整的检查和安全修复流程；
- 增量 checkout 和大目录分页。
- Linux libfuse3 预览收口和 Windows WinFsp 原生挂载入口；
- mount 缓存失效通知和只读离线浏览优化。

### 4.4 非目标

- MVP Gate 不承诺 Linux、Windows、macOS 13.0 以下版本和所有容器都能真实挂载；
- 不把 mount gateway 做成另一套权威文件服务器；
- 不建立另一套网络协议；
- 不提供必须接入的 SDK；
- 不替代 ext4、APFS、NTFS 或 Git；
- 不支持设备文件、socket 或 FIFO；
- 不作为视频、模型文件或数据湖的无限对象存储；
- 不管理数据库启动、停止、WAL、复制或物理备份任务；
- 不做跨数据库分布式事务；
- 不把任意已有数据库表自动投影成文件；
- 不做语义检索、Embedding 或 Agent 内容理解。
- MVP Gate 不支持多 gateway、跨 gateway 锁和主动缓存失效；
- 最初 MVP Gate 不要求 chown、路径 ACL 继承、hardlink、共享可写 mmap 和特殊文件；当前 hardlink、owner/group 元数据和便携 ACL 已进入合同，但仍不做完整 POSIX 权限判定；
- 当前已支持 symlink 和 xattr，但不承诺编译器全量产物、`node_modules` 或大型构建缓存适合作为长期版本化数据；
- DuckDB 可写 mount 不进入 MVP Gate；除非 gateway 是唯一写进程，否则不承诺与外部 DuckDB 进程共享写入。

## 5. 名词定义

| 名词 | 定义 |
|---|---|
| workspace | 最高文件隔离单位，拥有根目录、owner、head commit、权限、配额和保留规则 |
| inode | 文件、目录或符号链接的稳定身份 |
| dentry | 父目录下的名称到 inode 的映射 |
| file version | 某个 inode 在一个 commit 中的不可变状态 |
| workspace commit | 一次成功 changeset 产生的工作区版本号 |
| snapshot | 指向固定 workspace commit 的不可变名称 |
| changeset | 一组新增、修改、复制、移动和删除操作 |
| mount gateway | 使用数据库连接和公开 VexFS 合同，把 workspace 显示为操作系统目录的非权威进程 |
| mount point | 用户可以直接 `cd` 进入的真实操作系统挂载目录 |
| file handle | mount gateway 为一次 open 保存的短期句柄；不代表长期数据库事务 |
| worktree | 从固定 commit 导出的临时本地普通文件目录 |
| principal | 数据库连接能够证明的用户或角色身份 |
| native backup | PostgreSQL、DuckDB 或 SQLite 自己提供的完整数据库备份 |
| logical export | VexFS 提供的单工作区、跨数据库可交换格式 |

## 6. 目标用户

### 6.1 Agent 平台开发者

希望 Agent 使用正常终端工具，但文件最终进入数据库权限、版本和审计边界。

### 6.2 数据库应用开发者

需要在业务事务中同时保存记录和附件、报告、配置或生成物。

### 6.3 企业数据平台团队

需要多用户共享、数据库角色、审计、备份恢复和统一运维。

### 6.4 本地与端侧应用开发者

希望 SQLite 或 DuckDB 单文件数据库同时保存业务数据和版本化文件工作区。

## 7. 核心场景

### 场景 A：业务状态与文件原子提交

```sql
BEGIN;

UPDATE tasks
SET status = 'completed'
WHERE id = 42;

SELECT vexfs_write(
  'task-files',
  '/tasks/42/report.md',
  CAST('# Task report' AS BLOB)
);

COMMIT;
```

任何一步失败，任务状态和报告都必须回滚。

### 场景 B：Agent 使用直接命令

```bash
vexfs --workspace task-files ls /tasks/42
vexfs --workspace task-files cat /tasks/42/report.md
vexfs --workspace task-files grep -R -n 'TODO' /tasks
vexfs --workspace task-files stat /tasks/42/report.md
```

### 场景 C：不能挂载时使用任意终端工具

```bash
vexfs shell task-files

find . -name '*.md'
rg 'TODO' .
sed -i.bak 's/draft/final/g' tasks/42/report.md
python scripts/generate.py
tar -czf output.tar.gz output/

vexfs worktree status
vexfs worktree commit -m 'finish task 42'
```

### 场景 D：并发冲突

Agent A 和 Agent B 从 commit 100 checkout 同一文件。Agent A 先提交 commit 101。Agent B 提交时必须收到明确冲突，不能覆盖 commit 101；Agent B 的本地差异必须保留。

### 场景 E：权限控制

报告作者可以在 `/reports/team-a` 创建和修改文件，但不能读取 `/reports/team-b`，不能修改 ACL，也不能恢复整个工作区。

### 场景 F：误操作恢复

用户创建快照后批量修改。出现问题时，可以恢复单文件、目录或整个工作区。恢复动作本身产生新 commit，不删除历史。

### 场景 G：数据库灾难恢复

数据库管理员使用数据库原生工具备份和恢复。恢复后业务表、VexFS 文件、版本、快照、权限和审计必须一致。

### 场景 H：跨数据库迁移

用户把 SQLite 工作区导出为 VexFS 逻辑包，再导入 PostgreSQL。目录、内容、版本、快照和 checksum 必须一致；数据库特有身份需要显式映射。

### 场景 I：安装后无缝进入 Bash 工作区

数据库扩展安装和初始化完成后，用户只需要配置一次数据库连接：

```bash
vexfs context add local-db --engine postgres --dsn postgres://localhost/mydb
vexfs context use local-db
vexfs workspace use task-files
vexfs mount ~/VexFS/task-files
cd ~/VexFS/task-files
```

挂载完成后，目录是数据库 workspace 的实时视图。用户直接使用系统原生命令：

```bash
pwd
ls
cat tasks/42/report.md
rg 'TODO' .
sed -i.bak 's/draft/final/g' tasks/42/report.md
python scripts/generate.py
git diff
vexfs mount status
```

用户不需要给 `ls/cat/rg/sed/python` 添加 `vexfs` 前缀。文件在平台 adapter 成功
publish/sync 后提交为新版本；macOS 默认 NFS 的 WRITE 返回 FILE_SYNC，显式 COMMIT/fsync、
gateway 同步和安全卸载都映射到数据库强同步。可选 FSKit 继续使用 `synchronize` 和可报告
结果的 close 路径。需要把多个文件和业务 SQL 一起原子提交时，必须使用显式
changeset/事务入口，不能假设多条 Bash 命令天然属于同一个数据库事务。

## 8. 系统边界和所有权

```text
模型 / 人 / 自动化
        |
        +--------------------+---------------------+
        |                    |                     |
        v                    v                     v
  操作系统文件 API       VexFS CLI              VexFS SQL
        |                    |
        v                    +---- 直接命令 / worktree
  mount gateway
        |
        +---------- 公开 VexFS SQL/文件合同 --------+
                             |
                             v
                   PostgreSQL / DuckDB / SQLite
                      ├── 管理扩展生命周期
                      ├── 管理内存、线程和连接
                      ├── 管理事务、锁和并发
                      ├── 管理 WAL 和崩溃恢复
                      ├── 管理数据库身份和外层权限
                      ├── 管理数据库备份和复制
                      └── 保存 VexFS 全部权威状态
```

VexFS 禁止：

- 直接读写数据库物理文件；
- 自己实现 WAL、buffer pool 或数据库锁；
- 自行提交用户事务；
- 把权威文件状态保存在数据库之外；
- 接受 CLI 自报身份替代数据库身份；
- 让 mount gateway 自报身份替代数据库连接身份；
- 让 mount gateway 保存数据库之外的最终文件状态；
- 让 worktree 直接修改内部表；
- 从数据库扩展初始化函数或 SQL callback 启动 FSKit/FUSE/WinFsp loop、网络服务或未受宿主管理的长期线程。SQLite 场景允许系统管理的 FSKit extension 在自身进程中打开 SQLite 连接。

## 9. 产品原则

### 9.1 数据库优先

数据库负责生命周期、事务、身份、权限上限、备份和恢复。VexFS 只实现文件规则。

### 9.2 SQL 是权威合同

所有数据能力必须存在对应 SQL 合同。mount gateway 和 CLI 只能调用公开合同；worktree 只能通过 changeset SQL 提交。

### 9.3 事务优先

文件操作必须参加当前数据库事务。完整旧状态和完整新状态之间不能出现半状态。

### 9.4 模型终端优先

安装后优先提供真实挂载目录，不重新实现完整 shell。任意现成程序直接通过操作系统文件 API 使用 VexFS；不能挂载时，再使用 CLI 或真实临时 worktree。

### 9.5 原生备份优先

数据库原生备份是灾难恢复方式。文件版本和快照不是数据库备份的替代品。

### 9.6 逻辑一致，能力分级

三端统一文件结果、错误和版本行为；身份强度、并发上限和运维方式按宿主能力分级。

## 10. 功能需求

### 10.1 扩展生命周期

| ID | 优先级 | 需求 |
|---|---|---|
| FR-LC-001 | P0 | VexFS 必须由宿主数据库扩展机制加载和卸载。 |
| FR-LC-002 | P0 | 初始化必须创建受保护的内部对象，并且可重复执行。 |
| FR-LC-003 | P0 | 必须提供扩展版本、内部 schema 版本和能力查询。 |
| FR-LC-004 | P0 | 升级必须使用数据库支持的升级路径，失败时不能留下半升级状态。 |
| FR-LC-005 | P0 | 卸载不得静默删除已有文件数据；破坏性卸载需要显式确认步骤。 |
| FR-LC-006 | P0 | 普通用户不能直接写内部表或调用内部函数。 |
| FR-LC-007 | P1 | 必须提供只读升级前检查和升级后完整性检查。 |

### 10.2 工作区

| ID | 优先级 | 需求 |
|---|---|---|
| FR-WS-001 | P0 | 支持创建、列出、查看、冻结、解冻和删除工作区。 |
| FR-WS-002 | P0 | 每个工作区必须拥有独立根目录和稳定 workspace id。 |
| FR-WS-003 | P0 | 每个工作区必须维护单调递增的 head commit。 |
| FR-WS-004 | P0 | 工作区必须拥有 owner principal。 |
| FR-WS-005 | P0 | 冻结后允许读取，不允许产生新文件 commit。 |
| FR-WS-006 | P0 | 删除非空工作区必须显式指定，并写审计记录。 |
| FR-WS-007 | P1 | 工作区必须提供文件数、目录数、当前字节、历史字节和快照数统计。 |
| FR-WS-008 | P1 | 支持工作区级配额和保留策略。 |
| FR-WS-009 | P1 | 支持只 checkout 某个子目录。 |

### 10.3 文件、目录和路径

| ID | 优先级 | 需求 |
|---|---|---|
| FR-FS-001 | P0 | 支持普通文件和目录。 |
| FR-FS-002 | P0 | 路径必须从工作区根开始解析，不能越过根目录。 |
| FR-FS-003 | P0 | 文件名禁止 NUL 和 `/`，同目录名称必须唯一。 |
| FR-FS-004 | P0 | 支持 mkdir、create、read、write、range read、range write 和 truncate。 |
| FR-FS-005 | P0 | 支持 copy、move、rename、replace、remove 和递归删除。 |
| FR-FS-006 | P0 | move、replace 和 remove 必须原子执行。 |
| FR-FS-007 | P0 | 支持 stat、分页 list 和稳定排序。 |
| FR-FS-008 | P0 | 支持 size、checksum、创建时间、修改时间、owner 和 mode 元数据。 |
| FR-FS-009 | P0 | 文件内容必须使用分块 BLOB，不能要求一次加载完整大文件。 |
| FR-FS-010 | P0 | 发布的 manifest 不能缺少任何 chunk。 |
| FR-FS-011 | P1 | 符号链接 target 按 POSIX 原样保存，支持相对、绝对、dangling 和越界目标；VexFS 不把它当作 Agent 沙箱。 |
| FR-FS-012 | P1 | 后续可增加显式 `internal` 策略，启用后拒绝绝对和越界目标；不得静默改变已有 POSIX 合同。 |
| FR-FS-013 | P1 | 支持 hardlink；当前 SQLite 合同和 Linux adapter 已接入，其他平台按统一合同补齐。 |
| FR-FS-014 | P0 | 不支持设备文件、socket 和 FIFO。 |
| FR-FS-015 | P0 | 默认单文件上限为 100 MiB，可由管理员调低或调高。 |
| FR-FS-016 | P0 | 默认单工作区当前内容上限为 10 GiB，可由管理员配置。 |

### 10.4 事务和 changeset

| ID | 优先级 | 需求 |
|---|---|---|
| FR-TX-001 | P0 | VexFS 写操作必须参加当前数据库事务。 |
| FR-TX-002 | P0 | VexFS 不得在内部自行 commit 用户事务。 |
| FR-TX-003 | P0 | 数据库 rollback 后不能留下可见文件修改。 |
| FR-TX-004 | P0 | 文件操作可以和普通业务表更新原子提交。 |
| FR-TX-005 | P0 | savepoint 和 rollback-to 的行为必须明确并通过三端合同测试。 |
| FR-TX-006 | P0 | worktree 必须通过单个 changeset 提交全部差异。 |
| FR-TX-007 | P0 | 一个成功 changeset 只产生一个 workspace commit。 |
| FR-TX-008 | P0 | changeset 必须携带 base commit 和被修改对象的 expected version。 |
| FR-TX-009 | P0 | 发现并发修改时必须返回冲突，不能静默覆盖。 |
| FR-TX-010 | P0 | changeset 中任意操作失败时，全部操作必须回滚。 |
| FR-TX-011 | P1 | 写操作支持可选 request id，重复请求不能产生重复 commit；至少保留最近 65,536 条完成结果，并以固定批次清理更早记录，避免长期 mount 无限增长。 |

### 10.5 版本、历史和快照

| ID | 优先级 | 需求 |
|---|---|---|
| FR-VS-001 | P0 | 每次成功修改必须产生不可变文件版本。 |
| FR-VS-002 | P0 | 文件版本必须记录 inode、version id、commit id、checksum、size、principal 和时间。 |
| FR-VS-003 | P0 | 支持查看文件历史和读取指定版本。 |
| FR-VS-004 | P0 | 支持比较两个 workspace commit 的目录和文件变化。 |
| FR-VS-005 | P0 | 快照必须指向固定 commit，创建快照不能复制全部文件内容。 |
| FR-VS-006 | P0 | 快照创建后保持不可变。 |
| FR-VS-007 | P0 | 支持恢复单文件、目录和整个工作区。 |
| FR-VS-008 | P0 | 恢复必须产生新 commit 和审计记录，不覆盖历史。 |
| FR-VS-009 | P0 | 快照引用的版本和 chunk 不能被保留策略或 GC 删除。 |
| FR-VS-010 | P1 | 支持给 commit 添加消息和外部业务引用。 |

### 10.6 权限和身份

统一权限集合：

| 权限 | 含义 |
|---|---|
| traverse | 经过目录解析下级路径 |
| list | 查看目录成员 |
| read | 读取文件内容和基本属性 |
| create | 在目录中创建文件或子目录 |
| write | 修改已有文件内容和属性 |
| delete | 删除目录项 |
| rename | 移动或改名目录项 |
| history | 查看历史版本和 diff |
| snapshot | 创建和恢复快照 |
| share | 修改 ACL |
| admin | 修改 owner、配额和保留规则 |

| ID | 优先级 | 需求 |
|---|---|---|
| FR-AC-001 | P0 | 公开 SQL 函数默认使用当前数据库连接身份。 |
| FR-AC-002 | P0 | 有效权限必须同时满足数据库外层权限和 VexFS 路径 ACL。 |
| FR-AC-003 | P0 | workspace owner 默认拥有 admin。 |
| FR-AC-004 | P0 | ACL 支持工作区根和任意目录或文件。 |
| FR-AC-005 | P0 | 子路径默认继承最近祖先 ACL；显式 deny 优先于 allow。 |
| FR-AC-006 | P0 | 删除、覆盖和改名必须检查源对象、源目录、目标目录和目标对象权限。 |
| FR-AC-007 | P0 | checkout 只能导出当前 principal 有 read 权限的内容。 |
| FR-AC-008 | P0 | worktree commit 必须重新检查全部权限。 |
| FR-AC-009 | P0 | worktree 本地 chmod、owner 或 ACL 不能直接成为数据库权限。 |
| FR-AC-010 | P0 | CLI 不能通过参数伪造 principal。 |
| FR-AC-011 | P0 | PostgreSQL principal 对应数据库 role。 |
| FR-AC-012 | P0 | DuckDB 和 SQLite 只能使用宿主能够证明的连接身份，不能假装拥有 PG 级角色系统。 |
| FR-AC-013 | P1 | chmod 作为兼容命令映射到 VexFS ACL；普通文件 execute 只作为元数据，不代表数据库执行文件。 |
| FR-AC-014 | P1 | chown 和 ACL 修改要求 share 或 admin。 |

### 10.7 审计

| ID | 优先级 | 需求 |
|---|---|---|
| FR-AU-001 | P0 | 所有文件写入、删除、改名、恢复和权限修改必须写审计记录。 |
| FR-AU-002 | P0 | 审计必须记录 principal、workspace、commit、操作、对象和前后版本。 |
| FR-AU-003 | P0 | 审计不能保存密码、连接串或文件正文。 |
| FR-AU-004 | P0 | 审计记录不能由普通用户修改或删除。 |
| FR-AU-005 | P1 | 审计支持按 commit、principal、路径和时间查询。 |
| FR-AU-006 | P1 | 支持可选审计 hash 链，用于发现记录被改写。 |
| FR-AU-007 | P1 | 工作区可以开启读取审计，默认关闭以控制数据量。 |

PostgreSQL `0.4.0-alpha.1` 当前对 workspace、目录、文件、link、owner、mode、时间戳、
xattr、ACL、单文件恢复、workspace 恢复和 format v2 导入等变更写审计。每行保存由数据库
认证的 `session_user` 与 role OID、workspace 名称和 ID、commit、操作、路径、inode、
`before_version` 与 `after_version`；不保存文件正文、xattr 值、密码或连接串。删除 workspace
前先写审计，外键使用 `ON DELETE SET NULL`，因此删除后仍保留 workspace 名称和最后操作，
不会产生无法识别的孤立记录。普通 role 不能读取审计，`vexfs_check` 会报告对象或前后版本
不完整的审计行。

### 10.8 配额、保留和垃圾回收

| ID | 优先级 | 需求 |
|---|---|---|
| FR-QT-001 | P0 | 必须支持最大单文件、当前内容字节和文件数限制。 |
| FR-QT-002 | P0 | changeset 发布前必须计算新增空间并检查配额。 |
| FR-QT-003 | P0 | 配额失败不能产生部分 commit。 |
| FR-QT-004 | P1 | 必须分别统计 live bytes 和 retained history bytes。 |
| FR-QT-005 | P1 | 保留策略支持保留最近版本数和保留天数。 |
| FR-QT-006 | P1 | GC 只能删除没有活动版本、快照或导出任务引用的对象。 |
| FR-QT-007 | P1 | GC 必须分批执行，避免不可控长事务。 |
| FR-QT-008 | P1 | 工作区冻结和备份验证期间可以暂停 GC。 |

### 10.9 数据保护、备份和恢复

VexFS 使用四层数据保护：

1. 文件版本；
2. 工作区快照；
3. 数据库原生备份；
4. 工作区逻辑导出。

| ID | 优先级 | 需求 |
|---|---|---|
| FR-BK-001 | P0 | 数据库原生备份必须包含全部 VexFS 元数据、内容块、版本、快照、ACL 和审计。 |
| FR-BK-002 | P0 | 数据库恢复后，VexFS 和普通业务表必须处于同一恢复点。 |
| FR-BK-003 | P0 | VexFS 不得自己调度或冒充数据库物理备份。 |
| FR-BK-004 | P0 | 必须提供备份后完整性验证入口。 |
| FR-BK-005 | P0 | 快照和文件版本不能被描述为灾难恢复备份。 |
| FR-BK-006 | P1 | 支持从固定快照导出单个工作区。 |
| FR-BK-007 | P1 | 逻辑导出必须携带格式版本、记录 checksum、内容 checksum 和源 commit。 |
| FR-BK-008 | P1 | 导入必须先进入隐藏状态，全部校验成功后原子发布。 |
| FR-BK-009 | P1 | 导入失败不能发布半个工作区。 |
| FR-BK-010 | P1 | 跨数据库导入必须验证目录结构和内容 hash 一致。 |
| FR-BK-011 | P1 | principal 映射必须显式提供，不能自动扩大权限。 |
| FR-BK-012 | P1 | 逻辑导出不得保存数据库密码或外部密钥。 |

### 10.10 完整性检查和修复

| ID | 优先级 | 需求 |
|---|---|---|
| FR-CK-001 | P0 | 提供默认只读的 workspace check。 |
| FR-CK-002 | P0 | check 必须验证 inode、dentry、manifest、chunk、checksum、commit 和 snapshot 引用。 |
| FR-CK-003 | P0 | 缺失 chunk 不能用空数据替代后报告成功。 |
| FR-CK-004 | P0 | 发现损坏时必须返回对象、类型和建议动作。 |
| FR-CK-005 | P1 | repair 必须先生成计划，用户确认后执行。 |
| FR-CK-006 | P1 | repair 不能静默删除用户文件或历史。 |
| FR-CK-007 | P1 | 修复动作必须产生审计记录。 |

### 10.11 CLI 直接命令

| ID | 优先级 | 需求 |
|---|---|---|
| FR-CLI-001 | P0 | CLI 必须使用 PostgreSQL、DuckDB 或 SQLite 正常连接方式。 |
| FR-CLI-002 | P0 | CLI 必须支持显式 workspace 和当前 workspace 配置。 |
| FR-CLI-003 | P0 | 直接读命令必须把正常结果写 stdout，诊断写 stderr。 |
| FR-CLI-004 | P0 | 直接写命令默认各自使用一个数据库事务。 |
| FR-CLI-005 | P0 | 文件不存在、权限不足、冲突、空间不足和连接失败必须返回不同退出码。 |
| FR-CLI-006 | P0 | CLI 不能直接修改内部表。 |
| FR-CLI-007 | P0 | CLI 不得把密码写入命令历史、worktree 清单或日志。 |
| FR-CLI-008 | P0 | cat/get/export 等大内容操作必须流式处理。 |
| FR-CLI-009 | P1 | 支持 JSON 输出，供 Agent 稳定解析。 |
| FR-CLI-010 | P1 | 支持安静模式、详细模式和无颜色模式。 |
| FR-CLI-011 | P1 | 支持命令超时、连接超时和 busy 重试参数。 |
| FR-CLI-012 | P0 | 支持保存不含密码的数据库 context，并切换当前 context。 |
| FR-CLI-013 | P0 | 支持保存当前 workspace，避免每条命令重复传参。 |
| FR-CLI-014 | P0 | 提供 doctor 命令检查 CLI、数据库连接、扩展版本、初始化状态和权限。 |
| FR-CLI-015 | P1 | vexfs shell 必须默认使用用户当前 shell，并把工作目录设置为 worktree 根。 |
| FR-CLI-016 | P1 | shell 退出时如果存在未提交修改，必须提示、自动提交或保留，不能静默丢弃。 |
| FR-CLI-017 | P1 | 非交互 Agent 使用 vexfs exec 时，可以在命令成功后自动提交。 |
| FR-CLI-018 | P1 | CLI 和数据库扩展版本不兼容时必须在执行写操作前拒绝并给出升级说明。 |

### 10.12 Worktree、shell 和 exec（不能挂载时的回退）

| ID | 优先级 | 需求 |
|---|---|---|
| FR-WT-001 | P1 | worktree create 必须固定 base commit。 |
| FR-WT-002 | P1 | worktree 清单必须记录 workspace、base commit、路径、inode、version、size 和 checksum。 |
| FR-WT-003 | P1 | worktree 只导出用户有权读取的普通文件、目录和安全链接。 |
| FR-WT-004 | P1 | 创建完成后不得长期保持数据库事务或锁。 |
| FR-WT-005 | P1 | worktree 必须使用权限受限的本地目录。 |
| FR-WT-006 | P1 | worktree 元数据不能包含数据库密码或访问令牌。 |
| FR-WT-007 | P1 | status 必须识别新增、修改、删除、改名和类型变化。 |
| FR-WT-008 | P1 | diff 必须支持文本摘要和机器可读结果。 |
| FR-WT-009 | P1 | commit 必须重新连接数据库并重新确认 principal。 |
| FR-WT-010 | P1 | commit 必须检查权限、配额、base commit 和 expected version。 |
| FR-WT-011 | P1 | commit 必须通过公开 changeset SQL 原子提交。 |
| FR-WT-012 | P1 | 提交失败必须保留本地 worktree 和差异。 |
| FR-WT-013 | P1 | discard 必须只删除受管 worktree，不能误删普通目录。 |
| FR-WT-014 | P1 | worktree 不能提交设备文件、socket、FIFO、绝对链接或越界链接。 |
| FR-WT-015 | P1 | vexfs shell 必须在 worktree 根目录启动真实系统 shell。 |
| FR-WT-016 | P1 | VexFS 不承诺限制 shell 子进程访问其他本地路径；进程沙箱由 Agent 运行环境负责。 |
| FR-WT-017 | P1 | vexfs exec 在命令退出非零时默认不能提交。 |
| FR-WT-018 | P1 | vexfs exec 只有命令成功且数据库提交成功时返回 0。 |
| FR-WT-019 | P1 | 支持只 checkout 子目录以及 include/exclude 规则。 |
| FR-WT-020 | P1 | 支持增量 refresh，并在覆盖本地修改前拒绝执行。 |
| FR-WT-021 | P1 | 支持 worktree 过期提示和安全清理。 |
| FR-WT-022 | P1 | 支持 commit message 和外部任务 id。 |

### 10.13 Mount gateway 与 POSIX 行为

#### 10.13.1 安装与生命周期

| ID | 优先级 | 需求 |
|---|---|---|
| FR-MNT-001 | P0 | macOS 发行物必须包含受系统信任的 VexDB CLI、SQLite 扩展和本机 NFS gateway；必须验证 Developer ID 签名、公证、安装、挂载和卸载。FSKit App Extension 是后续可选产物。 |
| FR-MNT-002 | P0 | `vexfs mount <目录>` 必须使用当前 context 和 workspace 完成挂载。 |
| FR-MNT-003 | P0 | 必须提供 `mount`、`unmount`、`mount list`、`mount status` 和 `doctor`，并显示 NFS gateway、系统 NFS client、挂载表、端口、进程和版本状态；安装了 FSKit 时再显示可选 adapter 状态。 |
| FR-MNT-004 | P0 | mount point 必须是当前操作系统用户控制的空目录，默认权限为 0700。 |
| FR-MNT-005 | P0 | 本地 mount 默认只服务当前操作系统用户，不向其他本地用户共享；Linux `allow_other` 默认关闭。 |
| FR-MNT-006 | P0 | 默认 NFS gateway 以当前用户身份运行，只绑定 loopback，由 CLI 管理生命周期；不能建立长期 root 服务。数据库扩展初始化和 SQL callback 不能启动文件系统 loop 或 NFS 服务。 |
| FR-MNT-007 | P1 | 登录后自动挂载必须显式开启，并使用 macOS 允许的用户级生命周期机制。 |
| FR-MNT-008 | P0 | mount gateway 停止后，数据库扩展、SQL 和已提交文件必须继续正常工作。 |
| FR-MNT-009 | P0 | 系统 NFS client 不可用、mount 被系统拒绝、gateway 启动失败、签名不可信或出现 stale mount 时，doctor 必须给出明确原因和清理方法；不能把 FSKit 未启用当作默认挂载失败。 |

#### 10.13.2 文件与目录操作

| ID | 优先级 | 需求 |
|---|---|---|
| FR-MNT-010 | P0 | macOS NFS adapter 支持 lookup/getattr/readdir/open/close/read/write/create，并映射到平台无关核心合同；后续 FSKit adapter 复用同一合同。 |
| FR-MNT-011 | P0 | macOS NFS adapter 支持 remove/rename/setattr/fsync/commit/statfs 等 MVP 所需操作；协议能力不足时返回稳定错误，不能假装成功。 |
| FR-MNT-012 | P0 | 支持 lseek、statfs 和 utimens；chmod、symlink、readlink、hardlink 和 chown 必须按能力矩阵接入。当前 Linux 尚需补齐 utimens 的正式验收。 |
| FR-MNT-013 | P0 | 正确处理 O_CREAT、O_EXCL、O_TRUNC、O_APPEND、只读和读写打开模式。 |
| FR-MNT-014 | P0 | 同一目录内及跨目录 rename 必须原子生效，不能短暂出现两个名称都不存在。 |
| FR-MNT-015 | P0 | open 后 unlink 必须允许已打开句柄继续使用；最后一个句柄关闭后再回收无引用版本。 |
| FR-MNT-016 | P0 | 路径规范化、`.`、`..` 和越界规则必须与 SQL 合同一致；当前符号链接明确采用 POSIX target 原样保存合同，未来的 workspace 内限制策略必须作为独立可选合同。 |
| FR-MNT-017 | P0 | 不支持设备文件、socket、FIFO 和 setuid/setgid 文件；创建时返回明确错误。 |
| FR-MNT-018 | P1 | MVP Gate 不承诺 advisory flock；Phase 0 必须记录 macOS NFS 的 `flock/fcntl` 实际行为。若默认 mount 使用 `nolocks`，doctor 和能力矩阵必须明确报告，不能声称适合在挂载内运行依赖文件锁的数据库。 |
| FR-MNT-019 | P1 | 支持 hardlink。SQLite、Linux FUSE、macOS FSKit 和默认 NFS 已接入同一合同；NFSv3 LINK 由仓库内固定 fork 实现并进入真实 eval。 |
| FR-MNT-020 | P1 | 已支持扩展属性，并把值持久化到 SQLite；未知或不存在的 xattr 返回稳定错误。 |

#### 10.13.3 写入、版本与事务

| ID | 优先级 | 需求 |
|---|---|---|
| FR-MNT-021 | P0 | 普通文件写入先进入带 request id 的未发布 staging；平台无关 `publish/sync` 以幂等短事务发布完整新版本。macOS NFS adapter 主要由 COMMIT/fsync、close 和安全卸载触发；具体映射必须由真实 syscall/protocol trace 冻结。 |
| FR-MNT-022 | P0 | 不能在文件 open 到 close 的整个期间持有长期数据库事务。 |
| FR-MNT-023 | P0 | `write` 成功的含义必须由 NFS reply、数据库 staging 和 COMMIT/fsync 边界共同定义，不能在数据只存在 gateway 易失内存时伪造已安全保存。SQLite mount 的未发布 staging 使用 WAL + NORMAL；强 publish/sync 前切回 FULL。普通 SQL 连接仍使用调用者自己的 durability 配置。 |
| FR-MNT-024 | P0 | create、mkdir、unlink、rename 和 truncate 等单个文件系统操作各自在数据库事务内原子执行；chmod/chown 在进入支持范围后遵守同一规则。 |
| FR-MNT-025 | P0 | 多条 Bash 命令和多个 file handle 默认不是一个数据库事务，文档和错误信息不能暗示它们天然原子。 |
| FR-MNT-026 | P0 | 多文件或“文件 + 业务 SQL”原子提交必须使用公开 changeset/数据库事务合同。 |
| FR-MNT-027 | P1 | 提供 `vexfs txn run -- <command>`，在隔离工作视图中运行命令，并把全部差异作为一个 changeset 提交。 |
| FR-MNT-028 | P0 | 每次成功发布文件内容都产生不可变 file version 和可审计 workspace commit。 |
| FR-MNT-029 | P0 | 并发写同一 expected version 时必须检测冲突，不能静默覆盖。 |

#### 10.13.4 身份与权限

| ID | 优先级 | 需求 |
|---|---|---|
| FR-MNT-030 | P0 | 一个 mount 实例只使用一个数据库 principal；principal 来自真实数据库认证。 |
| FR-MNT-031 | P0 | 操作系统 uid 不能替代或提升数据库 principal。 |
| FR-MNT-032 | P0 | 每次 open、目录修改和内容发布都必须在数据库内重新检查权限。 |
| FR-MNT-033 | P0 | 数据库权限变化后，后续文件操作必须生效；已有 handle 不能绕过写入和发布权限。 |
| FR-MNT-034 | P0 | 凭证不能写入命令参数、mount 元数据、日志或审计；使用数据库密码文件、环境变量、系统凭证或交互输入。 |
| FR-MNT-035 | P0 | 其他本地用户读取 mount point 不等于获得数据库权限；MVP 使用当前用户私有 mount point，不开放本地多用户共享。 |

#### 10.13.5 缓存、故障与恢复

| ID | 优先级 | 需求 |
|---|---|---|
| FR-MNT-036 | P0 | 正确性优先：NFS attribute/data cache 的 TTL、close-to-open、COMMIT/fsync 和主动失效行为必须可测；默认参数不能让刚写、rename、restore 或权限变化后的内容静默保持旧值。FSKit DataCacheHandler 留到后续版本。 |
| FR-MNT-037 | P0 | 数据库连接中断时，未确认提交的操作必须返回 I/O 错误，不能报告成功。 |
| FR-MNT-038 | P0 | gateway 崩溃后，数据库中只能存在完整旧版本、完整新版本或可识别的未发布暂存对象。 |
| FR-MNT-039 | P0 | 重新挂载后必须清理或恢复过期暂存对象，不能把半写文件发布给用户。 |
| FR-MNT-040 | P0 | stale handle、数据库重启、workspace freeze、配额不足和冲突必须映射为稳定 errno，并保留详细诊断。 |
| FR-MNT-041 | P0 | mount gateway 不能成为数据库备份的一部分；数据库原生备份必须独立包含全部已提交状态。 |
| FR-MNT-042 | P1 | 多个 gateway 同时访问时支持主动失效通知；在此能力完成前，不承诺跨 gateway 实时可见，只保证 close-to-open、冲突检测和不静默覆盖。 |
| FR-MNT-043 | P0 | 同一 handle 必须读到自己的 staged writes；其他已打开 handle 继续读取其打开时版本；成功 publish/synchronize 后的新 open 读取新版本。 |
| FR-MNT-044 | P0 | NFS close/COMMIT/fsync 或 gateway 回收 handle 时如果仍有 dirty staging，可以执行可恢复的兜底发布，但失败不能删除 staging、伪造成功或产生半版本。 |

### 10.14 多数据库适配

| ID | 优先级 | 需求 |
|---|---|---|
| FR-DB-001 | P0 | 已进入支持范围的数据库必须共享路径、版本、快照、changeset 和错误分类合同；不要求三个适配器同时发布。 |
| FR-DB-002 | P0 | 三端不得共享或模拟数据库内部实现。 |
| FR-DB-003 | P0 | 每个适配器必须使用宿主 allocator、事务和存储 API。 |
| FR-DB-004 | P0 | 适配器不得直接读写数据库物理文件。 |
| FR-DB-005 | P0 | 宿主不允许时，不得创建线程、后台任务或外部文件。 |
| FR-DB-006 | P0 | PostgreSQL 必须使用 role、MVCC、WAL 和数据库锁能力。 |
| FR-DB-007 | P0 | SQLite 必须正确处理单 writer、busy、savepoint、WAL/reopen 和 Backup API。mount staging 的热点元数据必须与内容块分开；正式发版前的旧实验 schema 明确拒绝，不保留迁移代码。 |
| FR-DB-008 | P1 | DuckDB 必须正确处理单进程写入、乐观冲突、checkpoint 和 reopen；默认先支持 SQL/CLI/worktree，可写 mount 只允许 gateway 成为唯一写进程或宿主提供可验证的多进程写入能力。 |
| FR-DB-009 | P1 | 同一组合同 spec 必须在所有已支持引擎运行。 |

## 11. CLI 命令范围

### 11.1 直接命令

| 分类 | 命令 | P0/P1 |
|---|---|---|
| 安装诊断 | setup、doctor、version | P0 |
| 挂载 | mount、unmount、mount list、mount status | P0 |
| 工作区 | workspace create/list/stat/freeze/unfreeze/drop/use | P0 |
| 浏览 | ls、tree、find | P0 |
| 读取 | cat、head、tail、stat、file | P0 |
| 查找统计 | grep、rg、wc、du | P0 |
| 修改 | touch、mkdir、put、get、cp、mv、rm、rmdir、truncate | P0 |
| 链接 | ln -s、readlink | P1 |
| 权限 | acl、chmod、chown | P1 |
| 版本 | history、show、diff、restore | P0 |
| 快照 | snapshot create/list/drop、restore | P0 |
| 运维 | check、repair plan、gc、quota、retention | P1 |
| 迁移 | export、import | P1 |
| worktree | create、status、diff、refresh、commit、discard、list | P1 |
| 运行 | shell、exec、txn run | P1 |
| 机器输出 | `--json`、`--no-color`、稳定退出码 | P1 |

### 11.2 命令行为

- 首选路径是 `vexfs mount ~/VexFS/<workspace>`，挂载后直接使用系统 `cd/ls/rg/sed/python/git`；
- `vexfs ls` 等直接命令用于脚本、诊断和不能挂载的环境，不覆盖系统命令；
- `vexfs shell` 是不能挂载时的 worktree 回退入口；
- 直接 `grep/rg` 是普通文本查找，不是语义搜索；
- `vexdb fs find` 是数据库批量元数据查询，不依赖先挂载目录；第一版支持名称 glob、类型、
  大小、修改时间、稳定路径游标和分页，单页最多 1000 项；
- 名称 glob 的 `*` 匹配任意数量字符，`?` 匹配一个 Unicode 字符；SQLite 与 PostgreSQL
  必须保持一致；
- PostgreSQL `find` 不能进入调用者无读取权限的目录，也不能返回无读取权限的对象；
- 二进制文件默认不作为文本输出，除非用户显式指定；
- 直接命令的路径表示 VexFS workspace 路径；
- mount point 内的路径表示数据库 workspace 的实时视图；
- worktree 内的路径表示本地普通文件路径；
- CLI 帮助必须清楚说明当前命令操作 mount、数据库路径还是本地 worktree。

### 11.3 直接命令与 SQL 映射

| CLI | SQL 合同 |
|---|---|
| ls/tree | vexfs_list / vexfs_walk |
| find | vexfs_find |
| cat/head/tail/get | vexfs_read / vexfs_read_range |
| grep/rg | vexfs_grep |
| stat/file/du/wc | vexfs_stat / vexfs_usage |
| touch/mkdir/put | vexfs_create / vexfs_mkdir / vexfs_write |
| cp/mv/rm | vexfs_copy / vexfs_move / vexfs_remove |
| history/show | vexfs_history / vexfs_read_version |
| diff | 读取两个版本后由 CLI 比较；workspace commit diff 后续使用 vexfs_diff |
| restore | vexfs_restore_version；snapshot 恢复后续使用 vexfs_restore_* |
| acl/chmod/chown | vexfs_grant / vexfs_revoke / vexfs_owner_set |
| mount 文件操作 | vexfs_handle_* / vexfs_read_range / vexfs_changeset_* |
| worktree commit | vexfs_changeset_* |
| check/gc | vexfs_check / vexfs_gc |
| export/import | vexfs_export_* / vexfs_import_* |

## 12. SQL 合同范围

以下是产品级函数族，不在本文锁定每个数据库的最终声明语法。

### 12.1 初始化和能力

```sql
SELECT vexfs_init();
SELECT * FROM vexfs_version();
SELECT * FROM vexfs_capabilities();
```

### 12.2 工作区

```sql
SELECT vexfs_workspace_create('workspace');
SELECT * FROM vexfs_workspace_list();
SELECT * FROM vexfs_workspace_stat('workspace');
SELECT vexfs_workspace_freeze('workspace');
SELECT vexfs_workspace_drop('workspace', false);
```

### 12.3 文件和目录

```sql
SELECT vexfs_mkdir('workspace', '/reports', true);
SELECT vexfs_write('workspace', '/reports/a.md', :content);
SELECT vexfs_write_range('workspace', '/reports/a.md', 100, :content);
SELECT vexfs_read('workspace', '/reports/a.md');
SELECT vexfs_read_range('workspace', '/reports/a.md', 0, 4096);
SELECT vexfs_copy('workspace', '/reports/a.md', '/archive/a.md');
SELECT vexfs_move('workspace', '/reports/a.md', '/reports/final.md');
SELECT vexfs_remove('workspace', '/archive', true);
SELECT * FROM vexfs_list('workspace', '/reports', 1000, :cursor);
SELECT * FROM vexfs_stat('workspace', '/reports/final.md');
```

### 12.4 版本和快照

SQLite `0.9.0` 当前已经实现文件版本和 workspace 快照：

```sql
SELECT vexfs_history('workspace', '/reports/final.md');
SELECT vexfs_history('workspace', '/reports/final.md', 100, :before_version);
SELECT vexfs_read_version('workspace', '/reports/final.md', 2);
SELECT vexfs_compare_versions('workspace', '/reports/final.md', 2, 3);
SELECT vexfs_restore_version('workspace', '/reports/final.md', 2, :expected_version);
```

四参数 `vexfs_history` 返回 `{"entries": [...], "next_before": ...}`，按版本倒序；每页最多 1000 条，CLI 默认 100 条。两参数形式为兼容入口，返回 JSON 数组并固定最多 100 条。每项包含 `version`、`commit`、`parent_commit`、`size`、`checksum`、`created_at`、`message` 和 `current`。`checksum` 是小写 SHA-256；文件和符号链接的 `vexfs_stat` 也返回当前版本的 `checksum`，目录返回 `null`。

恢复成功返回新版本号。恢复必须使用带 `expected_version` 的四参数接口，不能省略并发检查。
恢复版本只引用同 inode 目标 canonical 版本，历史中仍保留一条新的 version 和 commit；普通
写入可以复用该 inode 上一版本未变化的物理块，但当前不做跨 inode、跨文件的通用去重。

workspace 快照当前公开合同：

```sql
SELECT vexfs_snapshot_create('workspace', 'before-change');
SELECT * FROM vexfs_snapshot_list('workspace');
SELECT vexfs_snapshot_diff('workspace', 'before-change', 'HEAD');
SELECT vexfs_snapshot_restore('workspace', 'before-change', :expected_head);
```

### 12.5 权限和审计

```sql
SELECT vexfs_grant('workspace', '/reports', 'report_writer', 'read,write');
SELECT vexfs_revoke('workspace', '/reports', 'report_writer', 'write');
SELECT vexfs_owner_set('workspace', '/reports', 'report_admin');
SELECT * FROM vexfs_audit('workspace', :since_commit, :limit, :cursor);
```

### 12.6 Changeset

```sql
BEGIN;

SELECT vexfs_changeset_open('workspace', 847, :request_id);
SELECT vexfs_changeset_put('/src/main.cpp', :content, :expected_version);
SELECT vexfs_changeset_move('/src/old.cpp', '/src/new.cpp', :expected_version);
SELECT vexfs_changeset_remove('/tmp/output', :expected_version, true);
SELECT vexfs_changeset_apply('update generated files', :external_task_id);

COMMIT;
```

大内容必须允许分块暂存，不能要求一个 SQL 参数携带完整工作区。

### 12.7 检查、配额和迁移

```sql
SELECT vexfs_check('workspace', 1); -- 深度检查，包含逐块和整文件 SHA-256
SELECT vexfs_check('workspace', 0); -- 快速检查，只检查结构、引用、顺序和块长度
SELECT * FROM vexfs_repair_plan('workspace');
SELECT vexfs_gc('workspace', 1000);
SELECT vexfs_quota_set('workspace', :max_bytes, :max_files, :max_file_bytes);
SELECT vexfs_retention_set('workspace', :keep_versions, :keep_days);
SELECT * FROM vexfs_export_begin('workspace', 'snapshot-name');
SELECT vexfs_import_begin('new-workspace', :format_version);
```

SQLite `0.9.0` 已实现只读的 `vexfs_check`。它返回 JSON，检查 SQLite 自身、workspace root、
inode、dentry、可达性、commit 父链、snapshot、当前和历史版本引用、版本别名、manifest/chunk、
staging 和 handle。深度模式额外按 64 KiB 流式计算 SHA-256，不把全部历史内容载入内存。
每个问题包含稳定的 `code`、对象、类型、说明和建议动作，最多返回 1000 个问题并报告完整
问题数。当前内容模型是 `chunked-v1`；quick 检查 manifest、块数量、顺序、大小和引用，deep
再逐块验证 SHA-256 以及整文件 SHA-256。

对应 CLI 是 `vexdb fs check`，默认深度检查；`--quick` 跳过内容哈希，`--json` 输出原始
JSON。发现损坏时仍输出报告并返回退出码 8。当前没有自动 repair；检查不能修改或填补内容。

SQLite `0.9.0` 同时公开：

```sql
SELECT vexfs_quota_get('workspace');
SELECT vexfs_quota_set('workspace', :max_bytes, :max_files, :max_file_bytes);
SELECT vexfs_retention_get('workspace');
SELECT vexfs_retention_set('workspace', :keep_versions, :keep_days);
SELECT vexfs_gc_pause('workspace', 1);
SELECT vexfs_gc('workspace', 1000);
```

配额参数使用 SQL `NULL` 表示不限额，`0` 表示真实的零配额。降低配额后可以继续做减少用量的
操作，但不能增加超额用量。GC 不自动运行；每次最多删除调用者给定的批量，并返回是否还有可
回收版本。CLI 对应 `quota show/set`、`retention show/set`、`gc pause/resume` 和 `gc --batch`。

## 13. 权限规则

### 13.1 两层权限

有效权限取以下两层的交集：

1. 数据库是否允许当前 principal 调用 VexFS 公开函数；
2. VexFS ACL 是否允许 principal 操作目标 workspace/path。

VexFS ACL 不能扩大数据库拒绝的权限。

### 13.2 ACL 继承

- 工作区根必须有 owner ACL；
- 子对象默认继承最近祖先 ACL；
- 显式 deny 优先于 allow；
- owner 变更需要 admin；
- ACL 变更需要 share 或 admin；
- 恢复旧版本不能恢复旧 ACL 来绕过当前权限；
- 删除和改名必须检查父目录权限。

### 13.3 三端身份

| 数据库 | principal 来源 | 权限等级 |
|---|---|---|
| PostgreSQL | 当前数据库 role | 完整多用户权限 |
| DuckDB | 当前连接和宿主可证明身份 | 应用内权限，不宣传为服务器级身份 |
| SQLite | 连接、authorizer 或宿主注入的受信身份 | 应用内权限，不宣传为服务器级身份 |

CLI 不能接受 `--as-user` 之类参数绕过数据库身份。

## 14. Worktree 详细流程

### 14.1 创建

1. CLI 使用标准数据库连接认证。
2. 查询 workspace head commit。
3. 检查目标路径的 traverse/list/read 权限。
4. 创建权限受限的临时目录。
5. 流式导出允许读取的普通文件和安全链接。
6. 保存 base commit、inode、version、size 和 checksum 清单。
7. 关闭读取事务和数据库锁。

worktree 清单不保存凭证，也不能被数据库扩展信任为权限依据。

### 14.2 工作

模型可以使用运行环境中已有的任何普通文件工具。VexFS 只设置工作目录，不承诺提供 OS 进程沙箱。禁止进程访问主机其他路径属于 Agent 沙箱或容器职责。

### 14.3 检测差异

必须识别：

- 新文件和目录；
- 内容修改；
- 元数据修改；
- 删除；
- 可证明的改名；
- 文件、目录、链接之间的类型变化；
- 不支持的特殊文件。

### 14.4 提交

1. 重新认证数据库身份。
2. 读取当前 workspace head。
3. 校验 base commit 和 expected version。
4. 校验路径和链接安全。
5. 重新检查所有读取、创建、写入、删除、改名和管理权限。
6. 计算新增空间并检查配额。
7. 把差异写入事务级 changeset。
8. 一次发布文件版本、目录版本、workspace commit 和审计记录。
9. 数据库 commit 成功后才更新本地 worktree 基线。

### 14.5 冲突和失败

- 权限失败：不提交，保留本地差异；
- 配额失败：不提交，返回超限对象和所需空间；
- 并发冲突：不覆盖，返回冲突路径、base version 和 current version；
- 网络中断：数据库事务回滚，本地 worktree 保留；
- 命令退出非零：`vexfs exec` 默认不提交；
- 数据库提交成功但本地状态更新失败：下次 status 必须能从数据库 commit 恢复基线。

### 14.6 清理

- discard 只能删除带有效 VexFS worktree 标记的目录；
- 非空且有未提交变更时，需要显式 `--force`；
- 过期 worktree 只提示，不自动删除用户未提交内容；
- 自动清理不得删除普通目录；
- worktree 不是备份，长期保存需要 commit、snapshot 或 export。

## 15. 数据保护与备份

### 15.1 四层保护

| 层次 | 解决问题 | 不解决问题 |
|---|---|---|
| 文件版本 | 单文件误改、查看历史 | 整个数据库丢失 |
| 工作区快照 | 批量误操作、目录恢复 | 数据库介质损坏 |
| 数据库原生备份 | 灾难恢复、业务数据一致性 | 跨数据库格式迁移 |
| 逻辑导出 | 单工作区离线保存和迁移 | 数据库全量灾难恢复 |

### 15.2 PostgreSQL

- pg_dump、物理备份和恢复测试必须包含 VexFS 内部对象和内容；
- extension 升级与 restore 顺序必须有文档；
- 权限和 ACL 需要验证恢复结果；
- VexFS 不调用或调度 pg_dump、base backup 或复制。

当前 PostgreSQL `0.4.0-alpha.1` 已实现文件版本、workspace commit、snapshot、
expected-head/version、chunk/manifest、SHA-256、quick/deep check、quota、retention、
分批 GC、ACL、完整变更审计、format v2 和 libpq 远程 mount。`pg_dump/pg_restore` 与
`pg_basebackup` 已通过恢复测试，恢复后文件、历史、快照、设置、权限、审计和业务表保持一致。
16 MiB 有界性能 Gate 已验证克隆可启动、`pg_verifybackup`、format v2 导入导出、CLI
峰值 RSS、容器 `memory.max=1 GiB`，且运行前后 `oom_kill=0`。当前审计实现下，逻辑
dump/restore 为 173.913/175.824 MiB/s，format v2 导出/导入为 72.727/106.667 MiB/s；
这些是 2026-07-24 本机有界样本，不是所有机器的固定承诺。

### 15.3 SQLite

- 使用 SQLite Online Backup API 或安全数据库备份方式；
- 禁止在活跃写入时直接复制 `.db` 而忽略 `-wal`；
- 恢复测试必须覆盖 WAL、busy、crash/reopen；
- 备份后必须运行 VexFS check。

### 15.4 DuckDB

- 使用 DuckDB 支持的数据库复制、导出或安全文件备份流程；
- 备份时遵守 DuckDB 连接、锁和 checkpoint 规则；
- 恢复后验证 catalog、内容和 checksum；
- 不把普通文件复制包装成无条件安全的在线备份。

### 15.5 逻辑导出格式

SQLite `0.9.0` 已实现 `.vexfs` format v2。它使用独立 SQLite 文件作为逻辑记录容器，
不复制源数据库的业务表、连接信息、密码、WAL 或内部运行句柄。当前命令：

```bash
vexdb fs export --output workspace.vexfs
vexdb fs export --snapshot before-change --output workspace.vexfs
vexdb fs archive verify workspace.vexfs
vexdb fs --db target.sqlite3 --workspace restored import workspace.vexfs
```

逻辑包包含：

- format version；
- workspace metadata；
- source engine 和 source commit；
- inode、dentry、file version、manifest 和 chunk；
- snapshot；
- ACL principal 映射表；
- 每条记录 checksum；
- 整包 checksum；
- 导出完成标记。

format v2 导出选择 HEAD 或一个固定快照，并保留该 commit 祖先链内仍可恢复的文件版本和
workspace 历史。canonical 内容保持 manifest + 64 KiB chunk，恢复别名保持对同 inode canonical 版本的引用。
记录 hash 覆盖记录全部字段，内容 hash 复用文件版本 SHA-256，整包 hash 覆盖 manifest 和
全部有序记录 hash。校验逐个读取不超过 64 KiB 的 chunk，不把整个工作区载入内存。

导入目标 workspace 必须不存在。导入先创建同一事务内不可见的 `importing` workspace，映射
commit/inode/manifest ID，重建 chunk 引用、当前树和历史，运行深度 `vexfs_check`，成功后才切换成 `active` 并提交。
任何格式、结构、记录、内容、整包 hash 或深度检查失败都会回滚，目标名称不会留下半成品。
SQLite 到 SQLite 采用 principal 原值映射；PostgreSQL 导入把目标 owner 绑定到真实数据库
role，并保留可移植 ACL 信息，不能自动扩大权限。DuckDB 后续也必须在 format v2 principal
表之上实现明确的目标身份映射。

## 16. 三端能力矩阵

| 能力 | PostgreSQL | SQLite | DuckDB |
|---|---|---|---|
| 扩展形态 | CREATE/ALTER EXTENSION | loadable/static extension | loadable extension |
| 主要场景 | 共享、多 Agent、企业服务 | 端侧、单文件、离线 | 本地分析、单进程工作区 |
| 用户身份 | 完整 role | 连接/authorizer | 连接/宿主边界 |
| 多写者 | 支持 | 单 writer | 主要是单进程内 |
| 冲突方式 | 锁、MVCC、约束 | busy、单 writer | 乐观冲突 |
| WAL/恢复 | PostgreSQL 管理 | SQLite 管理 | DuckDB 管理 |
| 原生备份 | PG 工具 | Online Backup API | DB copy/export/安全文件流程 |
| 路径 ACL | 已实现 | 应用内 | 应用内 |
| P0 合同 | 已实现 | Phase 1 收口 | Phase 4 实现 |
| 可写 macOS mount | 已实现 | 已有 SQLite 预览 | Phase 4 后按单写者条件决定 |
| 可写 Linux mount | 已实现 | 已有 libfuse3 预览 | Phase 4 后按单写者条件决定 |
| 可写 Windows mount | Phase 3 | Phase 3 | Phase 4 后按单写者条件决定 |

版本约束：

- PostgreSQL：16、17、18、19；
- DuckDB：仓库当前约束 v1.5.2；
- SQLite：不低于 3.24.0，并由构建和发布矩阵固定具体版本。

## 17. 核心数据模型要求

### 17.1 工作区

必须保存 workspace id、名称、根 inode、head commit、owner、冻结状态、配额、保留规则和时间。

### 17.2 inode 版本

必须保存 inode id、version id、有效 commit 范围、类型、mode、owner、size、manifest、checksum、删除标记和时间。

### 17.3 目录项版本

必须保存 workspace、parent inode、原始名称、child inode、有效 commit 范围和 tombstone。

### 17.4 文件内容

必须使用不可变 manifest 和 chunk；manifest 发布后不能原地修改；chunk 必须带 checksum。

### 17.5 快照

快照只保存名称、workspace、commit、创建人和创建时间。

### 17.6 ACL 与审计

ACL 必须绑定 workspace/inode/principal；审计必须绑定 workspace/commit/sequence/principal/operation/before/after。

## 18. 非功能需求

### 18.1 正确性

| ID | 要求 |
|---|---|
| NFR-COR-001 | 任何写入只能出现完整旧状态或完整新状态。 |
| NFR-COR-002 | 三端对相同操作序列返回相同目录树、内容 hash、版本结果和错误分类。 |
| NFR-COR-003 | 每个内部写步骤必须支持故障注入测试。 |
| NFR-COR-004 | 数据库 crash/reopen 后，已提交文件完整，未提交修改不可见。 |
| NFR-COR-005 | worktree changeset 不能产生半个目录项、半个 manifest 或缺块文件。 |
| NFR-COR-006 | mount 的每个成功操作必须符合文档中的 POSIX 子集和数据库提交语义。 |
| NFR-COR-007 | mount adapter 崩溃、进程终止或断网后，已报告 publish/sync 成功的文件版本必须存在；只报告 write 成功的内容至少保留为可识别的 staging。电源故障发生在 SQLite NORMAL staging 阶段时允许丢失最近未发布内容，但不能损坏最后一次 FULL publish 的版本。 |

### 18.2 可靠性

| ID | 要求 |
|---|---|
| NFR-REL-001 | 提交结果不确定时，CLI 必须通过 request id 或 commit 查询确认，不能盲目重试。 |
| NFR-REL-002 | 数据库连接中断不能导致本地未提交内容丢失。 |
| NFR-REL-003 | GC、check、export 和 backup verify 必须可中断并安全重试。 |
| NFR-REL-004 | 恢复和 repair 也必须生成版本和审计记录。 |
| NFR-REL-005 | mount gateway 可重启、可重新挂载，不能成为已提交数据恢复的前置条件。 |
| NFR-REL-006 | 数据库重启后 gateway 必须明确断开旧 handle，并在重新连接后恢复新操作。 |

### 18.3 安全

| ID | 要求 |
|---|---|
| NFR-SEC-001 | 所有外部输入按不可信处理，包括路径、导入包、BLOB、正则模式和 worktree。 |
| NFR-SEC-002 | 路径规范化后必须再次检查工作区边界。 |
| NFR-SEC-003 | worktree 使用当前用户私有目录，默认权限不得允许其他本地用户读取。 |
| NFR-SEC-004 | 密码不得出现在命令参数、日志、审计或 worktree 元数据中。 |
| NFR-SEC-005 | 传输加密使用数据库连接能力，静态加密使用数据库或磁盘能力。 |
| NFR-SEC-006 | VexFS 不承诺提供进程级沙箱；文档必须明确这一点。 |
| NFR-SEC-007 | 正则、递归目录和导入大小必须有限制，避免资源耗尽。 |
| NFR-SEC-008 | mount point 默认只允许当前操作系统用户访问，一个 mount 只能代表一个数据库 principal。 |
| NFR-SEC-009 | gateway 必须以普通用户运行；安装流程不能要求长期 root 进程。 |

### 18.4 资源和规模

MVP Gate 验收数据集：

- 1,000 个普通文件；
- 总当前内容 1 GiB；
- 单文件最大 100 MiB；
- 单目录最大 1,000 个直接子项；
- 100 个 workspace commit；
- 10 个快照。

完整 P0 产品验收数据集：

- 10 万个普通文件；
- 总当前内容 10 GiB；
- 单文件最大 100 MiB；
- 单目录最大 1 万个直接子项；
- 1,000 个 workspace commit；
- 100 个快照。

| ID | 要求 |
|---|---|
| NFR-RES-001 | 扩展使用宿主 allocator，不使用不受管长期内存。 |
| NFR-RES-002 | 大内容读写和导出必须流式或分块。 |
| NFR-RES-003 | list、history 和 audit 必须分页。 |
| NFR-RES-004 | GC 必须分批并可配置最大处理量。 |
| NFR-RES-005 | worktree 必须支持空间上限和最小导出范围。 |
| NFR-RES-006 | 版本、快照和历史空间必须可观测。 |

### 18.5 初始性能目标

以下是完整 P0 的本地参考环境目标，不是公开 SLA。参考环境为 8 核 CPU、16 GiB 内存、NVMe、本机数据库连接。MVP Gate 只要求正确完成 1,000 文件和 1 GiB 数据集，并记录每个调用的基线；在语义稳定前不为原型设公开性能承诺。

| 操作 | 目标 |
|---|---|
| stat 单文件 SQL P95 | 小于 50 ms |
| list 1,000 项首屏 P95 | 小于 300 ms |
| 读取 64 KiB 小文件 SQL P95 | 小于 100 ms |
| 写入 64 KiB 小文件 SQL P95 | 小于 200 ms |
| CLI 单次只读命令 P95 | 小于 300 ms |
| mount stat 单文件 P95 | 小于 100 ms |
| mount 读取 64 KiB 小文件 P95 | 小于 200 ms |
| mount 写入并 close 64 KiB 小文件 P95 | 小于 400 ms |
| mount 顺序读取 100 MiB | 不低于参考环境本地数据库 SQL 流式读取吞吐的 60% |
| 10 万文件 worktree status | 小于 30 秒 |
| 创建快照 | 不随工作区总内容线性复制 |

2026-07-21 当前参考机实测：SQLite 直连创建 10 万文件为 123.327 秒；`preview.13`
FSKit 创建 1 万文件为 114.828 秒，遍历为 0.462 秒，`rg` 为 23.895 秒，快照恢复为
21.465 秒。这些是当前基线，不是公开 SLA。100,000 文件真实 mount 已完成创建、遍历
和抽样读取，但 `rg` 在 3,600 秒内未完成，快照/恢复后半段尚未验收。

### 18.6 可运维性

- 提供扩展版本、能力、schema 版本和初始化状态；
- 提供 mount 列表、连接状态、principal、workspace、缓存和未发布 handle 数量；
- 提供 workspace usage、quota、retention 和 GC 状态；
- 提供只读 check 和 backup verify；
- 错误消息使用通俗语言，并保留底层数据库诊断；
- 日志不能包含密码或文件正文；
- 升级、备份、恢复、迁移和故障处理必须有独立文档。

### 18.7 兼容性

- macOS 26.0+、Apple Silicon 是 mount gateway 的首个验证平台；
- MVP 使用当前稳定 Xcode SDK 中可用的 FSKit API，并通过薄适配层隔离后续 `Operations` 到 `Handler` 的迁移；
- macOS 技术预览需要签名、公证，并提供 FSKit extension 启用检查；
- CLI 首轮支持 macOS；Linux 和 Windows 在 mount adapter 完成前使用直接命令或 worktree；
- PostgreSQL 和 DuckDB 数据库扩展仍按各自服务器支持平台构建，不因 macOS gateway 改变宿主约束；
- 文件名按字节稳定比较，不依赖系统语言区域；
- UTF-8 文本命令提供稳定行为，二进制内容按字节保存；
- 路径分隔符在 VexFS 合同中统一使用 `/`；
- worktree 在宿主平台映射文件名时必须发现并拒绝无法安全表示的冲突。

## 19. 错误与退出码

### 19.1 统一错误分类

```text
VEXFS_NOT_FOUND
VEXFS_ALREADY_EXISTS
VEXFS_NOT_DIRECTORY
VEXFS_IS_DIRECTORY
VEXFS_NOT_EMPTY
VEXFS_PERMISSION_DENIED
VEXFS_READ_ONLY
VEXFS_QUOTA_EXCEEDED
VEXFS_CONFLICT
VEXFS_BUSY
VEXFS_NO_SPACE
VEXFS_CORRUPTION
VEXFS_INVALID_PATH
VEXFS_UNSAFE_LINK
VEXFS_UNSUPPORTED
VEXFS_VERSION_MISMATCH
VEXFS_BACKUP_INCOMPLETE
VEXFS_UNAVAILABLE
VEXFS_TIMEOUT
VEXFS_STALE_HANDLE
```

### 19.2 Mount POSIX errno 映射

首个 FSKit 原型开始前必须冻结以下基础映射；FSKit adapter 使用 `POSIXError` 或等价结果返回，底层数据库诊断写入日志和 `doctor`，不能把不同数据库错误直接泄露成不同 errno。

| VexFS 错误 | errno | 说明 |
|---|---:|---|
| `VEXFS_NOT_FOUND` | `ENOENT` | 文件、目录或 workspace 不存在 |
| `VEXFS_ALREADY_EXISTS` | `EEXIST` | O_EXCL 或同名创建冲突 |
| `VEXFS_NOT_DIRECTORY` | `ENOTDIR` | 路径中间项不是目录 |
| `VEXFS_IS_DIRECTORY` | `EISDIR` | 对目录执行普通文件操作 |
| `VEXFS_NOT_EMPTY` | `ENOTEMPTY` | 删除非空目录 |
| `VEXFS_PERMISSION_DENIED` | `EACCES` | 数据库 principal 无权操作 |
| `VEXFS_READ_ONLY` | `EROFS` | workspace 或数据库只读 |
| `VEXFS_QUOTA_EXCEEDED` | `EDQUOT` | workspace 配额不足 |
| `VEXFS_NO_SPACE` | `ENOSPC` | 数据库或宿主空间不足 |
| `VEXFS_CONFLICT` | `EAGAIN` | expected version 已变化，可重新打开后重试 |
| `VEXFS_BUSY` | `EBUSY` | SQLite busy、workspace freeze 或资源正被占用 |
| `VEXFS_CORRUPTION` | `EIO` | 校验失败或内部状态损坏 |
| `VEXFS_INVALID_PATH` | `EINVAL` | 路径或参数不合法 |
| `VEXFS_UNSAFE_LINK` | `ELOOP` | 链接循环或越界 |
| `VEXFS_UNSUPPORTED` | `ENOTSUP` | 当前阶段不支持的调用 |
| `VEXFS_VERSION_MISMATCH` | `EPROTO` | gateway 与扩展合同版本不兼容 |
| `VEXFS_UNAVAILABLE` | `EIO` | 数据库连接已断开 |
| `VEXFS_TIMEOUT` | `ETIMEDOUT` | 数据库调用超时 |
| `VEXFS_STALE_HANDLE` | `ESTALE` | handle 已过期或数据库重启后失效 |

chmod/chown 等已明确不支持的权限修改返回 `EPERM`；跨 workspace rename 在尚未支持时返回 `EXDEV`。同一个错误在重试前后必须保持稳定，不能根据 PostgreSQL、SQLite 或 DuckDB 的原始错误文案变化。

### 19.3 CLI 退出码

| 退出码 | 含义 |
|---|---|
| 0 | 命令和数据库提交成功 |
| 1 | 普通命令失败 |
| 2 | 参数或路径格式错误 |
| 3 | 文件或工作区不存在 |
| 4 | 权限不足 |
| 5 | 并发冲突 |
| 6 | 配额或空间不足 |
| 7 | 数据库连接、busy 或超时 |
| 8 | 数据损坏或校验失败 |
| 9 | 不支持的文件类型或能力 |
| 10 | 外部命令退出非零，未提交 |

`--json` 输出必须同时提供稳定错误 code、通俗 message、可选 path 和底层 detail。

## 20. 验收标准

### 20.1 MVP Gate 验收

| ID | 验收场景 |
|---|---|
| AC-MVP-001 | 在干净 Mac 安装默认 NFS 发行物后，不启用文件系统扩展，`vexdb fs doctor` 通过，一条 `vexdb fs mount` 命令得到可 `cd` 的真实目录。最低系统和 CPU 范围由发行真机矩阵确定。 |
| AC-MVP-002 | `ls/cat/cp/mv/rm/mkdir/find/grep/sed`、Python 读写和基础 Git `init/add/commit/checkout` 直接运行。 |
| AC-MVP-003 | macOS NFS mount 的 lookup/getattr/readdir/open/close/create/read/write/remove/rename/setattr/fsync/COMMIT 和 statfs 通过核心合同与真实 Bash 测试。 |
| AC-MVP-004 | 同一 handle 读到自己的未发布写入；其他 handle 看到打开时版本；成功 publish/synchronize 后的新 open 看到新版本。 |
| AC-MVP-005 | `synchronize` 重复调用不产生重复 commit；close/reclaim 失败不能丢弃 dirty staging 或伪造提交成功。 |
| AC-MVP-006 | 强 sync 成功后终止 NFS gateway、重启 SQLite 并重新挂载，内容和 checksum 保持正确；未 sync 窗口的行为有明确测试证据。 |
| AC-MVP-007 | SQLite busy、连接中断、空间不足、版本冲突和 stale handle 返回冻结后的稳定 errno。 |
| AC-MVP-008 | NFS gateway 只监听 loopback，mount point 为当前用户私有；其他本地用户和局域网主机不能访问 export。 |
| AC-MVP-009 | 1,000 个文件、1 GiB 当前内容、100 MiB 单文件完成写入、重挂载、读取和校验，没有半文件或丢失目录项。 |
| AC-MVP-010 | gateway 与扩展合同版本不兼容时拒绝可写挂载；卸载 gateway 后，SQL 仍可读写全部已提交文件。 |

当前验收已经额外覆盖 `chmod` 的 `0000..0777` mode、可执行脚本、symlink、readlink、xattr、hardlink、owner/group 元数据和便携 ACL；Linux 已覆盖 chown 和 hardlink 的 mount 映射。仍不验收多 gateway、完整 ACL 授权、跨 gateway flock、共享可写 mmap、10 万文件和 DuckDB mount。

2026-07-24 默认 NFS 已通过 35 项真机回归和 30 项 package smoke，包含 hardlink 与 fsync；
正式 0.1 仍需补干净 Mac 安装、公证包、gateway crash/sleep-wake、锁行为和长期负载 Gate。

### 20.2 P0 产品验收

| ID | 验收场景 |
|---|---|
| AC-P0-001 | 业务表更新和文件写入在同一事务中同时 commit。 |
| AC-P0-002 | 事务 rollback 后业务表和文件都保持旧状态。 |
| AC-P0-003 | 两个 Agent 修改同一文件时，后提交者得到冲突且本地差异保留。 |
| AC-P0-004 | 无 read 权限的用户不能通过 SQL、CLI、checkout 或历史读取内容。 |
| AC-P0-005 | 无 write/delete/rename 权限的 changeset 整体失败。 |
| AC-P0-006 | macOS 用户挂载 workspace 后，可以直接运行 ls、cat、rg、sed、python、tar、git、编译器和 apply_patch。 |
| AC-P0-007 | create、write、rename、unlink、truncate、symlink、flock、fsync 和 open-unlink 行为通过 POSIX 子集测试。 |
| AC-P0-008 | 快照后批量修改，恢复单文件、目录和工作区均正确。 |
| AC-P0-009 | 数据库原生备份恢复后，业务表、文件、版本、快照、ACL 和审计一致。 |
| AC-P0-010 | crash 注入后只出现完整旧状态或完整新状态。 |
| AC-P0-011 | 缺失 chunk 时 check 报告损坏，read 不返回伪造内容。 |
| AC-P0-012 | 10 万文件和 10 GiB 数据集通过正确性与初始性能目标。 |
| AC-P0-013 | 完成一次 context 和 workspace 配置后，用户可以用一条 vexfs mount 命令得到真实目录并直接 cd 进入。 |
| AC-P0-014 | NFS gateway 被终止、数据库重启或连接中断后，已成功强 sync 的文件仍完整，未成功发布不被伪装成成功。 |
| AC-P0-015 | `vexdb fs doctor` 能准确报告 gateway 不存在或版本不兼容、NFS client 不可用、mount 权限被拒、stale mount、端口冲突、未初始化、连接失败和权限不足。 |
| AC-P0-016 | mount point 默认仅当前操作系统用户可访问，数据库 ACL 变化会影响后续文件操作。 |
| AC-P0-017 | 两个 mount 同时修改同一版本时至少一个得到冲突，不能静默覆盖。 |
| AC-P0-018 | 不安装或不运行 gateway 时，SQL 和 CLI 数据能力仍然可用。 |
| AC-P0-019 | 发行评测没有匹配用例、选中用例全部跳过或必跑用例出现跳过时，必须非零退出。 |
| AC-P0-020 | manifest、CLI、SQLite 扩展和 mount adapter 的合同版本、runtime ABI、源码 commit 与哈希必须来自同一次构建。 |
| AC-P0-021 | Developer ID 和公证包必须来自干净 commit；安装器拒绝脏源码正式包、错误 TeamIdentifier、哈希损坏和 ABI/合同不一致。 |
| AC-P0-022 | ad-hoc 包只能通过显式本地测试开关安装，不能伪装成用户可分发的正式包。 |

### 20.3 合同测试

- 路径规范化和非法路径；
- 同名创建和原子 replace；
- range read/write 和 truncate；
- 目录分页和稳定排序；
- savepoint 和 rollback-to；
- 同文件双写；
- move 与 remove 并发；
- workspace freeze 与写入；
- 版本、快照、恢复和 retention；
- ACL 继承、deny 和 owner；
- quota、GC 和快照保护；
- export/import checksum；
- mount lookup/getattr/readdir/open/read/write/close/reclaim；
- mount create/mkdir/rmdir/unlink/rename/truncate/publish/sync；
- O_EXCL、O_TRUNC、O_APPEND、open-unlink、symlink 和 flock；
- mount principal、ACL 变更、freeze、quota 和冲突；
- gateway kill、数据库重启、断网、重新挂载和暂存清理；
- worktree create/status/diff/commit/discard；
- 安全链接和越界链接；
- 数据库 backup/restore 和 crash/reopen。

### 20.4 数据库专项测试

#### SQLite

- transaction、savepoint、busy 和单 writer；
- WAL 模式 crash/reopen；
- Online Backup API；
- shadow/internal tables 保护；
- 扩展加载和 SQLite 最低版本检查。

#### PostgreSQL

- PG16、17、18、19；
- role、权限和内部 schema；
- 多连接并发和 deadlock；
- WAL/restart；
- pg_dump/pg_restore 和物理备份恢复；
- 两个真实 mount gateway 同时在线时，分别注入 helper 崩溃、网络中断和数据库停机；
- helper 异常撤销后底层 mountpoint 必须不可写，不能让普通用户产生未进入数据库的本地文件；
- 网络恢复不得隐式重放提交状态未知的写请求，必须先用幂等读操作确认连接与状态；
- extension 安装和升级。

#### DuckDB

- v1.5.2 加载；
- 单进程多连接和乐观冲突；
- checkpoint/reopen；
- 数据库 copy/export 恢复；
- catalog 和内部对象保护。

## 21. 发布阶段

发布阶段的详细范围、状态、完成条件和下一步只在路线图维护：

`docs/plans/2026-07-21_vexfs-final-goal-and-roadmap.md`

当前阶段摘要如下：

| 阶段 | 目标 | 状态 |
|---|---|---|
| Phase 0 | 统一合同、macOS + SQLite 垂直闭环、Linux 真实挂载验证 | 已完成 |
| Phase 1 | SQLite 本地开发者预览：macOS + Linux 可安装、可恢复、可跑真实项目 | 进行中 |
| Phase 2 | PostgreSQL 共享工作区：跨电脑、多用户、数据库权限和并发 | 代码与自动测试完成；当前源码 Developer ID 包已验证并安装，待重新授权后补跑真挂载 Gate |
| Phase 3 | Windows WinFsp 和 macOS/Linux/Windows 跨系统预览 | 未开始 |
| Phase 4 | DuckDB 本地分析工作区 Beta | 未开始 |
| Phase 5 | VexDB-Lite Files v1 稳定版 | 未开始 |

阶段不能只因代码合并或单个 demo 通过而完成。必须达到路线图中对应的干净机器安装、真机、一致性、故障、备份和规模验收条件。

## 22. 成功指标

### 22.1 产品通过条件

在继续投入完整三端 v1 前，必须满足：

1. 至少三个真实流程需要业务数据和文件原子提交；
2. 真实模型可以在 macOS FSKit 挂载目录中完成任务，不需要为工具逐个适配；
3. 并发修改不会静默丢失；
4. 权限、配额和命令失败不会产生部分提交；
5. 数据库原生备份恢复后状态一致；
6. 10 万文件规模下达到正确性和性能目标；
7. 历史空间可以通过 retention 和 GC 控制。

### 22.2 不适用判断

- 只有单机源码编辑需求：优先使用 Git 和普通文件系统；
- 主要保存超大视频、模型或数据湖对象：优先使用对象存储加数据库元数据；
- 不需要事务、共享、权限、版本或审计：VexFS 价值不足。

## 23. 主要风险与应对

| 风险 | 影响 | 应对 |
|---|---|---|
| 范围过大 | 三端长期无法交付 | 用 MVP Gate 隔离首个闭环；SQLite 技术验证，PG 产品验证，DuckDB 后接 |
| POSIX 范围无限扩大 | 无法交付稳定挂载 | MVP 只做普通文件和目录最小调用集；其余能力逐项用真实程序数据决定 |
| gateway 变成权威服务 | 数据和数据库事务失去控制 | 只调用公开数据库合同，不保存最终状态，SQL 独立可用 |
| open 到 close 长事务 | 锁、膨胀和连接耗尽 | handle staging 使用短事务；publish/synchronize 幂等发布，close/reclaim 只做可恢复兜底 |
| 多条 Bash 命令被误认为一个事务 | 多文件任务出现部分结果 | 明确每操作事务；多文件使用 changeset/txn run |
| 缓存返回旧权限或旧内容 | 越权和覆盖 | MVP 不启用 FSKit kernel data cache；后续缓存必须带版本校验和主动失效 |
| FSKit extension 未启用 | 用户无法挂载 | 首次启动引导到系统设置，doctor 显示准确状态，CLI/worktree 可回退 |
| FSKit API 快速变化 | 新 SDK 编译失败或行为变化 | 独立 macOS adapter；MVP 使用稳定 SDK API；新版 Handler 接口通过兼容层迁移 |
| 签名、entitlement 或分发路径错误 | App 能构建但 extension 无法安装或加载 | Phase 1 验证 Developer ID 或 App Store 路径；CI 检查签名、entitlement 和系统信任状态，并在干净 macOS 环境做安装回归 |
| gateway 崩溃留下暂存对象 | 空间泄露或半文件 | 未发布状态不可见、租约/句柄 id、重挂载清理、GC |
| 临时文件泄露 | 敏感内容暴露 | 私有目录、最小导出、无凭证、清理提示、可选加密盘 |
| 长任务并发冲突 | 模型工作无法直接提交 | 不长期锁库，提交时检测并保留本地差异 |
| 大文件放大 WAL 和备份 | 成本不可控 | 默认 100 MiB 单文件、10 GiB 工作区、分块和配额 |
| 历史无限增长 | 数据库膨胀 | retention、快照保护、分批 GC 和空间统计 |
| 三端权限不对等 | 宣传和行为不一致 | 统一权限合同，明确 PostgreSQL 完整、嵌入式应用内 |
| arbitrary exec 被误认为沙箱 | 主机安全风险 | 明确进程沙箱不在 VexFS 范围，交给 Agent 环境 |
| 数据库备份职责混淆 | 灾难恢复失败 | 四层保护模型和真实 restore 演练 |
| CLI 与 SQL 漂移 | 行为不一致 | CLI 合同测试必须对照 SQL 结果 |
| gateway 与扩展版本不兼容 | 写入协议错误或数据损坏 | 挂载前能力握手；不兼容时拒绝可写挂载并给出升级步骤 |
| DuckDB 多进程写入受限 | mount 与应用互相阻塞或失败 | DuckDB 先做 SQL/CLI/worktree；可写 mount 默认要求 gateway 是唯一写进程 |

## 24. 已确定决策

- 产品名暂定 VexFS；
- 产品属于 VexDB-Lite 仓库和品牌；
- VexFS 是数据库 extension；
- 数据库管理 VexFS；
- VexFS 只管理数据库中的文件；
- SQL 是完整权威合同；
- “安装后无缝 Bash”是第一产品目标；
- 产品 P0 与首个 MVP Gate 分开管理，P0 能力不要求全部进入 SQLite 首个闭环；
- mount gateway 是第一等入口，但不是权威核心；
- macOS 默认 mount 是系统 NFS client + 本机用户态 NFS gateway；
- FSKit 作为后续原生增强保留，不是 0.1 首次安装和发布前置；
- NFS gateway 只绑定 loopback，不作为远程共享协议；跨电脑共享仍由每台机器的本地 mount gateway 连接 PostgreSQL；
- 默认 NFS 的协议版本、锁、xattr、缓存、sleep/wake、mount 权限和 crash 恢复必须先通过真机 Gate；
- mount 采用 close-to-open 一致性，不承诺完整跨 gateway POSIX 强一致；
- 平台无关 publish/sync 是可报告的幂等发布边界；macOS 由 synchronize 映射，close/reclaim 只做可恢复兜底；
- 一个 mount 绑定一个数据库 principal，mount point 默认当前用户私有；
- mount 的单个文件操作分别使用短数据库事务；
- 多文件和业务 SQL 原子性通过显式 changeset/事务提供；
- CLI 是配套工具，不保存权威状态；
- 常用命令直接映射 SQL；
- 任意普通终端程序优先通过挂载目录运行；
- worktree 是不能挂载时的回退，不是权威存储或备份；
- macOS NFS 只作为 loopback mount adapter，不开放远程文件服务、不提供 SDK、不管理数据库；
- 通过 SQL 调用的文件修改参加调用者当前数据库事务；mount 操作使用 gateway 短事务；
- 权限来自数据库可证明身份；
- 使用文件版本、快照、数据库原生备份和逻辑导出四层保护；
- 数据库原生备份必须包含 VexFS；
- 技术原型已完成 macOS + SQLite，并已跑通 Linux + SQLite libfuse3 预览；
- 当前优先收口 macOS/Linux + SQLite 本地开发者预览，然后用 PostgreSQL 验证跨电脑共享；
- Windows 使用 WinFsp 接入同一 mount runtime；
- DuckDB 在产品价值验证后适配，默认先做 SQL/CLI/worktree，可写 mount 不与外部写进程并存；
- 不做内容检索层。

## 25. 技术评审前需要确定

这些问题不改变产品需求，但必须在对应阶段开始前确定：

1. VexFS 编入现有 `vexdb_lite` 扩展，还是同仓库独立扩展并随同发布；
2. SQLite 和 DuckDB principal 的宿主注入接口；
3. changeset 大内容分块暂存的三端 SQL 形式；
4. 逻辑导出容器格式和版本升级策略；
5. Phase 1 是否启用 FSKit DataCacheHandler、只读缓存、主动失效和 mmap；当前正确性基线不启用 kernel data cache；
6. PostgreSQL 内容存储使用普通 BLOB/TOAST、Large Object 或分块表的最终选择；
7. 首轮性能参考机和自动化基准环境；
8. hardlink 在 macOS、Linux、Windows 上的最终降级和跨系统导出规则；
9. mount gateway 使用独立连接池还是每 handle 短连接，以及连接上限；
10. SQLite P0 暂存对象和 crash 清理的具体表结构；
11. FSKit `Operations` 到新版 `Handler` API 的兼容层和最低 Xcode 版本；
12. dirty staging 的保留时间、手动恢复命令和自动回收门槛；
13. SQLite gateway 每次 write 的最大批量、连接 busy_timeout 和背压上限；
14. PostgreSQL 多 gateway 场景采用通知、轮询还是租约来做缓存失效与 flock；
15. FSKit extension、签名 CLI、security-scoped 数据库目录和 `.vexfs-volume.json` 的升级方案；
16. Finder 产生的 `.DS_Store`、xattr、资源 fork 和文件协调调用在技术预览中的支持边界。

## 26. 相关文档

- 最终目标与阶段路线图：`docs/plans/2026-07-21_vexfs-final-goal-and-roadmap.md`
- 技术设计：`docs/design/2026-07-16_agent-files-universal-filesystem.md`
- 可行性判断：`docs/analysis/2026-07-16_vexfs-requirement-feasibility.md`
- 文章研究：`docs/research/2026-07-16_agent-filesystem-article-research.md`
- 输入文章：`/Users/Four/.codex/attachments/90dd9c19-5911-4cbb-8751-e71298816b37/pasted-text.txt`

## 27. 安装与首次使用体验

### 27.1 用户需要安装什么

macOS 用户安装一个签名并公证的 VexDB-Lite 发行包，默认包含三个产物：

1. SQLite VexFS 数据库扩展；
2. 当前用户运行的本机 NFS gateway；
3. `vexdb` CLI，以及指向 `vexdb fs` 的 `vexfs` 兼容入口。

默认安装不要求用户进入“文件系统扩展”页面。`vexdb fs mount` 负责启动 gateway、确认它只监听
loopback、调用系统 NFS client 并验证真实读写。macOS 仍可能针对挂载操作、目标目录或下载的
可执行文件给出普通系统授权或 Gatekeeper 提示；安装器必须把实际提示做成真机 Gate，不能把
“不需要 FSKit 授权”写成“macOS 永远不会出现任何授权”。

后续 FSKit 版本可以在发行包中增加 `VexDB Lite.app` 和 App Extension，作为显式选择的
`--backend fskit`；它不能拥有另一套数据库 schema、版本、权限或备份逻辑。

发行包、解压验证目录和构建目录不得长期暴露第二份同 bundle ID 的 `.app`。打包完成后必须撤销这些临时路径的 LaunchServices 登记并删除展开 stage；安装包把 App 放在隐藏 `.payload` 中。扩展已经获准时，安装器必须以临时数据库执行真实 mount、写入、读回和 unmount，不能只根据注册状态宣布成功。

默认 SQLite 数据库放在 `~/Library/Application Support/VexDB-Lite/default.sqlite3`。NFS gateway
直接以当前用户权限打开数据库，不需要 security-scoped `FSPathURLResource`。非权威 mount
descriptor 只保存合同版本、数据库标识、workspace、gateway 实例和 mountpoint，不保存密码。

CLI 与 NFS gateway 必须在挂载前读取数据库扩展的合同版本和能力位。主版本不兼容时拒绝可写
挂载；卸载时先停止新操作、执行强 sync、报告仍未发布的 handle，再执行 unmount 并退出
gateway。安装、升级和卸载都不得删除数据库中的已提交文件。

运行时边界保持不变：数据库扩展保存全部权威状态；mount gateway 只把数据库文件展示为本地路径；CLI 管理连接和挂载。gateway 不保存最终文件，不管理数据库。

### 27.2 数据库初始化

```sql
-- PostgreSQL
CREATE EXTENSION vexdb_lite;
SELECT vexfs_init();

-- DuckDB
LOAD vexdb_lite;
SELECT vexfs_init();

-- SQLite
.load ./vexdb_lite
SELECT vexfs_init();
```

最终扩展名由技术评审决定，但三端必须提供一致的初始化和能力检查。

### 27.3 一次性配置并挂载

```bash
vexdb fs --workspace agent-workspace setup \
  --mount ~/VexDB/agent-workspace
```

未传 `--db` 时使用 `~/Library/Application Support/VexDB-Lite/default.sqlite3`。`setup` 初始化
SQLite 和 workspace；传入 `--mount` 时检查 gateway 签名、数据库合同、loopback 端口和系统
NFS client，创建私有 mount point 并请求系统挂载。默认流程不检查或要求启用 FSKit extension。
显式使用 `--backend fskit` 时，才检查 App Extension 和系统启用状态。

context 配置不能保存明文密码。认证使用数据库支持的密码文件、环境变量、系统凭证或交互提示。需要登录后自动挂载时，用户显式增加 `--persist`。

### 27.4 日常 Bash 使用

```bash
cd ~/VexFS/agent-workspace
ls
rg 'TODO' .
python scripts/generate.py
git status
```

这是真实挂载目录，不是 checkout 副本。程序通过普通文件 API 读写；NFS `COMMIT`、fsync、
close 和安全 unmount 映射到平台无关 sync。macOS mount 的专用 SQLite 连接使用
`WAL + NORMAL staging + FULL publish`：普通 write 返回时内容仍未发布，sync 成功时
完整版本已经发布并达到 FULL。这个策略不修改其他业务 SQLite 连接，`doctor` 必须显示
实际配置和风险。NFS handle close 和 gateway 故障恢复只负责句柄结束和可恢复兜底。另一个 handle
在重新 open 后看到已发布版本；MVP 不承诺多个 gateway 实时同步。

常用生命周期命令：

- `vexfs mount ~/VexFS/agent-workspace`：挂载当前 workspace；
- `vexfs mount status`：查看连接、principal、workspace 和待处理 handle；
- `vexfs unmount ~/VexFS/agent-workspace`：安全卸载；
- `vexfs doctor`：检查 gateway、系统 NFS client、loopback 端口、挂载表、签名、SQLite
  扩展、连接、权限和版本；显式选择 FSKit 时再检查 extension。

### 27.5 Agent 使用

Agent 的默认工作目录直接设置为 mount point：

```bash
cd ~/VexFS/agent-workspace
python scripts/generate.py
rg 'ERROR' logs/
```

无需让模型学习 VexFS 专用文件命令。Agent 运行环境仍负责进程沙箱，VexFS 只限制挂载目录内的数据库文件权限。

如果一个任务要求“多个文件要么全部成功，要么全部失败”，使用显式事务入口，而不是连续执行普通 Bash 命令：

```bash
vexfs txn run --workspace agent-workspace -- \
  bash -lc "python scripts/generate.py && rg 'ERROR' logs/"
```

`txn run` 使用隔离工作视图，在命令成功后把差异作为单个 changeset 提交；该能力为 P1。P0 中每个文件系统操作分别原子，多个命令不自动合成一个事务。

### 27.6 “无缝”的准确含义

VexFS 承诺：

- macOS 完成 setup 后，一条 mount 命令启动本机 gateway 并得到可以直接 `cd` 的真实路径；
- 默认入口不要求用户进入“文件系统扩展”页面；
- Bash、ls、cat、rg、sed、python、git、编译器和 apply_patch 直接可用；
- 不需要为每个系统命令开发 VexFS 版本；
- 每个文件操作仍由数据库统一处理事务、权限、版本、配额和审计；
- gateway 停止不会使数据库中的已提交文件失效。

VexFS 不承诺：

- 首个默认发行覆盖 Linux、Windows、普通容器或未进入 NFS 真机矩阵的 macOS 版本；
- 把多条 Bash 命令自动变成一个数据库事务；
- 支持设备文件、socket、FIFO、setuid 或全部 POSIX 特殊行为；
- 通过 mount point 获得高于数据库 principal 的权限。

不能挂载时，用户使用 `vexfs` 直接命令或 `vexfs shell/exec` worktree 回退。回退不改变数据库权威地位，也不作为安装后无缝 Bash 的主宣传路径。

## 28. 参考资料

- [PostgreSQL Large Objects](https://www.postgresql.org/docs/current/lo-intro.html)
- [PostgreSQL Backup and Restore](https://www.postgresql.org/docs/current/backup.html)
- [SQLite Limits](https://www.sqlite.org/limits.html)
- [SQLite Online Backup API](https://www.sqlite.org/backup.html)
- [SQLite Atomic Commit](https://www.sqlite.org/atomiccommit.html)
- [DuckDB Transactions](https://duckdb.org/docs/current/sql/statements/transactions)
- [DuckDB Concurrency](https://duckdb.org/docs/current/connect/concurrency)
- [DuckDB Securing DuckDB](https://duckdb.org/docs/current/operations_manual/securing_duckdb/overview)
- [Apple FSKit](https://developer.apple.com/documentation/fskit)
- [Apple Building a passthrough file system](https://developer.apple.com/documentation/fskit/building-a-passthrough-file-system)
- [Apple FSKit updates](https://developer.apple.com/documentation/updates/fskit)
- [WinFsp Documentation](https://winfsp.dev/doc/)
- [libfuse fuse_operations](https://libfuse.github.io/doxygen/structfuse__operations.html)
- [libfuse low-level operations](https://libfuse.github.io/doxygen/structfuse__lowlevel__ops.html)
- [libfuse FAQ](https://github.com/libfuse/libfuse/wiki/FAQ)
- [Linux FUSE I/O modes](https://docs.kernel.org/filesystems/fuse-io.html)
