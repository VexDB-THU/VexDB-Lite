#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
SOURCE_DB="${VEXDB_PG_BACKUP_PERF_SOURCE_DB:-vexfs_backup_perf_source}"
TARGET_DB="${VEXDB_PG_BACKUP_PERF_TARGET_DB:-vexfs_backup_perf_target}"
WORKSPACE="pg-backup-performance"
PAYLOAD_BYTES="${VEXDB_PG_BACKUP_PERF_BYTES:-16777216}"
FILE_COUNT="${VEXDB_PG_BACKUP_PERF_FILES:-16}"
PG_BIN="${VEXDB_PG_BIN:-/opt/pg19/bin}"
CLONE_PORT="${VEXDB_PG_BACKUP_PERF_CLONE_PORT:-55433}"
HOST_PORT="${VEXDB_PG_HOST_PORT:-}"
CLI="${VEXFS_EVAL_MOUNT_CLI:-$ROOT/vexdb_sqlite/build/vexdb}"
MAX_CLI_RSS_BYTES="${VEXDB_PG_BACKUP_PERF_MAX_CLI_RSS_BYTES:-536870912}"
MIN_LOGICAL_DUMP_MIB_S="${VEXDB_PG_BACKUP_PERF_MIN_LOGICAL_DUMP_MIB_S:-1.0}"
MIN_LOGICAL_RESTORE_MIB_S="${VEXDB_PG_BACKUP_PERF_MIN_LOGICAL_RESTORE_MIB_S:-0.5}"
MIN_PHYSICAL_MIB_S="${VEXDB_PG_BACKUP_PERF_MIN_PHYSICAL_MIB_S:-1.0}"
MIN_ARCHIVE_EXPORT_MIB_S="${VEXDB_PG_BACKUP_PERF_MIN_ARCHIVE_EXPORT_MIB_S:-0.5}"
MIN_ARCHIVE_IMPORT_MIB_S="${VEXDB_PG_BACKUP_PERF_MIN_ARCHIVE_IMPORT_MIB_S:-0.5}"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-backup-perf.XXXXXX")"
DUMP_PATH="/tmp/vexfs-backup-performance.dump"
BACKUP_DIR="/tmp/vexfs-pg-backup-performance"
ARCHIVE="$TMP/postgresql-format-v2.vexfs"
SQLITE_DB="$TMP/imported.sqlite3"
CHECKS=0

cleanup() {
    local status=$?
    trap - EXIT
    docker exec "$CONTAINER" "$PG_BIN/pg_ctl" -D "$BACKUP_DIR" \
        -m immediate -w stop >/dev/null 2>&1 || true
    docker exec "$CONTAINER" "$PG_BIN/dropdb" --if-exists --force "$TARGET_DB" \
        >/dev/null 2>&1 || true
    docker exec "$CONTAINER" "$PG_BIN/dropdb" --if-exists --force "$SOURCE_DB" \
        >/dev/null 2>&1 || true
    docker exec "$CONTAINER" rm -rf "$DUMP_PATH" "$BACKUP_DIR" \
        >/dev/null 2>&1 || true
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT

fail() {
    echo "$*" >&2
    exit 1
}

assert_number_range() {
    local value=$1
    local minimum=$2
    local maximum=$3
    local name=$4
    [[ "$value" =~ ^[0-9]+$ ]] || fail "$name 必须是整数"
    (( value >= minimum && value <= maximum )) || \
        fail "$name 必须在 $minimum..$maximum"
}

now_ms() {
    perl -MTime::HiRes=time -e 'printf "%.0f\n", time() * 1000'
}

elapsed_ms() {
    local started=$1
    local ended
    ended="$(now_ms)"
    echo $((ended - started))
}

rate_mib_s() {
    local bytes=$1
    local milliseconds=$2
    awk -v b="$bytes" -v ms="$milliseconds" \
        'BEGIN { if (ms <= 0) print "0.000"; else printf "%.3f", b / 1048576.0 / (ms / 1000.0) }'
}

assert_rate() {
    local actual=$1
    local minimum=$2
    local label=$3
    awk -v a="$actual" -v m="$minimum" 'BEGIN { exit !(a + 0 >= m + 0) }' || \
        fail "$label 吞吐不足：实际 ${actual} MiB/s，最低 ${minimum} MiB/s"
    CHECKS=$((CHECKS + 1))
}

timed_cli() {
    local label=$1
    shift
    local time_file="$TMP/${label}.time"
    /usr/bin/time -l -o "$time_file" "$@" >/dev/null
    local elapsed rss
    elapsed="$(awk '/ real / { print int(($1 * 1000) + 0.5); exit }' "$time_file")"
    rss="$(awk '/maximum resident set size/ { print $1; exit }' "$time_file")"
    [[ "$elapsed" =~ ^[0-9]+$ ]] || fail "$label 没有得到耗时"
    [[ "$rss" =~ ^[0-9]+$ ]] || fail "$label 没有得到峰值内存"
    (( rss <= MAX_CLI_RSS_BYTES )) || \
        fail "$label 峰值内存过高：$rss bytes，限制 $MAX_CLI_RSS_BYTES bytes"
    CHECKS=$((CHECKS + 1))
    printf '%s|%s\n' "$elapsed" "$rss"
}

assert_number_range "$PAYLOAD_BYTES" 1048576 134217728 VEXDB_PG_BACKUP_PERF_BYTES
assert_number_range "$FILE_COUNT" 1 1024 VEXDB_PG_BACKUP_PERF_FILES
(( PAYLOAD_BYTES % FILE_COUNT == 0 )) || \
    fail "VEXDB_PG_BACKUP_PERF_BYTES 必须能被 VEXDB_PG_BACKUP_PERF_FILES 整除"
CHUNK_BYTES=$((PAYLOAD_BYTES / FILE_COUNT))
(( CHUNK_BYTES <= 8388608 )) || fail "单文件不能超过 8 MiB，请增加文件数"

[ -x "$CLI" ] || fail "找不到 vexdb CLI：$CLI"
docker inspect "$CONTAINER" >/dev/null
if [ -z "$HOST_PORT" ]; then
    HOST_BINDING="$(docker port "$CONTAINER" 5432/tcp 2>/dev/null | sed -n '1p')"
    HOST_PORT="${HOST_BINDING##*:}"
    case "$HOST_PORT" in
        ''|*[!0-9]*)
            fail "无法从容器 $CONTAINER 推导 PostgreSQL 端口；请设置 VEXDB_PG_HOST_PORT"
            ;;
    esac
fi

MEMORY_MAX="$(docker exec "$CONTAINER" cat /sys/fs/cgroup/memory.max)"
[[ "$MEMORY_MAX" =~ ^[0-9]+$ ]] || fail "PG 容器没有有限的 memory.max：$MEMORY_MAX"
(( MEMORY_MAX <= 1073741824 )) || \
    fail "PG 性能容器内存上限必须不大于 1 GiB，当前为 $MEMORY_MAX bytes"
OOM_BEFORE="$(docker exec "$CONTAINER" awk '$1=="oom_kill" {print $2}' /sys/fs/cgroup/memory.events)"
CHECKS=$((CHECKS + 2))

docker exec "$CONTAINER" "$PG_BIN/pg_ctl" -D "$BACKUP_DIR" \
    -m immediate -w stop >/dev/null 2>&1 || true
docker exec "$CONTAINER" rm -rf "$DUMP_PATH" "$BACKUP_DIR" >/dev/null 2>&1 || true
docker exec "$CONTAINER" "$PG_BIN/dropdb" --if-exists --force "$TARGET_DB" \
    >/dev/null 2>&1 || true
docker exec "$CONTAINER" "$PG_BIN/dropdb" --if-exists --force "$SOURCE_DB" \
    >/dev/null 2>&1 || true
docker exec "$CONTAINER" "$PG_BIN/createdb" "$SOURCE_DB"

docker exec -i "$CONTAINER" "$PG_BIN/psql" -d "$SOURCE_DB" -X -q \
    -v ON_ERROR_STOP=1 -v file_count="$FILE_COUNT" -v chunk_bytes="$CHUNK_BYTES" \
    >/dev/null <<'SQL'
CREATE EXTENSION vexdb_lite VERSION '1.0';
SELECT public.vexfs_workspace_create('pg-backup-performance');
SELECT public.vexfs_mkdir('pg-backup-performance', '/payload', true);
SELECT count(public.vexfs_write(
    'pg-backup-performance',
    '/payload/file-' || lpad(i::text, 4, '0') || '.bin',
    decode(repeat(lpad(to_hex(i % 256), 2, '0'), :'chunk_bytes'::integer), 'hex')))
  FROM generate_series(1, :'file_count'::integer) AS g(i);
SELECT public.vexfs_snapshot_create('pg-backup-performance', 'performance-baseline');
SQL

SOURCE_STATE="$(docker exec "$CONTAINER" "$PG_BIN/psql" -d "$SOURCE_DB" \
    -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT sum((public.vexfs_stat('$WORKSPACE', '/payload/' || name)->>'size')::bigint),
            count(*),
            (public.vexfs_check('$WORKSPACE',1)->>'ok')::boolean,
            (SELECT count(*) FROM public.vexfs_snapshot_list('$WORKSPACE'))
       FROM public.vexfs_list('$WORKSPACE','/payload');")"
[[ "$SOURCE_STATE" == "$PAYLOAD_BYTES|$FILE_COUNT|t|1" ]] || \
    fail "PG 性能数据源状态错误：$SOURCE_STATE"
CHECKS=$((CHECKS + 4))

STARTED="$(now_ms)"
docker exec "$CONTAINER" "$PG_BIN/pg_dump" --format=custom \
    --file="$DUMP_PATH" "$SOURCE_DB"
LOGICAL_DUMP_MS="$(elapsed_ms "$STARTED")"
LOGICAL_DUMP_RATE="$(rate_mib_s "$PAYLOAD_BYTES" "$LOGICAL_DUMP_MS")"
assert_rate "$LOGICAL_DUMP_RATE" "$MIN_LOGICAL_DUMP_MIB_S" "pg_dump"

docker exec "$CONTAINER" "$PG_BIN/createdb" "$TARGET_DB"
STARTED="$(now_ms)"
docker exec "$CONTAINER" "$PG_BIN/pg_restore" --exit-on-error \
    --single-transaction --dbname="$TARGET_DB" "$DUMP_PATH"
LOGICAL_RESTORE_MS="$(elapsed_ms "$STARTED")"
LOGICAL_RESTORE_RATE="$(rate_mib_s "$PAYLOAD_BYTES" "$LOGICAL_RESTORE_MS")"
assert_rate "$LOGICAL_RESTORE_RATE" "$MIN_LOGICAL_RESTORE_MIB_S" "pg_restore"

LOGICAL_STATE="$(docker exec "$CONTAINER" "$PG_BIN/psql" -d "$TARGET_DB" \
    -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT sum((public.vexfs_stat('$WORKSPACE', '/payload/' || name)->>'size')::bigint),
            count(*),
            (public.vexfs_check('$WORKSPACE',1)->>'ok')::boolean
       FROM public.vexfs_list('$WORKSPACE','/payload');")"
[[ "$LOGICAL_STATE" == "$PAYLOAD_BYTES|$FILE_COUNT|t" ]] || \
    fail "逻辑恢复后的文件状态错误：$LOGICAL_STATE"
CHECKS=$((CHECKS + 3))
docker exec "$CONTAINER" "$PG_BIN/dropdb" --force "$TARGET_DB" >/dev/null

STARTED="$(now_ms)"
docker exec "$CONTAINER" "$PG_BIN/pg_basebackup" -D "$BACKUP_DIR" \
    -U postgres --checkpoint=fast --wal-method=stream --no-slot
PHYSICAL_MS="$(elapsed_ms "$STARTED")"
PHYSICAL_BYTES="$(( $(docker exec "$CONTAINER" du -sk "$BACKUP_DIR" | awk '{print $1}') * 1024 ))"
PHYSICAL_RATE="$(rate_mib_s "$PHYSICAL_BYTES" "$PHYSICAL_MS")"
assert_rate "$PHYSICAL_RATE" "$MIN_PHYSICAL_MIB_S" "pg_basebackup"
docker exec "$CONTAINER" "$PG_BIN/pg_verifybackup" "$BACKUP_DIR" >/dev/null
CHECKS=$((CHECKS + 1))

docker exec "$CONTAINER" "$PG_BIN/pg_ctl" -D "$BACKUP_DIR" \
    -o "-p $CLONE_PORT -k /tmp -c listen_addresses=''" \
    -l "$BACKUP_DIR/clone.log" -w start >/dev/null
PHYSICAL_STATE="$(docker exec "$CONTAINER" "$PG_BIN/psql" -h /tmp -p "$CLONE_PORT" \
    -d "$SOURCE_DB" -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT sum((public.vexfs_stat('$WORKSPACE', '/payload/' || name)->>'size')::bigint),
            count(*),
            (public.vexfs_check('$WORKSPACE',1)->>'ok')::boolean
       FROM public.vexfs_list('$WORKSPACE','/payload');")"
[[ "$PHYSICAL_STATE" == "$PAYLOAD_BYTES|$FILE_COUNT|t" ]] || \
    fail "物理恢复后的文件状态错误：$PHYSICAL_STATE"
CHECKS=$((CHECKS + 3))
docker exec "$CONTAINER" "$PG_BIN/pg_ctl" -D "$BACKUP_DIR" \
    -m fast -w stop >/dev/null

PG_DSN="${VEXDB_PG_DSN:-postgresql://postgres@127.0.0.1:${HOST_PORT}/${SOURCE_DB}}"
PG_CLI=("$CLI" fs --backend pg --dsn "$PG_DSN" --workspace "$WORKSPACE")
IFS='|' read -r ARCHIVE_EXPORT_MS ARCHIVE_EXPORT_RSS < <(
    timed_cli archive-export "${PG_CLI[@]}" export --output "$ARCHIVE")
ARCHIVE_EXPORT_RATE="$(rate_mib_s "$PAYLOAD_BYTES" "$ARCHIVE_EXPORT_MS")"
assert_rate "$ARCHIVE_EXPORT_RATE" "$MIN_ARCHIVE_EXPORT_MIB_S" "format v2 导出"
ARCHIVE_VERIFY="$("${PG_CLI[@]}" archive verify "$ARCHIVE")"
[[ "$ARCHIVE_VERIFY" == *'"ok":true'* ]] || fail "format v2 包校验失败：$ARCHIVE_VERIFY"
CHECKS=$((CHECKS + 1))

SQLITE_CLI=("$CLI" fs --db "$SQLITE_DB" --workspace pg-backup-performance-import)
IFS='|' read -r ARCHIVE_IMPORT_MS ARCHIVE_IMPORT_RSS < <(
    timed_cli archive-import "${SQLITE_CLI[@]}" import "$ARCHIVE")
ARCHIVE_IMPORT_RATE="$(rate_mib_s "$PAYLOAD_BYTES" "$ARCHIVE_IMPORT_MS")"
assert_rate "$ARCHIVE_IMPORT_RATE" "$MIN_ARCHIVE_IMPORT_MIB_S" "format v2 导入"

SQLITE_STATE="$("$CLI" "$SQLITE_DB" \
    "SELECT sum(json_extract(value,'$.size')),
            count(*),
            json_extract(vexfs_check('pg-backup-performance-import',1),'$.ok')
       FROM json_each(vexfs_list('pg-backup-performance-import','/payload'));")"
[[ "$SQLITE_STATE" == "$PAYLOAD_BYTES|$FILE_COUNT|1" ]] || \
    fail "format v2 导入 SQLite 后的文件状态错误：$SQLITE_STATE"
CHECKS=$((CHECKS + 3))

OOM_AFTER="$(docker exec "$CONTAINER" awk '$1=="oom_kill" {print $2}' /sys/fs/cgroup/memory.events)"
[[ "$OOM_AFTER" == "$OOM_BEFORE" ]] || \
    fail "性能测试触发了容器 OOM：测试前 $OOM_BEFORE，测试后 $OOM_AFTER"
CHECKS=$((CHECKS + 1))

echo "VEXFS PG BACKUP PERFORMANCE: PASS ($CHECKS checks)"
echo "  payload_bytes=$PAYLOAD_BYTES files=$FILE_COUNT container_memory_max=$MEMORY_MAX oom_kill=$OOM_AFTER"
echo "  logical_dump_ms=$LOGICAL_DUMP_MS logical_dump_mib_s=$LOGICAL_DUMP_RATE"
echo "  logical_restore_ms=$LOGICAL_RESTORE_MS logical_restore_mib_s=$LOGICAL_RESTORE_RATE"
echo "  physical_backup_bytes=$PHYSICAL_BYTES physical_backup_ms=$PHYSICAL_MS physical_backup_mib_s=$PHYSICAL_RATE"
echo "  archive_export_ms=$ARCHIVE_EXPORT_MS archive_export_mib_s=$ARCHIVE_EXPORT_RATE archive_export_rss_bytes=$ARCHIVE_EXPORT_RSS"
echo "  archive_import_ms=$ARCHIVE_IMPORT_MS archive_import_mib_s=$ARCHIVE_IMPORT_RATE archive_import_rss_bytes=$ARCHIVE_IMPORT_RSS"
