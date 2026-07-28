# VexFS 快照、备份、保留与 prune 报告

日期：2026-07-28

## 结论

快照、文件版本、format v2 和数据库原生备份不是重复功能，它们处理四种不同的恢复问题，应继续
保留。原 PostgreSQL 实现中“每个快照复制整棵元数据树”的问题已经修复：现在 SQLite 和 PG 的
快照都只是固定 commit 的小引用；PG 使用一个元数据基线和后续增量状态，GC 会压实旧增量链并
推进真实 `history_floor_commit`。

自动 Agent checkpoint 的前置 Gate 已通过，但 `vexdb fs run --snapshot-before -- <command>` 本身
尚未在本次修改中实现。下一步可以开始做这条用户命令，不需要再改快照存储模型。

## 最终合同

| 对象 | 用途 | 是否复制整棵树 |
|---|---|---|
| workspace commit | 有序变更、审计和冲突边界 | 否 |
| 文件版本 | 查看和恢复单个文件 | 只新增不可变内容引用 |
| 元数据 checkpoint | 内部可恢复基线或增量，不是用户命令 | 首个或 GC floor 是基线，其他是增量 |
| 工作区 snapshot | 用户可见的命名恢复点和 GC pin | 否，只保存 commit 引用 |
| format v2 | 单 workspace 离线保存、迁移和克隆 | 输出增量状态，不是全库备份 |
| 数据库原生备份 | 数据库或机器损坏后的全库恢复 | 由数据库自己的备份工具决定 |

### checkpoint 与 snapshot

- checkpoint 是内部持久化状态边界；snapshot 是用户可见名称。
- 同一 commit 可以有多个 snapshot 名称，但只需要一份 checkpoint 状态。
- 首个 checkpoint 保存当前完整可恢复树。
- 后续 checkpoint 只保存自上次边界后变化的 inode、dentry、xattr 和 tombstone。
- ACL 使用不可变、内容寻址的 ACL set；inode 增量只保存 `acl_set_id`，不重复复制 ACL 条目。
- snapshot prune 只删除名称和 pin，不立即猜测哪些正文或元数据可以删除。
- GC 以最老仍保留 snapshot 为新基线；没有 snapshot 时以当前 HEAD 为基线。压实与 floor 更新在
  同一事务完成。

PG 内部权威表为：

- `_vexfs.metadata_checkpoints`
- `_vexfs.inode_states`
- `_vexfs.dentry_states`
- `_vexfs.xattr_states`

`_vexfs.snapshot_inodes`、`snapshot_dentries`、`snapshot_xattrs` 继续存在，但已经是只读兼容视图，
用于恢复、检查和原有 eval，不再是每个快照的物理副本。

### history floor

`history_floor_commit` 表示最早还能重建完整 workspace 元数据树的 commit，不表示最早仍能看到的
日志摘要。第一次创建 checkpoint 时设置 floor；旧 snapshot 被删除后，GC 原子执行：

1. 解析新 floor 的完整状态；
2. 把它写成一份基线；
3. 删除 floor 以前的 checkpoint 状态；
4. 更新 `history_floor_commit`。

旧 commit 和审计摘要可以继续存在，但不能据此宣传任意旧 commit 仍可恢复。

## 用户命令

```bash
vexdb fs snapshot create before-refactor
vexdb fs snapshot create run-start --type agent
vexdb fs snapshot policy show
vexdb fs snapshot policy set --agent-keep 20 --safety-keep 10 --days 30
vexdb fs snapshot prune --dry-run
vexdb fs snapshot prune
```

- `manual`：用户手工快照，不自动清理；
- `agent`：Agent 任务 checkpoint，按数量和天数保留；
- `safety`：恢复前自动创建，按独立数量和天数保留；
- 最新 N 个或最近 D 天满足任一条件即保留；
- `days=0` 关闭天数保护；
- dry-run 和正式 prune 最多返回 100 条候选明细，并返回 `truncated`。

snapshot policy 管命名恢复点，retention 管文件版本。两者不能合成一条内部策略；后续可以增加
只读 `vexdb fs storage status`，统一展示候选、保护原因和预计可回收空间。

## 备份边界

### 四层保护

1. 文件版本：单文件误改。
2. 工作区快照：批量误改和整工作区恢复。
3. 数据库原生备份：数据库灾难恢复，包含业务表和 VexFS。
4. format v2：单 workspace 跨数据库迁移和离线保存。

VexFS 不调度 `pg_dump`、`pg_basebackup`、SQLite Online Backup，也不保存备份密钥。需要“文件系统
一致备份”时，应先同步可写 handle、创建 snapshot，再执行数据库备份或 format v2 export。

可移植 format v2 只包含已发布状态。数据库原生备份只承诺数据库事务一致性；某个后端是否物理
保存未发布 staging 是后端恢复细节，不是跨后端合同。克隆恢复后不能复用源机器的 session、lease
或 gateway 身份。

### 校验命令不合并

不增加含糊的通用 `backup verify`。不同对象使用不同校验：

- 当前 workspace：`vexdb fs check`；
- format v2：`vexdb fs archive verify FILE`；
- PG 逻辑备份：真实 `pg_restore` 后运行 VexFS deep check；
- PG 物理备份：`pg_verifybackup` 加真实恢复演练；
- SQLite 原生备份：打开备份副本并运行 VexFS deep check。

## 实现结果

### PG 快照写入

快照创建先锁 workspace，再把 dirty key 写入当前 commit 的状态表。创建 snapshot 只新增一行引用。
单文件写入通常只产生文件 inode 和父目录 inode 两条状态；没有变化的 dentry、xattr 和 ACL 不写。
从未建立 checkpoint 的工作区第一次执行 GC 时，HEAD capture 已经是完整基线；GC 会重新读取最新
floor，不再使用调用前的旧值重复压实整棵树。

恢复时只展开一次目标 overlay，写入事务内临时表。完整性检查、相等比较、ACL、内容版本别名和最终
替换复用同一份结果，避免恢复流程多次扫描同一棵大树。

### format v2

PG 导出不再把每个 snapshot 的完整树装入临时表，也不再生成“checkpoint × 所有 key”的网格。
目标 HEAD 或指定 snapshot 只解析一次；历史状态直接从增量状态表输出。外部 format v2 结构未变，
SQLite 与 PG 仍可互相导入。

### 诊断

`doctor --json` 的 `database.recovery` 现在包含：

- `snapshot_count`
- `metadata_checkpoint_count`
- `history_floor_commit`
- `content_history_bytes`
- `metadata_history_bytes`
- `protected_history_bytes`
- `reclaimable_bytes`
- `oldest_recovery_commit`
- `oldest_recovery_created_at`

其中 `protected_history_bytes` 已包含正文保护字节与 checkpoint 状态行的逻辑字节，不再只统计 chunk。

## 真实测试结果

### 大工作区 × 多快照

脚本：`tests/eval/vexfs/run_pg_snapshot_checkpoint_performance.sh`

环境：PG19、1 GiB `memory.max`、10,000 个文件、30 个快照，每次只改一个文件。

| 指标 | 结果 |
|---|---:|
| 首个基线快照 | 115.698 ms |
| 后续增量快照 P95 | 3.317 ms |
| format v2 记录流导出 | 454 ms |
| 恢复到最老快照 | 992 ms |
| 删除 29 个快照后的元数据压实 | 426 ms |
| 实际 inode state 行 | 10,069 |
| 原整树复制等价行 | 300,330 |
| 状态行压缩比 | 约 29.8 倍 |
| OOM kill | 0 |

压实后 `history_floor_commit` 已推进到唯一保留快照的 commit，floor 以前状态行归零；随后从这个
快照恢复出的文件内容正确。优化前，同一 10,000 文件恢复约 57.9 秒；目标树只解析一次后降到
约 0.99 秒。

当前明确边界：`vexfs_gc(..., batch)` 的 `batch` 只限制文件版本回收；metadata floor 必须原子
推进，所以一次压实会重写新 floor 的完整元数据基线。10,000 文件实测 426 ms，可接受；10 万和
百万文件尚未形成正式预算，不能据此宣传线性扩展无停顿。

### 备份性能

脚本：`tests/eval/vexfs/run_pg_backup_performance.sh`

环境：1 GiB 容器、16 MiB 正文、16 个文件。

| 路径 | 结果 |
|---|---:|
| `pg_dump` | 139.130 MiB/s |
| `pg_restore` | 128.000 MiB/s |
| `pg_basebackup` | 28.902 MiB/s |
| format v2 export | 55.172 MiB/s，CLI RSS 16,498,688 B |
| format v2 import | 21.333 MiB/s，CLI RSS 17,940,480 B |

23 项检查通过，`oom_kill=0`。

### 功能和恢复

- PostgreSQL VexFS spec：15/15；
- 新增 checkpoint 基线、同 commit 多 snapshot、首次 GC 不重复压实、GC floor 推进、压实后恢复 spec；
- SQLite → PG → SQLite format v2 往返：6 项通过；
- PG 逻辑备份恢复：45 项通过；
- PG 物理备份恢复：3 组、29 个字段通过；
- 10 万 snapshot 目录记录 policy/dry-run/prune 测试继续保留，它只衡量目录策略查询，不再被当作
  真实快照写入性能。

## 后续顺序

1. 实现 `vexdb fs run --snapshot-before -- <command>`，失败和退出码合同进入 eval。
2. 给 SQLite 补同样明确的元数据 floor/GC 指标。
3. 做 SQLite 任意保留 commit 固定为 snapshot；floor 以前必须拒绝。
4. 增加统一只读 `storage status`，不增加第三套 retention 策略。
5. 发版前在最终 macOS/Linux 挂载包复跑 Agent workspace、snapshot、restore 和备份文档流程。
