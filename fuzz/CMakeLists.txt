# libFuzzer targets. Requires a clang with the libFuzzer runtime (Linux clang
# or Homebrew LLVM; Apple Clang does not ship it — use scripts/fuzz.sh, which
# runs in a Linux container on macOS).
#
# Library sources are compiled directly into the target so they receive
# -fsanitize=fuzzer coverage instrumentation; linking vkp_lib would leave the
# parser uninstrumented and the fuzzing blind.
add_executable(resp_parser_fuzz
  resp_parser_fuzz.cpp
  ${CMAKE_SOURCE_DIR}/src/core/buffer.cpp
  ${CMAKE_SOURCE_DIR}/src/resp/parser.cpp
  ${CMAKE_SOURCE_DIR}/src/resp/value.cpp
)
target_include_directories(resp_parser_fuzz PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(resp_parser_fuzz PRIVATE -fsanitize=fuzzer)
target_link_options(resp_parser_fuzz PRIVATE -fsanitize=fuzzer)
target_link_libraries(resp_parser_fuzz PRIVATE vkp_options)
