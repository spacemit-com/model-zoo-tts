/*
 * Copyright (C) 2026 SpacemiT (Hangzhou) Technology Co. Ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "backends/tts_backend.hpp"
#include "tts_config.hpp"
#include "tts_service.h"
#include "tts_types.hpp"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "ASSERTION FAILED: " << message << std::endl;
        std::exit(1);
    }
}

bool contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool near(float actual, float expected, float tolerance = 1e-5f) {
    return std::fabs(actual - expected) <= tolerance;
}

class SaveOnlyBackend : public tts::ITtsBackend {
public:
    tts::ErrorInfo initialize(const tts::TtsConfig& config) override {
        config_ = config;
        initialized_ = true;
        return tts::ErrorInfo::ok();
    }

    void shutdown() override { initialized_ = false; }
    bool isInitialized() const override { return initialized_; }
    tts::BackendType getType() const override { return tts::BackendType::CUSTOM; }
    std::string getName() const override { return "save-only"; }
    std::string getVersion() const override { return "test"; }
    bool supportsStreaming() const override { return false; }
    int getNumSpeakers() const override { return 1; }
    int getSampleRate() const override { return config_.sample_rate; }

    tts::ErrorInfo synthesize(const std::string& text, tts::SynthesisResult& result) override {
        (void)text;
        (void)result;
        return tts::ErrorInfo::error(tts::ErrorCode::SYNTHESIS_FAILED, "not implemented");
    }

    tts::ErrorInfo saveEmptyForTest() {
        tts::AudioChunk empty;
        return saveToFile(empty, "/tmp/tts-empty-should-not-be-written.wav");
    }

private:
    bool initialized_ = false;
    tts::TtsConfig config_;
};

void verify_public_presets() {
    const auto presets = SpacemiT::TtsConfig::AvailablePresets();
    require(contains(presets, "matcha_zh"), "matcha_zh preset must be advertised");
    require(contains(presets, "matcha_en"), "matcha_en preset must be advertised");
    require(contains(presets, "matcha_zh_en"), "matcha_zh_en preset must be advertised");
    require(contains(presets, "kokoro"), "kokoro preset must be advertised");
    require(contains(presets, "kokoro_en"), "kokoro_en preset must be advertised");
    require(contains(presets, "kokoro_zh"), "kokoro_zh preset must be advertised");

    const auto zh = SpacemiT::TtsConfig::Preset("matcha_zh");
    require(zh.backend == SpacemiT::BackendType::MATCHA_ZH,
            "matcha_zh must select MATCHA_ZH backend");
    require(zh.sample_rate == 22050, "matcha_zh must use 22050 Hz sample rate");
    require(!zh.model_dir.empty(), "matcha_zh must provide a model directory");

    const auto zh_en = SpacemiT::TtsConfig::Preset("matcha_zh_en");
    require(zh_en.backend == SpacemiT::BackendType::MATCHA_ZH_EN,
            "matcha_zh_en must select MATCHA_ZH_EN backend");
    require(zh_en.sample_rate == 16000, "matcha_zh_en must use 16000 Hz sample rate");

    const auto kokoro = SpacemiT::TtsConfig::Preset("kokoro");
    require(kokoro.backend == SpacemiT::BackendType::KOKORO,
            "kokoro must select the compatibility KOKORO backend");
    require(kokoro.num_threads == 4, "kokoro must use 4 threads");

    const auto kokoro_en = SpacemiT::TtsConfig::Preset("kokoro_en");
    require(kokoro_en.backend == SpacemiT::BackendType::KOKORO_EN,
            "kokoro_en must select KOKORO_EN backend");
    require(kokoro_en.sample_rate == 24000,
            "kokoro_en must use 24000 Hz sample rate");
    require(kokoro_en.num_threads == 4, "kokoro_en must use 4 threads");

    const auto kokoro_zh = SpacemiT::TtsConfig::Preset("kokoro_zh");
    require(kokoro_zh.backend == SpacemiT::BackendType::KOKORO_ZH,
            "kokoro_zh must select KOKORO_ZH backend");
    require(kokoro_zh.sample_rate == 24000,
            "kokoro_zh must use 24000 Hz sample rate");
    require(kokoro_zh.num_threads == 4, "kokoro_zh must use 4 threads");

    const auto tuned = zh_en.withSpeed(1.25f).withSpeaker(3).withVolume(80).withProvider("cpu");
    require(near(zh_en.speech_rate, 1.0f), "withSpeed must not mutate original config");
    require(tuned.speaker_id == 3, "withSpeaker must set speaker on returned config");
    require(tuned.volume == 80, "withVolume must set volume on returned config");
    require(tuned.provider == "cpu", "withProvider must set provider on returned config");
    require(near(tuned.speech_rate, 1.25f), "withSpeed must set speed on returned config");
}

void verify_internal_config_and_audio_chunk() {
    tts::TtsConfig config;
    require(config.validate().isOk(), "default internal TTS config must be valid");

    const auto tuned = config.withSpeed(0.8f)
        .withPitch(1.1f)
        .withVolume(30)
        .withSampleRate(22050)
        .withFormat(tts::AudioFormat::PCM_F32LE);
    require(near(config.speech_rate, 1.0f), "internal builders must not mutate original speed");
    require(near(tuned.speech_rate, 0.8f), "withSpeed must set internal speed");
    require(near(tuned.pitch, 1.1f), "withPitch must set internal pitch");
    require(tuned.volume == 30, "withVolume must set internal volume");
    require(tuned.sample_rate == 22050, "withSampleRate must set internal sample rate");
    require(tuned.format == tts::AudioFormat::PCM_F32LE, "withFormat must set internal format");

    const auto float_chunk = tts::AudioChunk::fromFloat({0.0f, 0.5f, -0.5f, 1.0f}, 16000);
    require(float_chunk.getNumSamples() == 4, "float chunk must report sample count");
    require(float_chunk.getDurationMs() == 0, "short float chunk duration must floor to 0 ms");
    require(!float_chunk.isEmpty(), "float chunk must not be empty");

    const auto int16_chunk = tts::AudioChunk::fromInt16({0, 32767, -32768}, 16000);
    require(int16_chunk.getNumSamples() == 3, "int16 chunk must report sample count");
    require(near(int16_chunk.samples[0], 0.0f), "int16 zero must convert to 0");
    require(int16_chunk.samples[1] > 0.999f, "positive int16 max must convert near 1");
    require(near(int16_chunk.samples[2], -1.0f), "negative int16 min must convert to -1");

    const auto bytes = int16_chunk.toBytes();
    require(bytes.size() == 6, "int16 chunk must convert to PCM byte payload");
}

void verify_invalid_input_error_path() {
    bool threw = false;
    try {
        (void)SpacemiT::TtsConfig::Preset("does-not-exist");
    } catch (const std::invalid_argument& exc) {
        threw = std::string(exc.what()).find("Unknown TTS preset") != std::string::npos;
    }
    require(threw, "unknown public preset must throw a useful invalid_argument");

    tts::TtsConfig invalid_rate;
    invalid_rate.sample_rate = 0;
    auto err = invalid_rate.validate();
    require(err.code == tts::ErrorCode::INVALID_CONFIG,
            "zero sample rate must report INVALID_CONFIG");

    tts::TtsConfig invalid_speed;
    invalid_speed.speech_rate = 0.0f;
    err = invalid_speed.validate();
    require(err.code == tts::ErrorCode::INVALID_CONFIG,
            "non-positive speech rate must report INVALID_CONFIG");

    tts::TtsConfig invalid_volume;
    invalid_volume.volume = 101;
    err = invalid_volume.validate();
    require(err.code == tts::ErrorCode::INVALID_CONFIG,
            "out-of-range volume must report INVALID_CONFIG");

    SaveOnlyBackend backend;
    require(backend.initialize(tts::TtsConfig()).isOk(), "save-only backend must initialize");
    err = backend.saveEmptyForTest();
    require(err.code == tts::ErrorCode::INVALID_CONFIG,
            "saving empty audio must report INVALID_CONFIG");
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "expected one test mode argument");
    const std::string mode = argv[1];

    if (mode == "--config-contract") {
        verify_public_presets();
        verify_internal_config_and_audio_chunk();
    } else if (mode == "--invalid-input-error-path") {
        verify_invalid_input_error_path();
    } else {
        std::cerr << "Unknown mode: " << mode << std::endl;
        return 2;
    }

    std::cout << "PASS " << mode << std::endl;
    return 0;
}
