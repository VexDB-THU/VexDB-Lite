#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SQLITE_EXTENSION="${VEXDB_SQLITE_EXTENSION:-$ROOT/vexdb_sqlite/build/vexdb_lite.dylib}"
PG_CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
PG_DATABASE="${VEXDB_PG_DATABASE:-test}"
COUNT="${VEXFS_SNAPSHOT_POLICY_COUNT:-100000}"
AGENT_KEEP="${VEXFS_SNAPSHOT_AGENT_KEEP:-20}"
SAFETY_KEEP="${VEXFS_SNAPSHOT_SAFETY_KEEP:-10}"
MAX_QUERY_MS="${VEXFS_SNAPSHOT_POLICY_MAX_QUERY_MS:-5000}"
SQLITE_MEMORY_LIMIT="${VEXFS_SNAPSHOT_SQLITE_MEMORY_LIMIT_BYTES:-536870912}"
PG_MEMORY_LIMIT="${VEXFS_SNAPSHOT_PG_MEMORY_LIMIT_BYTES:-1073741824}"
WORKSPACE="eval-snapshot-policy-$$"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-snapshot-policy.XXXXXX")"
REPORT_DIR="$ROOT/build/eval/vexfs"
REPORT="$REPORT_DIR/snapshot_policy_performance_${COUNT}.tsv"

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
    echo "VEXFS SNAPSHOT POLICY PERFORMANCE: FAIL: $*" >&2
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

[[ "$COUNT" =~ ^[0-9]+$ ]] || fail "snapshot 数必须是整数"
[[ "$AGENT_KEEP" =~ ^[0-9]+$ && "$SAFETY_KEEP" =~ ^[0-9]+$ ]] || \
    fail "保留数量必须是整数"
(( COUNT >= 10000 && COUNT <= 1000000 )) || fail "snapshot 数必须在 10000..1000000"
(( AGENT_KEEP <= (COUNT + 1) / 2 && SAFETY_KEEP <= COUNT / 2 )) || \
    fail "每类保留数不能超过该类型的 snapshot 数"
[[ -f "$SQLITE_EXTENSION" ]] || fail "找不到 SQLite 扩展：$SQLITE_EXTENSION"
docker inspect "$PG_CONTAINER" >/dev/null
CONTAINER_LIMIT="$(memory_value memory.max)"
[[ "$CONTAINER_LIMIT" =~ ^[0-9]+$ ]] || fail "PG 测试容器必须设置 memory.max"
(( CONTAINER_LIMIT <= PG_MEMORY_LIMIT )) || \
    fail "PG 容器 memory.max=$CONTAINER_LIMIT，超过上限 $PG_MEMORY_LIMIT"
OOM_BEFORE="$(oom_kills)"

SQLITE_RESULT="$(VEXDB_LITE_PYTHON="${VEXDB_LITE_PYTHON:-/opt/anaconda3/bin/python3}" \
    "$ROOT/tests/eval/vexfs/python.sh" \
    "$ROOT/tests/eval/vexfs/snapshot_policy_sqlite_perf.py" \
    "$SQLITE_EXTENSION" "$TMP/snapshot-policy.sqlite3" "$WORKSPACE" "$COUNT" \
    "$AGENT_KEEP" "$SAFETY_KEEP" "$MAX_QUERY_MS")"
IFS='|' read -r SQLITE_POLICY_MS SQLITE_DRY_MS SQLITE_PRUNE_MS SQLITE_RSS EXPECTED_CANDIDATES \
    <<<"$SQLITE_RESULT"
(( SQLITE_RSS <= SQLITE_MEMORY_LIMIT )) || \
    fail "SQLite 峰值内存 $SQLITE_RSS 超过上限 $SQLITE_MEMORY_LIMIT"

docker exec "$PG_CONTAINER" psql -U postgres -d "$PG_DATABASE" -X -q \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_workspace_create('$WORKSPACE');
     INSERT INTO _vexfs.snapshots(
       workspace_id,name,snapshot_type,head_commit,created_by_oid,created_by,created_at)
     SELECT workspace.workspace_id,
            CASE WHEN value % 2 = 0 THEN 'agent-' ELSE 'safety-' END || lpad(value::text,6,'0'),
            CASE WHEN value % 2 = 0 THEN 'agent' ELSE 'safety' END,
            workspace.head_commit,role.oid,'postgres',
            to_timestamp((1700000000000+value)/1000.0)
       FROM _vexfs.workspaces workspace
       JOIN pg_catalog.pg_roles role ON role.rolname='postgres'
       CROSS JOIN generate_series(0,$((COUNT - 1))) AS value
      WHERE workspace.name='$WORKSPACE';
     SELECT public.vexfs_snapshot_create('$WORKSPACE','manual-keep',NULL,'consistent','manual');
     SELECT public.vexfs_snapshot_policy_set('$WORKSPACE',$AGENT_KEEP,$SAFETY_KEEP,0);" \
    >/dev/null

PG_STARTED="$(now_ms)"
docker exec "$PG_CONTAINER" psql -U postgres -d "$PG_DATABASE" -X -q -t -A \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_snapshot_policy_get('$WORKSPACE');" >"$TMP/pg-policy.json"
PG_POLICY_MS=$(( $(now_ms) - PG_STARTED ))
PG_STARTED="$(now_ms)"
docker exec "$PG_CONTAINER" psql -U postgres -d "$PG_DATABASE" -X -q -t -A \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_snapshot_prune('$WORKSPACE',1);" >"$TMP/pg-dry.json"
PG_DRY_MS=$(( $(now_ms) - PG_STARTED ))
PG_STARTED="$(now_ms)"
docker exec "$PG_CONTAINER" psql -U postgres -d "$PG_DATABASE" -X -q -t -A \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_snapshot_prune('$WORKSPACE',0);" >"$TMP/pg-prune.json"
PG_PRUNE_MS=$(( $(now_ms) - PG_STARTED ))
(( PG_POLICY_MS <= MAX_QUERY_MS && PG_DRY_MS <= MAX_QUERY_MS && PG_PRUNE_MS <= MAX_QUERY_MS )) || \
    fail "PG 查询超时：policy=${PG_POLICY_MS}ms dry=${PG_DRY_MS}ms prune=${PG_PRUNE_MS}ms"

PG_REMAINING="$(docker exec "$PG_CONTAINER" psql -U postgres -d "$PG_DATABASE" \
    -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT count(*),
            count(*) FILTER(WHERE snapshot_type='manual'),
            count(*) FILTER(WHERE snapshot_type='agent'),
            count(*) FILTER(WHERE snapshot_type='safety')
       FROM _vexfs.snapshots
      WHERE workspace_id=(SELECT workspace_id FROM _vexfs.workspaces WHERE name='$WORKSPACE');")"
"$ROOT/tests/eval/vexfs/python.sh" -c \
    'import json,sys
count=int(sys.argv[1]); expected=int(sys.argv[2])
policy=json.load(open(sys.argv[3])); dry=json.load(open(sys.argv[4])); prune=json.load(open(sys.argv[5]))
assert policy["snapshots"]["total"] == count + 1
assert dry["candidate_count"] == expected and dry["deleted"] == 0
assert len(dry["candidates"]) == min(100, expected)
assert dry["truncated"] is (expected > 100)
assert prune["deleted"] == expected
' "$COUNT" "$EXPECTED_CANDIDATES" \
    "$TMP/pg-policy.json" "$TMP/pg-dry.json" "$TMP/pg-prune.json"
[[ "$PG_REMAINING" == "$((AGENT_KEEP + SAFETY_KEEP + 1))|1|$AGENT_KEEP|$SAFETY_KEEP" ]] || \
    fail "PG 剩余快照分类不正确：$PG_REMAINING"

OOM_AFTER="$(oom_kills)"
MEMORY_AFTER="$(memory_value memory.current)"
[[ "$OOM_AFTER" == "$OOM_BEFORE" ]] || fail "测试期间发生 oom_kill"
(( MEMORY_AFTER <= PG_MEMORY_LIMIT )) || fail "PG 测试后内存超过上限：$MEMORY_AFTER"

mkdir -p "$REPORT_DIR"
printf 'engine\tsnapshots\tpolicy_ms\tdry_run_ms\tprune_ms\tmemory_bytes\toom_kills\n' >"$REPORT"
printf 'sqlite\t%s\t%s\t%s\t%s\t%s\t-\n' \
    "$COUNT" "$SQLITE_POLICY_MS" "$SQLITE_DRY_MS" "$SQLITE_PRUNE_MS" "$SQLITE_RSS" >>"$REPORT"
printf 'postgresql\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$COUNT" "$PG_POLICY_MS" "$PG_DRY_MS" "$PG_PRUNE_MS" "$MEMORY_AFTER" "$OOM_AFTER" >>"$REPORT"

echo "VEXFS SNAPSHOT POLICY PERFORMANCE: PASS ($COUNT snapshots)"
echo "sqlite policy=${SQLITE_POLICY_MS}ms dry=${SQLITE_DRY_MS}ms prune=${SQLITE_PRUNE_MS}ms rss=$SQLITE_RSS"
echo "postgresql policy=${PG_POLICY_MS}ms dry=${PG_DRY_MS}ms prune=${PG_PRUNE_MS}ms memory=$MEMORY_AFTER"
echo "report=$REPORT"
