#!/usr/bin/env python3
"""Generate Chinese dictionary golden data with official Misaki 1.1."""

import argparse
import sys
from pathlib import Path


CASES = [
    "开户行和发卡行",
    "茧行的行号",
    "各地时间为准",
    "色差和掺和",
    "嗲，呗，不，咗，嘞。",
    "嗯。",
]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--misaki-source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    sys.path.insert(0, str(args.misaki_source))
    from misaki.zh import ZHG2P  # pylint: disable=import-outside-toplevel

    g2p = ZHG2P(version="1.1", en_callable=lambda text: text)
    lines = ["# Generated from hexgrad/misaki ZHG2P version 1.1."]
    for text in CASES:
        phonemes, _ = g2p(text)
        lines.append(f"{text}\t{phonemes}")
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
