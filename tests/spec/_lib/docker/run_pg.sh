#!/usr/bin/env bash
# PG 16-19 + vexdb_lite 测试 runner（默认 PG 19）
#
# 用法:
#   bash run_pg.sh build              # 构建镜像 (首次 ~30min: build PG + vexdb_lite)
#   bash run_pg.sh up                 # 启动容器 (后台)
#   bash run_pg.sh down               # 停止 + 删容器
#   bash run_pg.sh shell              # psql 进入容器 test 数据库
#   bash run_pg.sh test [pattern]     # 跑 spec 测试, 默认全部. pattern 例: 'types__*'
#   PG_MAJOR=18 bash run_pg.sh test   # 指定 PostgreSQL 大版本
#   bash run_pg.sh matrix             # 依次构建并测试 PG 16/17/18/19
#   bash run_pg.sh logs               # 查看 PG server 日志
#   bash run_pg.sh status             # 容器状态
#
# 测试输出: build/spec/pg/results/<spec>.diff (失败时), 以及汇总到 stdout
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../../../.." && pwd)"
# PG source lives inside this repo (vexdb_lite/duckdb). The legacy
# separate ~/PersonalProjects/PG_VEXDB checkout was retired 2026-05-13.
PG_VEXDB_SRC="${PG_VEXDB_SRC:-$ROOT_DIR}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
SPEC_DIR="${ROOT_DIR}/build/spec/pg"
RESULTS_DIR="${SPEC_DIR}/results"
PG_MAJOR="${PG_MAJOR:-19}"
case "$PG_MAJOR" in
    16|17|18|19) ;;
    *) echo "PG_MAJOR must be one of: 16, 17, 18, 19" >&2; exit 2 ;;
esac
PG_GIT_TAG="${PG_GIT_TAG:-REL_${PG_MAJOR}_STABLE}"
IMAGE="vexdb_pg${PG_MAJOR}:latest"
CONTAINER="vexdb_pg${PG_MAJOR}-test"
if [[ "$PG_MAJOR" == "19" ]]; then
    PG_HOST_PORT="${PG_HOST_PORT:-5433}"
else
    PG_HOST_PORT="${PG_HOST_PORT:-$((5400 + PG_MAJOR))}"
fi

YEL=$'\033[1;33m'; GRN=$'\033[0;32m'; RED=$'\033[0;31m'; NC=$'\033[0m'
info() { printf '%s[pg]%s %s\n' "$YEL" "$NC" "$*"; }
ok()   { printf '%s[pg]%s %s\n' "$GRN" "$NC" "$*"; }
fail() { printf '%s[pg]%s %s\n' "$RED" "$NC" "$*" >&2; }

require_docker() {
    if ! docker info >/dev/null 2>&1; then
        fail "Docker daemon 未启动. 请先打开 Docker Desktop"
        exit 1
    fi
}

cmd_build() {
    require_docker
    [[ -d "$PG_VEXDB_SRC" ]] || { fail "PG_VEXDB src 不存在: $PG_VEXDB_SRC"; exit 1; }

    info "准备 build 上下文 → /tmp/vexdb_pg-ctx"
    rm -rf /tmp/vexdb_pg-ctx
    mkdir -p /tmp/vexdb_pg-ctx
    cp "${ROOT_DIR}/tests/spec/_lib/docker/Dockerfile" /tmp/vexdb_pg-ctx/
    mkdir -p /tmp/vexdb_pg-ctx/vexdb_lite_src
    (
        cd "$PG_VEXDB_SRC"
        tar \
            --exclude='./.git' \
            --exclude='./build' \
            --exclude='./dist' \
            --exclude='./release' \
            --exclude='./.cache' \
            --exclude='./.pytest_cache' \
            --exclude='./cmake-build-*' \
            --exclude='*.dSYM' \
            -cf - common vexdb_pg thirdparties
    ) | (
        cd /tmp/vexdb_pg-ctx/vexdb_lite_src
        tar -xf -
    )

    info "docker build (首次 ~30min, build PG ${PG_MAJOR} + vexdb_lite)"
    # CI 通过 BUILDX_EXTRA_ARGS 传 cache 参数 (例: --cache-from type=gha --cache-to type=gha,mode=max)
    if [[ -n "${BUILDX_EXTRA_ARGS:-}" ]]; then
        # shellcheck disable=SC2086  # 故意让 BUILDX_EXTRA_ARGS 按空格 split
        docker buildx build $BUILDX_EXTRA_ARGS --load --tag "$IMAGE" \
            --build-arg "PG_GIT_TAG=$PG_GIT_TAG" /tmp/vexdb_pg-ctx
    else
        docker build --tag "$IMAGE" --build-arg "PG_GIT_TAG=$PG_GIT_TAG" /tmp/vexdb_pg-ctx
    fi
    ok "镜像就绪: $IMAGE"
}

cmd_up() {
    require_docker
    if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER"; then
        info "container $CONTAINER 已存在, 启动"
        docker start "$CONTAINER" >/dev/null
    else
        info "首次启动 container"
        # 默认只绑 localhost; 跨主机访问需 PG_PUBLISH_HOST=0.0.0.0 显式打开
        docker run -d --name "$CONTAINER" \
            -p "${PG_PUBLISH_HOST:-127.0.0.1}:${PG_HOST_PORT}:5432" "$IMAGE"
    fi
    wait_pg_ready
}

wait_pg_ready() {
    info "等待 PG ready..."
    for _ in $(seq 1 30); do
        if docker exec "$CONTAINER" psql -d test -c 'SELECT 1' >/dev/null 2>&1; then
            ok "PG ready on localhost:${PG_HOST_PORT} db=test (容器内 5432)"
            return 0
        fi
        sleep 1
    done
    fail "等待超时"; docker logs --tail 50 "$CONTAINER"; exit 1
}

restart_pg_immediate() {
    info "按 spec 指令立即重启 PostgreSQL（验证 WAL 恢复）"
    docker exec "$CONTAINER" pg_ctl -D /var/lib/postgresql/data \
        -l /tmp/pg.log -m immediate -w restart >/dev/null
    wait_pg_ready
}

run_spec_sql() {
    local sql_file="$1"
    local actual="$2"
    local segment_dir
    segment_dir="$(mktemp -d "${SPEC_DIR}/pg-segments.XXXXXX")"

    # renderer 用精确的独立行 `-- @restart` 标记连接边界。切段后按顺序执行，
    # 并在每个后续段之前立即重启 PG；这样有 CHECKPOINT 的场景验证磁盘数据，
    # 无 CHECKPOINT 的场景验证 WAL 恢复。
    awk -v dir="$segment_dir" '
        BEGIN { part = 0; path = sprintf("%s/%04d.sql", dir, part) }
        $0 == "-- @restart" {
            close(path)
            part++
            path = sprintf("%s/%04d.sql", dir, part)
            next
        }
        { print >> path }
        END { close(path) }
    ' "$sql_file"

    : > "$actual"
    local first=1
    local segment
    for segment in "$segment_dir"/*.sql; do
        if (( first == 0 )); then
            restart_pg_immediate
        fi
        first=0
        docker exec -i "$CONTAINER" psql -d test -X -q -t -A -F '|' -P pager=off \
            >> "$actual" 2>&1 < "$segment" || true
    done
    rm -rf "$segment_dir"
}

cmd_down() {
    require_docker
    docker rm -f "$CONTAINER" >/dev/null 2>&1 || true
    ok "已停止 + 删除"
}

cmd_shell() {
    docker exec -it "$CONTAINER" psql -d test
}

cmd_logs() {
    docker exec "$CONTAINER" tail -100 /tmp/pg.log
}

cmd_status() {
    docker ps -a --filter "name=$CONTAINER" --format 'table {{.Names}}\t{{.Status}}\t{{.Ports}}'
}

cmd_render() {
    info "运行 renderer 安全回归"
    "$PYTHON_BIN" "${ROOT_DIR}/tests/spec/_lib/test_render_safety.py"
    info "render spec → ${SPEC_DIR#$ROOT_DIR/}"
    # The rendered file name comes from the spec's `name`, not its YAML path.
    # Recreate this engine directory so a renamed spec cannot leave an old SQL
    # file behind and make a full run report a false extra pass/failure.
    "$PYTHON_BIN" "${ROOT_DIR}/tests/spec/_lib/render.py" --engine pg --out "${ROOT_DIR}/build/spec"
}

cmd_test() {
    require_docker
    local pattern="${1:-*}"
    cmd_render
    docker ps --format '{{.Names}}' | grep -qx "$CONTAINER" || cmd_up

    mkdir -p "$RESULTS_DIR"
    rm -f "$RESULTS_DIR"/*.diff "$RESULTS_DIR"/summary.txt

    local pass=0 failed_count=0
    local -a failures=()
    for sql_file in "$SPEC_DIR/sql/"$pattern.sql; do
        [[ -e "$sql_file" ]] || continue
        local name="$(basename "$sql_file" .sql)"
        local expected="$SPEC_DIR/expected/${name}.out"
        local actual="$RESULTS_DIR/${name}.actual"
        local diff_file="$RESULTS_DIR/${name}.diff"

        # -X: 不读 .psqlrc; -q: quiet (不输出 CREATE TABLE 等); -t: 不带 header/footer
        # -A: unaligned; -F '|': 字段分隔符; --no-psqlrc; ON_ERROR_STOP 失败立即停
        # 每个 spec 跑前: 删除 public schema 下所有用户表 (保留 extension)
        docker exec -i "$CONTAINER" psql -d test -X -q -t -A -F '|' -P pager=off > /dev/null 2>&1 <<'EOF'
DO $$
DECLARE r record;
BEGIN
  FOR r IN SELECT tablename FROM pg_tables WHERE schemaname='public' LOOP
    EXECUTE 'DROP TABLE IF EXISTS public.' || quote_ident(r.tablename) || ' CASCADE';
  END LOOP;
END $$;
EOF
        run_spec_sql "$sql_file" "$actual"
        # 过滤 psql 命令完成消息 + 服务端日志级别消息 + 报错位置标记 (^ / LINE).
        # 单条 ERE 替代之前 23 个 -e 模式.
        sed -i.bak -E \
            -e '/^(CREATE|INSERT|DROP|DELETE|UPDATE|TRUNCATE|ALTER|COPY|SELECT|Time:|EXPLAIN) /d' \
            -e '/^(SET|BEGIN|COMMIT|ROLLBACK|EXPLAIN)$/d' \
            -e '/^(NOTICE|WARNING|INFO|DEBUG|HINT|DETAIL|CONTEXT):/d' \
            -e '/^LINE [0-9]/d' -e '/^[ \t]*\^[ \t]*$/d' \
            "$actual" 2>/dev/null
        rm -f "${actual}.bak"
        # 删除尾部空白行
        awk 'NF || prev_nf { print } { prev_nf = NF }' "$actual" > "${actual}.tmp" && mv "${actual}.tmp" "$actual"

        if [[ -f "$expected" ]]; then
            if "$PYTHON_BIN" "${ROOT_DIR}/tests/spec/_lib/docker/compare.py" "$expected" "$actual" 2>"$diff_file"; then
                pass=$((pass+1))
                rm -f "$diff_file"
                printf '  %s%s%s %s\n' "$GRN" "PASS" "$NC" "$name"
            else
                failed_count=$((failed_count+1))
                failures+=("$name")
                printf '  %sFAIL%s %s (see %s)\n' "$RED" "$NC" "$name" "${diff_file#$ROOT_DIR/}"
            fi
        else
            failed_count=$((failed_count+1))
            failures+=("$name")
            printf '  %sFAIL%s %s (no expected)\n' "$RED" "$NC" "$name"
        fi
    done

    {
        echo "=== PG spec test summary ==="
        echo "passed: $pass / $((pass+failed_count))"
        if (( failed_count > 0 )); then
            echo ""
            echo "failures:"
            printf '  %s\n' "${failures[@]}"
        fi
    } | tee "$RESULTS_DIR/summary.txt"

    (( failed_count == 0 ))
}

cmd_matrix() {
    require_docker
    local major
    for major in 16 17 18 19; do
        info "=== PostgreSQL $major matrix ==="
        PG_MAJOR="$major" "$0" build
        PG_MAJOR="$major" "$0" down
        PG_MAJOR="$major" "$0" up
        PG_MAJOR="$major" "$0" test
        PG_MAJOR="$major" "$0" down
    done
    ok "PostgreSQL 16/17/18/19 matrix passed"
}

case "${1:-}" in
    build)  cmd_build ;;
    up)     cmd_up ;;
    down)   cmd_down ;;
    shell)  cmd_shell ;;
    test)   shift; cmd_test "$@" ;;
    matrix) cmd_matrix ;;
    logs)   cmd_logs ;;
    status) cmd_status ;;
    "" | -h | --help) sed -n '2,16p' "$0" ;;
    *) fail "unknown command: ${1:-}; see -h for usage" ;;
esac
