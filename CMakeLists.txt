cmake_minimum_required(VERSION 3.16)

project(VexDBLiteStandalone)

set(DUCKDB_SOURCE_DIR "" CACHE PATH "Absolute path to DuckDB source tree")
if(NOT DUCKDB_SOURCE_DIR)
  message(FATAL_ERROR
    "DUCKDB_SOURCE_DIR is required. Example: cmake -S . -B build -DDUCKDB_SOURCE_DIR=/path/to/duckdb")
endif()

get_filename_component(VEXDB_ROOT_REAL "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
get_filename_component(DUCKDB_ROOT_REAL "${DUCKDB_SOURCE_DIR}" REALPATH)

if(VEXDB_ROOT_REAL STREQUAL DUCKDB_ROOT_REAL)
  message(FATAL_ERROR
    "DUCKDB_SOURCE_DIR cannot point to this repository itself in standalone mode. "
    "Use a separate DuckDB checkout.")
endif()

if(NOT EXISTS "${DUCKDB_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Invalid DUCKDB_SOURCE_DIR: '${DUCKDB_SOURCE_DIR}' (CMakeLists.txt not found)")
endif()

# Build extension selection via custom config file.
set(BUILD_EXTENSIONS "" CACHE STRING "DuckDB extensions to build" FORCE)
set(DUCKDB_EXTENSION_CONFIGS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vex_standalone_extension_config.cmake"
    CACHE STRING "Extra DuckDB extension config files" FORCE)

# Keep the build focused on extension artifacts.
set(BUILD_EXTENSIONS_ONLY ON CACHE BOOL "Build only extensions" FORCE)
set(BUILD_MAIN_DUCKDB_LIBRARY OFF CACHE BOOL "Skip building DuckDB core binaries" FORCE)
set(BUILD_UNITTESTS OFF CACHE BOOL "Disable DuckDB unittests" FORCE)
set(BUILD_BENCHMARKS OFF CACHE BOOL "Disable DuckDB benchmarks" FORCE)
set(BUILD_SHELL OFF CACHE BOOL "Disable DuckDB shell" FORCE)
set(EXTENSION_STATIC_BUILD OFF CACHE BOOL "Do not link extension against duckdb_static" FORCE)

add_subdirectory("${DUCKDB_SOURCE_DIR}" _duckdb)
