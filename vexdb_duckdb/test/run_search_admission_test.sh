#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 4 || ! -f "$1/duckdb/storage/object_cache.hpp" ]]; then
    echo "usage: $0 <duckdb_source/src/include> <duckdb_shared_library> <extension> <build_dir>" >&2
    exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$4"
run_dir="$(mktemp -d "$4/run.XXXXXX")"
library_dir="$(cd "$(dirname "$2")" && pwd)"
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$root/vexdb_duckdb/include" \
    "$root/vexdb_duckdb/test/search_queue_test.cpp" -o "$run_dir/search_queue_test"
"$run_dir/search_queue_test" 2>&1 | tee "$run_dir/queue.log"
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$root/vexdb_duckdb/include" \
    "$root/vexdb_duckdb/test/search_queue_transition_test.cpp" -o "$run_dir/search_queue_transition_test"
"$run_dir/search_queue_transition_test" 2>&1 | tee "$run_dir/transitions.log"
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$1" -I"$root/vexdb_duckdb/include" \
    "$root/vexdb_duckdb/test/search_admission_integration_test.cpp" "$2" \
    "-Wl,-rpath,$library_dir" -o "$run_dir/search_admission_test"
"$run_dir/search_admission_test" "$3" "$run_dir/regression.duckdb" 2>&1 | tee "$run_dir/sql.log"
echo "evidence: $run_dir"
