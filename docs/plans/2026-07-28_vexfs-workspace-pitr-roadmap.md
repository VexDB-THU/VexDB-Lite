# VexFS workspace PITR 后续需求顺序

日期：2026-07-28

当前状态：批次 A 的数据库合同、CLI 和自动测试已完成。PG 批量历史和 ACL 不可变集合也已
收口，全部 PG VexFS spec 为 12/12。下一项固定为批次 B 的 `workspace log`。正式发布前仍需
在最终 macOS NFS 包和 Linux FUSE 包上复跑真实挂载 Gate；这不是当前数据库实现缺口。

## 1. 产品目标

这一阶段不追求数据库宣传语里的“任意一秒恢复”。先完成 Coding Agent 真正需要的闭环：

1. Agent 运行前自动留下恢复点。
2. 用户能看懂这些恢复点和 workspace 变化历史。
3. 一条命令恢复整个 workspace。
4. 恢复期间不会被另一台机器继续写坏。
5. 恢复错了还能回到恢复前。

当这个闭环稳定后，再补 SQLite 任意 commit 恢复。PostgreSQL 的完整逐 commit PITR 根据真实需求决定，不提前增加长期写放大。

## 2. 固定优先级

### P0-0：数据库批量 find

状态：**已完成（2026-07-28）**。

- SQLite 和 PostgreSQL 均实现 `vexfs_find`，C ABI 与 `vexdb fs find` 已接通；
- 第一版范围固定为名称、类型、大小、修改时间、稳定路径游标和分页；
- PG 普通 role 不进入无读取权限目录，SQLite/PG 的 `?` 均按一个 Unicode 字符匹配；
- 1 万与 10 万文件功能、性能和 RSS 已进入 eval；详细证据见
  `docs/reports/2026-07-28_vexfs-database-find.md`。

这项能力先于 workspace log 完成，因为系统 `find` 只能在挂载后使用，数据库批量查询是 CLI、
远程 PG 和未挂载环境共同需要的基础能力。

### P0-1：PostgreSQL 多机恢复屏障

状态：**已完成（2026-07-28）**。

这是后续所有恢复能力的前置条件。

需求：

- PG 服务端恢复入口拒绝仍有有效 mount lease 的 workspace。
- mount 注册与 snapshot restore 必须使用同一个 workspace 锁，不能出现“检查完没有 mount，马上又挂载”的竞态。
- 恢复开始后，旧 session 和旧 handle 不能再发布数据。
- 第一版只做安全拒绝，不做远程强制踢下线。
- CLI 返回活动 session 数量和明确卸载建议，不只返回模糊的 busy。机器标识尚未写入
  mount lease，因此第一版不返回机器名。

验收：

- Mac A 和 Mac B 同时挂载时，A 恢复必须返回 `VEXFS_MOUNT_BUSY`，数据库内容不变化。
- B 正常卸载或 lease 到期后，A 可以恢复成功。
- B 的旧 dirty handle 不能在恢复完成后重新发布。
- 并发 mount/restore、断线、lease 到期、PG 重启都进入 eval。

### P0-2：恢复前自动安全快照

状态：**已完成（2026-07-28）**。

需求：

- `snapshot restore NAME` 在真正恢复前，自动为当前 HEAD 创建系统安全快照。
- 安全快照名包含 workspace、HEAD 和时间，并保证不会重名。
- CLI 成功输出同时返回 `restored_snapshot` 和 `safety_snapshot`。
- 安全快照创建和恢复在同一个数据库事务内；恢复失败时两者一起回滚，不留下半恢复状态。
- 第一版不提供默认关闭选项；以后如需 `--no-safety-snapshot`，必须作为明确的危险选项。

原因：

- SQLite 能从旧 commit 重建树，但公共接口尚未开放。
- PG 只能从命名快照重建完整树。没有安全快照就无法保证撤销一次错误恢复。

验收：

- SQLite、PG 都能执行“恢复 A -> 恢复安全快照 -> 回到恢复前状态”。
- 文件内容、目录、Git、mode、链接、owner、ACL、xattr 和索引状态完全一致。
- 配额不足、快照损坏、重挂载失败和进程退出都有原子性测试。

### P0-3：统一恢复正确性 Gate

状态：**数据库与 CLI Gate 已完成；发行物真实挂载复跑待下一次打包执行**。

P0-1 和 P0-2 完成后，建立一个不能跳过的统一 Gate：

- SQLite 本地挂载。
- PG 单机挂载。
- PG 两个 gateway。
- macOS NFS 和 Linux FUSE 共用同一套步骤。
- `expected_head` 冲突、远端活动 mount、旧 handle 发布、恢复撤销、数据库重启。
- 测试继续限制并发、文件数和 RSS，避免爆内存。

完成标准：所有 P0 用例进入 `tests/eval/vexfs/`，不能只保留手工脚本。

当前自动证据包括 SQLite C ABI/CLI eval、PG spec、PG C ABI runtime、PG adapter 并发
eval 和 PG CLI runtime。增强后的 `run_pg_runtime.sh` 会实际执行“恢复目标快照 -> 恢复自动
安全快照 -> 再次恢复目标快照”。

## 3. Agent 可用闭环

### P1-1：统一 workspace 日志

增加：

```bash
vexdb fs workspace log
vexdb fs workspace log --limit 50 --json
```

统一字段：

- commit ID
- parent commit
- 时间
- 操作类型
- path
- actor
- session/run ID
- 是否有对应快照

要求：

- SQLite 和 PG 输出同一 JSON 合同。
- 默认按新到旧排列，支持有界分页。
- 查询不能扫描文件正文，也不能在大历史上一次加载全部结果。
- actor 和 run ID 是通用审计字段，核心中不保存 prompt、memory 或模型语义。

### P1-2：快照分类和自动保留策略

在自动创建 Agent 快照前，先解决无限增长问题。

快照至少区分：

- `manual`：用户手工创建，默认长期保留。
- `agent`：Agent 任务前后创建，按策略清理。
- `safety`：恢复前自动创建，保留最近若干个或若干天。

命令：

```bash
vexdb fs snapshot policy show
vexdb fs snapshot policy set --agent-keep 20 --safety-keep 10 --days 30
vexdb fs snapshot prune --dry-run
vexdb fs snapshot prune
```

要求：

- 被保留的快照继续保护它引用的内容和历史。
- prune 与 GC 分开：先删除过期快照引用，再由 GC 分批回收。
- doctor/check 能报告快照数、受保护字节、可回收字节和最老恢复点。

### P1-3：Agent 运行前自动 checkpoint

增加通用命令，不绑定某一个 Agent：

```bash
vexdb fs run --snapshot-before -- opencode run ...
vexdb fs run --snapshot-before -- claude ...
```

行为：

1. 确认命令运行目录属于目标 workspace。
2. 等待已发布状态，创建 `agent` 快照。
3. 记录 run ID、actor、命令类型和开始时间。
4. 执行原命令，原样传递 stdin/stdout/stderr 和退出码。
5. 可选 `--snapshot-after-success` 创建完成快照。
6. 输出可以直接复制使用的恢复命令。

边界：

- 不分析 prompt，不判断 Agent 做了什么。
- 不把 Agent 专用字段放进文件系统核心。
- Bash 直接运行仍然可用；`fs run` 只是增加明确的任务边界。

验收：

- OpenCode 修改 Git workspace、执行测试、失败后恢复到运行前。
- 命令被 Ctrl-C、进程崩溃、网络断开时，运行前快照仍然可用。
- SQLite 与远程 PG 使用相同命令和输出。

## 4. SQLite commit 级恢复

### P2-1：从历史 commit 固定成快照

增加：

```bash
vexdb fs snapshot create before-bug --commit 123
```

不直接增加 `restore --commit`。先把历史 commit 固定为快照，再复用现有 diff、dry-run、安全快照和 restore 路径。

要求：

- commit 必须属于当前 workspace。
- commit 不能早于 `history_floor_commit`。
- 创建后立即保护依赖的 file version、manifest、xattr 和 ACL 历史。
- 损坏、缺失内容和配额问题在创建阶段就失败。
- 第一版 SQLite 可用；PG 返回明确的 capability 错误，不能假装支持。

### P2-2：历史 commit 查看和比较

在 P2-1 稳定后增加：

```bash
vexdb fs workspace show --commit 123
vexdb fs workspace diff --from 123 --to HEAD
```

大 workspace 的输出必须流式或分页；默认只输出变化摘要，不直接输出所有文件。

### P2-3：按时间选择 commit

`--at TIME` 只是“找出不晚于该时间的最后一个 commit”，最终仍转换成 commit ID 和命名快照：

```bash
vexdb fs snapshot create before-noon --at '2026-07-28T12:00:00+08:00'
```

要求明确显示实际选中的 commit 和时间，不能让用户误以为文件系统能恢复到两个 commit 之间。

## 5. PostgreSQL 后续选择

### P3-1：先评估命名快照是否够用

完成 Agent 自动 checkpoint 后，先测：

- 1 千、1 万和长期 workspace 的快照时间。
- snapshot 表增长速度。
- prune、GC、备份和恢复耗时。
- 远程网络下 Agent 启动增加的等待时间。

如果命名快照已经满足 Agent 运行前恢复，不做 PG 逐 commit 历史。

### P3-2：只在真实需求成立时做 PG 任意 commit PITR

触发条件至少满足一项：

- 用户明确需要恢复到没有预先创建快照的历史 commit。
- 自动快照的存储或创建成本无法接受。
- 审计法规要求每次 workspace commit 都可重建。

触发后再设计 PG 的 per-commit inode/dentry/xattr/ACL 状态或不可变 tree manifest，并同时设计 retention、GC、备份和空间上限。不能只增加一个 CLI 参数。

## 6. 当前不排入开发

- DuckDB adapter：已明确不在本方向处理。
- 直接 `restore --commit`：先固定成快照，保证可审计和可回收。
- 真正连续到任意秒的 workspace 恢复：文件系统只有 commit 边界。
- 自动合并两个 Agent 的并发修改：这是协作/版本控制问题，不属于恢复阶段。
- 把 prompt、模型 memory 或 embedding 放入 VexFS 核心。
- 为了 PITR 改回 FSKit 主路线；恢复合同应独立于 NFS、FUSE、FSKit。

## 7. 实施顺序总表

| 顺序 | 需求 | 优先级 | 完成后得到什么 |
|---:|---|---|---|
| 0 | 数据库批量 find（已完成） | P0 | 未挂载时也能高效查文件元数据 |
| 1 | PG 多机恢复屏障 | P0 | 恢复期间不会被其他机器继续写坏 |
| 2 | 恢复前自动安全快照 | P0 | 错误恢复可以撤销 |
| 3 | 统一恢复正确性 Gate | P0 | SQLite、PG、macOS、Linux 不会各自漂移 |
| 4 | workspace log | P1 | 用户能找到和理解历史版本 |
| 5 | 快照分类、保留和 prune | P1 | 自动快照不会无限增长 |
| 6 | `vexdb fs run --snapshot-before` | P1 | Agent 任务前自动留下恢复点 |
| 7 | SQLite `snapshot create --commit` | P2 | 可以固定任意仍被保留的 SQLite commit |
| 8 | commit show/diff | P2 | 先看清差异再选择恢复点 |
| 9 | `--at TIME` 到 commit | P2 | 提供时间入口，但保持真实 commit 语义 |
| 10 | PG 快照规模与长期评估 | P3 | 用数据决定是否做完整 PG PITR |
| 11 | PG 逐 commit PITR | P3 条件项 | 只在真实需求成立时投入 |

## 8. 最近三个开发批次

### 批次 A：恢复安全（数据库与 CLI 已完成）

只做顺序 1～3。结束条件是两台 gateway 仍挂载时恢复被安全拒绝，全部卸载后恢复成功，且可以通过自动安全快照撤销。

完成证据：`docs/reports/2026-07-28_vexfs-pitr-batch-a.md`。

### 批次 B：Agent 闭环

做顺序 4～6。结束条件是用户可以运行一条 `vexdb fs run`，让 OpenCode 修改项目，并用输出中的恢复命令回到任务前。

### 批次 C：SQLite commit PITR

做顺序 7～9。结束条件是用户能从日志选择一个 SQLite commit，固定成快照，查看差异并走统一恢复流程。

PG 逐 commit PITR 不进入这三个批次。
