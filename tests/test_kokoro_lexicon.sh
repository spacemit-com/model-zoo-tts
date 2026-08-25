#!/usr/bin/env bash
#
# Copyright (C) 2025 SpacemiT Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -lt 5 ]]; then
    echo "Usage: $0 <tts_file_demo> <en-model> <en-voice> <zh-model> <zh-voice>" >&2
    exit 2
fi

for required in "$@"; do
    if [[ ! -e "${required}" ]]; then
        echo "Kokoro lexicon test skipped: runtime asset is not installed." >&2
        exit 77
    fi
done

demo="$(realpath "$1")"
en_package="$(dirname "$(realpath "$2")")"
zh_package="$(dirname "$(realpath "$4")")"
work_dir="$(mktemp -d "${TMPDIR:-/tmp}/kokoro-lexicon.XXXXXX")"
trap 'rm -rf "${work_dir}"' EXIT
test_home="${work_dir}/home"
model_root="${test_home}/.cache/models/tts/kokoro-tts"
mkdir -p "${model_root}" "${test_home}/.cache"
ln -s "${en_package}" "${model_root}/kokoro-v1.0-en"
ln -s "${zh_package}" "${model_root}/kokoro-v1.1-zh"
if [[ -d "${HOME}/.cache/thirdparty" ]]; then
    ln -s "${HOME}/.cache/thirdparty" "${test_home}/.cache/thirdparty"
fi

HOME="${test_home}" \
KOKORO_DUMP_PHONEMES="${work_dir}/en-reference.txt" \
SPACEMIT_TTS_WARMUP_RUNS=0 \
"${demo}" --provider cpu -l kokoro:en -p "space meet" \
    -o "${work_dir}/en-reference.wav" >/dev/null 2>&1

HOME="${test_home}" \
KOKORO_DUMP_PHONEMES="${work_dir}/en-custom.txt" \
SPACEMIT_TTS_WARMUP_RUNS=0 \
"${demo}" --provider cpu -l kokoro:en -p "codexlex" \
    --lexicon "codexlex:space meet:en" \
    -o "${work_dir}/en-custom.wav" >/dev/null 2>&1

if ! diff -u "${work_dir}/en-reference.txt" "${work_dir}/en-custom.txt"; then
    echo "Kokoro English custom lexicon did not replace the pronunciation." >&2
    exit 1
fi

HOME="${test_home}" \
KOKORO_DUMP_TOKENS="${work_dir}/mixed-reference.ids" \
SPACEMIT_TTS_WARMUP_RUNS=0 \
"${demo}" --provider cpu -l kokoro:zh -p "space meet" \
    -o "${work_dir}/mixed-reference.wav" >/dev/null 2>&1 || true

HOME="${test_home}" \
KOKORO_DUMP_TOKENS="${work_dir}/mixed-custom.ids" \
SPACEMIT_TTS_WARMUP_RUNS=0 \
"${demo}" --provider cpu -l kokoro:zh -p "codexlex" \
    --lexicon "codexlex:space meet:en" \
    -o "${work_dir}/mixed-custom.wav" >/dev/null 2>&1 || true

if [[ ! -s "${work_dir}/mixed-reference.ids" ||
      ! -s "${work_dir}/mixed-custom.ids" ]]; then
    echo "Kokoro mixed-English token dump was not produced." >&2
    exit 1
fi

if ! diff -u \
    "${work_dir}/mixed-reference.ids" "${work_dir}/mixed-custom.ids"; then
    echo "Kokoro mixed-English custom lexicon did not replace pronunciation." >&2
    exit 1
fi

HOME="${test_home}" \
KOKORO_DUMP_TOKENS="${work_dir}/zh-custom.ids" \
SPACEMIT_TTS_WARMUP_RUNS=0 \
"${demo}" --provider cpu -l kokoro:zh -p "为你" \
    --lexicon "为你:wei1 ni3" \
    -o "${work_dir}/zh-custom.wav" >/dev/null 2>&1 || true

if [[ ! -s "${work_dir}/zh-custom.ids" ]]; then
    echo "Kokoro Chinese token dump was not produced." >&2
    exit 1
fi

zh_tokenizer="${zh_package}/tokens.txt"
if [[ ! -f "${zh_tokenizer}" ]]; then
    zh_tokenizer="${zh_package}/tokenizer.json"
fi
if [[ ! -f "${zh_tokenizer}" ]]; then
    echo "Kokoro Chinese tokenizer asset was not found." >&2
    exit 1
fi

python3 - "${zh_tokenizer}" "${work_dir}/zh-custom.ids" <<'PY'
import json
import pathlib
import sys

token_path = pathlib.Path(sys.argv[1])
ids_path = pathlib.Path(sys.argv[2])
if token_path.suffix == ".json":
    token_to_id = json.loads(token_path.read_text(encoding="utf-8"))
    id_to_token = {int(token_id): token for token, token_id in token_to_id.items()}
else:
    id_to_token = {}
    for line in token_path.read_text(encoding="utf-8").splitlines():
        token, token_id = line.rsplit(" ", 1)
        id_to_token[int(token_id)] = token
ids = [int(value) for value in ids_path.read_text().split()]
actual = "".join(id_to_token[token_id] for token_id in ids[1:-1])
# Use a deliberately non-default tone to prove the custom entry is applied.
if actual != "为1ㄋㄧ3":
    raise SystemExit(
        "Kokoro Chinese custom lexicon mismatch: "
        f"expected 为1ㄋㄧ3, got {actual}")
PY

echo "Kokoro custom lexicon: English, mixed-English and Chinese passed."
