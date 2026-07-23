#!/bin/bash
set -euo pipefail

BIN_DIR="${VEXDB_LITE_BIN_DIR:-$HOME/.local/bin}"
LIB_DIR="${VEXDB_LITE_LIB_DIR:-$HOME/.local/lib/vexdb-lite}"

if awk '$0 ~ / - fuse(\.vexfs)? vexfs / { found=1 } END { exit !found }' \
    /proc/self/mountinfo 2>/dev/null; then
    echo "仍有 VexFS 目录挂载。请先执行 vexdb fs unmount，必要时加 --force。" >&2
    exit 1
fi

rm -f "$BIN_DIR/vexfs" "$BIN_DIR/vexfs-fuse" "$BIN_DIR/vexdb"
rm -f "$LIB_DIR/vexdb_lite.so"
rmdir "$LIB_DIR" 2>/dev/null || true

echo "VexDB-Lite 程序已卸载。数据库没有删除："
echo "  ${XDG_DATA_HOME:-$HOME/.local/share}/vexdb-lite/"
