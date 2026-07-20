cmake_minimum_required(VERSION 3.14)
project(vexdb_sqlite CXX C)

# 独立构建（仿 vexdb_pg 模式）。根 build.sh 深耦合 DuckDB 源码树，不复用。
#   cd vexdb_sqlite && cmake -S . -B build && cmake --build build
# 或走 ../build_sqlite.sh。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# 导出 compile_commands.json 供 clangd/LSP 用（配合 vexdb_sqlite/.clangd）。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VEXDB_SQLITE_BUILD_TESTS "Build M0 smoke tests" ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

find_package(Threads REQUIRED)

# macOS：默认对齐真实硬件架构。两个坑：① anaconda clang 默认 target 是 x86_64；
# ② 在 Rosetta(x86_64) 进程里 `uname -m` 会谎报 x86_64。都会编出与系统 arm64
# sqlite3 不匹配的库。用 sysctl hw.optional.arm64 探真实硬件（不被 Rosetta 欺骗）。
# 交叉编译（Stage C iOS/Android toolchain 文件）时不介入，arch 全权交 toolchain。
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_TOOLCHAIN_FILE
   AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    execute_process(COMMAND sysctl -n hw.optional.arm64
        OUTPUT_VARIABLE _IS_ARM64 OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_IS_ARM64 STREQUAL "1")
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
    else()
        execute_process(COMMAND uname -m OUTPUT_VARIABLE _HOST_ARCH OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(CMAKE_OSX_ARCHITECTURES "${_HOST_ARCH}" CACHE STRING "" FORCE)
    endif()
    message(STATUS "macOS arch 对齐真实硬件: ${CMAKE_OSX_ARCHITECTURES}")
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# vendored SQLite amalgamation：移动端静态形态必需 sqlite3.c；loadable 也用其头。
set(SQLITE_AMALG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
if(NOT EXISTS "${SQLITE_AMALG_DIR}/sqlite3ext.h")
    message(FATAL_ERROR
        "缺少 SQLite amalgamation。先运行:\n"
        "  bash ${CMAKE_CURRENT_SOURCE_DIR}/vendor_sqlite.sh")
endif()

# Boost（仅 header-only boost/preprocessor，距离 dispatcher 模板展开用——与
# DuckDB 端 vcpkg.json 同一依赖）。探测顺序：系统 find_package → 仓库内
# community-ext vcpkg 安装（DuckDB 分发链已拉好的）→ 报错给指引。
find_package(Boost QUIET)
if(Boost_FOUND)
    set(VEXDB_BOOST_INC "${Boost_INCLUDE_DIRS}")
else()
    file(GLOB _VCPKG_BOOST "${REPO_ROOT}/build/community-ext/vcpkg/installed/*/include")
    if(_VCPKG_BOOST)
        list(GET _VCPKG_BOOST 0 VEXDB_BOOST_INC)
    endif()
endif()
if(NOT VEXDB_BOOST_INC OR NOT EXISTS "${VEXDB_BOOST_INC}/boost/preprocessor/seq.hpp")
    message(FATAL_ERROR
        "缺少 header-only boost/preprocessor（距离 dispatcher 依赖）。任选其一:\n"
        "  brew install boost  /  apt install libboost-dev\n"
        "  或 cmake -DVEXDB_BOOST_INC=/path/to/include")
endif()
message(STATUS "Boost include: ${VEXDB_BOOST_INC}")

# 距离层源：common 侧 dispatcher + SQLite 侧 per-arch 薄实例化文件
# （与 vexdb_pg/vexdb_duckdb 同一套模板，运行时 SIMD dispatch）。
# 目标 arch 判定：macOS 交叉编译（-DCMAKE_OSX_ARCHITECTURES=x86_64 等）时
# CMAKE_SYSTEM_PROCESSOR 仍是 host 值，须以 OSX_ARCHITECTURES 为准。
set(_VEX_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    set(_VEX_TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
endif()
set(VEXDB_DISTANCE_SRCS
    ${REPO_ROOT}/common/distance/src/architecture.cpp
    ${REPO_ROOT}/common/distance/src/general_dispatcher.cpp
    src/distance/general.cpp
)
if(_VEX_TARGET_ARCH MATCHES "x86_64|amd64|AMD64")
    list(APPEND VEXDB_DISTANCE_SRCS
        ${REPO_ROOT}/common/distance/src/sse_dispatcher.cpp
        ${REPO_ROOT}/common/distance/src/avx_dispatcher.cpp
        ${REPO_ROOT}/common/distance/src/avx512_dispatcher.cpp
        src/distance/sse.cpp
        src/distance/avx.cpp
        src/distance/avx512.cpp
    )
elseif(_VEX_TARGET_ARCH MATCHES "aarch64|arm64")
    list(APPEND VEXDB_DISTANCE_SRCS
        ${REPO_ROOT}/common/distance/src/neon_dispatcher.cpp
        src/distance/neon.cpp
    )
endif()

set(VEXDB_SQLITE_SRCS
    src/vexdb_sqlite_init.cpp
    src/agent_files/vexfs_sqlite.cpp
    src/vtab/graph_index_vtab.cpp
    src/functions/distance_functions.cpp
    src/functions/vector_codec.cpp
    src/distance/vex_distance_entry.cpp
    src/index/graph_algo_smoke.cpp
    src/index/graph_bridge.cpp
    ${VEXDB_DISTANCE_SRCS}
)

set(VEXDB_SQLITE_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/store
    ${SQLITE_AMALG_DIR}
    ${REPO_ROOT}/common/include
    ${REPO_ROOT}/common
    ${REPO_ROOT}/common/distance/core
    ${REPO_ROOT}/common/distance/pq
    ${REPO_ROOT}/common/distance/src
    ${VEXDB_BOOST_INC}
)

# 版本信息注入（vexdb_version() 输出；对齐 duck 端做法）
execute_process(COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE VEXDB_SQLITE_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT VEXDB_SQLITE_GIT_HASH)
    set(VEXDB_SQLITE_GIT_HASH "unknown")
endif()
string(TIMESTAMP VEXDB_SQLITE_BUILD_TIME "%Y-%m-%d %H:%M")
add_compile_definitions(
    VEXDB_SQLITE_GIT_HASH="${VEXDB_SQLITE_GIT_HASH}"
    VEXDB_SQLITE_BUILD_TIME="${VEXDB_SQLITE_BUILD_TIME}")

# SQLite 端靠 C++ 异常在边界 catch 转 SQLITE_ERROR（与 PG 的 -fno-exceptions 不同，
# 不可关异常）。debug info 交给 CMAKE_BUILD_TYPE（Release 不带 -g——静态库
# 满载 debug info 时 XCFramework 膨胀到 46MB，去掉后 ~6MB）。
add_compile_options(-Wall)

# common 的宿主分支宏：SQLITE 与 DUCK 同走"纯 STL 环境"语义分支（不引
# pg_compat.h/postgres.h），但不拉 DuckDB 的 store 头——见
# common/distance/core/distance_dispatcher.h 的 SQLITE 分支。
add_compile_definitions(PG_VEXDB_TARGET_SQLITE)

# SIMD per-file flags（对齐 vexdb_pg/vexdb_duckdb：模板核 -fno-exceptions 纯计算）。
# set_source_files_properties 是目录级作用域，对 loadable/static 两 target 同时生效。
set(SIMD_COMMON_FLAGS "-ftree-vectorize -fassociative-math -fno-signed-zeros -fno-trapping-math -fno-exceptions -funroll-loops -Wno-ignored-attributes")
# -floop-interchange 是 GCC 专属（NDK clang 报 unknown argument 硬错；apple
# clang 容忍）——仅 GCC 加。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(SIMD_COMMON_FLAGS "${SIMD_COMMON_FLAGS} -floop-interchange")
endif()
if(NOT CMAKE_BUILD_TYPE MATCHES "[Dd]ebug")
    set(SIMD_COMMON_FLAGS "${SIMD_COMMON_FLAGS} -O3 -ffp-contract=fast")
endif()
include(CheckCXXCompilerFlag)
if(_VEX_TARGET_ARCH MATCHES "x86_64|amd64|AMD64")
    set_source_files_properties(${REPO_ROOT}/common/distance/src/sse_dispatcher.cpp src/distance/sse.cpp
        PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -msse3 -msse4.1 -msse4.2 -mfma -mf16c")
    set_source_files_properties(${REPO_ROOT}/common/distance/src/avx_dispatcher.cpp src/distance/avx.cpp
        PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -mavx -mavx2 -mfma -mf16c")
    set_source_files_properties(${REPO_ROOT}/common/distance/src/avx512_dispatcher.cpp src/distance/avx512.cpp
        PROPERTIES COMPILE_FLAGS "${SIMD_COMMON_FLAGS} -mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx -mavx2 -mfma -mf16c")
elseif(_VEX_TARGET_ARCH MATCHES "aarch64|arm64")
    if(ANDROID)
        # Android 端侧基线 = armv8-a（NDK 默认；大量在役设备是 v8.0 老核，
        # +lse/+fp16 会 SIGILL——服务器侧的 v8.2 基线是 Kunpeng 场景取舍）。
        set(NEON_FLAGS "${SIMD_COMMON_FLAGS}")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        # iOS arm64 = A12+（armv8.3+），v8.2 特性全集安全。
        set(NEON_FLAGS "${SIMD_COMMON_FLAGS} -march=armv8.2-a+lse+fp16+dotprod+crc")
    else()
        # 服务器/桌面 ARMv8.2 基线（Kunpeng/TaiShan v110；v8.0 老核 SIGILL on
        # LSE），对齐 pg/duck。
        if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "8.0")
            set(NEON_FLAGS "${SIMD_COMMON_FLAGS} -march=armv8.2-a+lse+fp16+dotprod+crc")
        else()
            set(NEON_FLAGS "${SIMD_COMMON_FLAGS} -march=armv8.2-a+lse+fp16")
        endif()
        check_cxx_compiler_flag("-mtune=tsv110" HAS_MTUNE_TSV110)
        if(HAS_MTUNE_TSV110)
            set(NEON_FLAGS "${NEON_FLAGS} -mtune=tsv110")
        endif()
    endif()
    set_source_files_properties(${REPO_ROOT}/common/distance/src/neon_dispatcher.cpp src/distance/neon.cpp
        PROPERTIES COMPILE_FLAGS "${NEON_FLAGS}")
endif()

# ---- 形态 1：loadable 扩展（.so/.dylib），不链 sqlite3，运行时用宿主 API ----
# iOS 禁运行时加载（系统 libsqlite3 关 load_extension + App Store 动态代码限制），
# 不产此形态；Android 保留（requery 等自带 SQLite 的宿主支持 load_extension）。
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_library(vexdb_lite_loadable SHARED ${VEXDB_SQLITE_SRCS})
    target_include_directories(vexdb_lite_loadable PRIVATE ${VEXDB_SQLITE_INCLUDES})
    set_target_properties(vexdb_lite_loadable PROPERTIES OUTPUT_NAME "vexdb_lite" PREFIX "")
    if(APPLE)
        # 扩展符号 sqlite3_* 在加载时由宿主进程解析。
        target_link_options(vexdb_lite_loadable PRIVATE -undefined dynamic_lookup)
    endif()
endif()

# ---- 形态 2：静态注册（.a），移动端唯一可行形态，直接链宿主 sqlite3 符号 ----
add_library(vexdb_lite_static STATIC ${VEXDB_SQLITE_SRCS})
target_include_directories(vexdb_lite_static PRIVATE ${VEXDB_SQLITE_INCLUDES})
target_compile_definitions(vexdb_lite_static PRIVATE VEXDB_SQLITE_CORE=1)
set_target_properties(vexdb_lite_static PROPERTIES OUTPUT_NAME "vexdb_lite_static")

# 平台挂载适配层共用的稳定 C ABI。FSKit/libfuse/WinFsp 都只接这一层，
# 具体状态仍由上面的 SQLite 插件通过 vexfs_* SQL 管理。
add_library(vexfs_mount_contract STATIC
    ${REPO_ROOT}/agent_files/mount/common/src/vexfs_mount_contract_sqlite.cpp)
target_include_directories(vexfs_mount_contract PUBLIC
    ${REPO_ROOT}/agent_files/mount/common/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SQLITE_AMALG_DIR})
target_compile_definitions(vexfs_mount_contract PRIVATE VEXDB_SQLITE_CORE=1)
target_link_libraries(vexfs_mount_contract PUBLIC vexdb_lite_static)
set_target_properties(vexfs_mount_contract PROPERTIES OUTPUT_NAME "vexfs_mount_contract")

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    enable_language(OBJCXX)
    find_library(VEXFS_SYSTEM_SQLITE3 sqlite3 REQUIRED)
    find_library(VEXFS_FOUNDATION Foundation REQUIRED)
    add_executable(vexfs_cli
        ${REPO_ROOT}/agent_files/cli/vexfs_main.cpp
        ${REPO_ROOT}/agent_files/cli/vexfs_fskit_state.mm)
    set_source_files_properties(
        ${REPO_ROOT}/agent_files/cli/vexfs_fskit_state.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_include_directories(vexfs_cli PRIVATE
        ${REPO_ROOT}/agent_files/mount/common/include
        ${REPO_ROOT}/agent_files/cli)
    target_link_libraries(vexfs_cli PRIVATE vexfs_mount_contract
        ${VEXFS_SYSTEM_SQLITE3} ${VEXFS_FOUNDATION} Threads::Threads ${CMAKE_DL_LIBS})
    set_target_properties(vexfs_cli PROPERTIES OUTPUT_NAME "vexfs")
endif()

if(VEXDB_SQLITE_BUILD_TESTS)
    # 把 amalgamation 编成静态库供 smoke test 链接（模拟移动端宿主自带 sqlite3）。
    add_library(sqlite3_amalg STATIC ${SQLITE_AMALG_DIR}/sqlite3.c)
    target_include_directories(sqlite3_amalg PUBLIC ${SQLITE_AMALG_DIR})
    target_compile_definitions(sqlite3_amalg PRIVATE SQLITE_ENABLE_LOAD_EXTENSION=1)

    foreach(smoke m0_static_smoke m1_distance_smoke m2_vtab_smoke m3_hnsw_smoke vexfs_static_smoke)
        add_executable(${smoke} test/${smoke}.c)
        target_include_directories(${smoke} PRIVATE ${VEXDB_SQLITE_INCLUDES})
        # 宿主侧静态注册形态：直接用真实 sqlite3 符号（非 ext 间接表）。
        target_compile_definitions(${smoke} PRIVATE VEXDB_SQLITE_CORE=1)
        target_link_libraries(${smoke} PRIVATE
            vexdb_lite_static sqlite3_amalg Threads::Threads ${CMAKE_DL_LIBS})
    endforeach()

    # M3+ 并行建图冒烟（C++，直接调 GraphBridge）
    add_executable(m3p_parallel_smoke test/m3p_parallel_smoke.cpp)
    target_include_directories(m3p_parallel_smoke PRIVATE ${VEXDB_SQLITE_INCLUDES})
    target_compile_definitions(m3p_parallel_smoke PRIVATE VEXDB_SQLITE_CORE=1)
    target_link_libraries(m3p_parallel_smoke PRIVATE
        vexdb_lite_static sqlite3_amalg Threads::Threads ${CMAKE_DL_LIBS})

    add_executable(vexfs_mount_contract_smoke
        ${REPO_ROOT}/agent_files/mount/common/test/vexfs_mount_contract_smoke.cpp)
    target_include_directories(vexfs_mount_contract_smoke PRIVATE
        ${REPO_ROOT}/agent_files/mount/common/include ${SQLITE_AMALG_DIR})
    target_link_libraries(vexfs_mount_contract_smoke PRIVATE
        vexfs_mount_contract sqlite3_amalg Threads::Threads ${CMAKE_DL_LIBS})
endif()
