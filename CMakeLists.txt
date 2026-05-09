cmake_minimum_required(VERSION 3.14)
project(pg_vexdb CXX C)

set(PG_CONFIG_BIN "$ENV{PG_CONFIG}" CACHE STRING "Path to pg_config")
if(NOT PG_CONFIG_BIN)
    set(PG_CONFIG_BIN pg_config)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

execute_process(
    COMMAND ${PG_CONFIG_BIN} --includedir-server
    OUTPUT_VARIABLE PG_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND ${PG_CONFIG_BIN} --libdir
    OUTPUT_VARIABLE PG_LIB_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND ${PG_CONFIG_BIN} --pkglibdir
    OUTPUT_VARIABLE PG_PKGLIB_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
    COMMAND ${PG_CONFIG_BIN} --sharedir
    OUTPUT_VARIABLE PG_SHARE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

message(STATUS "PostgreSQL include: ${PG_INCLUDE_DIR}")
message(STATUS "PostgreSQL lib: ${PG_LIB_DIR}")
message(STATUS "PostgreSQL pkglib: ${PG_PKGLIB_DIR}")
message(STATUS "PostgreSQL sharedir: ${PG_SHARE_DIR}")

include_directories(
    ${PG_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/distance/core
    ${CMAKE_SOURCE_DIR}/distance/pg
    ${CMAKE_SOURCE_DIR}/distance/pq
    ${CMAKE_SOURCE_DIR}/src/distance/core
    ${CMAKE_SOURCE_DIR}/src/distance/pg
    ${CMAKE_SOURCE_DIR}/vec_types
    ${CMAKE_SOURCE_DIR}/rabitq
    ${CMAKE_SOURCE_DIR}/module
    /usr/include
)

add_compile_options(-std=c++17 -fPIC -fno-exceptions -Wno-write-strings -Wno-ignored-attributes)

if(CMAKE_BUILD_TYPE STREQUAL "DEBUG")
    add_compile_options(-O0)
endif()
add_compile_options(-g)
add_compile_definitions(PG_EXTENSION HAVE_CXX_TYPEOF_UNQUAL PG_VEXDB_TARGET_PG)

set(SIMD_COMMON_FLAGS "-ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -floop-interchange -fno-exceptions -g1")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(SSE_FLAGS "${SIMD_COMMON_FLAGS} -msse3 -msse4.1 -msse4.2 -mfma")
    set(AVX_FLAGS "${SIMD_COMMON_FLAGS} -mavx -mavx2 -mfma -mf16c")
    set(AVX512_FLAGS "${SIMD_COMMON_FLAGS} -mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx -mavx2 -mfma -mf16c")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(SSE_FLAGS "")
    set(AVX_FLAGS "")
    set(AVX512_FLAGS "")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "8.0")
        set(NEON_FLAGS "${SIMD_COMMON_FLAGS} -march=armv8.2-a+simd+fp16+dotprod")
    else()
        set(NEON_FLAGS "${SIMD_COMMON_FLAGS} -march=armv8.2-a+simd+fp16")
    endif()
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
    knl/knl_alloc.cpp
)

set(DISTANCE_SRCS
    src/distance/core/architecture.cpp
    src/distance/core/general_dispatcher.cpp
    src/distance/pg/distance.cpp
    src/distance/pg/general.cpp
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND DISTANCE_SRCS
        src/distance/core/sse_dispatcher.cpp
        src/distance/core/avx_dispatcher.cpp
        src/distance/core/avx512_dispatcher.cpp
        src/distance/pg/sse.cpp
        src/distance/pg/avx.cpp
        src/distance/pg/avx512.cpp
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    list(APPEND DISTANCE_SRCS
        src/distance/core/neon_dispatcher.cpp
        src/distance/pg/neon.cpp
    )
endif()

add_library(pg_vexdb SHARED ${GRAPH_INDEX_SRCS} ${DISTANCE_SRCS})
set_target_properties(pg_vexdb PROPERTIES OUTPUT_NAME "pg_vexdb" PREFIX "")
target_link_libraries(pg_vexdb stdc++)

set_source_files_properties(src/distance/pg/sse.cpp src/distance/core/sse_dispatcher.cpp PROPERTIES COMPILE_FLAGS "${SSE_FLAGS}")
set_source_files_properties(src/distance/pg/avx.cpp src/distance/core/avx_dispatcher.cpp PROPERTIES COMPILE_FLAGS "${AVX_FLAGS}")
set_source_files_properties(src/distance/pg/avx512.cpp src/distance/core/avx512_dispatcher.cpp PROPERTIES COMPILE_FLAGS "${AVX512_FLAGS}")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set_source_files_properties(src/distance/pg/neon.cpp src/distance/core/neon_dispatcher.cpp PROPERTIES COMPILE_FLAGS "${NEON_FLAGS}")
endif()

install(TARGETS pg_vexdb LIBRARY DESTINATION "${PG_PKGLIB_DIR}")
install(FILES sql/pg_vexdb--1.0.sql DESTINATION "${PG_SHARE_DIR}/extension")
install(FILES pg_vexdb.control DESTINATION "${PG_SHARE_DIR}/extension")
