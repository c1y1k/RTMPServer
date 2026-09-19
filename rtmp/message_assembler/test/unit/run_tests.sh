#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
module_dir="$(cd "${script_dir}/../.." && pwd)"
test_binary="/tmp/message_assembler_tests"

cleanup() {
    rm -f "${test_binary}"
}
trap cleanup EXIT

g++ \
    -std=c++17 \
    -Wall \
    -Wextra \
    -Wpedantic \
    -fsanitize=address,undefined \
    -fno-omit-frame-pointer \
    -I"${module_dir}" \
    "${script_dir}/message_assembler_test.cpp" \
    "${module_dir}/message_assembler.cpp" \
    -o "${test_binary}"

ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "${test_binary}"
