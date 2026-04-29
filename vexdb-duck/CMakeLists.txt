cmake_minimum_required(VERSION 2.8.12...3.29)

project(VexdbDuckExtension)

include_directories(
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE VEXDB_DUCK_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT VEXDB_DUCK_GIT_HASH)
    set(VEXDB_DUCK_GIT_HASH "unknown")
endif()
string(TIMESTAMP VEXDB_DUCK_BUILD_TIME "%Y-%m-%d %H:%M")
add_definitions(-DVEXDB_DUCK_GIT_HASH="${VEXDB_DUCK_GIT_HASH}")
add_definitions(-DVEXDB_DUCK_BUILD_TIME="${VEXDB_DUCK_BUILD_TIME}")
add_definitions(-DPG_VEXDB_TARGET_DUCK)

set(VEXDB_DUCK_EXTENSION_FILES
    vex_extension.cpp
    functions/distance_functions.cpp
    functions/vector_functions.cpp
    functions/ann_search_functions.cpp
    functions/index_info_function.cpp
    index/graph_index.cpp
    index/graph_index_disk.cpp
    index/vex_disk_block_store.cpp
    optimizer/vex_optimizer.cpp
    optimizer/vex_physical_create_index.cpp
    optimizer/vex_physical_index_scan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/architecture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/general_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/sse_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/avx_dispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/avx512_dispatcher.cpp
)

build_static_extension(vex ${VEXDB_DUCK_EXTENSION_FILES})
set(PARAMETERS "-warnings")
build_loadable_extension(vex ${PARAMETERS} ${VEXDB_DUCK_EXTENSION_FILES})

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/sse_dispatcher.cpp
        PROPERTIES COMPILE_FLAGS "-msse4.1 -msse4.2 -mfma -mf16c -Wno-ignored-attributes")
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/avx_dispatcher.cpp
        PROPERTIES COMPILE_FLAGS "-mavx2 -mfma -mf16c -Wno-ignored-attributes")
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/avx512_dispatcher.cpp
        PROPERTIES COMPILE_FLAGS "-mavx512f -mavx512dq -mavx512bw -mavx512vl -mf16c -Wno-ignored-attributes")
endif()
