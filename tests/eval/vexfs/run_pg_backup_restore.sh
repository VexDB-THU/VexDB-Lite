#!/usr/bin/env bash
set -euo pipefail

CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
SOURCE_DB="${VEXDB_PG_BACKUP_SOURCE_DB:-vexfs_backup_source_eval}"
TARGET_DB="${VEXDB_PG_BACKUP_TARGET_DB:-vexfs_backup_target_eval}"
DUMP_PATH="/tmp/vexfs-backup-restore-eval.dump"
CHECKS=0

cleanup() {
    docker exec "$CONTAINER" dropdb --if-exists --force "$TARGET_DB" >/dev/null 2>&1 || true
    docker exec "$CONTAINER" dropdb --if-exists --force "$SOURCE_DB" >/dev/null 2>&1 || true
    docker exec "$CONTAINER" rm -f "$DUMP_PATH" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker inspect "$CONTAINER" >/dev/null
cleanup

DEFAULT_VERSION="$(docker exec "$CONTAINER" psql -d postgres -X -q -t -A \
    -v ON_ERROR_STOP=1 -c \
    "SELECT default_version FROM pg_available_extensions WHERE name='vexdb_lite';")"
if [[ "$DEFAULT_VERSION" != "1.0" ]]; then
    echo "PG 扩展默认版本错误：期望 1.0，实际 ${DEFAULT_VERSION:-missing}。" >&2
    echo "pg_dump 恢复时按默认版本安装扩展，控制文件与 SQL 版本必须一起发布。" >&2
    exit 1
fi

docker exec "$CONTAINER" createdb "$SOURCE_DB"

docker exec -i "$CONTAINER" psql -d "$SOURCE_DB" -X -q -v ON_ERROR_STOP=1 \
    >/dev/null <<'SQL'
CREATE EXTENSION pg_trgm;
CREATE EXTENSION vexdb_lite VERSION '1.0';
CREATE TABLE public.vexfs_backup_business(
    id integer PRIMARY KEY,
    checkpoint text NOT NULL
);
INSERT INTO public.vexfs_backup_business VALUES (1, 'same-transaction-boundary');
SELECT public.vexfs_workspace_create('backup-source');
SELECT public.vexfs_write(
    'backup-source',
    '/workspace.bin',
    convert_to(repeat('a', 70000) || '-before', 'UTF8'));
SELECT public.vexfs_mkdir('backup-source', '/links', true);
SELECT public.vexfs_link(
    'backup-source', '/workspace.bin', '/links/workspace-hard.bin');
SELECT public.vexfs_symlink(
    'backup-source', '/links/current', convert_to('../workspace.bin', 'UTF8'));
SELECT public.vexfs_set_mode(
    'backup-source',
    (public.vexfs_stat('backup-source', '/workspace.bin')->>'inode')::bigint,
    493);
SELECT public.vexfs_chown(
    'backup-source',
    (public.vexfs_stat('backup-source', '/workspace.bin')->>'inode')::bigint,
    501,
    20);
SELECT public.vexfs_xattr_set(
    'backup-source',
    (public.vexfs_stat('backup-source', '/workspace.bin')->>'inode')::bigint,
    'user.backup',
    convert_to('metadata-survives', 'UTF8'),
    1);
SELECT public.vexfs_acl_grant(
    'backup-source',
    (public.vexfs_stat('backup-source', '/workspace.bin')->>'inode')::bigint,
    'public',
    'read',
    'allow',
    0);
SELECT public.vexfs_snapshot_create('backup-source', 'baseline');
SELECT public.vexfs_write(
    'backup-source',
    '/workspace.bin',
    convert_to(repeat('b', 70000) || '-after', 'UTF8'));
SELECT public.vexfs_quota_set('backup-source', 200000, 10, 100000);
SELECT public.vexfs_retention_set('backup-source', 1, 0);
SELECT public.vexfs_grep_index('backup-source', 'enable');
SQL

SOURCE_STATE="$(docker exec "$CONTAINER" psql -d "$SOURCE_DB" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -c \
    "SELECT (public.vexfs_check('backup-source',1)->>'ok')::boolean,
            (SELECT count(*) FROM _vexfs.manifests),
            (SELECT count(*) FROM _vexfs.chunks),
            (public.vexfs_workspace_stat('backup-source')->>'head_commit')::bigint,
            (public.vexfs_grep_index('backup-source','status')->>'enabled')::boolean,
            (public.vexfs_grep_index('backup-source','status')->>'dirty')::boolean,
            (public.vexfs_grep_index('backup-source','status')->>'indexed_files')::integer,
            (public.vexfs_grep('backup-source','/','after',0,10)->>'index_used')::boolean;")"
if [[ "$SOURCE_STATE" != "t|3|5|10|t|f|1|t" ]]; then
    echo "PG 备份源状态错误：$SOURCE_STATE" >&2
    exit 1
fi
CHECKS=$((CHECKS + 8))

docker exec "$CONTAINER" pg_dump \
    --format=custom \
    --file="$DUMP_PATH" \
    "$SOURCE_DB"
docker exec "$CONTAINER" createdb "$TARGET_DB"
docker exec "$CONTAINER" pg_restore \
    --exit-on-error \
    --single-transaction \
    --dbname="$TARGET_DB" \
    "$DUMP_PATH"

TARGET_STATE="$(docker exec "$CONTAINER" psql -d "$TARGET_DB" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_pg_adapter_version(),
            public.vexfs_read('backup-source','/workspace.bin') =
              convert_to(repeat('b',70000) || '-after','UTF8'),
            public.vexfs_read_version('backup-source','/workspace.bin',1) =
              convert_to(repeat('a',70000) || '-before','UTF8'),
            (SELECT count(*) FROM public.vexfs_snapshot_list('backup-source')),
            (public.vexfs_quota_get('backup-source')->>'max_bytes')::bigint,
            (public.vexfs_retention_get('backup-source')->>'keep_versions')::integer,
            (SELECT checkpoint FROM public.vexfs_backup_business WHERE id=1),
            (public.vexfs_check('backup-source',1)->>'ok')::boolean,
            (public.vexfs_stat('backup-source','/workspace.bin')->>'inode') =
              (public.vexfs_stat('backup-source','/links/workspace-hard.bin')->>'inode'),
            convert_from(public.vexfs_readlink(
              'backup-source',
              (public.vexfs_stat('backup-source','/links/current')->>'inode')::bigint),
              'UTF8'),
            convert_from(public.vexfs_xattr_get(
              'backup-source',
              (public.vexfs_stat('backup-source','/workspace.bin')->>'inode')::bigint,
              'user.backup'), 'UTF8'),
            public.vexfs_stat('backup-source','/workspace.bin')->>'mode',
            public.vexfs_stat('backup-source','/workspace.bin')->>'uid',
            public.vexfs_stat('backup-source','/workspace.bin')->>'gid',
            (public.vexfs_acl_get(
              'backup-source',
              (public.vexfs_stat('backup-source','/workspace.bin')->>'inode')::bigint)
              @> '[{\"principal\":\"public\",\"effect\":\"allow\",\"permissions\":\"read\"}]'::jsonb),
            (SELECT count(*) >= 9 FROM public.vexfs_audit_list(
              'backup-source', 1000, NULL)),
            (SELECT bool_and(actor IS NOT NULL AND path IS NOT NULL
                             AND inode_id IS NOT NULL
                             AND details ? 'before_version'
                             AND details ? 'after_version')
               FROM public.vexfs_audit_list('backup-source', 1000, NULL)),
            (SELECT path FROM public.vexfs_audit_list('backup-source', 1000, NULL)
              WHERE operation='write' ORDER BY event_id DESC LIMIT 1),
            (SELECT details->>'before_version' FROM public.vexfs_audit_list(
              'backup-source', 1000, NULL)
              WHERE operation='write' ORDER BY event_id DESC LIMIT 1),
            (SELECT details->>'after_version' FROM public.vexfs_audit_list(
              'backup-source', 1000, NULL)
              WHERE operation='write' ORDER BY event_id DESC LIMIT 1),
            (SELECT actor::text FROM public.vexfs_audit_list(
              'backup-source', 1000, NULL)
              WHERE operation='write' ORDER BY event_id DESC LIMIT 1),
            (public.vexfs_grep_index('backup-source','status')->>'enabled')::boolean,
            (public.vexfs_grep_index('backup-source','status')->>'dirty')::boolean,
            (public.vexfs_grep_index('backup-source','status')->>'indexed_files')::integer,
            (public.vexfs_grep('backup-source','/','after',0,10)->>'index_used')::boolean,
            (public.vexfs_grep('backup-source','/','after',0,10)->>'match_count')::integer;")"
if [[ "$TARGET_STATE" != \
      "0.4.0-alpha.1|t|t|1|200000|1|same-transaction-boundary|t|t|../workspace.bin|metadata-survives|493|501|20|t|t|t|/workspace.bin|1|2|postgres|t|t|0|f|2" ]]; then
    echo "PG 恢复后状态错误：$TARGET_STATE" >&2
    exit 1
fi
CHECKS=$((CHECKS + 26))

INDEX_REBUILD_ACTION="$(docker exec "$CONTAINER" psql -d "$TARGET_DB" -X -q -t -A \
    -v ON_ERROR_STOP=1 -c \
    "SELECT (public.vexfs_grep_index('backup-source','rebuild')->>'dirty')::boolean;")"
if [[ "$INDEX_REBUILD_ACTION" != "f" ]]; then
    echo "PG 恢复后索引重建动作错误：$INDEX_REBUILD_ACTION" >&2
    exit 1
fi
INDEX_REBUILD_STATE="$(docker exec "$CONTAINER" psql -d "$TARGET_DB" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -c \
    "SELECT (public.vexfs_grep_index('backup-source','status')->>'indexed_files')::integer,
            (public.vexfs_grep('backup-source','/','after',0,10)->>'index_used')::boolean;")"
if [[ "$INDEX_REBUILD_STATE" != "1|t" ]]; then
    echo "PG 恢复后索引重建状态错误：$INDEX_REBUILD_STATE" >&2
    exit 1
fi
CHECKS=$((CHECKS + 3))

RESTORE_STATE="$(docker exec "$CONTAINER" psql -d "$TARGET_DB" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_snapshot_restore('backup-source','baseline',10);
     SELECT (public.vexfs_workspace_stat('backup-source')->>'head_commit')::bigint,
            public.vexfs_read('backup-source','/workspace.bin') =
              convert_to(repeat('a',70000) || '-before','UTF8'),
            (public.vexfs_check('backup-source',1)->>'ok')::boolean,
            (public.vexfs_stat('backup-source','/workspace.bin')->>'inode') =
              (public.vexfs_stat('backup-source','/links/workspace-hard.bin')->>'inode'),
            convert_from(public.vexfs_xattr_get(
              'backup-source',
              (public.vexfs_stat('backup-source','/workspace.bin')->>'inode')::bigint,
              'user.backup'), 'UTF8'),
            (public.vexfs_grep('backup-source','/','before',0,10)->>'index_used')::boolean,
            (public.vexfs_grep('backup-source','/','before',0,10)->>'match_count')::integer;")"
RESTORE_STATE="$(printf '%s\n' "$RESTORE_STATE" | tail -n 1)"
if [[ "$RESTORE_STATE" != "11|t|t|t|metadata-survives|t|2" ]]; then
    echo "PG 恢复库快照状态错误：$RESTORE_STATE" >&2
    exit 1
fi
CHECKS=$((CHECKS + 7))

IDENTITY_STATE="$(docker exec "$CONTAINER" psql -d "$TARGET_DB" -X -q -t -A \
    -v ON_ERROR_STOP=1 -c \
    "SELECT public.vexfs_workspace_create('after-restore') >
            (SELECT workspace_id FROM _vexfs.workspaces WHERE name='backup-source');")"
if [[ "$IDENTITY_STATE" != "t" ]]; then
    echo "PG 恢复后 identity sequence 没有继续递增：$IDENTITY_STATE" >&2
    exit 1
fi
CHECKS=$((CHECKS + 1))

echo "VEXFS PG BACKUP RESTORE: PASS ($CHECKS checks)"
