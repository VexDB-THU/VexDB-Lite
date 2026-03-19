################################################################################
# DuckDB minimal extension config (smallest possible build)
################################################################################
#
# Only loads the vex vector-search extension. Skips core_functions to minimize
# binary size. Basic SQL still works (CREATE/INSERT/SELECT/JOIN/etc.), but some
# scalar functions (e.g., string_split, list_transform) may be unavailable.
#
# Use --profile full or --profile compact if you need core_functions.

duckdb_extension_load(vex
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/vex
    INCLUDE_DIR ${CMAKE_CURRENT_LIST_DIR}/vex/include
)
