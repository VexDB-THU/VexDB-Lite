# VexFS SQLite commit 级恢复报告

日期：2026-07-28

## 结论

SQLite 现在可以从仍被保留的 workspace commit 创建命名快照，并在恢复前分页查看历史目录树、
比较两个 commit。用户也可以用带时区的 RFC3339 时间选择“不晚于该时间的最后一个 commit”。
这不是连续时间恢复；真正的恢复单位仍是数据库 commit。

## 命令

```bash
vexdb fs workspace log --limit 50
vexdb fs workspace show --commit 123 --limit 100
vexdb fs workspace diff --from 123 --to HEAD --limit 100
vexdb fs snapshot create before-bug --commit 123
vexdb fs snapshot create before-noon --at '2026-07-28T12:00:00+08:00'
```

`show/diff` 的 `--after` 是排他路径游标，单页最多 1000 条。普通 `show` 只打印路径；普通
`diff` 打印 `change + path`，有变化时返回 1。需要完整 inode、mode、owner、ACL 和 xattr 状态时
使用全局 `--json`。

## 正确性边界

- commit 必须属于当前 workspace，且不能早于 `history_floor_commit`。
- 创建历史快照前会验证配额、版本引用、manifest、chunk 数量和完整文件 SHA-256。
- 快照创建成功后立即保护对应历史；删除名称后仍由现有 retention 和分批 GC 决定回收。
- `--at` 必须包含 `Z` 或 `+HH:MM`，输出请求时间、实际 commit 和 commit 毫秒时间。
- commit 创建时间使用毫秒精度，同一秒内的多次 Agent 写入不再全部落到同一个时间值。
- PG 没有逐 commit 完整树，相关命令返回 `VEXFS_UNSUPPORTED`，不创建近似结果。

## 回归结果

- 构建：`vexdb_sqlite/build-nfs-dev` 全部目标通过。
- SQLite spec：33 passed，0 failed。
- SQLite C ABI：`VEXFS RUNTIME SMOKE: PASS`。
- CLI：`VEXFS CLI SMOKE: PASS`。
- PG C ABI 合同：151 checks 通过，包含三项 SQLite-only 边界。
- SQLite macOS NFS 真挂载：68 checks，100 小文件 1095.668 files/s，运行前快照 42 ms。
- PG macOS NFS 真挂载：21 checks，成功双快照闭环 1083 ms，运行前快照 110 ms。

### 大树性能 Gate

`run_sqlite_commit_pitr_performance.sh` 直接建立可恢复历史树，输出始终限制为 100 条，并设置
512 MiB RSS 硬上限：

| 文件数 | show 头/深页 | diff 头/深页 | 按时间建快照 | 峰值 RSS |
|---:|---:|---:|---:|---:|
| 10,000 | 46 / 43 ms | 44 / 45 ms | 64 ms | 37,830,656 B |
| 100,000 | 516 / 518 ms | 498 / 513 ms | 786 ms | 159,547,392 B |

初版 1 万文件历史快照约 7.7 秒。性能 Gate 定位到创建阶段为了比较配额而通过当前 dentry
逐项重建整棵树。现在直接使用事务内维护的 `live_files/live_bytes` 和一个最大文件聚合，只重建
目标历史树；版本元数据一次批量读取，同一 manifest 只哈希一次。优化后 1 万文件约 64 ms，
同时保留缺失版本、错误 state size、manifest、chunk 和完整内容哈希检查。
