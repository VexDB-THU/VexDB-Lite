# VexFS 统一 workspace log 完成报告

日期：2026-07-28
分支：`feature/agent_files`

## 结论

SQLite 和 PostgreSQL 已提供同一套 workspace commit 日志。用户可以在不挂载目录、不读取文件
正文的情况下，从新到旧查看操作、路径、actor、session/run ID 和对应快照，并用排他游标读取
下一页。

```bash
vexdb fs workspace log
vexdb fs workspace log --limit 50 --before 123
vexdb fs --json workspace log --limit 50
```

## 数据合同

每条记录包含：

- `commit`、`parent_commit`、`created_at`；
- `operation`、`path`、`actor`；
- `session_id`、`run_id`；
- `has_snapshot`、`snapshots`。

`next_before` 非空时可直接传给下一次 `--before`。游标是排他的，因此翻页不会重复上一页最后
一条记录。`NULL` 和 `0` 都表示从 HEAD 开始。单页限制为 1～1000 条。

PG 在服务端继续执行 workspace `read` 权限检查。跨引擎导入时保留操作、路径和 session/run
元数据；actor 会映射为目标数据库的执行身份，避免导入包伪造本地权限身份。

## 实现边界

- 这是文件系统提交日志，不等同于一次 Agent 调用；Agent 任务边界由后续 `vexdb fs run` 提供。
- 核心不保存 prompt、模型 memory 或其他语义内容。
- SQLite 从本次提交的 dirty inode/dentry 选一个稳定主路径；一次批量提交的完整逐路径变化仍以
  现有 change 记录为准，workspace log 只显示摘要主路径。
- 查询只读取 commit 与 snapshot 元数据，不扫描文件正文。

## 自动测试

- SQLite 全量 spec：31/31；
- PostgreSQL VexFS 全量 spec：13/13；
- SQLite runtime smoke、static smoke、CLI smoke：全部通过；
- PG CLI/runtime：49 项检查通过；
- SQLite→PG→SQLite format v2 往返：6 项检查通过，包含 commit path 和 workspace log；
- 显式 `NULL` 首页游标已在 SQLite 和 PG spec 中覆盖。

## 10 万 commit 性能 Gate

测试只建立 commit 元数据，不写入文件正文；单页 100 条，头页和深页查询预算均为 2 秒。

| 引擎 | 头页 | 深页 | 内存保护 |
|---|---:|---:|---:|
| SQLite | 0 ms | 3 ms | 峰值 RSS 小于 512 MiB |
| PostgreSQL | 157 ms | 158 ms | 587,739,136 B；容器上限 1 GiB；无 OOM |

原始结果：`build/eval/vexfs/workspace_log_performance_100000.tsv`。

## 下一项

先实现 `manual`、`agent`、`safety` 快照分类，再实现保留策略与
`snapshot prune --dry-run`。这样后续 Agent 自动 checkpoint 才不会让快照无限增长。
