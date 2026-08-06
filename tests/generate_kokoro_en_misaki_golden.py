#!/usr/bin/env python3
"""Generate Kokoro English frontend golden data with official Misaki."""

import argparse
import json
import sys
import types
from pathlib import Path


CASES = [
    "a test.",
    "Plan A works.",
    "an engine.",
    "the engine.",
    "the test.",
    "to eat.",
    "to run.",
    "I am ready.",
    "AM radio.",
    "in action.",
    "IN is an abbreviation.",
    "This system works.",
    "I record audio.",
    "This is a record.",
    "The project works.",
    "We project growth.",
    "I used to test it.",
    "The used device works.",
    "English speech engine.",
    "with a longer sentence.",
    "The stories, buses, and cats arrived.",
    "We tested, packed, and waited.",
    "Testing, making, and running are useful.",
    "Travel by train.",
    "CPU vs. GPU.",
    "Use 25% CPU & 3+4 @ home.",
    "The U.S. project works.",
    "The 21st test costs $12.50.",
    "Dr. Smith has 1,234 apples.",
    "Prof. Jones bought 5 dollars and 6 euros.",
    "NASA's project works.",
    "James' record works.",
    "Version 3.14 works.",
    "Room 101 is ready.",
    "The 1st, 2nd, and 3rd tests passed.",
    "The 100th and 101st tests passed.",
    "The 102nd, 103rd, 105th, 108th, and 109th tests passed.",
    "The 112th, 120th, and 121st tests passed.",
    "The 1000th and 1001st checks passed.",
    "Use $1 and $0.01.",
    "HELLO there.",
    "Read a book.",
    "I read it yesterday.",
    "Live music works.",
    "We live here.",
]


class GoldenFallback:
    """Keep the fixture independent of Misaki's neural OOV fallback."""

    def __call__(self, _token):
        return "?", 0


def load_g2p(misaki_source: Path):
    """Import official Misaki without loading its optional transformer model."""
    transformers = types.ModuleType("transformers")
    transformers.BartForConditionalGeneration = object
    sys.modules["transformers"] = transformers
    sys.modules["torch"] = types.ModuleType("torch")
    sys.path.insert(0, str(misaki_source))
    from misaki.en import G2P  # pylint: disable=import-outside-toplevel

    return G2P(version="1.0", british=False, fallback=GoldenFallback())


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--misaki-source", required=True, type=Path)
    parser.add_argument("--kokoro-config", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    g2p = load_g2p(args.misaki_source)
    with args.kokoro_config.open(encoding="utf-8") as stream:
        vocab = json.load(stream)["vocab"]

    lines = [
        "# Generated from hexgrad/misaki en.py with neural fallback disabled."
    ]
    for text in CASES:
        phonemes, _ = g2p(text)
        token_ids = [0, *(vocab[c] for c in phonemes if c in vocab), 0]
        lines.append(
            "\t".join(
                [text, phonemes, " ".join(str(token) for token in token_ids)]
            )
        )
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
