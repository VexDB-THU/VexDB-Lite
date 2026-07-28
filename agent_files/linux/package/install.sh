#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BIN_DIR="${VEXDB_LITE_BIN_DIR:-$HOME/.local/bin}"
LIB_DIR="${VEXDB_LITE_LIB_DIR:-$HOME/.local/lib/vexdb-lite}"
EXPECTED_ARCH="$(sed -n 's/^architecture=//p' "$ROOT/MANIFEST.txt")"
ACTUAL_ARCH="$(uname -m)"

[ "$(uname -s)" = Linux ] || { echo "这个安装包只支持 Linux" >&2; exit 1; }
[ "$ACTUAL_ARCH" = "$EXPECTED_ARCH" ] || {
    echo "安装包架构不匹配：当前 $ACTUAL_ARCH，安装包 $EXPECTED_ARCH" >&2
    exit 1
}
[ -x "$ROOT/bin/vexdb" ] || { echo "安装包缺少 bin/vexdb" >&2; exit 1; }
[ -x "$ROOT/bin/vexfs-fuse" ] || { echo "安装包缺少 bin/vexfs-fuse" >&2; exit 1; }
[ -f "$ROOT/lib/vexdb_lite.so" ] || { echo "安装包缺少 lib/vexdb_lite.so" >&2; exit 1; }
(
    cd "$ROOT"
    sha256sum -c SHA256SUMS.txt >/dev/null
) || { echo "安装包文件哈希校验失败" >&2; exit 1; }

mkdir -p "$BIN_DIR" "$LIB_DIR"
install -m 0755 "$ROOT/bin/vexdb" "$BIN_DIR/vexdb"
install -m 0755 "$ROOT/bin/vexfs-fuse" "$BIN_DIR/vexfs-fuse"
ln -sfn vexdb "$BIN_DIR/vexfs"
install -m 0644 "$ROOT/lib/vexdb_lite.so" "$LIB_DIR/vexdb_lite.so"

echo "VexDB-Lite 已安装："
echo "  CLI: $BIN_DIR/vexdb"
echo "  文件快捷命令: $BIN_DIR/vexfs"
echo "  FUSE helper: $BIN_DIR/vexfs-fuse"
echo "  SQLite 扩展: $LIB_DIR/vexdb_lite.so"

if ! command -v fusermount3 >/dev/null 2>&1; then
    echo ""
    echo "还不能挂载：系统缺少 fuse3。"
    echo "  Debian/Ubuntu: sudo apt install fuse3"
    echo "  Fedora/RHEL:   sudo dnf install fuse3"
elif [ ! -e /dev/fuse ]; then
    echo ""
    echo "还不能挂载：系统没有 /dev/fuse，请加载 fuse 内核模块或检查容器权限。"
elif [ ! -r /dev/fuse ] || [ ! -w /dev/fuse ]; then
    echo ""
    echo "还不能挂载：当前用户没有读写 /dev/fuse 的权限。"
fi

case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *)
        echo ""
        echo "请把下面一行加入 shell 配置："
        echo "  export PATH=\"$BIN_DIR:\$PATH\""
        ;;
esac

echo ""
echo "下一步：$BIN_DIR/vexdb --version && $BIN_DIR/vexdb fs setup && $BIN_DIR/vexdb fs doctor"
