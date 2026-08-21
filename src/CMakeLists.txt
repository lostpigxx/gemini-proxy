add_executable(proxyd main.cpp)
target_include_directories(proxyd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(proxyd
  PRIVATE
    vkp_options
    fmt::fmt
    quill::quill
    CLI11::CLI11
)

if(VKP_USE_MIMALLOC)
  target_link_libraries(proxyd PRIVATE mimalloc-static)
endif()
