#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "Usage: $0 <tts_file_demo> <kokoro-v1.1-zh.q.onnx> <voice.npy>" >&2
  exit 2
fi

if [[ ! -x "$1" || ! -f "$2" || ! -f "$3" ]]; then
  echo "Kokoro Chinese frontend test skipped: runtime model is not installed." >&2
  exit 77
fi

demo="$(realpath "$1")"
model="$(realpath "$2")"
voice="$(realpath "$3")"
package_dir="$(dirname "${model}")"
test_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/kokoro-zh-golden.XXXXXX")"
trap 'rm -rf "${work_dir}"' EXIT
test_home="${work_dir}/home"
mkdir -p "${test_home}/.cache/models/tts/kokoro-tts" "${test_home}/.cache"
ln -s "${package_dir}" \
  "${test_home}/.cache/models/tts/kokoro-tts/kokoro-v1.1-zh"
if [[ -d "${HOME}/.cache/thirdparty" ]]; then
  ln -s "${HOME}/.cache/thirdparty" "${test_home}/.cache/thirdparty"
fi
if [[ "$(dirname "$(dirname "${voice}")")" != "${package_dir}" ]]; then
  echo "Kokoro Chinese voice does not belong to the supplied model package." >&2
  exit 1
fi

cut -f1 "${test_dir}/kokoro_zh_golden.tsv" >"${work_dir}/input.txt"
printf 'q\n' >>"${work_dir}/input.txt"
cut -f2 "${test_dir}/kokoro_zh_golden.tsv" >"${work_dir}/expected.txt"

HOME="${test_home}" \
KOKORO_DUMP_TOKENS="${work_dir}/actual.txt" \
SPACEMIT_TTS_WARMUP_RUNS=0 \
"${demo}" --provider cpu -l kokoro:zh -o "${work_dir}/unused.wav" \
  <"${work_dir}/input.txt" >"${work_dir}/run.log" 2>&1

if ! diff -u "${work_dir}/expected.txt" "${work_dir}/actual.txt"; then
  echo "Kokoro Chinese frontend differs from its Misaki/model golden set." >&2
  exit 1
fi

echo "Kokoro Chinese frontend: 20/20 Misaki/model cases matched."
