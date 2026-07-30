#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "Usage: $0 <tts_file_demo> <kokoro-v1.0-en.q.onnx> <voice.bin>" >&2
  exit 2
fi
if [[ ! -x "$1" || ! -f "$2" || ! -f "$3" ]]; then
  echo "Kokoro long-text test skipped: runtime model is not installed." >&2
  exit 77
fi

demo="$(realpath "$1")"
model="$(realpath "$2")"
voice="$(realpath "$3")"
package_dir="$(dirname "${model}")"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/kokoro-long-text.XXXXXX")"
trap 'rm -rf "${work_dir}"' EXIT
test_home="${work_dir}/home"
mkdir -p "${test_home}/.cache/models/tts/kokoro-tts" "${test_home}/.cache"
ln -s "${package_dir}" \
  "${test_home}/.cache/models/tts/kokoro-tts/kokoro-v1.0-en"
if [[ -d "${HOME}/.cache/thirdparty" ]]; then
  ln -s "${HOME}/.cache/thirdparty" "${test_home}/.cache/thirdparty"
fi
if [[ "$(dirname "$(dirname "${voice}")")" != "${package_dir}" ]]; then
  echo "Kokoro English voice does not belong to the supplied model package." >&2
  exit 1
fi

text=""
for i in $(seq 1 6); do
  text="${text}This is Kokoro long text segment number ${i}, with CPU, K3, punctuation, and 3 cores. "
done

HOME="${test_home}" \
KOKORO_DUMP_TOKENS="${work_dir}/full.txt" \
KOKORO_DUMP_CHUNKS="${work_dir}/chunks.txt" \
SPACEMIT_TTS_WARMUP_RUNS=0 \
"${demo}" --provider spacemit -l kokoro:en -p "${text}" \
  -o "${work_dir}/unused.wav" >"${work_dir}/run.log" 2>&1 || true

if [[ ! -s "${work_dir}/full.txt" ]]; then
  echo "Kokoro long input did not produce a token sequence." >&2
  exit 1
fi

full_count="$(wc -w <"${work_dir}/full.txt")"
if (( full_count <= 512 )); then
  echo "Kokoro long input did not exceed the legacy 512-token boundary." >&2
  exit 1
fi

chunk_lines="$(wc -l <"${work_dir}/chunks.txt")"
if (( chunk_lines <= 1 )); then
  echo "Kokoro long input was not split at punctuation boundaries." >&2
  exit 1
fi

while IFS= read -r line; do
  count="$(wc -w <<<"${line}")"
  if (( count > 128 || count <= 2 )); then
    echo "Kokoro emitted an invalid warmup-shape chunk: ${count}" >&2
    exit 1
  fi
done <"${work_dir}/chunks.txt"

KOKORO_DUMP_TOKENS="${work_dir}/cpu_full.txt" \
KOKORO_DUMP_CHUNKS="${work_dir}/cpu_chunks.txt" \
SPACEMIT_TTS_WARMUP_RUNS=0 \
"${demo}" --provider cpu -l kokoro:en -p "${text}" \
  -o "${work_dir}/unused_cpu.wav" >"${work_dir}/cpu_run.log" 2>&1 || true

found_cpu_chunk_above_ep_limit=false
while IFS= read -r line; do
  count="$(wc -w <<<"${line}")"
  if (( count > 512 || count <= 2 )); then
    echo "Kokoro CPU emitted an invalid model-limit chunk: ${count}" >&2
    exit 1
  fi
  if (( count > 128 )); then
    found_cpu_chunk_above_ep_limit=true
  fi
done <"${work_dir}/cpu_chunks.txt"
if [[ "${found_cpu_chunk_above_ep_limit}" != true ]]; then
  echo "Kokoro CPU incorrectly retained the 128-token EP shape limit." >&2
  exit 1
fi

echo "Kokoro long text: ${full_count} tokens use 128-token EP and 512-token CPU limits."
