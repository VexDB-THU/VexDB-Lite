#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PYTHON="$ROOT/tests/eval/vexfs/python.sh"
IMAGE="${VEXFS_LINUX_EVAL_IMAGE:-vexdb-lite-vexfs-linux-eval:debian}"
PG_CONTAINER="${VEXDB_PG_CONTAINER:-vexdb_pg19-test}"
DSN="${VEXDB_PG_DSN:-postgresql://postgres@127.0.0.1:5433/test}"
MOUNT_CLI="${VEXFS_EVAL_MOUNT_CLI:-$HOME/.local/bin/vexdb}"
MEMORY="${VEXFS_DOCKER_MEMORY:-1g}"
MEMORY_SWAP="${VEXFS_DOCKER_MEMORY_SWAP:-1g}"
BUILD_VOLUME="${VEXFS_PG_CROSS_GATEWAY_BUILD_VOLUME:-vexfs-pg-cross-gateway-build}"

[ "$(uname -s)" = Darwin ] || { echo "跨网关编排需要从 macOS 运行" >&2; exit 2; }
[ -x "$MOUNT_CLI" ] || { echo "找不到已安装的签名 CLI：$MOUNT_CLI" >&2; exit 2; }
docker inspect "$PG_CONTAINER" >/dev/null

drop_workspace() {
    docker exec "$PG_CONTAINER" psql -U postgres -d test -X -q \
        -c "SELECT vexfs_workspace_drop('pg-cross-gateway', true);" \
        >/dev/null 2>&1 || true
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    drop_workspace
    exit "$status"
}
trap cleanup EXIT INT TERM
drop_workspace

docker build \
    --file "$ROOT/tests/eval/vexfs/Dockerfile.linux-fuse" \
    --tag "$IMAGE" \
    "$ROOT/tests/eval/vexfs"

run_mac_phase() {
    local phase=$1
    VEXFS_MOUNT_BACKEND=postgresql \
    VEXFS_MOUNT_DSN="$DSN" \
    VEXFS_PG_CROSS_GATEWAY_PHASE="$phase" \
        "$PYTHON" "$ROOT/tests/eval/vexfs/run.py" \
            --root "$ROOT" \
            --build-dir "$ROOT/vexdb_sqlite/build" \
            --output-dir "$ROOT/vexdb_sqlite/build/eval/vexfs-pg-cross-gateway/macos" \
            --mode quick \
            --mount-cli "$MOUNT_CLI" \
            --fail-on-skip \
            --filter portability.pg-cross-gateway
}

run_linux_phase() {
    local phase=$1
    docker run --rm \
        --memory "$MEMORY" \
        --memory-swap "$MEMORY_SWAP" \
        --network "container:$PG_CONTAINER" \
        --cap-add SYS_ADMIN \
        --device /dev/fuse \
        --security-opt apparmor=unconfined \
        --env "VEXFS_PG_CROSS_GATEWAY_PHASE=$phase" \
        --volume "$ROOT:/workspace" \
        --volume "$BUILD_VOLUME:/vexfs-build" \
        --workdir /workspace \
        "$IMAGE" \
        /bin/bash /workspace/tests/eval/vexfs/run_pg_cross_gateway_linux_in_container.sh
}

run_mac_phase mac-create
run_linux_phase linux-modify
run_mac_phase mac-verify
run_linux_phase linux-final

echo "VEXFS PG CROSS GATEWAY ROUNDTRIP: PASS"
