#!/usr/bin/env bash
set -euo pipefail

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
        [ -n "$candidate" ] || continue
        [ -x "$candidate" ] || continue
        if "$candidate" -c 'import sys; raise SystemExit(sys.version_info < (3, 8))' \
                >/dev/null 2>&1; then
            PYTHON_BIN="$candidate"
            break
        fi
    done
    [ -n "$PYTHON_BIN" ] || {
        echo "找不到 Python 3.8 或更高版本；可设置 VEXDB_LITE_PYTHON" >&2
        exit 2
    }
fi

exec "$PYTHON_BIN" "$@"
