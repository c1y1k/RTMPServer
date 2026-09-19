#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rtmp_dir="$(cd "${script_dir}/../.." && pwd)"
mode="${1:-all}"
benchmark_iterations="${2:-1000}"
correctness_binary="/tmp/rtmp_handshake_tests_$$"
benchmark_binary="/tmp/rtmp_handshake_benchmark_$$"

cleanup() {
    rm -f "${correctness_binary}" "${benchmark_binary}"
}
trap cleanup EXIT

compile_common=(
    -std=c++17
    -Wall
    -Wextra
    -Wpedantic
    -I"${rtmp_dir}"
    -I"${rtmp_dir}/chunk_parser"
    -I"${rtmp_dir}/message_assembler"
    "${rtmp_dir}/rtmp.cpp"
    "${rtmp_dir}/chunk_parser/chunk_parser.cpp"
    "${rtmp_dir}/message_assembler/message_assembler.cpp"
)

run_correctness() {
    g++ \
        "${compile_common[@]}" \
        -fsanitize=address,undefined \
        -fno-omit-frame-pointer \
        "${script_dir}/handshake_test.cpp" \
        -o "${correctness_binary}"

    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "${correctness_binary}"
}

run_benchmark() {
    g++ \
        "${compile_common[@]}" \
        -O2 \
        -DNDEBUG \
        "${script_dir}/handshake_benchmark.cpp" \
        -o "${benchmark_binary}"

    "${benchmark_binary}" "${benchmark_iterations}"
}

case "${mode}" in
    all)
        run_correctness
        run_benchmark
        ;;
    correctness)
        run_correctness
        ;;
    benchmark)
        run_benchmark
        ;;
    *)
        echo "usage: $0 [all|correctness|benchmark] [benchmark_iterations]" >&2
        exit 2
        ;;
esac
