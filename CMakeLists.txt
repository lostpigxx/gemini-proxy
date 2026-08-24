cmake_minimum_required(VERSION 3.28)

project(valkey-proxy
  VERSION 0.1.0
  DESCRIPTION "A high-performance valkey proxy"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++20 modules (docs/architecture-decisions.md §3.3); skipping the scan
# also drops the clang-scan-deps requirement on minimal toolchains.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VKP_WERROR "Treat warnings as errors" OFF)
option(VKP_USE_MIMALLOC "Link mimalloc as the global allocator" OFF)
option(VKP_BUILD_TESTS "Build unit tests" ON)
option(VKP_BUILD_BENCHMARKS "Build micro benchmarks" ON)
option(VKP_BUILD_FUZZERS "Build libFuzzer targets (requires clang with libFuzzer runtime)" OFF)
set(VKP_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. 'address,undefined' or 'thread'")

if(VKP_SANITIZE)
  add_compile_options(-fsanitize=${VKP_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${VKP_SANITIZE})
endif()

# Warning flags live on an interface target linked only into our own code,
# so third-party dependencies are never subjected to -Werror.
add_library(vkp_options INTERFACE)
target_compile_options(vkp_options INTERFACE -Wall -Wextra -Wpedantic)
if(VKP_WERROR)
  target_compile_options(vkp_options INTERFACE -Werror)
endif()

include(cmake/Dependencies.cmake)

add_subdirectory(src)

if(VKP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(VKP_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(VKP_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
