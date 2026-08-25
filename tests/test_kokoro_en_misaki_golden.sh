#!/usr/bin/env bash
# Copyright (C) 2025 SpacemiT Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <golden-test> <kokoro-en-model-dir> [golden.tsv]" >&2
    exit 2
fi

test_bin="$(realpath "$1")"
model_dir="$(realpath "$2")"
root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
golden="${3:-${root_dir}/tests/kokoro_en_misaki_golden.tsv}"

if [[ ! -x "${test_bin}" || ! -d "${model_dir}" || ! -f "${golden}" ]]; then
    echo "Kokoro English golden test skipped: runtime files are missing." >&2
    exit 77
fi

exec "${test_bin}" "${model_dir}" "${golden}"
