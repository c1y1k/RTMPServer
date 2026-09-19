#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

bash "${script_dir}/handshake/run_tests.sh"
bash "${script_dir}/orchestration/run_tests.sh"
