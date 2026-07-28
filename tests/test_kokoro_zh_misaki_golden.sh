#!/usr/bin/env bash
#
# Copyright (C) 2025 SpacemiT Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEMO="${1:-${ROOT_DIR}/build/bin/tts_file_demo}"
GOLDEN="${ROOT_DIR}/tests/kokoro_zh_misaki_golden.tsv"
MODEL_DIR="${KOKORO_ZH_MODEL_DIR:-${HOME}/.cache/models/tts/kokoro-tts/kokoro-v1.1-zh}"
TOKENS="${MODEL_DIR}/tokens.txt"

if [[ ! -x "${DEMO}" ]]; then
    echo "Missing tts_file_demo: ${DEMO}" >&2
    exit 2
fi
if [[ ! -f "${TOKENS}" ]]; then
    echo "Missing Kokoro Chinese tokens: ${TOKENS}" >&2
    exit 2
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

case_index=0
while IFS=$'\t' read -r text expected; do
    [[ -z "${text}" || "${text}" == \#* ]] && continue
    case_index=$((case_index + 1))
    ids_file="${TMP_DIR}/case-${case_index}.ids"
    output_file="${TMP_DIR}/case-${case_index}.wav"

    SPACEMIT_TTS_WARMUP_RUNS=0 \
    KOKORO_DUMP_TOKENS="${ids_file}" \
        "${DEMO}" --provider cpu -l kokoro:zh \
        -p "${text}" -o "${output_file}" >/dev/null 2>&1 || true
    if [[ ! -s "${ids_file}" ]]; then
        echo "Kokoro token dump failed for: ${text}" >&2
        exit 1
    fi

    actual="$(
        python3 - "${TOKENS}" "${ids_file}" <<'PY'
import pathlib
import sys

token_path = pathlib.Path(sys.argv[1])
ids_path = pathlib.Path(sys.argv[2])
id_to_token = {}
for line in token_path.read_text(encoding="utf-8").splitlines():
    token, token_id = line.rsplit(" ", 1)
    id_to_token[int(token_id)] = token
ids = [int(value) for value in ids_path.read_text().split()]
print("".join(id_to_token[token_id] for token_id in ids[1:-1]), end="")
PY
    )"

    if [[ "${actual}" != "${expected}" ]]; then
        echo "Misaki golden mismatch for: ${text}" >&2
        echo "expected: ${expected}" >&2
        echo "actual:   ${actual}" >&2
        exit 1
    fi
    echo "PASS: ${text}"
done < "${GOLDEN}"

echo "Kokoro Chinese Misaki golden cases passed: ${case_index}"
