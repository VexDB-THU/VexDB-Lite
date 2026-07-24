#!/bin/bash
set -euo pipefail

BUILD_DIR=/vexfs-build
BUILD_LOG=/tmp/vexfs-pg-cross-gateway-build.log

if ! cmake -S /workspace/vexdb_sqlite -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DVEXDB_SQLITE_BUILD_TESTS=ON >"$BUILD_LOG" 2>&1; then
    tail -n 200 "$BUILD_LOG"
    exit 1
fi
if ! cmake --build "$BUILD_DIR" -j 1 >>"$BUILD_LOG" 2>&1; then
    tail -n 200 "$BUILD_LOG"
    exit 1
fi

VEXFS_MOUNT_BACKEND=postgresql \
VEXFS_MOUNT_DSN=postgresql://postgres@127.0.0.1:5432/test \
    python3 /workspace/tests/eval/vexfs/run.py \
        --root /workspace \
        --build-dir "$BUILD_DIR" \
        --output-dir /workspace/vexdb_sqlite/build/eval/vexfs-pg-cross-gateway/linux \
        --mode quick \
        --fail-on-skip \
        --filter portability.pg-cross-gateway
