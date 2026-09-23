cmake_minimum_required(VERSION 3.20)
project(xmatch_matching_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Warnings + optimization flags
# ---------------------------------------------------------------------------
add_compile_options(-Wall -Wextra -Wpedantic)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
option(ENGINE_NATIVE_ARCH "Build with -march=native" OFF)
if(ENGINE_NATIVE_ARCH)
  add_compile_options(-march=native)
endif()

# ---------------------------------------------------------------------------
# Internal engine core: everything except the C ABI entry points. Compiled
# once (PIC, hidden visibility) and linked into both the shared library and
# the internal unit tests. engine_api.cpp stays out so no test binary can
# carry its own xmatch_create and shadow the one in the .so.
# ---------------------------------------------------------------------------
add_library(matching_engine_core STATIC
  src/tick_table.cpp
  src/price_ladder.cpp
  src/order_book.cpp
  src/engine.cpp
)

target_include_directories(matching_engine_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(matching_engine_core PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# ---------------------------------------------------------------------------
# The engine shared library
# ---------------------------------------------------------------------------
add_library(matching_engine SHARED
  src/engine_api.cpp
)

target_link_libraries(matching_engine PRIVATE matching_engine_core)

target_include_directories(matching_engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(matching_engine PROPERTIES
  OUTPUT_NAME "matching_engine"
  VERSION 1.0.0
  # Export only the XMATCH_EXPORT-marked C entry points; everything in
  # xmatch::detail stays internal to the .so.
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# No exceptions/RTTI concerns: we keep exceptions enabled (STL may use them
# internally) but the engine itself never lets one cross the C ABI boundary
# (see engine_api.cpp). We intentionally do NOT build with -fno-exceptions
# because STL containers we rely on (std::vector, std::deque) use them
# internally for allocation failures; catching at the ABI boundary is safer
# than aborting the process on OOM.

# ---------------------------------------------------------------------------
# Tests (GoogleTest via FetchContent)
# ---------------------------------------------------------------------------
option(ENGINE_BUILD_TESTS "Build unit tests" ON)
option(ENGINE_BUILD_BENCH "Build the latency benchmark" ON)

if(ENGINE_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  include(GoogleTest)

  # Public-API tests: link only the shipped .so, so they exercise exactly
  # what a host program loads.
  add_executable(engine_api_tests
    tests/test_golden_scenario.cpp
    tests/test_validation.cpp
    tests/test_matching.cpp
    tests/test_cancel_replace.cpp
    tests/test_multi_instrument.cpp
  )
  target_link_libraries(engine_api_tests PRIVATE matching_engine gtest_main)
  gtest_discover_tests(engine_api_tests)

  # detail:: building-block tests: the .so hides these symbols, so link
  # the internal core directly.
  add_executable(engine_internal_tests
    tests/test_internal_structures.cpp
  )
  target_link_libraries(engine_internal_tests PRIVATE matching_engine_core gtest_main)
  gtest_discover_tests(engine_internal_tests)
endif()

# ---------------------------------------------------------------------------
# Standalone latency/throughput benchmark (custom chrono-based)
# ---------------------------------------------------------------------------
if(ENGINE_BUILD_BENCH)
  add_executable(engine_bench bench/benchmark_main.cpp)
  target_link_libraries(engine_bench PRIVATE matching_engine)
  target_include_directories(engine_bench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_compile_options(engine_bench PRIVATE -O3)
endif()
