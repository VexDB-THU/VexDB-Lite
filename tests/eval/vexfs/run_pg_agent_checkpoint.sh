#!/bin/bash
set -euo pipefail

# PostgreSQL 真实挂载下的通用 Agent checkpoint 回归。
# usage: VEXDB_PG_DSN=... bash run_pg_agent_checkpoint.sh [/path/to/vexdb]

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
VEXDB="${1:-${VEXDB_BIN:-$ROOT/vexdb_sqlite/build-nfs-dev/vexdb}}"
[ -x "$VEXDB" ] || { echo "找不到 vexdb：$VEXDB" >&2; exit 1; }
VEXDB="$(cd "$(dirname "$VEXDB")" && pwd)/$(basename "$VEXDB")"

DSN="${VEXDB_PG_DSN:-postgresql://postgres@127.0.0.1:5433/test}"
PG_CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
WORKSPACE="agent-run-$$-$(date +%s)"
TEMP_ROOT="$(mktemp -d -t vexdb-pg-agent-run)"
TEST_HOME="$TEMP_ROOT/home"
MOUNT_POINT="$TEMP_ROOT/mnt"
MOUNTED=0
CHECKS=0

mkdir -p "$TEST_HOME" "$MOUNT_POINT"

pgfs() {
    HOME="$TEST_HOME" VEXDB_PG_DSN="$DSN" \
        "$VEXDB" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" "$@"
}

run_in_workspace() {
    (
        cd "$MOUNT_POINT/project"
        HOME="$TEST_HOME" VEXDB_PG_DSN="$DSN" \
            "$VEXDB" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" "$@"
    )
}

drop_workspace() {
    if command -v psql >/dev/null 2>&1; then
        psql "$DSN" -X -q -v ON_ERROR_STOP=1 \
            -c "SELECT vexfs_workspace_drop('$WORKSPACE', true);" >/dev/null 2>&1 || true
    elif docker inspect "$PG_CONTAINER" >/dev/null 2>&1; then
        docker exec "$PG_CONTAINER" psql -U postgres -d "${VEXDB_PG_DATABASE:-test}" \
            -X -q -v ON_ERROR_STOP=1 \
            -c "SELECT vexfs_workspace_drop('$WORKSPACE', true);" >/dev/null 2>&1 || true
    fi
}

cleanup() {
    if [ "$MOUNTED" = 1 ]; then
        pgfs unmount --force "$MOUNT_POINT" >/dev/null 2>&1 || true
    fi
    drop_workspace
    rm -rf "$TEMP_ROOT"
}
trap cleanup EXIT

pgfs setup >/dev/null
set +e
PG_HISTORICAL_ERROR="$(pgfs snapshot create unsupported-history --commit 1 2>&1)"
PG_HISTORICAL_STATUS=$?
set -e
[ "$PG_HISTORICAL_STATUS" -eq 9 ]
case "$PG_HISTORICAL_ERROR" in
    *'supported only by SQLite'*) ;;
    *) echo "PG historical snapshot did not return the SQLite-only boundary" >&2; exit 1 ;;
esac
CHECKS=$((CHECKS + 2))
set +e
PG_TIME_ERROR="$(pgfs snapshot create unsupported-time \
    --at '2099-01-01T00:00:00Z' 2>&1)"
PG_TIME_STATUS=$?
set -e
[ "$PG_TIME_STATUS" -eq 9 ]
case "$PG_TIME_ERROR" in
    *'supported only by SQLite'*) ;;
    *) echo "PG time snapshot did not return the SQLite-only boundary" >&2; exit 1 ;;
esac
CHECKS=$((CHECKS + 2))
set +e
PG_TREE_ERROR="$(pgfs workspace show --commit 1 2>&1)"
PG_TREE_STATUS=$?
set -e
[ "$PG_TREE_STATUS" -eq 9 ]
case "$PG_TREE_ERROR" in
    *'supported only by SQLite'*) ;;
    *) echo "PG historical tree did not return the SQLite-only boundary" >&2; exit 1 ;;
esac
CHECKS=$((CHECKS + 2))
pgfs mount "$MOUNT_POINT"
MOUNTED=1
mkdir -p "$MOUNT_POINT/project"
printf 'pg-base\n' >"$MOUNT_POINT/project/state.txt"

SUCCESS_ERR="$TEMP_ROOT/success.err"
START_MS="$(python3 -c 'import time; print(int(time.time()*1000))')"
run_in_workspace --json run --snapshot-before --snapshot-after-success -- \
    /bin/sh -c \
    '[ "$1" = "--backend" ] && [ -n "$VEXFS_RUN_ID" ] && printf "pg-agent\n" > result.txt && printf "pg-child\n"' \
    vexfs-child --backend >"$TEMP_ROOT/success.out" 2>"$SUCCESS_ERR"
END_MS="$(python3 -c 'import time; print(int(time.time()*1000))')"
RUN_MS=$((END_MS - START_MS))
[ "$(cat "$TEMP_ROOT/success.out")" = pg-child ]
[ "$(cat "$MOUNT_POINT/project/result.txt")" = pg-agent ]

SUCCESS_META="$(python3 - "$SUCCESS_ERR" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")
          if line.startswith("{")]
before = next(event for event in events if event["event"] == "snapshot_before")
after = next(event for event in events if event["event"] == "snapshot_after")
finished = next(event for event in events if event["event"] == "run_exit")
assert before["run_id"] == after["run_id"] == finished["run_id"]
assert before["restore_command"].find("$VEXDB_PG_DSN") >= 0
assert finished["exit_code"] == 0
print(before["snapshot"], after["snapshot"])
PY
)"
SUCCESS_BEFORE="${SUCCESS_META%% *}"
SUCCESS_AFTER="${SUCCESS_META#* }"
if grep -Fq "$DSN" "$SUCCESS_ERR"; then
    echo "生命周期输出泄漏了 PostgreSQL DSN" >&2
    exit 1
fi
pgfs --json snapshot list | python3 -c \
    'import json,sys; names=set(sys.argv[1:]); rows=json.load(sys.stdin); assert names == {row["name"] for row in rows if row["name"] in names and row["type"]=="agent"}' \
    "$SUCCESS_BEFORE" "$SUCCESS_AFTER"
CHECKS=$((CHECKS + 8))

pgfs snapshot restore "$SUCCESS_BEFORE" >/dev/null
[ ! -e "$MOUNT_POINT/project/result.txt" ]
[ "$(cat "$MOUNT_POINT/project/state.txt")" = pg-base ]
CHECKS=$((CHECKS + 2))

FAILURE_ERR="$TEMP_ROOT/failure.err"
set +e
run_in_workspace --json run --snapshot-before --snapshot-after-success -- \
    /bin/sh -c 'printf "pg-failed\n" > failed.txt; exit 29' \
    >"$TEMP_ROOT/failure.out" 2>"$FAILURE_ERR"
FAILURE_STATUS=$?
set -e
[ "$FAILURE_STATUS" -eq 29 ]
[ "$(cat "$MOUNT_POINT/project/failed.txt")" = pg-failed ]
FAILURE_BEFORE="$(python3 - "$FAILURE_ERR" <<'PY'
import json
import sys

events = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8")
          if line.startswith("{")]
assert not any(event["event"] == "snapshot_after" for event in events)
assert next(event for event in events if event["event"] == "run_exit")["exit_code"] == 29
print(next(event for event in events if event["event"] == "snapshot_before")["snapshot"])
PY
)"
pgfs snapshot restore "$FAILURE_BEFORE" >/dev/null
[ ! -e "$MOUNT_POINT/project/failed.txt" ]
CHECKS=$((CHECKS + 4))

STARTUP_START_MS="$(python3 -c 'import time; print(int(time.time()*1000))')"
run_in_workspace --json run --snapshot-before -- /usr/bin/true \
    >"$TEMP_ROOT/startup.out" 2>"$TEMP_ROOT/startup.err"
STARTUP_END_MS="$(python3 -c 'import time; print(int(time.time()*1000))')"
STARTUP_MS=$((STARTUP_END_MS - STARTUP_START_MS))
[ "$STARTUP_MS" -le 5000 ]
CHECKS=$((CHECKS + 1))

echo "VEXFS PG AGENT CHECKPOINT: PASS ($CHECKS checks)"
echo "run_success_ms=$RUN_MS"
echo "snapshot_before_true_ms=$STARTUP_MS"
