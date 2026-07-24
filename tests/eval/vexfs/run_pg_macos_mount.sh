#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PYTHON="$ROOT/tests/eval/vexfs/python.sh"
PG_CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DSN="${VEXDB_PG_DSN:-postgresql://postgres@127.0.0.1:5433/test}"
MOUNT_CLI="${VEXFS_EVAL_MOUNT_CLI:-$HOME/.local/bin/vexdb}"
OUTPUT="${VEXFS_MACOS_PG_OUTPUT:-$ROOT/vexdb_sqlite/build/eval/vexfs-pg-macos-mount}"
CASES="${VEXFS_MACOS_PG_CASES:-mount.cross-platform-conformance mount.timestamps mount.concurrent-append mount.open-rename-unlink mount.read-only-open-lifecycle mount.process-locks mount.force-unmount mount.real-bash mount.posix-metadata mount.performance mount.git-workspace mount.real-toolchain-projects mount.scale-tree}"

[ "$(uname -s)" = Darwin ] || { echo "该脚本只在 macOS 上运行" >&2; exit 2; }
[ -x "$MOUNT_CLI" ] || { echo "找不到已安装的签名 CLI：$MOUNT_CLI" >&2; exit 2; }
docker inspect "$PG_CONTAINER" >/dev/null

cleanup_pg_workspaces() {
    local workspace
    for workspace in conformance timestamps append open-life read-life locks \
        force-unmount eval posix perf git-eval toolchains mount-scale opencode; do
        docker exec "$PG_CONTAINER" psql -U postgres -d test -X -q \
            -v ON_ERROR_STOP=1 \
            -c "SELECT vexfs_workspace_drop('$workspace', true);" \
            >/dev/null 2>&1 || true
    done
}

cleanup_mounts() {
    local mount_point
    while IFS= read -r mount_point; do
        [ -n "$mount_point" ] || continue
        "$MOUNT_CLI" fs --backend pg --dsn "$DSN" --workspace conformance \
            unmount --force "$mount_point" >/dev/null 2>&1 || true
    done < <("$MOUNT_CLI" fs --backend pg --dsn "$DSN" --workspace conformance \
        --json mount status 2>/dev/null | \
        "$PYTHON" -c 'import json,sys; [print(x.get("mount_point", "")) for x in json.load(sys.stdin)]' \
        2>/dev/null || true)
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    cleanup_mounts
    cleanup_pg_workspaces
    exit "$status"
}
trap cleanup EXIT INT TERM

mkdir -p "$OUTPUT"
read -r -a mount_cases <<< "$CASES"
if [ "${VEXFS_EVAL_OPENCODE:-0}" = 1 ]; then
    mount_cases+=(mount.real-opencode-project)
fi

for test_case in "${mount_cases[@]}"; do
    cleanup_mounts
    cleanup_pg_workspaces
    VEXFS_MOUNT_BACKEND=postgresql \
    VEXFS_MOUNT_DSN="$DSN" \
    VEXFS_EVAL_OPENCODE="${VEXFS_EVAL_OPENCODE:-0}" \
    VEXFS_EVAL_OPENCODE_MODEL="${VEXFS_EVAL_OPENCODE_MODEL:-openai/gpt-5.4-mini}" \
        "$PYTHON" "$ROOT/tests/eval/vexfs/run.py" \
            --root "$ROOT" \
            --build-dir "$ROOT/vexdb_sqlite/build" \
            --output-dir "$OUTPUT" \
            --mode quick \
            --mount-cli "$MOUNT_CLI" \
            --fail-on-skip \
            --filter "$test_case"
done

echo "VEXFS PG MACOS MOUNT CONFORMANCE: PASS (${#mount_cases[@]} cases)"
