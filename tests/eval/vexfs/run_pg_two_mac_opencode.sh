#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NODE_SCRIPT="$ROOT/tests/eval/vexfs/run_pg_two_mac_opencode_node.sh"
REMOTE_HOST="${VEXFS_REMOTE_HOST:?VEXFS_REMOTE_HOST is required}"
REMOTE_USER="${VEXFS_REMOTE_USER:-$USER}"
LOCAL_CLI="${VEXFS_EVAL_MOUNT_CLI:-$HOME/.local/bin/vexdb}"
REMOTE_CLI="${VEXFS_REMOTE_CLI:-/Users/$REMOTE_USER/.local/bin/vexdb}"
LOCAL_OPENCODE="${VEXFS_LOCAL_OPENCODE:-$(command -v opencode || true)}"
# Apple Silicon 优先使用原生 Homebrew。Rosetta 下的 x86_64 Bun 会在无 AVX 的 M1 上
# 警告并可能在 agent server 启动后崩溃；Intel Mac 可显式覆盖此路径。
REMOTE_OPENCODE="${VEXFS_REMOTE_OPENCODE:-/opt/homebrew/bin/opencode}"
CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-vexfs-dev}"
PG_DATABASE="${VEXFS_PG_DATABASE:-test}"
PG_USER="${VEXFS_PG_USER:-postgres}"
LOCAL_PG_PORT="${VEXFS_LOCAL_PG_PORT:-5434}"
REMOTE_TUNNEL_PORT="${VEXFS_REMOTE_TUNNEL_PORT:-6550}"
WORKSPACE="${VEXFS_PG_WORKSPACE:-pg-two-mac-opencode}"
LOCAL_MODEL="${VEXFS_LOCAL_OPENCODE_MODEL:-${VEXFS_EVAL_OPENCODE_MODEL:-openai/gpt-5.4-mini}}"
REMOTE_MODEL="${VEXFS_REMOTE_OPENCODE_MODEL:-opencode/north-mini-code-free}"
KEEP_WORKSPACE="${VEXFS_KEEP_WORKSPACE:-0}"
KNOWN_HOSTS="${VEXFS_REMOTE_KNOWN_HOSTS:-/tmp/vexfs-pg-opencode-known-hosts}"
CONTROL="/tmp/vexfs-pg-opencode-tunnel-$$.sock"
REMOTE_NODE="/tmp/vexfs-pg-opencode-node-$$.sh"
TARGET="$REMOTE_USER@$REMOTE_HOST"
LOCAL_DSN="host=127.0.0.1 port=$LOCAL_PG_PORT dbname=$PG_DATABASE user=$PG_USER connect_timeout=8"
REMOTE_DSN="host=127.0.0.1 port=$REMOTE_TUNNEL_PORT dbname=$PG_DATABASE user=$PG_USER connect_timeout=8"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
OUTPUT_DIR="${VEXFS_EVAL_OUTPUT_DIR:-$ROOT/vexdb_sqlite/build/eval/vexfs-pg-two-mac-opencode/$RUN_ID}"
LOG_FILE="$OUTPUT_DIR/run.log"
REPORT_FILE="$OUTPUT_DIR/report.md"
TUNNEL_RUNNING=false

case "$WORKSPACE" in
    ''|*[!A-Za-z0-9_-]*) echo "workspace 只能包含字母、数字、下划线和连字符" >&2; exit 2 ;;
esac
[ "$(uname -s)" = Darwin ] || { echo "双 Mac OpenCode 编排必须从 macOS 运行" >&2; exit 2; }
[ -x "$LOCAL_CLI" ] || { echo "找不到本机 vexdb CLI：$LOCAL_CLI" >&2; exit 2; }
[ -x "$LOCAL_OPENCODE" ] || { echo "找不到本机 OpenCode：$LOCAL_OPENCODE" >&2; exit 2; }
[ -f "$NODE_SCRIPT" ] || { echo "找不到节点脚本：$NODE_SCRIPT" >&2; exit 2; }
mkdir -p "$OUTPUT_DIR"
: >"$LOG_FILE"

SSH_OPTIONS=(-o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile="$KNOWN_HOSTS")
SSH=(ssh "${SSH_OPTIONS[@]}")
SCP=(scp "${SSH_OPTIONS[@]}")

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

vexfs_local() {
    "$LOCAL_CLI" fs --backend pg --dsn "$LOCAL_DSN" --workspace "$WORKSPACE" "$@"
}

drop_workspace() {
    docker exec "$CONTAINER" /opt/pg19/bin/psql -U postgres -d "$PG_DATABASE" -X -q \
        -c "SELECT public.vexfs_workspace_drop('$WORKSPACE', true);" >/dev/null 2>&1 || true
}

start_tunnel() {
    "${SSH[@]}" -M -S "$CONTROL" -fNT -o ExitOnForwardFailure=yes \
        -R "127.0.0.1:$REMOTE_TUNNEL_PORT:127.0.0.1:$LOCAL_PG_PORT" "$TARGET"
    TUNNEL_RUNNING=true
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    "$LOCAL_CLI" fs --backend pg --dsn "$LOCAL_DSN" --workspace "$WORKSPACE" \
        unmount --force >/dev/null 2>&1 || true
    remote_exec "$REMOTE_CLI" fs --backend pg --dsn "$REMOTE_DSN" \
        --workspace "$WORKSPACE" unmount --force >/dev/null 2>&1 || true
    remote_exec rm -f "$REMOTE_NODE" >/dev/null 2>&1 || true
    if [ "$TUNNEL_RUNNING" = true ]; then
        "${SSH[@]}" -S "$CONTROL" -O exit "$TARGET" >/dev/null 2>&1 || true
    fi
    if [ "$KEEP_WORKSPACE" != 1 ]; then
        drop_workspace
    fi
    exit "$status"
}
trap cleanup EXIT INT TERM

run_node() {
    local phase="$1" cli="$2" dsn="$3" opencode="$4" model="$5" script="$6"
    env VEXFS_NODE_PHASE="$phase" VEXFS_NODE_CLI="$cli" VEXFS_NODE_DSN="$dsn" \
        VEXFS_NODE_WORKSPACE="$WORKSPACE" VEXFS_NODE_OPENCODE="$opencode" \
        VEXFS_EVAL_OPENCODE_MODEL="$model" /bin/bash "$script"
}

run_remote_node() {
    local phase="$1" command
    printf -v command \
        'env VEXFS_NODE_PHASE=%q VEXFS_NODE_CLI=%q VEXFS_NODE_DSN=%q VEXFS_NODE_WORKSPACE=%q VEXFS_NODE_OPENCODE=%q VEXFS_EVAL_OPENCODE_MODEL=%q /bin/bash %q' \
        "$phase" "$REMOTE_CLI" "$REMOTE_DSN" "$WORKSPACE" "$REMOTE_OPENCODE" \
        "$REMOTE_MODEL" "$REMOTE_NODE"
    printf 'set -euo pipefail\n%s\n' "$command" | \
        "${SSH[@]}" "$TARGET" /bin/bash -s
}

drop_workspace
start_tunnel
remote_exec test -x "$REMOTE_CLI"
remote_exec test -x "$REMOTE_OPENCODE"
"${SCP[@]}" "$NODE_SCRIPT" "$TARGET:$REMOTE_NODE" >/dev/null
remote_exec chmod 0700 "$REMOTE_NODE"

{
    echo "VEXFS PG TWO-MAC OPENCODE: phase=mac1-create"
    run_node mac1-create "$LOCAL_CLI" "$LOCAL_DSN" "$LOCAL_OPENCODE" "$LOCAL_MODEL" "$NODE_SCRIPT"
    echo "VEXFS PG TWO-MAC OPENCODE: phase=mac2-modify"
    run_remote_node mac2-modify
    echo "VEXFS PG TWO-MAC OPENCODE: phase=mac1-verify-restore"
    run_node mac1-verify-restore "$LOCAL_CLI" "$LOCAL_DSN" "$LOCAL_OPENCODE" "$LOCAL_MODEL" "$NODE_SCRIPT"
} 2>&1 | tee "$LOG_FILE"

cat >"$REPORT_FILE" <<EOF
# VexFS PostgreSQL 双 Mac OpenCode 工作区测试

- 日期：$(date '+%Y-%m-%d %H:%M:%S %Z')
- Linux PostgreSQL：容器 \`$CONTAINER\`，本机端口 \`$LOCAL_PG_PORT\`
- 第一台 Mac：本机 FSKit + \`$LOCAL_OPENCODE\`
- 第二台 Mac：\`$REMOTE_HOST\`，FSKit + \`$REMOTE_OPENCODE\`
- workspace：\`$WORKSPACE\`
- 第一台 model：\`$LOCAL_MODEL\`
- 第二台 model：\`$REMOTE_MODEL\`
- 网络：SSH reverse tunnel，远端 \`127.0.0.1:$REMOTE_TUNNEL_PORT\` → PG \`127.0.0.1:$LOCAL_PG_PORT\`
- 结果：PASS

## 已验证

1. 第一台 Mac 挂载空 workspace，OpenCode 实现 \`multiply\`，测试通过并创建 \`mac1-opencode\` 快照。
2. 第二台 Mac 挂载同一 workspace，继承 Git 和测试，OpenCode 新增 \`power\`，创建 \`mac2-opencode\` 快照。
3. 第一台 Mac 再次挂载后看到第二台的提交和文件，全部测试通过。
4. CLI 能列出两个完整 workspace 快照、查看 \`calc.py\` 历史并比较两个快照。
5. 一条 \`snapshot restore mac1-opencode\` 恢复整个工作区，第二台新增文件和实现消失，第一阶段测试仍通过。
6. 原生 grep 和数据库 grep 都可用；PG 当前如实返回 \`index_used=false\`、\`available=false\`，未宣称索引加速。

完整过程见 \`run.log\`。
EOF

echo "VEXFS PG TWO-MAC OPENCODE WORKSPACE: PASS"
echo "report=$REPORT_FILE"
