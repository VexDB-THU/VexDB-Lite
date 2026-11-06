#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 4 || ! -f "$1/duckdb/storage/object_cache.hpp" ]]; then
  echo "usage: $0 <duckdb_source/src/include> <duckdb_library> <extension> <new_build_dir>" >&2
  exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$4"
run_dir="$(mktemp -d "$4/run.XXXXXX")"
library_dir="$(cd "$(dirname "$2")" && pwd)"
# Use source headers consistently: do not mix the amalgamated SDK header with
# internal source headers in the same translation unit.
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$1" -I"$root/vexdb_duckdb/include" \
  "$root/vexdb_duckdb/test/filtered_hybrid_integration_test.cpp" "$2" \
  "-Wl,-rpath,$library_dir" -o "$run_dir/filtered-hybrid-test"
"$run_dir/filtered-hybrid-test" "$3" "$run_dir/business.duckdb" 2>&1 | tee "$run_dir/result.log"
echo "evidence: $run_dir"
