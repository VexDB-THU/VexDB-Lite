cmake_minimum_required(VERSION 3.20)
project(vexdb_wasm_demo C CXX)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "请使用 emcmake cmake 配置这个示例")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(VEXDB_SQLITE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPO_ROOT}/vexdb_sqlite" "${CMAKE_BINARY_DIR}/vexdb_sqlite")

add_library(sqlite3_wasm STATIC
    "${REPO_ROOT}/vexdb_sqlite/third_party/sqlite/sqlite3.c")
target_include_directories(sqlite3_wasm PUBLIC
    "${REPO_ROOT}/vexdb_sqlite/third_party/sqlite")
target_compile_definitions(sqlite3_wasm PRIVATE
    SQLITE_THREADSAFE=0
    SQLITE_OMIT_LOAD_EXTENSION=1)

add_executable(vexdb_wasm native/vexdb_wasm.cpp)
target_include_directories(vexdb_wasm PRIVATE
    "${REPO_ROOT}/vexdb_sqlite/include"
    "${REPO_ROOT}/vexdb_sqlite/third_party/sqlite")
target_compile_definitions(vexdb_wasm PRIVATE VEXDB_SQLITE_CORE=1)
target_link_libraries(vexdb_wasm PRIVATE vexdb_lite_static sqlite3_wasm)

set_target_properties(vexdb_wasm PROPERTIES
    OUTPUT_NAME "vexdb-wasm"
    SUFFIX ".js")

target_link_options(vexdb_wasm PRIVATE
    "-sWASM=1"
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createVexDBWasm"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=67108864"
    "-sMAXIMUM_MEMORY=536870912"
    "-sENVIRONMENT=web,node"
    "-sNO_EXIT_RUNTIME=1"
    "-sFILESYSTEM=0"
    "-sEXPORTED_RUNTIME_METHODS=['cwrap']")
