#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${1:-$root/build/graph-connectivity-test}"
mkdir -p "$out"
flags=(-std=c++17 -O2 -UNDEBUG)
if [[ -n "${SANITIZER:-}" ]];then flags+=(-fsanitize="$SANITIZER" -fno-omit-frame-pointer);fi
"${CXX:-c++}" "${flags[@]}" -I"$root/vexdb_duckdb/include" "$root/vexdb_duckdb/test/graph_connectivity_test.cpp" -o "$out/graph_connectivity_test"
"$out/graph_connectivity_test"
