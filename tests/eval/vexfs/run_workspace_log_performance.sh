#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SQLITE_EXTENSION="${VEXDB_SQLITE_EXTENSION:-$ROOT/vexdb_sqlite/build/vexdb_lite.dylib}"
PG_CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
PG_DATABASE="${VEXDB_PG_DATABASE:-test}"
COUNT="${VEXFS_WORKSPACE_LOG_COMMITS:-100000}"
LIMIT="${VEXFS_WORKSPACE_LOG_LIMIT:-100}"
MAX_QUERY_MS="${VEXFS_WORKSPACE_LOG_MAX_QUERY_MS:-2000}"
MEMORY_LIMIT_BYTES="${VEXFS_WORKSPACE_LOG_MEMORY_LIMIT_BYTES:-1073741824}"
WORKSPACE="eval-workspace-log-$$"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-workspace-log.XXXXXX")"
REPORT_DIR="$ROOT/build/eval/vexfs"
REPORT="$REPORT_DIR/workspace_log_performance_${COUNT}.tsv"

cleanup() {
    local status=$?
    trap - EXIT
    docker exec "$PG_CONTAINER" psql -U postgres -d "$PG_DATABASE" -X -q \
        -c "SELECT public.vexfs_workspace_drop('$WORKSPACE', true);" \
        >/dev/null 2>&1 || true
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

fail() {
    echo "VEXFS WORKSPACE LOG PERFORMANCE: FAIL: $*" >&2
    exit 1
}

now_ms() {
    "$ROOT/tests/eval/vexfs/python.sh" -c \
        'import time; print(time.monotonic_ns() // 1000000)'
}

memory_value() {
    local file=$1
    docker exec "$PG_CONTAINER" sh -c \
        "if [ -r /sys/fs/cgroup/$file ]; then cat /sys/fs/cgroup/$file; else echo unavailable; fi"
}

oom_kills() {
    docker exec "$PG_CONTAINER" sh -c \
        "if [ -r /sys/fs/cgroup/memory.events ]; then awk '\$1==\"oom_kill\" {print \$2}' /sys/fs/cgroup/memory.events; else echo 0; fi"
}

[[ "$COUNT" =~ ^[0-9]+$ ]] || fail "commit 数必须是整数"
[[ "$LIMIT" =~ ^[0-9]+$ ]] || fail "limit 必须是整数"
(( COUNT >= 10000 && COUNT <= 1000000 )) || fail "commit 数必须在 10000..1000000"
(( LIMIT >= 1 && LIMIT <= 1000 )) || fail "limit 必须在 1..1000"
[[ -f "$SQLITE_EXTENSION" ]] || fail "找不到 SQLite 扩展：$SQLITE_EXTENSION"
docker inspect "$PG_CONTAINER" >/dev/null
CONTAINER_LIMIT="$(memory_value memory.max)"
[[ "$CONTAINER_LIMIT" =~ ^[0-9]+$ ]] || fail "PG 测试容器必须设置 memory.max"
(( CONTAINER_LIMIT <= MEMORY_LIMIT_BYTES )) || \
    fail "PG 容器 memory.max=$CONTAINER_LIMIT，超过上限 $MEMORY_LIMIT_BYTES"
OOM_BEFORE="$(oom_kills)"

DB="$TMP/workspace-log.sqlite3"
SQLITE_RESULT="$("$ROOT/tests/eval/vexfs/python.sh" \
    "$ROOT/tests/eval/vexfs/workspace_log_sqlite_perf.py" \
    "$SQLITE_EXTENSION" "$DB" "$WORKSPACE" "$COUNT" "$LIMIT" \
    "$MAX_QUERY_MS" 536870912)"
IFS='|' read -r SQLITE_HEAD_MS SQLITE_TAIL_MS SQLITE_RSS <<<"$SQLITE_RESULT"
SQLITE_TAIL_CURSOR=$((LIMIT + 2))

docker exec "$PG_CONTAINER" psql -U postgres -d "$PG_DATABASE" -X -q \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_workspace_create('$WORKSPACE');
     INSERT INTO _vexfs.commits(
       workspace_id,commit_no,parent_commit,operation,path,created_by_oid,created_by,created_at)
     SELECT workspace.workspace_id,n,n-1,'eval_commit','/src/file-'||n||'.txt',
            role.oid,'postgres',to_timestamp((1700000000000+n)/1000.0)
       FROM _vexfs.workspaces workspace
       JOIN pg_catalog.pg_roles role ON role.rolname='postgres'
       CROSS JOIN generate_series(2,$((COUNT + 1))) AS n
      WHERE workspace.name='$WORKSPACE';
     UPDATE _vexfs.workspaces SET head_commit=$((COUNT + 1)) WHERE name='$WORKSPACE';
     INSERT INTO _vexfs.snapshots(
       workspace_id,name,head_commit,created_by_oid,created_by)
     SELECT workspace.workspace_id,'head-snapshot',$((COUNT + 1)),role.oid,'postgres'
       FROM _vexfs.workspaces workspace
       JOIN pg_catalog.pg_roles role ON role.rolname='postgres'
      WHERE workspace.name='$WORKSPACE';" >/dev/null

PG_STARTED="$(now_ms)"
docker exec "$PG_CONTAINER" psql -U postgres -d "$PG_DATABASE" -X -q -t -A \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_workspace_log('$WORKSPACE',$LIMIT,0);" \
    >"$TMP/pg-head.json"
PG_HEAD_MS=$(( $(now_ms) - PG_STARTED ))
PG_STARTED="$(now_ms)"
docker exec "$PG_CONTAINER" psql -U postgres -d "$PG_DATABASE" -X -q -t -A \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_workspace_log('$WORKSPACE',$LIMIT,$SQLITE_TAIL_CURSOR);" \
    >"$TMP/pg-tail.json"
PG_TAIL_MS=$(( $(now_ms) - PG_STARTED ))
"$ROOT/tests/eval/vexfs/python.sh" -c \
    'import json,sys
count=int(sys.argv[1]); limit=int(sys.argv[2]); cursor=int(sys.argv[3])
head=json.load(open(sys.argv[4])); tail=json.load(open(sys.argv[5]))
assert len(head["entries"]) == limit
assert head["entries"][0]["commit"] == count + 1
assert head["entries"][0]["snapshots"] == ["head-snapshot"]
assert head["next_before"] == count + 2 - limit
assert tail["entries"] and all(row["commit"] < cursor for row in tail["entries"])
' "$COUNT" "$LIMIT" "$SQLITE_TAIL_CURSOR" "$TMP/pg-head.json" "$TMP/pg-tail.json"
(( PG_HEAD_MS <= MAX_QUERY_MS && PG_TAIL_MS <= MAX_QUERY_MS )) || \
    fail "PG workspace log 超时：head=${PG_HEAD_MS}ms tail=${PG_TAIL_MS}ms"

OOM_AFTER="$(oom_kills)"
MEMORY_AFTER="$(memory_value memory.current)"
[[ "$OOM_AFTER" == "$OOM_BEFORE" ]] || fail "测试期间发生 oom_kill"
(( MEMORY_AFTER <= MEMORY_LIMIT_BYTES )) || fail "PG 测试后内存超过上限：$MEMORY_AFTER"

mkdir -p "$REPORT_DIR"
printf 'engine\tcommits\tlimit\thead_ms\ttail_ms\tmemory_bytes\toom_kills\n' >"$REPORT"
printf 'sqlite\t%s\t%s\t%s\t%s\t%s\t-\n' \
    "$COUNT" "$LIMIT" "$SQLITE_HEAD_MS" "$SQLITE_TAIL_MS" "$SQLITE_RSS" >>"$REPORT"
printf 'postgresql\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$COUNT" "$LIMIT" "$PG_HEAD_MS" "$PG_TAIL_MS" "$MEMORY_AFTER" "$OOM_AFTER" >>"$REPORT"

echo "VEXFS WORKSPACE LOG PERFORMANCE: PASS ($COUNT commits)"
echo "sqlite head=${SQLITE_HEAD_MS}ms tail=${SQLITE_TAIL_MS}ms"
echo "postgresql head=${PG_HEAD_MS}ms tail=${PG_TAIL_MS}ms memory=$MEMORY_AFTER"
echo "report=$REPORT"
