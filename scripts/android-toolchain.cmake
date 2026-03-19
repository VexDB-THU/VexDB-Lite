# Android CMake toolchain wrapper for DuckDB cross-compilation
#
# This file configures DuckDB-specific settings and then includes the
# Android NDK's built-in toolchain. It is not a standalone toolchain.
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=scripts/android-toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a ..
#
# Required:
#   ANDROID_NDK - Path to the Android NDK (env var or CMake var)
#
# Supported ANDROID_ABI values:
#   arm64-v8a   - 64-bit ARM (primary target, best NEON support)
#   armeabi-v7a - 32-bit ARM
#   x86_64      - 64-bit x86 (emulator)

# Resolve NDK path from environment if not set as CMake variable
if(NOT DEFINED ANDROID_NDK)
    if(DEFINED ENV{ANDROID_NDK})
        SET(ANDROID_NDK $ENV{ANDROID_NDK})
    elseif(DEFINED ENV{ANDROID_NDK_HOME})
        SET(ANDROID_NDK $ENV{ANDROID_NDK_HOME})
    else()
        message(FATAL_ERROR "ANDROID_NDK is not set. Set it as a CMake variable or environment variable.")
    endif()
endif()

# Default ABI to arm64-v8a if not specified
if(NOT DEFINED ANDROID_ABI)
    SET(ANDROID_ABI "arm64-v8a")
endif()

# Minimum API level 24 for good NEON intrinsics and C++17 support
if(NOT DEFINED ANDROID_PLATFORM)
    SET(ANDROID_PLATFORM "android-24")
endif()

# Use libc++ as the STL implementation
if(NOT DEFINED ANDROID_STL)
    SET(ANDROID_STL "c++_static")
endif()

# Include the NDK's built-in CMake toolchain
SET(NDK_TOOLCHAIN_FILE "${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
if(NOT EXISTS "${NDK_TOOLCHAIN_FILE}")
    message(FATAL_ERROR "NDK toolchain not found at: ${NDK_TOOLCHAIN_FILE}\nCheck that ANDROID_NDK points to a valid NDK installation.")
endif()
include("${NDK_TOOLCHAIN_FILE}")

# Note: jemalloc and other extensions are skipped via -DSKIP_EXTENSIONS in build.sh.
# Do not set SKIP_EXTENSIONS here to avoid CACHE FORCE conflicts with command-line values.

# Android-specific compile definitions
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DDUCKDB_ANDROID")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DDUCKDB_ANDROID")

# Link against Android system libraries + enable linker dead code stripping
SET(DUCKDB_EXTRA_LINK_FLAGS -llog -ldl -lm)
SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections -Wl,--strip-all")
SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections -Wl,--strip-all")

# Use our definitions for compiler tools
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# Search for libraries and headers in the target directories only
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
