# VexDB-Lite macOS 技术预览

完整的安装、SQLite、向量、文件版本、挂载、备份和卸载说明见同目录
[`使用说明.md`](使用说明.md)。

VexDB-Lite 在一个安装包中提供：

- `.payload/VexDB Lite.app`：安装程序使用的隐藏 App payload，包含 VexFS FSKit 文件系统扩展；
- `bin/vexdb`：内置 SQLite、向量检索和文件 SQL 能力的统一命令；
- `bin/vexfs`：`vexdb fs` 的兼容快捷入口；
- `lib/vexdb_lite.dylib`：供现有 SQLite 程序加载的扩展；
- 安装和卸载脚本。

## 系统要求

- macOS 26.0 或更高版本；
- Apple Silicon 包要求 arm64 Mac；
- 只有真实挂载需要 macOS 允许 `VexDB Lite` 文件系统扩展；系统显示的是宿主 App 名，内部文件系统名才是 VexFS。

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

安装程序会在后台启动一次 VexDB Lite App，让 macOS 立即发现新安装或刚替换的 FSKit 模块。
它不会替用户打开扩展开关；首次安装仍需在系统设置中允许。
App payload 放在隐藏目录中，避免解压目录和安装目录同时被 macOS 注册为两个同名
FSKit 模块。扩展已经获准时，安装程序还会执行一次临时真实挂载；挂载失败不会假报安装成功。
升级时，旧 App、CLI、SQLite 扩展和 PostgreSQL runtime 只放在隐藏事务目录中；任何一步失败
都会恢复旧版本，成功后立即删除临时备份，不会在 Applications 中留下可被系统再次识别的
`.app.disabled` 副本。如果 macOS 要求新签名版本重新授权，安装程序会明确提示，不会静默跳过。
安装或升级前请先卸载正在使用的 VexFS、exFAT 等 FSKit 卷。安装程序会拒绝在这些卷
仍挂载时替换扩展，然后只重启当前用户的 `fskit_agent` 来刷新模块；它不会重启 `pkd`、
清空 LaunchServices 注册库或影响其他用户。

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
vexdb fs snapshot diff before-agent
vexdb fs snapshot restore before-agent --dry-run
vexdb fs snapshot restore before-agent
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

FSKit 异常退出后，底层 mountpoint 会保持不可写，避免 Bash 把文件误写进普通本机目录。
此时先运行 `vexdb fs unmount --force MOUNTPOINT` 清理，再重新挂载。PostgreSQL 网络中断时，
失败的写命令不会被隐式重放；连接恢复后先读取或 quick check，再重新发出写命令。

旧命令继续可用：

```bash
vexfs ls /
```

## 真实 Bash 挂载

1. 打开 `~/Applications/VexDB Lite.app`；
2. 进入“系统设置 → 通用 → 登录项与扩展”，滚动到扩展区域；
3. 切换到“按类别”视图；
4. 在“文件系统扩展”这一行点右侧 ⓘ，找到 `VexDB Lite` 并打开它的开关；
5. 如果看到的是 `VexDB Lite / FSKit Modules` 只读详情，说明仍在“按 App”视图，返回上一层切换视图；
6. 运行：

```bash
vexdb fs doctor
vexdb fs mount ~/VexDB
cd ~/VexDB
ls
cat hello.txt
```

## 签名状态

打开 `MANIFEST.txt` 查看：

- `signature=developer-id`：App、extension、CLI 和 dylib 使用 Developer ID 签名；
- `notarization=accepted`：包已通过 Apple 公证；
- `signature=ad-hoc`：仅供本机开发测试。

默认拒绝安装任何脏源码 Developer ID 包。只有真机开发验证时才可显式运行
`VEXDB_LITE_ALLOW_DIRTY_INSTALL=1 ./install.sh`；正式发布和普通安装禁止使用这个开关。

FSKit 首次使用需要 macOS 允许该文件系统扩展。安装程序不能静默启用；
请在“按类别”视图的“文件系统扩展”中完成这一步。

## 卸载

```bash
./uninstall.sh
```

卸载不会删除默认数据库。
