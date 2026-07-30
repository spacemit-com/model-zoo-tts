#!/usr/bin/env bash
#
# Copyright (C) 2025 SpacemiT Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

if [[ $# -lt 1 || ! -x "$1" ]]; then
    echo "Usage: $0 <tts_file_demo>" >&2
    exit 2
fi

help="$("$1" --help)"
grep -Fq "kokoro         Kokoro" <<<"${help}"
grep -Fq "kokoro:en" <<<"${help}"
grep -Fq "kokoro:zh" <<<"${help}"
grep -Fq -- "--voice <voice>" <<<"${help}"

voices="$("$1" --list-voices)"
grep -Fq "kokoro:en=af_heart" <<<"${voices}"
grep -Fq "kokoro:zh=zf_001" <<<"${voices}"
grep -Fq -- "--voice <voice>" <<<"${voices}"

if "$1" -l matcha:en --voice af_heart -p test >/dev/null 2>&1; then
    echo "--voice was incorrectly accepted for a Matcha backend." >&2
    exit 1
fi

echo "Kokoro CLI contract: aliases, language variants and --voice passed."
