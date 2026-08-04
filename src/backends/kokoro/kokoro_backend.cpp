/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#include "backends/kokoro/kokoro_backend.hpp"

#include <sstream>

#include <fstream>

#ifdef USE_SPACEMIT_EP
#include "spacemit_ort_env.h"
#endif

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <malloc.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>  // NOLINT(build/c++17)
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "audio/audio_processor.hpp"
#include "backends/kokoro/kokoro_model_downloader.hpp"

namespace fs = std::filesystem;

namespace {

void releaseUnusedHeapMemory() noexcept {
#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

int getEnvInt(const char* name, int def) {
    const char* v = std::getenv(name);
    return v ? std::atoi(v) : def;
}

std::string normalizeProvider(const std::string& p) {
    std::string v = p;
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v.empty() || v == "auto") return "auto";
    if (v == "cpu") return "cpu";
    if (v == "spacemit" || v == "spacemit_ep" || v == "ep") return "spacemit";
    std::cerr << "[Kokoro] Unsupported provider '" << p
                << "', fallback to auto" << std::endl;
    return "auto";
}

std::vector<std::string> splitTextUnits(const std::string& text) {
    std::vector<std::string> units;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            const auto is_word_char = [](unsigned char value) {
                return std::isalnum(value) != 0 || value == '\'' || value == '-';
            };
            size_t end = i + 1;
            if (is_word_char(c)) {
                while (end < text.size() &&
                        is_word_char(static_cast<unsigned char>(text[end]))) {
                    ++end;
                }
            } else if (std::isspace(c) != 0) {
                while (end < text.size() &&
                        std::isspace(static_cast<unsigned char>(text[end])) != 0) {
                    ++end;
                }
            }
            units.push_back(text.substr(i, end - i));
            i = end;
            continue;
        }

        size_t length = 1;
        if ((c & 0xF8) == 0xF0) length = 4;
        else if ((c & 0xF0) == 0xE0) length = 3;
        else if ((c & 0xE0) == 0xC0) length = 2;
        if (i + length > text.size()) length = 1;
        units.push_back(text.substr(i, length));
        i += length;
    }
    return units;
}

enum class BoundaryKind {
    kNone,
    kForced,
    kSecondary,
    kSentence
};

struct TextSpan {
    std::string text;
    BoundaryKind boundary = BoundaryKind::kNone;
};

BoundaryKind getBoundaryKind(const std::string& unit) {
    if (unit == "." || unit == "!" || unit == "?" ||
        unit == "\xE3\x80\x82" ||  // 。
        unit == "\xEF\xBC\x81" ||  // ！
        unit == "\xEF\xBC\x9F") {  // ？
        return BoundaryKind::kSentence;
    }
    if (unit == "," || unit == ":" || unit == ";" ||
        unit == "\xEF\xBC\x8C" ||  // ，
        unit == "\xE3\x80\x81" ||  // 、
        unit == "\xEF\xBC\x9A" ||  // ：
        unit == "\xEF\xBC\x9B") {  // ；
        return BoundaryKind::kSecondary;
    }
    return BoundaryKind::kNone;
}

bool isNumericSeparator(
        const std::vector<std::string>& units, size_t index) {
    if (index == 0 || index + 1 >= units.size()) {
        return false;
    }
    const std::string& separator = units[index];
    if (separator != "." && separator != "," && separator != ":") {
        return false;
    }
    const std::string& previous = units[index - 1];
    const std::string& next = units[index + 1];
    return !previous.empty() && !next.empty() &&
        std::isdigit(static_cast<unsigned char>(previous.back())) != 0 &&
        std::isdigit(static_cast<unsigned char>(next.front())) != 0;
}

std::vector<TextSpan> splitTextSpans(
    const std::string& text, BoundaryKind minimum_boundary) {
    std::vector<TextSpan> spans;
    std::string pending;
    const std::vector<std::string> units = splitTextUnits(text);
    for (size_t index = 0; index < units.size(); ++index) {
        const std::string& unit = units[index];
        pending += unit;
        const BoundaryKind boundary = isNumericSeparator(units, index)
            ? BoundaryKind::kNone : getBoundaryKind(unit);
        if (static_cast<int>(boundary) >=
            static_cast<int>(minimum_boundary)) {
            spans.push_back({std::move(pending), boundary});
            pending.clear();
        }
    }
    if (!pending.empty()) {
        const bool whitespace_only =
            std::all_of(pending.begin(), pending.end(), [](unsigned char c) {
                return std::isspace(c) != 0;
            });
        if (!whitespace_only) {
            spans.push_back({std::move(pending), BoundaryKind::kNone});
        }
    }
    return spans;
}

bool isEnglishFunctionWord(const std::string& word) {
    return word == "a" || word == "an" || word == "the" || word == "to" ||
        word == "in" || word == "am" || word == "is" || word == "are" ||
        word == "of" || word == "for" || word == "with" || word == "at" ||
        word == "on" || word == "by" || word == "from" || word == "and" ||
        word == "or" || word == "but";
}

bool splitTrailingEnglishFunctionWords(
        const std::string& text,
        std::string& prefix,
        std::string& carry) {
    constexpr size_t kMaxCarriedFunctionWords = 3;
    size_t scan_end = text.size();
    size_t carry_begin = text.size();
    size_t carried_words = 0;
    while (scan_end > 0 && carried_words < kMaxCarriedFunctionWords) {
        size_t word_end = scan_end;
        while (word_end > 0 &&
                std::isspace(
                    static_cast<unsigned char>(text[word_end - 1])) != 0) {
            --word_end;
        }
        size_t word_begin = word_end;
        while (word_begin > 0) {
            const unsigned char value =
                static_cast<unsigned char>(text[word_begin - 1]);
            if (std::isalnum(value) == 0 && value != '\'' && value != '-') {
                break;
            }
            --word_begin;
        }
        if (word_begin == word_end) {
            break;
        }

        std::string word = text.substr(word_begin, word_end - word_begin);
        std::transform(
            word.begin(), word.end(), word.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        if (!isEnglishFunctionWord(word)) {
            break;
        }
        carry_begin = word_begin;
        scan_end = word_begin;
        ++carried_words;
    }

    if (carried_words == 0 || carry_begin == 0 ||
        carry_begin == text.size()) {
        return false;
    }
    prefix = text.substr(0, carry_begin);
    carry = text.substr(carry_begin);
    return !prefix.empty() && !carry.empty();
}

void trimExcessTrailingSilence(std::vector<float>& audio,
        size_t sample_rate,
        size_t retained_silence_milliseconds) {
    constexpr float kRelativeSilenceThreshold = 0.01f;
    constexpr float kAbsoluteSilenceThreshold = 1.0e-4f;
    constexpr size_t kWindowMilliseconds = 10;
    if (audio.empty()) {
        return;
    }

    float peak = 0.0f;
    for (const float sample : audio) {
        peak = std::max(peak, std::abs(sample));
    }
    const float threshold =
        std::max(kAbsoluteSilenceThreshold, peak * kRelativeSilenceThreshold);
    const size_t window =
        std::max<size_t>(1, sample_rate * kWindowMilliseconds / 1000);
    size_t speech_end = audio.size();
    while (speech_end >= window) {
        float square_sum = 0.0f;
        for (size_t i = speech_end - window; i < speech_end; ++i) {
            square_sum += audio[i] * audio[i];
        }
        const float rms = std::sqrt(square_sum / static_cast<float>(window));
        if (rms > threshold) {
            break;
        }
        speech_end -= window;
    }

    const size_t retained =
        sample_rate * retained_silence_milliseconds / 1000;
    if (speech_end + retained < audio.size()) {
        audio.resize(speech_end + retained);
    }
}

void trimExcessLeadingSilence(std::vector<float>& audio,
        size_t sample_rate,
        size_t retained_silence_milliseconds) {
    constexpr float kRelativeSilenceThreshold = 0.01f;
    constexpr float kAbsoluteSilenceThreshold = 1.0e-4f;
    constexpr size_t kWindowMilliseconds = 10;
    if (audio.empty()) {
        return;
    }

    float peak = 0.0f;
    for (const float sample : audio) {
        peak = std::max(peak, std::abs(sample));
    }
    const float threshold =
        std::max(kAbsoluteSilenceThreshold, peak * kRelativeSilenceThreshold);
    const size_t window =
        std::max<size_t>(1, sample_rate * kWindowMilliseconds / 1000);
    const size_t retained =
        sample_rate * retained_silence_milliseconds / 1000;
    size_t speech_begin = 0;
    for (; speech_begin < audio.size(); speech_begin += window) {
        const size_t begin = speech_begin;
        const size_t end = std::min(audio.size(), begin + window);
        float square_sum = 0.0f;
        for (size_t i = begin; i < end; ++i) {
            square_sum += audio[i] * audio[i];
        }
        const float rms =
            std::sqrt(square_sum / static_cast<float>(end - begin));
        if (rms > threshold) {
            break;
        }
    }
    const size_t erase_count =
        speech_begin > retained ? speech_begin - retained : 0;
    if (erase_count > 0 && erase_count < audio.size()) {
        audio.erase(audio.begin(), audio.begin() + erase_count);
    }
}

void appendSilence(std::vector<float>& audio,
        size_t sample_rate,
        size_t silence_milliseconds) {
    audio.insert(
        audio.end(), sample_rate * silence_milliseconds / 1000, 0.0f);
}

float edgeRms(const std::vector<float>& audio, size_t count, bool from_end) {
    if (audio.empty() || count == 0) {
        return 0.0f;
    }
    count = std::min(count, audio.size());
    const size_t begin = from_end ? audio.size() - count : 0;
    float square_sum = 0.0f;
    for (size_t i = begin; i < begin + count; ++i) {
        square_sum += audio[i] * audio[i];
    }
    return std::sqrt(square_sum / static_cast<float>(count));
}

void appendWithCrossfade(std::vector<float>& output,
                            const std::vector<float>& chunk,
                            size_t crossfade_samples) {
    if (output.empty()) {
        output = chunk;
        return;
    }
    const size_t overlap =
        std::min({crossfade_samples, output.size(), chunk.size()});
    if (overlap == 0) {
        output.insert(output.end(), chunk.begin(), chunk.end());
        return;
    }
    // Model chunks normally end in silence. Concatenate quiet edges so the
    // next chunk's initial consonant is not attenuated by the crossfade.
    if (edgeRms(output, overlap, true) < 1.0e-3f ||
        edgeRms(chunk, overlap, false) < 1.0e-3f) {
        output.insert(output.end(), chunk.begin(), chunk.end());
        return;
    }
    const size_t output_begin = output.size() - overlap;
    for (size_t i = 0; i < overlap; ++i) {
        const float incoming =
            static_cast<float>(i + 1) / static_cast<float>(overlap + 1);
        output[output_begin + i] =
            output[output_begin + i] * (1.0f - incoming) +
            chunk[i] * incoming;
    }
    output.insert(output.end(), chunk.begin() + overlap, chunk.end());
}

std::vector<float> smoothNarrowImpulses(const std::vector<float>& audio) {
    if (audio.size() < 3) {
        return audio;
    }

    std::vector<float> smoothed(audio);
    for (size_t i = 1; i + 1 < audio.size(); ++i) {
        smoothed[i] = 0.25f * audio[i - 1] + 0.5f * audio[i] + 0.25f * audio[i + 1];
    }
    return smoothed;
}

}  // namespace

namespace tts {

// =============================================================================
// Construction / Destruction
// =============================================================================

KokoroBackend::KokoroBackend(BackendType type)
    : type_(type)
    , initialized_(false)
    , current_speed_(1.0f) {
}

KokoroBackend::~KokoroBackend() {
    shutdown();
}

std::vector<std::string> KokoroBackend::getChunkingUnits(
        const std::string& text) {
    return splitTextUnits(text);
}

std::string KokoroBackend::prepareTextForChunking(
        const std::string& text) const {
    return text;
}

// =============================================================================
// Lifecycle
// =============================================================================

ErrorInfo KokoroBackend::initialize(const TtsConfig& config) {
    if (initialized_) {
        return ErrorInfo::error(ErrorCode::ALREADY_STARTED, "Backend already initialized");
    }

    config_ = config;

    // Resolve model directory. An explicit model path is self-contained: the
    // frontend tokenizer and voice are discovered next to that model.
    std::string model_dir = getModelDir();
    const std::string subdir = getModelSubdir();
    std::string model_path = model_dir + "/" + subdir + "/" + getModelFile();
    const char* model_override = std::getenv("KOKORO_MODEL_PATH");
    if (model_override != nullptr && *model_override != '\0') {
        model_path = model_override;
        active_model_dir_ = fs::path(model_path).parent_path().string();
        if (!fs::exists(model_path)) {
            return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND,
                "Kokoro model file not found: " + model_path);
        }
    } else {
        active_model_dir_ = model_dir + "/" + subdir;

        KokoroModelDownloader downloader(model_dir);
        if (!downloader.ensureModelsExist(getLanguage())) {
            return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND,
                "Failed to download Kokoro models to: " + model_dir);
        }
    }

    // Load voice (skipped in token-injection mode, where style comes from the file)
    const bool inject_mode = (std::getenv("KOKORO_TOKEN_IDS_FILE") != nullptr);
    if (!inject_mode) {
        std::string voice_path;
        if (const char* voice_override = std::getenv("KOKORO_VOICE_PATH")) {
            voice_path = voice_override;
        } else {
            const std::string voice_name =
                (config.voice.empty() || config.voice == "default")
                    ? getVoiceName()
                    : config.voice;
            const fs::path voice_base =
                fs::path(active_model_dir_) / "voices" / voice_name;
            if (fs::exists(voice_base.string() + ".bin")) {
                voice_path = voice_base.string() + ".bin";
            } else {
                voice_path = voice_base.string() + ".npy";
            }
        }
        if (!fs::exists(voice_path)) {
            return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND,
                "Kokoro voice file not found at: " + voice_path);
        }
        if (!voice_manager_.loadVoice(voice_path)) {
            return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND,
                "Failed to load voice file: " + voice_path);
        }
    }

    try {
        // Initialize ONNX Runtime (suppress stderr warnings)
        int stderr_fd = dup(STDERR_FILENO);
        int devnull_fd = open("/dev/null", O_WRONLY);
        dup2(devnull_fd, STDERR_FILENO);

        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "KokoroBackend");

        // Restore stderr
        dup2(stderr_fd, STDERR_FILENO);
        close(stderr_fd);
        close(devnull_fd);

        const int default_threads = config.num_threads > 0 ? config.num_threads : 4;
        const int cpu_threads =
            getEnvInt("SPACEMIT_TTS_CPU_THREADS", default_threads);
        const int ep_threads =
            getEnvInt("SPACEMIT_TTS_EP_THREADS", default_threads);

        // Keep the same public provider semantics as Matcha:
        // config.provider -> SPACEMIT_TTS_PROVIDER -> model-specific auto route.
        std::string provider = normalizeProvider(config.provider);
        if (provider == "auto") {
            const char* env_p = std::getenv("SPACEMIT_TTS_PROVIDER");
            if (env_p != nullptr && env_p[0] != '\0') {
                provider = normalizeProvider(env_p);
            }
        }
#ifdef USE_SPACEMIT_EP
        if (provider == "auto") provider = "spacemit";
#else
        if (provider == "auto") provider = "cpu";
#endif
        bool use_ep = (provider == "spacemit");

        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(use_ep ? ep_threads : cpu_threads);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef USE_SPACEMIT_EP
        if (use_ep) {
            std::unordered_map<std::string, std::string> ep_opts = {
                {"SPACEMIT_EP_INTRA_THREAD_NUM", std::to_string(ep_threads)},
                {"SPACEMIT_EP_DISABLE_OP_NAME_FILTER", getConvFallbackFilter()}
            };
            if (const char* ov = std::getenv("SPACEMIT_EP_DISABLE_OP_NAME_FILTER")) {
                ep_opts["SPACEMIT_EP_DISABLE_OP_NAME_FILTER"] = ov;
            }
            Ort::Status st = Ort::SessionOptionsSpaceMITEnvInit(session_options, ep_opts);
            if (st.IsOK()) {
                std::cout << "[Kokoro] SpaceMIT EP initialized (ep_threads=" << ep_threads << ")" << std::endl;
            } else {
                std::cerr << "[Kokoro] SpaceMIT EP init failed: " << st.GetErrorMessage()
                            << ", fallback to CPU" << std::endl;
                use_ep = false;
                session_options.SetIntraOpNumThreads(cpu_threads);
#if defined(__riscv) || defined(__riscv__)
                session_options.DisableMemPattern();
                session_options.DisableCpuMemArena();
#endif
            }
        } else {
#if defined(__riscv) || defined(__riscv__)
            session_options.DisableMemPattern();
            session_options.DisableCpuMemArena();
#endif
        }
#else
        if (use_ep) {
            std::cerr << "[Kokoro] SpaceMIT EP requested but binary built without USE_SPACEMIT_EP, using CPU" << std::endl;
            use_ep = false;
        }
#if defined(__riscv) || defined(__riscv__)
        session_options.DisableMemPattern();
        session_options.DisableCpuMemArena();
#endif
#endif

        active_model_path_ = model_path;
        using_spacemit_ep_ = use_ep;
        ep_threads_ = ep_threads;
        std::cout << "[Kokoro] Provider: " << (use_ep ? "spacemit" : "cpu") << std::endl;
        session_ = std::make_unique<Ort::Session>(*env_, model_path.c_str(), session_options);

        ErrorInfo lang_err = initializeLanguageSpecific(config);
        if (!lang_err.isOk()) {
            return lang_err;
        }

        if (config.speech_rate <= 0.0f || config.speech_rate > 10.0f) {
            return ErrorInfo::error(ErrorCode::INVALID_CONFIG,
                "speech_rate must be > 0 and <= 10.0");
        }
        current_speed_ = config.speech_rate;

        if (config.enable_warmup && !inject_mode) {
            std::cout << "[Kokoro] Warming up model..." << std::endl;
            auto start = std::chrono::high_resolution_clock::now();

            std::vector<int64_t> warm_tokens;
            if (std::getenv("SPACEMIT_TTS_WARMUP_TOKENS") == nullptr) {
                // Use a valid frontend sequence by default. Arbitrary repeated
                // token IDs can make the stochastic duration path generate
                // incompatible F0/noise lengths (for example 360 vs 720) in
                // the quantized Chinese graph.
                const std::string warm_text =
                    type_ == BackendType::KOKORO_ZH
                        // 63 effective tokens (65 including BOS/EOS): large
                        // enough to establish the decoder workspace used by
                        // common Chinese requests.
                        ? "这是一次中文语音合成模型的完整预热测试。"
                        // 64 effective tokens. This is the largest cold-start
                        // shape validated for the English EP path and becomes
                        // the upper bound for every long-text chunk.
                        : "This is a spacemit k3 kokoro performance test "
                            "for today.";
                warm_tokens = textToTokenIds(warm_text);
            } else {
                // Keep the explicit length override for EP benchmarking.
                const int default_warm_len =
                    (type_ == BackendType::KOKORO ||
                        type_ == BackendType::KOKORO_EN)
                        ? EN_WARMUP_TOKEN_LENGTH
                        : WARMUP_TOKEN_LENGTH;
                int warm_len = getEnvInt(
                    "SPACEMIT_TTS_WARMUP_TOKENS", default_warm_len);
                if (warm_len < 4) warm_len = 4;
                if (warm_len > MAX_TOKEN_LENGTH) warm_len = MAX_TOKEN_LENGTH;
                warm_tokens.reserve(warm_len);
                warm_tokens.push_back(0);
                for (int i = 0; i < warm_len - 2; ++i) {
                    warm_tokens.push_back(43 + (i % 26));
                }
                warm_tokens.push_back(0);
            }
            auto style = voice_manager_.getStyleVector(
                static_cast<int>(warm_tokens.size()) - 2);

            const int warmup_runs = getEnvInt("SPACEMIT_TTS_WARMUP_RUNS", 1);
            bool warmup_ok = true;
            try {
                for (int i = 0; i < warmup_runs; ++i) {
                    runInference(warm_tokens, style, 1.0f / current_speed_);
                }
            } catch (const std::exception& e) {
                // Warmup is an optimization, not a prerequisite for synthesis.
                // A failed dynamic-shape probe must not make the backend
                // unavailable; the first real request can establish its shape.
                std::cerr << "[Kokoro] Warning: warmup failed, continuing: "
                            << e.what() << std::endl;
                warmup_ok = false;
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            if (warmup_ok) {
                std::cout << "[Kokoro] Model warmed up in " << dur.count() << "ms ("
                            << warmup_runs << " runs, " << warm_tokens.size()
                            << " tokens)" << std::endl;
            }
        }

        initialized_ = true;

        std::cout << "[Kokoro] Backend initialized successfully (" << backendTypeToString(type_) << ")" << std::endl;
        return ErrorInfo::ok();
    } catch (const std::exception& e) {
        return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND,
            std::string("Failed to initialize Kokoro model: ") + e.what());
    }
}

void KokoroBackend::shutdown() {
    if (initialized_) {
        shutdownLanguageSpecific();
        session_.reset();
        env_.reset();
        initialized_ = false;
    }
    releaseUnusedHeapMemory();
}

bool KokoroBackend::isInitialized() const {
    return initialized_;
}

// =============================================================================
// Backend Info
// =============================================================================

BackendType KokoroBackend::getType() const {
    return type_;
}

std::string KokoroBackend::getName() const {
    return std::string("Kokoro-TTS (") + backendTypeToString(type_) + ")";
}

std::string KokoroBackend::getVersion() const {
    return type_ == BackendType::KOKORO_ZH ? "1.1-zh" : "1.0-en";
}

bool KokoroBackend::supportsStreaming() const {
    return false;
}

int KokoroBackend::getNumSpeakers() const {
    return 1;
}

int KokoroBackend::getSampleRate() const {
    return SAMPLE_RATE;
}

// =============================================================================
// Synthesis
// =============================================================================

ErrorInfo KokoroBackend::synthesize(const std::string& text, SynthesisResult& result) {
    if (!initialized_) {
        return ErrorInfo::error(ErrorCode::NOT_INITIALIZED, "Backend not initialized");
    }

    if (text.empty()) {
        return ErrorInfo::error(ErrorCode::INVALID_TEXT, "Empty text");
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    const bool profile = std::getenv("KOKORO_PROFILE") != nullptr;

    try {
        // Optional token-injection path (frontend generated externally, e.g. Python).
        // If KOKORO_TOKEN_IDS_FILE is set, read pregenerated token_ids (line 1) and
        // style vector (line 2, 256 floats), bypassing the C++ frontend/voice.
        std::vector<int64_t> token_ids;
        std::vector<float> style_vector;
        const char* tk_file = std::getenv("KOKORO_TOKEN_IDS_FILE");
        const bool injected_tokens = tk_file != nullptr;
        if (injected_tokens) {
            std::ifstream tf(tk_file);
            if (!tf) {
                return ErrorInfo::error(ErrorCode::INVALID_TEXT,
                    std::string("cannot open KOKORO_TOKEN_IDS_FILE: ") + tk_file);
            }
            std::string line1, line2;
            std::getline(tf, line1);
            std::getline(tf, line2);
            { std::istringstream is(line1); long v; while (is >> v) token_ids.push_back((int64_t)v); }
            { std::istringstream is(line2); float v; while (is >> v) style_vector.push_back(v); }
            if ((int)style_vector.size() != KokoroVoiceManager::STYLE_DIM) {
                return ErrorInfo::error(ErrorCode::INVALID_TEXT,
                    "KOKORO_TOKEN_IDS_FILE style dim != 256");
            }
        } else {
            // Step 1: Convert to token IDs via the language-specific frontend
            token_ids = textToTokenIds(text);

            if (token_ids.empty()) {
                return ErrorInfo::error(
                    ErrorCode::INVALID_TEXT,
                    "Kokoro frontend produced no supported tokens");
            }

        }

        // Step 2: Run ONNX inference. Kokoro uses inverse speed:
        // 1.0/speech_rate. Split at sentence punctuation first, then secondary
        // punctuation, and use word/token boundaries only for a single span
        // that exceeds the active runtime limit.
        float kokoro_speed = 1.0f / current_speed_;
        std::vector<float> audio_samples;
        constexpr size_t kModelMaxEffectiveTokens = MAX_TOKEN_LENGTH - 2;
        // Keep every EP chunk within the largest shape already exercised by
        // warmup. This lets the provider reuse one bounded workspace instead
        // of compiling and retaining larger dynamic-shape workspaces.
        const size_t warmup_shape_tokens =
            type_ == BackendType::KOKORO_ZH
                ? ZH_EP_WARMUP_EFFECTIVE_TOKENS
                : EN_EP_WARMUP_EFFECTIVE_TOKENS;
        const size_t runtime_max_effective_tokens = using_spacemit_ep_
            ? warmup_shape_tokens
            : kModelMaxEffectiveTokens;
        const auto effectiveTokenCount = [](const std::vector<int64_t>& ids) {
            const bool has_padding =
                ids.size() >= 2 && ids.front() == 0 && ids.back() == 0;
            return ids.size() - (has_padding ? 2 : 0);
        };
        const size_t effective_count = effectiveTokenCount(token_ids);

        if (effective_count == 0) {
            return ErrorInfo::error(
                ErrorCode::INVALID_TEXT,
                "Kokoro frontend produced no effective tokens");
        }

        struct ModelChunk {
            std::vector<int64_t> tokens;
            BoundaryKind boundary = BoundaryKind::kNone;
        };
        std::vector<ModelChunk> model_chunks;
        bool warned_about_token_limit_split = false;
        const auto pushHardLimitedTokens =
            [&](const std::vector<int64_t>& ids, BoundaryKind boundary) {
            const size_t count = effectiveTokenCount(ids);
            if (count == 0) {
                return;
            }
            if (count <= runtime_max_effective_tokens) {
                model_chunks.push_back({ids, boundary});
                return;
            }
            if (!warned_about_token_limit_split) {
                std::cerr
                    << "[Kokoro] Warning: a text unit exceeds "
                    << runtime_max_effective_tokens
                    << " effective tokens; splitting at word/token boundaries"
                    << std::endl;
                warned_about_token_limit_split = true;
            }
            const bool has_padding =
                ids.size() >= 2 && ids.front() == 0 && ids.back() == 0;
            const size_t begin = has_padding ? 1 : 0;
            const size_t end = ids.size() - (has_padding ? 1 : 0);
            const size_t part_count =
                (count + runtime_max_effective_tokens - 1) /
                runtime_max_effective_tokens;
            const size_t base_part_size = count / part_count;
            const size_t larger_part_count = count % part_count;
            size_t offset = begin;
            for (size_t part = 0; part < part_count; ++part) {
                const size_t part_size =
                    base_part_size + (part < larger_part_count ? 1 : 0);
                const size_t chunk_end = offset + part_size;
                std::vector<int64_t> model_chunk;
                model_chunk.reserve(chunk_end - offset + 2);
                model_chunk.push_back(0);
                model_chunk.insert(
                    model_chunk.end(),
                    ids.begin() + offset,
                    ids.begin() + chunk_end);
                model_chunk.push_back(0);
                model_chunks.push_back({
                    std::move(model_chunk),
                    part + 1 < part_count ? BoundaryKind::kForced : boundary});
                offset = chunk_end;
            }
        };

        const auto splitOversizedSpan =
            [&](const TextSpan& span) {
            std::string pending_text;
            std::vector<int64_t> pending_tokens;
            for (const auto& unit : getChunkingUnits(span.text)) {
                const std::string candidate = pending_text + unit;
                std::vector<int64_t> candidate_tokens =
                    textToTokenIds(candidate);
                if (effectiveTokenCount(candidate_tokens) <=
                    runtime_max_effective_tokens) {
                    pending_text = candidate;
                    pending_tokens = std::move(candidate_tokens);
                    continue;
                }
                if (effectiveTokenCount(pending_tokens) > 0) {
                    if (type_ == BackendType::KOKORO ||
                        type_ == BackendType::KOKORO_EN) {
                        std::string prefix;
                        std::string carry;
                        if (splitTrailingEnglishFunctionWords(
                                pending_text, prefix, carry)) {
                            std::vector<int64_t> prefix_tokens =
                                textToTokenIds(prefix);
                            std::vector<int64_t> carried_tokens =
                                textToTokenIds(carry + unit);
                            if (effectiveTokenCount(prefix_tokens) > 0 &&
                                effectiveTokenCount(carried_tokens) <=
                                    runtime_max_effective_tokens) {
                                pushHardLimitedTokens(
                                    prefix_tokens, BoundaryKind::kForced);
                                pending_text = carry + unit;
                                pending_tokens = std::move(carried_tokens);
                                continue;
                            }
                        }
                    }
                    pushHardLimitedTokens(
                        pending_tokens, BoundaryKind::kForced);
                }
                pending_text = unit;
                pending_tokens = textToTokenIds(pending_text);
                if (effectiveTokenCount(pending_tokens) >
                    runtime_max_effective_tokens) {
                    pushHardLimitedTokens(
                        pending_tokens, BoundaryKind::kForced);
                    pending_text.clear();
                    pending_tokens.clear();
                }
            }
            if (effectiveTokenCount(pending_tokens) > 0) {
                pushHardLimitedTokens(pending_tokens, span.boundary);
            } else if (!model_chunks.empty() &&
                    span.boundary != BoundaryKind::kNone) {
                model_chunks.back().boundary = span.boundary;
            }
        };

        if (injected_tokens) {
            pushHardLimitedTokens(token_ids, BoundaryKind::kNone);
        } else {
            const std::string chunking_text = prepareTextForChunking(text);
            const std::vector<TextSpan> sentences =
                splitTextSpans(chunking_text, BoundaryKind::kSentence);
            if (effective_count <= runtime_max_effective_tokens) {
                const BoundaryKind boundary = sentences.empty()
                    ? BoundaryKind::kNone : sentences.back().boundary;
                pushHardLimitedTokens(token_ids, boundary);
            } else {
                std::string pending_sentence_text;
                std::vector<int64_t> pending_sentence_tokens;
                BoundaryKind pending_sentence_boundary = BoundaryKind::kNone;
                const auto flushPendingSentence = [&]() {
                    if (effectiveTokenCount(pending_sentence_tokens) > 0) {
                        pushHardLimitedTokens(
                            pending_sentence_tokens,
                            pending_sentence_boundary);
                    }
                    pending_sentence_text.clear();
                    pending_sentence_tokens.clear();
                    pending_sentence_boundary = BoundaryKind::kNone;
                };
                for (const auto& sentence : sentences) {
                    std::vector<int64_t> sentence_tokens =
                        textToTokenIds(sentence.text);
                    if (effectiveTokenCount(sentence_tokens) <=
                        runtime_max_effective_tokens) {
                        if (using_spacemit_ep_) {
                            pushHardLimitedTokens(
                                sentence_tokens, sentence.boundary);
                            continue;
                        }
                        const std::string candidate =
                            pending_sentence_text + sentence.text;
                        std::vector<int64_t> candidate_tokens =
                            textToTokenIds(candidate);
                        if (!pending_sentence_text.empty() &&
                            effectiveTokenCount(candidate_tokens) >
                                runtime_max_effective_tokens) {
                            flushPendingSentence();
                            pending_sentence_text = sentence.text;
                            pending_sentence_tokens =
                                std::move(sentence_tokens);
                        } else {
                            pending_sentence_text = candidate;
                            pending_sentence_tokens =
                                std::move(candidate_tokens);
                        }
                        pending_sentence_boundary = sentence.boundary;
                        continue;
                    }

                    flushPendingSentence();
                    std::string pending_text;
                    std::vector<int64_t> pending_tokens;
                    BoundaryKind pending_boundary = BoundaryKind::kNone;
                    for (const auto& clause :
                        splitTextSpans(
                            sentence.text, BoundaryKind::kSecondary)) {
                        std::vector<int64_t> clause_tokens =
                            textToTokenIds(clause.text);
                        if (effectiveTokenCount(clause_tokens) >
                            runtime_max_effective_tokens) {
                            if (effectiveTokenCount(pending_tokens) > 0) {
                                pushHardLimitedTokens(
                                    pending_tokens, pending_boundary);
                                pending_text.clear();
                                pending_tokens.clear();
                            }
                            splitOversizedSpan(clause);
                            continue;
                        }

                        const std::string candidate =
                            pending_text + clause.text;
                        std::vector<int64_t> candidate_tokens =
                            textToTokenIds(candidate);
                        if (!pending_text.empty() &&
                            effectiveTokenCount(candidate_tokens) >
                            runtime_max_effective_tokens) {
                            pushHardLimitedTokens(
                                pending_tokens, pending_boundary);
                            pending_text = clause.text;
                            pending_tokens = std::move(clause_tokens);
                        } else {
                            pending_text = candidate;
                            pending_tokens = std::move(candidate_tokens);
                        }
                        pending_boundary = clause.boundary;
                    }
                    if (effectiveTokenCount(pending_tokens) > 0) {
                        pushHardLimitedTokens(
                            pending_tokens, pending_boundary);
                    }
                }
                flushPendingSentence();
            }
        }

        if (model_chunks.size() > 1) {
            std::cout << "[Kokoro] Splitting long input: " << effective_count
                        << " effective tokens, " << model_chunks.size()
                        << " sentence/model-safe chunks [";
            for (size_t index = 0; index < model_chunks.size(); ++index) {
                if (index > 0) {
                    std::cout << ", ";
                }
                std::cout << effectiveTokenCount(model_chunks[index].tokens);
            }
            std::cout << "]" << std::endl;
        }
        const auto frontend_end = std::chrono::high_resolution_clock::now();

        // Diagnostic-only token dumps skip inference. The full-token file keeps
        // the existing one-line-per-request golden-test contract; the optional
        // chunk file exposes one model-safe chunk per line for long-text tests.
        const char* token_dump_path = std::getenv("KOKORO_DUMP_TOKENS");
        const char* chunk_dump_path = std::getenv("KOKORO_DUMP_CHUNKS");
        if (token_dump_path != nullptr || chunk_dump_path != nullptr) {
            if (token_dump_path != nullptr) {
                std::ofstream output(token_dump_path, std::ios::app);
                for (size_t i = 0; i < token_ids.size(); ++i) {
                    output << token_ids[i]
                            << (i + 1 < token_ids.size() ? ' ' : '\n');
                }
            }
            if (chunk_dump_path != nullptr) {
                std::ofstream output(chunk_dump_path, std::ios::app);
                for (const auto& chunk : model_chunks) {
                    for (size_t i = 0; i < chunk.tokens.size(); ++i) {
                        output << chunk.tokens[i]
                                << (i + 1 < chunk.tokens.size() ? ' ' : '\n');
                    }
                }
            }
            result.audio = AudioChunk::fromFloat({}, SAMPLE_RATE, true);
            result.success = true;
            return ErrorInfo::ok();
        }

        for (size_t chunk_index = 0; chunk_index < model_chunks.size();
                ++chunk_index) {
            const auto& model_chunk = model_chunks[chunk_index];
            const auto& chunk_tokens = model_chunk.tokens;
            const size_t chunk_effective_count =
                effectiveTokenCount(chunk_tokens);
            std::vector<float> chunk_style = injected_tokens
                ? style_vector
                : voice_manager_.getStyleVector(
                        static_cast<int>(chunk_effective_count));
            std::vector<float> chunk_audio =
                runInference(chunk_tokens, chunk_style, kokoro_speed);
            const bool has_next = chunk_index + 1 < model_chunks.size();
            if (model_chunks.size() > 1) {
                const bool follows_forced_boundary =
                    chunk_index > 0 &&
                    model_chunks[chunk_index - 1].boundary ==
                        BoundaryKind::kForced;
                trimExcessLeadingSilence(
                    chunk_audio,
                    SAMPLE_RATE,
                    follows_forced_boundary ? 20 : 10);
                if (has_next) {
                    const size_t retained_silence =
                        model_chunk.boundary == BoundaryKind::kForced ? 20 : 0;
                    trimExcessTrailingSilence(
                        chunk_audio, SAMPLE_RATE, retained_silence);
                } else {
                    trimExcessTrailingSilence(chunk_audio, SAMPLE_RATE, 150);
                }
            }

            if (chunk_index == 0 ||
                model_chunks[chunk_index - 1].boundary !=
                    BoundaryKind::kForced) {
                audio_samples.insert(
                    audio_samples.end(), chunk_audio.begin(), chunk_audio.end());
            } else {
                appendWithCrossfade(
                    audio_samples, chunk_audio, SAMPLE_RATE / 100);  // 10 ms
            }
            if (has_next) {
                if (model_chunk.boundary == BoundaryKind::kSentence) {
                    appendSilence(audio_samples, SAMPLE_RATE, 180);
                } else if (
                    model_chunk.boundary == BoundaryKind::kSecondary) {
                    appendSilence(audio_samples, SAMPLE_RATE, 100);
                }
            }
            // SpaceMIT EP retains part of its compiled graph allocation
            // process-wide. Recreating the session for every chunk increases
            // peak RSS and can OOM on the second chunk. Keep one warmed session
            // for the bounded chunks instead.
        }
        const auto inference_end = std::chrono::high_resolution_clock::now();

        if (audio_samples.empty()) {
            return ErrorInfo::error(
                ErrorCode::SYNTHESIS_FAILED,
                "Kokoro ONNX inference produced no audio");
        }

        // Step 4: Audio post-processing
        audio::AudioProcessConfig audio_config;
        audio_config.target_rms = config_.target_rms;
        audio_config.compression_ratio = config_.compression_ratio;
        audio_config.use_rms_norm = config_.use_rms_norm;
        audio_config.remove_clicks = config_.remove_clicks;

        audio_samples = audio::processAudio(audio_samples, audio_config);
        if (config_.remove_clicks) {
            audio_samples = smoothNarrowImpulses(audio_samples);
        }
        const auto postprocess_end = std::chrono::high_resolution_clock::now();
        if (profile) {
            const auto frontend_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                frontend_end - start_time).count() / 1000.0;
            const auto inference_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                inference_end - frontend_end).count() / 1000.0;
            const auto postprocess_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                postprocess_end - inference_end).count() / 1000.0;
            std::cerr << "[KokoroProfile] frontend=" << frontend_ms
                        << "ms inference=" << inference_ms
                        << "ms postprocess=" << postprocess_ms << "ms" << std::endl;
        }

        // Record timing
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        // Fill result
        result.audio = AudioChunk::fromFloat(audio_samples, SAMPLE_RATE, true);
        result.audio_duration_ms = result.audio.getDurationMs();
        result.processing_time_ms = duration.count();
        result.calculateRTF();
        result.success = true;

        // Add sentence info
        SentenceInfo sentence;
        sentence.text = text;
        sentence.begin_time_ms = 0;
        sentence.end_time_ms = result.audio_duration_ms;
        sentence.is_final = true;
        result.sentences.push_back(sentence);

        // Trigger callback if set
        if (callback_) {
            notifyAudioChunk(result.audio);
        }

        return ErrorInfo::ok();
    } catch (const std::exception& e) {
        return ErrorInfo::error(ErrorCode::SYNTHESIS_FAILED,
            std::string("Kokoro synthesis failed: ") + e.what());
    }
}

ErrorInfo KokoroBackend::setSpeed(float speed) {
    if (speed <= 0.0f || speed > 10.0f) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "Speed must be > 0 and <= 10.0");
    }
    current_speed_ = speed;
    return ErrorInfo::ok();
}

ErrorInfo KokoroBackend::updateLexicon(
    const std::vector<LexiconEntry>& entries) {
    if (!initialized_) {
        return ErrorInfo::error(
            ErrorCode::NOT_INITIALIZED, "Backend not initialized");
    }
    return updateLanguageLexicon(entries);
}

// =============================================================================
// Protected / Private helpers
// =============================================================================

std::string KokoroBackend::getModelDir() const {
    std::string model_dir = config_.model_dir;
    if (model_dir.empty()) {
        model_dir = "~/.cache/models/tts/kokoro-tts";
    }
    // Expand ~
    if (!model_dir.empty() && model_dir[0] == '~') {
        const char* home = getenv("HOME");
        if (home) {
            model_dir = std::string(home) + model_dir.substr(1);
        }
    }
    return model_dir;
}

std::vector<float> KokoroBackend::runInference(
    const std::vector<int64_t>& token_ids,
    const std::vector<float>& style_vector,
    float speed) {
    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // Input 1: input_ids [1, seq_len]
    std::vector<int64_t> ids_shape = {1, static_cast<int64_t>(token_ids.size())};
    auto ids_tensor = Ort::Value::CreateTensor<int64_t>(
        memory_info,
        const_cast<int64_t*>(token_ids.data()),
        token_ids.size(),
        ids_shape.data(),
        ids_shape.size());

    // Input 2: style [1, 256]
    std::vector<int64_t> style_shape = {1, KokoroVoiceManager::STYLE_DIM};
    auto style_tensor = Ort::Value::CreateTensor<float>(
        memory_info,
        const_cast<float*>(style_vector.data()),
        style_vector.size(),
        style_shape.data(),
        style_shape.size());

    // Input 3: speed [1]
    std::vector<float> speed_data = {speed};
    std::vector<int64_t> speed_shape = {1};
    auto speed_tensor = Ort::Value::CreateTensor<float>(
        memory_info,
        speed_data.data(),
        speed_data.size(),
        speed_shape.data(),
        speed_shape.size());

    // Run inference
    Ort::AllocatorWithDefaultOptions allocator;
    auto ids_name = session_->GetInputNameAllocated(0, allocator);
    auto style_name = session_->GetInputNameAllocated(1, allocator);
    auto speed_name = session_->GetInputNameAllocated(2, allocator);
    auto waveform_name = session_->GetOutputNameAllocated(0, allocator);
    const char* input_names[] = {
        ids_name.get(), style_name.get(), speed_name.get()};
    const char* output_names[] = {waveform_name.get()};

    std::vector<Ort::Value> input_tensors;
    input_tensors.push_back(std::move(ids_tensor));
    input_tensors.push_back(std::move(style_tensor));
    input_tensors.push_back(std::move(speed_tensor));

    std::lock_guard<std::mutex> lock(inference_mutex_);
    auto output_tensors = session_->Run(
        Ort::RunOptions{nullptr},
        input_names, input_tensors.data(), 3,
        output_names, 1);

    // Extract audio output [1, num_samples]
    float* audio_data = output_tensors[0].GetTensorMutableData<float>();
    auto audio_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();

    size_t num_samples = 1;
    for (auto dim : audio_shape) {
        num_samples *= static_cast<size_t>(dim);
    }

    return std::vector<float>(audio_data, audio_data + num_samples);
}

}  // namespace tts
