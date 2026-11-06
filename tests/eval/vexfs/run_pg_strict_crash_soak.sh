#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
ROUND_SCRIPT="$ROOT/tests/eval/vexfs/run_pg_strict_crash_recovery.sh"
ROUNDS="${VEXFS_PG_STRICT_CRASH_ROUNDS:-20}"
DATABASE_PREFIX="${VEXFS_PG_STRICT_CRASH_DATABASE_PREFIX:-vexfs_strict_soak}"
WORKSPACE_PREFIX="${VEXFS_PG_STRICT_CRASH_WORKSPACE_PREFIX:-strict-soak}"
RUN_ID="${VEXFS_PG_STRICT_CRASH_RUN_ID:-$(date +%Y%m%d%H%M%S)_$$_${RANDOM}}"

case "$ROUNDS" in
    ''|*[!0-9]*) echo "crash soak 轮数必须是正整数：$ROUNDS" >&2; exit 2 ;;
esac
(( ROUNDS >= 1 && ROUNDS <= 100 )) || {
    echo "crash soak 轮数必须在 1 到 100 之间：$ROUNDS" >&2
    exit 2
}
case "$DATABASE_PREFIX:$WORKSPACE_PREFIX:$RUN_ID" in
    *[!A-Za-z0-9_:-]*)
        echo "crash soak 数据库和 workspace 前缀只能包含字母、数字、下划线和连字符" >&2
        exit 2
        ;;
esac

started_at="$(date +%s)"
for round in $(seq 1 "$ROUNDS"); do
    echo "VEXFS PG STRICT CRASH SOAK: round=$round/$ROUNDS"
    database="${DATABASE_PREFIX}_${RUN_ID}_${round}"
    (( ${#database} <= 63 )) || {
        echo "crash soak 数据库名不能超过 63 个字符：$database" >&2
        exit 2
    }
    VEXDB_PG_DSN="" \
    VEXDB_PG_DATABASE="$database" \
    VEXDB_PG_WORKSPACE="${WORKSPACE_PREFIX}-${RUN_ID}-${round}" \
        bash "$ROUND_SCRIPT"
done
elapsed="$(( $(date +%s) - started_at ))"

echo "VEXFS PG STRICT CRASH SOAK: PASS (rounds=$ROUNDS elapsed_seconds=$elapsed)"
