#!/usr/bin/env bash
set -euo pipefail

module_dir="components/model_zoo/tts"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/tts-pr-test.XXXXXX")"
trap 'rm -rf "${build_dir}"' EXIT

g++ -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter \
  "${module_dir}/tests/tts_pr_contract_test.cpp" \
  "${module_dir}/src/tts_presets.cpp" \
  -I"${module_dir}/include" \
  -I"${module_dir}/src" \
  -o "${build_dir}/tts_pr_contract_test"

"${build_dir}/tts_pr_contract_test" --config-contract
