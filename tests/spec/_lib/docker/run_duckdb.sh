#!/usr/bin/env bash
# DuckDB spec runner (类比 run_pg.sh 但本地运行, 不用 Docker)
#
# 流程: render spec → 部署到 vex test 子目录 → cmake build unittest 让 catch2
# 注册新 .test → 用 group [spec_run] 跑.
#
# 用法:
#   bash run_duckdb.sh test            # 全集 (默认)
#   bash run_duckdb.sh test '*pq*'     # 模式过滤
#   bash run_duckdb.sh deploy          # 仅渲染+部署, 不跑
#   bash run_duckdb.sh clean           # 清掉 spec_run/ 和渲染产物
#
# 依赖外层 ../build_duck.sh 的 setup/build/build-unittest. 也可手动准备:
#   - DUCK_UNITTEST  指向 unittest 二进制 (env override)
#   - DUCK_VEX_TEST  指向 vexdb-duck/test/sql/vex (默认从仓库根推导)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../../../.." && pwd)"   # = duckdb/ 仓库根
SPEC_DIR="${ROOT_DIR}/build/spec/duckdb"
VEX_TEST_DIR="${DUCK_VEX_TEST:-${ROOT_DIR}/vexdb-duck/test/sql/vex}"
SPEC_RUN_DIR="${VEX_TEST_DIR}/spec_run"
WORKSPACE_ROOT="$(cd "${ROOT_DIR}/.." && pwd)"           # = vexdb_lite/ (含 build_duck.sh)
BUILD_DUCK="${WORKSPACE_ROOT}/build_duck.sh"

YEL=$'\033[1;33m'; GRN=$'\033[0;32m'; RED=$'\033[0;31m'; NC=$'\033[0m'
info() { printf '%s[duck-spec]%s %s\n' "$YEL" "$NC" "$*"; }
ok()   { printf '%s[duck-spec]%s %s\n' "$GRN" "$NC" "$*"; }
fail() { printf '%s[duck-spec]%s %s\n' "$RED" "$NC" "$*" >&2; exit 1; }

cmd_render() {
    info "render spec → ${SPEC_DIR#$ROOT_DIR/}"
    rm -rf "$SPEC_DIR"
    python3 "${ROOT_DIR}/tests/spec/_lib/render.py" --engine duckdb --out "${ROOT_DIR}/build/spec"
}

cmd_deploy() {
    cmd_render
    info "部署 .test → ${SPEC_RUN_DIR#$ROOT_DIR/}"
    rm -rf "$SPEC_RUN_DIR"
    mkdir -p "$SPEC_RUN_DIR"
    cp "$SPEC_DIR"/*.test "$SPEC_RUN_DIR/" 2>/dev/null || fail "no .test rendered to $SPEC_DIR"
    ok "deployed: $(ls "$SPEC_RUN_DIR" | wc -l | tr -d ' ') files"
}

resolve_unittest() {
    if [[ -n "${DUCK_UNITTEST:-}" && -x "$DUCK_UNITTEST" ]]; then
        echo "$DUCK_UNITTEST"; return
    fi
    # 默认用外层 build_duck.sh 的产物
    local default_bin="${WORKSPACE_ROOT}/build/duck/build/test/unittest"
    if [[ -x "$default_bin" ]]; then
        echo "$default_bin"; return
    fi
    # 尝试 build
    if [[ -x "$BUILD_DUCK" ]]; then
        info "unittest 二进制不存在, 调用 ${BUILD_DUCK##*/} build-unittest"
        bash "$BUILD_DUCK" build-unittest >&2
        if [[ -x "$default_bin" ]]; then echo "$default_bin"; return; fi
    fi
    fail "找不到 unittest 二进制. 请设 DUCK_UNITTEST=<path> 或在工作区根执行: bash build_duck.sh build-unittest"
}

cmd_test() {
    local pattern="${1:-}"
    cmd_deploy

    # 部署后必须重 build unittest, 让 catch2 注册新 .test 文件
    info "重 build unittest 让 catch2 注册新部署的 .test"
    if [[ -x "$BUILD_DUCK" ]]; then
        bash "$BUILD_DUCK" build-unittest >&2
    else
        info "no build_duck.sh — 假设 unittest 已是最新 (DUCK_UNITTEST=$DUCK_UNITTEST)"
    fi

    local UT
    UT="$(resolve_unittest)"
    info "unittest = $UT"

    local filter
    if [[ -n "$pattern" ]]; then
        filter="$pattern"
    else
        filter="[spec_run]"
    fi
    info "filter = $filter"
    "$UT" "$filter"
}

cmd_clean() {
    rm -rf "$SPEC_RUN_DIR"
    rm -rf "${ROOT_DIR}/build/spec/duckdb"
    ok "cleaned spec_run/ + build/spec/duckdb/"
}

case "${1:-}" in
    render)  cmd_render ;;
    deploy)  cmd_deploy ;;
    test)    shift; cmd_test "$@" ;;
    clean)   cmd_clean ;;
    *) sed -n '2,18p' "$0" ;;
esac
