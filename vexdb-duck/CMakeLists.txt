cmake_minimum_required(VERSION 2.8.12...3.29)

project(VexExtension)

include_directories(include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../libvex-core/include)

# ============================================================
# VEX Mobile/Embedded Feature Switches
# ============================================================
option(VEX_ENABLE_OPTIMIZER "Enable query optimizer rewrite" ON)
option(VEX_MOBILE_MODE "Enable mobile-friendly defaults (mutex locks, lower thresholds)" OFF)
# Note: PQ (Product Quantization) is always compiled because it is deeply integrated
# into GraphIndexCore (member variables, serialization, search paths). The binary size
# overhead is minimal (~300 lines). Use CREATE INDEX ... WITH (pq_m=0) to disable at runtime.

# Get git commit hash at configure time
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE VEX_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT VEX_GIT_HASH)
    set(VEX_GIT_HASH "unknown")
endif()
# Get build timestamp
string(TIMESTAMP VEX_BUILD_TIME "%Y-%m-%d %H:%M")
add_definitions(-DVEX_GIT_HASH="${VEX_GIT_HASH}")
add_definitions(-DVEX_BUILD_TIME="${VEX_BUILD_TIME}")
message(STATUS "VEX: version ${VEX_GIT_HASH} built at ${VEX_BUILD_TIME}")

# Conditionally include source files
set(VEX_EXTENSION_FILES
    vex_extension.cpp
    functions/distance_functions.cpp
    functions/vector_functions.cpp
    functions/ann_search_functions.cpp
    functions/index_info_function.cpp
    distance/distance.cpp
    ../libvex-core/src/distance.cpp
    ../libvex-core/src/graph_algo.cpp
    ../libvex-core/src/graph_maintenance.cpp
    ../libvex-core/src/node_store_memory.cpp
    ../libvex-core/src/product_quantizer.cpp
    ../libvex-core/src/quant_distancer.cpp
    index/graph_index_core.cpp
    index/graph_index.cpp
    index/vex_core_node_store_bridge.cpp
    index/vex_core_bridge_graph_helpers.cpp
    index/vex_core_bridge_runtime.cpp
    optimizer/vex_physical_create_index.cpp)

if(VEX_ENABLE_OPTIMIZER)
    list(APPEND VEX_EXTENSION_FILES
        optimizer/vex_optimizer.cpp
        optimizer/vex_physical_index_scan.cpp)
    add_definitions(-DVEX_ENABLE_OPTIMIZER)
endif()

if(VEX_MOBILE_MODE)
    add_definitions(-DVEX_MOBILE_MODE)
endif()

build_static_extension(vex ${VEX_EXTENSION_FILES})
set(PARAMETERS "-warnings")
build_loadable_extension(vex ${PARAMETERS} ${VEX_EXTENSION_FILES})

install(
  TARGETS vex_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

if(BUILD_MAIN_DUCKDB_LIBRARY)
    add_executable(vex_extension_e2e_test
        test/vex_extension_e2e.cpp)
    target_link_libraries(vex_extension_e2e_test PRIVATE duckdb_static)
    target_compile_features(vex_extension_e2e_test PRIVATE cxx_std_17)
    target_compile_definitions(vex_extension_e2e_test PRIVATE
        VEX_E2E_EXTENSION_PATH="${CMAKE_CURRENT_BINARY_DIR}/vex.duckdb_extension")
    add_dependencies(vex_extension_e2e_test vex_loadable_extension)
    add_test(NAME vex_extension_e2e_test COMMAND vex_extension_e2e_test)
else()
    message(STATUS "VEX: skipping vex_extension_e2e_test because BUILD_MAIN_DUCKDB_LIBRARY=OFF")
endif()
