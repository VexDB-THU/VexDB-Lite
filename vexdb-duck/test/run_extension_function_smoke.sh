#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DUCKDB_BUILD_DIR="${1:-/tmp/vexdb-duck-build}"
EXTENSION_PATH="${2:-$DUCKDB_BUILD_DIR/extension/vex/vex.duckdb_extension}"
SMOKE_SRC="$ROOT_DIR/vexdb-duck/test/smoke_create_index.cpp"
SMOKE_BIN="${3:-$DUCKDB_BUILD_DIR/vexdb_duck_smoke}"
CXX_BIN="${CXX:-c++}"

if [[ ! -f "$SMOKE_SRC" ]]; then
  echo "missing smoke source: $SMOKE_SRC" >&2
  exit 2
fi
if [[ ! -f "$DUCKDB_BUILD_DIR/src/libduckdb_static.a" ]]; then
  echo "missing duckdb static lib: $DUCKDB_BUILD_DIR/src/libduckdb_static.a" >&2
  exit 2
fi
if [[ ! -f "$DUCKDB_BUILD_DIR/extension/libdummy_static_extension_loader.a" ]]; then
  echo "missing dummy loader: $DUCKDB_BUILD_DIR/extension/libdummy_static_extension_loader.a" >&2
  exit 2
fi
if [[ ! -f "$EXTENSION_PATH" ]]; then
  echo "missing extension: $EXTENSION_PATH" >&2
  exit 2
fi

"$CXX_BIN" -std=c++17 -O2 \
  "$SMOKE_SRC" \
  -I"/Users/sunji/Work/duckdb/src/include" \
  "$DUCKDB_BUILD_DIR/src/libduckdb_static.a" \
  "$DUCKDB_BUILD_DIR/extension/libdummy_static_extension_loader.a" \
  -o "$SMOKE_BIN"

"$SMOKE_BIN" "$EXTENSION_PATH"
