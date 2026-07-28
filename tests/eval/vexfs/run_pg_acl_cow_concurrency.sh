#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DATABASE="${VEXDB_PG_DATABASE:-test}"
WORKERS="${VEXFS_PG_ACL_WORKERS:-16}"
MEMORY_LIMIT_BYTES="${VEXFS_PG_ACL_MEMORY_LIMIT_BYTES:-1073741824}"
WORKSPACE="eval-acl-cow-$$"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-acl-cow.XXXXXX")"
REPORT_DIR="$ROOT/build/eval/vexfs"
REPORT="$REPORT_DIR/pg_acl_cow_concurrency.tsv"

cleanup() {
    local status=$?
    trap - EXIT
    docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
        -c "SELECT public.vexfs_workspace_drop('$WORKSPACE', true);" \
        >/dev/null 2>&1 || true
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

fail() {
    echo "VEXFS PG ACL COW CONCURRENCY: FAIL: $*" >&2
    exit 1
}

memory_value() {
    local file=$1
    docker exec "$CONTAINER" sh -c \
        "if [ -r /sys/fs/cgroup/$file ]; then cat /sys/fs/cgroup/$file; else echo unavailable; fi"
}

oom_kills() {
    docker exec "$CONTAINER" sh -c \
        "if [ -r /sys/fs/cgroup/memory.events ]; then awk '\$1==\"oom_kill\" {print \$2}' /sys/fs/cgroup/memory.events; else echo 0; fi"
}

[[ "$WORKERS" =~ ^[0-9]+$ ]] || fail "workers 必须是整数"
(( WORKERS >= 2 && WORKERS <= 64 )) || fail "workers 必须在 2..64"
[[ "$MEMORY_LIMIT_BYTES" =~ ^[0-9]+$ ]] || fail "内存上限必须是整数"
docker inspect "$CONTAINER" >/dev/null
CONTAINER_LIMIT="$(memory_value memory.max)"
[[ "$CONTAINER_LIMIT" =~ ^[0-9]+$ ]] || fail "测试容器必须设置 memory.max"
(( CONTAINER_LIMIT <= MEMORY_LIMIT_BYTES )) || \
    fail "容器 memory.max=$CONTAINER_LIMIT，超过测试上限 $MEMORY_LIMIT_BYTES"
OOM_BEFORE="$(oom_kills)"
MEMORY_BEFORE="$(memory_value memory.current)"

docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_workspace_create('$WORKSPACE');
     SELECT public.vexfs_create_batch(
       '$WORKSPACE','/',
       (SELECT jsonb_agg(jsonb_build_object('name','file-'||value||'.txt') ORDER BY value)
          FROM generate_series(1,$WORKERS) AS value));" >/dev/null

run_wave() {
    local wave=$1
    local pids=()
    local index
    local status=0
    local started=$SECONDS
    for ((index=1; index<=WORKERS; index++)); do
        docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q -t -A \
            -v ON_ERROR_STOP=1 -c \
            "SELECT public.vexfs_acl_set(
               '$WORKSPACE',
               (public.vexfs_stat('$WORKSPACE','/file-$index.txt')->>'inode')::bigint,
               '[{\"principal\":\"agent-reader\",\"effect\":\"allow\",\"permissions\":\"read\",\"inherit\":0},{\"principal\":\"agent-writer\",\"effect\":\"allow\",\"permissions\":\"write\",\"inherit\":0}]');" \
            >"$TMP/$wave-$index.out" 2>"$TMP/$wave-$index.err" &
        pids+=("$!")
    done
    for index in "${!pids[@]}"; do
        wait "${pids[$index]}" || status=1
    done
    if (( status != 0 )); then
        sed -n '1,80p' "$TMP"/"$wave"-*.err >&2
        fail "$wave 并发 ACL 写入失败"
    fi
    if ! awk 'NF && $0 != "2" { exit 1 } END { if (NR != '"$WORKERS"') exit 1 }' \
            "$TMP"/"$wave"-*.out; then
        fail "$wave 返回值不正确"
    fi
    echo $((SECONDS - started))
}

FIRST_SECONDS="$(run_wave first)"
STATE="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -c \
    "SELECT w.head_commit,
            (SELECT count(*) FROM _vexfs.acl_sets s WHERE s.workspace_id=w.workspace_id),
            (SELECT count(*) FROM _vexfs.acl_set_entries e JOIN _vexfs.acl_sets s USING (acl_set_id) WHERE s.workspace_id=w.workspace_id),
            (SELECT count(DISTINCT i.acl_set_id) FROM _vexfs.inodes i WHERE i.workspace_id=w.workspace_id AND i.kind='file'),
            (SELECT count(*) FROM _vexfs.acl_entries a WHERE a.workspace_id=w.workspace_id),
            (public.vexfs_check('$WORKSPACE',1)->>'ok')::boolean
       FROM _vexfs.workspaces w WHERE w.name='$WORKSPACE';")"
EXPECTED_HEAD=$((WORKERS + 2))
EXPECTED_EXPANDED=$((WORKERS * 2))
[[ "$STATE" == "$EXPECTED_HEAD|1|2|1|$EXPECTED_EXPANDED|t" ]] || \
    fail "首次并发写入状态不正确：$STATE"

SECOND_SECONDS="$(run_wave idempotent)"
FINAL_STATE="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -c \
    "SELECT w.head_commit,
            (SELECT count(*) FROM _vexfs.acl_sets s WHERE s.workspace_id=w.workspace_id),
            (SELECT count(*) FROM _vexfs.acl_set_entries e JOIN _vexfs.acl_sets s USING (acl_set_id) WHERE s.workspace_id=w.workspace_id)
       FROM _vexfs.workspaces w WHERE w.name='$WORKSPACE';")"
[[ "$FINAL_STATE" == "$EXPECTED_HEAD|1|2" ]] || \
    fail "幂等并发写入产生了额外集合或 commit：$FINAL_STATE"

docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_snapshot_create('$WORKSPACE','acl-baseline');" >/dev/null
SNAPSHOT_STATE="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -c \
    "SELECT count(DISTINCT inode.acl_set_id),
            (SELECT count(*) FROM _vexfs.acl_set_entries entry JOIN _vexfs.acl_sets acl_set USING (acl_set_id) WHERE acl_set.workspace_id=snapshot.workspace_id)
       FROM _vexfs.snapshots snapshot
       JOIN _vexfs.snapshot_inodes inode USING (snapshot_id)
      WHERE snapshot.workspace_id=(SELECT workspace_id FROM _vexfs.workspaces WHERE name='$WORKSPACE')
      GROUP BY snapshot.workspace_id;")"
[[ "$SNAPSHOT_STATE" == "1|2" ]] || fail "快照复制了 ACL 明细：$SNAPSHOT_STATE"

OOM_AFTER="$(oom_kills)"
MEMORY_AFTER="$(memory_value memory.current)"
[[ "$OOM_AFTER" == "$OOM_BEFORE" ]] || fail "测试期间发生 oom_kill"
(( MEMORY_AFTER <= MEMORY_LIMIT_BYTES )) || fail "测试后内存超过上限：$MEMORY_AFTER"

mkdir -p "$REPORT_DIR"
printf 'workers\tfirst_seconds\tidempotent_seconds\tacl_sets\tacl_set_entries\texpanded_acl_entries\tmemory_before\tmemory_after\toom_kills\n' > "$REPORT"
printf '%s\t%s\t%s\t1\t2\t%s\t%s\t%s\t%s\n' \
    "$WORKERS" "$FIRST_SECONDS" "$SECOND_SECONDS" "$EXPECTED_EXPANDED" \
    "$MEMORY_BEFORE" "$MEMORY_AFTER" "$OOM_AFTER" >> "$REPORT"

echo "VEXFS PG ACL COW CONCURRENCY: PASS ($WORKERS workers)"
echo "report=$REPORT"
