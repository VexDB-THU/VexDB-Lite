#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
APP_DIR="${VEXFS_APP_DIR:-$HOME/Applications}"
BIN_DIR="${VEXFS_BIN_DIR:-$HOME/.local/bin}"
LIB_DIR="${VEXFS_LIB_DIR:-$HOME/.local/lib/vexfs}"

[ -d "$ROOT/VexFS.app" ] || { echo "安装包缺少 VexFS.app" >&2; exit 1; }
[ -x "$ROOT/bin/vexfs" ] || { echo "安装包缺少 bin/vexfs" >&2; exit 1; }
[ -f "$ROOT/lib/vexdb_lite.dylib" ] || { echo "安装包缺少 SQLite 扩展" >&2; exit 1; }

mkdir -p "$APP_DIR" "$BIN_DIR" "$LIB_DIR"
ditto "$ROOT/VexFS.app" "$APP_DIR/VexFS.app"
install -m 0755 "$ROOT/bin/vexfs" "$BIN_DIR/vexfs"
install -m 0644 "$ROOT/lib/vexdb_lite.dylib" "$LIB_DIR/vexdb_lite.dylib"

echo "VexFS 已安装："
echo "  App: $APP_DIR/VexFS.app"
echo "  CLI: $BIN_DIR/vexfs"
echo "  SQLite: $LIB_DIR/vexdb_lite.dylib"
case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *)
        echo ""
        echo "请把下面一行加入 ~/.zshrc："
        echo "  export PATH=\"$BIN_DIR:\$PATH\""
        ;;
esac
echo ""
echo "下一步：$BIN_DIR/vexfs setup && $BIN_DIR/vexfs doctor"
