#!/bin/bash
set -euo pipefail

APP_DIR="${VEXDB_LITE_APP_DIR:-${VEXFS_APP_DIR:-$HOME/Applications}}"
BIN_DIR="${VEXDB_LITE_BIN_DIR:-${VEXFS_BIN_DIR:-$HOME/.local/bin}}"
LIB_DIR="${VEXDB_LITE_LIB_DIR:-${VEXFS_LIB_DIR:-$HOME/.local/lib/vexdb-lite}}"

rm -rf "$APP_DIR/VexDB Lite.app"
rm -f "$BIN_DIR/vexdb" "$BIN_DIR/vexfs" "$BIN_DIR/vexfs-nfs-gateway" \
    "$LIB_DIR/vexdb_lite.dylib"
rm -rf "$LIB_DIR/runtime"
rmdir "$LIB_DIR" 2>/dev/null || true

echo "VexDB-Lite App、CLI、SQLite 扩展和 PostgreSQL runtime 已卸载。"
echo "数据库未删除："
echo "  $HOME/Library/Application Support/VexDB-Lite/"
