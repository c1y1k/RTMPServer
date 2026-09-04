#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
module_dir="$(cd "${script_dir}/../.." && pwd)"
work_dir="$(mktemp -d /tmp/rtmp-chunk-capture.XXXXXX)"
trap 'rm -rf "${work_dir}"' EXIT

python3 "${script_dir}/prepare_real_capture.py" \
    --capture "${script_dir}/rtmp_capture.pcapng" \
    --wireshark-json "${script_dir}/wireshark_rtmp_obs_srs.json" \
    --stream-output "${work_dir}/client_rtmp.bin" \
    --reference-output "${work_dir}/wireshark_headers.tsv" \
    --frame-map-output "${work_dir}/frame_map.tsv"

g++ -std=c++17 -Wall -Wextra -Wpedantic \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    "${module_dir}/chunk_parser.cpp" \
    "${script_dir}/real_capture_test.cpp" \
    -o "${work_dir}/real_capture_test"

ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" \
    "${work_dir}/real_capture_test" \
    "${work_dir}/client_rtmp.bin" \
    "${work_dir}/wireshark_headers.tsv" \
    "${work_dir}/frame_map.tsv" \
    "${script_dir}/real_capture_comparison.md"
