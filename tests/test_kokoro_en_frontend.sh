#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "Usage: $0 <tts_file_demo> <kokoro-v1.0-en.q.onnx> <voice.bin>" >&2
  exit 2
fi

if [[ ! -x "$1" || ! -f "$2" || ! -f "$3" ]]; then
  echo "Kokoro English frontend test skipped: runtime model is not installed." >&2
  exit 77
fi

demo="$(realpath "$1")"
model="$(realpath "$2")"
voice="$(realpath "$3")"
package_dir="$(dirname "${model}")"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/kokoro-en-frontend.XXXXXX")"
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

cat >"${work_dir}/input.txt" <<'EOF'
This is a punctuation test: CPU, K3, and 3.14!
I have 3 CPUs, and each CPU has one core.
SpaceMIT develops efficient AI systems.
The year 2026 has four digits.
Don't truncate single words or acronyms.
q
EOF

HOME="${test_home}" \
KOKORO_DUMP_TOKENS="${work_dir}/actual.txt" \
SPACEMIT_TTS_WARMUP_RUNS=0 \
"${demo}" --provider cpu -l kokoro:en -o "${work_dir}/unused.wav" \
  <"${work_dir}/input.txt" >"${work_dir}/run.log" 2>&1

if [[ "$(wc -l <"${work_dir}/actual.txt")" -ne 5 ]]; then
  echo "Kokoro English frontend did not produce tokens for every case." >&2
  exit 1
fi
if grep -Eq '^(0[[:space:]]+0|[[:space:]]*)$' "${work_dir}/actual.txt"; then
  echo "Kokoro English frontend produced an empty effective token sequence." >&2
  exit 1
fi

echo "Kokoro English frontend: numbers, punctuation, words and acronyms passed."
