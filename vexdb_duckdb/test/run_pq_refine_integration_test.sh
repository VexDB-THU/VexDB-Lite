#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 3 || $# -gt 5 ]]; then
    echo "usage: $0 <duckdb_include_dir> <duckdb_shared_library> <extension_path> [build_dir] [pq_only|pq_graph]" >&2
    exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
include_dir="$1"
library="$2"
extension="$3"
build_dir="${4:-$root/build/product-tests/pq-refine-integration}"
[[ -f "$include_dir/duckdb.hpp" && -f "$library" && -f "$extension" ]] || {
    echo "missing DuckDB headers, shared library, or extension; use the same DuckDB version for all three" >&2
    exit 2
}
mkdir -p "$build_dir"
library_dir="$(cd "$(dirname "$library")" && pwd)"
"${CXX:-c++}" -std=c++17 -O2 -pthread -I"$include_dir" \
    "$root/vexdb_duckdb/test/pq_refine_integration_test.cpp" "$library" \
    "-Wl,-rpath,$library_dir" -o "$build_dir/pq_refine_integration_test"
# Keep each run's database and log for diagnosis; never replace an old fixture.
run_dir="$(mktemp -d "$build_dir/run.XXXXXX")"
"$build_dir/pq_refine_integration_test" "$extension" "$run_dir/regression.duckdb" "${5:-pq_only}" 2>&1 | tee "$run_dir/integration.log"
echo "evidence: $run_dir"
