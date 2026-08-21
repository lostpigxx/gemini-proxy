#!/usr/bin/env bash
# Format all tracked C++ sources in place. Pass --check to verify without modifying.
set -euo pipefail
cd "$(dirname "$0")/.."

mode_args=(-i)
if [[ "${1:-}" == "--check" ]]; then
  mode_args=(--dry-run --Werror)
fi

git ls-files '*.cpp' '*.hpp' | xargs clang-format "${mode_args[@]}"
