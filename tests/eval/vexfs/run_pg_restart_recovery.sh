#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DATABASE="${VEXDB_PG_DATABASE:-test}"
DSN="${VEXDB_PG_DSN:-postgresql://postgres@127.0.0.1:5433/test}"
BINARY="${VEXDB_PG_RUNTIME_SMOKE:-$ROOT/vexdb_sqlite/build/vexfs_pg_runtime_smoke}"
GATE="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-restart.XXXXXX")"
OUTPUT="$GATE/runtime.out"
ERRORS="$GATE/runtime.err"
PID=""

cleanup() {
    local status=$?
    trap - EXIT
    if [ -n "$PID" ] && kill -0 "$PID" >/dev/null 2>&1; then
        kill "$PID" >/dev/null 2>&1 || true
        wait "$PID" >/dev/null 2>&1 || true
    fi
    if [ "$status" -ne 0 ]; then
        sed -n '1,240p' "$OUTPUT" >&2 2>/dev/null || true
        sed -n '1,240p' "$ERRORS" >&2 2>/dev/null || true
    fi
    rm -rf "$GATE"
    exit "$status"
}
trap cleanup EXIT

[ -x "$BINARY" ] || { echo "找不到 PG runtime smoke：$BINARY" >&2; exit 1; }
docker inspect "$CONTAINER" >/dev/null
docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -v ON_ERROR_STOP=1 \
    -c "CREATE EXTENSION IF NOT EXISTS pg_trgm;" >/dev/null

VEXFS_PG_RESTART_GATE_DIR="$GATE" "$BINARY" "$DSN" >"$OUTPUT" 2>"$ERRORS" &
PID=$!

ready=0
for _ in $(seq 1 240); do
    if [ -f "$GATE/ready" ]; then
        ready=1
        break
    fi
    if ! kill -0 "$PID" >/dev/null 2>&1; then
        wait "$PID" || true
        echo "PG runtime 在数据库重启门禁前已经退出" >&2
        exit 1
    fi
    sleep 0.25
done
[ "$ready" -eq 1 ] || { echo "等待 PG runtime 重启门禁超时" >&2; exit 1; }

docker exec "$CONTAINER" pg_ctl \
    -D /var/lib/postgresql/data -m fast -w restart >/dev/null
touch "$GATE/continue"

wait "$PID"
PID=""
grep -q '^VEXFS PG RUNTIME CONTRACT: PASS' "$OUTPUT"
cat "$OUTPUT"
echo "VEXFS PG SERVER RESTART RECOVERY: PASS"
