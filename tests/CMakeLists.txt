include(Catch)

add_executable(vkp_tests
  core/smoke_test.cpp
  core/buffer_test.cpp
  io/event_loop_test.cpp
  io/task_test.cpp
  resp/parser_test.cpp
  resp/serializer_test.cpp
  resp/value_test.cpp
)
target_link_libraries(vkp_tests
  PRIVATE
    vkp_options
    vkp_lib
    Catch2::Catch2WithMain
    tomlplusplus::tomlplusplus
    xxHash::xxhash
)

catch_discover_tests(vkp_tests)
