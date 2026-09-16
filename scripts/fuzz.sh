#!/usr/bin/env bash
# Runs a libFuzzer target for N seconds (default 60, all targets by default).
#
#   ./scripts/fuzz.sh [seconds] [target ...]
#
# Targets are the names under fuzz/corpus/: resp_parser, admin_http. Each one
# builds <target>_fuzz and seeds from its own corpus directory.
#
# Apple Clang has no libFuzzer runtime, so on macOS this runs inside a Linux
# container (requires Docker). The build tree and CPM sources live in a named
# volume (vkp-fuzz-cache), so only the first run pays the setup cost. New
# corpus entries are written back to fuzz/corpus/ on the host.
set -euo pipefail

DURATION="${1:-60}"
# Not `shift; TARGETS=("$@")`: an empty "$@" under `set -u` is an error on the
# bash 3.2 that macOS still ships.
if [[ $# -gt 1 ]]; then
  shift
  TARGETS=("$@")
else
  TARGETS=(resp_parser admin_http)
fi

# Direct docker.io pulls may be blocked; override with a mirror if needed,
# e.g. VKP_FUZZ_IMAGE=docker.m.daocloud.io/library/ubuntu:24.04
# Slow apt? Point VKP_APT_MIRROR at a full mirror URL incl. trailing slash,
# e.g. VKP_APT_MIRROR=https://mirrors.aliyun.com/ubuntu-ports/ (arm64 host)
FUZZ_IMAGE="${VKP_FUZZ_IMAGE:-ubuntu:24.04}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FUZZ_FLAGS="-max_total_time=${DURATION} -rss_limit_mb=2048 -print_final_stats=1"

# Build every requested target first, then run them in turn, so a compile error
# in the second target does not surface only after a full run of the first.
# Emitted as shell text because the macOS path hands the whole script to
# `docker run` in one go.
build_and_run() {
  local build_dir="$1"
  local t
  for t in "${TARGETS[@]}"; do
    echo "cmake --build ${build_dir} --target ${t}_fuzz"
  done
  for t in "${TARGETS[@]}"; do
    echo "echo '=== ${t} ==='"
    echo "${build_dir}/fuzz/${t}_fuzz ${FUZZ_FLAGS} fuzz/corpus/${t}"
  done
}

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
      $(build_and_run /cache/build)
    "
fi

# Linux: build natively (clang required for -fsanitize=fuzzer).
BUILD_DIR="${REPO_ROOT}/build/fuzz"
export CC="${CC:-clang}" CXX="${CXX:-clang++}"
cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DVKP_SANITIZE=address,undefined \
  -DVKP_BUILD_FUZZERS=ON -DVKP_BUILD_TESTS=OFF -DVKP_BUILD_BENCHMARKS=OFF
cd "${REPO_ROOT}"
for t in "${TARGETS[@]}"; do
  cmake --build "${BUILD_DIR}" --target "${t}_fuzz"
done
for t in "${TARGETS[@]}"; do
  echo "=== ${t} ==="
  # shellcheck disable=SC2086  # FUZZ_FLAGS is deliberately word-split
  "${BUILD_DIR}/fuzz/${t}_fuzz" ${FUZZ_FLAGS} "fuzz/corpus/${t}"
done
