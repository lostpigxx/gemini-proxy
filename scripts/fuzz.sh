#!/usr/bin/env bash
# Runs the RESP parser fuzzer for N seconds (default 60).
#
#   ./scripts/fuzz.sh [seconds]
#
# Apple Clang has no libFuzzer runtime, so on macOS this runs inside a Linux
# container (requires Docker). The build tree and CPM sources live in a named
# volume (vkp-fuzz-cache), so only the first run pays the setup cost. New
# corpus entries are written back to fuzz/corpus/ on the host.
set -euo pipefail

DURATION="${1:-60}"
# Direct docker.io pulls may be blocked; override with a mirror if needed,
# e.g. VKP_FUZZ_IMAGE=docker.m.daocloud.io/library/ubuntu:24.04
# Slow apt? Point VKP_APT_MIRROR at a full mirror URL incl. trailing slash,
# e.g. VKP_APT_MIRROR=https://mirrors.aliyun.com/ubuntu-ports/ (arm64 host)
FUZZ_IMAGE="${VKP_FUZZ_IMAGE:-ubuntu:24.04}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FUZZ_ARGS=(-max_total_time="${DURATION}" -rss_limit_mb=2048 -print_final_stats=1 fuzz/corpus)

if [[ "$(uname)" == "Darwin" ]]; then
  exec docker run --rm \
    -v "${REPO_ROOT}:/src" \
    -v vkp-fuzz-cache:/cache \
    -w /src \
    "${FUZZ_IMAGE}" \
    bash -ec "
      export DEBIAN_FRONTEND=noninteractive
      if [[ -n '${VKP_APT_MIRROR:-}' ]]; then
        sed -i 's|http://ports.ubuntu.com/ubuntu-ports/|${VKP_APT_MIRROR}|; s|http://archive.ubuntu.com/ubuntu/|${VKP_APT_MIRROR}|' /etc/apt/sources.list.d/ubuntu.sources
      fi
      apt-get update -qq && apt-get install -y -qq clang cmake ninja-build git ca-certificates pkg-config liburing-dev > /dev/null
      export CPM_SOURCE_CACHE=/cache/cpm CC=clang CXX=clang++
      cmake -S /src -B /cache/build -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DVKP_SANITIZE=address,undefined \
        -DVKP_BUILD_FUZZERS=ON -DVKP_BUILD_TESTS=OFF -DVKP_BUILD_BENCHMARKS=OFF
      cmake --build /cache/build --target resp_parser_fuzz
      /cache/build/fuzz/resp_parser_fuzz ${FUZZ_ARGS[*]}
    "
fi

# Linux: build natively (clang required for -fsanitize=fuzzer).
BUILD_DIR="${REPO_ROOT}/build/fuzz"
export CC="${CC:-clang}" CXX="${CXX:-clang++}"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DVKP_SANITIZE=address,undefined \
  -DVKP_BUILD_FUZZERS=ON -DVKP_BUILD_TESTS=OFF -DVKP_BUILD_BENCHMARKS=OFF
cmake --build "${BUILD_DIR}" --target resp_parser_fuzz
cd "${REPO_ROOT}"
exec "${BUILD_DIR}/fuzz/resp_parser_fuzz" "${FUZZ_ARGS[@]}"
