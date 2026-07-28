#!/bin/bash
set -euo pipefail

# Keep this file directly executable: CI and local acceptance use the shebang path.

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
REMOTE_HOST="${VEXFS_REMOTE_HOST:?VEXFS_REMOTE_HOST is required}"
REMOTE_USER="${VEXFS_REMOTE_USER:-$USER}"
REMOTE_PASSWORD="${VEXFS_REMOTE_PASSWORD:-}"
LOCAL_CLI="${VEXFS_EVAL_MOUNT_CLI:-$HOME/.local/bin/vexdb}"
REMOTE_CLI="${VEXFS_REMOTE_CLI:-/Users/$REMOTE_USER/.local/bin/vexdb}"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
PG_BIN="${VEXDB_PG_BIN:-/opt/pg19/bin}"
PG_DATABASE="${VEXFS_REMOTE_PG_DATABASE:-test}"
PG_USER="${VEXFS_REMOTE_PG_USER:-postgres}"
PG_PASSWORD="${VEXFS_REMOTE_PG_PASSWORD:-}"
LOCAL_PG_PORT="${VEXFS_LOCAL_PG_PORT:-5433}"
REMOTE_TUNNEL_PORT="${VEXFS_REMOTE_TUNNEL_PORT:-6545}"
WORKSPACE="${VEXFS_REMOTE_FAULT_WORKSPACE:-pg-two-mac-fault}"
KNOWN_HOSTS="${VEXFS_REMOTE_KNOWN_HOSTS:-/tmp/vexfs-remote-macos-fault-known-hosts}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-two-mac-fault.XXXXXX")"
CONTROL="/tmp/vexfs-pg-fault-tunnel-$$.sock"
LOCAL_MOUNT="$TMP/mount"
LOCAL_PASSFILE="$TMP/pgpass"
REMOTE_ROOT="/tmp/vexfs-pg-two-mac-fault-$$"
REMOTE_MOUNT="$REMOTE_ROOT/mount"
REMOTE_PASSFILE="$REMOTE_ROOT/pgpass"
TARGET="$REMOTE_USER@$REMOTE_HOST"
CHECKS=0
TUNNEL_RUNNING=false
DATABASE_STOPPED=false
PHASE=initializing

case "$WORKSPACE" in
    ''|*[!A-Za-z0-9_-]*) echo "workspace 只能包含字母、数字、下划线和连字符" >&2; exit 2 ;;
esac
case "$PG_DATABASE$PG_USER$LOCAL_PG_PORT$REMOTE_TUNNEL_PORT" in
    *[!A-Za-z0-9_-]*) echo "PG 连接字段包含不支持的字符" >&2; exit 2 ;;
esac
[ "$(uname -s)" = Darwin ] || { echo "双 Mac 故障测试必须从 macOS 运行" >&2; exit 2; }
[ -x "$LOCAL_CLI" ] || { echo "找不到本机 vexdb CLI：$LOCAL_CLI" >&2; exit 2; }
echo "VEXFS PG TWO-MAC FAULT: phase=prerequisites"
docker inspect "$CONTAINER" >/dev/null

SSH_OPTIONS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN_HOSTS" -o ConnectTimeout=8)
if [ -n "$REMOTE_PASSWORD" ]; then
    SSHPASS_BIN="$(command -v sshpass || true)"
    [ -n "$SSHPASS_BIN" ] || { echo "密码登录需要 sshpass" >&2; exit 2; }
    export SSHPASS="$REMOTE_PASSWORD"
    SSH=("$SSHPASS_BIN" -e ssh "${SSH_OPTIONS[@]}")
else
    SSH=(ssh "${SSH_OPTIONS[@]}")
fi

LOCAL_DSN="host=127.0.0.1 port=$LOCAL_PG_PORT dbname=$PG_DATABASE user=$PG_USER connect_timeout=3"
REMOTE_DSN="host=127.0.0.1 port=$REMOTE_TUNNEL_PORT dbname=$PG_DATABASE user=$PG_USER connect_timeout=3"
if [ -n "$PG_PASSWORD" ]; then
    umask 077
    printf '%s:%s:%s:%s:%s\n' 127.0.0.1 "$LOCAL_PG_PORT" \
        "$PG_DATABASE" "$PG_USER" "$PG_PASSWORD" >"$LOCAL_PASSFILE"
    chmod 0600 "$LOCAL_PASSFILE"
    LOCAL_DSN="$LOCAL_DSN passfile=$LOCAL_PASSFILE"
    REMOTE_DSN="$REMOTE_DSN passfile=$REMOTE_PASSFILE"
fi

fail() {
    echo "$*" >&2
    exit 1
}

pass() {
    CHECKS=$((CHECKS + 1))
}

shell_join() {
    local output="" argument quoted
    for argument in "$@"; do
        printf -v quoted '%q' "$argument"
        output+="$quoted "
    done
    printf '%s' "$output"
}

remote_exec() {
    local command
    command="$(shell_join "$@")"
    printf 'set -euo pipefail\n%s\n' "$command" | "${SSH[@]}" "$TARGET" /bin/bash -s
}

remote_shell() {
    printf 'set -euo pipefail\n%s\n' "$1" | "${SSH[@]}" "$TARGET" /bin/bash -s
}

remote_vexfs() {
    remote_exec "$REMOTE_CLI" fs --backend pg --dsn "$REMOTE_DSN" \
        --workspace "$WORKSPACE" "$@"
}

local_vexfs() {
    "$LOCAL_CLI" fs --backend pg --dsn "$LOCAL_DSN" \
        --workspace "$WORKSPACE" "$@"
}

start_tunnel() {
    [ "$TUNNEL_RUNNING" = false ] || return 0
    rm -f "$CONTROL"
    "${SSH[@]}" -M -S "$CONTROL" -fNT -o ExitOnForwardFailure=yes \
        -R "127.0.0.1:$REMOTE_TUNNEL_PORT:127.0.0.1:$LOCAL_PG_PORT" "$TARGET"
    TUNNEL_RUNNING=true
}

stop_tunnel() {
    [ "$TUNNEL_RUNNING" = true ] || return 0
    "${SSH[@]}" -S "$CONTROL" -O exit "$TARGET" >/dev/null 2>&1 || true
    TUNNEL_RUNNING=false
    rm -f "$CONTROL"
}

database_start() {
    if ! docker exec "$CONTAINER" "$PG_BIN/pg_ctl" \
            -D /var/lib/postgresql/data status >/dev/null 2>&1; then
        docker exec "$CONTAINER" "$PG_BIN/pg_ctl" \
            -D /var/lib/postgresql/data -w start >/dev/null
    fi
    DATABASE_STOPPED=false
}

database_stop() {
    docker exec "$CONTAINER" "$PG_BIN/pg_ctl" \
        -D /var/lib/postgresql/data -m fast -w stop >/dev/null
    DATABASE_STOPPED=true
}

drop_workspace() {
    docker exec "$CONTAINER" "$PG_BIN/psql" -U "$PG_USER" -d "$PG_DATABASE" \
        -X -q -v ON_ERROR_STOP=1 \
        -c "SELECT public.vexfs_workspace_drop('$WORKSPACE', true);" \
        >/dev/null 2>&1 || true
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [ "$status" -ne 0 ]; then
        echo "双 Mac 故障 eval 失败：phase=$PHASE status=$status" >&2
    fi
    database_start >/dev/null 2>&1 || true
    start_tunnel >/dev/null 2>&1 || true
    local_vexfs unmount --force "$LOCAL_MOUNT" >/dev/null 2>&1 || true
    remote_vexfs unmount --force "$REMOTE_MOUNT" >/dev/null 2>&1 || true
    remote_exec rm -rf "$REMOTE_ROOT" >/dev/null 2>&1 || true
    stop_tunnel
    drop_workspace
    rm -rf "$TMP"
    exit "$status"
}
trap cleanup EXIT INT TERM

expect_remote_write_failure() {
    local relative=$1 content=$2 description=$3 command
    printf -v command 'printf "%%s\\n" %q > %q' "$content" "$REMOTE_MOUNT/$relative"
    if remote_exec /usr/bin/perl -e 'alarm 12; exec @ARGV' /bin/bash -c "$command" \
        >/dev/null 2>&1; then
        fail "$description：写入意外成功"
    fi
    pass
}

expect_database_path_missing() {
    local path=$1 description=$2
    if local_vexfs stat "$path" >/dev/null 2>&1; then
        fail "$description：数据库中留下了 $path"
    fi
    pass
}

wait_remote_unmounted() {
    local output=""
    for _ in $(seq 1 40); do
        output="$(remote_vexfs --json mount status "$REMOTE_MOUNT" 2>/dev/null || true)"
        [ "$output" = "[]" ] && return 0
        sleep 0.25
    done
    fail "VexFSAppEx 退出后挂载没有在 10 秒内撤销：$output"
}

wait_remote_mount_read() {
    local relative=$1 expected=$2 description=$3 actual=""
    for _ in $(seq 1 20); do
        if actual="$(remote_exec cat "$REMOTE_MOUNT/$relative" 2>/dev/null)" && \
                [ "$actual" = "$expected" ]; then
            pass
            return 0
        fi
        sleep 0.25
    done
    fail "$description：10 秒内没有恢复，最后内容为 [$actual]"
}

wait_local_mount_read() {
    local relative=$1 expected=$2 description=$3 actual=""
    for _ in $(seq 1 20); do
        if actual="$(cat "$LOCAL_MOUNT/$relative" 2>/dev/null)" && \
                [ "$actual" = "$expected" ]; then
            pass
            return 0
        fi
        sleep 0.25
    done
    fail "$description：10 秒内没有恢复，最后内容为 [$actual]"
}

# A previous FSKit fault run can still be finishing ExtensionKit teardown even
# after both mount tables are empty. Avoid immediately reusing that process window.
sleep 3
echo "VEXFS PG TWO-MAC FAULT: phase=database-and-tunnel"
database_start
start_tunnel
PHASE=mounting
echo "VEXFS PG TWO-MAC FAULT: phase=mounting"
remote_exec test -x "$REMOTE_CLI"
remote_exec rm -rf "$REMOTE_ROOT"
remote_exec mkdir -p "$REMOTE_MOUNT"
if [ -n "$PG_PASSWORD" ]; then
    PASSFILE_LINE="127.0.0.1:$REMOTE_TUNNEL_PORT:$PG_DATABASE:$PG_USER:$PG_PASSWORD"
    printf -v command 'umask 077; printf "%%s\\n" %q > %q; chmod 0600 %q' \
        "$PASSFILE_LINE" "$REMOTE_PASSFILE" "$REMOTE_PASSFILE"
    remote_shell "$command"
fi
mkdir -p "$LOCAL_MOUNT"
local_vexfs unmount --force "$LOCAL_MOUNT" >/dev/null 2>&1 || true
drop_workspace
# ExtensionKit tears down an unmounted volume asynchronously. A new volume
# started in that short window can inherit the previous termination signal.
# Let both machines finish the old volume before creating the fault-test pair.
sleep 3
local_vexfs setup >/dev/null
local_vexfs mount "$LOCAL_MOUNT" >/dev/null
remote_vexfs mount "$REMOTE_MOUNT" >/dev/null

mkdir -p "$LOCAL_MOUNT/shared"
printf '%s\n' baseline-from-local >"$LOCAL_MOUNT/shared/baseline.txt"
[ "$(remote_exec cat "$REMOTE_MOUNT/shared/baseline.txt")" = baseline-from-local ] || \
    fail "第二台 Mac 没有读到第一台的基线文件"
pass
[[ "$(remote_vexfs --json mount status "$REMOTE_MOUNT")" == *"\"workspace\":\"$WORKSPACE\""* ]] || \
    fail "第二台 Mac 的 FSKit 挂载状态不正确"
pass
sleep 2

# ExtensionKit 会在 VexFSAppEx 崩溃后撤销挂载。底层目录必须保持 0500，
# 避免 Bash 把文件写进本机目录；显式 mount 后应继续使用原 workspace。
PHASE=extension-crash
echo "VEXFS PG TWO-MAC FAULT: phase=extension-crash"
remote_exec pgrep -x VexFSAppEx >/dev/null
remote_exec pkill -KILL -x VexFSAppEx
wait_remote_unmounted
pass
[ "$(remote_exec stat -f '%Lp' "$REMOTE_MOUNT")" = 500 ] || \
    fail "VexFSAppEx 崩溃后挂载点没有进入防误写模式"
pass
expect_remote_write_failure unsafe-after-extension-crash.txt should-not-be-local \
    "VexFSAppEx 崩溃后的底层目录保护"
expect_database_path_missing /unsafe-after-extension-crash.txt \
    "VexFSAppEx 崩溃失败写"
remote_vexfs mount "$REMOTE_MOUNT" >/dev/null
[ "$(remote_exec cat "$REMOTE_MOUNT/shared/baseline.txt")" = baseline-from-local ] || \
    fail "VexFSAppEx 崩溃重挂后基线文件丢失"
pass
remote_shell "printf '%s\n' after-extension-remount > $(shell_join "$REMOTE_MOUNT/shared/after-extension-remount.txt")"
[ "$(local_vexfs cat /shared/after-extension-remount.txt)" = after-extension-remount ] || \
    fail "VexFSAppEx 重挂后的写入没有进入 PostgreSQL"
pass

# 关闭 SSH 反向隧道等价于第二台机器到 PG 的网络中断。挂载保留，
# 当前写必须明确失败；隧道恢复后同一个挂载应在下一次操作自动重连。
PHASE=network-outage
echo "VEXFS PG TWO-MAC FAULT: phase=network-outage"
stop_tunnel
expect_remote_write_failure shared/during-network-outage.txt should-fail \
    "网络中断期间写入"
start_tunnel
expect_database_path_missing /shared/during-network-outage.txt \
    "网络中断失败写"
wait_remote_mount_read shared/baseline.txt baseline-from-local \
    "网络恢复后的挂载读"
remote_shell "printf '%s\n' after-network-reconnect > $(shell_join "$REMOTE_MOUNT/shared/after-network-reconnect.txt")"
[ "$(local_vexfs cat /shared/after-network-reconnect.txt)" = after-network-reconnect ] || \
    fail "网络恢复后的写入没有进入 PostgreSQL"
pass

# PostgreSQL 整体停止时，两个 gateway 都会失去连接。服务器恢复后不重挂，
# libpq HostStore 必须在下一次操作自动建立新连接。
PHASE=database-outage
echo "VEXFS PG TWO-MAC FAULT: phase=database-outage"
database_stop
expect_remote_write_failure shared/during-database-outage.txt should-fail \
    "数据库停止期间写入"
database_start
expect_database_path_missing /shared/during-database-outage.txt \
    "数据库停止失败写"
wait_remote_mount_read shared/baseline.txt baseline-from-local \
    "数据库恢复后的挂载读"
wait_local_mount_read shared/baseline.txt baseline-from-local \
    "数据库恢复后的本机挂载读"
remote_shell "printf '%s\n' after-database-restart > $(shell_join "$REMOTE_MOUNT/shared/after-database-restart.txt")"
[ "$(local_vexfs cat /shared/after-database-restart.txt)" = after-database-restart ] || \
    fail "数据库恢复后的写入没有进入 PostgreSQL"
pass

printf '%s\n' final-from-local >"$LOCAL_MOUNT/shared/final-from-local.txt"
PHASE=final-consistency
echo "VEXFS PG TWO-MAC FAULT: phase=final-consistency"
[ "$(remote_exec cat "$REMOTE_MOUNT/shared/final-from-local.txt")" = final-from-local ] || \
    fail "故障恢复后第二台 Mac 没有看到第一台写入"
pass
remote_shell "printf '%s\n' final-from-remote > $(shell_join "$REMOTE_MOUNT/shared/final-from-remote.txt")"
[ "$(cat "$LOCAL_MOUNT/shared/final-from-remote.txt")" = final-from-remote ] || \
    fail "故障恢复后第一台 Mac 没有看到第二台写入"
pass

CHECK_OUTPUT="$(local_vexfs check)"
[[ "$CHECK_OUTPUT" == *"OK workspace=$WORKSPACE"* ]] || \
    fail "双机故障恢复后的 deep check 失败：$CHECK_OUTPUT"
pass
FINAL_STATE="$(docker exec "$CONTAINER" "$PG_BIN/psql" -U "$PG_USER" \
    -d "$PG_DATABASE" -X -q -t -A -F '|' -v ON_ERROR_STOP=1 -c \
    "SELECT (public.vexfs_check('$WORKSPACE',1)->>'ok')::boolean,
            (SELECT count(*) FROM public.vexfs_audit_list('$WORKSPACE',1000,NULL)) >= 6,
            (SELECT count(*) >= 2
               FROM _vexfs.mount_sessions AS mounted
               JOIN _vexfs.workspaces AS workspace USING (workspace_id)
              WHERE workspace.name='$WORKSPACE'
                AND mounted.lease_until > clock_timestamp());")"
[[ "$FINAL_STATE" == "t|t|t" ]] || fail "双机最终数据库状态错误：$FINAL_STATE"
CHECKS=$((CHECKS + 3))

local_vexfs unmount "$LOCAL_MOUNT" >/dev/null
remote_vexfs unmount "$REMOTE_MOUNT" >/dev/null
[ "$(stat -f '%Lp' "$LOCAL_MOUNT")" = 700 ] || \
    fail "本机正常卸载后挂载点权限没有恢复到 0700"
pass
[ "$(remote_exec stat -f '%Lp' "$REMOTE_MOUNT")" = 700 ] || \
    fail "第二台 Mac 正常卸载后挂载点权限没有恢复到 0700"
pass

echo "VEXFS PG TWO-MAC FAULT RECOVERY: PASS ($CHECKS checks)"
