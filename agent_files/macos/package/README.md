# VexDB-Lite macOS 技术预览

完整的安装、SQLite、向量、文件版本、挂载、备份和卸载说明见同目录
[`使用说明.md`](使用说明.md)。

VexDB-Lite 在一个安装包中提供：

- `.payload/VexDB Lite.app`：可选的 VexFS FSKit 文件系统扩展宿主；
- `bin/vexdb`：内置 SQLite、向量检索和文件 SQL 能力的统一命令；
- `bin/vexfs`：`vexdb fs` 的兼容快捷入口；
- `bin/vexfs-nfs-gateway`：macOS 默认真实挂载使用的本机 NFS gateway；
- `lib/vexdb_lite.dylib`：供现有 SQLite 程序加载的扩展；
- 安装和卸载脚本。

## 系统要求

- 默认 NFS、CLI 和 SQLite 扩展支持 macOS 13.0 或更高版本；
- Apple Silicon 包要求 arm64 Mac；
- 默认真实挂载使用 macOS 自带的 NFS client，不要求安装驱动或打开系统扩展开关；
- FSKit 是 macOS 26.0+ 的可选 driver。只有显式使用 `--mount-driver fskit` 时才需要 macOS 允许
  `VexDB Lite` 文件系统扩展。

## 安装

双击 `Install.command`，或在终端运行：

```bash
./install.sh
```

正式包必须是 `signature=developer-id`。本地构建的 `signature=ad-hoc` 包默认不能安装；
仅在开发测试时显式运行 `VEXDB_LITE_ALLOW_ADHOC_INSTALL=1 ./install.sh`。

默认安装位置：

- App：`~/Applications/VexDB Lite.app`
- CLI：`~/.local/bin/vexdb`
- 文件快捷命令：`~/.local/bin/vexfs`
- SQLite 扩展：`~/.local/lib/vexdb-lite/vexdb_lite.dylib`
- 默认数据库：`~/Library/Application Support/VexDB-Lite/default.sqlite3`

安装程序会用默认 NFS driver 执行一次临时真实挂载；只有 gateway 启动、写入、读回和
卸载都成功才报告完成。App payload 放在隐藏目录中，避免解压目录和安装目录同时被
macOS 注册为两个同名 FSKit 模块。FSKit 未授权不影响默认 NFS。
升级时，旧 App、CLI、SQLite 扩展和 PostgreSQL runtime 只放在隐藏事务目录中；任何一步失败
都会恢复旧版本，成功后立即删除临时备份，不会在 Applications 中留下可被系统再次识别的
`.app.disabled` 副本。如果 macOS 要求新签名版本重新授权，安装程序会明确提示，不会静默跳过。
安装或升级前请先卸载正在使用的 VexFS。安装程序不会重启 `pkd`、清空 LaunchServices
注册库或修改其他 App 的扩展状态。

如果 `~/.local/bin` 不在 PATH：

```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

## SQLite 和向量

`vexdb` 自带经过验证的 SQLite，不需要手动 `.load`：

```bash
vexdb ~/data/agent.db
vexdb ~/data/agent.db "SELECT vexdb_version();"
vexdb ~/data/agent.db "SELECT vexdb_l2_distance('[1,2]', '[4,6]');"
```

## 文件管理

不挂载也能使用：

```bash
vexdb fs setup
printf 'hello VexFS\n' | vexdb fs write /hello.txt
vexdb fs cat /hello.txt
vexdb fs ls /
vexdb fs grep -n hello /
vexdb fs index enable       # 可选的 FTS5 trigram 文本索引
vexdb fs history /hello.txt
vexdb fs snapshot create before-agent
vexdb fs snapshot create task-start --type agent
vexdb fs snapshot diff before-agent
vexdb fs snapshot restore before-agent --dry-run
vexdb fs snapshot restore before-agent
vexdb fs snapshot policy show
vexdb fs snapshot policy set --agent-keep 20 --safety-keep 10 --days 30
vexdb fs snapshot prune --dry-run
vexdb fs snapshot prune
vexdb fs check              # 深度检查结构、历史和内容 SHA-256
vexdb fs check --quick      # 只检查结构和引用
vexdb fs quota show         # 查看 live 文件数、字节和上限
vexdb fs retention show     # 查看保留策略和可回收历史
vexdb fs gc --batch 1000    # 显式分批清理无引用历史
vexdb fs export --snapshot before-agent --output workspace.vexfs
vexdb fs archive verify workspace.vexfs
```

已挂载 workspace 会在恢复时自动安全卸载并挂回原目录；正常卸载失败时不会开始恢复。
`--force-unmount` 只用于用户明确接受中断打开文件的场景。

手工快照类型是 `manual`，不会被自动清理；Agent checkpoint 使用 `agent`，恢复前自动生成的是
`safety`。prune 只删除过期快照引用，不直接删除文件版本；随后仍由 `gc --batch` 分批回收。

NFS gateway 异常退出后，底层 mountpoint 会保持不可写，避免 Bash 把文件误写进普通本机
目录。此时先运行 `vexdb fs unmount --force MOUNTPOINT` 清理，再重新挂载。PostgreSQL 网络中断时，
失败的写命令不会被隐式重放；连接恢复后先读取或 quick check，再重新发出写命令。

旧命令继续可用：

```bash
vexfs ls /
```

## 真实 Bash 挂载

```bash
vexdb fs doctor
vexdb fs mount ~/VexDB
cd ~/VexDB
ls
cat hello.txt
```

gateway 只监听 `127.0.0.1`，mount status 的来源应是 `127.0.0.1:/`。状态文件和日志位于
`~/Library/Application Support/VexDB-Lite/nfs-gateways/`，正常卸载会清理对应目录。

要单独测试可选 FSKit：

```bash
vexdb fs --mount-driver fskit doctor
vexdb fs --mount-driver fskit mount ~/VexDB-FSKit
```

## 签名状态

打开 `MANIFEST.txt` 查看：

- `signature=developer-id`：App、extension、CLI 和 dylib 使用 Developer ID 签名；
- `notarization=accepted`：包已通过 Apple 公证；
- `signature=ad-hoc`：仅供本机开发测试。

默认拒绝安装任何脏源码 Developer ID 包。只有真机开发验证时才可显式运行
`VEXDB_LITE_ALLOW_DIRTY_INSTALL=1 ./install.sh`；正式发布和普通安装禁止使用这个开关。

FSKit 首次使用需要 macOS 允许该文件系统扩展，安装程序不能静默启用；这只影响显式
选择 FSKit 的用户，不影响默认 NFS 挂载。

## 卸载

```bash
./uninstall.sh
```

卸载不会删除默认数据库。
