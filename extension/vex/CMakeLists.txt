cmake_minimum_required(VERSION 2.8.12...3.29)

project(VexExtension)

include_directories(include)

set(VEX_EXTENSION_FILES
    vex_extension.cpp
    types/floatvector.cpp
    functions/distance_functions.cpp
    functions/vector_functions.cpp
    functions/ann_search_functions.cpp
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
