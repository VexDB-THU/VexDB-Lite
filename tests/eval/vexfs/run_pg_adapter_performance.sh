#!/usr/bin/env bash
set -euo pipefail

CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DATABASE="${VEXDB_PG_DATABASE:-test}"
FILE_COUNT="${VEXDB_PG_PERF_FILES:-1000}"
WORKSPACE="pg-performance"

if ! [[ "$FILE_COUNT" =~ ^[0-9]+$ ]] || (( FILE_COUNT < 1 || FILE_COUNT > 10000 )); then
    echo "VEXDB_PG_PERF_FILES 必须在 1..10000" >&2
    exit 2
fi

docker inspect "$CONTAINER" >/dev/null

RESULT="$(docker exec -i "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -v file_count="$FILE_COUNT" <<'SQL' | tail -n 1
SELECT set_config('vexfs.perf_files', :'file_count', false);
DO $$
DECLARE
    v_count integer := current_setting('vexfs.perf_files')::integer;
    v_started timestamptz;
    v_elapsed_ms double precision;
    v_written bigint;
BEGIN
    PERFORM public.vexfs_workspace_drop('pg-performance', true);
    PERFORM public.vexfs_workspace_create('pg-performance');
    CREATE TEMP TABLE vexfs_perf_result(elapsed_ms double precision) ON COMMIT PRESERVE ROWS;
    v_started := clock_timestamp();
    SELECT count(public.vexfs_write(
        'pg-performance',
        '/f-' || i::text || '.txt',
        convert_to(i::text, 'UTF8')))
      INTO v_written
      FROM generate_series(1, v_count) AS g(i);
    v_elapsed_ms := extract(epoch FROM clock_timestamp() - v_started) * 1000.0;
    IF v_written <> v_count THEN
        RAISE EXCEPTION 'write count mismatch: expected %, actual %', v_count, v_written;
    END IF;
    INSERT INTO vexfs_perf_result VALUES (v_elapsed_ms);
END;
$$;
SELECT round(r.elapsed_ms::numeric, 3),
       (public.vexfs_workspace_stat('pg-performance')->>'head_commit')::bigint,
       (SELECT count(*) FROM public.vexfs_list('pg-performance', '/')),
       min(c.commit_no),
       max(c.commit_no),
       count(c.commit_no),
       (SELECT count(*) FROM _vexfs.audit_events AS audit
         WHERE audit.workspace_id = w.workspace_id),
       (SELECT bool_and(audit.path IS NOT NULL
                        AND audit.inode_id IS NOT NULL
                        AND audit.details ? 'before_version'
                        AND audit.details ? 'after_version')
          FROM _vexfs.audit_events AS audit
         WHERE audit.workspace_id = w.workspace_id)
  FROM vexfs_perf_result AS r
  JOIN _vexfs.workspaces AS w ON w.name = 'pg-performance'
  JOIN _vexfs.commits AS c ON c.workspace_id = w.workspace_id
 GROUP BY r.elapsed_ms, w.workspace_id;
DO $$ BEGIN PERFORM public.vexfs_workspace_drop('pg-performance', true); END $$;
SQL
)"

IFS='|' read -r ELAPSED_MS HEAD FILES MIN_COMMIT MAX_COMMIT COMMITS AUDITS AUDIT_COMPLETE <<<"$RESULT"
EXPECTED_HEAD=$((FILE_COUNT + 1))
if [[ "$HEAD" != "$EXPECTED_HEAD" || "$FILES" != "$FILE_COUNT" || \
      "$MIN_COMMIT" != 1 || "$MAX_COMMIT" != "$EXPECTED_HEAD" || \
      "$COMMITS" != "$EXPECTED_HEAD" || "$AUDITS" != "$EXPECTED_HEAD" || \
      "$AUDIT_COMPLETE" != t ]]; then
    echo "PG 性能基线状态错误：$RESULT" >&2
    exit 1
fi

RATE="$(awk -v n="$FILE_COUNT" -v ms="$ELAPSED_MS" 'BEGIN { if (ms <= 0) print 0; else printf "%.3f", n * 1000 / ms }')"
echo "VEXFS PG PERFORMANCE: PASS (files=$FILE_COUNT elapsed_ms=$ELAPSED_MS files_per_second=$RATE checks=7)"
