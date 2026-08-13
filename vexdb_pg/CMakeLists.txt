cmake_minimum_required(VERSION 3.14)
project(vexdb_lite CXX C)

if(DEFINED ENV{PG_CONFIG} AND NOT "$ENV{PG_CONFIG}" STREQUAL "")
    set(PG_CONFIG_BIN "$ENV{PG_CONFIG}")
else()
    find_program(PG_CONFIG_BIN NAMES pg_config)
    if(NOT PG_CONFIG_BIN)
        message(FATAL_ERROR
            "PG_CONFIG is not set and pg_config was not found in PATH.\n"
            "Set PG_CONFIG explicitly, for example:\n"
            "  export PG_CONFIG=/path/to/pg_config")
    endif()
endif()

set(PG_CONFIG_BIN "${PG_CONFIG_BIN}" CACHE STRING "Path to pg_config" FORCE)
if(NOT EXISTS "${PG_CONFIG_BIN}")
    message(FATAL_ERROR "PG_CONFIG points to a missing file: ${PG_CONFIG_BIN}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(VEXDB_USE_CCACHE "Use ccache when available" ON)
if(VEXDB_USE_CCACHE)
    find_program(CCACHE_PROGRAM ccache)
    if(CCACHE_PROGRAM)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
        message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
    endif()
else()
    set(CMAKE_CXX_COMPILER_LAUNCHER "")
endif()

execute_process(
    COMMAND ${PG_CONFIG_BIN} --includedir-server
    OUTPUT_VARIABLE PG_INCLUDE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PG_CONFIG_INCLUDE_RC
)
if(NOT PG_CONFIG_INCLUDE_RC EQUAL 0 OR "${PG_INCLUDE_DIR}" STREQUAL "")
    message(FATAL_ERROR "Failed to query PostgreSQL include dir via ${PG_CONFIG_BIN} --includedir-server")
endif()
execute_process(
    COMMAND ${PG_CONFIG_BIN} --libdir
    OUTPUT_VARIABLE PG_LIB_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PG_CONFIG_LIB_RC
)
if(NOT PG_CONFIG_LIB_RC EQUAL 0 OR "${PG_LIB_DIR}" STREQUAL "")
    message(FATAL_ERROR "Failed to query PostgreSQL lib dir via ${PG_CONFIG_BIN} --libdir")
endif()
execute_process(
    COMMAND ${PG_CONFIG_BIN} --pkglibdir
    OUTPUT_VARIABLE PG_PKGLIB_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PG_CONFIG_PKGLIB_RC
)
if(NOT PG_CONFIG_PKGLIB_RC EQUAL 0 OR "${PG_PKGLIB_DIR}" STREQUAL "")
    message(FATAL_ERROR "Failed to query PostgreSQL pkglib dir via ${PG_CONFIG_BIN} --pkglibdir")
endif()
execute_process(
    COMMAND ${PG_CONFIG_BIN} --sharedir
    OUTPUT_VARIABLE PG_SHARE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE PG_CONFIG_SHARE_RC
)
if(NOT PG_CONFIG_SHARE_RC EQUAL 0 OR "${PG_SHARE_DIR}" STREQUAL "")
    message(FATAL_ERROR "Failed to query PostgreSQL sharedir via ${PG_CONFIG_BIN} --sharedir")
endif()

message(STATUS "PostgreSQL include: ${PG_INCLUDE_DIR}")
message(STATUS "PostgreSQL lib: ${PG_LIB_DIR}")
message(STATUS "PostgreSQL pkglib: ${PG_PKGLIB_DIR}")
message(STATUS "PostgreSQL sharedir: ${PG_SHARE_DIR}")

# REPO_ROOT: 兼容两种调用方式
#   - `cd vexdb_pg && cmake .`           (PG 单独 build)
#   - `add_subdirectory(vexdb_pg)`        (顶层 CMakeLists dispatch)
# 用 CMAKE_CURRENT_SOURCE_DIR/.. 统一指向 repo root, 这样 common/ 等公共目录可定位.
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

include_directories(
    ${REPO_ROOT}/thirdparties   # patched Boost, takes priority over system
    ${PG_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${REPO_ROOT}/common/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${REPO_ROOT}/common
    ${REPO_ROOT}/common/distance/core
    ${CMAKE_CURRENT_SOURCE_DIR}/distance
    ${REPO_ROOT}/common/distance/pq
    ${REPO_ROOT}/common/distance/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/distance
    ${REPO_ROOT}/common/rabitq
    ${REPO_ROOT}/common/module
    /usr/include
)

# vendored boost(thirdparties)是 patch 覆盖层,trim 掉了 preprocessor 等基础库,靠完整
# boost 兜底。系统有 boost 的机器靠上面的 /usr/include 即可;系统无 boost 的 build 机
# (如 Kylin)需显式传完整 boost:cmake -DBOOST_FALLBACK_INC=/path/to/boost_root。
# 放在 thirdparties 之后,保证 vendored(getpid patch)优先、仅缺失部分回退到完整 boost。
if(BOOST_FALLBACK_INC)
    include_directories(${BOOST_FALLBACK_INC})
    message(STATUS "Boost fallback (vendored 缺失部分): ${BOOST_FALLBACK_INC}")
endif()

add_compile_options(-std=c++17 -fPIC -fno-exceptions -fno-rtti -Wno-write-strings -Wno-ignored-attributes -pthread)

string(TOLOWER ${CMAKE_BUILD_TYPE} BUILD_TYPE_LOWER)
if(BUILD_TYPE_LOWER STREQUAL "debug")
    add_compile_options(-O0)
    set(SIMD_G_FLAG "-g3")
else()
    set(SIMD_G_FLAG "-g1")
endif()
add_compile_options(-g)
add_compile_definitions(PG_EXTENSION HAVE_CXX_TYPEOF_UNQUAL PG_VEXDB_TARGET_PG)

set(SIMD_COMMON_FLAGS "-ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -floop-interchange -fno-exceptions -funroll-loops ${SIMD_G_FLAG}")

if(NOT BUILD_TYPE_LOWER STREQUAL "debug")
    set(SIMD_COMMON_FLAGS "${SIMD_COMMON_FLAGS} -O3 -ffp-contract=fast")
endif()

include(CheckCXXCompilerFlag)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(SSE_FLAGS "${SIMD_COMMON_FLAGS} -msse3 -msse4.1 -msse4.2 -mfma")
    set(AVX_FLAGS "${SIMD_COMMON_FLAGS} -mavx -mavx2 -mfma -mf16c")
    set(AVX512_FLAGS "${SIMD_COMMON_FLAGS} -mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx -mavx2 -mfma -mf16c")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(SSE_FLAGS "")
    set(AVX_FLAGS "")
    set(AVX512_FLAGS "")
    # Kunpeng/TaiShan v110 baseline. ARMv8.0 老核 SIGILL on LSE — release-notes minimum ARMv8.2.
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "8.0")
        set(NEON_FLAGS "${SIMD_COMMON_FLAGS} -march=armv8.2-a+lse+fp16+dotprod+crc")
    else()
        set(NEON_FLAGS "${SIMD_COMMON_FLAGS} -march=armv8.2-a+lse+fp16")
    endif()
    # macOS Apple Clang 不识别 -mtune=tsv110 — probe before adding.
    check_cxx_compiler_flag("-mtune=tsv110" HAS_MTUNE_TSV110)
    if(HAS_MTUNE_TSV110)
        set(NEON_FLAGS "${NEON_FLAGS} -mtune=tsv110")
    endif()
endif()

set(GRAPH_INDEX_SRCS
    src/session_compat.cpp
    src/guc_config.cpp
    src/graph_index/graph_index_am.cpp
    src/graph_index/graph_index.cpp
    src/graph_index/graph_index_build.cpp
    src/graph_index/graph_index_insert.cpp
    src/graph_index/graph_index_inspect.cpp
    src/vex_index_info.cpp
    src/graph_index/graph_index_scan.cpp
    src/graph_index/graph_index_utils.cpp
    src/graph_index/graph_index_vacuum.cpp
    src/floatvector.cpp
    src/ann_utils.cpp
    src/pg_yield.cpp
    src/vector_page_storage.cpp
    src/pq.cpp
    src/rabitq_distancer.cpp
    src/quantizer_stubs.cpp
    src/pg_init.cpp
)

set(DISTANCE_SRCS
    ${REPO_ROOT}/common/distance/src/architecture.cpp
    ${REPO_ROOT}/common/distance/src/general_dispatcher.cpp
    ${REPO_ROOT}/common/distance/src/pq_dispatcher.cpp
    ${REPO_ROOT}/common/distance/src/rabitq_dispatcher.cpp
    ${REPO_ROOT}/common/distance/src/vector_alloc.cpp
    src/distance/distance.cpp
    src/distance/general.cpp
)

set(QUANTIZER_SRCS
    ${REPO_ROOT}/common/quantizer/annkmeans.cpp
    ${REPO_ROOT}/common/quantizer/pq_alloc.cpp
    ${REPO_ROOT}/common/quantizer/product_quantizer.cpp
    ${REPO_ROOT}/common/rabitq/rabitq.cpp
    ${REPO_ROOT}/common/rabitq/estimator.cpp
    ${REPO_ROOT}/common/rabitq/rotator.cpp
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND DISTANCE_SRCS
        ${REPO_ROOT}/common/distance/src/sse_dispatcher.cpp
        ${REPO_ROOT}/common/distance/src/avx_dispatcher.cpp
        ${REPO_ROOT}/common/distance/src/avx512_dispatcher.cpp
        src/distance/sse.cpp
        src/distance/avx.cpp
        src/distance/avx512.cpp
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    list(APPEND DISTANCE_SRCS
        ${REPO_ROOT}/common/distance/src/neon_dispatcher.cpp
        src/distance/neon.cpp
    )
endif()

find_package(Threads REQUIRED)

add_library(vexdb_lite SHARED ${GRAPH_INDEX_SRCS} ${DISTANCE_SRCS} ${QUANTIZER_SRCS})
set_target_properties(vexdb_lite PROPERTIES PREFIX "")
target_link_libraries(vexdb_lite PRIVATE stdc++ Threads::Threads)

set_source_files_properties(src/distance/sse.cpp ${REPO_ROOT}/common/distance/src/sse_dispatcher.cpp PROPERTIES COMPILE_FLAGS "${SSE_FLAGS}")
set_source_files_properties(src/distance/avx.cpp ${REPO_ROOT}/common/distance/src/avx_dispatcher.cpp PROPERTIES COMPILE_FLAGS "${AVX_FLAGS}")
set_source_files_properties(src/distance/avx512.cpp ${REPO_ROOT}/common/distance/src/avx512_dispatcher.cpp PROPERTIES COMPILE_FLAGS "${AVX512_FLAGS}")
set_source_files_properties(${REPO_ROOT}/common/quantizer/annkmeans.cpp PROPERTIES COMPILE_FLAGS "-include pg_compat.h")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set_source_files_properties(src/distance/neon.cpp ${REPO_ROOT}/common/distance/src/neon_dispatcher.cpp PROPERTIES COMPILE_FLAGS "${NEON_FLAGS}")
endif()

install(TARGETS vexdb_lite LIBRARY DESTINATION "${PG_PKGLIB_DIR}")
install(FILES sql/vexdb_lite--1.0.sql DESTINATION "${PG_SHARE_DIR}/extension")
install(FILES sql/vexdb_lite--1.0.sql
        DESTINATION "${PG_SHARE_DIR}/extension"
        RENAME vexdb_lite--1.1.sql)
install(FILES sql/vexdb_lite--1.0--1.1.sql DESTINATION "${PG_SHARE_DIR}/extension")
install(FILES vexdb_lite.control DESTINATION "${PG_SHARE_DIR}/extension")
