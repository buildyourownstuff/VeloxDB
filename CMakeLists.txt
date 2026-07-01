cmake_minimum_required(VERSION 3.20)

project(VeloxDB VERSION 0.1.0 LANGUAGES CXX)

option(VELOXDB_BUILD_TESTS "Build VeloxDB tests" ON)
option(VELOXDB_BUILD_INTEGRATION_TESTS "Build VeloxDB integration tests" ON)
option(VELOXDB_BUILD_BENCHMARKS "Build VeloxDB benchmarks" ON)
option(VELOXDB_BUILD_FUZZERS "Build libFuzzer fuzz targets" OFF)
option(VELOXDB_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(VELOXDB_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(VELOXDB_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(VELOXDB_ENABLE_LTO "Enable link-time optimization" OFF)
set(VELOXDB_FUZZ_SANITIZERS "address,undefined" CACHE STRING
    "Comma-separated sanitizer list used by libFuzzer targets")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CheckIPOSupported)
include(CheckCXXSourceCompiles)

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

  if(VELOXDB_BUILD_INTEGRATION_TESTS)
    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE)
      add_test(
        NAME redis_cli_integration
        COMMAND ${BASH_EXECUTABLE}
                ${PROJECT_SOURCE_DIR}/tests/integration/redis_cli_smoke.sh
                $<TARGET_FILE:veloxdb>)
      set_tests_properties(redis_cli_integration PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 45)
    else()
      message(WARNING "bash not found; redis-cli integration test will not be registered")
    endif()
  endif()
endif()

if(VELOXDB_BUILD_BENCHMARKS)
  add_executable(veloxdb_bench benchmarks/veloxdb_bench.cpp)
  veloxdb_apply_common(veloxdb_bench)
  target_link_libraries(veloxdb_bench PRIVATE veloxdb_core)
  set_target_properties(veloxdb_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/benchmarks)
endif()

if(VELOXDB_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "VELOXDB_BUILD_FUZZERS requires Clang with libFuzzer support")
  endif()
  if(VELOXDB_ENABLE_TSAN)
    message(FATAL_ERROR "libFuzzer targets cannot be built with VELOXDB_ENABLE_TSAN")
  endif()

  if(VELOXDB_FUZZ_SANITIZERS STREQUAL "")
    set(_fuzzer_no_link "-fsanitize=fuzzer-no-link")
    set(_fuzzer_link "-fsanitize=fuzzer")
  else()
    set(_fuzzer_no_link "-fsanitize=fuzzer-no-link,${VELOXDB_FUZZ_SANITIZERS}")
    set(_fuzzer_link "-fsanitize=fuzzer,${VELOXDB_FUZZ_SANITIZERS}")
  endif()

  set(_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "${_fuzzer_link}")
  check_cxx_source_compiles(
    "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long) { return 0; }"
    VELOXDB_HAS_LIBFUZZER_RUNTIME)
  set(CMAKE_REQUIRED_FLAGS "${_saved_required_flags}")
  if(NOT VELOXDB_HAS_LIBFUZZER_RUNTIME)
    message(FATAL_ERROR
      "Clang was found, but libFuzzer/compiler-rt runtime is unavailable. "
      "Install compiler-rt, for example libclang-rt-18-dev on Ubuntu 24.04.")
  endif()

  add_library(veloxdb_resp_parser_fuzzlib
    src/protocol/resp.cpp
    src/util/status.cpp
    src/util/string.cpp
  )
  veloxdb_apply_common(veloxdb_resp_parser_fuzzlib)
  target_compile_options(veloxdb_resp_parser_fuzzlib PRIVATE
    ${_fuzzer_no_link}
    -fno-omit-frame-pointer)

  add_executable(veloxdb_resp_parser_fuzzer fuzz/resp_parser_fuzzer.cpp)
  veloxdb_apply_common(veloxdb_resp_parser_fuzzer)
  target_compile_options(veloxdb_resp_parser_fuzzer PRIVATE
    ${_fuzzer_link}
    -fno-omit-frame-pointer)
  target_link_options(veloxdb_resp_parser_fuzzer PRIVATE ${_fuzzer_link})
  target_link_libraries(veloxdb_resp_parser_fuzzer PRIVATE veloxdb_resp_parser_fuzzlib)
  set_target_properties(veloxdb_resp_parser_fuzzer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz)
endif()
