/* Copyright (C) 2025 SpacemiT Co., Ltd.
    * SPDX-License-Identifier: Apache-2.0 */

#include "backends/kokoro/kokoro_en_backend.hpp"

#include <string>
#include <vector>

namespace tts {

KokoroEnBackend::KokoroEnBackend() : KokoroBackend(BackendType::KOKORO_EN) {}

KokoroEnBackend::~KokoroEnBackend() = default;

ErrorInfo KokoroEnBackend::updateLanguageLexicon(
    const std::vector<LexiconEntry>& entries) {
    for (const auto& entry : entries) {
        if (entry.locale != "en") {
            return ErrorInfo::error(
                ErrorCode::UNSUPPORTED_LANGUAGE,
                "Kokoro English lexicon entries require locale=en");
        }
        if (!phonemizer_.addEnglishPronunciation(
                entry.word, entry.phoneme)) {
            return ErrorInfo::error(
                ErrorCode::INVALID_CONFIG,
                "Invalid Kokoro English lexicon entry: " + entry.word);
        }
    }
    return ErrorInfo::ok();
}

std::string KokoroEnBackend::getModelSubdir() const { return "kokoro-v1.0-en"; }

std::string KokoroEnBackend::getModelFile() const { return "kokoro-v1.0-en.q.onnx"; }

std::string KokoroEnBackend::getLanguage() const { return "en"; }

std::string KokoroEnBackend::getVoiceName() const { return "af_heart"; }

std::string KokoroEnBackend::getConvFallbackFilter() const {
    // v1.0 English model generator output convs (conv_post + iSTFT ConvTranspose).
    return "/decoder/decoder/generator/conv_post/Conv;"
            "/decoder/decoder/generator/istft/stft/ConvTranspose";
}

ErrorInfo KokoroEnBackend::initializeLanguageSpecific(const TtsConfig& config) {
    (void)config;
    try {
        phonemizer_.initPinyin();  // sets up vocab + espeak availability
        // Load the misaki-derived English lexicon (word -> IPA); espeak is the
        // fallback for out-of-vocabulary words only.
        phonemizer_.initEnglishLexicon(getModelDir() + "/" + getModelSubdir());
    } catch (const std::exception& e) {
        return ErrorInfo::error(
            ErrorCode::INVALID_CONFIG, std::string("Failed to initialize Kokoro English phonemizer: ") + e.what());
    }
    return ErrorInfo::ok();
}

std::vector<int64_t> KokoroEnBackend::textToTokenIds(const std::string& text) {
    return phonemizer_.englishTextToTokenIds(text);
}

}  // namespace tts
