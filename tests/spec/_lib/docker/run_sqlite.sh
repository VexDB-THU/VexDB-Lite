#!/bin/bash
# SQLite spec runner（本地直跑，不走 docker——python sqlite3 + loadable 扩展）。
#
#   bash run_sqlite.sh            # 渲染 + 全量跑
#   bash run_sqlite.sh <name>     # 只跑名字含 <name> 的 spec
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
PYTHON_BIN="${PYTHON_BIN:-python3}"

"$PYTHON_BIN" "$ROOT_DIR/tests/spec/_lib/render.py" --engine sqlite --out build/spec >/dev/null || exit 1
exec "$PYTHON_BIN" "$ROOT_DIR/tests/spec/_lib/docker/sqlite_spec_runner.py" "$ROOT_DIR" "$FILTER"
