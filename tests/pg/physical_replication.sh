#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
PG_CONFIG_BIN=${PG_CONFIG_BIN:-pg_config}
PG_BIN=${PG_BIN:-$($PG_CONFIG_BIN --bindir)}
VEXDB_LIBRARY=${VEXDB_LIBRARY:-}
VEXDB_SQL=${VEXDB_SQL:-$ROOT_DIR/vexdb_pg/sql/vexdb_lite--1.0.sql}
PRIMARY_PORT=${PRIMARY_PORT:-56432}
STANDBY_PORT=${STANDBY_PORT:-56433}

if [[ -z "$VEXDB_LIBRARY" || ! -f "$VEXDB_LIBRARY" ]]; then
    echo "VEXDB_LIBRARY must point to the built vexdb_lite shared library" >&2
    exit 2
fi

for program in initdb pg_ctl psql pg_basebackup; do
    if [[ ! -x "$PG_BIN/$program" ]]; then
        echo "missing PostgreSQL program: $PG_BIN/$program" >&2
        exit 2
    fi
done

test_root=$(mktemp -d "${TMPDIR:-/tmp}/vexdb-pg-ha.XXXXXX")
primary_data="$test_root/primary"
primary_socket="$test_root/primary-socket"
standby_data="$test_root/standby"
standby_socket="$test_root/standby-socket"
mkdir "$primary_socket" "$standby_socket"

cleanup() {
    "$PG_BIN/pg_ctl" -D "$standby_data" -m immediate stop >/dev/null 2>&1 || true
    "$PG_BIN/pg_ctl" -D "$primary_data" -m immediate stop >/dev/null 2>&1 || true
    rm -rf "$test_root"
}
trap cleanup EXIT

primary_psql() {
    "$PG_BIN/psql" -h "$primary_socket" -p "$PRIMARY_PORT" \
        -d postgres -X -v ON_ERROR_STOP=1 "$@"
}

standby_psql() {
    "$PG_BIN/psql" -h "$standby_socket" -p "$STANDBY_PORT" \
        -d postgres -X -v ON_ERROR_STOP=1 "$@"
}

"$PG_BIN/initdb" -D "$primary_data" --data-checksums -A trust >/dev/null
"$PG_BIN/pg_ctl" -D "$primary_data" -l "$test_root/primary.log" \
    -o "-p $PRIMARY_PORT -k $primary_socket -c listen_addresses=127.0.0.1 -c shared_preload_libraries=$VEXDB_LIBRARY" \
    start >/dev/null

escaped_library=${VEXDB_LIBRARY//&/\\&}
sed -n '4,$p' "$VEXDB_SQL" | \
    sed "s|MODULE_PATHNAME|$escaped_library|g" | primary_psql -f - >/dev/null

primary_psql <<'SQL' >/dev/null
CREATE TABLE ha_vectors (id int primary key, v floatvector(4));
INSERT INTO ha_vectors
SELECT i, ('[' || i || ',0,0,0]')::floatvector
FROM generate_series(1, 700) AS g(i);
CREATE INDEX ha_vectors_idx ON ha_vectors
USING vexdb_graph (v floatvector_l2_ops);

SET maintenance_work_mem = '2GB';
CREATE TABLE ha_pq (id int primary key, v floatvector(8));
INSERT INTO ha_pq
SELECT i, ('[' || (i*.01) || ',' || (i*.02) || ',' || (i*.03) || ',' ||
           (i*.04) || ',' || (i*.05) || ',' || (i*.06) || ',' ||
           (i*.07) || ',' || (i*.08) || ']')::floatvector
FROM generate_series(0, 499) AS g(i);
CREATE INDEX ha_pq_idx ON ha_pq USING vexdb_graph (v floatvector_l2_ops)
WITH (quantizer=pq, pq_m=4, memory_mode=compact);
SQL

"$PG_BIN/pg_basebackup" -h 127.0.0.1 -p "$PRIMARY_PORT" \
    -D "$standby_data" -R -X stream -c fast >/dev/null
"$PG_BIN/pg_ctl" -D "$standby_data" -l "$test_root/standby.log" \
    -o "-p $STANDBY_PORT -k $standby_socket -c hot_standby=on -c shared_preload_libraries=$VEXDB_LIBRARY" \
    start >/dev/null

basebackup_ids=$(standby_psql -At <<'SQL'
SET enable_seqscan=off;
SELECT string_agg(id::text, ',' ORDER BY rank)
FROM (
  SELECT id, row_number() OVER () AS rank
  FROM (
    SELECT id FROM ha_vectors
    ORDER BY v <-> '[1.1,0,0,0]'::floatvector LIMIT 3
  ) nearest
) ranked;
SQL
)
basebackup_ids=$(printf '%s\n' "$basebackup_ids" | tail -n 1)
[[ "$basebackup_ids" == "1,2,3" ]] || {
    echo "basebackup index query mismatch: $basebackup_ids" >&2
    exit 1
}

basebackup_pq_id=$(standby_psql -At <<'SQL'
SET enable_seqscan=off;
SELECT id FROM ha_pq
ORDER BY v <-> '[1,2,3,4,5,6,7,8]'::floatvector LIMIT 1;
SQL
)
basebackup_pq_id=$(printf '%s\n' "$basebackup_pq_id" | tail -n 1)
[[ "$basebackup_pq_id" == "100" ]] || {
    echo "basebackup PQ index query mismatch: $basebackup_pq_id" >&2
    exit 1
}

primary_psql <<'SQL' >/dev/null
INSERT INTO ha_vectors VALUES (701, '[1.05,0,0,0]');
INSERT INTO ha_vectors
SELECT i, ('[' || i || ',0,0,0]')::floatvector
FROM generate_series(702, 1200) AS g(i);
UPDATE ha_vectors SET v = '[9,0,0,0]' WHERE id = 2;
DELETE FROM ha_vectors WHERE id = 3;
VACUUM ha_vectors;

INSERT INTO ha_pq VALUES (1000, '[1,2,3,4,5,6,7,8]');
DELETE FROM ha_pq WHERE id = 100;
SQL

target_lsn=$(primary_psql -Atc 'SELECT pg_current_wal_flush_lsn()')
caught_up=f
for _ in $(seq 1 120); do
    caught_up=$(standby_psql -Atc \
        "SELECT COALESCE(pg_last_wal_replay_lsn() >= '$target_lsn'::pg_lsn, false)")
    [[ "$caught_up" == "t" ]] && break
    sleep 0.25
done
[[ "$caught_up" == "t" ]] || {
    echo "standby did not replay through $target_lsn" >&2
    exit 1
}

incremental_ids=$(standby_psql -At <<'SQL'
SET enable_seqscan=off;
SELECT string_agg(id::text, ',' ORDER BY rank)
FROM (
  SELECT id, row_number() OVER () AS rank
  FROM (
    SELECT id FROM ha_vectors
    ORDER BY v <-> '[1.1,0,0,0]'::floatvector LIMIT 4
  ) nearest
) ranked;
SQL
)
incremental_ids=$(printf '%s\n' "$incremental_ids" | tail -n 1)
[[ "$incremental_ids" == "701,1,4,5" ]] || {
    echo "streaming replication index query mismatch: $incremental_ids" >&2
    exit 1
}

incremental_pq_id=$(standby_psql -At <<'SQL'
SET enable_seqscan=off;
SELECT id FROM ha_pq
ORDER BY v <-> '[1,2,3,4,5,6,7,8]'::floatvector LIMIT 1;
SQL
)
incremental_pq_id=$(printf '%s\n' "$incremental_pq_id" | tail -n 1)
[[ "$incremental_pq_id" == "1000" ]] || {
    echo "streaming replication PQ query mismatch: $incremental_pq_id" >&2
    exit 1
}

row_count=$(standby_psql -Atc 'SELECT count(*) FROM ha_vectors')
[[ "$row_count" == "1199" ]] || {
    echo "streaming replication row count mismatch: $row_count" >&2
    exit 1
}

echo "PASS: pg_basebackup and streaming replay preserved vexdb_graph queries"
