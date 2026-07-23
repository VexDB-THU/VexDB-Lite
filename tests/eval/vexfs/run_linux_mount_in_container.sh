#!/bin/bash
set -euo pipefail

BUILD_DIR=/tmp/vexfs-linux-build
OUTPUT_BASE=/workspace/vexdb_sqlite/build/eval/vexfs-linux-mount
ROOT_OUTPUT="$OUTPUT_BASE/root"
USER_OUTPUT="$OUTPUT_BASE/uid-1000"
BUILD_LOG=/tmp/vexfs-linux-build.log

if ! cmake -S /workspace/vexdb_sqlite -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DVEXDB_SQLITE_BUILD_TESTS=ON >"$BUILD_LOG" 2>&1; then
    tail -n 200 "$BUILD_LOG"
    exit 1
fi
if ! cmake --build "$BUILD_DIR" -j "$(nproc)" >>"$BUILD_LOG" 2>&1; then
    tail -n 200 "$BUILD_LOG"
    exit 1
fi

read -r -a MOUNT_CASES <<< "${VEXFS_LINUX_EVAL_CASES:-mount.cross-platform-conformance mount.timestamps mount.concurrent-append mount.open-rename-unlink mount.process-locks mount.force-unmount mount.helper-crash-recovery}"

for test_case in "${MOUNT_CASES[@]}"; do
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
    su --shell /bin/bash vexeval --command \
        "python3 /workspace/tests/eval/vexfs/run.py \
            --root /workspace \
            --build-dir $BUILD_DIR \
            --output-dir $USER_OUTPUT \
            --mode quick \
            --fail-on-skip \
            --filter $test_case"
done

echo "VEXFS LINUX MOUNT CONFORMANCE: PASS"
