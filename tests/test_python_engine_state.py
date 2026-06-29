from __future__ import annotations

import importlib.util
import sys
import types
from pathlib import Path


def _load_engine_module(monkeypatch):
    root = Path(__file__).resolve().parents[1]
    package = types.ModuleType("spacemit_tts")
    package.__path__ = [str(root / "python" / "spacemit_tts")]

    class NativeBackendType:
        MATCHA_ZH = "matcha_zh"
        MATCHA_EN = "matcha_en"
        MATCHA_ZH_EN = "matcha_zh_en"
        COSYVOICE = "cosyvoice"
        VITS = "vits"
        PIPER = "piper"
        KOKORO = "kokoro"

    class NativeAudioFormat:
        PCM = "pcm"
        WAV = "wav"
        MP3 = "mp3"
        OGG = "ogg"

    class FakeTtsConfig:
        def __init__(self):
            self.backend = None
            self.model_dir = ""
            self.provider = "auto"
            self.sample_rate = 16000
            self.speech_rate = 1.0
            self.volume = 100
            self.speaker_id = 0
            self.pitch = 1.0

        @staticmethod
        def preset(name):
            config = FakeTtsConfig()
            config.backend = name
            return config

        @staticmethod
        def available_presets():
            return ["matcha_zh", "matcha_en", "matcha_zh_en", "kokoro"]

    class FakeNativeEngine:
        def __init__(self, config):
            self.config = config
            self.initialized = True
            self.shutdown_calls = 0

        def shutdown(self):
            self.shutdown_calls += 1
            self.initialized = False

        def is_initialized(self):
            return self.initialized

    fake_tts = types.SimpleNamespace(
        BackendType=NativeBackendType,
        AudioFormat=NativeAudioFormat,
        TtsConfig=FakeTtsConfig,
        TtsEngine=FakeNativeEngine,
    )
    monkeypatch.setitem(sys.modules, "spacemit_tts", package)
    monkeypatch.setitem(sys.modules, "spacemit_tts._spacemit_tts", fake_tts)

    spec = importlib.util.spec_from_file_location(
        "spacemit_tts.engine",
        root / "python" / "spacemit_tts" / "engine.py",
    )
    module = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, "spacemit_tts.engine", module)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_engine_is_initialized_returns_false_after_close(monkeypatch):
    engine_module = _load_engine_module(monkeypatch)

    engine = engine_module.Engine()
    assert engine.is_initialized is True

    native_engine = engine._engine
    engine.close()

    assert native_engine.shutdown_calls == 1
    assert engine.is_initialized is False
    engine.close()
    assert engine.is_initialized is False
