include(Catch)

add_executable(vkp_tests
  core/smoke_test.cpp
)
target_include_directories(vkp_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vkp_tests
  PRIVATE
    vkp_options
    Catch2::Catch2WithMain
    tomlplusplus::tomlplusplus
    xxHash::xxhash
)

catch_discover_tests(vkp_tests)
