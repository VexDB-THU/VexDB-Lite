#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
VERSIONS="${VEXDB_PG_MATRIX_VERSIONS:-16 17 18 19}"
MEMORY="${VEXFS_DOCKER_MEMORY:-1g}"
MEMORY_SWAP="${VEXFS_DOCKER_MEMORY_SWAP:-1g}"
CONTEXT="${TMPDIR:-/tmp}/vexfs-pg-version-matrix-context"
CHECKS=0
ACTIVE_CONTAINER=""
ACTIVE_CONTAINER_OWNED=0
PG19_CONTAINER="${VEXDB_PG19_MATRIX_CONTAINER:-${VEXDB_PG_CONTAINER:-}}"
PYTHON_BIN="${VEXDB_LITE_PYTHON:-}"

if [ -n "$PYTHON_BIN" ]; then
    [ -x "$PYTHON_BIN" ] || {
        echo "VEXDB_LITE_PYTHON 不可执行：$PYTHON_BIN" >&2
        exit 2
    }
    "$PYTHON_BIN" -c 'import sys; raise SystemExit(sys.version_info < (3, 8))' || {
        echo "VEXDB_LITE_PYTHON 需要 Python 3.8 或更高版本：$PYTHON_BIN" >&2
        exit 2
    }
else
    for candidate in \
        "$(command -v python3 2>/dev/null || true)" \
        /opt/homebrew/bin/python3 \
        /opt/anaconda3/bin/python3 \
        /usr/bin/python3; do
        [ -n "$candidate" ] && [ -x "$candidate" ] || continue
        if "$candidate" -c \
                'import sys; raise SystemExit(sys.version_info < (3, 8))' \
                >/dev/null 2>&1; then
            PYTHON_BIN="$candidate"
            break
        fi
    done
    [ -n "$PYTHON_BIN" ] || {
        echo "PG 版本矩阵需要 Python 3.8 或更高版本" >&2
        exit 2
    }
fi

cleanup_container() {
    if [ -n "$ACTIVE_CONTAINER" ] && [ "$ACTIVE_CONTAINER_OWNED" -eq 1 ]; then
        docker rm -f "$ACTIVE_CONTAINER" >/dev/null 2>&1 || true
    fi
    ACTIVE_CONTAINER=""
    ACTIVE_CONTAINER_OWNED=0
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    cleanup_container
    rm -rf "$CONTEXT"
    exit "$status"
}
trap cleanup EXIT INT TERM

case "$MEMORY" in ''|*[!0-9gGmMkK.]*) echo "VEXFS_DOCKER_MEMORY 格式无效" >&2; exit 2;; esac
case " $VERSIONS " in
    *" 19 "*)
        [ -n "$PG19_CONTAINER" ] || {
            echo "PG19 尚无官方 postgres:19 镜像；请用 VEXDB_PG19_MATRIX_CONTAINER 指定已安装当前扩展的受控 PG19 测试容器" >&2
            exit 2
        }
        ;;
esac
rm -rf "$CONTEXT"
mkdir -p "$CONTEXT/vexdb_lite_src"
cp "$ROOT/tests/eval/vexfs/Dockerfile.pg-version-matrix" "$CONTEXT/Dockerfile"
(
    cd "$ROOT"
    tar -cf - common vexdb_pg thirdparties
) | (
    cd "$CONTEXT/vexdb_lite_src"
    tar -xf -
)

"$PYTHON_BIN" "$ROOT/tests/spec/_lib/render.py" --engine pg --out "$ROOT/build/spec"

for version in $VERSIONS; do
    case "$version" in 16|17|18|19) ;; *) echo "不支持 PG $version" >&2; exit 2;; esac
    image="vexdb-pg-vexfs-matrix:$version"
    container="vexdb-pg-vexfs-matrix-$version"
    cleanup_container
    if [ "$version" = 19 ] && [ -n "$PG19_CONTAINER" ]; then
        container="$PG19_CONTAINER"
        docker inspect "$container" >/dev/null
        ACTIVE_CONTAINER="$container"
        ACTIVE_CONTAINER_OWNED=0
    else
        docker rm -f "$container" >/dev/null 2>&1 || true
        docker build \
            --build-arg "PG_MAJOR=$version" \
            --build-arg JOBS=1 \
            --tag "$image" \
            "$CONTEXT"

        docker run -d \
            --name "$container" \
            --memory "$MEMORY" \
            --memory-swap "$MEMORY_SWAP" \
            --env POSTGRES_HOST_AUTH_METHOD=trust \
            --env POSTGRES_DB=test \
            "$image" \
            -c shared_preload_libraries=vexdb_lite >/dev/null
        ACTIVE_CONTAINER="$container"
        ACTIVE_CONTAINER_OWNED=1

        initialized=0
        for _ in $(seq 1 240); do
            if docker logs "$container" 2>&1 | \
                    grep -q 'PostgreSQL init process complete'; then
                initialized=1
                break
            fi
            sleep 0.25
        done
        if [ "$initialized" -ne 1 ]; then
            docker logs --tail 200 "$container" >&2
            echo "PG $version 初始化超时" >&2
            exit 1
        fi
    fi

    ready=0
    for _ in $(seq 1 120); do
        if docker exec "$container" psql -U postgres -d test -X -q -t -A \
                -c 'SELECT 1' 2>/dev/null | grep -qx 1; then
            ready=1
            break
        fi
        sleep 0.25
    done
    if [ "$ready" -ne 1 ]; then
        docker logs --tail 200 "$container" >&2
        echo "PG $version 启动超时" >&2
        exit 1
    fi
    MEMORY_MAX="$(docker exec "$container" cat /sys/fs/cgroup/memory.max)"
    if [[ ! "$MEMORY_MAX" =~ ^[0-9]+$ ]] || [ "$MEMORY_MAX" -gt 1073741824 ]; then
        echo "PG $version 容器必须设置不超过 1 GiB 的 memory.max，实际 $MEMORY_MAX" >&2
        exit 1
    fi
    OOM_BEFORE="$(docker exec "$container" awk '$1=="oom_kill" {print $2}' \
        /sys/fs/cgroup/memory.events)"
    docker exec "$container" psql -U postgres -d test -X -q -v ON_ERROR_STOP=1 \
        -c "CREATE EXTENSION IF NOT EXISTS vexdb_lite VERSION '1.0';" >/dev/null

    actual_version="$(docker exec "$container" psql -U postgres -d test -X -q -t -A \
        -c "SELECT current_setting('server_version_num')::integer / 10000,
                   vexfs_pg_adapter_version();")"
    if [[ "$actual_version" != "$version|0.4.0-alpha.1" ]]; then
        echo "PG $version 版本/adapter 错误：$actual_version" >&2
        exit 1
    fi
    CHECKS=$((CHECKS + 2))

    VEXDB_PG_CONTAINER="$container" \
        bash "$ROOT/tests/spec/_lib/docker/run_pg.sh" test 'pg__vexfs_*'
    CHECKS=$((CHECKS + 7))
    OOM_AFTER="$(docker exec "$container" awk '$1=="oom_kill" {print $2}' \
        /sys/fs/cgroup/memory.events)"
    if [ "$OOM_AFTER" != "$OOM_BEFORE" ]; then
        echo "PG $version 矩阵触发 OOM kill：$OOM_BEFORE -> $OOM_AFTER" >&2
        exit 1
    fi
    cleanup_container
done

echo "VEXFS PG 16-19 VERSION MATRIX: PASS ($CHECKS checks)"
