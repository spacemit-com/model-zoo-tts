/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef KOKORO_EN_BACKEND_HPP
#define KOKORO_EN_BACKEND_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "backends/kokoro/kokoro_backend.hpp"
#include "backends/kokoro/kokoro_phonemizer.hpp"

namespace tts {

// =============================================================================
// Kokoro English Backend
// =============================================================================
//
// English TTS backend using the kokoro-v1.0-en model (24000Hz) with the
// espeak-ng English phonemizer.
//

class KokoroEnBackend : public KokoroBackend {
public:
    KokoroEnBackend();
    ~KokoroEnBackend() override;

protected:
    std::vector<int64_t> textToTokenIds(const std::string& text) override;
    std::string getModelSubdir() const override;
    std::string getModelFile() const override;
    std::string getLanguage() const override;
    std::string getVoiceName() const override;
    std::string getConvFallbackFilter() const override;
    ErrorInfo initializeLanguageSpecific(const TtsConfig& config) override;

private:
    KokoroPhonemizer phonemizer_;
};

}  // namespace tts

#endif  // KOKORO_EN_BACKEND_HPP
