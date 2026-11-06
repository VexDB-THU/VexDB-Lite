# VexFS PostgreSQL 批量历史优化报告

日期：2026-07-28

## 结论

逐文件生成 commit、manifest、ACL、审计和 workspace 更新的问题已经完成两轮优化。文件仍然各自
保留 version 和路径级变化记录，但一个批次只推进一次 workspace HEAD、只写一条 commit、
只写一条 audit、只发一次通知。相同 ACL 变成不可变集合，inode 和快照只保存集合引用。
1,000 个带继承 ACL 的空文件，真实 PG19 对照从 1,331 ms 降到 328 ms，提升 4.06 倍；
10 万文件通过 100 个有界事务完成，用时 10.232 秒。

这不是删除历史。新的 `commit_changes` 保存每个路径的操作、inode、前后版本和详细信息；
`commits` 只保存批次级提交头。空文件各自仍有 version 1，但同一 workspace 共享一个不可变
空 manifest。

## ACL 集合和写时复制

- `_vexfs.acl_sets` 保存规范化 JSON、SHA256 指纹和条目数；相同 workspace 内相同 ACL 只存一次。
- `_vexfs.acl_set_entries` 保存真正的权限明细；inode 和 snapshot inode 只保存 `acl_set_id`。
- `_vexfs.acl_entries` 与 `_vexfs.snapshot_acl_entries` 保留为只读兼容视图，权限查询、导出格式和
  检查接口不需要改变。
- 父 ACL 全部可继承时，子 inode 直接复用父集合；只有部分条目可继承时，只创建一次子集集合。
- `set/grant/revoke/delete` 先锁单个 inode，再生成或复用新集合，最后切换引用；不会修改共享集合。
- ACL commit change 记录 `before_acl_set` 和 `after_acl_set`，旧集合继续被快照和历史引用。
- 相同 ACL 的幂等调用不推进 workspace HEAD，也不改写已有集合头。
- 16 个真实 PG 连接并发写入同一个 ACL 内容时，只生成 1 个集合和 2 条权限明细。

## 已完成的实现

### 提交和历史

- 增加 `_vexfs.commit_changes`，把批次提交头与逐路径变化分开。
- 原有单文件函数仍保持“一次调用一个 commit”的行为，并自动写一条 change。
- `vexfs_create_batch(workspace, parent, entries)` 支持同一父目录 1～1,000 个文件或目录。
- 批量创建一次完成名称、类型、mode、路径长度、冲突、权限和 quota 校验；任一项失败全部回滚。
- workspace HEAD、cache generation、父目录时间、audit 和通知每批只更新一次。
- inode、dentry、file version、继承 ACL 和 commit change 使用集合写入。

### 挂载发布

- `vexfs_mount_publish_close_all` 的一个刷盘批次只生成一个 `publish_batch` commit。
- `vexfs_mount_publish_close_claimed` 的最多 64 个 generation claim 共用一个 commit。
- 每个文件继续生成独立 file version 和 commit change。
- 已发布 generation 的重复 claimed 调用直接返回原结果，不新增 commit。
- 批内任一文件冲突或失败时，提交头、version、change 和句柄状态一起回滚。

### 内容和归档

- manifest 和 chunk 不再绑定单个 inode，为内容复用留下统一数据模型。
- 同一 workspace 的所有空文件共享一个 canonical empty manifest。
- format v2 归档新增 `commit_changes`，校验 checksum、commit/inode 引用和 change 序号。
- PG 导出保留完整 change；PG 导入完整恢复 change。
- SQLite 来源暂时没有这张明细表，导入 PG 时为每个 commit 写明确的
  `imported_without_change_detail` 标记，不伪造路径级信息。
- SQLite → PG → SQLite 和 PG → SQLite 的真实双向归档测试通过。

### 热点更新

- live file/byte quota 计数从逐 inode 行触发改为 statement 级聚合更新。
- grep 索引维护改为 statement 级接收新旧行，再只刷新实际受影响的 inode。
- 为 inode、commit、manifest 和 workspace 的外键子表补齐前导索引，避免大 workspace
  级联删除时按每个 inode 反复扫描 10 万行。
- grep 已启用和未启用两条路径都由现有 PG spec 覆盖。

## 性能结果

环境：本机 Docker 中的 PostgreSQL 19 测试容器，单连接串行执行，批次大小 1,000，
  `work_mem=16MB`，`temp_file_limit=512MB`。脚本监控创建和 workspace 删除两个阶段的整个
容器 cgroup memory，超过 1 GiB 会主动终止，不会继续扩大规模。

| 路径 | 文件数 | 批次数 | 创建耗时 | 清理耗时 | 吞吐 | commits | changes | versions | manifests | ACL sets/entries | 容器峰值内存 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 原单文件接口 | 1,000 | 1,000 | 1,331 ms | 334 ms | 751.31/s | 1,002 | 1,002 | 1,000 | 1 | 1 / 1 | 567,619,584 B |
| 批量接口 | 1,000 | 1 | 328 ms | 335 ms | 3,048.78/s | 3 | 1,002 | 1,000 | 1 | 1 / 1 | 563,863,552 B |
| 批量接口 | 10,000 | 10 | 839 ms | 581 ms | 11,918.95/s | 12 | 10,002 | 10,000 | 1 | 1 / 1 | 579,420,160 B |
| 批量接口 | 100,000 | 100 | 10,232 ms | 2,342 ms | 9,773.26/s | 102 | 100,002 | 100,000 | 1 | 1 / 1 | 726,503,424 B |

表中的 commit/change 数包含 workspace 创建时的第一条记录。10 万规模的峰值是整个 PG
容器的内存和文件页缓存，不是单个 SQL 后端 RSS；测试前容器已有较多累积缓存，最终
峰值低于 1 GiB 保护线，但高于原调研中提出的
512 MiB 理想线。下一轮如果要把 512 MiB 作为硬门槛，应先拆分共享缓冲和页缓存指标，再决定
是降低批次、主动 checkpoint，还是只限制进程 RSS。

第一次 10 万测试还发现 workspace 删除运行超过 9 分钟且后台继续占锁。根因是 inode、commit、
manifest 等外键的子表缺少以前导外键列建立的索引，级联删除反复全表扫描。补齐这些索引后，
同一 10 万 workspace 的本轮完整删除为 2.342 秒；eval 现在对 cleanup 也设置 2 分钟超时和内存线。

## 自动测试证据

| 测试 | 结果 |
|---|---|
| 当前 SQL 在干净 PG19 数据库 `CREATE EXTENSION` | PASS |
| `pg__vexfs_batch_create` | PASS |
| 全部 `pg__vexfs_*` spec | 12/12 PASS |
| 1,000 inode ACL 集合复用、快照和写时复制 spec | PASS |
| 16 连接 ACL 集合并发去重与幂等 eval | PASS，OOM kill 0 |
| claimed 两文件一个 commit + 连续 change ordinal | PASS |
| claimed 重放不推进 HEAD | PASS |
| publish-close-all 两文件一个 commit | PASS |
| SQLite ↔ PG format v2 双向归档 | PASS，6 checks |
| PG adapter 事务、并发和恢复 eval | PASS，160 checks |
| `pg_dump` / `pg_restore` ACL 集合与快照恢复 | PASS，45 checks |
| CLI C++ Release 目标（`vexfs_cli`、`vexdb_cli`，`-j2`） | PASS |
| 1k/10k/100k 批量性能与内存 Gate | PASS |

性能 eval 固化在 `tests/eval/vexfs/run_pg_create_batch_performance.sh`，默认运行旧路径 1,000
文件控制组和新路径 1k/10k/100k，并校验至少 2 倍加速、ACL 集合数量、数据结构数量、
quick/deep check 和内存。并发 eval 位于 `tests/eval/vexfs/run_pg_acl_cow_concurrency.sh`。

## 仍未完成的边界

- `vexfs_create_batch` 第一版只支持同一父目录；多父目录需要固定锁顺序。
- 普通 Bash create 的“立即远端可见”仍按现有挂载合同执行；本次优化的是 publisher 已聚合的
  close/idle 批次，没有偷偷降低 strict durability。
- 大型 archive import 已经整体事务可见，但还没有通用隐藏 generation 切换。
- SQLite 还没有与 PG 相同的 `commit_changes` 明细；跨引擎导入不会伪造不存在的历史。
- 当前项目未发版，本次直接调整当前 schema，没有增加旧 schema 迁移代码。

## 下一步建议

1. 给批量 API 增加 request id，补“服务端已提交但客户端断线”的整批幂等重试。
2. 给普通 NFS create/mkdir/rename/unlink 增加有界微批队列，分别验证普通写回与 strict 模式。
3. 记录 WAL 增量、锁等待、通知数和 replica lag，再与 TigerFS/AgentFS 做同机同规模对照。
4. 如果必须把容器总内存压到 512 MiB，再建立 shared buffers、backend RSS 和 page cache 的拆分指标。
