#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NODE_SCRIPT="$ROOT/tests/eval/vexfs/run_pg_remote_macos_node.sh"
REMOTE_HOST="${VEXFS_REMOTE_HOST:?VEXFS_REMOTE_HOST is required}"
REMOTE_USER="${VEXFS_REMOTE_USER:-$USER}"
REMOTE_PASSWORD="${VEXFS_REMOTE_PASSWORD:-}"
PG_HOST="${VEXFS_REMOTE_PG_HOST:?VEXFS_REMOTE_PG_HOST is required}"
PG_PORT="${VEXFS_REMOTE_PG_PORT:-5432}"
LOCAL_PG_HOST="${VEXFS_LOCAL_PG_HOST:-$PG_HOST}"
LOCAL_PG_PORT="${VEXFS_LOCAL_PG_PORT:-$PG_PORT}"
PG_DATABASE="${VEXFS_REMOTE_PG_DATABASE:-test}"
PG_USER="${VEXFS_REMOTE_PG_USER:-postgres}"
PG_PASSWORD="${VEXFS_REMOTE_PG_PASSWORD:?VEXFS_REMOTE_PG_PASSWORD is required}"
PG_CONTAINER="${VEXDB_PG_CONTAINER:-}"
LOCAL_CLI="${VEXFS_EVAL_MOUNT_CLI:-$HOME/.local/bin/vexdb}"
REMOTE_CLI="${VEXFS_REMOTE_CLI:-/Users/$REMOTE_USER/.local/bin/vexdb}"
WORKSPACE="${VEXFS_REMOTE_WORKSPACE:-pg-remote-macos}"
KNOWN_HOSTS="${VEXFS_REMOTE_KNOWN_HOSTS:-/tmp/vexfs-remote-macos-known-hosts}"
REMOTE_NODE="/tmp/vexfs-pg-remote-node-$$.sh"
PASSFILE="/tmp/vexfs-pg-remote-${WORKSPACE}.pass"
TARGET="$REMOTE_USER@$REMOTE_HOST"

case "$WORKSPACE" in
    ''|*[!A-Za-z0-9_-]*) echo "workspace 只能包含字母、数字、下划线和连字符" >&2; exit 2 ;;
esac
case "$PG_HOST$PG_PORT$LOCAL_PG_HOST$LOCAL_PG_PORT$PG_DATABASE$PG_USER" in
    *[!A-Za-z0-9_.:-]*) echo "PG 连接字段包含不支持的字符" >&2; exit 2 ;;
esac
[ "$(uname -s)" = Darwin ] || { echo "跨 Mac 编排必须从 macOS 运行" >&2; exit 2; }
[ -x "$LOCAL_CLI" ] || { echo "找不到本机 vexdb CLI：$LOCAL_CLI" >&2; exit 2; }
[ -f "$NODE_SCRIPT" ] || { echo "找不到节点脚本：$NODE_SCRIPT" >&2; exit 2; }

SSH_OPTIONS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN_HOSTS" -o ConnectTimeout=8)
if [ -n "$REMOTE_PASSWORD" ]; then
    SSHPASS_BIN="$(command -v sshpass || true)"
    [ -n "$SSHPASS_BIN" ] || { echo "密码登录需要 sshpass" >&2; exit 2; }
    export SSHPASS="$REMOTE_PASSWORD"
    SSH=("$SSHPASS_BIN" -e ssh "${SSH_OPTIONS[@]}")
    SCP=("$SSHPASS_BIN" -e scp "${SSH_OPTIONS[@]}")
else
    SSH=(ssh "${SSH_OPTIONS[@]}")
    SCP=(scp "${SSH_OPTIONS[@]}")
fi

REMOTE_DSN="host=$PG_HOST port=$PG_PORT dbname=$PG_DATABASE user=$PG_USER passfile=$PASSFILE connect_timeout=8"
LOCAL_DSN="host=$LOCAL_PG_HOST port=$LOCAL_PG_PORT dbname=$PG_DATABASE user=$PG_USER passfile=$PASSFILE connect_timeout=8"
REMOTE_PASSFILE_LINE="$PG_HOST:$PG_PORT:$PG_DATABASE:$PG_USER:$PG_PASSWORD"
LOCAL_PASSFILE_LINE="$LOCAL_PG_HOST:$LOCAL_PG_PORT:$PG_DATABASE:$PG_USER:$PG_PASSWORD"

remote_command() {
    "${SSH[@]}" "$TARGET" "$1"
}

drop_workspace() {
    [ -n "$PG_CONTAINER" ] || return 0
    docker exec "$PG_CONTAINER" /opt/pg19/bin/psql -U postgres -d "$PG_DATABASE" -X -q \
        -c "SELECT vexfs_workspace_drop('$WORKSPACE', true);" >/dev/null 2>&1 || true
}

cleanup() {
    local status=$? command
    trap - EXIT INT TERM
    "$LOCAL_CLI" fs --backend pg --dsn "$LOCAL_DSN" --workspace "$WORKSPACE" \
        unmount --force >/dev/null 2>&1 || true
    printf -v command 'rm -f %q %q' "$REMOTE_NODE" "$PASSFILE"
    remote_command "$command" >/dev/null 2>&1 || true
    rm -f "$PASSFILE"
    drop_workspace
    exit "$status"
}
trap cleanup EXIT INT TERM

write_remote_passfile() {
    local command
    printf -v command 'umask 077; printf "%%s\\n" %q > %q; chmod 0600 %q' \
        "$REMOTE_PASSFILE_LINE" "$PASSFILE" "$PASSFILE"
    remote_command "$command"
}

remote_phase() {
    local phase="$1" command
    printf -v command \
        'env VEXFS_NODE_PHASE=%q VEXFS_NODE_CLI=%q VEXFS_NODE_DSN=%q VEXFS_NODE_WORKSPACE=%q /bin/bash %q' \
        "$phase" "$REMOTE_CLI" "$REMOTE_DSN" "$WORKSPACE" "$REMOTE_NODE"
    remote_command "$command"
}

local_phase() {
    VEXFS_NODE_PHASE="$1" \
    VEXFS_NODE_CLI="$LOCAL_CLI" \
    VEXFS_NODE_DSN="$LOCAL_DSN" \
    VEXFS_NODE_WORKSPACE="$WORKSPACE" \
        /bin/bash "$NODE_SCRIPT"
}

drop_workspace
umask 077
printf '%s\n' "$LOCAL_PASSFILE_LINE" >"$PASSFILE"
chmod 0600 "$PASSFILE"
write_remote_passfile
"${SCP[@]}" "$NODE_SCRIPT" "$TARGET:$REMOTE_NODE" >/dev/null
remote_command "chmod 0700 $REMOTE_NODE"

local_phase local-create
remote_phase remote-runtime-check
remote_phase remote-modify
local_phase local-restore
remote_phase remote-final

echo "VEXFS PG TWO-MAC ROUNDTRIP: PASS"
