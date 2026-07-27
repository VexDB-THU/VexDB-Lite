#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-vexfs-dev}"
DATABASE="${VEXDB_PG_DATABASE:-vexfs_strict_crash}"
WORKSPACE="${VEXDB_PG_WORKSPACE:-strict-crash}"
CLI="${VEXFS_EVAL_MOUNT_CLI:-$ROOT/vexdb_sqlite/build/vexdb}"
DSN="${VEXDB_PG_DSN:-}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-strict-crash.XXXXXX")"
TEST_HOME="$TMP/home"
MOUNT_POINT="$TMP/mount"
MOUNTED=0
DATABASE_CREATED=0
CHECKS=0

fail() {
    echo "$*" >&2
    exit 1
}

is_test_mount_live() {
    /sbin/mount | grep -F " on $MOUNT_POINT (" >/dev/null 2>&1
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [ "$MOUNTED" -eq 1 ]; then
        HOME="$TEST_HOME" "$CLI" fs --backend pg --dsn "$DSN" \
            --workspace "$WORKSPACE" unmount --force "$MOUNT_POINT" \
            >/dev/null 2>&1 || true
    fi
    for _ in $(seq 1 30); do
        is_test_mount_live || break
        /sbin/umount -f "$MOUNT_POINT" >/dev/null 2>&1 || true
        sleep 0.25
    done
    if is_test_mount_live; then
        echo "测试清理失败，NFS 挂载仍存在：$MOUNT_POINT" >&2
        status=1
    fi
    if [ "$DATABASE_CREATED" -eq 1 ]; then
        docker exec "$CONTAINER" dropdb -U postgres --if-exists --force "$DATABASE" \
            >/dev/null 2>&1 || true
    fi
    if ! is_test_mount_live; then
        rm -rf "$TMP"
    fi
    exit "$status"
}
trap cleanup EXIT INT TERM

[ "$(uname -s)" = Darwin ] || fail "该 Gate 只在 macOS NFS 上运行"
[ -x "$CLI" ] || fail "找不到当前 vexdb CLI：$CLI"
docker inspect "$CONTAINER" >/dev/null

MEMORY_MAX="$(docker exec "$CONTAINER" cat /sys/fs/cgroup/memory.max)"
[[ "$MEMORY_MAX" =~ ^[0-9]+$ ]] || \
    fail "PG crash Gate 必须使用有限 memory.max，当前为 $MEMORY_MAX"
(( MEMORY_MAX <= 1073741824 )) || \
    fail "PG crash Gate 的容器内存上限必须不大于 1 GiB"
OOM_BEFORE="$(docker exec "$CONTAINER" awk '$1=="oom_kill" {print $2}' \
    /sys/fs/cgroup/memory.events)"

if [ -z "$DSN" ]; then
    HOST_BINDING="$(docker port "$CONTAINER" 5432/tcp 2>/dev/null | sed -n '1p')"
    HOST_PORT="${HOST_BINDING##*:}"
    [[ "$HOST_PORT" =~ ^[0-9]+$ ]] || fail "无法推导 PG 容器端口"
    DSN="postgresql://postgres@127.0.0.1:$HOST_PORT/$DATABASE"
fi

mkdir -p "$TEST_HOME" "$MOUNT_POINT"
docker exec "$CONTAINER" dropdb -U postgres --if-exists --force "$DATABASE" \
    >/dev/null 2>&1 || true
docker exec "$CONTAINER" createdb -U postgres "$DATABASE"
DATABASE_CREATED=1
docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
    -v ON_ERROR_STOP=1 -c 'CREATE EXTENSION vexdb_lite;' >/dev/null

FS=("$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE")
HOME="$TEST_HOME" "${FS[@]}" setup >/dev/null
START_BEFORE="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -c 'SELECT pg_postmaster_start_time();')"

HOME="$TEST_HOME" VEXFS_NFS_STRICT_DURABILITY=1 \
    "${FS[@]}" mount "$MOUNT_POINT" >/dev/null
MOUNTED=1
/usr/bin/python3 - "$MOUNT_POINT/durable.txt" <<'PY'
import os
import sys

path = sys.argv[1]
descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
try:
    os.write(descriptor, b"strict-after-crash\n")
    os.fsync(descriptor)
finally:
    os.close(descriptor)
PY
CHECKS=$((CHECKS + 1))

STATE_BEFORE="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT encode(vexfs_read('$WORKSPACE','/durable.txt'),'hex'),
            (vexfs_diagnostics('$WORKSPACE')->>'pending_handles')::bigint,
            (vexfs_diagnostics('$WORKSPACE')->>'staging_bytes')::bigint,
            (vexfs_check('$WORKSPACE',1)->>'ok')::boolean;")"
[ "$STATE_BEFORE" = '7374726963742d61667465722d63726173680a|0|0|t' ] || \
    fail "fsync 返回后数据库状态不完整：$STATE_BEFORE"
CHECKS=$((CHECKS + 4))

GATEWAY_RECORD="$(find "$TEST_HOME/Library/Application Support/VexDB-Lite/nfs-gateways" \
    -name gateway.record -type f -print -quit)"
[ -f "$GATEWAY_RECORD" ] || fail "找不到 NFS gateway 状态文件"
GATEWAY_PID="$(sed -n '2p' "$GATEWAY_RECORD")"
[[ "$GATEWAY_PID" =~ ^[0-9]+$ ]] || fail "无效 gateway PID：$GATEWAY_PID"
GATEWAY_COMMAND="$(/bin/ps -p "$GATEWAY_PID" -o command= 2>/dev/null || true)"
[[ "$GATEWAY_COMMAND" == *vexfs-nfs-gateway* &&
   "$GATEWAY_COMMAND" == *"--workspace $WORKSPACE"* ]] || \
    fail "拒绝终止身份不匹配的 gateway PID $GATEWAY_PID：$GATEWAY_COMMAND"
CHECKS=$((CHECKS + 1))
kill -9 "$GATEWAY_PID"
for _ in $(seq 1 50); do
    kill -0 "$GATEWAY_PID" >/dev/null 2>&1 || break
    sleep 0.1
done
kill -0 "$GATEWAY_PID" >/dev/null 2>&1 && fail "gateway SIGKILL 后仍存活"
CHECKS=$((CHECKS + 1))

HOME="$TEST_HOME" "${FS[@]}" unmount --force "$MOUNT_POINT" >/dev/null
MOUNTED=0
CHECKS=$((CHECKS + 1))

docker exec "$CONTAINER" pg_ctl -D /var/lib/postgresql/data \
    -m immediate -w restart >/dev/null
START_AFTER="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -c 'SELECT pg_postmaster_start_time();')"
[ "$START_AFTER" != "$START_BEFORE" ] || fail "PG immediate restart 没有发生"
CHECKS=$((CHECKS + 1))

HOME="$TEST_HOME" VEXFS_NFS_STRICT_DURABILITY=1 \
    "${FS[@]}" mount "$MOUNT_POINT" >/dev/null
MOUNTED=1
[ "$(cat "$MOUNT_POINT/durable.txt")" = 'strict-after-crash' ] || \
    fail "immediate restart 后挂载内容不正确"
CHECKS=$((CHECKS + 1))

CHECK_JSON="$(HOME="$TEST_HOME" "${FS[@]}" --json check)"
printf '%s' "$CHECK_JSON" | /usr/bin/python3 -c \
    'import json,sys; value=json.load(sys.stdin); assert value["ok"] is True; assert value["issue_count"] == 0'
CHECKS=$((CHECKS + 2))

STATE_AFTER="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
    -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT count(*),
            (vexfs_diagnostics('$WORKSPACE')->>'pending_handles')::bigint,
            (vexfs_diagnostics('$WORKSPACE')->>'staging_bytes')::bigint
       FROM _vexfs.file_versions AS version
       JOIN _vexfs.workspaces AS workspace USING (workspace_id)
      WHERE workspace.name='$WORKSPACE';")"
[ "$STATE_AFTER" = '1|0|0' ] || fail "恢复后版本或 staging 状态错误：$STATE_AFTER"
CHECKS=$((CHECKS + 3))

HOME="$TEST_HOME" "${FS[@]}" unmount "$MOUNT_POINT" >/dev/null
MOUNTED=0
OOM_AFTER="$(docker exec "$CONTAINER" awk '$1=="oom_kill" {print $2}' \
    /sys/fs/cgroup/memory.events)"
[ "$OOM_AFTER" = "$OOM_BEFORE" ] || \
    fail "PG crash Gate 触发 OOM：before=$OOM_BEFORE after=$OOM_AFTER"
CHECKS=$((CHECKS + 1))

echo "VEXFS PG STRICT CRASH RECOVERY: PASS ($CHECKS checks, memory_max=$MEMORY_MAX, oom_kill=$OOM_AFTER)"
