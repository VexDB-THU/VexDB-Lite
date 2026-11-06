#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PYTHON="$ROOT/tests/eval/vexfs/python.sh"
IMAGE="${VEXFS_LINUX_EVAL_IMAGE:-vexdb-lite-vexfs-linux-eval:debian}"
HOST_BUILD_DIR="${VEXFS_MACOS_BUILD_DIR:-$ROOT/vexdb_sqlite/build}"
MOUNT_CLI="${VEXFS_EVAL_MOUNT_CLI:?set VEXFS_EVAL_MOUNT_CLI to the signed CLI from the current build}"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$"
OUTPUT_ROOT="$ROOT/vexdb_sqlite/build/eval/vexfs-portability/$RUN_ID"
DATABASE="$OUTPUT_ROOT/shared.sqlite3"
RELATIVE_DATABASE="${DATABASE#"$ROOT/"}"

mkdir -p "$OUTPUT_ROOT/mac-create" "$OUTPUT_ROOT/linux-roundtrip" "$OUTPUT_ROOT/mac-verify"

require_pass() {
    local report="$1"
    "$PYTHON" -c \
        'import json,sys; from pathlib import Path; latest=Path(sys.argv[1]); pointer=json.load(open(latest)); reports=sorted(latest.parent.glob("*/report.json"), key=lambda path: path.stat().st_mtime); assert reports, latest; value=json.load(open(reports[-1])); summary=value["summary"]; assert pointer["status"] == "PASS" and summary["failed"] == 0 and summary["passed"] == 1 and summary["skipped"] == 0, summary' \
        "$report"
}

echo "=== macOS FSKit 创建共享工作区 ==="
VEXFS_PORTABILITY_PHASE=mac-create \
VEXFS_PORTABILITY_DB="$DATABASE" \
"$PYTHON" "$ROOT/tests/eval/vexfs/run.py" \
    --root "$ROOT" \
    --build-dir "$HOST_BUILD_DIR" \
    --output-dir "$OUTPUT_ROOT/mac-create" \
    --mode quick \
    --fail-on-skip \
    --mount-cli "$MOUNT_CLI" \
    --filter portability.cross-os-roundtrip
require_pass "$OUTPUT_ROOT/mac-create/latest.json"

echo "=== Linux libfuse3 读取、恢复并修改同一数据库 ==="
docker build \
    --file "$ROOT/tests/eval/vexfs/Dockerfile.linux-fuse" \
    --tag "$IMAGE" \
    "$ROOT/tests/eval/vexfs"
docker run --rm \
    --cap-add SYS_ADMIN \
    --device /dev/fuse \
    --security-opt apparmor=unconfined \
    --env VEXFS_PORTABILITY_PHASE=linux-roundtrip \
    --env "VEXFS_PORTABILITY_DB=/workspace/$RELATIVE_DATABASE" \
    --env "VEXFS_PORTABILITY_OUTPUT=/workspace/${OUTPUT_ROOT#"$ROOT/"}/linux-roundtrip" \
    --volume "$ROOT:/workspace" \
    --workdir /workspace \
    "$IMAGE" \
    /bin/bash /workspace/tests/eval/vexfs/run_cross_platform_portability_in_container.sh
require_pass "$OUTPUT_ROOT/linux-roundtrip/latest.json"

echo "=== macOS FSKit 回读并恢复 Linux 快照 ==="
VEXFS_PORTABILITY_PHASE=mac-verify \
VEXFS_PORTABILITY_DB="$DATABASE" \
"$PYTHON" "$ROOT/tests/eval/vexfs/run.py" \
    --root "$ROOT" \
    --build-dir "$HOST_BUILD_DIR" \
    --output-dir "$OUTPUT_ROOT/mac-verify" \
    --mode quick \
    --fail-on-skip \
    --mount-cli "$MOUNT_CLI" \
    --filter portability.cross-os-roundtrip
require_pass "$OUTPUT_ROOT/mac-verify/latest.json"

echo "VEXFS CROSS-PLATFORM PORTABILITY: PASS"
echo "  database: $DATABASE"
echo "  reports: $OUTPUT_ROOT"
