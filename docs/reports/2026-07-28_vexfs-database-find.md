# VexFS 数据库批量 find 实现与测试报告

- 日期：2026-07-28
- 分支：`feature/agent_files`
- 基线提交：`90b29ec080`（本报告对应未提交工作区修改）
- 范围：SQLite、PostgreSQL、公共 C ABI、`vexdb fs find`、功能和性能 eval
- 不包含：DuckDB、系统挂载目录上的原生 `find` 加速、文件正文搜索

## 1. 结论

数据库批量 `find` 已完成。它是数据库管理的文件元数据查询，不要求先 mount，也不会读取文件
正文。SQLite 和 PostgreSQL 共享以下用户合同：

- 根路径；
- 名称 glob：`*` 和 `?`，其中 `?` 匹配一个 Unicode 字符；
- 类型：file、directory、symlink；
- 最小/最大大小；
- 修改时间起止毫秒；
- 排他的路径游标；
- 1～1000 的页大小；
- 按二进制路径稳定排序的 JSON 结果和 `next_cursor`。

CLI 文本模式每行输出一个路径，`--json` 输出完整结构。PG 普通 role 的递归会在无读取权限的
目录前停止，并过滤无读取权限对象；workspace owner 和 superuser 走快速路径。

## 2. 实现要点

最初的递归 CTE 把所有文件也放进递归队列。SQLite 1 万文件首屏查询达到 26.527 秒，这是
真实问题。修复后，递归队列只枚举目录，再一次连接每个目录的直接子项。相同 1 万文件首屏降到
约 20 毫秒，且不读取正文。

SQLite 名称匹配使用 UTF-8 字符边界，避免 `?` 把一个中文字符当作三个字节。PostgreSQL 使用
等价的字符级正则。PG ACL 只在普通 role 路径执行逐对象检查，避免 owner 基准被无意义拖慢。

## 3. 最新性能结果

测试都使用真实数据库文件或真实 PG 容器，页大小 100；10 万规模期间没有扩大并发。

| 后端 | 文件数 | 夹具生成 | 首屏 | 第二页 | 精确名称 | 数据库/RSS |
|---|---:|---:|---:|---:|---:|---:|
| SQLite | 10,000 | 6.340 s | 19.814 ms | 18.238 ms | 21.595 ms | DB 9.97 MB，RSS 22.00 MB |
| SQLite | 100,000 | 65.711 s | 205.268 ms | 200.127 ms | 235.201 ms | DB 101.76 MB，RSS 93.78 MB |
| PostgreSQL | 10,000 | 1.987 s | 22.106 ms | 22.042 ms | 11.771 ms | 容器内批量元数据夹具 |
| PostgreSQL | 100,000 | 173.883 s | 78.709 ms | 83.903 ms | 10.203 ms | 容器内批量元数据夹具 |

PG 性能脚本直接批量生成合法的 live inode/dentry 树，用于隔离 `find` 查询成本。公开
`vexfs_create` 在 10 万文件时会同时生成 commit、manifest、版本和 ACL 历史，单轮超过 15 分钟，
因此不能当作查询基准的夹具生成器。这是另一个真实写入性能任务，不计入 `find` 查询耗时。

## 4. 已通过测试

- SQLite 静态合同：`VEXFS STATIC SMOKE: PASS`；
- SQLite runtime：`VEXFS RUNTIME SMOKE: PASS`；
- CLI：69 checks；
- SQLite 1 万/10 万性能：各 10 checks，RSS 小于 512 MiB；
- PG find spec：名称、类型、大小、时间、游标、Unicode、ACL；
- PG adapter：160 checks；
- PG C ABI runtime：148 checks；
- PG CLI runtime：45 checks；
- PG 1 万/10 万性能门槛均通过。

SQLite 原始报告：

- `vexdb_sqlite/build/eval/vexfs/20260728T050841.598754Z-full-20260718/report.json`
- `vexdb_sqlite/build/eval/vexfs/20260728T050855.805263Z-stress-20260718/report.json`

## 5. 后续任务

1. 优化 PG 大批量创建，避免逐文件生成历史时耗时线性放大到不可接受的分钟级。
2. 做 workspace log、快照分类/保留和 `run --snapshot-before`。
3. 再做 SQLite 任意 commit 固定、show/diff 和按时间选择 commit。
4. PG 逐 commit PITR 仍是条件项，不因 `find` 完成而提前投入。
