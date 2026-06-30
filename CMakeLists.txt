cmake_minimum_required(VERSION 3.20)

project(VeloxDB VERSION 0.1.0 LANGUAGES CXX)

option(VELOXDB_BUILD_TESTS "Build VeloxDB tests" ON)
option(VELOXDB_BUILD_BENCHMARKS "Build VeloxDB benchmarks" ON)
option(VELOXDB_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(VELOXDB_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(VELOXDB_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(VELOXDB_ENABLE_LTO "Enable link-time optimization" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckIPOSupported)

function(veloxdb_apply_common target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR}/include)
  target_compile_definitions(${target} PUBLIC VELOXDB_VERSION="${PROJECT_VERSION}")

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endif()

  if(VELOXDB_ENABLE_ASAN AND VELOXDB_ENABLE_TSAN)
    message(FATAL_ERROR "ASAN and TSAN cannot be enabled together")
  endif()

  set(_sanitizers "")
  if(VELOXDB_ENABLE_ASAN)
    list(APPEND _sanitizers address)
  endif()
  if(VELOXDB_ENABLE_UBSAN)
    list(APPEND _sanitizers undefined)
  endif()
  if(VELOXDB_ENABLE_TSAN)
    list(APPEND _sanitizers thread)
  endif()
  if(_sanitizers AND NOT MSVC)
    list(JOIN _sanitizers "," _sanitizer_flags)
    target_compile_options(${target} PRIVATE -fsanitize=${_sanitizer_flags} -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=${_sanitizer_flags})
  endif()

  if(VELOXDB_ENABLE_LTO)
    check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_error)
    if(_ipo_supported)
      set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
      message(WARNING "IPO/LTO is not supported: ${_ipo_error}")
    endif()
  endif()
endfunction()

add_library(veloxdb_core
  src/commands/command_registry.cpp
  src/commands/commands.cpp
  src/config/config.cpp
  src/metrics/metrics.cpp
  src/net/tcp_server.cpp
  src/persistence/aof.cpp
  src/persistence/snapshot.cpp
  src/protocol/resp.cpp
  src/server/server.cpp
  src/storage/storage_engine.cpp
  src/util/logger.cpp
  src/util/status.cpp
  src/util/string.cpp
)
veloxdb_apply_common(veloxdb_core)
find_package(Threads REQUIRED)
target_link_libraries(veloxdb_core PUBLIC Threads::Threads)

add_executable(veloxdb src/main.cpp)
veloxdb_apply_common(veloxdb)
target_link_libraries(veloxdb PRIVATE veloxdb_core)

if(VELOXDB_BUILD_TESTS)
  enable_testing()
  add_executable(veloxdb_tests
    tests/test_main.cpp
    tests/unit/command_tests.cpp
    tests/unit/persistence_tests.cpp
    tests/unit/resp_parser_tests.cpp
    tests/unit/storage_tests.cpp
  )
  veloxdb_apply_common(veloxdb_tests)
  target_link_libraries(veloxdb_tests PRIVATE veloxdb_core)
  add_test(NAME veloxdb_tests COMMAND veloxdb_tests)
endif()

if(VELOXDB_BUILD_BENCHMARKS)
  add_executable(veloxdb_bench benchmarks/veloxdb_bench.cpp)
  veloxdb_apply_common(veloxdb_bench)
  target_link_libraries(veloxdb_bench PRIVATE veloxdb_core)
  set_target_properties(veloxdb_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/benchmarks)
endif()
