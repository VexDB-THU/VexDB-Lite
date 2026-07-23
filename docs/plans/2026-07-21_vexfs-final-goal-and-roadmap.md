# VexFS 最终目标与阶段路线图

- 文档类型：唯一有效的目标和开发顺序基线
- 所属产品：VexDB-Lite 的文件管理能力 VexFS
- 日期：2026-07-22
- 分支：`feature/agent_files`
- 文档版本：2.0
- 当前阶段：Phase 1，SQLite 本地开发者预览

## 1. 这份文档解决什么问题

VexFS 已经从 SQLite SQL/CLI 发展到 macOS FSKit 和 Linux libfuse3 真实挂载。如果继续按单个功能推进，很容易出现三个问题：

1. 做了很多 POSIX 功能，但没有形成用户可以安装和验证的产品；
2. 数据库、操作系统和 CLI 各自扩展，最后变成多套不同实现；
3. 新功能不断加入，核心目标“数据库管理的无缝 Agent 工作区”反而被冲淡。

从本版本开始，文档职责固定如下：

- 产品规格定义“产品是什么、用户需要什么”；
- 技术设计定义“统一合同和各适配器怎么实现”；
- **本路线图定义“最终做到哪里、现在在哪、下一步按什么顺序做”。**

如果其他文档中的阶段顺序、实现状态与本路线图冲突，以本路线图为准。历史分析文档只保留当时的分析价值，不再决定当前开发顺序。

## 2. 不变的最终目标

> 用户下载并安装 VexDB-Lite 后，可以在 macOS、Linux 或 Windows 上把数据库工作区挂载成普通目录。人、Coding Agent 和现有工具直接使用 Bash、Git、编辑器和编译器，不需要改命令，也不需要学习另一套文件协议。文件、目录、权限、版本、快照、审计、配额和备份都由数据库管理，挂载进程不保存权威数据。

VexFS 是 VexDB-Lite 内的一项文件管理能力，不是独立 SDK、独立客户端或反过来管理数据库的文件服务器。

### 2.1 最终用户体验

```bash
# 安装 VexDB-Lite 后完成一次初始化和授权
vexdb fs setup

# 把数据库工作区挂载为普通目录
vexdb fs --workspace workspace mount ~/Work/agent-workspace

# 后续全部使用原生命令和现有 Agent
cd ~/Work/agent-workspace
ls
rg "TODO"
git status
opencode

# 查看和恢复数据库保存的工作区版本
vexdb fs --workspace workspace snapshot list
vexdb fs --workspace workspace snapshot restore before-refactor
```

产品可以继续保留 `vexfs` 兼容命令，但对外安装物和主入口应属于 VexDB-Lite，避免用户把向量、SQLite 基础能力和文件能力理解成三个无关产品。

### 2.2 最终能力边界

最终版本必须同时满足以下条件：

1. **统一安装**：同一份 VexDB-Lite 发行物包含数据库基础能力、向量能力和文件能力；安装过程会检查并引导系统挂载依赖。
2. **无缝终端**：挂载后可直接运行常用 Bash、Git、Coding Agent、编辑器、编译器和构建工具。
3. **数据库权威**：已提交文件及元数据只以数据库为准；mount adapter 崩溃、重启或被删除不能破坏已提交数据。
4. **本地和远程**：SQLite 提供单机工作区；PostgreSQL 提供跨电脑、多用户和共享工作区；DuckDB 提供本地分析场景。
5. **版本和恢复**：支持文件历史、完整工作区快照、一键恢复、备份恢复、保留策略和垃圾回收。
6. **管理能力**：支持身份、权限、ACL、审计、配额、完整性检查和安全导入导出。
7. **跨平台一致**：macOS、Linux、Windows 共享同一逻辑合同；平台不支持的能力必须明确返回错误，不能静默丢失数据或权限。
8. **可发布质量**：有干净机器安装、升级、卸载、故障、规模、安全和性能证据，不以“能编译”或“单个 demo 能运行”代替完成。

## 3. 不能改变的架构规则

以下规则用于判断一个需求是否会让项目偏离目标：

1. **数据库管理 VexFS**。数据库扩展是权威核心，挂载进程只是协议翻译层。
2. **核心不做语义层**。Embedding、向量搜索、BM25 和 Agent 记忆不属于文件系统核心；它们可以读取 VexFS 文件，但不能改变文件合同。
3. **不是 3 × 3 九套实现**。三个数据库 adapter 加三个操作系统 mount adapter，通过统一 SQL/C ABI 和测试合同组合。
4. **mount 不拥有最终状态**。它只保留 handle、缓存和未发布写入；所有提交、冲突、权限和版本判断回到数据库合同。
5. **SQL 和 Bash 都是一等入口**。没有 mount 时数据仍可通过 SQL/CLI 使用；挂载不能成为读取数据库文件的唯一办法。
6. **快照不等于备份**。快照用于版本检查点和误操作恢复；数据库原生备份或逻辑导出用于灾难恢复和跨机器迁移。
7. **正确性先于索引加速**。原生 `grep`、`rg` 先按普通文件工作；索引搜索是后续显式能力，不伪装成透明的原生命令加速。

## 4. 明确不做什么

这些内容不是 VexFS 的目标，除非路线图经过正式修改：

- 不替代 APFS、ext4、NTFS 等系统根文件系统；
- 不追求所有 POSIX 特殊文件和内核设备语义；
- 不替代 Git 的分支、合并和代码协作能力；
- 不把大视频、模型文件和数据湖对象作为第一目标；
- 不新建脱离数据库权限和事务的文件服务器协议；
- 不让 DuckDB 承担多用户远程文件服务器职责；
- 不为了宣传“更快 grep”而阻塞挂载正确性、备份和跨机器工作区；
- 不在核心文件合同中加入 Agent 专用的 prompt、memory 或语义字段。

## 5. 当前基线：2026-07-23

| 范围 | 当前事实 | 主要缺口 |
|---|---|---|
| 统一合同 | SQLite 合同 `0.9.0`，runtime ABI 1；SQL、C ABI、CLI、mount runtime 已连通；已增加跨平台一致性合同和结构化错误状态 | PostgreSQL、DuckDB 尚未实现 VexFS adapter |
| 文件语义 | 文件、目录、四类时间戳、并发 append、进程锁、mode、symlink、xattr、hardlink、owner/group 元数据、便携 ACL 已进入数据库合同和测试 | ACL 还没有完整授权执行和继承；特殊文件不支持 |
| 版本恢复 | 单文件版本、workspace commit、snapshot、diff、expected-head restore、SQLite Backup、retention、显式分批 GC、live quota 和 format v2 逻辑导入导出已实现 | 审计、自动维护、history/staging/index/total quota、PG/DuckDB 导入端未完成 |
| 长期校验 | `chunked-v1` 使用不可变 manifest、64 KiB 块和逐块/整文件 SHA-256；只读 check 覆盖结构、引用、顺序、大小和流式内容校验 | 自动 repair 和跨文件通用去重不在当前范围 |
| macOS | preview.22 已从干净 commit `8a8b0c139a` 构建并通过 Apple 公证；本机 13 个真实挂载 Gate 通过，另一台无源码 M1 macOS 26.5.2 的 30 项安装后 smoke 也通过 | macOS 26.0–26.4 与 x86_64 尚未独立发行验证；构建机 26.3.1(a) 的 Code Signing 子系统当前异常 |
| Linux | libfuse3 真实挂载已通过 root 和普通用户；时间戳、并发 append、进程锁、打开文件生命周期、强制卸载和 helper 崩溃恢复已验证；x86_64/AArch64 manylinux 安装包已在对应架构真机完成无 root 安装回归 | 缓存/TTL、更多干净发行版、长期运行和真实挂载安装回归仍不足 |
| Windows | 只有平台边界和规划 | WinFsp adapter、安装签名、路径/ACL/SID 合同均未实现 |
| 远程共享 | 统一合同为 PostgreSQL 预留 | 还不能让另一台电脑挂载同一个远程工作区 |
| 性能规模 | SQLite 直连已完成 10 万文件；preview.20 真机为 46.863 MiB/s 顺序写、1,041.044 次/s 随机覆盖、175.253 files/s，1 千文件普通扫描 428.041 ms、索引搜索 7.449 ms | 小文件创建仍比同轮 APFS 慢 72.012 倍，普通扫描慢 23.422 倍；后续仍需重跑 1 万/10 万和真实 Coding Agent |

“合同中已保存”不等于“所有平台已经完整执行”。例如 owner/group 和便携 ACL 已经可以保存、读取和恢复，但身份认证、权限判断和各系统的原生 ACL 映射仍属于后续阶段。

### 5.1 当前收口 Gate

2026-07-22 产品对齐审查后，Phase 1 先执行收口 Gate，再继续长期存储能力：

1. **已完成代码修复**：零用例失败、`PASS_WITH_SKIPS`、`--fail-on-skip`、显式 package/mount CLI 绑定、runtime ABI 单一来源、旧 schema 明确拒绝、数据库符号链接拒绝、备份目标 0600、安装哈希/签名/ABI/合同校验；
2. **已完成本地交付验证**：当前源码 ad-hoc 包通过 64 项文档 smoke 和 2 个 package Gate；这只证明本地包结构与功能，不替代 Developer ID、公证和真实 FSKit；
3. **已完成 macOS arm64 发行证明**：preview.22 从干净 commit 重新 Developer ID 签名、公证，
   本机 13 个真实 mount Gate 通过；另一台没有源码和完整 Xcode 的 M1 也完成安装、真实挂载、
   Git、快照、deep check 和 archive v2 的 30 项 smoke；
4. **长期安全与分块内容模型已完成**：checksum/check、retention/GC、live quota、SQLite format v2
   逻辑 export/import、chunk + manifest 和 append 合并发布已进入 spec、runtime/CLI smoke 和 eval；
   下一步集中解决小文件写入性能，并补 macOS x86_64 交付验证。

当前不提前开发 PostgreSQL、DuckDB 或 Windows adapter。

### 5.2 checksum/check 完成证据

2026-07-22 SQLite 合同升到 `0.7.0`：普通写入保存 canonical BLOB 和 SHA-256；单文件恢复
与 workspace 恢复只创建直接别名，并复制源版本的 size 和 checksum。默认读取、历史读取、
版本比较和数据库 grep 都会拒绝返回校验失败的内容。

- `vexdb fs check` 默认深度检查，`--quick` 只检查结构；不存在的数据库不会被 check 创建；
- 损坏注入覆盖同长度 BLOB 改写、目录父引用、commit 父引用、snapshot、version alias 和
  staging BLOB 缺失；深度内容损坏返回 `VEXFS_CHECKSUM_MISMATCH`，CLI 退出码为 8；
- 最终 quick eval：52 passed、0 failed、18 个平台性 skipped、3121 checks；
- 4 MiB / 4 文件深度扫描为 0.081381 秒、49.152 MiB/s，quick 为 0.001861 秒；检查阶段
  观测到的额外 RSS 峰值增长为 0；
- 报告：`vexdb_sqlite/build/eval/vexfs/20260722T104155.646993Z-quick-20260718/report.json`。

这是合同 `0.7.0` 当时的证据；当前 `0.9.0` 已由下方 `chunked-v1` 证据替代，但自动 repair 仍不在范围内。

### 5.3 长期安全最小集合完成证据

2026-07-23 SQLite 合同升到 `0.8.0`，旧 schema 明确拒绝且不迁移：

- quota 覆盖最大 live bytes、live files 和单文件 bytes；数据库触发器维护 O(1) live 计数；
- retention 支持最近版本数和天数；GC 显式分批，保护 current、snapshot、handle 和 alias source，
  活动挂载以及 `gc pause` 会阻止删除；
- `.vexfs` format v1 是独立逻辑记录容器，不是原 SQLite 数据库副本；包含 commit、inode、
  dentry、文件版本、快照、xattr、ACL、principal 和逐记录/内容/整包 SHA-256；
- 导入在一个事务内使用 `importing` 隐藏状态，完成整包校验、ID 映射和深度 check 后才原子发布；
- quick 性能 eval 中，开启配额创建 250 个 1-byte 文件为 0.101915 秒，即 2,453.024 files/s；
  300 个历史版本按 128 条分批 GC 为 0.015086 秒，共 3 批，删除 299 个版本；
- 8 MiB canonical 内容的固定快照导出、独立 verify、导入、hardlink/版本/xattr/ACL 对比以及
  内容损坏和半成品注入测试通过；export、verify、import 的 BLOB 路径固定使用 1 MiB 缓冲。

GC 删除记录后只让 SQLite 页面可复用，不自动 `VACUUM`；format v1 当前只有 SQLite producer/
consumer，PG/DuckDB adapter 尚未开始。完整证据见
`docs/reports/2026-07-23_vexfs-retention-quota-logical-archive.md`。

### 5.4 chunk + manifest 完成证据

2026-07-23 SQLite 合同升到 `0.9.0`，逻辑归档升到 format v2：

- canonical 文件版本只引用不可变 manifest，不再在版本行保存整文件 BLOB；
- manifest 固定 64 KiB 逻辑块，物理 chunk object 可以被同 inode 的多个版本和多个块位置复用；
- 挂载打开从 chunk 流式写入 staging，publish 两遍扫描 staging 后流式生成 manifest/chunk，固定
  64 KiB 工作缓冲；不会额外组装 128 MiB 整文件；
- 随机覆盖只新建受影响块；恢复版本继续使用版本别名，不新建 manifest；GC 只删除没有任何
  manifest 引用的物理块；
- quick check 验证 manifest 引用、块数量、顺序、大小和可达性，deep check 再验证逐块和整文件
  SHA-256；
- `.vexfs` format v2 携带 manifest/chunk 记录，verify/import 均逐块处理，导入会恢复块复用；
- 8 MiB、100 次 4 KiB 随机修改样本中，71 个 64 KiB 块受影响，57 个块直接复用，两个版本最终
  只保存 72 个物理块；发布耗时 0.370 秒；
- 最终 quick eval 为 57 passed、0 failed、18 个平台条件 skipped、2,863 checks，耗时 24.824 秒；
  SQLite spec、runtime smoke、VexFS CLI smoke 和 VexDB unified CLI smoke 全部通过。

当前仍没有跨文件通用去重、自动 repair、自动 GC 或在线 VACUUM。完整证据见
`docs/reports/2026-07-23_vexfs-chunk-manifest-v2.md`。

## 6. 阶段路线图

每个阶段必须同时写清范围、暂时不做的内容和完成证据。没有达到完成条件，不得把阶段标为完成。

### Phase 0：统一合同与垂直闭环 — 已完成

目标：证明“数据库是权威状态，同时 Bash 可以操作真实挂载目录”在技术上成立。

已完成范围：

- SQLite 内部 schema、SQL API、C ABI、CLI 和 mount handle 合同；
- 写入暂存、publish/sync/close/reclaim、request-id 和崩溃恢复；
- macOS FSKit 真实挂载和 SQLite 垂直闭环；
- 文件版本、workspace commit、快照和恢复；
- mode、symlink、xattr、hardlink、owner/group 和便携 ACL 基础合同；
- Linux libfuse3 的真实普通用户挂载验证。

完成证据：数据库重开、挂载重开、基础 Bash/Git、快照恢复、备份恢复和错误注入测试已经形成 eval。

### Phase 1：SQLite 本地开发者预览（macOS + Linux）— 进行中

目标：把当前原型变成其他开发者可以安装、可以日常运行、出现故障可以恢复的本地产品。

本阶段必须完成：

1. 建立 macOS FSKit 和 Linux FUSE 共用的 mount 一致性 eval，防止两个 adapter 行为分叉；
2. 补齐真实项目最容易碰到的正确性：时间戳更新、`O_APPEND` 并发、`flock`/`fcntl` 最小锁合同、rename/unlink 打开文件、崩溃和强制卸载；
3. 明确缓存和 TTL 行为，验证重新挂载、两个进程和数据库重开后不会读到错误内容或权限；
4. 做 macOS 与 Linux 的 SQLite 工作区互换测试，覆盖 Unicode、大小写边界、mode、symlink、hardlink、xattr/ACL 和快照恢复；
5. 跑真实 Python、Node.js、Go、Rust、Git 和至少一种 Coding Agent 工作区；
6. 完成 1 万、10 万文件以及小文件/大文件/混合读写的正确性和性能基线；
7. macOS 完成 Apple 公证和干净机器安装；Linux 完成 x86_64、AArch64 安装包和普通用户安装说明；
8. 补齐 check、retention、GC、quota、export/import 中保证长期试用不会失控的最小集合。

暂时不做：

- PostgreSQL 多用户产品能力；
- Windows adapter；
- DuckDB adapter；
- 透明加速原生 `grep`；
- 完整 POSIX 特殊文件。

完成条件：

- 两台干净机器只按使用文档即可安装、挂载、运行真实项目、卸载和恢复；
- 普通用户可以持续使用，不要求源码构建或 root 运行 mount helper；
- macOS/Linux 共用一致性 eval 全通过，没有静默丢文件、版本、权限或链接；
- 10 万文件和长时间运行测试达到文档中的性能目标；
- 数据库备份恢复、进程崩溃、断电等价故障和错误卸载有可重复证据；
- 发布包、版本清单、使用文档和已知边界放在一起。

### Phase 2：PostgreSQL 共享工作区 Alpha（macOS + Linux）— 未开始

目标：让电脑 A 创建的 Agent 工作区可以由电脑 B 通过 PostgreSQL 直接挂载，并由数据库角色管理并发和权限。

本阶段必须完成：

- PostgreSQL HostStore 和与 SQLite 等价的 SQL/C ABI 合同；
- 每个 mount 绑定真实 PostgreSQL role，完成 principal、ACL、审计和配额；
- 多 gateway、多 Agent、文件锁、expected-head 和并发冲突；
- 网络断开、重连、数据库重启、缓存失效和慢网络行为；
- 文件变更与普通业务 SQL 在同一数据库事务提交或回滚；
- pg_dump/pg_restore、物理备份和逻辑导入导出；
- 电脑 A 写入、电脑 B 挂载并继承同一版本历史和快照。

完成条件：

- 两台电脑可以同时操作一个远程 workspace，权限隔离和冲突行为可重复；
- 任一 gateway 或数据库重启后不出现静默覆盖；
- PostgreSQL 原生备份恢复后文件、历史、权限和业务表处于同一时间点；
- macOS 和 Linux 使用同一 mount 一致性测试，不增加 PostgreSQL 专用文件语义。

### Phase 3：Windows 与跨系统预览 — 未开始

目标：通过 WinFsp 接入同一 mount runtime，使 Windows 既能使用本地 SQLite 工作区，也能挂载 PostgreSQL 远程工作区。

本阶段必须完成：

- WinFsp adapter、盘符/目录挂载、安装、签名和卸载；
- Windows 路径、保留名、大小写、Unicode、share mode 和 delete-pending；
- SID 与 VexFS principal/owner/ACL 的明确映射；
- symlink/reparse point、hardlink、可执行属性和 xattr 的跨系统降级规则；
- macOS、Linux、Windows 三端读写同一逻辑工作区的兼容测试。

完成条件：Windows 上常用终端、Git、编辑器、构建工具和 Coding Agent 可以直接使用；跨系统移动工作区不会静默改变内容、版本或权限。

### Phase 4：DuckDB 本地分析工作区 Beta — 未开始

目标：让 DuckDB 用户在本地分析流程中使用相同文件合同、版本和 CLI，而不是新建一套文件系统。

本阶段必须完成：

- DuckDB HostStore 和相同 SQL/C ABI 能力矩阵；
- 事务冲突、checkpoint、reopen、backup/copy/export；
- SQLite、PostgreSQL、DuckDB 之间的逻辑导入导出；
- 默认提供 SQL、CLI 和 worktree；
- 只有 gateway 能成为唯一写进程时，才开放 DuckDB 可写 mount。

完成条件：DuckDB 不产生独有文件语义；相同 eval 可以验证支持的公共能力；不把单写者数据库宣传成多用户共享文件服务器。

### Phase 5：VexDB-Lite Files v1 稳定版 — 未开始

目标：把已验证能力固化为长期兼容、可升级和可支持的正式产品。

本阶段必须完成：

- 稳定 SQL、C ABI、CLI、schema 和逻辑导出格式；
- retention、GC、quota、audit、check 和修复工具；
- 三个数据库与三个操作系统的正式能力矩阵和版本协商；
- 安装、升级、降级保护、卸载、备份、灾难恢复和安全评审；
- 10 万文件以上规模、长时间运行、并发、故障和性能回归；
- 用户文档、管理员文档、Agent 使用文档和故障处理手册；
- 可选的显式文本索引和搜索命令，前提是不改变普通文件语义。

完成条件：所有正式承诺的平台和数据库都有可下载产物、干净机器证据、自动回归和清楚的已知边界。

## 7. 现在立即做什么

下一轮开发固定按以下顺序推进：

### 当前固定顺序（2026-07-23 M1 快照恢复复测后）

1. **P0（已完成）：修复 FSKit 元数据缓存失效。** hardlink 后立即刷新源 inode；create、symlink、
   hardlink、remove、rename 后刷新或失效相关父目录。250 ms TTL 只做兜底，不能代替
   mutation 后精确失效。
2. **P0（已完成）：修复 macOS 打包注册。** `package_preview.sh` 固定独立 DerivedData，
   在 export 前撤销 InstallationBuildProductsLocation，结束时删除 archive/export/DerivedData；
   安装完成后验证 FSKit module URL 必须指向已安装 App，不能只看 `enabled`。
3. **P0（已完成）：构建 preview.16 并重跑真实 Gate。** `mount.cross-platform-conformance`、
   `mount.timestamps` 必须恢复全绿；同时重跑 Bash、Git、工具链、强制卸载和 1 千文件
   性能，确认正确性修复没有明显性能回退。
4. **P0（代码和 M1 实测已完成）：修复挂载状态快照恢复。** 数据库拒绝活动 mount 的直接
   restore；CLI 正常卸载、恢复并原位重挂载；FSKit 在 `unmount()` / `deactivate()` 关闭
   session，在 `activate()` 重开并清理旧缓存。M1 实测 lease 约 2.832 ms 释放，恢复后内容
   正确且无残留挂载。
5. **P0（下一项）：从已提交的干净源码重新签名并公证当前 preview。** 使用已经验证可用的
   `vexdb-lite-notary` profile，要求 Apple `Accepted`、staple、Gatekeeper、安装后 doctor 和
   快照恢复真机回归全部通过；旧 preview.17 的公证不能替代当前实现的公证。
6. **P0（紧随公证）：把本次真实挂载快照恢复脚本变成正式 eval。** 覆盖活动 mount 直接
   restore 被拒、一键卸载/恢复/重挂载、lease 立即释放、旧缓存清理、失败回滚、多 workspace
   定位和最终无残留挂载，避免以后只靠 `/tmp` 手工脚本发现回归。
7. **P1：完成干净 Mac 安装。** 在没有源码、Xcode DerivedData 和历史 VexDB Lite 注册记录的
   Mac 上，仅按压缩包文档完成安装、启用、挂载、Bash/Git、快照恢复和卸载。
8. **P1：完成长期管理能力。** `check` 已完成，下一项固定为 retention/GC，随后是 quota 和
   export/import，先保证长期使用不会无限增长且可以迁移、恢复。
9. **P1：恢复规模和 Agent 验证。** 先 1 万文件，再决定是否重跑 10 万；最后执行真实
   OpenCode 工作区。所有大测试继续保留 RSS 上限和低并发。

当前 1–4 已完成。接下来严格按 5（当前代码公证）→ 6（永久回归）→ 7（干净 Mac）推进，
然后再做 retention/GC。以上完成前，不把主要开发重心切到 PostgreSQL、DuckDB 或 Windows；
三种数据库和三端适配仍是最终目标，但必须建立在稳定的文件系统合同和发行 Gate 上。

### N1. 共用 mount 一致性 eval

先把同一套文件操作分别跑在 macOS FSKit 和 Linux FUSE 上，覆盖 errno、mode、link、xattr、snapshot、重挂载和故障。它是后续增加 PostgreSQL、Windows 时防止出现九套行为的基础。

状态（2026-07-22）：**已完成**。

- 已建立 `mount.cross-platform-conformance`，macOS FSKit 与 Linux libfuse3 共用同一测试函数和 50 项检查；
- Linux AArch64 已在真实 `/dev/fuse` 上分别以 root 和 uid 1000 通过；
- 测试发现并修复了非空目录错误由 `EEXIST` 错映射的问题，数据库合同现返回结构化 `VEXFS_MOUNT_NOT_EMPTY`，平台映射为 `ENOTEMPTY`；
- macOS 已使用 `Developer ID Application: Jian ming Wu (BB5VK42K87)` 重新签名并启用 FSKit，在真实挂载、快照恢复和重挂载上通过同一套 50 项检查；
- Developer ID `preview.5` 统一安装回归通过 13 项检查，App、extension、CLI 和 SQLite dylib 的 TeamIdentifier 均为 `BB5VK42K87`。
- preview.15 的同一合同真实重跑曾发现 hardlink 即时 `st_nlink=1`，而数据库中为 2；
  preview.16 在 hardlink 成功后立即刷新源 inode，并把真实 module URL 纳入自动验收，
  最终 macOS 真实挂载 52 项检查全部通过。

证据位置：

- Linux root：`vexdb_sqlite/build/eval/vexfs-linux-mount/root/latest.json`
- Linux uid 1000：`vexdb_sqlite/build/eval/vexfs-linux-mount/uid-1000/latest.json`
- macOS FSKit：`vexdb_sqlite/build/eval/vexfs/20260721T052213.276765Z-quick-20260718/report.json`
- macOS Developer ID 安装：`vexdb_sqlite/build/eval/vexfs/20260721T051850.511156Z-quick-20260718/report.json`
- macOS preview.16：`vexdb_sqlite/build/eval/vexfs/20260722T061930.005789Z-quick-20260718/report.json`
- 测试入口：`tests/eval/vexfs/run_linux_mount.sh`

### N2. 时间戳、锁、append 和挂载生命周期

补 `utimens`、最小 `flock`/`fcntl`、并发 `O_APPEND`、打开文件 rename/unlink、强制卸载和 helper 异常退出。这些问题比增加更多 CLI 命令更容易破坏真实项目。

状态（2026-07-22）：**已完成**。

- 数据库保存 birth/access/modify/change 四类时间戳，macOS FSKit 与 Linux FUSE 均可读取并通过 `utimens` 更新；
- `O_APPEND` 使用数据库原子 append，四个并发进程各写 100 条记录时没有丢失、重复或覆盖；
- macOS 和 Linux 均通过最小 `flock`/`fcntl` 进程锁合同；
- 打开文件被 rename 或 unlink 后，已有句柄仍可读写并正常关闭；
- CLI 增加 `vexdb fs unmount --force MOUNT_POINT`，macOS 和 Linux 的占用挂载强制卸载均通过；
- Linux helper 被 `SIGKILL` 后，重新挂载会回收过期 session 并把保留的暂存写入发布到数据库；root 与 uid 1000 均通过；
- Developer ID `preview.8` 已包含 mount ABI 7。最终 macOS quick eval 为 62 passed、0 failed、3 个平台性 skipped，共 1027 项检查。
- preview.15 真实 `mount.timestamps` 曾显示创建子项后父目录即时 mtime 没有推进；
  preview.16 在 mutation 提交后直接更新父目录内存时间戳，不增加每个文件一次数据库
  stat。最终时间戳 10 项、并发 append 8 项、open/rename/unlink 14 项、锁 9 项、强制
  卸载 11 项均通过。

证据位置：

- macOS 最终全量：`vexdb_sqlite/build/eval/vexfs/20260721T061518.496228Z-quick-20260718/report.json`
- macOS 强制卸载单项：`vexdb_sqlite/build/eval/vexfs/20260721T061217.566816Z-quick-20260718/report.json`
- macOS preview.16 mount quick：`vexdb_sqlite/build/eval/vexfs/20260722T062028.825614Z-quick-20260718/report.json`
- Linux root 强制卸载：`vexdb_sqlite/build/eval/vexfs-linux-mount/root/20260721T060850.055986Z-quick-20260718/report.json`
- Linux root 崩溃恢复：`vexdb_sqlite/build/eval/vexfs-linux-mount/root/20260721T060850.232851Z-quick-20260718/report.json`
- Linux uid 1000 强制卸载：`vexdb_sqlite/build/eval/vexfs-linux-mount/uid-1000/20260721T060850.416919Z-quick-20260718/report.json`
- Linux uid 1000 崩溃恢复：`vexdb_sqlite/build/eval/vexfs-linux-mount/uid-1000/20260721T060850.564144Z-quick-20260718/report.json`
- 签名包：`dist/vexdb-lite/vexdb-lite-0.1.0-preview.8-macos-arm64.zip`
- SHA-256：`0e0c10f8711950eed9558aa57369a0461baa6f4ee008990850089698926e7c44`

### N3. macOS ↔ Linux 可移植性

在两端打开同一个 SQLite Backup 或 export，验证内容、目录、Unicode、mode、symlink、hardlink、xattr/ACL、历史和快照；同时明确 owner/group 在另一台机器上的映射规则。

状态（2026-07-21）：**已完成**。同一数据库文件已完成 macOS → Linux → macOS 真机往返。
数字 UID/GID 在数据库和快照中原样保存；Linux 挂载原样显示，macOS
普通用户 FSKit 挂载被系统强制设为 `noowners`，Bash `stat` 显示挂载用户，原始值通过
`vexdb fs stat` 读取。该行为必须作为明确的跨平台规则，不能误报为 owner 丢失。

已验证内容：目录、Unicode、mode、可执行脚本、symlink、hardlink、xattr、便携 ACL、数字
owner、文件历史、macOS 快照在 Linux 恢复、Linux 快照在 macOS 恢复，以及最终
`PRAGMA integrity_check`。

证据位置：

- 汇总目录：`vexdb_sqlite/build/eval/vexfs-portability/20260721T062755Z-72014`
- macOS 创建：`mac-create/20260721T062755.952108Z-quick-20260718/report.json`（6 checks）
- Linux 往返：`linux-roundtrip/20260721T062841.965486Z-quick-20260718/report.json`（36 checks）
- macOS 回读：`mac-verify/20260721T062842.499861Z-quick-20260718/report.json`（37 checks）
- 测试入口：`tests/eval/vexfs/run_cross_platform_portability.sh`

### N4. 真正可交付的安装包

macOS 完成 Apple 公证、干净机器安装和 extension 启用流程；Linux 完成普通用户依赖检查、x86_64/AArch64 包和卸载。安装说明必须与压缩包或安装包一起交付。

状态（2026-07-23）：**进行中**。macOS arm64 已由 `preview.22` 从干净 commit
`8a8b0c139a` 完成交付闭环；N4 剩余主要缺口是 macOS x86_64 和更多 Linux 发行版验证。

- preview.22 使用 `Developer ID Application: Jian ming Wu (BB5VK42K87)` 签名，Apple 公证
  submission ID 为 `406b904b-6b01-4ae7-8283-4db6c909c5cc`；staple、Gatekeeper、解压复验、
  67 项文档 smoke 和 17 项 package Gate 全部通过；
- 本机 13 个 macOS 必跑 mount 用例全部逐项开启 `--fail-on-skip`：13 passed、0 failed、
  0 skipped、227 checks；覆盖 Bash、Git、Python/Node/Go/Rust、并发、锁、元数据、重挂载、
  性能和 1,000 文件；
- 另一台没有源码、只有 Command Line Tools 的 M1 已覆盖安装 preview.22；FSKit extension
  enabled，30 项无源码 smoke 通过，覆盖 Git、快照恢复、deep check、archive v2 校验与导入；
- 该真机先后发现并验证修复两个真实问题：未发布 `index.lock` 的 version-0 历史污染，以及
  快照 archive 把已删除 inode 的 ACL 当作当前 ACL；preview.20/21 因此不再作为推荐包；

历史包证据继续保留：

- 历史 `preview.17` 已使用 `Developer ID Application: Jian ming Wu (BB5VK42K87)` 签名；
  App、FSKit extension、CLI 和 SQLite dylib 的签名链、统一安装回归与文档 smoke 均通过；
- 打包会在 Xcode export 前撤销临时 App 注册并删除专用 archive/export/DerivedData；真实
  mount eval 会拒绝 module URL 指向 `.xcarchive`、DerivedData 或 dist 副本；
- `preview.17` 已由 Apple 公证接受，submission ID 为
  `57118d6b-12d9-4fb1-9c5d-147eb4fc4bca`；App 已 staple，`spctl` 返回
  `source=Notarized Developer ID`；最终包安装后的真实 FSKit quick 为 14 passed、
  0 failed、3 skipped、229 checks；
- 当前发行脚本已经禁止公证脏工作树，Developer ID 包默认禁止脏源码；安装器核对官方
  TeamIdentifier、SHA-256、文件合同和 runtime ABI，ad-hoc 包必须显式打开本地测试开关；
- `preview.19-test` 的 M1 真机普通卸载后 lease 约 `2.832 ms` 消失；CLI 一键恢复自动重挂载，
  旧内容恢复、快照后新增文件消失，最终 `mount status` 为 `[]`。当前 ZIP SHA-256 为
  `3e45f0684dc34118450f55887a3f75ef5f10eef6aa7e21cf48b32b585cd050ef`，但它尚未公证，
  不能作为最终对外交付包；
- package eval 不再接受零用例或必跑 SKIP，并要求显式指定本次 stage；当前工作树生成的
  `0.1.0-gate0-local` ad-hoc 包已通过 64 项文档 smoke、统一安装以及 2 个 package Gate，
  但它不作为正式发行物；
- Linux x86_64 和 AArch64 均在对应架构的 manylinux_2_28 环境原生构建；CLI、libfuse3
  helper 和 SQLite loadable extension 均无超出 `GLIBCXX_3.4.22` 的依赖；
- 两个 Linux 最终压缩包均带安装、卸载、中文使用说明、MANIFEST 和文件哈希；
- 两台对应架构的真实 Linux 机器均在全新临时 HOME 中通过无 root 安装、内置 SQLite 查询、
  独立 `.so` 加载、FUSE helper 自检、VexFS 文件读写、卸载和数据库保留测试；
- 发行脚本增加可重复入口：`bash scripts/release.sh test sqlite all`。

交付物和证据：

- macOS：`dist/vexdb-lite/vexdb-lite-0.1.0-preview.22-macos-arm64.zip`
  （SHA-256 `773b0c05e47908dfe2b8f890be4a16f04f10ff3c1937843e9e2aea1c3aa25d1c`）；
- Linux x86_64：`dist/release/vexdb-lite-sqlite-files-linux-x86_64.tar.gz`
  （SHA-256 `7a736d020cb523740b6e5763d27d96dcb7a7124442b851f2f1c4d389b31e2085`）；
- Linux AArch64：`dist/release/vexdb-lite-sqlite-files-linux-aarch64.tar.gz`
  （SHA-256 `8455773a2808e60dfb3ba60366f634a1bd55b560825722c9bb1aa49e074ef6fe`）；
- 详细报告：`docs/reports/2026-07-21_vexfs-preview9-cross-platform-release.md`。
- preview.16 修复报告：`docs/reports/2026-07-22_vexfs-preview16-p0-fixes.md`。
- preview.17 公证报告：`docs/reports/2026-07-22_vexfs-preview17-apple-notarization.md`。
- preview.20 公证与真实 FSKit Gate：
  `docs/reports/2026-07-23_vexfs-preview20-notarized-fskit-gate.md`。
- preview.22 无源码 M1 真机 Gate：
  `docs/reports/2026-07-23_vexfs-preview22-clean-mac-gate.md`。

### N5. 真实项目和规模基线

持续运行 Git、Python、Node.js、Go、Rust 和 Coding Agent；补 1 万/10 万文件、大小文件混合、并发和长时间运行。结果全部进入 eval，不只保留手工日志。

状态（2026-07-22）：**进行中**。

- `mount.real-toolchain-projects` 已在真实 FSKit 挂载中运行 Python 3.12、Node.js 26、
  Go 1.22、Cargo 1.93 和 Git 2.50，卸载、重挂载后全部再次通过；
- SQLite 直连已通过 100,000 文件：123.327 秒创建，810.854 文件/秒；
- `preview.13` FSKit 已通过 10,000 文件创建、遍历、`rg`、快照恢复和重挂载：
  总耗时 177.224 秒；创建 114.828 秒，`rg` 23.895 秒；
- schema 热路径缓存让 10,000 文件创建较 `preview.12` 快约 37%，有界幂等日志至少
  保留最近 65,536 条并已由 69,632 行 eval 验证批量清理；
- 100,000 文件真实 mount 完成创建、遍历和抽样读取，但 `rg` 在 3,600 秒内未完成，
  用例总耗时 13,002.565 秒后 FAIL；快照恢复和重挂载因旧 eval 提前退出尚未验证；
- 60 秒混合稳定性通过 59,904 次操作、299 次数据库重开、29 个快照和 11 次 checkpoint，
  没有未发布 staging；
- 900 秒长测通过 594,000 次操作、2,970 次数据库重开、297 个快照和 118 次 checkpoint，
  共 102,516 项检查，最终没有未发布 staging；
- `preview.13` 全量 quick eval 为 68 passed、0 failed、5 skipped、3,639 checks；
- 数据库批量 `grep` 和显式可选 FTS5 trigram 索引已完成。1 万文件无索引扫描为
  6.416 秒，索引构建 0.024 秒，索引查询 0.013 秒，只读取 10 个候选文件；
- 规模 eval 已增加 1/1.5/2 GiB 峰值 RSS 保护线、64 MiB SQLite 页缓存和磁盘临时表；
- 2026-07-22 完整 quick eval 为 68 passed、0 failed、5 skipped、3,414 checks；
- handle open/close 降低 durability 的实验没有带来有效写入收益，最终包已恢复 FULL；
- 真实 OpenCode 已形成显式 eval，但审批要求用户看到具体外发风险后再次明确授权，
  不能写成已通过；
- preview.17 公证最终包在真实 FSKit 上完成 14 passed、0 failed、3 skipped、229 checks；
  1000 文件创建 333.971 files/s，同轮比 APFS 慢 38.954 倍；
- preview.20 的 13 个 macOS 必跑 Gate 为 13 passed、0 failed、0 skipped、227 checks；8 MiB
  顺序写 46.863 MiB/s、随机写 1,041.044 ops/s，1,000 文件创建 175.253 files/s，同轮比
  APFS 慢 72.012 倍；
- preview.22 在另一台无源码 M1 上完成 30 项安装后 smoke；Git workspace、快照恢复、索引、
  deep check 和 archive v2 导出/校验/导入均通过；
- 尚需完成 100,000 文件完整复测、真实 Coding Agent 和写入优化。

证据位置：

- 详细报告：`docs/reports/2026-07-21_vexfs-scale-and-toolchain-eval.md`
- 10 万 SQLite：`vexdb_sqlite/build/eval/vexfs/20260721T074800.216004Z-stress-20260718/report.json`
- 1 万 FSKit：`vexdb_sqlite/build/eval/vexfs/20260721T124346.009574Z-full-20260718/report.json`
- 10 万 FSKit 失败基线：`vexdb_sqlite/build/eval/vexfs/20260721T085806.189675Z-stress-20260718/report.json`
- 60 秒稳定性：`vexdb_sqlite/build/eval/vexfs/20260721T074017.731725Z-full-20260718/report.json`
- 900 秒稳定性：`vexdb_sqlite/build/eval/vexfs/20260721T084114.256428Z-stress-20260718/report.json`
- 最终 quick：`vexdb_sqlite/build/eval/vexfs/20260721T124750.583778Z-quick-20260718/report.json`
- 索引优化：`docs/reports/2026-07-22_vexfs-indexed-grep-optimization.md`
- 最新 quick：`vexdb_sqlite/build/eval/vexfs/20260722T021515.322756Z-quick-20260718/report.json`
- preview.17 最终公证包 mount quick：`vexdb_sqlite/build/eval/vexfs/20260722T065031.225854Z-quick-20260718/report.json`

只有 N1–N5 达到 Phase 1 完成条件，才把主要开发重心转到 PostgreSQL。可以提前做 PostgreSQL 设计验证，但不能用它代替 SQLite 本地预览的发布闭环。

## 8. 防偏离规则

新增需求进入开发前，必须回答四个问题：

1. 它服务于哪个最终目标和哪个当前阶段？
2. 如果现在不做，会阻塞本阶段哪条完成条件？
3. 它应该进入统一核心、数据库 adapter、mount adapter，还是只进入 CLI？
4. 用什么自动测试或真机证据证明完成？

如果无法回答，默认放入后续列表，不进入当前阶段。

阶段状态只能使用“未开始、进行中、已完成”。更新状态时必须同时更新：

- 当前事实和缺口；
- 完成证据或 eval 报告；
- 产品规格中的当前实现状态；
- 技术设计中的对应 adapter 状态。

不允许把“schema 中有字段”“函数能单独调用”“代码可以编译”写成完整产品能力。必须区分合同已实现、平台已接入、权限已执行、真机已验证和发行物已交付。

## 9. 相关文档

- 产品规格：`docs/specs/2026-07-16_vexfs-product-spec.md`
- 技术设计：`docs/design/2026-07-16_agent-files-universal-filesystem.md`
- Workspace 场景分析：`docs/analysis/2026-07-20_vexfs-claude-code-workspace-scenario.md`
- POSIX 与快照边界：`docs/analysis/2026-07-20_vexfs-remaining-posix-snapshot-feasibility.md`
- Eval 说明：`agent_files/eval/README.md`
