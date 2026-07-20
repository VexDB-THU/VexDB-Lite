# VexFS Eval

这套 eval 使用真实构建产物和磁盘 SQLite 数据库，不使用 mock。每个用例独立记录
结果、耗时、断言数和性能指标，失败后仍继续执行其他用例。

## 运行

```bash
# 默认 full：构建后覆盖功能、恢复、并发、备份、性能和 FSKit 编译
bash build_sqlite.sh eval

# 日常快速回归
bash build_sqlite.sh eval quick

# 最大规模（128 MiB 文件、1 万随机操作、1 万小文件）
bash build_sqlite.sh eval stress

# 只跑某一类或某个用例
python3 tests/eval/vexfs/run.py --mode full --filter recovery
python3 tests/eval/vexfs/run.py --mode full --filter performance.staged-overwrite

# 把性能预算也作为硬 Gate；默认只记录指标，避免不同机器误报
python3 tests/eval/vexfs/run.py --mode full --enforce-performance
```

报告生成在：

```text
vexdb_sqlite/build/eval/vexfs/<run-id>/report.json
vexdb_sqlite/build/eval/vexfs/<run-id>/report.md
vexdb_sqlite/build/eval/vexfs/latest.json
```

随机状态机使用固定 seed，可通过 `--seed` 复现或扩充样本。

## 覆盖范围

- 现有 Gate：SQL 静态注册、C ABI、CLI smoke、全部 SQLite YAML spec。
- 文件合同：二进制/Unicode、元数据、路径校验、rename replace、递归删除、公开版本历史和指定版本读取。
- 版本恢复：SQL/C ABI/CLI restore、expected-version 冲突、事务回滚、dry-run 不写库、文本 diff 和末尾换行差异。
- 事务：commit、rollback、savepoint、读者快照、目录 verifier。
- 句柄：flags、稀疏写、truncate、幂等指纹、同步、乐观写冲突。
- 恢复：进程中途退出、WAL 重开、integrity check、旧 schema 迁移、retained reclaim。
- 并发：多个真实进程写入、数据库锁和 busy timeout。
- 备份：运行中的 SQLite online backup、恢复、未发布 staging 可见性。
- 随机模型：真实数据库状态与 Python 参考文件树持续比对。
- 性能：大量小文件、大文件顺序读写、分块 staging、随机 4 KiB 覆盖、备份吞吐。
- 空间：版本历史增长、幂等请求行数、checkpoint/VACUUM 前后的 DB/WAL 体积。
- 平台：FSKit App/extension 无签名编译；扩展已启用时执行真实 mount 和 bash 命令。

## 模式规模

| 项目 | quick | full | stress |
|---|---:|---:|---:|
| 随机模型操作 | 300 | 2,000 | 10,000 |
| 小文件 | 250 | 3,000 | 10,000 |
| 顺序文件 | 8 MiB | 100 MiB | 128 MiB |
| staging 文件 | 8 MiB | 100 MiB | 128 MiB |
| 随机 4 KiB 写 | 100 | 1,000 | 5,000 |
| 并发进程 | 4 | 8 | 8 |

`mount.real-bash` 是环境 Gate。macOS、FSKit 和已启用的 VexFS 系统扩展都满足时，
它会真实挂载并运行 `mkdir`、`cat`、`grep`、`cp`、`mv`、`find`、`rm` 等命令；
条件不满足时报告会明确写 `SKIP` 和原因，不会把未执行伪装成通过。
