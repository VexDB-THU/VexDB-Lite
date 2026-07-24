#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DATABASE="${VEXDB_PG_DATABASE:-test}"
WORKSPACE="pg-concurrent"
CHECKS=0
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-alpha.XXXXXX")"

cleanup() {
    docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -v ON_ERROR_STOP=1 \
        -c "SELECT vexfs_workspace_drop('$WORKSPACE', true);" >/dev/null 2>&1 || true
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

docker inspect "$CONTAINER" >/dev/null

# Public function names shared by SQLite must also exist in PostgreSQL. Keep
# this check source-derived so adding a new common SQLite function cannot leave
# the PG adapter silently behind.
sed -n '/static const FunctionDefinition functions\[\]/,/^    };/p' \
    "$ROOT/vexdb_sqlite/src/agent_files/vexfs_sqlite.cpp" \
    | sed -n 's/^[[:space:]]*{"\(vexfs_[a-z0-9_]*\)".*/\1/p' \
    | sort -u >"$TMP_DIR/sqlite-contract.txt"
sed -n '/static const FunctionDefinition functions\[\]/,/^    };/p' \
    "$ROOT/vexdb_sqlite/src/agent_files/vexfs_sqlite.cpp" \
    | sed -n 's/^[[:space:]]*{"\(vexfs_[a-z0-9_]*\)",[[:space:]]*\([0-9][0-9]*\).*/\1|\2/p' \
    | sort -u >"$TMP_DIR/sqlite-arities.txt"
docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -v ON_ERROR_STOP=1 \
    -c "SELECT DISTINCT p.proname
          FROM pg_catalog.pg_proc AS p
          JOIN pg_catalog.pg_namespace AS n ON n.oid=p.pronamespace
         WHERE n.nspname='public' AND p.proname LIKE 'vexfs_%'
         ORDER BY p.proname;" \
    >"$TMP_DIR/pg-contract.txt"
docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 \
    -c "SELECT p.proname,p.pronargs-p.pronargdefaults,p.pronargs
          FROM pg_catalog.pg_proc AS p
          JOIN pg_catalog.pg_namespace AS n ON n.oid=p.pronamespace
         WHERE n.nspname='public' AND p.proname LIKE 'vexfs_%'
         ORDER BY p.proname,p.pronargs-p.pronargdefaults,p.pronargs;" \
    >"$TMP_DIR/pg-arities.txt"
MISSING_CONTRACT="$(comm -23 "$TMP_DIR/sqlite-contract.txt" "$TMP_DIR/pg-contract.txt")"
if [[ -n "$MISSING_CONTRACT" ]]; then
    echo "PostgreSQL 缺少公共 VexFS SQL 函数：" >&2
    printf '%s\n' "$MISSING_CONTRACT" >&2
    exit 1
fi
while IFS='|' read -r function_name arguments; do
    if ! awk -F '|' -v name="$function_name" -v argc="$arguments" \
            '$1 == name && argc >= $2 && argc <= $3 { found=1 }
             END { exit(found ? 0 : 1) }' "$TMP_DIR/pg-arities.txt"; then
        echo "PostgreSQL 公共函数不接受 SQLite 参数数量：$function_name/$arguments" >&2
        exit 1
    fi
done <"$TMP_DIR/sqlite-arities.txt"
CONTRACT_STATE="$(docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A \
    -F '|' -v ON_ERROR_STOP=1 \
    -c "SELECT vexfs_pg_adapter_version(),vexfs_contract_version(),vexfs_init();")"
if [[ "$CONTRACT_STATE" != "0.4.0-alpha.1|0.9.0|1" ]]; then
    echo "PostgreSQL VexFS 合同版本错误：$CONTRACT_STATE" >&2
    exit 1
fi
CHECKS=$((CHECKS + $(wc -l <"$TMP_DIR/sqlite-contract.txt") + \
    $(wc -l <"$TMP_DIR/sqlite-arities.txt") + 3))

docker exec -i "$CONTAINER" psql -d "$DATABASE" -X -q -v ON_ERROR_STOP=1 <<SQL >/dev/null
SELECT vexfs_workspace_drop('$WORKSPACE', true);
SELECT vexfs_workspace_create('$WORKSPACE');
SELECT vexfs_write('$WORKSPACE', '/shared.txt', convert_to('base', 'UTF8'));
SQL
CHECKS=$((CHECKS + 3))

docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -v ON_ERROR_STOP=1 \
    -c "SELECT vexfs_write('$WORKSPACE', '/shared.txt', convert_to('writer-a', 'UTF8'));" \
    >"$TMP_DIR/a.out" &
PID_A=$!
docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -v ON_ERROR_STOP=1 \
    -c "SELECT vexfs_write('$WORKSPACE', '/shared.txt', convert_to('writer-b', 'UTF8'));" \
    >"$TMP_DIR/b.out" &
PID_B=$!

wait "$PID_A"
wait "$PID_B"

VERSIONS="$(sort -n "$TMP_DIR/a.out" "$TMP_DIR/b.out" | tr '\n' ',' | sed 's/,$//')"
if [[ "$VERSIONS" != "2,3" ]]; then
    echo "并发写版本错误：$VERSIONS" >&2
    exit 1
fi
CHECKS=$((CHECKS + 1))

STATE="$(docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -F '|' -v ON_ERROR_STOP=1 \
    -c "SELECT i.current_version, count(f.version_no), convert_from(vexfs_read('$WORKSPACE', '/shared.txt'), 'UTF8') IN ('writer-a', 'writer-b') FROM _vexfs.inodes i JOIN _vexfs.file_versions f USING (workspace_id, inode_id) WHERE i.kind='file' AND i.workspace_id=(SELECT workspace_id FROM _vexfs.workspaces WHERE name='$WORKSPACE') GROUP BY i.current_version;")"
if [[ "$STATE" != "3|3|t" ]]; then
    echo "并发写最终状态错误：$STATE" >&2
    exit 1
fi
CHECKS=$((CHECKS + 3))

EXPECTED_HEAD="$(docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -v ON_ERROR_STOP=1 \
    -c "SELECT vexfs_snapshot_create('$WORKSPACE', 'concurrent-restore'); SELECT vexfs_write('$WORKSPACE', '/shared.txt', convert_to('after-snapshot', 'UTF8')); SELECT (vexfs_workspace_stat('$WORKSPACE')->>'head_commit')::bigint;")"
EXPECTED_HEAD="$(printf '%s\n' "$EXPECTED_HEAD" | tail -n 1)"
if [[ "$EXPECTED_HEAD" != "5" ]]; then
    echo "并发恢复准备 head 错误：$EXPECTED_HEAD" >&2
    exit 1
fi
CHECKS=$((CHECKS + 3))

docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -v ON_ERROR_STOP=1 \
    -c "SELECT vexfs_snapshot_restore('$WORKSPACE', 'concurrent-restore', $EXPECTED_HEAD);" \
    >"$TMP_DIR/restore-a.out" 2>"$TMP_DIR/restore-a.err" &
PID_A=$!
docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -v ON_ERROR_STOP=1 \
    -c "SELECT vexfs_snapshot_restore('$WORKSPACE', 'concurrent-restore', $EXPECTED_HEAD);" \
    >"$TMP_DIR/restore-b.out" 2>"$TMP_DIR/restore-b.err" &
PID_B=$!

set +e
wait "$PID_A"; RC_A=$?
wait "$PID_B"; RC_B=$?
set -e
if [[ "$RC_A" -eq 0 && "$RC_B" -eq 0 ]] || [[ "$RC_A" -ne 0 && "$RC_B" -ne 0 ]]; then
    echo "并发恢复必须一成一败：rc_a=$RC_A rc_b=$RC_B" >&2
    exit 1
fi
if ! grep -q 'VEXFS_HEAD_CONFLICT' "$TMP_DIR/restore-a.err" "$TMP_DIR/restore-b.err"; then
    echo "并发恢复失败端没有返回 VEXFS_HEAD_CONFLICT" >&2
    exit 1
fi
CHECKS=$((CHECKS + 2))

RESTORE_STATE="$(docker exec "$CONTAINER" psql -d "$DATABASE" -X -q -t -A -F '|' -v ON_ERROR_STOP=1 \
    -c "SELECT (vexfs_workspace_stat('$WORKSPACE')->>'head_commit')::bigint, convert_from(vexfs_read('$WORKSPACE', '/shared.txt'), 'UTF8') IN ('writer-a', 'writer-b');")"
if [[ "$RESTORE_STATE" != "6|t" ]]; then
    echo "并发恢复最终状态错误：$RESTORE_STATE" >&2
    exit 1
fi
CHECKS=$((CHECKS + 2))

echo "VEXFS PG ADAPTER ALPHA: PASS ($CHECKS checks)"
