#!/usr/bin/env bash
set -euo pipefail
if [[ $# -ne 3 ]];then
    echo "usage: $0 <duckdb_include> <matching_libduckdb> <new_build_dir>" >&2
    exit 2
fi
root="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$3"
out="$(mktemp -d "$3/run.XXXXXX")"
libdir="$(cd "$(dirname "$2")" && pwd)"
boost="${BOOST_ROOT:-/opt/boost}"
if [[ -d "$boost/include/boost" ]];then boost="$boost/include";fi
"${CXX:-c++}" -std=c++17 -O2 -pthread -fvisibility=hidden -fvisibility-inlines-hidden \
    -DPG_VEXDB_TARGET_DUCK=1 -I"$root/vexdb_duckdb/include" -I"$root/common" \
    -I"$root/common/include" -I"$1" -I"$boost" \
    "$root/tests/eval/graph_search/entry_lock_contract.cpp" "$2" \
    "-Wl,-rpath,$libdir" -o "$out/entry_lock_contract"
"$out/entry_lock_contract"
