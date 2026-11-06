#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 3 || ! -f "$1/duckdb.hpp" || ! -f "$2" ]]; then
  echo "usage: $0 <duckdb_source/src/include> <duckdb_library> <evidence_dir>" >&2
  exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$3"
run_dir="$(mktemp -d "$3/run.XXXXXX")"
library_dir="$(cd "$(dirname "$2")" && pwd)"
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$1" -I"$root/vexdb_duckdb/include" \
  "$root/vexdb_duckdb/test/fetch_rows_capacity_test.cpp" "$2" \
  "-Wl,-rpath,$library_dir" -o "$run_dir/fetch-capacity-test" 2>&1 | tee "$run_dir/compile.log"
"$run_dir/fetch-capacity-test" 2>&1 | tee "$run_dir/result.log"
echo "evidence: $run_dir"
