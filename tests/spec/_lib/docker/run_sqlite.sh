#!/bin/bash
# SQLite spec runner（本地直跑，不走 docker——python sqlite3 + loadable 扩展）。
#
#   bash run_sqlite.sh            # 渲染 + 全量跑
#   bash run_sqlite.sh <name>     # 只跑名字含 <name> 的 spec
#   VEXDB_SQLITE_EXTENSION=/abs/vexdb_lite.dylib bash run_sqlite.sh
#                               # 显式绑定本次构建，避免误测旧产物
#
# 协议：
#   - 渲染产物 build/spec/sqlite/sql/<name>.sql + expected/<name>.out
#   - 每个 spec 独立临时 db 文件；`-- @restart` 关闭连接重开（持久化验证）
#   - `-- @expect-error` 的下一条语句必须失败（宽松匹配），其输出不入 actual
#   - query 输出 '|' 分隔、NULL→空串，与 expected 走 compare.py 容差对比
#   - 用 python sqlite3 驱动（同一连接逐条执行 → BEGIN/ROLLBACK 等事务语义正确）
set -u

ROOT_DIR="$(cd "$(dirname "$0")/../../../.." && pwd)"
FILTER="${1:-}"
PYTHON_BIN="${VEXDB_LITE_PYTHON:-}"

python_is_usable() {
    "$1" -c 'import sqlite3,sys; connection=sqlite3.connect(":memory:"); raise SystemExit(sys.version_info < (3, 8) or not hasattr(connection, "enable_load_extension"))' \
        >/dev/null 2>&1
}

if [[ -n "$PYTHON_BIN" ]]; then
    [[ -x "$PYTHON_BIN" ]] || {
        echo "VEXDB_LITE_PYTHON 不可执行：$PYTHON_BIN" >&2
        exit 2
    }
    python_is_usable "$PYTHON_BIN" || {
        echo "VEXDB_LITE_PYTHON 需要 Python 3.8+ 和 SQLite 扩展加载能力：$PYTHON_BIN" >&2
        exit 2
    }
else
    for candidate in \
        "$(command -v python3 2>/dev/null || true)" \
        /opt/homebrew/bin/python3 \
        /opt/anaconda3/bin/python3 \
        /usr/bin/python3; do
        [[ -n "$candidate" && -x "$candidate" ]] || continue
        if python_is_usable "$candidate"; then
            PYTHON_BIN="$candidate"
            break
        fi
    done
    [[ -n "$PYTHON_BIN" ]] || {
        echo "SQLite spec 需要 Python 3.8+ 和 SQLite 扩展加载能力" >&2
        exit 2
    }
fi

"$PYTHON_BIN" "$ROOT_DIR/tests/spec/_lib/render.py" --engine sqlite --out build/spec >/dev/null || exit 1
exec "$PYTHON_BIN" "$ROOT_DIR/tests/spec/_lib/docker/sqlite_spec_runner.py" \
    "$ROOT_DIR" "$FILTER" "${VEXDB_SQLITE_EXTENSION:-}"
