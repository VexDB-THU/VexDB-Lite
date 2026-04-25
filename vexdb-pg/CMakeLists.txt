cmake_minimum_required(VERSION 3.14)
project(pg_vexdb CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE "Enable libvex-core NodeStore bridge skeleton (no runtime switch yet)" ON)
set(PG_VEXDB_BOOST_ROOT "" CACHE PATH "Optional isolated Boost install root")

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

if(DEFINED ENV{PG_CONFIG} AND NOT "$ENV{PG_CONFIG}" STREQUAL "")
    set(PG_CONFIG_BIN "$ENV{PG_CONFIG}")
else()
    find_program(PG_CONFIG_BIN pg_config)
endif()

if(NOT PG_CONFIG_BIN)
    message(FATAL_ERROR "pg_config not found. Set PG_CONFIG env var or install PostgreSQL dev tools.")
endif()

execute_process(
    COMMAND ${PG_CONFIG_BIN} --includedir-server
    OUTPUT_VARIABLE PG_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PG_INCLUDE_DIR_RC
)
execute_process(
    COMMAND ${PG_CONFIG_BIN} --includedir
    OUTPUT_VARIABLE PG_INCLUDE_BASE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PG_INCLUDE_BASE_DIR_RC
)
execute_process(
    COMMAND ${PG_CONFIG_BIN} --libdir
    OUTPUT_VARIABLE PG_LIB_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PG_LIB_DIR_RC
)

if(NOT PG_INCLUDE_DIR_RC EQUAL 0 OR NOT PG_INCLUDE_BASE_DIR_RC EQUAL 0 OR NOT PG_LIB_DIR_RC EQUAL 0
   OR PG_INCLUDE_DIR STREQUAL "" OR PG_INCLUDE_BASE_DIR STREQUAL "" OR PG_LIB_DIR STREQUAL "")
    message(FATAL_ERROR "Failed to query PostgreSQL dirs via pg_config: ${PG_CONFIG_BIN}")
endif()

message(STATUS "Using pg_config: ${PG_CONFIG_BIN}")
message(STATUS "PostgreSQL include: ${PG_INCLUDE_DIR}")
message(STATUS "PostgreSQL include(base): ${PG_INCLUDE_BASE_DIR}")
message(STATUS "PostgreSQL lib: ${PG_LIB_DIR}")

if(PG_VEXDB_BOOST_ROOT)
    if(NOT EXISTS "${PG_VEXDB_BOOST_ROOT}/include/boost")
        message(FATAL_ERROR "PG_VEXDB_BOOST_ROOT does not contain include/boost: ${PG_VEXDB_BOOST_ROOT}")
    endif()
    include_directories(BEFORE ${PG_VEXDB_BOOST_ROOT}/include)
    if(EXISTS "${PG_VEXDB_BOOST_ROOT}/lib")
        link_directories(${PG_VEXDB_BOOST_ROOT}/lib)
    endif()
    message(STATUS "Using isolated Boost root: ${PG_VEXDB_BOOST_ROOT}")
endif()

include_directories(
    ${PG_INCLUDE_DIR}
    ${PG_INCLUDE_BASE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/../libvex-core/include
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/distance
    ${CMAKE_SOURCE_DIR}/src/distance
    ${CMAKE_SOURCE_DIR}/vec_types
    ${CMAKE_SOURCE_DIR}/rabitq
    ${CMAKE_SOURCE_DIR}/module
    /usr/include
)

if(PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE)
    add_compile_definitions(PG_VEXDB_ENABLE_LIBVEX_CORE_BRIDGE)
endif()

add_compile_options(-std=c++17 -O0 -g -fPIC -fno-exceptions -Wno-write-strings -Wno-ignored-attributes)
add_compile_definitions(PG_EXTENSION HAVE_CXX_TYPEOF_UNQUAL)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    add_compile_options(-march=native)
    set(SSE_FLAGS "-msse4.1 -msse4.2 -mfma")
    set(AVX_FLAGS "-mavx2 -mfma")
    set(AVX512_FLAGS "-mavx512f -mavx512dq -mavx512bw -mavx512vl")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    add_compile_options(-march=armv8-a+simd)
endif()

set(GRAPH_INDEX_SRCS
    src/graph_index_am.cpp
    src/session_compat.cpp
    src/guc_config.cpp
    src/graph_index.cpp
    src/graph_index_build.cpp
    src/graph_index_insert.cpp
    src/graph_index_inspect.cpp
    src/graph_index_scan.cpp
    src/graph_index_utils.cpp
    src/graph_index_vacuum.cpp
    src/graph_index_xlog.cpp
    src/floatvector.cpp
    src/halfvec.cpp
    src/ann_utils.cpp
    src/pg_yield.cpp
    src/vector_storage.cpp
    src/vector_smgr.cpp
    src/pq.cpp
    src/rabitq_distancer.cpp
    src/bulkbuf_smgr.cpp
    src/quantizer_stubs.cpp
    src/shared_alloc_set.cpp
    src/vecbuf_worker.cpp
    src/core_node_store_bridge.cpp
    knl/knl_alloc.cpp
    ../libvex-core/src/distance.cpp
    ../libvex-core/src/graph_algo.cpp
    ../libvex-core/src/node_store_memory.cpp
)

set(DISTANCE_SRCS
    src/distance/architecture.cpp
    src/distance/distance.cpp
    src/distance/general.cpp
    src/distance/general_dispatcher.cpp
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND DISTANCE_SRCS
        src/distance/sse.cpp
        src/distance/avx.cpp
        src/distance/avx512.cpp
        src/distance/sse_dispatcher.cpp
        src/distance/avx_dispatcher.cpp
        src/distance/avx512_dispatcher.cpp
    )
endif()

add_library(pg_vexdb SHARED ${GRAPH_INDEX_SRCS} ${DISTANCE_SRCS})
set_target_properties(pg_vexdb PROPERTIES OUTPUT_NAME "pg_vexdb" PREFIX "")
target_link_libraries(pg_vexdb stdc++)
if(APPLE)
    target_link_options(pg_vexdb PRIVATE "-Wl,-undefined,dynamic_lookup")
endif()

set_source_files_properties(
    ../libvex-core/src/distance.cpp
    ../libvex-core/src/graph_algo.cpp
    ../libvex-core/src/node_store_memory.cpp
    PROPERTIES COMPILE_FLAGS "-fexceptions")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set_source_files_properties(src/distance/sse.cpp src/distance/sse_dispatcher.cpp PROPERTIES COMPILE_FLAGS "${SSE_FLAGS}")
    set_source_files_properties(src/distance/avx.cpp src/distance/avx_dispatcher.cpp PROPERTIES COMPILE_FLAGS "${AVX_FLAGS}")
    set_source_files_properties(src/distance/avx512.cpp src/distance/avx512_dispatcher.cpp PROPERTIES COMPILE_FLAGS "${AVX512_FLAGS}")
endif()

install(TARGETS pg_vexdb LIBRARY DESTINATION "${PG_LIB_DIR}")
install(FILES sql/pg_vexdb--1.0.sql DESTINATION "${PG_LIB_DIR}/../share/extension")
install(FILES pg_vexdb.control DESTINATION "${PG_LIB_DIR}/../share/extension")
