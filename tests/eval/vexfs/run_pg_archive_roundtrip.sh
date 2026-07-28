#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DATABASE="${VEXDB_PG_DATABASE:-test}"
DSN="${VEXDB_PG_DSN:-}"
CLI="${VEXFS_EVAL_MOUNT_CLI:-$ROOT/vexdb_sqlite/build/vexdb}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-archive.XXXXXX")"
SQLITE_SOURCE="$TMP/sqlite-source.sqlite3"
SQLITE_TARGET="$TMP/sqlite-target.sqlite3"
SQLITE_PACKAGE="$TMP/sqlite-to-pg.vexfs"
PG_PACKAGE="$TMP/pg-to-sqlite.vexfs"
CORRUPT_PACKAGE="$TMP/corrupt.vexfs"
CHECKS=0

cleanup() {
    local status=$?
    trap - EXIT
    local workspace
    for workspace in sqlite-to-pg pg-to-sqlite corrupt-import; do
        docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
            -c "SELECT vexfs_workspace_drop('$workspace', true);" \
            >/dev/null 2>&1 || true
    done
    rm -rf "$TMP"
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

[ -x "$CLI" ] || { echo "找不到 vexdb CLI：$CLI" >&2; exit 2; }
docker inspect "$CONTAINER" >/dev/null
if [ -z "$DSN" ]; then
    HOST_BINDING="$(docker port "$CONTAINER" 5432/tcp 2>/dev/null | sed -n '1p')"
    HOST_PORT="${HOST_BINDING##*:}"
    case "$HOST_PORT" in
        ''|*[!0-9]*)
            echo "无法从容器 $CONTAINER 推导 PostgreSQL 端口；请设置 VEXDB_PG_DSN" >&2
            exit 2
            ;;
    esac
    DSN="postgresql://postgres@127.0.0.1:${HOST_PORT}/${DATABASE}"
fi

for workspace in sqlite-to-pg pg-to-sqlite corrupt-import; do
    docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
        -c "SELECT vexfs_workspace_drop('$workspace', true);" \
        >/dev/null 2>&1 || true
done

SQLITE=("$CLI" fs --db "$SQLITE_SOURCE" --workspace sqlite-source)
PG_IMPORT=("$CLI" fs --backend pg --dsn "$DSN" --workspace sqlite-to-pg)
"${SQLITE[@]}" setup >/dev/null
"${SQLITE[@]}" mkdir /tree >/dev/null
printf 'sqlite-v1' | "${SQLITE[@]}" write /tree/value.txt >/dev/null
: | "${SQLITE[@]}" write /tree/empty-a.txt >/dev/null
: | "${SQLITE[@]}" write /tree/empty-b.txt >/dev/null
"${SQLITE[@]}" ln /tree/value.txt /tree/value-hard.txt >/dev/null
printf '[{"principal":"agent-portable","effect":"allow","permissions":"read","inherit":0}]' | \
    "${SQLITE[@]}" setfacl /tree/value.txt >/dev/null
"$CLI" "$SQLITE_SOURCE" \
    "SELECT vexfs_symlink('sqlite-source','/tree/value-link.txt',CAST('value.txt' AS BLOB));
     SELECT vexfs_xattr_set(
       'sqlite-source',(vexfs_stat('sqlite-source','/tree/value.txt')->>'inode'),
       'user.cross-engine',CAST('sqlite-metadata' AS BLOB),0);" >/dev/null
"${SQLITE[@]}" snapshot create baseline >/dev/null
printf 'sqlite-v2' | "${SQLITE[@]}" write /tree/value.txt >/dev/null
"${SQLITE[@]}" export --output "$SQLITE_PACKAGE" >/dev/null

VERIFY_SQLITE_PACKAGE="$("${SQLITE[@]}" archive verify "$SQLITE_PACKAGE")"
[[ "$VERIFY_SQLITE_PACKAGE" == *'"ok":true'* ]] || {
    echo "SQLite 导出包校验失败：$VERIFY_SQLITE_PACKAGE" >&2
    exit 1
}
CHECKS=$((CHECKS + 1))
"${PG_IMPORT[@]}" import "$SQLITE_PACKAGE" >/dev/null

PG_IMPORTED="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -c \
    "SELECT convert_from(vexfs_read('sqlite-to-pg','/tree/value.txt'),'UTF8'),
            convert_from(vexfs_read_version('sqlite-to-pg','/tree/value.txt',1),'UTF8'),
            (SELECT count(*) FROM vexfs_snapshot_list('sqlite-to-pg')),
            (vexfs_stat('sqlite-to-pg','/tree/value.txt')->>'inode') =
              (vexfs_stat('sqlite-to-pg','/tree/value-hard.txt')->>'inode'),
            convert_from(vexfs_readlink(
              'sqlite-to-pg',(vexfs_stat('sqlite-to-pg','/tree/value-link.txt')->>'inode')::bigint),
              'UTF8'),
            convert_from(vexfs_xattr_get(
              'sqlite-to-pg',(vexfs_stat('sqlite-to-pg','/tree/value.txt')->>'inode')::bigint,
              'user.cross-engine'),'UTF8'),
            vexfs_acl_get(
              'sqlite-to-pg',(vexfs_stat('sqlite-to-pg','/tree/value.txt')->>'inode')::bigint)
              @> '[{\"principal\":\"agent-portable\",\"effect\":\"allow\",\"permissions\":\"read\"}]'::jsonb,
            octet_length(vexfs_read('sqlite-to-pg','/tree/empty-a.txt'))=0,
            (SELECT count(*) FROM _vexfs.manifests AS manifest
              WHERE manifest.workspace_id=(SELECT workspace_id FROM _vexfs.workspaces
                                             WHERE name='sqlite-to-pg')
                AND manifest.file_size=0 AND manifest.chunk_count=0)=1,
            EXISTS(
              SELECT 1 FROM _vexfs.commits AS commit_row
               WHERE commit_row.workspace_id=(SELECT workspace_id FROM _vexfs.workspaces
                                            WHERE name='sqlite-to-pg')
                 AND commit_row.path='/tree/value.txt'),
            jsonb_array_length(vexfs_workspace_log('sqlite-to-pg',10,NULL)->'entries')>0,
            (vexfs_check('sqlite-to-pg',1)->>'ok')::boolean;")"
assert_equal "$PG_IMPORTED" \
    "sqlite-v2|sqlite-v1|1|t|value.txt|sqlite-metadata|t|t|t|t|t|t" \
    "SQLite 到 PostgreSQL format v2 往返错误"

# 目标 workspace 已存在时必须拒绝，且不能改动已导入内容。
if "${PG_IMPORT[@]}" import "$SQLITE_PACKAGE" >/dev/null 2>&1; then
    echo "重复导入现有 PostgreSQL workspace 应该失败" >&2
    exit 1
fi
assert_equal "$("${PG_IMPORT[@]}" cat /tree/value.txt)" "sqlite-v2" \
    "重复导入失败后内容不应改变"

docker exec -i "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
    -v ON_ERROR_STOP=1 >/dev/null <<'SQL'
SELECT vexfs_workspace_create('pg-to-sqlite');
SELECT vexfs_mkdir('pg-to-sqlite','/tree',true);
SELECT vexfs_create_batch(
  'pg-to-sqlite','/tree','[{"name":"empty-a.txt"},{"name":"empty-b.txt"}]'::jsonb);
SELECT vexfs_write('pg-to-sqlite','/tree/value.txt',convert_to('pg-v1','UTF8'));
SELECT vexfs_link('pg-to-sqlite','/tree/value.txt','/tree/value-hard.txt');
SELECT vexfs_symlink('pg-to-sqlite','/tree/value-link.txt',convert_to('value.txt','UTF8'));
SELECT vexfs_xattr_set(
  'pg-to-sqlite',(vexfs_stat('pg-to-sqlite','/tree/value.txt')->>'inode')::bigint,
  'user.cross-engine',convert_to('pg-metadata','UTF8'),1);
SELECT vexfs_acl_grant(
  'pg-to-sqlite',(vexfs_stat('pg-to-sqlite','/tree/value.txt')->>'inode')::bigint,
  'agent-portable','read','allow',0);
SELECT vexfs_snapshot_create('pg-to-sqlite','baseline');
SELECT vexfs_write('pg-to-sqlite','/tree/value.txt',convert_to('pg-v2','UTF8'));
SQL

PG_EXPORT=("$CLI" fs --backend pg --dsn "$DSN" --workspace pg-to-sqlite)
"${PG_EXPORT[@]}" export --output "$PG_PACKAGE" >/dev/null
VERIFY_PG_PACKAGE="$("${PG_EXPORT[@]}" archive verify "$PG_PACKAGE")"
[[ "$VERIFY_PG_PACKAGE" == *'"ok":true'* ]] || {
    echo "PostgreSQL 导出包校验失败：$VERIFY_PG_PACKAGE" >&2
    exit 1
}
CHECKS=$((CHECKS + 1))

SQLITE_IMPORT=("$CLI" fs --db "$SQLITE_TARGET" --workspace pg-imported)
"${SQLITE_IMPORT[@]}" import "$PG_PACKAGE" >/dev/null
SQLITE_IMPORTED="$("$CLI" "$SQLITE_TARGET" \
    "SELECT CAST(vexfs_read('pg-imported','/tree/value.txt') AS TEXT),
            CAST(vexfs_read_version('pg-imported','/tree/value.txt',1) AS TEXT),
            (SELECT count(*) FROM json_each(vexfs_snapshot_list('pg-imported'))),
            json_extract(vexfs_stat('pg-imported','/tree/value.txt'),'$.inode') =
              json_extract(vexfs_stat('pg-imported','/tree/value-hard.txt'),'$.inode'),
            CAST(vexfs_readlink(
              'pg-imported',json_extract(vexfs_stat('pg-imported','/tree/value-link.txt'),'$.inode')) AS TEXT),
            CAST(vexfs_xattr_get(
              'pg-imported',json_extract(vexfs_stat('pg-imported','/tree/value.txt'),'$.inode'),
              'user.cross-engine') AS TEXT),
            EXISTS(
              SELECT 1 FROM json_each(vexfs_acl_get(
                'pg-imported',json_extract(vexfs_stat('pg-imported','/tree/value.txt'),'$.inode')))
               WHERE json_extract(value,'$.principal')='agent-portable'
                 AND json_extract(value,'$.permissions')='read'),
            length(vexfs_read('pg-imported','/tree/empty-a.txt'))=0,
            EXISTS(
              SELECT 1 FROM _vexfs_commits AS commit_row
               WHERE commit_row.workspace_id=(SELECT workspace_id FROM _vexfs_workspaces
                                            WHERE name='pg-imported')
                 AND commit_row.path='/tree/value.txt'),
            json_array_length(json_extract(
              vexfs_workspace_log('pg-imported',10,NULL),'$.entries'))>0,
            json_extract(vexfs_check('pg-imported',1),'$.ok');")"
assert_equal "$SQLITE_IMPORTED" \
    "pg-v2|pg-v1|1|1|value.txt|pg-metadata|1|1|1|1|1" \
    "PostgreSQL 到 SQLite format v2 往返错误"

cp "$SQLITE_PACKAGE" "$CORRUPT_PACKAGE"
"$CLI" "$CORRUPT_PACKAGE" \
    "UPDATE chunks SET content=zeroblob(length(content))
      WHERE rowid=(SELECT min(rowid) FROM chunks);" >/dev/null
CORRUPT_IMPORT=("$CLI" fs --backend pg --dsn "$DSN" --workspace corrupt-import)
if "${CORRUPT_IMPORT[@]}" import "$CORRUPT_PACKAGE" >/dev/null 2>&1; then
    echo "损坏的 format v2 包导入 PostgreSQL 应该失败" >&2
    exit 1
fi
CORRUPT_EXISTS="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q -t -A \
    -c "SELECT EXISTS(SELECT 1 FROM _vexfs.workspaces WHERE name='corrupt-import');")"
assert_equal "$CORRUPT_EXISTS" "f" "损坏包失败后不能留下 workspace"

echo "VEXFS PG ARCHIVE CROSS-ENGINE ROUNDTRIP: PASS ($CHECKS checks)"
