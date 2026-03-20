# iOS CMake toolchain file for DuckDB cross-compilation
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=scripts/ios-toolchain.cmake \
#         -DIOS_PLATFORM=OS ..
#
# Supported IOS_PLATFORM values:
#   OS          - Build for arm64 real devices
#   SIMULATOR   - Build for x86_64 simulator
#   SIMULATOR64 - Build for arm64 simulator (Apple Silicon host)

# System identification
SET(CMAKE_SYSTEM_NAME iOS)

# Minimum deployment target
SET(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum iOS version")

# Default platform to device if not specified
if(NOT DEFINED IOS_PLATFORM)
    SET(IOS_PLATFORM "OS")
endif()

# Configure architecture and SDK based on platform selection
if(IOS_PLATFORM STREQUAL "OS")
    SET(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "iOS device architecture")
    SET(CMAKE_OSX_SYSROOT "iphoneos" CACHE STRING "iOS SDK")
elseif(IOS_PLATFORM STREQUAL "SIMULATOR")
    SET(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "iOS simulator architecture")
    SET(CMAKE_OSX_SYSROOT "iphonesimulator" CACHE STRING "iOS Simulator SDK")
elseif(IOS_PLATFORM STREQUAL "SIMULATOR64")
    SET(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "iOS arm64 simulator architecture")
    SET(CMAKE_OSX_SYSROOT "iphonesimulator" CACHE STRING "iOS Simulator SDK")
else()
    message(FATAL_ERROR "Invalid IOS_PLATFORM: ${IOS_PLATFORM}. Must be OS, SIMULATOR, or SIMULATOR64.")
endif()

# Use our definitions for compiler tools
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# Search for libraries and headers in the target directories only
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Disable threads options that don't work on iOS
SET(CMAKE_THREAD_LIBS_INIT "-lpthread")
SET(CMAKE_HAVE_THREADS_LIBRARY 1)
SET(CMAKE_USE_WIN32_THREADS_INIT 0)
SET(CMAKE_USE_PTHREADS_INIT 1)

# Link against standard iOS frameworks
SET(DUCKDB_EXTRA_LINK_FLAGS -lc++ -lm -framework Foundation -framework Security)
