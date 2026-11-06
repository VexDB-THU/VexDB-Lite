#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 4 ]]; then
    echo "usage: $0 <duckdb_include_dir> <duckdb_shared_library> <extension> <build_dir>" >&2
    exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$4"
library_dir="$(cd "$(dirname "$2")" && pwd)"
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$1" \
    "$root/vexdb_duckdb/test/filtered_search_integration_test.cpp" "$2" \
    "-Wl,-rpath,$library_dir" -o "$4/filtered_search_integration_test"
run_dir="$(mktemp -d "$4/run.XXXXXX")"
"$4/filtered_search_integration_test" "$3" "$run_dir/regression.duckdb" 2>&1 | tee "$run_dir/results.log"
echo "evidence: $run_dir"
