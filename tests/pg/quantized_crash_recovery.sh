#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
PG_CONFIG_BIN=${PG_CONFIG_BIN:-pg_config}
PG_BIN=${PG_BIN:-$($PG_CONFIG_BIN --bindir)}
VEXDB_LIBRARY=${VEXDB_LIBRARY:-}
VEXDB_SQL=${VEXDB_SQL:-$ROOT_DIR/vexdb_pg/sql/vexdb_lite--1.0.sql}
PG_PORT=${PG_PORT:-56442}

if [[ -z "$VEXDB_LIBRARY" || ! -f "$VEXDB_LIBRARY" ]]; then
    echo "VEXDB_LIBRARY must point to the built vexdb_lite shared library" >&2
    exit 2
fi

for program in initdb pg_ctl psql; do
    if [[ ! -x "$PG_BIN/$program" ]]; then
        echo "missing PostgreSQL program: $PG_BIN/$program" >&2
        exit 2
    fi
done

test_root=$(mktemp -d "${TMPDIR:-/tmp}/vexdb-pg-quant-recovery.XXXXXX")
data_dir="$test_root/data"
socket_dir="$test_root/socket"
mkdir "$socket_dir"

cleanup() {
    "$PG_BIN/pg_ctl" -D "$data_dir" -m immediate stop >/dev/null 2>&1 || true
    rm -rf "$test_root"
}
trap cleanup EXIT

psql_cmd() {
    "$PG_BIN/psql" -h "$socket_dir" -p "$PG_PORT" \
        -d postgres -X -v ON_ERROR_STOP=1 "$@"
}

start_server() {
    "$PG_BIN/pg_ctl" -D "$data_dir" -l "$test_root/postgres.log" \
        -o "-p $PG_PORT -k $socket_dir -c listen_addresses='' -c shared_preload_libraries=$VEXDB_LIBRARY" \
        start >/dev/null
}

"$PG_BIN/initdb" -D "$data_dir" --data-checksums -A trust >/dev/null
start_server

escaped_library=${VEXDB_LIBRARY//&/\\&}
sed -n '4,$p' "$VEXDB_SQL" | \
    sed "s|MODULE_PATHNAME|$escaped_library|g" | psql_cmd -f - >/dev/null

psql_cmd <<'SQL' >/dev/null
SET maintenance_work_mem = '2GB';
SET max_parallel_maintenance_workers = 0;

CREATE TABLE recovery_pq (id int primary key, v floatvector(8));
INSERT INTO recovery_pq
SELECT i, ('[' || (i*.01) || ',' || (i*.02) || ',' || (i*.03) || ',' ||
           (i*.04) || ',' || (i*.05) || ',' || (i*.06) || ',' ||
           (i*.07) || ',' || (i*.08) || ']')::floatvector
FROM generate_series(0, 499) AS g(i);

CREATE FUNCTION __recovery_rq_vec16(i int) RETURNS floatvector
LANGUAGE SQL IMMUTABLE STRICT AS $$
  SELECT array_agg(
    (sin(i * 0.013 + j * 0.17) +
     cos(i * 0.007 * (j + 1)) +
     ((i % 97) * (j + 1)) * 0.00001)::float4
    ORDER BY j
  )::floatvector
  FROM generate_series(0, 15) AS j
$$;
CREATE TABLE recovery_rq (id int primary key, v floatvector(16));
INSERT INTO recovery_rq
SELECT i, __recovery_rq_vec16(i)
FROM generate_series(1, 12000) AS g(i);
ANALYZE recovery_rq;

CHECKPOINT;

CREATE INDEX recovery_pq_idx ON recovery_pq
USING vexdb_graph (v floatvector_l2_ops)
WITH (quantizer=pq, pq_m=4, memory_mode=compact);
CREATE INDEX recovery_rq_idx ON recovery_rq
USING vexdb_graph (v floatvector_l2_ops)
WITH (quantizer=rabitq, memory_mode=compact, m=16, ef_construction=64);

INSERT INTO recovery_pq VALUES (1000, '[1,2,3,4,5,6,7,8]');
DELETE FROM recovery_pq WHERE id = 100;
INSERT INTO recovery_rq VALUES
  (20000, array_fill(50::float4, ARRAY[16])::floatvector);
DELETE FROM recovery_rq WHERE id = 5000;
SQL

# No checkpoint after either CREATE INDEX or incremental DML. Recovery must
# reconstruct graph pages, quantizer metadata and compact code pages from WAL.
"$PG_BIN/pg_ctl" -D "$data_dir" -m immediate stop >/dev/null
start_server

pq_result=$(psql_cmd -At <<'SQL'
SET enable_seqscan=off;
SET vexdb.ef_search=100;
SELECT id FROM recovery_pq
ORDER BY v <-> '[1,2,3,4,5,6,7,8]'::floatvector LIMIT 1;
SQL
)
pq_result=$(printf '%s\n' "$pq_result" | tail -n 1)
[[ "$pq_result" == "1000" ]] || {
    echo "PQ crash-recovery query mismatch: $pq_result" >&2
    exit 1
}

rq_result=$(psql_cmd -At <<'SQL'
SET enable_seqscan=off;
SET vexdb.ef_search=100;
SELECT id FROM recovery_rq
ORDER BY v <-> array_fill(50::float4, ARRAY[16])::floatvector LIMIT 1;
SQL
)
rq_result=$(printf '%s\n' "$rq_result" | tail -n 1)
[[ "$rq_result" == "20000" ]] || {
    echo "RaBitQ crash-recovery query mismatch: $rq_result" >&2
    exit 1
}

pq_count=$(psql_cmd -Atc 'SELECT count(*) FROM recovery_pq')
rq_count=$(psql_cmd -Atc 'SELECT count(*) FROM recovery_rq')
[[ "$pq_count" == "500" ]] || {
    echo "PQ crash-recovery row count mismatch: $pq_count" >&2
    exit 1
}
[[ "$rq_count" == "12000" ]] || {
    echo "RaBitQ crash-recovery row count mismatch: $rq_count" >&2
    exit 1
}

"$PG_BIN/pg_ctl" -D "$data_dir" -m fast stop >/dev/null
if [[ -x "$PG_BIN/pg_checksums" ]]; then
    "$PG_BIN/pg_checksums" --check -D "$data_dir" >/dev/null
fi

echo "PASS: crash recovery preserved PQ and RaBitQ metadata, codes and DML"
