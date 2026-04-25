#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DUCKDB_SOURCE_DIR="${1:-/tmp/duckdb-v1.4.4}"
DUCKDB_HOST_LIB_DIR="${2:-$ROOT_DIR/build/duckhost-v144/src}"
EXTENSION_PATH="${3:-$ROOT_DIR/build/standalone-v144/_duckdb/extension/vex/vex.duckdb_extension}"
HARNESS_SRC="$ROOT_DIR/vexdb-duck/test/vex_extension_function_smoke.cpp"
HARNESS_BIN="$ROOT_DIR/build/duckhost-v144/vex_extension_function_smoke"

if [[ "$(uname -s)" == "Darwin" ]]; then
  DUCKDB_HOST_LIB_NAME="libduckdb.dylib"
else
  DUCKDB_HOST_LIB_NAME="libduckdb.so"
fi

if [[ ! -f "$HARNESS_SRC" ]]; then
  echo "missing harness source: $HARNESS_SRC" >&2
  exit 2
fi
if [[ ! -d "$DUCKDB_SOURCE_DIR/src/include" ]]; then
  echo "missing duckdb source include dir: $DUCKDB_SOURCE_DIR/src/include" >&2
  exit 2
fi
if [[ ! -f "$DUCKDB_HOST_LIB_DIR/$DUCKDB_HOST_LIB_NAME" ]]; then
  echo "missing duckdb host library: $DUCKDB_HOST_LIB_DIR/$DUCKDB_HOST_LIB_NAME" >&2
  exit 2
fi
if [[ ! -f "$EXTENSION_PATH" ]]; then
  echo "missing extension: $EXTENSION_PATH" >&2
  exit 2
fi

mkdir -p "$(dirname "$HARNESS_BIN")"

clang++ -std=c++17 -O0 -g \
  "$HARNESS_SRC" \
  -I"$DUCKDB_SOURCE_DIR/src/include" \
  -L"$DUCKDB_HOST_LIB_DIR" \
  -lduckdb \
  -Wl,-rpath,"$DUCKDB_HOST_LIB_DIR" \
  -o "$HARNESS_BIN"

tests=(
  load_and_basic_query
  create_index_and_ann
  explain_ann_plan
  insert_delete_update_regression
)

fail_count=0

for test_name in "${tests[@]}"; do
  echo "==> $test_name"
  if ! ASAN_OPTIONS="${ASAN_OPTIONS:-detect_container_overflow=0}" \
      "$HARNESS_BIN" "$test_name" "$EXTENSION_PATH"; then
    fail_count=$((fail_count + 1))
  fi
done

if [[ "$fail_count" -ne 0 ]]; then
  echo "FAILED $fail_count test(s)" >&2
  exit 1
fi
