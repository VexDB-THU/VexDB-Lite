#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DATABASE="${VEXDB_PG_DATABASE:-test}"
FILES="${VEXFS_CHECKPOINT_FILES:-10000}"
SNAPSHOTS="${VEXFS_CHECKPOINT_SNAPSHOTS:-30}"
MAX_BASELINE_MS="${VEXFS_CHECKPOINT_MAX_BASELINE_MS:-5000}"
MAX_DELTA_P95_MS="${VEXFS_CHECKPOINT_MAX_DELTA_P95_MS:-500}"
MAX_RESTORE_MS="${VEXFS_CHECKPOINT_MAX_RESTORE_MS:-10000}"
MAX_EXPORT_MS="${VEXFS_CHECKPOINT_MAX_EXPORT_MS:-10000}"
MAX_GC_MS="${VEXFS_CHECKPOINT_MAX_GC_MS:-10000}"
MAX_CONTAINER_BYTES="${VEXFS_CHECKPOINT_MAX_CONTAINER_BYTES:-1073741824}"
WORKSPACE="eval-checkpoint-$$"
REPORT_DIR="$ROOT/build/eval/vexfs"
REPORT="$REPORT_DIR/pg_snapshot_checkpoint_${FILES}_${SNAPSHOTS}.tsv"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-checkpoint.XXXXXX")"

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
    echo "VEXFS PG SNAPSHOT CHECKPOINT PERFORMANCE: FAIL: $*" >&2
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

now_ms() {
    "$ROOT/tests/eval/vexfs/python.sh" -c \
        'import time; print(time.monotonic_ns() // 1000000)'
}

[[ "$FILES" =~ ^[0-9]+$ ]] || fail "文件数必须是整数"
[[ "$SNAPSHOTS" =~ ^[0-9]+$ ]] || fail "快照数必须是整数"
(( FILES >= 1000 && FILES <= 100000 && FILES % 1000 == 0 )) || \
    fail "文件数必须在 1000..100000，且是 1000 的倍数"
(( SNAPSHOTS >= 2 && SNAPSHOTS <= 1000 )) || \
    fail "快照数必须在 2..1000"
docker inspect "$CONTAINER" >/dev/null

CONTAINER_LIMIT="$(memory_value memory.max)"
[[ "$CONTAINER_LIMIT" =~ ^[0-9]+$ ]] || \
    fail "PG 测试容器必须设置 memory.max"
(( CONTAINER_LIMIT <= MAX_CONTAINER_BYTES )) || \
    fail "PG 容器 memory.max=$CONTAINER_LIMIT，超过上限 $MAX_CONTAINER_BYTES"
OOM_BEFORE="$(oom_kills)"

DIRS=$((FILES / 1000))
docker exec -i "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -v workspace="$WORKSPACE" -v dirs="$DIRS" \
    -v snapshots="$SNAPSHOTS" >"$TMP/result.tsv" <<'SQL'
CREATE TEMP TABLE checkpoint_eval_timing(
    phase text NOT NULL,
    ordinal integer NOT NULL,
    elapsed_ms double precision NOT NULL);

SELECT set_config('vexfs.eval_workspace', :'workspace', false);
SELECT set_config('vexfs.eval_dirs', :'dirs', false);
SELECT set_config('vexfs.eval_snapshots', :'snapshots', false);
SELECT public.vexfs_workspace_create(:'workspace');
DO $body$
DECLARE
    v_workspace text := current_setting('vexfs.eval_workspace');
    v_dirs integer := current_setting('vexfs.eval_dirs')::integer;
    v_dir integer;
BEGIN
    FOR v_dir IN 0..v_dirs - 1 LOOP
        PERFORM public.vexfs_mkdir(
            v_workspace, '/d-' || lpad(v_dir::text, 3, '0'), false);
        PERFORM public.vexfs_create_batch(
            v_workspace,
            '/d-' || lpad(v_dir::text, 3, '0'),
            (SELECT jsonb_agg(jsonb_build_object(
                        'name', 'file-' || lpad(item::text, 4, '0') || '.txt'))
               FROM generate_series(1, 1000) AS item));
    END LOOP;
END;
$body$;

DO $body$
DECLARE
    v_workspace text := current_setting('vexfs.eval_workspace');
    v_snapshots integer := current_setting('vexfs.eval_snapshots')::integer;
    v_started timestamptz;
    v_ordinal integer;
BEGIN
    v_started := clock_timestamp();
    PERFORM public.vexfs_snapshot_create(v_workspace, 'baseline');
    INSERT INTO checkpoint_eval_timing
    VALUES ('baseline', 0,
            extract(epoch FROM clock_timestamp() - v_started) * 1000.0);

    FOR v_ordinal IN 1..v_snapshots - 1 LOOP
        PERFORM public.vexfs_write(
            v_workspace,
            '/d-000/file-0001.txt',
            convert_to('version-' || v_ordinal::text, 'UTF8'));
        v_started := clock_timestamp();
        PERFORM public.vexfs_snapshot_create(
            v_workspace, 'delta-' || lpad(v_ordinal::text, 4, '0'));
        INSERT INTO checkpoint_eval_timing
        VALUES ('delta', v_ordinal,
                extract(epoch FROM clock_timestamp() - v_started) * 1000.0);
    END LOOP;
END;
$body$;

WITH workspace AS (
    SELECT workspace_id, root_inode, head_commit
      FROM _vexfs.workspaces WHERE name = :'workspace'),
metric AS (
    SELECT
        (SELECT elapsed_ms FROM checkpoint_eval_timing WHERE phase = 'baseline')
            AS baseline_ms,
        (SELECT percentile_cont(0.95) WITHIN GROUP (ORDER BY elapsed_ms)
           FROM checkpoint_eval_timing WHERE phase = 'delta') AS delta_p95_ms,
        (SELECT max(elapsed_ms)
           FROM checkpoint_eval_timing WHERE phase = 'delta') AS delta_max_ms,
        (SELECT count(*) FROM _vexfs.inode_states AS state
          JOIN workspace USING (workspace_id)) AS inode_state_rows,
        (SELECT count(*) FROM _vexfs.dentry_states AS state
          JOIN workspace USING (workspace_id)) AS dentry_state_rows,
        (SELECT max(checkpoint.inode_changes)
           FROM _vexfs.metadata_checkpoints AS checkpoint
          JOIN workspace USING (workspace_id)
          WHERE checkpoint.commit_no > (
              SELECT min(commit_no) FROM _vexfs.metadata_checkpoints
               WHERE workspace_id = checkpoint.workspace_id)) AS max_delta_inodes,
        (SELECT count(*) FROM _vexfs.snapshot_inodes AS inode
          JOIN _vexfs.snapshots AS snapshot USING (snapshot_id)
          JOIN workspace USING (workspace_id)
         WHERE snapshot.name = 'baseline') AS resolved_baseline_inodes,
        (SELECT count(*) FROM _vexfs.snapshots AS snapshot
          JOIN workspace USING (workspace_id)) AS snapshot_count,
        (SELECT head_commit FROM workspace) AS head_commit)
SELECT baseline_ms,
       delta_p95_ms,
       delta_max_ms,
       inode_state_rows,
       dentry_state_rows,
       max_delta_inodes,
       resolved_baseline_inodes,
       snapshot_count,
       head_commit
  FROM metric;
SQL

RESULT="$(tail -n 1 "$TMP/result.tsv" | tr -d '[:space:]')"
IFS='|' read -r BASELINE_MS DELTA_P95_MS DELTA_MAX_MS INODE_STATES DENTRY_STATES \
    MAX_DELTA_INODES RESOLVED_INODES SNAPSHOT_COUNT HEAD_COMMIT <<<"$RESULT"

EXPECTED_INODES=$((FILES + DIRS + 1))
EXPECTED_BASE_DENTRIES=$((FILES + DIRS))
EXPECTED_INODE_STATES=$((EXPECTED_INODES + (SNAPSHOTS - 1) * 2))
FULL_COPY_EQUIVALENT=$((EXPECTED_INODES * SNAPSHOTS))

[[ "$SNAPSHOT_COUNT" == "$SNAPSHOTS" ]] || \
    fail "快照数错误：$SNAPSHOT_COUNT"
[[ "$RESOLVED_INODES" == "$EXPECTED_INODES" ]] || \
    fail "基线解析 inode 数错误：$RESOLVED_INODES，期望 $EXPECTED_INODES"
[[ "$INODE_STATES" == "$EXPECTED_INODE_STATES" ]] || \
    fail "增量 inode 状态数错误：$INODE_STATES，期望 $EXPECTED_INODE_STATES"
[[ "$DENTRY_STATES" == "$EXPECTED_BASE_DENTRIES" ]] || \
    fail "目录项状态数错误：$DENTRY_STATES，期望 $EXPECTED_BASE_DENTRIES"
(( MAX_DELTA_INODES <= 2 )) || \
    fail "单文件修改快照写入了 $MAX_DELTA_INODES 条 inode 状态"

"$ROOT/tests/eval/vexfs/python.sh" - "$BASELINE_MS" "$DELTA_P95_MS" \
    "$MAX_BASELINE_MS" "$MAX_DELTA_P95_MS" <<'PY'
import sys
baseline, delta, max_baseline, max_delta = map(float, sys.argv[1:])
if baseline > max_baseline:
    raise SystemExit(f"baseline {baseline:.3f}ms > {max_baseline:.3f}ms")
if delta > max_delta:
    raise SystemExit(f"delta p95 {delta:.3f}ms > {max_delta:.3f}ms")
PY

EXPORT_STARTED="$(now_ms)"
EXPORT_COUNTS="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT count(*),
            count(*) FILTER (WHERE record_type='inode_states'),
            count(*) FILTER (WHERE record_type='dentry_states'),
            count(*) FILTER (WHERE record_type='xattr_states'),
            count(*) FILTER (WHERE record_type='acl_states')
       FROM public.vexfs_archive_export_records('$WORKSPACE',NULL);" )"
EXPORT_MS=$(( $(now_ms) - EXPORT_STARTED ))
IFS='|' read -r EXPORT_RECORDS EXPORT_INODE_STATES EXPORT_DENTRY_STATES \
    EXPORT_XATTR_STATES EXPORT_ACL_STATES <<<"$EXPORT_COUNTS"
[[ "$EXPORT_INODE_STATES" == "$INODE_STATES" ]] || \
    fail "归档 inode 状态重新膨胀：$EXPORT_INODE_STATES，期望 $INODE_STATES"
[[ "$EXPORT_DENTRY_STATES" == "$DENTRY_STATES" ]] || \
    fail "归档目录项状态重新膨胀：$EXPORT_DENTRY_STATES，期望 $DENTRY_STATES"
"$ROOT/tests/eval/vexfs/python.sh" - "$EXPORT_MS" "$MAX_EXPORT_MS" <<'PY'
import sys
actual, maximum = map(float, sys.argv[1:])
if actual > maximum:
    raise SystemExit(f"export {actual:.3f}ms > {maximum:.3f}ms")
PY

RESTORE_STARTED="$(now_ms)"
RESTORE_COMMIT="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_snapshot_restore(
        '$WORKSPACE','baseline',$HEAD_COMMIT);")"
RESTORE_MS=$(( $(now_ms) - RESTORE_STARTED ))
RESTORED_SIZE="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -v ON_ERROR_STOP=1 -c \
    "SELECT octet_length(public.vexfs_read(
        '$WORKSPACE','/d-000/file-0001.txt'));" )"
[[ "$RESTORED_SIZE" == "0" ]] || fail "恢复后文件内容不属于基线"
"$ROOT/tests/eval/vexfs/python.sh" - "$RESTORE_MS" "$MAX_RESTORE_MS" <<'PY'
import sys
actual, maximum = map(float, sys.argv[1:])
if actual > maximum:
    raise SystemExit(f"restore {actual:.3f}ms > {maximum:.3f}ms")
PY

printf -v KEEP_SNAPSHOT 'delta-%04d' "$((SNAPSHOTS - 1))"
docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
    -v ON_ERROR_STOP=1 -c \
    "WITH candidates AS MATERIALIZED (
         SELECT name FROM public.vexfs_snapshot_list('$WORKSPACE')
          WHERE name <> '$KEEP_SNAPSHOT')
     SELECT count(public.vexfs_snapshot_drop('$WORKSPACE',name))
       FROM candidates;" >/dev/null
GC_STARTED="$(now_ms)"
GC_RESULT="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT (result->>'metadata_floor_commit')::bigint,
            (result->>'metadata_deleted_checkpoints')::bigint,
            (result->>'metadata_deleted_state_rows')::bigint
       FROM (SELECT public.vexfs_gc('$WORKSPACE',10000) AS result) AS gc;" )"
GC_MS=$(( $(now_ms) - GC_STARTED ))
IFS='|' read -r GC_FLOOR GC_CHECKPOINTS GC_STATE_ROWS <<<"$GC_RESULT"
[[ "$GC_FLOOR" == "$HEAD_COMMIT" ]] || \
    fail "metadata floor 错误：$GC_FLOOR，期望 $HEAD_COMMIT"
(( GC_STATE_ROWS > 0 )) || fail "metadata GC 没有压实旧状态"
GC_STATE="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT count(*) FILTER (WHERE state.commit_no < workspace.history_floor_commit),
            count(DISTINCT snapshot.snapshot_id),
            workspace.history_floor_commit
       FROM _vexfs.workspaces AS workspace
       LEFT JOIN _vexfs.inode_states AS state USING(workspace_id)
       LEFT JOIN _vexfs.snapshots AS snapshot USING(workspace_id)
      WHERE workspace.name='$WORKSPACE'
      GROUP BY workspace.history_floor_commit;" )"
[[ "$GC_STATE" == "0|1|$HEAD_COMMIT" ]] || \
    fail "metadata GC 后状态不正确：$GC_STATE"
"$ROOT/tests/eval/vexfs/python.sh" - "$GC_MS" "$MAX_GC_MS" <<'PY'
import sys
actual, maximum = map(float, sys.argv[1:])
if actual > maximum:
    raise SystemExit(f"metadata gc {actual:.3f}ms > {maximum:.3f}ms")
PY

LATEST_COMMIT="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_snapshot_restore(
        '$WORKSPACE','$KEEP_SNAPSHOT',$RESTORE_COMMIT);" )"
LATEST_CONTENT="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -v ON_ERROR_STOP=1 -c \
    "SELECT convert_from(public.vexfs_read(
        '$WORKSPACE','/d-000/file-0001.txt'),'UTF8');" )"
[[ "$LATEST_CONTENT" == "version-$((SNAPSHOTS - 1))" ]] || \
    fail "压实后保留快照恢复错误：$LATEST_CONTENT"

OOM_AFTER="$(oom_kills)"
MEMORY_AFTER="$(memory_value memory.current)"
[[ "$OOM_AFTER" == "$OOM_BEFORE" ]] || fail "测试期间发生 oom_kill"
(( MEMORY_AFTER <= MAX_CONTAINER_BYTES )) || \
    fail "测试后容器内存 $MEMORY_AFTER 超过上限 $MAX_CONTAINER_BYTES"

mkdir -p "$REPORT_DIR"
printf 'files\tsnapshots\tbaseline_ms\tdelta_p95_ms\tdelta_max_ms\texport_ms\trestore_ms\tmetadata_gc_ms\tinode_state_rows\tfull_copy_equivalent\tcompression_ratio\texport_records\tmemory_bytes\toom_kills\n' >"$REPORT"
"$ROOT/tests/eval/vexfs/python.sh" - "$FILES" "$SNAPSHOTS" "$BASELINE_MS" \
    "$DELTA_P95_MS" "$DELTA_MAX_MS" "$EXPORT_MS" "$RESTORE_MS" "$GC_MS" "$INODE_STATES" \
    "$FULL_COPY_EQUIVALENT" "$EXPORT_RECORDS" "$MEMORY_AFTER" "$OOM_AFTER" >>"$REPORT" <<'PY'
import sys
files, snapshots, baseline, p95, maximum, export, restore, gc, states, full, records, memory, oom = sys.argv[1:]
ratio = int(full) / int(states)
print("\t".join((files, snapshots, baseline, p95, maximum, export, restore, gc, states,
                 full, f"{ratio:.2f}", records, memory, oom)))
PY

echo "VEXFS PG SNAPSHOT CHECKPOINT PERFORMANCE: PASS"
echo "files=$FILES snapshots=$SNAPSHOTS baseline=${BASELINE_MS}ms delta_p95=${DELTA_P95_MS}ms export=${EXPORT_MS}ms restore=${RESTORE_MS}ms metadata_gc=${GC_MS}ms"
echo "inode_states=$INODE_STATES full_copy_equivalent=$FULL_COPY_EQUIVALENT report=$REPORT"
