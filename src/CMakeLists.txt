# Core library: everything except main(), shared by proxyd/tests/benchmarks/fuzzers.
add_library(vkp_lib STATIC
  core/buffer.cpp
  io/backend.cpp
  io/event_loop.cpp
  io/frame_pool.cpp
  io/socket.cpp
  resp/parser.cpp
  resp/serializer.cpp
  resp/value.cpp
)
if(APPLE)
  target_sources(vkp_lib PRIVATE io/backend_kqueue.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(vkp_lib PRIVATE io/backend_epoll.cpp io/backend_uring.cpp)
  target_link_libraries(vkp_lib PRIVATE PkgConfig::liburing)
endif()
target_include_directories(vkp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vkp_lib PRIVATE vkp_options PUBLIC fmt::fmt)

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
