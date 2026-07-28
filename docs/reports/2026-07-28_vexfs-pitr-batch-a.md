# VexFS workspace PITR 批次 A 实现报告

日期：2026-07-28

## 结论

批次 A 的数据库合同和 CLI 已完成：

1. PostgreSQL 恢复会阻止其他仍有效的 gateway session。
2. mount 注册、heartbeat 和恢复共用 workspace 行锁，关闭了检查与新挂载之间的竞态。
3. SQLite 和 PostgreSQL 在恢复前都会在同一事务内创建自动安全快照。
4. CLI 返回 `safety_snapshot`，并可用它把 workspace 恢复到操作前。
5. 有界 eval 已覆盖并发、恢复撤销、C ABI、SQL 合同和 PG CLI 端到端流程。

正式发行前还要在最终 macOS NFS 与 Linux FUSE 包上复跑真实挂载 Gate。恢复合同不依赖
mount adapter，本次没有改 NFS、FUSE 或 FSKit 实现。

## 关键行为

### PG 多机恢复屏障

- `vexfs_mount_session_start()`、heartbeat 和 restore 都先锁 workspace 行。
- restore 可以排除发起恢复的当前 gateway，但会统计并拒绝其他有效 lease。
- 直接 SQL 调用没有 caller session，因此会拒绝全部有效 mount。
- restore 同时拒绝 open/retained handle。
- 过期 session 不能被旧 heartbeat 重新续租。
- 当前第一版只返回其他活动 session 数量，不自动踢下线，也没有机器名字段。

### 自动安全快照

- CLI 名称格式为 `vexfs-safety-<workspace>-h<head>-<time>-<unique>`。
- 快照创建和恢复处于同一数据库事务。任何配额、快照、并发或内容错误都会整体回滚。
- JSON 输出同时包含目标快照、原 HEAD、新 commit 和安全快照名。
- `--dry-run` 只比较差异，不创建安全快照。

## 自动测试证据

为避免占满本机内存，构建固定使用 `-j2`，eval 使用 quick/单目标模式。

| 测试 | 结果 |
|---|---|
| `vexfs_static_smoke` | PASS |
| `vexfs_runtime_smoke` | PASS |
| `existing.cabi-smoke` | 1 passed，2 checks |
| `contract.workspace-snapshot*` | 2 passed，33 checks |
| `concurrency.workspace-snapshot-restore-race` | 1 passed，5 checks |
| `cli.command-surface` | 1 passed，62 checks |
| PG `pg__vexfs_history_snapshot` spec | 1/1 PASS |
| `vexfs_pg_runtime_smoke` | PASS，144 checks |
| `run_pg_adapter_alpha.sh` | PASS，158 checks |
| `run_pg_runtime.sh` | PASS，42 checks |

PG runtime 的 144 项检查真实建立两个连接：另一 gateway 活动时恢复返回
`VEXFS_MOUNT_BUSY`；关闭另一 gateway 后恢复成功；随后恢复自动安全快照，确认恢复前树可找回。

完整 94 组 PG spec 额外发现两个与 VexFS 无关的现有图索引失败：

- `graph_index_parallel_rabitq_id_cap`：当前构建返回 `RaBitQ quantizer is not yet supported`。
- `graph_index_pq_backup_restore`：PG server 在 PQ backup/restore 用例中异常断开。

VexFS 目标 spec 已单独通过。这两个图索引问题不能被记成批次 A 失败，但合并前仍应由向量索引方向单独处理。

## 后续顺序

下一批进入 Agent 闭环：

1. `vexdb fs workspace log`。
2. 快照分类、保留策略与 prune。
3. `vexdb fs run --snapshot-before -- <agent command>`。

不会在这一阶段做 DuckDB 或 PG 任意 commit PITR。
