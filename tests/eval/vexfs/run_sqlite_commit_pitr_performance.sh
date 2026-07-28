#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SQLITE_EXTENSION="${VEXDB_SQLITE_EXTENSION:-$ROOT/vexdb_sqlite/build-nfs-dev/vexdb_lite.dylib}"
FILES="${VEXFS_COMMIT_PITR_FILES:-10000}"
LIMIT="${VEXFS_COMMIT_PITR_LIMIT:-100}"
MAX_QUERY_MS="${VEXFS_COMMIT_PITR_MAX_QUERY_MS:-5000}"
MAX_SNAPSHOT_MS="${VEXFS_COMMIT_PITR_MAX_SNAPSHOT_MS:-20000}"
MAX_RSS="${VEXFS_COMMIT_PITR_MAX_RSS_BYTES:-536870912}"
WORKSPACE="eval-commit-pitr-$$"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-commit-pitr.XXXXXX")"
REPORT_DIR="$ROOT/build/eval/vexfs"
REPORT="$REPORT_DIR/sqlite_commit_pitr_performance_${FILES}.tsv"
trap 'rm -rf "$TMP"' EXIT

fail() {
    echo "VEXFS SQLITE COMMIT PITR PERFORMANCE: FAIL: $*" >&2
    exit 1
}

[[ "$FILES" =~ ^[0-9]+$ ]] || fail "文件数必须是整数"
[[ "$LIMIT" =~ ^[0-9]+$ ]] || fail "limit 必须是整数"
(( FILES >= 10000 && FILES <= 100000 )) || fail "文件数必须在 10000..100000"
(( LIMIT >= 1 && LIMIT <= 1000 && LIMIT < FILES )) || \
    fail "limit 必须在 1..1000 且小于文件数"
[[ -f "$SQLITE_EXTENSION" ]] || fail "找不到 SQLite 扩展：$SQLITE_EXTENSION"

RESULT="$("$ROOT/tests/eval/vexfs/python.sh" \
    "$ROOT/tests/eval/vexfs/sqlite_commit_pitr_perf.py" \
    "$SQLITE_EXTENSION" "$TMP/commit-pitr.sqlite3" "$WORKSPACE" "$FILES" \
    "$LIMIT" "$MAX_QUERY_MS" "$MAX_SNAPSHOT_MS" "$MAX_RSS")"
IFS='|' read -r SHOW_HEAD_MS SHOW_DEEP_MS DIFF_HEAD_MS DIFF_DEEP_MS \
    SNAPSHOT_MS RSS DATABASE_BYTES TARGET_COMMIT <<<"$RESULT"

mkdir -p "$REPORT_DIR"
printf 'files\tlimit\tshow_head_ms\tshow_deep_ms\tdiff_head_ms\tdiff_deep_ms\tsnapshot_ms\trss_bytes\tdatabase_bytes\tcommit\n' >"$REPORT"
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$FILES" "$LIMIT" "$SHOW_HEAD_MS" "$SHOW_DEEP_MS" "$DIFF_HEAD_MS" \
    "$DIFF_DEEP_MS" "$SNAPSHOT_MS" "$RSS" "$DATABASE_BYTES" "$TARGET_COMMIT" \
    >>"$REPORT"

echo "VEXFS SQLITE COMMIT PITR PERFORMANCE: PASS ($FILES files)"
echo "show head=${SHOW_HEAD_MS}ms deep=${SHOW_DEEP_MS}ms"
echo "diff head=${DIFF_HEAD_MS}ms deep=${DIFF_DEEP_MS}ms"
echo "snapshot=${SNAPSHOT_MS}ms rss=${RSS} database=${DATABASE_BYTES}"
echo "report=$REPORT"
