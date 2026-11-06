#!/bin/bash
set -euo pipefail

BUILD_DIR=/tmp/vexfs-portability-build
BUILD_LOG=/tmp/vexfs-portability-build.log
OUTPUT_DIR="${VEXFS_PORTABILITY_OUTPUT:?missing VEXFS_PORTABILITY_OUTPUT}"
: "${VEXFS_PORTABILITY_DB:?missing VEXFS_PORTABILITY_DB}"
: "${VEXFS_PORTABILITY_PHASE:?missing VEXFS_PORTABILITY_PHASE}"

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

python3 /workspace/tests/eval/vexfs/run.py \
    --root /workspace \
    --build-dir "$BUILD_DIR" \
    --output-dir "$OUTPUT_DIR" \
    --mode quick \
    --fail-on-skip \
    --filter portability.cross-os-roundtrip
