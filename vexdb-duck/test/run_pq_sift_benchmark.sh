#!/bin/zsh
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
DATASET="${1:-10k}"
DATA_DIR="${2:-$ROOT_DIR/vexdb-duck/test/benchmark/data}"
BENCH_SRC="$ROOT_DIR/vexdb-duck/test/benchmark/sift_benchmark.cpp"
BENCH_BIN="$ROOT_DIR/build/core/sift_benchmark"
CXX_BIN="${CXX:-c++}"

if [[ ! -f "$BENCH_SRC" ]]; then
  echo "missing benchmark source: $BENCH_SRC" >&2
  exit 2
fi
if [[ ! -d "$DATA_DIR" ]]; then
  echo "missing benchmark data dir: $DATA_DIR" >&2
  exit 2
fi
if [[ "$DATASET" != "10k" && "$DATASET" != "100k" ]]; then
  echo "dataset must be 10k or 100k" >&2
  exit 2
fi

mkdir -p "$(dirname "$BENCH_BIN")"

"$CXX_BIN" -std=c++17 -O2 \
  "$BENCH_SRC" \
  "$ROOT_DIR/libvex-core/src/product_quantizer.cpp" \
  "$ROOT_DIR/libvex-core/src/distance.cpp" \
  -I"$ROOT_DIR/libvex-core/include" \
  -I"$ROOT_DIR/vexdb-duck/include" \
  -o "$BENCH_BIN"

"$BENCH_BIN" "$DATA_DIR" "$DATASET"
