#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
build_dir="${VEX_PQ_TEST_BUILD_DIR:-$root/build/product-tests/pq-training-executor}"
mkdir -p "$build_dir"
flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror -pthread)
if [[ -n "${SANITIZER:-}" ]]; then
    flags+=(-fsanitize="$SANITIZER" -fno-omit-frame-pointer)
fi
"${CXX:-clang++}" "${flags[@]}" -I"$root/vexdb_duckdb/include" \
    "$root/vexdb_duckdb/test/pq_training_executor_test.cpp" -o "$build_dir/pq_training_executor_test"
"$build_dir/pq_training_executor_test"
