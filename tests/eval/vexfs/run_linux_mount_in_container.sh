#!/bin/bash
set -euo pipefail

BUILD_DIR="${VEXFS_LINUX_BUILD_DIR:-/workspace/.cache/vexfs-linux-build}"
OUTPUT_BASE=/workspace/vexdb_sqlite/build/eval/vexfs-linux-mount
ROOT_OUTPUT="$OUTPUT_BASE/root"
USER_OUTPUT="$OUTPUT_BASE/uid-1000"
BUILD_LOG="$BUILD_DIR/build.log"

mkdir -p "$BUILD_DIR"

if ! cmake -S /workspace/vexdb_sqlite -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DVEXDB_SQLITE_BUILD_TESTS=ON >"$BUILD_LOG" 2>&1; then
    tail -n 200 "$BUILD_LOG"
    exit 1
fi
BUILD_JOBS="${VEXFS_BUILD_JOBS:-1}"
case "$BUILD_JOBS" in ''|*[!0-9]*|0) echo "VEXFS_BUILD_JOBS 必须是正整数" >&2; exit 2;; esac
[ "$BUILD_JOBS" -le 2 ] || BUILD_JOBS=2
if ! cmake --build "$BUILD_DIR" -j "$BUILD_JOBS" >>"$BUILD_LOG" 2>&1; then
    tail -n 200 "$BUILD_LOG"
    exit 1
fi

read -r -a MOUNT_CASES <<< "${VEXFS_LINUX_EVAL_CASES:-mount.cross-platform-conformance mount.timestamps mount.concurrent-append mount.open-rename-unlink mount.process-locks mount.force-unmount mount.helper-crash-recovery}"

cleanup_pg_workspaces() {
    [ "${VEXFS_MOUNT_BACKEND:-sqlite}" = postgresql ] || return 0
    local workspace
    for workspace in conformance timestamps append open-life read-life locks \
        force-unmount helper-crash linux-eval toolchains; do
        psql "$VEXFS_MOUNT_DSN" -X -q -v ON_ERROR_STOP=1 \
            -c "SELECT vexfs_workspace_drop('$workspace', true);" >/dev/null 2>&1 || true
    done
}

for test_case in "${MOUNT_CASES[@]}"; do
    cleanup_pg_workspaces
    python3 /workspace/tests/eval/vexfs/run.py \
        --root /workspace \
        --build-dir "$BUILD_DIR" \
        --output-dir "$ROOT_OUTPUT" \
        --mode quick \
        --fail-on-skip \
        --filter "$test_case"
done

useradd --create-home --uid 1000 vexeval
chmod 0666 /dev/fuse
mkdir -p "$ROOT_OUTPUT" "$USER_OUTPUT"
chown vexeval:vexeval "$USER_OUTPUT"
for test_case in "${MOUNT_CASES[@]}"; do
    cleanup_pg_workspaces
    su --shell /bin/bash vexeval --command \
        "python3 /workspace/tests/eval/vexfs/run.py \
            --root /workspace \
            --build-dir $BUILD_DIR \
            --output-dir $USER_OUTPUT \
            --mode quick \
            --fail-on-skip \
            --filter $test_case"
done

cleanup_pg_workspaces

echo "VEXFS LINUX MOUNT CONFORMANCE: PASS"
