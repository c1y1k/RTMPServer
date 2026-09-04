#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
module_dir="$(cd "${script_dir}/../.." && pwd)"
test_binary="/tmp/chunk_parser_tests"

cleanup() {
    rm -f "${test_binary}"
}
trap cleanup EXIT

g++ \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -pthread \
    -fsanitize=address,undefined \
    -fno-omit-frame-pointer \
    -I"${module_dir}" \
    "${script_dir}/chunk_parser_test.cpp" \
    "${module_dir}/chunk_parser.cpp" \
    -o "${test_binary}"

ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "${test_binary}"
