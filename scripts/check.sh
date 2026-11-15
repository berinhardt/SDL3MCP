#!/bin/sh
# The mechanical gate for SDL3MCP: file size, formatting, build, tests.
#
# scripts/hooks/pre-commit runs this on every commit, and scripts/install-hooks.sh
# points a fresh clone at that hook. The build tree is reused, so a commit pays
# only for what changed.
#
# Environment:
#   SDL3MCP_CHECK_BUILD_DIR  build tree, default build/check, relative to the
#                            repository root
#   CMAKE_PREFIX_PATH        forwarded to CMake so an installed doctest can be
#                            found; without one the pinned fetch resolves it
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

build_dir=${SDL3MCP_CHECK_BUILD_DIR:-build/check}

fail() {
   printf 'check: %s\n' "$1" >&2
   exit 1
}

run() {
   printf '+ %s\n' "$*" >&2
   "$@" || fail "command failed: $*"
}

require() {
   command -v "$1" >/dev/null 2>&1 || fail "$1 is not installed"
}

check_file_size() {
   # The fixture proves the rule before it runs over the tree, so a broken rule
   # fails here instead of silently passing every file.
   printf 'check: file-size fixture\n' >&2
   sh "$root/tests/check_file_size.sh"
   printf 'check: file-size rule over the product sources\n' >&2
   sh "$root/scripts/check-file-size.sh"
}

check_format() {
   require clang-format
   git rev-parse --git-dir >/dev/null 2>&1 ||
      fail "$root is not a git checkout; the format gate needs its file list"
   printf 'check: clang-format over the tracked C and C++ sources\n' >&2
   # third_party/ holds a tracked upstream header. It is not ours to reformat.
   git ls-files -- '*.c' '*.h' '*.cpp' '*.hpp' |
      while IFS= read -r file; do
         case $file in
         third_party/*) continue ;;
         esac
         if ! clang-format --dry-run --Werror "$file" >/dev/null 2>&1; then
            printf 'check: not clang-formatted: %s\n' "$file" >&2
            printf 'check: run: clang-format -i %s\n' "$file" >&2
            exit 1
         fi
      done
}

check_build() {
   require cmake
   require ctest
   set -- --preset debug -B "$build_dir"
   if [ -n "${CMAKE_PREFIX_PATH:-}" ]; then
      set -- "$@" -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH"
   fi
   printf 'check: configure, build and test in %s\n' "$build_dir" >&2
   run cmake "$@"
   run cmake --build "$build_dir" --parallel
   # The full run includes handler_only_check: the nested Handler-only
   # configure that proves the Handler half needs no SDL.
   run ctest --test-dir "$build_dir" --output-on-failure
}

check_file_size
check_format
check_build
printf 'check: ok\n'
