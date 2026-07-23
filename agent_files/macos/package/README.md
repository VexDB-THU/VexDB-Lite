# VexDB-Lite macOS 技术预览

完整的安装、SQLite、向量、文件版本、挂载、备份和卸载说明见同目录
[`使用说明.md`](使用说明.md)。

VexDB-Lite 在一个安装包中提供：

- `VexDB Lite.app`：包含 VexFS FSKit 文件系统扩展；
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
```

已挂载 workspace 会在恢复时自动安全卸载并挂回原目录；正常卸载失败时不会开始恢复。
`--force-unmount` 只用于用户明确接受中断打开文件的场景。

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
