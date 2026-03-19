################################################################################
# DuckDB mobile extension config (iOS / Android)
################################################################################
#
# Minimal extension set for mobile platforms. Only loads core_functions and
# the vex vector-search extension. All non-essential extensions are skipped
# to reduce binary size and avoid platform-incompatible dependencies.
#
# Usage:
#   cmake -DLOCAL_EXTENSION_REPO=... \
#         -DDUCKDB_EXTENSION_CONFIGS=extension/extension_config_mobile.cmake ..

# Core scalar/aggregate functions required by DuckDB internals
duckdb_extension_load(core_functions)

# VEX: HNSW vector search extension (the whole point of this build)
duckdb_extension_load(vex
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/vex
    INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/vex/include
)

# Note: dynamic extension loading is disabled via ENABLE_EXTENSION_AUTOLOADING=OFF
# and ENABLE_EXTENSION_AUTOINSTALL=OFF in build.sh, not via DISABLE_EXTENSION_LOAD
# which strips the ExtensionHelper implementation and causes link errors.

################################################################################
# Explicitly skipped extensions and rationale:
#
# parquet   - Large dependency, mobile use cases read vectors from app layer
# httpfs    - Network I/O handled by the host app, not DuckDB
# json      - Not needed for vector search workloads
# icu       - Adds ~10 MB for Unicode collation; not needed for vector ops
# fts       - Full-text search not required alongside ANN search
# tpch      - Benchmark-only extension
# tpcds     - Benchmark-only extension
# jemalloc  - Not compatible with iOS; Android bionic allocator is sufficient
################################################################################
