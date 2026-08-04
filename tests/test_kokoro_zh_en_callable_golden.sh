#!/usr/bin/env bash
#
# Copyright (C) 2025 SpacemiT Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEMO="${1:-${ROOT_DIR}/build/bin/tts_file_demo}"
GOLDEN="${ROOT_DIR}/tests/kokoro_zh_en_callable_golden.tsv"
MODEL_DIR="${KOKORO_ZH_MODEL_DIR:-${HOME}/.cache/models/tts/kokoro-tts/kokoro-v1.1-zh}"
TOKENS="${MODEL_DIR}/tokens.txt"
if [[ ! -f "${TOKENS}" ]]; then
    TOKENS="${MODEL_DIR}/tokenizer.json"
fi

if [[ ! -x "${DEMO}" || ! -f "${TOKENS}" ]]; then
    echo "Missing tts_file_demo or Kokoro Chinese tokens" >&2
    exit 2
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

case_index=0
while IFS=$'\t' read -r text expected_english; do
    [[ -z "${text}" || "${text}" == \#* ]] && continue
    case_index=$((case_index + 1))
    ids_file="${TMP_DIR}/case-${case_index}.ids"

    SPACEMIT_TTS_WARMUP_RUNS=0 \
    KOKORO_DUMP_TOKENS="${ids_file}" \
        "${DEMO}" --provider cpu -l kokoro:zh \
        -p "${text}" -o "${TMP_DIR}/case-${case_index}.wav" \
        >/dev/null 2>&1 || true
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

    if [[ "${actual}" != *"${expected_english}"* ]]; then
        echo "Misaki en_callable mismatch for: ${text}" >&2
        echo "expected English span: ${expected_english}" >&2
        echo "actual mixed output:  ${actual}" >&2
        exit 1
    fi
    echo "PASS: ${text}"
done < "${GOLDEN}"

echo "Kokoro Chinese en_callable golden cases passed: ${case_index}"
