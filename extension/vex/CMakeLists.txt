cmake_minimum_required(VERSION 2.8.12...3.29)

project(VexExtension)

include_directories(include)

# ============================================================
# VEX Mobile/Embedded Feature Switches
# ============================================================
option(VEX_ENABLE_HYBRID_INDEX "Enable HybridIndex (partitioned index)" ON)
option(VEX_ENABLE_OPTIMIZER "Enable query optimizer rewrite" ON)
option(VEX_MOBILE_MODE "Enable mobile-friendly defaults (mutex locks, lower thresholds)" OFF)
# Note: PQ (Product Quantization) is always compiled because it is deeply integrated
# into GraphIndexCore (member variables, serialization, search paths). The binary size
# overhead is minimal (~300 lines). Use CREATE INDEX ... WITH (pq_m=0) to disable at runtime.

# Detect GlobalIndexTypeRegistry availability (present in our fork, absent in upstream DuckDB)
set(INDEX_TYPE_SET_HEADER "${CMAKE_SOURCE_DIR}/src/include/duckdb/execution/index/index_type_set.hpp")
if(EXISTS "${INDEX_TYPE_SET_HEADER}")
    file(READ "${INDEX_TYPE_SET_HEADER}" INDEX_TYPE_SET_CONTENT)
    string(FIND "${INDEX_TYPE_SET_CONTENT}" "GlobalIndexTypeRegistry" HAS_GLOBAL_REGISTRY_POS)
    if(NOT HAS_GLOBAL_REGISTRY_POS EQUAL -1)
        add_definitions(-DVEX_HAS_GLOBAL_INDEX_REGISTRY)
        message(STATUS "VEX: GlobalIndexTypeRegistry detected")
    else()
        message(STATUS "VEX: Using DBConfig::GetIndexTypes() fallback")
    endif()
else()
    message(STATUS "VEX: index_type_set.hpp not found, using fallback")
endif()

# Conditionally include source files
set(VEX_EXTENSION_FILES
    vex_extension.cpp
    functions/distance_functions.cpp
    functions/vector_functions.cpp
    functions/ann_search_functions.cpp
    functions/index_info_function.cpp
    distance/distance.cpp
    quantizer/product_quantizer.cpp
    index/graph_index_core.cpp
    index/graph_index.cpp)

if(VEX_ENABLE_HYBRID_INDEX)
    list(APPEND VEX_EXTENSION_FILES index/hybrid_index.cpp)
    add_definitions(-DVEX_ENABLE_HYBRID_INDEX)
endif()

if(VEX_ENABLE_OPTIMIZER)
    list(APPEND VEX_EXTENSION_FILES optimizer/vex_optimizer.cpp)
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
