#!/usr/bin/env python3
# Copyright (C) 2025 SpacemiT Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

"""Exercise Kokoro custom lexicons through the public Python API."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import sys
import tempfile

from spacemit_tts import BackendType, Config, Engine


def _read_dump(path: Path) -> str:
    if not path.is_file():
        raise RuntimeError(f"Kokoro diagnostic dump was not created: {path}")
    return path.read_text(encoding="utf-8").strip()


def _synthesize_dump(
    engine: Engine, text: str, variable: str, path: Path
) -> str:
    path.unlink(missing_ok=True)
    os.environ[variable] = str(path)
    try:
        engine.synthesize(text)
    finally:
        os.environ.pop(variable, None)
    return _read_dump(path)


def _make_engine(backend: BackendType, model_root: Path) -> Engine:
    config = Config(backend, model_dir=str(model_root), provider="cpu")
    config.enable_warmup = False
    return Engine(config)


def _verify_english(model_root: Path, work_dir: Path) -> None:
    with _make_engine(BackendType.KOKORO_EN, model_root) as engine:
        reference = _synthesize_dump(
            engine,
            "space meet",
            "KOKORO_DUMP_PHONEMES",
            work_dir / "en-reference.txt",
        )
        engine.update_lexicon(
            [{"word": "codexlex", "phoneme": "space meet", "locale": "en"}]
        )
        custom = _synthesize_dump(
            engine,
            "codexlex",
            "KOKORO_DUMP_PHONEMES",
            work_dir / "en-custom.txt",
        )
        if custom != reference:
            raise RuntimeError("Python Kokoro English lexicon was not applied")


def _verify_chinese(model_root: Path, work_dir: Path) -> None:
    with _make_engine(BackendType.KOKORO_ZH, model_root) as engine:
        mixed_reference = _synthesize_dump(
            engine,
            "space meet",
            "KOKORO_DUMP_TOKENS",
            work_dir / "mixed-reference.ids",
        )
        engine.update_lexicon(
            [{"word": "codexlex", "phoneme": "space meet", "locale": "en"}]
        )
        mixed_custom = _synthesize_dump(
            engine,
            "codexlex",
            "KOKORO_DUMP_TOKENS",
            work_dir / "mixed-custom.ids",
        )
        if mixed_custom != mixed_reference:
            raise RuntimeError("Python Kokoro mixed-English lexicon was not applied")

        default_zh = _synthesize_dump(
            engine,
            "为你",
            "KOKORO_DUMP_TOKENS",
            work_dir / "zh-default.ids",
        )
        engine.update_lexicon([{"word": "为你", "phoneme": "wei1 ni3"}])
        custom_zh = _synthesize_dump(
            engine,
            "为你",
            "KOKORO_DUMP_TOKENS",
            work_dir / "zh-custom.ids",
        )
        if custom_zh == default_zh:
            raise RuntimeError("Python Kokoro Chinese lexicon was not applied")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--model-root",
        type=Path,
        default=Path.home() / ".cache/models/tts/kokoro-tts",
    )
    args = parser.parse_args()

    required = (
        args.model_root / "kokoro-v1.0-en",
        args.model_root / "kokoro-v1.1-zh",
    )
    if not all(path.is_dir() for path in required):
        print(
            "Kokoro Python lexicon test skipped: runtime assets are missing.",
            file=sys.stderr,
        )
        return 77

    os.environ["SPACEMIT_TTS_WARMUP_RUNS"] = "0"
    with tempfile.TemporaryDirectory(prefix="kokoro-python-lexicon.") as temp:
        work_dir = Path(temp)
        _verify_english(args.model_root, work_dir)
        _verify_chinese(args.model_root, work_dir)

    print("Kokoro Python lexicon: English, mixed-English and Chinese passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
