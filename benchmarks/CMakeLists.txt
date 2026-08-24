add_executable(vkp_bench
  smoke_bench.cpp
)
target_include_directories(vkp_bench PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vkp_bench
  PRIVATE
    vkp_options
    nanobench
    fmt::fmt
    xxHash::xxhash
)

add_executable(vkp_resp_bench
  resp_bench.cpp
)
target_link_libraries(vkp_resp_bench
  PRIVATE
    vkp_options
    vkp_lib
    nanobench
)
