#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CLI="${VEXDB_PG_CLI:-$ROOT/vexdb_sqlite/build/vexdb}"
PG_CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
PG_DATABASE="${VEXDB_PG_DATABASE:-test}"
PG_DSN="${VEXDB_PG_DSN:-postgresql://postgres@127.0.0.1:5433/test}"
WORKERS="${VEXFS_SNAPSHOT_CONCURRENCY_WORKERS:-8}"
KEEP=5
PG_WORKSPACE="eval-snapshot-concurrency-pg-$$"
SQLITE_WORKSPACE="eval-snapshot-concurrency-sqlite-$$"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-snapshot-concurrency.XXXXXX")"
SQLITE_DB="$TMP/snapshot-concurrency.sqlite3"
REPORT_DIR="$ROOT/build/eval/vexfs"
REPORT="$REPORT_DIR/snapshot_policy_concurrency.tsv"

if [ -n "${VEXDB_PG_LIB_DIR:-}" ]; then
    export DYLD_LIBRARY_PATH="$VEXDB_PG_LIB_DIR${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH="$VEXDB_PG_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

cleanup() {
    local status=$?
    trap - EXIT
    docker exec "$PG_CONTAINER" psql -U postgres -d "$PG_DATABASE" -X -q \
        -c "SELECT public.vexfs_workspace_drop('$PG_WORKSPACE', true);" \
        >/dev/null 2>&1 || true
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

fail() {
    echo "VEXFS SNAPSHOT POLICY CONCURRENCY: FAIL: $*" >&2
    exit 1
}

[[ "$WORKERS" =~ ^[0-9]+$ ]] || fail "workers 必须是整数"
(( WORKERS >= 4 && WORKERS <= 32 )) || fail "workers 必须在 4..32"
[ -x "$CLI" ] || fail "找不到 vexdb CLI：$CLI"

sqlite() {
    "$CLI" fs --db "$SQLITE_DB" --workspace "$SQLITE_WORKSPACE" "$@"
}

postgresql() {
    "$CLI" fs --backend pg --dsn "$PG_DSN" --workspace "$PG_WORKSPACE" "$@"
}

prepare_workspace() {
    local engine=$1
    "$engine" setup >/dev/null
    "$engine" snapshot create manual-keep --type manual >/dev/null
    "$engine" snapshot policy set --agent-keep "$KEEP" --safety-keep "$KEEP" --days 0 \
        >/dev/null
    local index
    for ((index=1; index<=12; index++)); do
        "$engine" snapshot create "agent-old-$index" --type agent >/dev/null
        "$engine" snapshot create "safety-old-$index" --type safety >/dev/null
    done
}

run_wave() {
    local engine=$1
    local label=$2
    local pids=()
    local index
    local status=0
    local started=$SECONDS
    for ((index=1; index<=WORKERS; index++)); do
        local type=agent
        if (( index % 2 == 0 )); then type=safety; fi
        "$engine" snapshot create "$label-new-$index" --type "$type" \
            >"$TMP/$label-create-$index.out" 2>"$TMP/$label-create-$index.err" &
        pids+=("$!")
    done
    for ((index=1; index<=4; index++)); do
        "$engine" snapshot prune \
            >"$TMP/$label-prune-$index.out" 2>"$TMP/$label-prune-$index.err" &
        pids+=("$!")
    done
    for index in "${!pids[@]}"; do
        wait "${pids[$index]}" || status=1
    done
    if (( status != 0 )); then
        sed -n '1,80p' "$TMP/$label-"*.err >&2
        fail "$label 并发 create/prune 失败"
    fi
    "$engine" snapshot prune >/dev/null
    "$engine" snapshot policy show >"$TMP/$label-policy.json"
    "$engine" --json check >"$TMP/$label-check.json"
    "$ROOT/tests/eval/vexfs/python.sh" -c \
        'import json,sys
policy=json.load(open(sys.argv[1])); check=json.load(open(sys.argv[2]))
assert policy["snapshots"] == {"total":11,"manual":1,"agent":5,"safety":5}
assert policy["expired"]["total"] == 0
assert check["ok"] is True
' "$TMP/$label-policy.json" "$TMP/$label-check.json"
    echo $((SECONDS - started))
}

prepare_workspace sqlite
SQLITE_SECONDS="$(run_wave sqlite sqlite)"

prepare_workspace postgresql
PG_SECONDS="$(run_wave postgresql postgresql)"

mkdir -p "$REPORT_DIR"
printf 'engine\tworkers\tcreate_operations\tprune_operations\tseconds\tremaining_snapshots\n' >"$REPORT"
printf 'sqlite\t%s\t%s\t4\t%s\t11\n' "$WORKERS" "$WORKERS" "$SQLITE_SECONDS" >>"$REPORT"
printf 'postgresql\t%s\t%s\t4\t%s\t11\n' "$WORKERS" "$WORKERS" "$PG_SECONDS" >>"$REPORT"

echo "VEXFS SNAPSHOT POLICY CONCURRENCY: PASS ($WORKERS create workers + 4 prune workers)"
echo "sqlite=${SQLITE_SECONDS}s postgresql=${PG_SECONDS}s"
echo "report=$REPORT"
