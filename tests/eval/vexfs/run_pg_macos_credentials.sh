#!/usr/bin/env bash
set -euo pipefail

[ "$(uname -s)" = Darwin ] || { echo "该测试只支持 macOS" >&2; exit 2; }

CLI="${VEXFS_EVAL_MOUNT_CLI:-$HOME/.local/bin/vexdb}"
PG_HOST="${VEXFS_PG_HOST:-127.0.0.1}"
PG_PORT="${VEXFS_PG_PORT:-5432}"
PG_DATABASE="${VEXFS_PG_DATABASE:-test}"
PG_USER="${VEXFS_PG_USER:-postgres}"
PG_PASSWORD="${VEXFS_PG_PASSWORD:?VEXFS_PG_PASSWORD is required}"
PG_CONTAINER="${VEXDB_PG_CONTAINER:-}"
WORKSPACE="${VEXFS_PG_WORKSPACE:-pg-macos-credentials}"
RESOURCE_ROOT="$HOME/Library/Application Support/VexDB-Lite/mount-resources"
PASSFILE="$(mktemp "${TMPDIR:-/tmp}/vexfs-pgpass.XXXXXX")"
MOUNT_POINT="$(mktemp -d "${TMPDIR:-/tmp}/vexfs-pg-credential-mount.XXXXXX")"
STALE_DIR="$RESOURCE_ROOT/stale-credential-eval-$$"
LOG_FILE="$(mktemp "${TMPDIR:-/tmp}/vexfs-pg-credential-log.XXXXXX")"
MOUNTED=false
CHECKS=0

[ -x "$CLI" ] || { echo "找不到 vexdb CLI：$CLI" >&2; exit 2; }
case "$PG_HOST$PG_PORT$PG_DATABASE$PG_USER$WORKSPACE" in
    *[!A-Za-z0-9_.:-]*) echo "连接字段包含不支持的字符" >&2; exit 2 ;;
esac

pass() { CHECKS=$((CHECKS + 1)); }
fail() { echo "$1" >&2; exit 1; }

drop_workspace() {
    [ -n "$PG_CONTAINER" ] || return 0
    docker exec "$PG_CONTAINER" /opt/pg19/bin/psql -U postgres -d "$PG_DATABASE" -X -q \
        -c "SELECT vexfs_workspace_drop('$WORKSPACE', true);" >/dev/null 2>&1 || true
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if [ "$MOUNTED" = true ]; then
        "$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" \
            unmount --force "$MOUNT_POINT" >/dev/null 2>&1 || true
    fi
    rm -f "$PASSFILE" "$PASSFILE.link" "$LOG_FILE"
    rm -f "$STALE_DIR/.vexfs-pgpass"
    rmdir "$STALE_DIR" "$MOUNT_POINT" >/dev/null 2>&1 || true
    drop_workspace
    exit "$status"
}
trap cleanup EXIT INT TERM

umask 077
printf '%s\n' "$PG_HOST:$PG_PORT:$PG_DATABASE:$PG_USER:$PG_PASSWORD" >"$PASSFILE"
chmod 0600 "$PASSFILE"
DSN="host=$PG_HOST port=$PG_PORT dbname=$PG_DATABASE user=$PG_USER passfile=$PASSFILE connect_timeout=8"

drop_workspace
"$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" setup >/dev/null

# 过宽权限、符号链接和 DSN 明文密码都必须在调用 FSKit 前拒绝。
chmod 0644 "$PASSFILE"
if "$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" \
    mount "$MOUNT_POINT" >"$LOG_FILE" 2>&1; then
    fail "0644 passfile 被错误接受"
fi
grep -q "0600" "$LOG_FILE" || fail "0644 passfile 错误信息不明确"
pass
chmod 0600 "$PASSFILE"

ln -s "$PASSFILE" "$PASSFILE.link"
SYMLINK_DSN="host=$PG_HOST port=$PG_PORT dbname=$PG_DATABASE user=$PG_USER passfile=$PASSFILE.link connect_timeout=8"
if "$CLI" fs --backend pg --dsn "$SYMLINK_DSN" --workspace "$WORKSPACE" \
    mount "$MOUNT_POINT" >"$LOG_FILE" 2>&1; then
    fail "符号链接 passfile 被错误接受"
fi
pass
rm -f "$PASSFILE.link"

INLINE_DSN="postgresql://$PG_USER:$PG_PASSWORD@$PG_HOST:$PG_PORT/$PG_DATABASE"
if "$CLI" fs --backend pg --dsn "$INLINE_DSN" --workspace "$WORKSPACE" \
    mount "$MOUNT_POINT" >"$LOG_FILE" 2>&1; then
    fail "DSN 明文密码被错误接受"
fi
grep -q "must not contain a password" "$LOG_FILE" || fail "明文密码错误信息不明确"
pass

# 凭据副本只在挂载期间存在，权限固定为 0600。
"$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" mount "$MOUNT_POINT" >/dev/null
MOUNTED=true
printf '%s\n' 'credential-reload-ok' >"$MOUNT_POINT/reload.txt"
STAGED_COUNT="$(find "$RESOURCE_ROOT" -name .vexfs-pgpass -type f -print | wc -l | tr -d ' ')"
STAGED_PATH="$(find "$RESOURCE_ROOT" -name .vexfs-pgpass -type f -print -quit)"
[ "$STAGED_COUNT" -eq 1 ] || fail "挂载期间应恰好存在一个受保护凭据副本"
[ "$(stat -f '%Lp' "$STAGED_PATH")" = 600 ] || fail "凭据副本权限不是 0600"
pass
pass

# 用新的 CLI 进程检查现有挂载，凭据不能只活在首次 mount 命令的内存里。
"$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" \
    --json mount status "$MOUNT_POINT" | grep -q "\"workspace\":\"$WORKSPACE\"" || \
    fail "新 CLI 进程无法识别 PostgreSQL 挂载"
[ "$(cat "$MOUNT_POINT/reload.txt")" = credential-reload-ok ] || \
    fail "新 CLI 进程下文件读取失败"
pass
pass

"$CLI" fs --backend pg --dsn "$DSN" --workspace "$WORKSPACE" unmount "$MOUNT_POINT" >/dev/null
MOUNTED=false
if find "$RESOURCE_ROOT" -name .vexfs-pgpass -type f -print -quit | grep -q .; then
    fail "最后一个挂载卸载后仍遗留 PostgreSQL 凭据"
fi
pass

# 模拟 CLI 异常退出的遗留文件，doctor 必须清理未挂载资源中的凭据。
mkdir -p "$STALE_DIR"
chmod 0700 "$STALE_DIR"
printf '%s\n' stale >"$STALE_DIR/.vexfs-pgpass"
chmod 0600 "$STALE_DIR/.vexfs-pgpass"
"$CLI" fs --json doctor >/dev/null 2>&1 || true
[ ! -e "$STALE_DIR/.vexfs-pgpass" ] || fail "doctor 没有清理未挂载的凭据"
pass

echo "VEXFS PG MACOS CREDENTIAL LIFECYCLE: PASS ($CHECKS checks)"
