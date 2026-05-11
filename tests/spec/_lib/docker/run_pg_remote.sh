#!/usr/bin/env bash
# PG spec runner — 连远程已部署的 PG 19 + pg_vexdb (无 Docker)
#
# 适用: 已经在远程机器上跑着 PG 19 + 装好 pg_vexdb 扩展, 仅需把 spec 推过去跑.
# 与 run_pg.sh (Docker 自包含) 互补.
#
# 环境变量:
#   PG_HOST       远程主机 (默认 116.204.26.51)
#   PG_PORT       端口    (默认 5532)
#   PG_USER       用户    (默认 wujianming)
#   PG_PASS       密码    (sshpass; 也供 PGPASSWORD)
#   PG_DB         库      (默认 spec_test)
#   PG_BIN        远程 psql 路径 (默认 /opt/wjm/pg19/bin/psql)
#   REMOTE_DIR    远程暂存目录 (默认 /tmp/vexdb_spec_run)
#
# 用法:
#   bash run_pg_remote.sh test [pattern]
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../../../.." && pwd)"
SPEC_DIR="${ROOT_DIR}/build/spec/pg"
RESULTS_DIR="${SPEC_DIR}/results"

PG_HOST="${PG_HOST:-116.204.26.51}"
PG_PORT="${PG_PORT:-5532}"
PG_USER="${PG_USER:-wujianming}"
PG_PASS="${PG_PASS:?PG_PASS env required}"
PG_DB="${PG_DB:-spec_test}"
PG_BIN="${PG_BIN:-/opt/wjm/pg19/bin/psql}"
REMOTE_DIR="${REMOTE_DIR:-/tmp/vexdb_spec_run}"

# 走 SSH 时同步用一个 master 连接, 避免每个 spec 重连
SSH_OPTS=(-o ControlMaster=auto -o ControlPath=/tmp/ssh-vexdb-%r@%h:%p -o ControlPersist=600 \
          -o StrictHostKeyChecking=accept-new -o LogLevel=ERROR)
SSH() { sshpass -p "$PG_PASS" ssh "${SSH_OPTS[@]}" "${PG_USER}@${PG_HOST}" "$@"; }
SCP() { sshpass -p "$PG_PASS" scp "${SSH_OPTS[@]}" "$@"; }

YEL=$'\033[1;33m'; GRN=$'\033[0;32m'; RED=$'\033[0;31m'; NC=$'\033[0m'
info() { printf '%s[pg-remote]%s %s\n' "$YEL" "$NC" "$*"; }
ok()   { printf '%s[pg-remote]%s %s\n' "$GRN" "$NC" "$*"; }
fail() { printf '%s[pg-remote]%s %s\n' "$RED" "$NC" "$*" >&2; }

cmd_test() {
    local pattern="${1:-*}"
    [[ -d "$SPEC_DIR/sql" ]] || { fail "spec 未渲染: $SPEC_DIR/sql 不存在"; exit 1; }

    info "目标: ${PG_USER}@${PG_HOST}:${PG_PORT}/${PG_DB}, pattern=${pattern}"

    info "推送 sql 到 ${REMOTE_DIR}"
    SSH "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR/sql' '$REMOTE_DIR/actual'"
    SCP -q "$SPEC_DIR/sql/"$pattern.sql "${PG_USER}@${PG_HOST}:${REMOTE_DIR}/sql/"

    # 远程 batch runner — 一次 ssh 跑完所有 spec
    info "远程执行 psql"
    SSH "export PGPASSWORD='$PG_PASS' PGCLIENTENCODING=UTF8; \
         cd '$REMOTE_DIR'; \
         for f in sql/*.sql; do \
           name=\$(basename \"\$f\" .sql); \
           ${PG_BIN} -h 127.0.0.1 -p ${PG_PORT} -U ${PG_USER} -d ${PG_DB} \
             -X -q -t -A -F '|' -P pager=off \
             -c \"DO \\\$\\\$ DECLARE r record; BEGIN FOR r IN SELECT tablename FROM pg_tables WHERE schemaname='public' LOOP EXECUTE 'DROP TABLE IF EXISTS public.' || quote_ident(r.tablename) || ' CASCADE'; END LOOP; END \\\$\\\$;\" \
             >/dev/null 2>&1 || true; \
           ${PG_BIN} -h 127.0.0.1 -p ${PG_PORT} -U ${PG_USER} -d ${PG_DB} \
             -X -q -t -A -F '|' -P pager=off > \"actual/\${name}.actual\" 2>&1 < \"\$f\" || true; \
         done; echo DONE"

    info "拉回 actual"
    mkdir -p "$RESULTS_DIR"
    rm -rf "$RESULTS_DIR/actual"
    SCP -q -r "${PG_USER}@${PG_HOST}:${REMOTE_DIR}/actual" "$RESULTS_DIR/"

    # 本地处理: 过滤噪声 + compare
    rm -f "$RESULTS_DIR"/*.diff "$RESULTS_DIR/summary.txt"
    local pass=0 nfail=0 list=()
    for sql_file in "$SPEC_DIR/sql/"$pattern.sql; do
        [[ -e "$sql_file" ]] || continue
        local name="$(basename "$sql_file" .sql)"
        local expected="$SPEC_DIR/expected/${name}.out"
        local actual="$RESULTS_DIR/actual/${name}.actual"
        local diff_file="$RESULTS_DIR/${name}.diff"
        [[ -f "$actual" ]] || { nfail=$((nfail+1)); list+=("$name (no actual)"); continue; }

        # 与 run_pg.sh 同步: 过滤命令完成消息 + 服务端日志级别 + 报错位置标记
        sed -i.bak -E \
            -e '/^(CREATE|INSERT|DROP|DELETE|UPDATE|TRUNCATE|ALTER|COPY|SELECT|Time:|EXPLAIN) /d' \
            -e '/^(SET|BEGIN|COMMIT|ROLLBACK|EXPLAIN)$/d' \
            -e '/^(NOTICE|WARNING|INFO|DEBUG|HINT|DETAIL|CONTEXT):/d' \
            -e '/^LINE [0-9]/d' -e '/^[ \t]*\^[ \t]*$/d' \
            "$actual" 2>/dev/null
        rm -f "${actual}.bak"
        awk 'NF || prev_nf { print } { prev_nf = NF }' "$actual" > "${actual}.tmp" && mv "${actual}.tmp" "$actual"

        if [[ -f "$expected" ]]; then
            if python3 "${ROOT_DIR}/tests/spec/_lib/docker/compare.py" "$expected" "$actual" 2>"$diff_file"; then
                pass=$((pass+1)); rm -f "$diff_file"
                printf '  %sPASS%s %s\n' "$GRN" "$NC" "$name"
            else
                nfail=$((nfail+1)); list+=("$name")
                printf '  %sFAIL%s %s\n' "$RED" "$NC" "$name"
            fi
        else
            nfail=$((nfail+1)); list+=("$name (no expected)")
        fi
    done

    {
        echo "=== PG spec test summary (remote: ${PG_HOST}:${PG_PORT}) ==="
        echo "passed: $pass / $((pass+nfail))"
        if (( nfail > 0 )); then echo ""; echo "failures:"; printf '  %s\n' "${list[@]}"; fi
    } | tee "$RESULTS_DIR/summary.txt"

    (( nfail == 0 ))
}

case "${1:-}" in
    test)   shift; cmd_test "$@" ;;
    "" | -h | --help) sed -n '2,18p' "$0" ;;
    *) fail "unknown command: ${1:-}"; exit 1 ;;
esac
