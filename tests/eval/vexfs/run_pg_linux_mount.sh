#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
IMAGE="${VEXFS_LINUX_EVAL_IMAGE:-vexdb-lite-vexfs-linux-eval:debian}"
PG_CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
CASES="${VEXFS_LINUX_EVAL_CASES:-mount.cross-platform-conformance mount.timestamps mount.concurrent-append mount.open-rename-unlink mount.read-only-open-lifecycle mount.process-locks mount.force-unmount mount.helper-crash-recovery mount.real-linux-bash-git}"
MEMORY="${VEXFS_DOCKER_MEMORY:-1g}"
MEMORY_SWAP="${VEXFS_DOCKER_MEMORY_SWAP:-1g}"

docker inspect "$PG_CONTAINER" >/dev/null
docker build \
    --file "$ROOT/tests/eval/vexfs/Dockerfile.linux-fuse" \
    --tag "$IMAGE" \
    "$ROOT/tests/eval/vexfs"

# 与 PG 容器共享网络命名空间，测试流量不会向局域网公开；VexFS 仍通过
# TCP/libpq 连接 127.0.0.1:5432，等价于远端数据库的数据面。
docker run --rm \
    --memory "$MEMORY" \
    --memory-swap "$MEMORY_SWAP" \
    --network "container:$PG_CONTAINER" \
    --cap-add SYS_ADMIN \
    --device /dev/fuse \
    --security-opt apparmor=unconfined \
    --env VEXFS_MOUNT_BACKEND=postgresql \
    --env VEXFS_MOUNT_DSN=postgresql://postgres@127.0.0.1:5432/test \
    --env "VEXFS_LINUX_EVAL_CASES=$CASES" \
    --env VEXFS_BUILD_JOBS=1 \
    --volume "$ROOT:/workspace" \
    --workdir /workspace \
    "$IMAGE" \
    /bin/bash /workspace/tests/eval/vexfs/run_linux_mount_in_container.sh
