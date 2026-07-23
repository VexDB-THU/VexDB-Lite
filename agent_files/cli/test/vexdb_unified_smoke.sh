#!/bin/bash
set -euo pipefail

VEXDB="${1:?usage: vexdb_unified_smoke.sh /path/to/vexdb}"
TMP_DIR="$(mktemp -d -t vexdb-unified-smoke)"
DB="$TMP_DIR/agent.sqlite3"
COPY="$TMP_DIR/agent-copy.sqlite3"
BACKUP_LINK_TARGET="$TMP_DIR/backup-link-target"
BACKUP_LINK="$TMP_DIR/backup-link.sqlite3"
VEXFS_EXE="$TMP_DIR/vexfs.exe"
cleanup() {
    rm -f "$DB" "$DB-wal" "$DB-shm" "$COPY" "$COPY-wal" "$COPY-shm" \
        "$BACKUP_LINK" "$BACKUP_LINK_TARGET" "$VEXFS_EXE"
    rmdir "$TMP_DIR" 2>/dev/null || true
}
trap cleanup EXIT

"$VEXDB" --version | grep -q 'vexdb-lite'
cp "$VEXDB" "$VEXFS_EXE"
"$VEXFS_EXE" --help | grep -q 'Commands:'

RESULT="$("$VEXDB" "$DB" \
    "SELECT vexfs_contract_version(), vexdb_l2_distance('[1,2]','[4,6]');")"
[ "$RESULT" = "0.7.0|5.0" ]

"$VEXDB" "$DB" \
    "CREATE VIRTUAL TABLE idx USING GRAPH_INDEX(embedding FLOAT[2], metric=l2); \
     INSERT INTO idx(rowid, embedding) VALUES (1, '[1,1]'), (2, '[9,9]');" >/dev/null
[ "$("$VEXDB" "$DB" \
    "SELECT rowid FROM idx WHERE embedding MATCH '[1,1]' AND k=1;")" = "1" ]

"$VEXDB" fs --db "$DB" --workspace smoke setup >/dev/null
printf 'one product' | "$VEXDB" fs --db "$DB" --workspace smoke write /note.txt >/dev/null
[ "$("$VEXDB" fs --db "$DB" --workspace smoke cat /note.txt)" = "one product" ]

"$VEXDB" backup "$DB" "$COPY" >/dev/null
[ "$("$VEXDB" "$COPY" "SELECT count(*) FROM idx;")" = "2" ]
[ "$("$VEXDB" fs --db "$COPY" --workspace smoke cat /note.txt)" = "one product" ]
COPY_MODE="$(stat -f '%Lp' "$COPY" 2>/dev/null || stat -c '%a' "$COPY")"
[ "$COPY_MODE" = "600" ]

if [ "$(uname -s)" != Windows_NT ]; then
    printf 'must-not-change' > "$BACKUP_LINK_TARGET"
    chmod 0644 "$BACKUP_LINK_TARGET"
    ln -s "$BACKUP_LINK_TARGET" "$BACKUP_LINK"
    if "$VEXDB" backup "$DB" "$BACKUP_LINK" >/dev/null 2>&1; then
        echo "backup unexpectedly followed a destination symlink" >&2
        exit 1
    fi
    [ "$(cat "$BACKUP_LINK_TARGET")" = "must-not-change" ]
    TARGET_MODE="$(stat -f '%Lp' "$BACKUP_LINK_TARGET" 2>/dev/null || stat -c '%a' "$BACKUP_LINK_TARGET")"
    [ "$TARGET_MODE" = "644" ]
fi

echo "VEXDB UNIFIED CLI SMOKE: PASS"
