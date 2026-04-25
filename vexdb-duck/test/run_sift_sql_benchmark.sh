#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DUCKDB_SOURCE_DIR="${1:-/tmp/duckdb-v1.4.4}"
DUCKDB_HOST_LIB_DIR="${2:-$ROOT_DIR/build/duckhost-v144/src}"
DATASET="${3:-both}"
DATA_DIR="${4:-$ROOT_DIR/vexdb-duck/test/benchmark/data}"
EXTENSION_PATH="${5:-$ROOT_DIR/build/standalone-v144/_duckdb/extension/vex/vex.duckdb_extension}"
BENCH_SRC="$ROOT_DIR/vexdb-duck/test/benchmark/vex_sift_sql_benchmark.cpp"
BENCH_BIN="$ROOT_DIR/build/duckhost-v144/vex_sift_sql_benchmark"
CXX_BIN="${CXX:-c++}"

if [[ "$(uname -s)" == "Darwin" ]]; then
  DUCKDB_HOST_LIB_NAME="libduckdb.dylib"
else
  DUCKDB_HOST_LIB_NAME="libduckdb.so"
fi

if [[ ! -f "$BENCH_SRC" ]]; then
  echo "missing benchmark source: $BENCH_SRC" >&2
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
if [[ ! -d "$DATA_DIR" ]]; then
  echo "missing benchmark data dir: $DATA_DIR" >&2
  exit 2
fi
if [[ ! -f "$EXTENSION_PATH" ]]; then
  echo "missing extension: $EXTENSION_PATH" >&2
  exit 2
fi

mkdir -p "$(dirname "$BENCH_BIN")"

"$CXX_BIN" -std=c++17 -O2 \
  "$BENCH_SRC" \
  -I"$DUCKDB_SOURCE_DIR/src/include" \
  -L"$DUCKDB_HOST_LIB_DIR" \
  -lduckdb \
  -Wl,-rpath,"$DUCKDB_HOST_LIB_DIR" \
  -o "$BENCH_BIN"

"$BENCH_BIN" "$DATA_DIR" "$DATASET" "$EXTENSION_PATH"
