#!/usr/bin/env bash
#
# Copyright (C) 2025 SpacemiT Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEMO="${1:-${ROOT_DIR}/build/bin/tts_file_demo}"
GOLDEN="${2:-${ROOT_DIR}/tests/kokoro_zh_misaki_golden.tsv}"
MODEL_DIR="${KOKORO_ZH_MODEL_DIR:-${HOME}/.cache/models/tts/kokoro-tts/kokoro-v1.1-zh}"
TOKENS="${MODEL_DIR}/tokens.txt"
if [[ ! -f "${TOKENS}" ]]; then
    TOKENS="${MODEL_DIR}/tokenizer.json"
fi

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

input_file="${TMP_DIR}/input.txt"
all_ids_file="${TMP_DIR}/actual.ids"
awk -F '\t' '!/^#/ && NF >= 2 {print $1}' "${GOLDEN}" > "${input_file}"
printf 'q\n' >> "${input_file}"

SPACEMIT_TTS_WARMUP_RUNS=0 \
KOKORO_DUMP_TOKENS="${all_ids_file}" \
    "${DEMO}" --provider cpu -l kokoro:zh \
    -o "${TMP_DIR}/unused.wav" < "${input_file}" >/dev/null 2>&1

case_index=0
exec 3< "${all_ids_file}"
while IFS=$'\t' read -r text expected; do
    [[ -z "${text}" || "${text}" == \#* ]] && continue
    case_index=$((case_index + 1))
    ids_file="${TMP_DIR}/case-${case_index}.ids"
    IFS= read -r ids_line <&3 || ids_line=""
    printf '%s\n' "${ids_line}" > "${ids_file}"
    if [[ ! -s "${ids_file}" ]]; then
        echo "Kokoro token dump failed for: ${text}" >&2
        exit 1
    fi

    actual="$(
        python3 - "${TOKENS}" "${ids_file}" <<'PY'
import pathlib
import json
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
