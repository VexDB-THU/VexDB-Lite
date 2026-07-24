#!/usr/bin/env bash
set -euo pipefail

CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
SOURCE_DB="${VEXDB_PG_PHYSICAL_SOURCE_DB:-vexfs_physical_source_eval}"
BACKUP_DIR="${VEXDB_PG_PHYSICAL_BACKUP_DIR:-/tmp/vexfs-pg-physical-backup}"
CLONE_PORT="${VEXDB_PG_PHYSICAL_CLONE_PORT:-55432}"
PG_BIN="${VEXDB_PG_BIN:-/opt/pg19/bin}"
CHECKS=0

cleanup() {
    local status=$?
    trap - EXIT
    docker exec "$CONTAINER" "$PG_BIN/pg_ctl" -D "$BACKUP_DIR" \
        -m immediate -w stop >/dev/null 2>&1 || true
    docker exec "$CONTAINER" "$PG_BIN/dropdb" --if-exists --force "$SOURCE_DB" \
        >/dev/null 2>&1 || true
    docker exec "$CONTAINER" rm -rf "$BACKUP_DIR" >/dev/null 2>&1 || true
    exit "$status"
}
trap cleanup EXIT

assert_equal() {
    local actual=$1
    local expected=$2
    local message=$3
    if [[ "$actual" != "$expected" ]]; then
        echo "${message}：期望 ${expected}，实际 ${actual}" >&2
        exit 1
    fi
    CHECKS=$((CHECKS + 1))
}

docker inspect "$CONTAINER" >/dev/null
docker exec "$CONTAINER" "$PG_BIN/pg_ctl" -D "$BACKUP_DIR" \
    -m immediate -w stop >/dev/null 2>&1 || true
docker exec "$CONTAINER" rm -rf "$BACKUP_DIR" >/dev/null 2>&1 || true
docker exec "$CONTAINER" "$PG_BIN/dropdb" --if-exists --force "$SOURCE_DB" \
    >/dev/null 2>&1 || true
docker exec "$CONTAINER" "$PG_BIN/createdb" "$SOURCE_DB"

docker exec -i "$CONTAINER" "$PG_BIN/psql" -d "$SOURCE_DB" -X -q \
    -v ON_ERROR_STOP=1 >/dev/null <<'SQL'
CREATE EXTENSION pg_trgm;
CREATE EXTENSION vexdb_lite VERSION '1.0';
CREATE TABLE public.physical_backup_business(
    id integer PRIMARY KEY,
    checkpoint text NOT NULL
);
INSERT INTO public.physical_backup_business VALUES (1, 'before-base-backup');
SELECT public.vexfs_workspace_create('physical-source');
SELECT public.vexfs_mkdir('physical-source', '/project', true);
SELECT public.vexfs_write(
    'physical-source', '/project/state.txt', convert_to('physical-v1', 'UTF8'));
SELECT public.vexfs_link(
    'physical-source', '/project/state.txt', '/project/state-hard.txt');
SELECT public.vexfs_symlink(
    'physical-source', '/project/state-link.txt', convert_to('state.txt', 'UTF8'));
SELECT public.vexfs_set_mode(
    'physical-source',
    (public.vexfs_stat('physical-source', '/project/state.txt')->>'inode')::bigint,
    488);
SELECT public.vexfs_xattr_set(
    'physical-source',
    (public.vexfs_stat('physical-source', '/project/state.txt')->>'inode')::bigint,
    'user.physical', convert_to('base-backup-metadata', 'UTF8'), 1);
SELECT public.vexfs_acl_grant(
    'physical-source',
    (public.vexfs_stat('physical-source', '/project/state.txt')->>'inode')::bigint,
    'public', 'read', 'allow', 0);
SELECT public.vexfs_snapshot_create('physical-source', 'baseline');
SELECT public.vexfs_write(
    'physical-source', '/project/state.txt', convert_to('physical-v2', 'UTF8'));
SELECT public.vexfs_grep_index('physical-source', 'enable');
SQL

SOURCE_STATE="$(docker exec "$CONTAINER" "$PG_BIN/psql" -d "$SOURCE_DB" \
    -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT convert_from(public.vexfs_read('physical-source','/project/state.txt'),'UTF8'),
            convert_from(public.vexfs_read_version(
              'physical-source','/project/state.txt',1),'UTF8'),
            (SELECT count(*) FROM public.vexfs_snapshot_list('physical-source')),
            (public.vexfs_check('physical-source',1)->>'ok')::boolean,
            (SELECT checkpoint FROM public.physical_backup_business WHERE id=1),
            (public.vexfs_grep('physical-source','/project','physical-v2',0,10)->>'index_used')::boolean,
            (public.vexfs_grep_index('physical-source','status')->>'indexed_files')::integer;")"
assert_equal "$SOURCE_STATE" "physical-v2|physical-v1|1|t|before-base-backup|t|1" \
    "物理备份源状态错误"

docker exec "$CONTAINER" "$PG_BIN/pg_basebackup" \
    -D "$BACKUP_DIR" \
    -U postgres \
    --checkpoint=fast \
    --wal-method=stream \
    --no-slot

# 备份完成后再写入源库；恢复副本必须停在备份完成时的一致边界。
docker exec "$CONTAINER" "$PG_BIN/psql" -d "$SOURCE_DB" -X -q \
    -v ON_ERROR_STOP=1 -c \
    "INSERT INTO public.physical_backup_business VALUES (2, 'after-base-backup');
     SELECT public.vexfs_write(
       'physical-source','/project/after-backup.txt',convert_to('too-new','UTF8'));" \
    >/dev/null

docker exec "$CONTAINER" "$PG_BIN/pg_ctl" -D "$BACKUP_DIR" \
    -o "-p $CLONE_PORT -k /tmp -c listen_addresses=''" \
    -l "$BACKUP_DIR/clone.log" -w start >/dev/null

CLONE_STATE="$(docker exec "$CONTAINER" "$PG_BIN/psql" \
    -h /tmp -p "$CLONE_PORT" -d "$SOURCE_DB" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -c \
    "SELECT convert_from(public.vexfs_read('physical-source','/project/state.txt'),'UTF8'),
            convert_from(public.vexfs_read_version(
              'physical-source','/project/state.txt',1),'UTF8'),
            (SELECT count(*) FROM public.vexfs_snapshot_list('physical-source')),
            (public.vexfs_check('physical-source',1)->>'ok')::boolean,
            (SELECT checkpoint FROM public.physical_backup_business WHERE id=1),
            NOT EXISTS(SELECT 1 FROM public.physical_backup_business WHERE id=2),
            public.vexfs_stat('physical-source','/project/state.txt')->>'mode',
            (public.vexfs_stat('physical-source','/project/state.txt')->>'inode') =
              (public.vexfs_stat('physical-source','/project/state-hard.txt')->>'inode'),
            convert_from(public.vexfs_readlink(
              'physical-source',
              (public.vexfs_stat('physical-source','/project/state-link.txt')->>'inode')::bigint),
              'UTF8'),
            convert_from(public.vexfs_xattr_get(
              'physical-source',
              (public.vexfs_stat('physical-source','/project/state.txt')->>'inode')::bigint,
              'user.physical'), 'UTF8'),
            (public.vexfs_acl_get(
              'physical-source',
              (public.vexfs_stat('physical-source','/project/state.txt')->>'inode')::bigint)
              @> '[{\"principal\":\"public\",\"effect\":\"allow\",\"permissions\":\"read\"}]'::jsonb),
            NOT EXISTS(
              SELECT 1
                FROM _vexfs.dentries AS d
                JOIN _vexfs.workspaces AS w USING (workspace_id)
               WHERE w.name='physical-source' AND d.name='after-backup.txt'),
            (public.vexfs_grep_index('physical-source','status')->>'enabled')::boolean,
            (public.vexfs_grep_index('physical-source','status')->>'dirty')::boolean,
            (public.vexfs_grep('physical-source','/project','physical-v2',0,10)->>'index_used')::boolean,
            (public.vexfs_grep('physical-source','/project','physical-v2',0,10)->>'match_count')::integer;")"
assert_equal "$CLONE_STATE" \
    "physical-v2|physical-v1|1|t|before-base-backup|t|488|t|state.txt|base-backup-metadata|t|t|t|f|t|2" \
    "物理备份恢复状态错误"

RESTORE_STATE="$(docker exec "$CONTAINER" "$PG_BIN/psql" \
    -h /tmp -p "$CLONE_PORT" -d "$SOURCE_DB" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_snapshot_restore(
       'physical-source', 'baseline',
       (public.vexfs_workspace_stat('physical-source')->>'head_commit')::bigint);
     SELECT convert_from(public.vexfs_read(
              'physical-source','/project/state.txt'),'UTF8'),
            (public.vexfs_check('physical-source',1)->>'ok')::boolean,
            (public.vexfs_stat('physical-source','/project/state.txt')->>'inode') =
              (public.vexfs_stat('physical-source','/project/state-hard.txt')->>'inode'),
            convert_from(public.vexfs_xattr_get(
              'physical-source',
              (public.vexfs_stat('physical-source','/project/state.txt')->>'inode')::bigint,
              'user.physical'),'UTF8'),
            (public.vexfs_grep('physical-source','/project','physical-v1',0,10)->>'index_used')::boolean,
            (public.vexfs_grep('physical-source','/project','physical-v1',0,10)->>'match_count')::integer;")"
RESTORE_STATE="$(printf '%s\n' "$RESTORE_STATE" | tail -n 1)"
assert_equal "$RESTORE_STATE" "physical-v1|t|t|base-backup-metadata|t|2" \
    "物理恢复副本的快照还原错误"

echo "VEXFS PG PHYSICAL BACKUP RESTORE: PASS ($CHECKS groups, 29 fields)"
