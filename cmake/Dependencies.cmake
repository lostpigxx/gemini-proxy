# All third-party dependencies, pinned via CPM (see docs/architecture-decisions.md §3).
include(${CMAKE_CURRENT_LIST_DIR}/CPM.cmake)

CPMAddPackage("gh:fmtlib/fmt#12.2.0")

CPMAddPackage(
  NAME quill
  GITHUB_REPOSITORY odygrd/quill
  GIT_TAG v12.1.0
)

CPMAddPackage(
  NAME CLI11
  GITHUB_REPOSITORY CLIUtils/CLI11
  GIT_TAG v2.7.2
)

CPMAddPackage(
  NAME tomlplusplus
  GITHUB_REPOSITORY marzer/tomlplusplus
  GIT_TAG v3.4.0
)

# xxHash ships CMake support only in a subdirectory and lags on modern CMake;
# we consume it header-only (XXH_INLINE_ALL) and skip its build system entirely.
CPMAddPackage(
  NAME xxHash
  GITHUB_REPOSITORY Cyan4973/xxHash
  GIT_TAG v0.8.3
  DOWNLOAD_ONLY YES
)
add_library(xxhash INTERFACE)
target_include_directories(xxhash SYSTEM INTERFACE ${xxHash_SOURCE_DIR})
target_compile_definitions(xxhash INTERFACE XXH_INLINE_ALL)
add_library(xxHash::xxhash ALIAS xxhash)

if(VKP_BUILD_TESTS)
  CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    GIT_TAG v3.15.3
  )
  list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
endif()

if(VKP_BUILD_BENCHMARKS)
  CPMAddPackage(
    NAME nanobench
    GITHUB_REPOSITORY martinus/nanobench
    GIT_TAG v4.6.0
  )
endif()

if(VKP_USE_MIMALLOC)
  CPMAddPackage(
    NAME mimalloc
    GITHUB_REPOSITORY microsoft/mimalloc
    GIT_TAG v3.5.0
    OPTIONS
      "MI_BUILD_SHARED OFF"
      "MI_BUILD_OBJECT OFF"
      "MI_BUILD_TESTS OFF"
  )
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(liburing REQUIRED IMPORTED_TARGET liburing>=2.5)
  message(STATUS "liburing ${liburing_VERSION} found")
endif()
