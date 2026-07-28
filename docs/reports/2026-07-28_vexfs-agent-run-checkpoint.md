# VexFS Agent 运行前 checkpoint 实现与真挂载报告

日期：2026-07-28

## 结论

`vexdb fs run --snapshot-before -- <command>` 已在 SQLite 和 PostgreSQL 后端完成，并通过当前
Mac 的真实 NFS 挂载测试。它不是 Agent SDK，也不识别 prompt；它只是数据库文件系统提供的
通用命令边界：先留下可恢复快照，再原样启动终端程序。

默认只创建运行前快照。需要任务成功后的第二个边界时，显式增加：

```bash
vexdb fs run --snapshot-before --snapshot-after-success -- npm test
```

## 已实现合同

1. 当前目录必须位于匹配 backend、connection 和 workspace 的活动挂载目录内。
2. 子程序启动前创建 `agent` 类型的一致快照；不会退化成可能漏文件的 `committed-only`。
3. 挂载端有已关闭但未发布的写入时最多等待 35 秒；仍未完成则返回 busy，子程序不启动。
4. `--` 后的参数完全停止 VexDB 全局解析，`--json`、`--backend` 等可以安全传给子程序。
5. macOS/Linux 使用 `fork` + `execvp`，不把参数重新拼成 shell 字符串。
6. 子程序继承 stdin、stdout、stderr，普通退出码保持不变；SIGINT、SIGTERM、SIGHUP 会被转发。
7. 快照名持久保存唯一 run ID，JSONL 生命周期事件包含开始时间、命令类型、workspace、挂载点、
   快照和退出码。数据库继续保存它自己的快照创建者和创建时间。
8. 子程序启动前把恢复命令写到 stderr。SQLite 使用规范化数据库路径；PG 只引用
   `$VEXDB_PG_DSN`，不会打印原始 DSN 或密码。
9. `--snapshot-after-success` 只在子程序返回 0 时创建完成快照。子程序失败、中断或不存在时，
   运行前快照仍保留。

`run_id` 表示一次 CLI 任务边界。当前没有把它强行写入挂载 gateway 产生的每个文件 commit，
因为 CLI 管理连接与真实挂载连接是两个进程；假装二者天然是同一数据库 session 会造成错误审计。
恢复点与任务的可靠关联由带 run ID 的 agent 快照提供。

## 自动用例

### 普通 CLI

```bash
bash agent_files/cli/test/vexfs_cli_smoke.sh vexdb_sqlite/build-nfs-dev/vexfs
```

结果：`VEXFS CLI SMOKE: PASS`。

新增检查确认未挂载目录拒绝运行，并确认子程序的 `--json` 不会泄漏到 VexDB 全局选项。

### SQLite 真实 macOS NFS 挂载

```bash
VEXDB_NFS_FILE_COUNT=100 \
  bash tests/eval/vexfs/run_macos_nfs_mount.sh vexdb_sqlite/build-nfs-dev/vexdb
```

结果：`PASS (68 checks)`。

覆盖：

- 子程序参数、run 环境变量、stdout 和写文件；
- before/after 两个 agent 快照；
- 成功任务恢复；
- 失败任务返回 23、不创建 after 快照、仍可恢复；
- Ctrl-C 返回 130，before 快照仍存在；
- 运行前快照加 `/usr/bin/true` 总耗时 59 ms；
- 100 个小文件写入 486.685 files/s。

当前 Mac 的 NFSv3 原生挂载仍不支持 mounted xattr，脚本明确输出
`macos-nfsv3-mounted-xattr`，这不是本次 run 命令新增的退化。

### PostgreSQL 真实 macOS NFS 挂载

```bash
VEXDB_PG_DSN=postgresql://postgres@127.0.0.1:5433/test \
VEXDB_PG_CONTAINER=vexdb_pg19-test \
  bash tests/eval/vexfs/run_pg_agent_checkpoint.sh vexdb_sqlite/build-nfs-dev/vexdb
```

结果：`PASS (21 checks)`。

覆盖成功写入、before/after、恢复、失败退出码 29、不创建 after、失败恢复、子程序
`--backend` 参数透传，以及生命周期输出不包含原始 DSN。运行前快照加 `/usr/bin/true` 为
110 ms；包含 before、一次小文件写入等待发布和 after 的成功闭环为 1,083 ms。

## 性能解释

默认命令只有运行前快照，不会承担 1,083 ms 的双快照闭环成本。本机 SQLite 为 42 ms，回环 PG
为 110 ms。显式 `--snapshot-after-success` 后，如果子程序刚写完文件，VexDB 需要等待 NFS gateway
的 500 ms 空闲发布窗口，再创建完成快照，因此本轮 PG 小任务总耗时约 1.1 秒。这是有意的完整性
等待，不是扫描或复制整棵 workspace。

## 剩余边界

- 已验证 Ctrl-C；数据库断网、gateway crash 和 PG restart 继续由现有故障 eval 覆盖，尚未把这些
  故障再次嵌入本条 CLI 专项脚本。
- Windows 还没有真实 WinFsp 挂载，因此 `run` 在 Windows 构建可用，但活动 workspace 校验会在
  WinFsp 适配完成前拒绝运行。
- JSON 生命周期事件写 stderr，子程序自己的 stdout/stderr 保持原样，因此调用方应按行识别
  `event`，不能假设整个 stderr 只含 JSON。
