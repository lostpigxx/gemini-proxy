#!/usr/bin/env bash
# Format all tracked C++ sources in place. Pass --check to verify without modifying.
#
# clang-format output differs between major versions, so the version is pinned here and
# CI installs the same one. Install locally with:
#   python3 -m pip install --user "clang-format==${REQUIRED_VERSION}"
# or point CLANG_FORMAT at a matching binary.
set -euo pipefail
cd "$(dirname "$0")/.."

REQUIRED_MAJOR=20
REQUIRED_VERSION=20.1.8

find_clang_format() {
  if [[ -n "${CLANG_FORMAT:-}" ]]; then
    echo "${CLANG_FORMAT}"
    return
  fi
  for candidate in "clang-format-${REQUIRED_MAJOR}" clang-format \
                   "${HOME}/Library/Python/"*/bin/clang-format "${HOME}/.local/bin/clang-format"; do
    if command -v "${candidate}" >/dev/null 2>&1; then
      echo "${candidate}"
      return
    fi
  done
  return 1
}

if ! cf=$(find_clang_format); then
  echo "error: clang-format not found." >&2
  echo "  install: python3 -m pip install --user \"clang-format==${REQUIRED_VERSION}\"" >&2
  echo "  or set CLANG_FORMAT=/path/to/clang-format" >&2
  exit 1
fi

version=$("${cf}" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
if [[ "${version%%.*}" != "${REQUIRED_MAJOR}" ]]; then
  echo "error: ${cf} is version ${version}, but this project pins clang-format ${REQUIRED_MAJOR}.x." >&2
  echo "  formatting differs across major versions and CI would reject the result." >&2
  echo "  install: python3 -m pip install --user \"clang-format==${REQUIRED_VERSION}\"" >&2
  exit 1
fi

mode_args=(-i)
if [[ "${1:-}" == "--check" ]]; then
  mode_args=(--dry-run --Werror)
fi

git ls-files '*.cpp' '*.hpp' | xargs "${cf}" "${mode_args[@]}"
