#!/usr/bin/env bash
# PG 19 + pg_vexdb 测试 runner
#
# 用法:
#   bash run_pg.sh build              # 构建镜像 (首次 ~30min: build PG + pg_vexdb)
#   bash run_pg.sh up                 # 启动容器 (后台)
#   bash run_pg.sh down               # 停止 + 删容器
#   bash run_pg.sh shell              # psql 进入容器 test 数据库
#   bash run_pg.sh test [pattern]     # 跑 spec 测试, 默认全部. pattern 例: 'types__*'
#   bash run_pg.sh logs               # 查看 PG server 日志
#   bash run_pg.sh status             # 容器状态
#
# 测试输出: build/spec/pg/results/<spec>.diff (失败时), 以及汇总到 stdout
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../../../.." && pwd)"
PG_VEXDB_SRC="${PG_VEXDB_SRC:-/Users/Four/PersonalProjects/PG_VEXDB}"
SPEC_DIR="${ROOT_DIR}/build/spec/pg"
RESULTS_DIR="${SPEC_DIR}/results"
IMAGE="vexdb-pg19:latest"
CONTAINER="vexdb-pg19-test"

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

    info "准备 build 上下文 → /tmp/vexdb-pg-ctx"
    rm -rf /tmp/vexdb-pg-ctx
    mkdir -p /tmp/vexdb-pg-ctx
    cp "${ROOT_DIR}/tests/spec/_lib/docker/Dockerfile" /tmp/vexdb-pg-ctx/
    cp -R "$PG_VEXDB_SRC" /tmp/vexdb-pg-ctx/pg_vexdb_src

    info "patch Makefile (PG_CONFIG / FLT16_SUPPORT / arch / 缺失源文件)"
    local mk=/tmp/vexdb-pg-ctx/pg_vexdb_src/Makefile
    sed -i.bak 's|^PG_CONFIG = .*|PG_CONFIG = /opt/pg19/bin/pg_config|' "$mk"
    sed -i.bak 's|^PG_CPPFLAGS = |PG_CPPFLAGS = -DFLT16_SUPPORT -DPG_EXTENSION -fpermissive |' "$mk"
    # 移除 -fvisibility=hidden / -fvisibility-inlines-hidden: PG 加载扩展时
    # 需要看到全局符号 (pg_vexdb_session 等)
    sed -i.bak 's|-fvisibility=hidden||g; s|-fvisibility-inlines-hidden||g' "$mk"
    sed -i.bak 's|src/distance/architecture_minimal.cpp|src/distance/architecture.cpp|' "$mk"
    # 注意: distances_simd_template.cpp / rabitq_template.cpp / template_half.cpp /
    # code_distance_template.cpp 是 *被 include 的模板*, 不能直接编译. 已被 general.cpp include
    # PG_VEXDB Makefile 漏了一大堆源文件, 补全:
    perl -i -pe 's|src/distance/general_dispatcher\.cpp$|src/distance/general_dispatcher.cpp \\\n    src/guc_config.cpp \\\n    src/bulkbuf_smgr.cpp \\\n    src/pg_yield.cpp \\\n    src/pq.cpp \\\n    src/quantizer_stubs.cpp \\\n    src/rabitq_distancer.cpp \\\n    src/shared_alloc_set.cpp \\\n    src/vecbuf_worker.cpp|' "$mk"
    rm -f "$mk.bak"

    # PG_VEXDB 当前只提供 general dispatcher 实现, 其他 ISA dispatcher 缺失.
    # 禁用所有 ARM (NEONV8/V9, SVEV9, SVE2V9, SMEV9, SME2V9) + x86 (SSE/AVX/AVX512) ISA
    # 让 DISTANCER_ISAS 只剩 GENERAL (但保留 enum 可见性给 distance.cpp 内的字面引用)
    local arch_h="/tmp/vexdb-pg-ctx/pg_vexdb_src/distance/architecture_macro.h"
    info "patch architecture_macro.h: 禁用所有 SIMD ISA (实现缺失)"
    # AVX512_EXTEND 是 derived macro (= DQ && BW && VL), 禁用 DQ/BW/VL 三个底层
    for isa in NEONV8 NEONV9 SVEV9 SVE2V9 SMEV9 SME2V9 SSE AVX AVX512_DQ AVX512_BW AVX512_VL; do
        perl -i -pe "s|^#define COMPILER_SUPPORT_${isa} .*|#define COMPILER_SUPPORT_${isa} 0|" "$arch_h"
    done
    info "patched Makefile 前 30 行:"
    head -30 "$mk"

    info "docker build (首次 ~30min, build PG 19devel + pg_vexdb)"
    # CI 通过 BUILDX_EXTRA_ARGS 传 cache 参数 (例: --cache-from type=gha --cache-to type=gha,mode=max)
    if [[ -n "${BUILDX_EXTRA_ARGS:-}" ]]; then
        # shellcheck disable=SC2086  # 故意让 BUILDX_EXTRA_ARGS 按空格 split
        docker buildx build $BUILDX_EXTRA_ARGS --load --tag "$IMAGE" /tmp/vexdb-pg-ctx
    else
        docker build --tag "$IMAGE" /tmp/vexdb-pg-ctx
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
        docker run -d --name "$CONTAINER" -p "${PG_PUBLISH_HOST:-127.0.0.1}:5433:5432" "$IMAGE"
    fi
    info "等待 PG ready..."
    for _ in $(seq 1 30); do
        if docker exec "$CONTAINER" psql -d test -c 'SELECT 1' >/dev/null 2>&1; then
            ok "PG ready on localhost:5433 db=test (容器内 5432)"
            return 0
        fi
        sleep 1
    done
    fail "等待超时"; docker logs --tail 50 "$CONTAINER"; exit 1
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

cmd_test() {
    require_docker
    local pattern="${1:-*}"
    [[ -d "$SPEC_DIR/sql" ]] || { fail "spec 未渲染. 先跑 python3 tests/spec/_lib/render.py --engine pg --out build/spec"; exit 1; }
    docker ps --format '{{.Names}}' | grep -qx "$CONTAINER" || cmd_up

    mkdir -p "$RESULTS_DIR"
    rm -f "$RESULTS_DIR"/*.diff "$RESULTS_DIR"/summary.txt

    local pass=0 fail=0 list=()
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
        docker exec -i "$CONTAINER" psql -d test -X -q -t -A -F '|' -P pager=off \
            > "$actual" 2>&1 < "$sql_file" || true
        # 过滤 psql 状态行 + NOTICE/WARNING/INFO 噪声
        sed -i.bak \
            -e '/^CREATE /d' -e '/^INSERT /d' -e '/^DROP /d' -e '/^DELETE /d' \
            -e '/^UPDATE /d' -e '/^TRUNCATE/d' -e '/^SET$/d' -e '/^BEGIN$/d' \
            -e '/^COMMIT$/d' -e '/^ROLLBACK$/d' -e '/^ALTER /d' -e '/^COPY /d' \
            -e '/^EXPLAIN$/d' -e '/^SELECT /d' -e '/^Time: /d' \
            -e '/^NOTICE:/d' -e '/^WARNING:/d' -e '/^INFO:/d' -e '/^DEBUG:/d' \
            -e '/^HINT:/d' -e '/^DETAIL:/d' -e '/^CONTEXT:/d' \
            -e '/^LINE [0-9]/d' -e '/^[ \t]*\^[ \t]*$/d' \
            "$actual" 2>/dev/null
        rm -f "${actual}.bak"
        # 删除尾部空白行
        awk 'NF || prev_nf { print } { prev_nf = NF }' "$actual" > "${actual}.tmp" && mv "${actual}.tmp" "$actual"

        if [[ -f "$expected" ]]; then
            if python3 "${ROOT_DIR}/tests/spec/_lib/docker/compare.py" "$expected" "$actual" 2>"$diff_file"; then
                pass=$((pass+1))
                rm -f "$diff_file"
                printf '  %s%s%s %s\n' "$GRN" "PASS" "$NC" "$name"
            else
                fail=$((fail+1))
                list+=("$name")
                printf '  %sFAIL%s %s (see %s)\n' "$RED" "$NC" "$name" "${diff_file#$ROOT_DIR/}"
            fi
        else
            fail=$((fail+1))
            list+=("$name")
            printf '  %sFAIL%s %s (no expected)\n' "$RED" "$NC" "$name"
        fi
    done

    {
        echo "=== PG spec test summary ==="
        echo "passed: $pass / $((pass+fail))"
        if (( fail > 0 )); then
            echo ""
            echo "failures:"
            printf '  %s\n' "${list[@]}"
        fi
    } | tee "$RESULTS_DIR/summary.txt"

    (( fail == 0 ))
}

case "${1:-}" in
    build)  cmd_build ;;
    up)     cmd_up ;;
    down)   cmd_down ;;
    shell)  cmd_shell ;;
    test)   shift; cmd_test "$@" ;;
    logs)   cmd_logs ;;
    status) cmd_status ;;
    *) sed -n '2,16p' "$0" ;;
esac
