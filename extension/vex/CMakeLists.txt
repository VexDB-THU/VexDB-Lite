cmake_minimum_required(VERSION 2.8.12...3.29)

project(VexExtension)

include_directories(include)

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

set(VEX_EXTENSION_FILES
    vex_extension.cpp
    types/floatvector.cpp
    functions/distance_functions.cpp
    functions/vector_functions.cpp
    functions/ann_search_functions.cpp
    functions/index_info_function.cpp
    distance/distance.cpp
    quantizer/product_quantizer.cpp
    index/graph_index_core.cpp
    index/graph_index.cpp
    index/hybrid_index.cpp
    optimizer/vex_optimizer.cpp)

build_static_extension(vex ${VEX_EXTENSION_FILES})
set(PARAMETERS "-warnings")
build_loadable_extension(vex ${PARAMETERS} ${VEX_EXTENSION_FILES})

install(
  TARGETS vex_extension
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
