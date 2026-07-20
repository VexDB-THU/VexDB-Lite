# VexFS macOS 技术预览

VexFS 是由 SQLite 管理的文件工作区。本包提供：

- `VexFS.app`：包含 FSKit 文件系统扩展；
- `bin/vexfs`：命令行工具；
- `lib/vexdb_lite.dylib`：可由 SQLite 加载的扩展；
- `install.sh`、`Install.command` 和 `uninstall.sh`。

## 系统要求

- macOS 26.0 或更高版本；
- Apple Silicon 包要求 arm64 Mac；
- 首次尝试真实挂载时，需要在系统设置中启用 VexFS 文件系统扩展。

## 重要限制

这是 **ad-hoc 签名的技术预览包**，没有 Developer ID 签名和 Apple 公证。

- CLI 和 SQLite 文件管理功能可以直接测试；
- 从聊天、邮件或浏览器下载后，macOS 可能阻止打开 App。请在 Finder 中右键 `VexFS.app`，选择“打开”；
- FSKit extension 是否允许在另一台 Mac 上启用，取决于该机器的系统安全设置。正式对外分发必须使用 Apple Developer 证书签名并公证。

## 安装

双击 `Install.command`，或在终端运行：

```bash
./install.sh
```

默认安装位置：

- App：`~/Applications/VexFS.app`
- CLI：`~/.local/bin/vexfs`
- SQLite 扩展：`~/.local/lib/vexfs/vexdb_lite.dylib`

如果 `~/.local/bin` 不在 PATH：

```bash
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.zshrc
source ~/.zshrc
```

## 不挂载也能使用

```bash
vexfs setup
printf 'hello VexFS\n' | vexfs write /hello.txt
vexfs cat /hello.txt
vexfs ls /
vexfs history /hello.txt
vexfs doctor --json
```

默认数据库位于：

```text
~/Library/Application Support/VexFS/vexfs.sqlite3
```

## 尝试真实 Bash 挂载

1. 打开 `~/Applications/VexFS.app`；
2. 进入“系统设置 → 通用 → 登录项与扩展 → 文件系统扩展”；
3. 启用 VexFS；
4. 运行：

```bash
vexfs doctor
vexfs mount ~/VexFS
cd ~/VexFS
ls
cat hello.txt
```

如果 `doctor` 显示扩展未启用，仍可继续使用上面的直接 CLI 命令。

## 卸载

```bash
./uninstall.sh
```

卸载脚本不会删除数据库。确认不再需要数据后，可自行删除：

```text
~/Library/Application Support/VexFS/
```
