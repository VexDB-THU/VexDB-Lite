#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
IMAGE="${VEXFS_LINUX_EVAL_IMAGE:-vexdb-lite-vexfs-linux-eval:debian}"

docker build \
    --file "$ROOT/tests/eval/vexfs/Dockerfile.linux-fuse" \
    --tag "$IMAGE" \
    "$ROOT/tests/eval/vexfs"

docker run --rm \
    --cap-add SYS_ADMIN \
    --device /dev/fuse \
    --security-opt apparmor=unconfined \
    --env "VEXFS_LINUX_EVAL_CASES=${VEXFS_LINUX_EVAL_CASES:-}" \
    --volume "$ROOT:/workspace" \
    --workdir /workspace \
    "$IMAGE" \
    /bin/bash /workspace/tests/eval/vexfs/run_linux_mount_in_container.sh
