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
        KOKORO_EN = "kokoro_en"
        KOKORO_ZH = "kokoro_zh"

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
            self.voice = "default"
            self.num_threads = 2
            self.enable_warmup = True
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
            return [
                "matcha_zh",
                "matcha_en",
                "matcha_zh_en",
                "kokoro",
                "kokoro_en",
                "kokoro_zh",
            ]

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


def test_config_selects_backend_specific_default_model_root(monkeypatch):
    engine_module = _load_engine_module(monkeypatch)
    cache_root = Path.home() / ".cache" / "models" / "tts"

    matcha = engine_module.Config(engine_module.BackendType.MATCHA_EN)
    kokoro_en = engine_module.Config(engine_module.BackendType.KOKORO_EN)
    kokoro_zh = engine_module.Config(engine_module.BackendType.KOKORO_ZH)

    assert Path(matcha.model_dir) == cache_root / "matcha-tts"
    assert Path(kokoro_en.model_dir) == cache_root / "kokoro-tts"
    assert Path(kokoro_zh.model_dir) == cache_root / "kokoro-tts"
    assert matcha.num_threads == 2
    assert kokoro_en.num_threads == 4
    assert kokoro_zh.num_threads == 4
    assert matcha.provider == "auto"
    assert {
        "kokoro",
        "kokoro_en",
        "kokoro_zh",
    }.issubset(engine_module.Config.available_presets())


def test_config_preserves_explicit_model_root(monkeypatch, tmp_path):
    engine_module = _load_engine_module(monkeypatch)
    config = engine_module.Config(
        engine_module.BackendType.KOKORO_EN,
        model_dir=str(tmp_path),
    )

    assert Path(config.model_dir) == tmp_path
