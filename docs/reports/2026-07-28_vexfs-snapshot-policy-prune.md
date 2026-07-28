# VexFS 快照分类、保留与 prune 报告

日期：2026-07-28

## 结论

SQLite 和 PostgreSQL 已共用同一套快照分类与清理合同。自动 checkpoint 现在有明确的增长上限，
同时不改变现有 workspace commit、文件版本、manifest 和 GC 模型。下一项可以直接实现
`vexdb fs run --snapshot-before -- <command>`，不需要再增加第二套版本系统。

## 用户合同

```bash
vexdb fs snapshot create before-refactor                 # manual
vexdb fs snapshot create run-start --type agent
vexdb fs snapshot policy show
vexdb fs snapshot policy set --agent-keep 20 --safety-keep 10 --days 30
vexdb fs snapshot prune --dry-run
vexdb fs snapshot prune
```

- `manual`：用户手工快照，永不自动清理；
- `agent`：Agent 任务 checkpoint，按 `agent-keep` 和天数保留；
- `safety`：恢复前自动生成，按 `safety-keep` 和天数保留；
- 数量和天数是“或”关系：最新 N 个保留，最近 D 天也保留；
- `days=0` 关闭天数保护，只按数量判断；
- dry-run 和正式 prune 最多返回 100 条候选明细，并用 `truncated` 表示是否截断。

## 存储边界

快照仍引用现有 commit/树状态，没有复制文件正文，也没有新增 Agent 专用版本表。prune 只删除
过期的 `agent`、`safety` 快照引用；被引用的内容继续受保护。删除引用后，已有 retention/GC
合同判断哪些文件版本、manifest 和 chunk 可以回收，并显式分批删除。

format v2 逻辑包会携带三个策略值和每个快照的类型，因此 SQLite 与 PG 往返不会把自动快照
误变成手工快照。`doctor --json` 的 `database.recovery` 返回：

- `snapshot_count`
- `protected_history_bytes`
- `reclaimable_bytes`
- `oldest_recovery_commit`
- `oldest_recovery_created_at`

## 正确性证据

- SQLite 全部 spec：32/32；
- PostgreSQL 全部 VexFS spec：14/14；
- SQLite CLI smoke：通过；
- PostgreSQL libpq runtime：57 项通过；
- SQLite → PG → SQLite format v2 往返：6 项通过；
- 8 个 typed create 与 4 个 prune 并发：SQLite、PG 均通过，最终保留
  1 manual、5 agent、5 safety，deep check 通过；
- 同一 SQL 重复执行 dry-run 的回归已覆盖。SQLite 候选集改为纯 CTE，不再依赖会干扰当前
  语句的临时表。

## 十万快照性能与内存

测试脚本：`tests/eval/vexfs/run_snapshot_policy_performance.sh`。

| 引擎 | 快照数 | policy | dry-run | prune | 内存 |
|---|---:|---:|---:|---:|---:|
| SQLite | 100,000 | 100 ms | 281 ms | 356 ms | 峰值 24,215,552 B |
| PostgreSQL | 100,000 | 389 ms | 706 ms | 1,601 ms | 测试后 589,504,512 B |

PG 容器 `memory.max` 不超过 1 GiB，测试前后 `oom_kill` 没有增加。SQLite 的硬上限是 512 MiB，
实际峰值约 23.1 MiB。两个引擎都满足单次查询/清理 5 秒预算。

一万快照预检也通过：SQLite 为 12/33/36 ms，PG 为 124/135/220 ms。

## 后续任务

按路线图进入 P1-3：实现通用 `vexdb fs run --snapshot-before -- <command>`。它只负责创建
`agent` 快照、记录通用 run 边界、透明转发终端和退出码，并输出恢复命令；不分析 prompt，
不把模型语义写进文件系统核心。
