cmake_minimum_required(VERSION 2.8.12...3.29)

project(VexdbDuckExtension)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED)

include_directories(
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${Boost_INCLUDE_DIRS}
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
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    list(APPEND VEXDB_DUCK_EXTENSION_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/sse_dispatcher.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/avx_dispatcher.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/avx512_dispatcher.cpp
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    list(APPEND VEXDB_DUCK_EXTENSION_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/neon_dispatcher.cpp
    )
endif()

build_static_extension(vex ${VEXDB_DUCK_EXTENSION_FILES})
set(PARAMETERS "-warnings")
build_loadable_extension(vex ${PARAMETERS} ${VEXDB_DUCK_EXTENSION_FILES})

target_compile_features(vex_extension PRIVATE cxx_std_17)
target_compile_features(vex_loadable_extension PRIVATE cxx_std_17)

set(SIMD_COMMON_FLAGS "-ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -floop-interchange -fno-exceptions -g1")
set(SIMD_WARN_FLAGS "-Wno-ignored-attributes")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/sse_dispatcher.cpp
        PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -msse3 -msse4.1 -msse4.2 -mfma ${SIMD_WARN_FLAGS}")
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/avx_dispatcher.cpp
        PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -mavx -mavx2 -mfma -mf16c ${SIMD_WARN_FLAGS}")
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/avx512_dispatcher.cpp
        PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx -mavx2 -mfma -mf16c ${SIMD_WARN_FLAGS}")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "8.0")
        set(NEON_FLAGS "${SIMD_COMMON_FLAGS} -march=armv8.2-a+simd+fp16+dotprod ${SIMD_WARN_FLAGS}")
    else()
        set(NEON_FLAGS "${SIMD_COMMON_FLAGS} -march=armv8.2-a+simd+fp16 ${SIMD_WARN_FLAGS}")
    endif()
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/distance/core/neon_dispatcher.cpp
        PROPERTIES COMPILE_FLAGS "${NEON_FLAGS}")
endif()
