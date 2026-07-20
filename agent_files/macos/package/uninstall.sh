#!/bin/bash
set -euo pipefail

APP_DIR="${VEXFS_APP_DIR:-$HOME/Applications}"
BIN_DIR="${VEXFS_BIN_DIR:-$HOME/.local/bin}"
LIB_DIR="${VEXFS_LIB_DIR:-$HOME/.local/lib/vexfs}"

rm -rf "$APP_DIR/VexFS.app"
rm -f "$BIN_DIR/vexfs" "$LIB_DIR/vexdb_lite.dylib"
rmdir "$LIB_DIR" 2>/dev/null || true

echo "VexFS App、CLI 和 SQLite 扩展已卸载。"
echo "数据库未删除：$HOME/Library/Application Support/VexFS/"
