# Skip DuckDB default in-tree extensions in standalone vex build.
set(SKIP_EXTENSIONS ${SKIP_EXTENSIONS} core_functions parquet jemalloc)

# Build vex as loadable extension from this repository.
duckdb_extension_load(vex
  DONT_LINK
  SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../extension/vex
  INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/../extension/vex/include
)
