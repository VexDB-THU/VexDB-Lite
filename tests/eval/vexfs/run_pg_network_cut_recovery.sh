#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-vexfs-dev}"
CLI="${VEXFS_EVAL_MOUNT_CLI:-$ROOT/vexdb_sqlite/build/vexdb}"
PYTHON="${VEXDB_LITE_PYTHON:-/usr/bin/python3}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-network-cut.XXXXXX")"
TMP="$(cd "$TMP" && pwd -P)"
RUN_ID="${TMP##*.}"
DATABASE="${VEXDB_PG_DATABASE:-vexfs_network_cut_${RUN_ID}}"
WORKSPACE="${VEXDB_PG_WORKSPACE:-network-cut-${RUN_ID}}"
HOME_DIR="$TMP/home"
MOUNT_POINT="$TMP/mount"
READY_FILE="$TMP/proxy-ready.json"
ACK_FILE="$TMP/proxy-cut.json"
STATUS_FILE="$TMP/proxy-status.json"
PROXY_PID=""
MOUNTED=0
DATABASE_CREATED=0
CHECKS=0
FS=()

fail() { echo "$*" >&2; exit 1; }

monotonic_ms() {
    "$PYTHON" -c 'import time; print(time.monotonic_ns() // 1000000)'
}

bounded_missing_probe() {
    "$PYTHON" - "$1" "$2" <<'PY'
import errno
import os
import signal
import sys
import time

path = sys.argv[1]
timeout_ms = int(sys.argv[2])
started = time.monotonic_ns()
pid = os.fork()
if pid == 0:
    try:
        os.stat(path)
    except OSError as error:
        os._exit(0 if error.errno == errno.ENOENT else 2)
    os._exit(1)

status = 124
deadline = time.monotonic() + timeout_ms / 1000
while time.monotonic() < deadline:
    waited, child_status = os.waitpid(pid, os.WNOHANG)
    if waited == pid:
        status = os.waitstatus_to_exitcode(child_status)
        break
    time.sleep(0.02)
else:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
print((time.monotonic_ns() - started) // 1000000)
raise SystemExit(status)
PY
}

is_mounted() {
    /sbin/mount | grep -F " on $MOUNT_POINT (" >/dev/null 2>&1
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [ -n "$PROXY_PID" ] && kill -0 "$PROXY_PID" >/dev/null 2>&1 &&
       [ -f "$STATUS_FILE" ]; then
        proxy_mode="$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["mode"])' "$STATUS_FILE" 2>/dev/null || true)"
        if [ "$proxy_mode" = blackhole ]; then
            kill -USR2 "$PROXY_PID" >/dev/null 2>&1 || true
            sleep 0.1
        elif [ "$proxy_mode" = established-blackhole ]; then
            kill -HUP "$PROXY_PID" >/dev/null 2>&1 || true
            sleep 0.1
        fi
    fi
    if [ "$MOUNTED" -eq 1 ]; then
        HOME="$HOME_DIR" "${FS[@]}" unmount --force "$MOUNT_POINT" >/dev/null 2>&1 || true
    fi
    if is_mounted; then
        /sbin/umount -f "$MOUNT_POINT" >/dev/null 2>&1 || true
    fi
    if [ "${#FS[@]}" -ne 0 ]; then
        HOME="$HOME_DIR" "${FS[@]}" unmount --force "$MOUNT_POINT" \
            >/dev/null 2>&1 || true
    fi
    if [ -n "$PROXY_PID" ]; then
        kill "$PROXY_PID" >/dev/null 2>&1 || true
        wait "$PROXY_PID" >/dev/null 2>&1 || true
    fi
    if [ "$DATABASE_CREATED" -eq 1 ]; then
        docker exec "$CONTAINER" dropdb -U postgres --if-exists --force "$DATABASE" \
            >/dev/null 2>&1 || true
    fi
    if [ "$status" -eq 0 ] && ! is_mounted; then
        rm -rf "$TMP"
    elif [ "$status" -ne 0 ]; then
        echo "network cut 失败现场保留在：$TMP" >&2
    fi
    exit "$status"
}
trap cleanup EXIT INT TERM

[ "$(uname -s)" = Darwin ] || fail "该 Gate 只在 macOS NFS 上运行"
[ -x "$CLI" ] || fail "找不到当前 vexdb CLI：$CLI"
[ -x "$PYTHON" ] || fail "找不到 Python：$PYTHON"
case "$DATABASE:$WORKSPACE" in
    *[!A-Za-z0-9_:-]*) fail "数据库和 workspace 只能包含字母、数字、下划线和连字符" ;;
esac
(( ${#DATABASE} <= 63 )) || fail "数据库名不能超过 63 个字符：$DATABASE"

MEMORY_MAX="$(docker exec "$CONTAINER" cat /sys/fs/cgroup/memory.max)"
[[ "$MEMORY_MAX" =~ ^[0-9]+$ ]] || fail "PG 容器必须设置有限 memory.max"
(( MEMORY_MAX <= 1073741824 )) || fail "PG 容器 memory.max 必须不大于 1 GiB"
OOM_BEFORE="$(docker exec "$CONTAINER" awk '$1=="oom_kill" {print $2}' \
    /sys/fs/cgroup/memory.events)"
HOST_BINDING="$(docker port "$CONTAINER" 5432/tcp | sed -n '1p')"
HOST_PORT="${HOST_BINDING##*:}"
[[ "$HOST_PORT" =~ ^[0-9]+$ ]] || fail "无法推导 PG 端口"

mkdir -p "$HOME_DIR" "$MOUNT_POINT"
docker exec "$CONTAINER" createdb -U postgres "$DATABASE"
DATABASE_CREATED=1
docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
    -v ON_ERROR_STOP=1 -c 'CREATE EXTENSION vexdb_lite;' >/dev/null

"$PYTHON" "$ROOT/tests/eval/vexfs/pg_tcp_cut_proxy.py" \
    --target-host 127.0.0.1 --target-port "$HOST_PORT" \
    --ready-file "$READY_FILE" --ack-file "$ACK_FILE" \
    --status-file "$STATUS_FILE" &
PROXY_PID=$!
for _ in $(seq 1 100); do
    [ -f "$READY_FILE" ] && break
    kill -0 "$PROXY_PID" >/dev/null 2>&1 || fail "TCP cut proxy 提前退出"
    sleep 0.05
done
[ -f "$READY_FILE" ] || fail "TCP cut proxy 没有就绪"
PROXY_PORT="$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["port"])' "$READY_FILE")"
[[ "$PROXY_PORT" =~ ^[0-9]+$ ]] || fail "无效 proxy 端口"

DSN="host=127.0.0.1 port=$PROXY_PORT dbname=$DATABASE user=postgres"
FS=("$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE")
HOME="$HOME_DIR" "${FS[@]}" setup >/dev/null
HOME="$HOME_DIR" VEXFS_NFS_STRICT_DURABILITY=1 \
    "${FS[@]}" mount "$MOUNT_POINT" >/dev/null
MOUNTED=1

"$PYTHON" - "$MOUNT_POINT/before-cut.txt" before-cut <<'PY'
import os, sys
fd = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
try:
    os.write(fd, (sys.argv[2] + "\n").encode())
    os.fsync(fd)
finally:
    os.close(fd)
PY
CHECKS=$((CHECKS + 1))
# Prime root directory/stat caches before any disconnect. A peer file created
# while LISTEN is offline must become visible after reconnect.
ls "$MOUNT_POINT" >/dev/null

# Keep the two established libpq TCP sessions open but stop forwarding bytes.
# This is different from rejecting a reconnect: synchronous PQexec used to hold
# the whole NFS runtime forever in this state.
rm -f "$ACK_FILE"
kill -HUP "$PROXY_PID"
for _ in $(seq 1 100); do
    if [ -f "$ACK_FILE" ] &&
       [ "$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["mode"])' "$ACK_FILE")" = established-blackhole ]; then
        break
    fi
    sleep 0.05
done
[ -f "$ACK_FILE" ] &&
[ "$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["mode"])' "$ACK_FILE")" = established-blackhole ] ||
    fail "TCP cut proxy 没有进入 established blackhole 模式"
ESTABLISHED_CONNECTIONS="$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["connections"])' "$ACK_FILE")"
(( ESTABLISHED_CONNECTIONS >= 2 )) || \
    fail "established blackhole 没有保留主连接和 publisher 连接"
set +e
ESTABLISHED_BLACKHOLE_MS="$(bounded_missing_probe \
    "$MOUNT_POINT/established-blackhole-probe" 7000)"
ESTABLISHED_BLACKHOLE_STATUS=$?
set -e
[ "$ESTABLISHED_BLACKHOLE_STATUS" -ne 124 ] || \
    fail "已建立的 PG TCP 黑洞超过 7 秒，挂载请求仍被阻塞"
[ "$ESTABLISHED_BLACKHOLE_STATUS" -ne 0 ] || \
    fail "已建立的 PG TCP 黑洞期间挂载读取不应成功"
(( ESTABLISHED_BLACKHOLE_MS >= 4000 && ESTABLISHED_BLACKHOLE_MS <= 6500 )) || \
    fail "已建立连接黑洞失败边界异常：${ESTABLISHED_BLACKHOLE_MS}ms"

rm -f "$ACK_FILE"
kill -HUP "$PROXY_PID"
for _ in $(seq 1 100); do
    if [ -f "$ACK_FILE" ] &&
       [ "$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["mode"])' "$ACK_FILE")" = relay ]; then
        break
    fi
    sleep 0.05
done
[ -f "$ACK_FILE" ] &&
[ "$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["mode"])' "$ACK_FILE")" = relay ] ||
    fail "TCP cut proxy 没有退出 established blackhole 模式"
ESTABLISHED_RECOVERED=0
for _ in $(seq 1 40); do
    set +e
    bounded_missing_probe "$MOUNT_POINT/established-reconnect-probe" 1000 >/dev/null
    established_probe_status=$?
    set -e
    if [ "$established_probe_status" -eq 0 ]; then
        ESTABLISHED_RECOVERED=1
        break
    fi
    sleep 0.25
done
[ "$ESTABLISHED_RECOVERED" -eq 1 ] || \
    fail "已建立连接黑洞解除后挂载没有恢复"
[ "$(cat "$MOUNT_POINT/before-cut.txt")" = before-cut ] || \
    fail "已建立连接黑洞恢复后的基线内容错误"
CHECKS=$((CHECKS + 7))

rm -f "$ACK_FILE"
kill -USR1 "$PROXY_PID"
for _ in $(seq 1 100); do
    [ -f "$ACK_FILE" ] && break
    sleep 0.05
done
[ -f "$ACK_FILE" ] || fail "TCP cut proxy 没有确认断线"
CUT_CONNECTIONS="$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["connections"])' "$ACK_FILE")"
(( CUT_CONNECTIONS >= 2 )) || fail "断线时没有覆盖 runtime 的主连接和 publisher 连接"
CHECKS=$((CHECKS + 1))
docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q \
    -v ON_ERROR_STOP=1 -c \
    "SELECT vexfs_write('$WORKSPACE','/peer-during-cut.txt',convert_to('peer-during-cut','UTF8'));" \
    >/dev/null
CHECKS=$((CHECKS + 1))

sleep 1.2
rm -f "$ACK_FILE"
kill -USR2 "$PROXY_PID"
for _ in $(seq 1 100); do
    if [ -f "$ACK_FILE" ] &&
       [ "$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["mode"])' "$ACK_FILE")" = blackhole ]; then
        break
    fi
    sleep 0.05
done
[ -f "$ACK_FILE" ] || fail "TCP cut proxy 没有进入 blackhole 模式"
set +e
DISCONNECT_DETECT_MS="$(bounded_missing_probe "$MOUNT_POINT/disconnect-detect" 7000)"
DISCONNECT_DETECT_STATUS=$?
set -e
[ "$DISCONNECT_DETECT_STATUS" -ne 124 ] || fail "第一次 PG blackhole 请求超过 7 秒"
[ "$DISCONNECT_DETECT_STATUS" -ne 0 ] || fail "断线检测期间挂载读取不应成功"
set +e
BLACKHOLE_ELAPSED_MS="$(bounded_missing_probe "$MOUNT_POINT/blackhole-probe" 7000)"
BLACKHOLE_STATUS=$?
set -e
[ "$BLACKHOLE_STATUS" -ne 124 ] || fail "PG 重连超过 7 秒，挂载请求仍被阻塞"
[ "$BLACKHOLE_STATUS" -ne 0 ] || fail "blackhole 期间挂载读取不应成功"
(( DISCONNECT_DETECT_MS > BLACKHOLE_ELAPSED_MS )) && \
    BLACKHOLE_MAX_MS=$DISCONNECT_DETECT_MS || BLACKHOLE_MAX_MS=$BLACKHOLE_ELAPSED_MS
(( BLACKHOLE_MAX_MS >= 4000 )) || \
    fail "blackhole 没有进入真实 PG 重连等待：first=${DISCONNECT_DETECT_MS}ms second=${BLACKHOLE_ELAPSED_MS}ms"
(( DISCONNECT_DETECT_MS <= 6500 )) || \
    fail "第一次 PG blackhole 失败返回过慢：${DISCONNECT_DETECT_MS}ms"
(( BLACKHOLE_ELAPSED_MS <= 6500 )) || \
    fail "PG blackhole 失败返回过慢：${BLACKHOLE_ELAPSED_MS}ms"
CHECKS=$((CHECKS + 4))

rm -f "$ACK_FILE"
kill -USR2 "$PROXY_PID"
for _ in $(seq 1 100); do
    if [ -f "$ACK_FILE" ] &&
       [ "$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["mode"])' "$ACK_FILE")" = relay ]; then
        break
    fi
    sleep 0.05
done
[ -f "$ACK_FILE" ] || fail "TCP cut proxy 没有恢复 relay 模式"

read_recovered=0
RECOVERY_STARTED_MS="$(monotonic_ms)"
RECOVERY_DEADLINE_MS=$((RECOVERY_STARTED_MS + 10000))
attempt=0
while [ "$(monotonic_ms)" -lt "$RECOVERY_DEADLINE_MS" ]; do
    attempt=$((attempt + 1))
    remaining_ms=$((RECOVERY_DEADLINE_MS - $(monotonic_ms)))
    (( remaining_ms > 0 )) || break
    (( remaining_ms > 1000 )) && probe_timeout_ms=1000 || probe_timeout_ms=$remaining_ms
    set +e
    bounded_missing_probe "$MOUNT_POINT/reconnect-probe-$attempt" \
        "$probe_timeout_ms" >/dev/null
    probe_status=$?
    set -e
    if [ "$probe_status" -eq 0 ]; then
        read_recovered=1
        break
    fi
    sleep 0.25
done
RECOVERY_ELAPSED_MS=$(( $(monotonic_ms) - RECOVERY_STARTED_MS ))
if [ "$RECOVERY_ELAPSED_MS" -lt 0 ]; then RECOVERY_ELAPSED_MS=0; fi
if [ "$read_recovered" -ne 1 ]; then
    POST_ACCEPTED="$("$PYTHON" -c 'import json,sys; print(json.load(open(sys.argv[1]))["accepted"])' "$STATUS_FILE")"
    fail "真实 TCP 断线后挂载读取没有在 10 秒内恢复（elapsed_ms=${RECOVERY_ELAPSED_MS} proxy_accepted=${POST_ACCEPTED}）"
fi
(( RECOVERY_ELAPSED_MS <= 10000 )) || \
    fail "真实 TCP 断线恢复超过 10 秒：${RECOVERY_ELAPSED_MS}ms"
[ "$(cat "$MOUNT_POINT/before-cut.txt")" = before-cut ] || fail "断线恢复后的基线内容错误"
[ "$(cat "$MOUNT_POINT/peer-during-cut.txt")" = peer-during-cut ] || \
    fail "断线期间 peer 创建的文件没有使 NFS 目录缓存失效"
CHECKS=$((CHECKS + 3))

write_after_cut() {
    "$PYTHON" - "$MOUNT_POINT/after-cut.txt" <<'PY'
import os, sys
fd = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
try:
    os.write(fd, b"after-cut\n")
    os.fsync(fd)
finally:
    os.close(fd)
PY
}
recovered=0
for _ in 1 2 3; do
    if write_after_cut; then recovered=1; break; fi
    sleep 0.5
done
[ "$recovered" -eq 1 ] || fail "真实 TCP 断线后写入没有恢复"
CHECKS=$((CHECKS + 1))

STATE="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q -t -A -F '|' \
    -v ON_ERROR_STOP=1 -c \
    "SELECT convert_from(vexfs_read('$WORKSPACE','/before-cut.txt'),'UTF8'),
            convert_from(vexfs_read('$WORKSPACE','/after-cut.txt'),'UTF8'),
            convert_from(vexfs_read('$WORKSPACE','/peer-during-cut.txt'),'UTF8'),
            (vexfs_diagnostics('$WORKSPACE')->>'pending_handles')::bigint,
            (vexfs_diagnostics('$WORKSPACE')->>'staging_bytes')::bigint,
            (vexfs_check('$WORKSPACE',1)->>'ok')::boolean;")"
[ "$STATE" = $'before-cut\n|after-cut\n|peer-during-cut|0|0|t' ] || fail "断线恢复后的数据库状态错误：$STATE"
CHECKS=$((CHECKS + 6))

VERSIONS="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" -X -q -t -A \
    -c "SELECT count(*) FROM _vexfs.file_versions v JOIN _vexfs.workspaces w USING(workspace_id) WHERE w.name='$WORKSPACE';")"
[ "$VERSIONS" = 3 ] || fail "断线恢复后正式版本数错误：$VERSIONS"
CHECKS=$((CHECKS + 1))

HOME="$HOME_DIR" "${FS[@]}" unmount "$MOUNT_POINT" >/dev/null
MOUNTED=0
HOME="$HOME_DIR" "${FS[@]}" mount "$MOUNT_POINT" >/dev/null
MOUNTED=1
ACCEPTED_BEFORE_BACKGROUND="$("$PYTHON" -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["accepted"])' "$STATUS_FILE")"
rm -f "$ACK_FILE"
kill -USR1 "$PROXY_PID"
for _ in $(seq 1 100); do
    [ -f "$ACK_FILE" ] && break
    sleep 0.05
done
[ -f "$ACK_FILE" ] || fail "后台发布测试没有确认断线"
BACKGROUND_CUT_CONNECTIONS="$("$PYTHON" -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["connections"])' "$ACK_FILE")"
(( BACKGROUND_CUT_CONNECTIONS >= 2 )) || \
    fail "后台发布测试没有同时切断主连接和 publisher 连接"

write_background_after_cut() {
"$PYTHON" - "$MOUNT_POINT/background-after-cut.txt" <<'PY'
import os
import sys

descriptor = os.open(sys.argv[1], os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
try:
    os.write(descriptor, b"background-after-cut\n")
finally:
    os.close(descriptor)
PY
}
BACKGROUND_WRITE_RECOVERED=0
BACKGROUND_WRITE_ERROR="$TMP/background-write.err"
for _ in 1 2 3 4; do
    if write_background_after_cut 2>"$BACKGROUND_WRITE_ERROR"; then
        BACKGROUND_WRITE_RECOVERED=1
        break
    fi
    # An in-flight mutation has an unknown commit state and is never replayed
    # inside the runtime. Retry with the same target after reconnect cooldown.
    sleep 1.1
done
if [ "$BACKGROUND_WRITE_RECOVERED" -ne 1 ]; then
    cat "$BACKGROUND_WRITE_ERROR" >&2
    fail "后台发布测试的前台写入没有从断线恢复"
fi
BACKGROUND_PUBLISHED=0
for _ in $(seq 1 60); do
    BACKGROUND_STATE="$(docker exec "$CONTAINER" psql -U postgres -d "$DATABASE" \
        -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
        "SELECT convert_from(vexfs_read('$WORKSPACE','/background-after-cut.txt'),'UTF8'),
                (vexfs_diagnostics('$WORKSPACE')->>'pending_handles')::bigint,
                (vexfs_diagnostics('$WORKSPACE')->>'staging_bytes')::bigint;" \
        2>/dev/null || true)"
    if [ "$BACKGROUND_STATE" = $'background-after-cut\n|0|0' ]; then
        BACKGROUND_PUBLISHED=1
        break
    fi
    sleep 0.25
done
[ "$BACKGROUND_PUBLISHED" -eq 1 ] || \
    fail "publisher 断线恢复后没有发布后台文件：$BACKGROUND_STATE"
ACCEPTED_AFTER_BACKGROUND="$("$PYTHON" -c \
    'import json,sys; print(json.load(open(sys.argv[1]))["accepted"])' "$STATUS_FILE")"
(( ACCEPTED_AFTER_BACKGROUND >= ACCEPTED_BEFORE_BACKGROUND + 2 )) || \
    fail "主连接和 publisher 没有分别重连：before=$ACCEPTED_BEFORE_BACKGROUND after=$ACCEPTED_AFTER_BACKGROUND"
CHECKS=$((CHECKS + 5))

HOME="$HOME_DIR" "${FS[@]}" unmount "$MOUNT_POINT" >/dev/null
MOUNTED=0
OOM_AFTER="$(docker exec "$CONTAINER" awk '$1=="oom_kill" {print $2}' \
    /sys/fs/cgroup/memory.events)"
[ "$OOM_AFTER" = "$OOM_BEFORE" ] || fail "network cut Gate 触发 OOM"
CHECKS=$((CHECKS + 1))

echo "VEXFS PG NETWORK CUT RECOVERY: PASS (checks=$CHECKS cut_connections=$CUT_CONNECTIONS reconnect_ms=$RECOVERY_ELAPSED_MS established_blackhole_ms=$ESTABLISHED_BLACKHOLE_MS blackhole_max_ms=$BLACKHOLE_MAX_MS blackhole_first_ms=$DISCONNECT_DETECT_MS blackhole_second_ms=$BLACKHOLE_ELAPSED_MS memory_max=$MEMORY_MAX oom_kill=$OOM_AFTER)"
