cmake_minimum_required(VERSION 3.16)

project(vex_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(VEX_CORE_BUILD_TESTS "Build libvex-core tests" OFF)

add_library(vex_core STATIC
  src/distance.cpp
  src/product_quantizer.cpp
  src/quant_distancer.cpp
  src/rabitq.cpp
  src/node_store_memory.cpp
  src/graph_algo.cpp
  src/adapter_low_level_binding_mock.cpp
  src/vex_core_capi.cpp
)

add_library(vex::core ALIAS vex_core)

target_include_directories(vex_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(vex_core PUBLIC cxx_std_17)

target_compile_definitions(vex_core PRIVATE VEX_CORE_BUILD)

if (MSVC)
  target_compile_options(vex_core PRIVATE /W4)
else()
  target_compile_options(vex_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS vex_core
  ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
  DESTINATION include
)

if (VEX_CORE_BUILD_TESTS)
  enable_testing()
  add_executable(vex_core_capi_roundtrip_test
    tests/capi_roundtrip.cpp
  )
  target_link_libraries(vex_core_capi_roundtrip_test PRIVATE vex_core)
  add_test(NAME vex_core_capi_roundtrip_test COMMAND vex_core_capi_roundtrip_test)

  add_executable(vex_core_duckdb_stub_store_smoke_test
    tests/duckdb_stub_store_smoke.cpp
  )
  target_link_libraries(vex_core_duckdb_stub_store_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_duckdb_stub_store_smoke_test COMMAND vex_core_duckdb_stub_store_smoke_test)

  add_executable(vex_core_duckdb_direct_backend_smoke_test
    tests/duckdb_direct_backend_smoke.cpp
  )
  target_link_libraries(vex_core_duckdb_direct_backend_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_duckdb_direct_backend_smoke_test COMMAND vex_core_duckdb_direct_backend_smoke_test)

  add_executable(vex_core_graph_add_point_with_level_smoke_test
    tests/graph_add_point_with_level_smoke.cpp
  )
  target_link_libraries(vex_core_graph_add_point_with_level_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_graph_add_point_with_level_smoke_test COMMAND vex_core_graph_add_point_with_level_smoke_test)

  add_executable(vex_core_duckdb_direct_filtered_backend_smoke_test
    tests/duckdb_direct_filtered_backend_smoke.cpp
  )
  target_link_libraries(vex_core_duckdb_direct_filtered_backend_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_duckdb_direct_filtered_backend_smoke_test COMMAND vex_core_duckdb_direct_filtered_backend_smoke_test)

  add_executable(vex_core_duckdb_direct_pq_backend_smoke_test
    tests/duckdb_direct_pq_backend_smoke.cpp
  )
  target_link_libraries(vex_core_duckdb_direct_pq_backend_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_duckdb_direct_pq_backend_smoke_test COMMAND vex_core_duckdb_direct_pq_backend_smoke_test)

  add_executable(vex_core_pg_direct_backend_smoke_test
    tests/pg_direct_backend_smoke.cpp
  )
  target_link_libraries(vex_core_pg_direct_backend_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_pg_direct_backend_smoke_test COMMAND vex_core_pg_direct_backend_smoke_test)

  add_executable(vex_core_pg_direct_backend_mutation_smoke_test
    tests/pg_direct_backend_mutation_smoke.cpp
  )
  target_link_libraries(vex_core_pg_direct_backend_mutation_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_pg_direct_backend_mutation_smoke_test COMMAND vex_core_pg_direct_backend_mutation_smoke_test)

  add_executable(vex_core_adapter_graph_state_callback_smoke_test
    tests/adapter_graph_state_callback_smoke.cpp
  )
  target_link_libraries(vex_core_adapter_graph_state_callback_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_adapter_graph_state_callback_smoke_test COMMAND vex_core_adapter_graph_state_callback_smoke_test)

  add_executable(vex_core_duckdb_graph_state_roundtrip_smoke_test
    tests/duckdb_graph_state_roundtrip_smoke.cpp
  )
  target_link_libraries(vex_core_duckdb_graph_state_roundtrip_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_duckdb_graph_state_roundtrip_smoke_test COMMAND vex_core_duckdb_graph_state_roundtrip_smoke_test)

  add_executable(vex_core_duckdb_graph_state_options_roundtrip_smoke_test
    tests/duckdb_graph_state_options_roundtrip_smoke.cpp
  )
  target_link_libraries(vex_core_duckdb_graph_state_options_roundtrip_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_duckdb_graph_state_options_roundtrip_smoke_test COMMAND vex_core_duckdb_graph_state_options_roundtrip_smoke_test)

  add_executable(vex_core_pg_graph_state_roundtrip_smoke_test
    tests/pg_graph_state_roundtrip_smoke.cpp
  )
  target_link_libraries(vex_core_pg_graph_state_roundtrip_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_pg_graph_state_roundtrip_smoke_test COMMAND vex_core_pg_graph_state_roundtrip_smoke_test)

  add_executable(vex_core_quant_distancer_smoke_test
    tests/quant_distancer_smoke.cpp
  )
  target_link_libraries(vex_core_quant_distancer_smoke_test PRIVATE vex_core)
  add_test(NAME vex_core_quant_distancer_smoke_test COMMAND vex_core_quant_distancer_smoke_test)
endif()
