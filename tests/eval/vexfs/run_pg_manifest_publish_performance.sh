#!/usr/bin/env bash
set -euo pipefail

CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DATABASE="${VEXDB_PG_DATABASE:-test}"
FILE_BYTES="${VEXDB_PG_MANIFEST_FILE_BYTES:-16777216}"
PATCH_BYTES="${VEXDB_PG_MANIFEST_PATCH_BYTES:-4096}"
ROUNDS="${VEXDB_PG_MANIFEST_ROUNDS:-5}"
MAX_MEDIAN_MS="${VEXDB_PG_MANIFEST_MAX_MEDIAN_MS:-250}"
WORKSPACE="pg-manifest-performance"

fail() {
    echo "$*" >&2
    exit 1
}

assert_integer_range() {
    local value=$1
    local minimum=$2
    local maximum=$3
    local name=$4
    [[ "$value" =~ ^[0-9]+$ ]] || fail "$name 必须是整数"
    (( value >= minimum && value <= maximum )) || \
        fail "$name 必须在 $minimum..$maximum"
}

assert_integer_range "$FILE_BYTES" 1048576 67108864 VEXDB_PG_MANIFEST_FILE_BYTES
assert_integer_range "$PATCH_BYTES" 1 65536 VEXDB_PG_MANIFEST_PATCH_BYTES
assert_integer_range "$ROUNDS" 1 20 VEXDB_PG_MANIFEST_ROUNDS
awk -v value="$MAX_MEDIAN_MS" 'BEGIN { exit !(value > 0) }' || \
    fail "VEXDB_PG_MANIFEST_MAX_MEDIAN_MS 必须大于 0"

docker inspect "$CONTAINER" >/dev/null
MEMORY_MAX="$(docker exec "$CONTAINER" cat /sys/fs/cgroup/memory.max)"
[[ "$MEMORY_MAX" =~ ^[0-9]+$ ]] || \
    fail "PG 性能容器必须设置有限的 memory.max，当前为 $MEMORY_MAX"
(( MEMORY_MAX <= 1073741824 )) || \
    fail "PG 性能容器内存上限必须不大于 1 GiB，当前为 $MEMORY_MAX bytes"
OOM_BEFORE="$(docker exec "$CONTAINER" awk '$1=="oom_kill" {print $2}' /sys/fs/cgroup/memory.events)"

cleanup() {
    local status=$?
    trap - EXIT
    docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
        -v ON_ERROR_STOP=1 \
        -c "SELECT public.vexfs_workspace_drop('$WORKSPACE', true);" \
        >/dev/null 2>&1 || true
    exit "$status"
}
trap cleanup EXIT

RESULT="$(docker exec -i "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -F '|' -v ON_ERROR_STOP=1 \
    -v file_bytes="$FILE_BYTES" -v patch_bytes="$PATCH_BYTES" \
    -v rounds="$ROUNDS" <<'SQL' | tail -n 1
SELECT set_config('vexfs.manifest_file_bytes', :'file_bytes', false);
SELECT set_config('vexfs.manifest_patch_bytes', :'patch_bytes', false);
SELECT set_config('vexfs.manifest_rounds', :'rounds', false);
SELECT public.vexfs_workspace_drop('pg-manifest-performance', true);
SELECT public.vexfs_workspace_create('pg-manifest-performance');
SELECT public.vexfs_write(
    'pg-manifest-performance', '/large.bin',
    decode(repeat('61', current_setting('vexfs.manifest_file_bytes')::integer), 'hex'));
CREATE TEMP TABLE manifest_publish_timings(
    round_no integer PRIMARY KEY,
    elapsed_ms double precision NOT NULL) ON COMMIT PRESERVE ROWS;
DO $$
DECLARE
    v_round integer;
    v_handle text;
    v_generation bigint;
    v_started timestamptz;
    v_elapsed_ms double precision;
    v_patch bytea;
BEGIN
    FOR v_round IN 1..current_setting('vexfs.manifest_rounds')::integer LOOP
        v_handle := public.vexfs_handle_open(
            'pg-manifest-performance', '/large.bin', 'rw',
            'manifest-open-' || v_round::text);
        v_patch := decode(repeat(
            lpad(pg_catalog.to_hex(97 + v_round), 2, '0'),
            current_setting('vexfs.manifest_patch_bytes')::integer), 'hex');
        v_generation := public.vexfs_handle_stage_write(
            v_handle,
            current_setting('vexfs.manifest_file_bytes')::bigint / 2,
            v_patch,
            'manifest-stage-' || v_round::text);
        v_started := clock_timestamp();
        PERFORM public.vexfs_handle_publish(
            v_handle, v_generation, 'data',
            'manifest-publish-' || v_round::text);
        v_elapsed_ms := extract(epoch FROM clock_timestamp() - v_started) * 1000.0;
        INSERT INTO manifest_publish_timings VALUES (v_round, v_elapsed_ms);
        PERFORM public.vexfs_handle_close(
            v_handle, false, 'manifest-close-' || v_round::text);
    END LOOP;
END;
$$;
SELECT round(percentile_cont(0.5) WITHIN GROUP (ORDER BY timing.elapsed_ms)::numeric, 3),
       round(min(timing.elapsed_ms)::numeric, 3),
       round(max(timing.elapsed_ms)::numeric, 3),
       (SELECT count(*) FROM _vexfs.file_versions AS version
         WHERE version.workspace_id = workspace.workspace_id),
       (SELECT count(*) FROM _vexfs.manifests AS manifest
         WHERE manifest.workspace_id = workspace.workspace_id),
       (SELECT count(*) FROM _vexfs.chunks AS chunk
         WHERE chunk.workspace_id = workspace.workspace_id),
       (public.vexfs_check('pg-manifest-performance', 1)->>'ok')::boolean,
       position('read_staging_content' IN pg_get_functiondef(
           'public.vexfs_handle_publish(text,bigint,text,text)'::regprocedure)) = 0,
       (public.vexfs_diagnostics('pg-manifest-performance')->>'pending_handles')::bigint,
       (public.vexfs_diagnostics('pg-manifest-performance')->>'staging_bytes')::bigint
  FROM manifest_publish_timings AS timing
 CROSS JOIN _vexfs.workspaces AS workspace
 WHERE workspace.name = 'pg-manifest-performance'
 GROUP BY workspace.workspace_id;
SQL
)"

IFS='|' read -r MEDIAN_MS MIN_MS MAX_MS VERSIONS MANIFESTS CHUNKS \
    CHECK_OK NO_FULL_ASSEMBLY PENDING_HANDLES STAGING_BYTES <<<"$RESULT"
EXPECTED_VERSIONS=$((ROUNDS + 1))
if [[ "$VERSIONS" != "$EXPECTED_VERSIONS" || \
      "$MANIFESTS" != "$EXPECTED_VERSIONS" || \
      "$CHUNKS" != "$EXPECTED_VERSIONS" || \
      "$CHECK_OK" != t || "$NO_FULL_ASSEMBLY" != t || \
      "$PENDING_HANDLES" != 0 || "$STAGING_BYTES" != 0 ]]; then
    fail "PG manifest 发布状态错误：$RESULT"
fi
awk -v actual="$MEDIAN_MS" -v maximum="$MAX_MEDIAN_MS" \
    'BEGIN { exit !(actual <= maximum) }' || \
    fail "PG manifest 发布中位耗时过高：${MEDIAN_MS} ms，限制 ${MAX_MEDIAN_MS} ms"

OOM_AFTER="$(docker exec "$CONTAINER" awk '$1=="oom_kill" {print $2}' /sys/fs/cgroup/memory.events)"
[[ "$OOM_AFTER" == "$OOM_BEFORE" ]] || \
    fail "PG manifest eval 触发了 OOM：before=$OOM_BEFORE after=$OOM_AFTER"

echo "VEXFS PG MANIFEST PUBLISH: PASS (file_bytes=$FILE_BYTES patch_bytes=$PATCH_BYTES rounds=$ROUNDS median_ms=$MEDIAN_MS min_ms=$MIN_MS max_ms=$MAX_MS memory_max=$MEMORY_MAX oom_kill=$OOM_AFTER checks=10)"
