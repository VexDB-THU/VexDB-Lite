#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 4 || ! -f "$1/duckdb/main/prepared_statement.hpp" || ! -f "$2" || ! -f "$3" ]]; then
  echo "usage: $0 <duckdb_source/src/include> <duckdb_library> <extension> <evidence_dir>" >&2
  exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$4"
run_dir="$(mktemp -d "$4/run.XXXXXX")"
library_dir="$(cd "$(dirname "$2")" && pwd)"
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$1" \
  "$root/vexdb_duckdb/test/bounded_array_order_test.cpp" "$2" \
  "-Wl,-rpath,$library_dir" -o "$run_dir/bounded-array-order-test" 2>&1 | tee "$run_dir/compile.log"
"$run_dir/bounded-array-order-test" "$3" "$run_dir/requests.duckdb" 2>&1 | tee "$run_dir/result.log"
echo "evidence: $run_dir"
