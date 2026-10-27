#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="${VEX_PQ_TEST_BUILD_DIR:-$root/build/product-tests/pq-center-policy}"
mkdir -p "$out"
flags=(-std=c++17 -O1 -g -Wall -Wextra -Werror)
if [[ -n "${SANITIZER:-}" ]]; then
    flags+=(-fsanitize="$SANITIZER" -fno-omit-frame-pointer)
fi
"${CXX:-clang++}" "${flags[@]}" -I"$root/common" \
    "$root/vexdb_duckdb/test/pq_center_policy_test.cpp" \
    "$root/common/quantizer/annkmeans.cpp" "$root/common/quantizer/pq_alloc.cpp" \
    -o "$out/pq_center_policy_test"
"$out/pq_center_policy_test"
