# Core library: everything except main(), shared by proxyd/tests/benchmarks/fuzzers.
add_library(vkp_lib STATIC
  core/buffer.cpp
  resp/parser.cpp
  resp/serializer.cpp
  resp/value.cpp
)
target_include_directories(vkp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vkp_lib PRIVATE vkp_options)

add_executable(proxyd main.cpp)
target_link_libraries(proxyd
  PRIVATE
    vkp_options
    vkp_lib
    fmt::fmt
    quill::quill
    CLI11::CLI11
)

if(VKP_USE_MIMALLOC)
  target_link_libraries(proxyd PRIVATE mimalloc-static)
endif()
