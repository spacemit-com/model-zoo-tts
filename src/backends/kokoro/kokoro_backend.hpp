/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef KOKORO_BACKEND_HPP
#define KOKORO_BACKEND_HPP

#include <onnxruntime_cxx_api.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "backends/kokoro/kokoro_voice_manager.hpp"
#include "backends/tts_backend.hpp"

namespace tts {

// =============================================================================
// Kokoro Backend Base Class
// =============================================================================
//
// Common base for all Kokoro TTS backends. Handles ONNX model loading, SpaceMIT
// EP setup, end-to-end inference and audio post-processing. Derived classes only
// provide the language-specific frontend (text -> token IDs) and model subdir.
//
// Kokoro is a single-model end-to-end TTS (token_ids + style + speed -> 24kHz
// waveform), so unlike Matcha (acoustic model + vocoder) it derives directly
// from ITtsBackend.
//
// Hierarchy:
//   ITtsBackend
//       +-- KokoroBackend (this class - common base)
//               +-- KokoroEnBackend  (English, kokoro-v1.0-en)
//               +-- KokoroZhBackend  (Chinese, kokoro-v1.1-zh)
//

class KokoroBackend : public ITtsBackend {
public:
    static constexpr int SAMPLE_RATE = 24000;
    static constexpr int MAX_TOKEN_LENGTH = 512;
    // Synthetic-token defaults are only used when the explicit benchmarking
    // override is malformed. Production warmup uses valid frontend text.
    static constexpr int WARMUP_TOKEN_LENGTH = 128;
    static constexpr int EN_WARMUP_TOKEN_LENGTH = 4;
    static constexpr int EN_EP_WARMUP_EFFECTIVE_TOKENS = 64;
    static constexpr int ZH_EP_WARMUP_EFFECTIVE_TOKENS = 63;

    explicit KokoroBackend(BackendType type);
    ~KokoroBackend() override;

    // -------------------------------------------------------------------------
    // ITtsBackend interface
    // -------------------------------------------------------------------------

    ErrorInfo initialize(const TtsConfig& config) override;
    void shutdown() override;
    bool isInitialized() const override;

    BackendType getType() const override;
    std::string getName() const override;
    std::string getVersion() const override;
    bool supportsStreaming() const override;
    int getNumSpeakers() const override;
    int getSampleRate() const override;

    ErrorInfo synthesize(const std::string& text, SynthesisResult& result) override;

    ErrorInfo setSpeed(float speed) override;
    ErrorInfo updateLexicon(const std::vector<LexiconEntry>& entries) override;

protected:
    // -------------------------------------------------------------------------
    // Methods derived classes must implement
    // -------------------------------------------------------------------------

    /// @brief Convert text to token IDs (padded [0, ...ids..., 0]).
    virtual std::vector<int64_t> textToTokenIds(const std::string& text) = 0;

    /// @brief Return language-aware units used only for an oversized clause.
    /// English keeps whole words; Chinese overrides this with cppjieba words.
    virtual std::vector<std::string> getChunkingUnits(
        const std::string& text);

    /// @brief Model subdirectory name (e.g. "kokoro-v1.0-en").
    virtual std::string getModelSubdir() const = 0;

    /// @brief Local quantized model file name (e.g. "kokoro-v1.0-en.q.onnx").
    virtual std::string getModelFile() const = 0;

    /// @brief Language tag passed to the downloader ("en" / "zh").
    virtual std::string getLanguage() const = 0;

    /// @brief Voice file name without .bin (e.g. "default" / "zf_001").
    virtual std::string getVoiceName() const = 0;

    /// @brief Semicolon-separated list of generator output convs to keep on the
    /// CPU float path, removing the metallic timbre introduced by EP int8
    /// quantization. Node names differ per model version, so the list is
    /// provided by the derived class. Overridable via the
    /// SPACEMIT_EP_DISABLE_OP_NAME_FILTER environment variable.
    virtual std::string getConvFallbackFilter() const = 0;

    /// @brief Language-specific initialization (called once the ONNX session is ready).
    virtual ErrorInfo initializeLanguageSpecific(const TtsConfig& config) = 0;

    /// @brief Language-specific cleanup.
    virtual void shutdownLanguageSpecific() {}

    /// @brief Apply language-specific custom pronunciations.
    virtual ErrorInfo updateLanguageLexicon(
        const std::vector<LexiconEntry>& entries) = 0;

    // -------------------------------------------------------------------------
    // Protected helpers (for derived classes)
    // -------------------------------------------------------------------------

    /// @brief Model directory with ~ expanded.
    std::string getModelDir() const;
    const std::string& getActiveModelDir() const { return active_model_dir_; }

    const TtsConfig& getConfig() const { return config_; }
    const KokoroVoiceManager& getVoiceManager() const { return voice_manager_; }

    BackendType type_;

private:
    std::vector<float> runInference(const std::vector<int64_t>& token_ids,
                                    const std::vector<float>& style_vector,
                                    float speed);

    KokoroVoiceManager voice_manager_;

    std::unique_ptr<Ort::Env> env_;
    std::unique_ptr<Ort::Session> session_;

    TtsConfig config_;
    std::string active_model_dir_;
    std::string active_model_path_;
    bool initialized_ = false;
    bool using_spacemit_ep_ = false;
    int ep_threads_ = 4;
    float current_speed_ = 1.0f;

    mutable std::mutex inference_mutex_;
};

// Forward declarations
class KokoroEnBackend;
class KokoroZhBackend;

}  // namespace tts

#endif  // KOKORO_BACKEND_HPP
