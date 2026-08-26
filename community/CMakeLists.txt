cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME vexdb_lite)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
project(${TARGET_NAME} CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# vexdb DuckDB 扩展唯一的外部依赖是 header-only boost/preprocessor，由 vcpkg 提供
# (见根 vcpkg.json)。不需要全量/patched boost——那是 PG 版 vexdb_pg 的事。
find_package(Boost REQUIRED)

# 源码不搬：直接引用仓库现有的 vexdb_duckdb/ 与 common/(本目录的上一级)。
# 与 ../vexdb_duckdb/CMakeLists.txt(build_duck.sh 用)保持同一份源文件清单。
set(VEX_DUCK   ${CMAKE_CURRENT_SOURCE_DIR}/../vexdb_duckdb)
set(VEX_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/../common)

include_directories(
    ${VEX_DUCK}/include
    ${VEX_COMMON}
    ${VEX_COMMON}/include
    ${Boost_INCLUDE_DIRS}
)

add_definitions(-DVEXDB_DUCK_GIT_HASH="community")
add_definitions(-DVEXDB_DUCK_BUILD_TIME="community")
add_definitions(-DPG_VEXDB_TARGET_DUCK)

set(EXTENSION_SOURCES
    ${VEX_DUCK}/vex_extension.cpp
    ${VEX_DUCK}/functions/distance_functions.cpp
    ${VEX_DUCK}/functions/vector_functions.cpp
    ${VEX_DUCK}/functions/index_info_function.cpp
    ${VEX_DUCK}/index/graph_index.cpp
    ${VEX_DUCK}/index/graph_index_disk.cpp
    ${VEX_DUCK}/index/vex_disk_block_store.cpp
    ${VEX_DUCK}/optimizer/vex_optimizer.cpp
    ${VEX_DUCK}/optimizer/vex_physical_create_index.cpp
    ${VEX_DUCK}/optimizer/vex_physical_index_scan.cpp
    ${VEX_COMMON}/quantizer/pq_alloc.cpp
    ${VEX_COMMON}/quantizer/annkmeans.cpp
    ${VEX_COMMON}/quantizer/product_quantizer.cpp
    ${VEX_COMMON}/distance/src/architecture.cpp
    ${VEX_COMMON}/distance/src/general_dispatcher.cpp
    ${VEX_COMMON}/distance/src/pq_dispatcher.cpp
    ${VEX_DUCK}/distance/general.cpp
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND EXTENSION_SOURCES
        ${VEX_COMMON}/distance/src/sse_dispatcher.cpp
        ${VEX_COMMON}/distance/src/avx_dispatcher.cpp
        ${VEX_COMMON}/distance/src/avx512_dispatcher.cpp
        ${VEX_DUCK}/distance/sse.cpp
        ${VEX_DUCK}/distance/avx.cpp
        ${VEX_DUCK}/distance/avx512.cpp
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    list(APPEND EXTENSION_SOURCES
        ${VEX_COMMON}/distance/src/neon_dispatcher.cpp
        ${VEX_DUCK}/distance/neon.cpp
    )
endif()

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} "-warnings" ${EXTENSION_SOURCES})

target_compile_features(${EXTENSION_NAME} PRIVATE cxx_std_17)
target_compile_features(${LOADABLE_EXTENSION_NAME} PRIVATE cxx_std_17)

# BoundIndex renamed CommitDrop(IndexLock &) to ResetStorage(IndexLock &) in
# DuckDB v1.5.5. Export one feature flag to the shared extension sources so the
# declarations can keep an exact override on both supported API shapes.
if(DEFINED DUCKDB_VERSION_NUMBER AND DUCKDB_VERSION_NUMBER VERSION_GREATER_EQUAL "1.5.5")
    target_compile_definitions(${EXTENSION_NAME} PRIVATE VEXDB_DUCKDB_HAS_RESET_STORAGE)
    target_compile_definitions(${LOADABLE_EXTENSION_NAME} PRIVATE VEXDB_DUCKDB_HAS_RESET_STORAGE)
endif()

set(SIMD_COMMON_FLAGS "-ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -floop-interchange -fno-exceptions -funroll-loops -g1")
set(SIMD_WARN_FLAGS "-Wno-ignored-attributes")
if(NOT CMAKE_BUILD_TYPE STREQUAL "DEBUG")
    set(SIMD_COMMON_FLAGS "${SIMD_COMMON_FLAGS} -O3 -ffp-contract=fast")
endif()

include(CheckCXXCompilerFlag)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set_source_files_properties(
        ${VEX_COMMON}/distance/src/sse_dispatcher.cpp
        ${VEX_DUCK}/distance/sse.cpp
        PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -msse3 -msse4.1 -msse4.2 -mfma -mf16c ${SIMD_WARN_FLAGS}")
    set_source_files_properties(
        ${VEX_COMMON}/distance/src/avx_dispatcher.cpp
        ${VEX_DUCK}/distance/avx.cpp
        PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -mavx -mavx2 -mfma -mf16c ${SIMD_WARN_FLAGS}")
    set_source_files_properties(
        ${VEX_COMMON}/distance/src/avx512_dispatcher.cpp
        ${VEX_DUCK}/distance/avx512.cpp
        PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx -mavx2 -mfma -mf16c ${SIMD_WARN_FLAGS}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "8.0")
        set(NEON_FLAGS "${SIMD_COMMON_FLAGS} -march=armv8.2-a+lse+fp16+dotprod+crc ${SIMD_WARN_FLAGS}")
    else()
        set(NEON_FLAGS "${SIMD_COMMON_FLAGS} -march=armv8.2-a+lse+fp16 ${SIMD_WARN_FLAGS}")
    endif()
    check_cxx_compiler_flag("-mtune=tsv110" HAS_MTUNE_TSV110_DUCK)
    if(HAS_MTUNE_TSV110_DUCK)
        set(NEON_FLAGS "${NEON_FLAGS} -mtune=tsv110")
    endif()
    set_source_files_properties(
        ${VEX_COMMON}/distance/src/neon_dispatcher.cpp
        ${VEX_DUCK}/distance/neon.cpp
        PROPERTIES COMPILE_FLAGS "${NEON_FLAGS}")
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
