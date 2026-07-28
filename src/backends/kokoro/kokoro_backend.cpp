/* Copyright (C) 2025 SpacemiT Co., Ltd.
    * SPDX-License-Identifier: Apache-2.0 */

#include "backends/kokoro/kokoro_backend.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#ifdef USE_SPACEMIT_EP
#include "spacemit_ort_env.h"
#endif

#include <fcntl.h>
#include <unistd.h>

#if defined(__linux__)
#include <malloc.h>
#endif

#include <chrono>
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
    if (v == "spacemit" || v == "spacemit_ep" || v == "ep") return "spacemit";
    if (v == "auto") return "auto";
    return "cpu";
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

std::vector<std::string> splitAtPreferredPunctuation(const std::string& text) {
    static const std::vector<std::string> boundaries = {
        "。", "！", "？", "；", ".", "!", "?", ";", "，", "、", "：", ",", ":"};

    std::vector<std::string> units;
    size_t unit_start = 0;
    size_t cursor = 0;
    while (cursor < text.size()) {
        size_t boundary_size = 0;
        for (const auto& boundary : boundaries) {
            if (text.compare(cursor, boundary.size(), boundary) == 0) {
                boundary_size = boundary.size();
                break;
            }
        }

        if (boundary_size == 0) {
            const unsigned char value = static_cast<unsigned char>(text[cursor]);
            if (value >= 0xF0) {
                cursor += 4;
            } else if (value >= 0xE0) {
                cursor += 3;
            } else if (value >= 0xC0) {
                cursor += 2;
            } else {
                ++cursor;
            }
            continue;
        }

        cursor += boundary_size;
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor]))) {
            ++cursor;
        }
        units.push_back(text.substr(unit_start, cursor - unit_start));
        unit_start = cursor;
    }

    if (unit_start < text.size()) {
        units.push_back(text.substr(unit_start));
    }
    return units;
}

}  // namespace

namespace tts {

// =============================================================================
// Construction / Destruction
// =============================================================================

KokoroBackend::KokoroBackend(BackendType type) : type_(type), initialized_(false), current_speed_(1.0f) {}

KokoroBackend::~KokoroBackend() { shutdown(); }

// =============================================================================
// Lifecycle
// =============================================================================

ErrorInfo KokoroBackend::initialize(const TtsConfig& config) {
    if (initialized_) {
        return ErrorInfo::error(ErrorCode::ALREADY_STARTED, "Backend already initialized");
    }

    config_ = config;

    // Resolve model directory
    std::string model_dir = getModelDir();

    // Auto-download language-specific model package if needed (tar.gz + extract)
    KokoroModelDownloader downloader(model_dir);
    if (!downloader.ensureModelsExist(getLanguage())) {
        return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND, "Failed to download Kokoro models to: " + model_dir);
    }

    const std::string subdir = getModelSubdir();

    // Load voice (skipped in token-injection mode, where style comes from the file)
    const bool inject_mode = (std::getenv("KOKORO_TOKEN_IDS_FILE") != nullptr);
    if (!inject_mode) {
        const std::string voice_name =
            (config.voice.empty() || config.voice == "default")
            ? getVoiceName()
            : config.voice;
        const std::string voice_base =
            model_dir + "/" + subdir + "/voices/" + voice_name;
        std::string voice_path = voice_base + ".bin";
        if (!fs::exists(voice_path)) {
            voice_path = voice_base + ".npy";
        }
        if (!fs::exists(voice_path)) {
            return ErrorInfo::error(
                ErrorCode::MODEL_NOT_FOUND,
                "Kokoro voice file not found (.bin or .npy): " + voice_base);
        }
        if (!voice_manager_.loadVoice(voice_path)) {
            return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND, "Failed to load voice file: " + voice_path);
        }
    }

    try {
        // Initialize ONNX Runtime (suppress stderr warnings)
        std::string model_path = model_dir + "/" + subdir + "/" + getModelFile();
        if (const char* mp = std::getenv("KOKORO_MODEL_PATH")) model_path = mp;
        int stderr_fd = dup(STDERR_FILENO);
        int devnull_fd = open("/dev/null", O_WRONLY);
        dup2(devnull_fd, STDERR_FILENO);

        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "KokoroBackend");

        // Restore stderr
        dup2(stderr_fd, STDERR_FILENO);
        close(stderr_fd);
        close(devnull_fd);

        const int cpu_threads = config.num_threads > 0 ? config.num_threads : getEnvInt("SPACEMIT_TTS_CPU_THREADS", 2);
        const int ep_threads = getEnvInt("SPACEMIT_TTS_EP_THREADS", 4);

        // Resolve provider: config.provider -> environment -> build default.
        std::string provider = normalizeProvider(config.provider);
        if (provider == "auto") {
            const char* env_p = std::getenv("SPACEMIT_TTS_PROVIDER");
            if (env_p) provider = normalizeProvider(env_p);
        }
        if (provider == "auto") {
#ifdef USE_SPACEMIT_EP
            provider = "spacemit";
#else
            provider = "cpu";
#endif
        }
        bool use_ep = (provider == "spacemit");

        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(use_ep ? ep_threads : cpu_threads);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef USE_SPACEMIT_EP
        if (use_ep) {
        std::unordered_map<std::string, std::string> ep_opts = {
            {"SPACEMIT_EP_INTRA_THREAD_NUM", std::to_string(ep_threads)},
            {"SPACEMIT_EP_DISABLE_OP_TYPE_FILTER", "Resize"},
            {"SPACEMIT_EP_DISABLE_OP_NAME_FILTER", getConvFallbackFilter()}};
        if (const char* ov = std::getenv("SPACEMIT_EP_DISABLE_OP_TYPE_FILTER")) {
            ep_opts["SPACEMIT_EP_DISABLE_OP_TYPE_FILTER"] = ov;
        }
        if (const char* ov = std::getenv("SPACEMIT_EP_DISABLE_OP_NAME_FILTER")) {
                ep_opts["SPACEMIT_EP_DISABLE_OP_NAME_FILTER"] = ov;
            }
            Ort::Status st = Ort::SessionOptionsSpaceMITEnvInit(session_options, ep_opts);
            if (st.IsOK()) {
                std::cout << "[Kokoro] SpaceMIT EP initialized (ep_threads=" << ep_threads << ")" << std::endl;
            } else {
                std::cerr << "[Kokoro] SpaceMIT EP init failed: " << st.GetErrorMessage() << ", fallback to CPU"
                            << std::endl;
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
            std::cerr << "[Kokoro] SpaceMIT EP requested but binary built without USE_SPACEMIT_EP, using CPU"
                        << std::endl;
            use_ep = false;
        }
#if defined(__riscv) || defined(__riscv__)
        session_options.DisableMemPattern();
        session_options.DisableCpuMemArena();
#endif
#endif

        using_spacemit_ep_ = use_ep;
        std::cout << "[Kokoro] Provider: " << (use_ep ? "spacemit" : "cpu") << std::endl;
        session_ = std::make_unique<Ort::Session>(*env_, model_path.c_str(), session_options);

        ErrorInfo lang_err = initializeLanguageSpecific(config);
        if (!lang_err.isOk()) {
            return lang_err;
        }

        if (config.speech_rate <= 0.0f || config.speech_rate > 10.0f) {
            return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "speech_rate must be > 0 and <= 10.0");
        }
        current_speed_ = config.speech_rate;

        if (config.enable_warmup && !inject_mode) {
            std::cout << "[Kokoro] Warming up model..." << std::endl;
            auto start = std::chrono::high_resolution_clock::now();

            int warm_len = getEnvInt("SPACEMIT_TTS_WARMUP_TOKENS", WARMUP_TOKEN_LENGTH);
            if (warm_len < 4) warm_len = 4;
            if (warm_len > MAX_TOKEN_LENGTH) warm_len = MAX_TOKEN_LENGTH;
            std::vector<int64_t> warm_tokens;
            warm_tokens.reserve(warm_len);
            warm_tokens.push_back(0);
            for (int i = 0; i < warm_len - 2; ++i) {
                warm_tokens.push_back(43 + (i % 26));
            }
            warm_tokens.push_back(0);
            auto style = voice_manager_.getStyleVector(static_cast<int>(warm_tokens.size()) - 2);

            const int warmup_runs = getEnvInt("SPACEMIT_TTS_WARMUP_RUNS", 1);
            for (int i = 0; i < warmup_runs; ++i) {
                runInference(warm_tokens, style, 1.0f / current_speed_);
            }

            auto end = std::chrono::high_resolution_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            std::cout << "[Kokoro] Model warmed up in " << dur.count() << "ms (" << warmup_runs << " runs, "
                        << warm_tokens.size() << " tokens)" << std::endl;
        }

        initialized_ = true;

        std::cout << "[Kokoro] Backend initialized successfully (" << backendTypeToString(type_) << ")" << std::endl;
        return ErrorInfo::ok();
    } catch (const std::exception& e) {
        return ErrorInfo::error(
            ErrorCode::MODEL_NOT_FOUND, std::string("Failed to initialize Kokoro model: ") + e.what());
    }
}

void KokoroBackend::shutdown() {
    if (initialized_) {
        shutdownLanguageSpecific();
        session_.reset();
        env_.reset();
        initialized_ = false;
    }
    using_spacemit_ep_ = false;
    releaseUnusedHeapMemory();
}

bool KokoroBackend::isInitialized() const { return initialized_; }

// =============================================================================
// Backend Info
// =============================================================================

BackendType KokoroBackend::getType() const { return type_; }

std::string KokoroBackend::getName() const { return std::string("Kokoro-TTS (") + backendTypeToString(type_) + ")"; }

std::string KokoroBackend::getVersion() const { return "1.0.0"; }

bool KokoroBackend::supportsStreaming() const { return false; }

int KokoroBackend::getNumSpeakers() const { return 1; }

int KokoroBackend::getSampleRate() const { return SAMPLE_RATE; }

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

    try {
        // Optional token-injection path (frontend generated externally, e.g. Python).
        // If KOKORO_TOKEN_IDS_FILE is set, read pregenerated token_ids (line 1) and
        // style vector (line 2, 256 floats), bypassing the C++ frontend/voice.
        std::vector<int64_t> token_ids;
        std::vector<float> injected_style;
        const char* tk_file = std::getenv("KOKORO_TOKEN_IDS_FILE");
        if (tk_file) {
            std::ifstream tf(tk_file);
            if (!tf) {
                return ErrorInfo::error(
                    ErrorCode::INVALID_TEXT, std::string("cannot open KOKORO_TOKEN_IDS_FILE: ") + tk_file);
            }
            std::string line1, line2;
            std::getline(tf, line1);
            std::getline(tf, line2);
            {
                std::istringstream is(line1);
                long v;
                while (is >> v) token_ids.push_back((int64_t)v);
            }
            {
                std::istringstream is(line2);
                float v;
                while (is >> v) injected_style.push_back(v);
            }
            if ((int)injected_style.size() != KokoroVoiceManager::STYLE_DIM) {
                return ErrorInfo::error(ErrorCode::INVALID_TEXT, "KOKORO_TOKEN_IDS_FILE style dim != 256");
            }
        } else {
            // Step 1: Convert to token IDs via the language-specific frontend
            token_ids = textToTokenIds(text);

            if (token_ids.empty()) {
                result.audio = AudioChunk::fromFloat({}, SAMPLE_RATE, true);
                result.success = true;
                return ErrorInfo::ok();
            }
        }

        // [DIAG] optional token dump for frontend comparison vs misaki.
        // Appends one line of ids per call (so interactive/batch mode aligns with
        // input order), and skips inference entirely.
        if (const char* df = std::getenv("KOKORO_DUMP_TOKENS")) {
            std::ofstream of(df, std::ios::app);
            for (size_t i = 0; i < token_ids.size(); ++i) {
                of << token_ids[i] << (i + 1 < token_ids.size() ? ' ' : '\n');
            }
            if (!tk_file) {
                if (const char* chunk_file = std::getenv("KOKORO_DUMP_CHUNKS")) {
                    std::ofstream chunk_stream(chunk_file, std::ios::app);
                    for (const auto& chunk : buildInferenceChunks(text, token_ids)) {
                        for (size_t i = 0; i < chunk.size(); ++i) {
                            chunk_stream << chunk[i] << (i + 1 < chunk.size() ? ' ' : '\n');
                        }
                    }
                }
            }
            result.audio = AudioChunk::fromFloat({}, SAMPLE_RATE, true);
            result.success = true;
            return ErrorInfo::ok();
        }

        // Step 3: Run ONNX inference. Kokoro uses inverse speed: 1.0/speech_rate
        float kokoro_speed = 1.0f / current_speed_;
        std::vector<float> audio_samples;
        if (tk_file) {
            audio_samples = runInference(token_ids, injected_style, kokoro_speed);
        } else {
            const auto chunks = buildInferenceChunks(text, token_ids);
            for (const auto& chunk : chunks) {
                // kokoro-onnx indexes voice[len(tokens)] before adding BOS/EOS,
                // so pass the effective count without the two padding tokens.
                const auto style = voice_manager_.getStyleVector(std::max(1, static_cast<int>(chunk.size()) - 2));
                auto chunk_audio = runInference(chunk, style, kokoro_speed);
                audio_samples.insert(audio_samples.end(), chunk_audio.begin(), chunk_audio.end());
            }
        }

        if (audio_samples.empty()) {
            result.audio = AudioChunk::fromFloat({}, SAMPLE_RATE, true);
            result.success = true;
            return ErrorInfo::ok();
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
        return ErrorInfo::error(ErrorCode::SYNTHESIS_FAILED, std::string("Kokoro synthesis failed: ") + e.what());
    }
}

ErrorInfo KokoroBackend::setSpeed(float speed) {
    if (speed <= 0.0f || speed > 10.0f) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "Speed must be > 0 and <= 10.0");
    }
    current_speed_ = speed;
    return ErrorInfo::ok();
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

std::vector<std::vector<int64_t>> KokoroBackend::buildInferenceChunks(
    const std::string& text, const std::vector<int64_t>& full_token_ids) {
    int shape_limit = MAX_TOKEN_LENGTH;
    if (using_spacemit_ep_) {
        // Keep EP requests within the shape compiled by the 128-token warmup.
        // Larger dynamic shapes accumulate EP workspace/TCM across requests and
        // can OOM on the 8 GiB K3; ORT CPU does not have this EP constraint.
        shape_limit = getEnvInt("SPACEMIT_TTS_WARMUP_TOKENS", WARMUP_TOKEN_LENGTH);
        if (shape_limit < 4) shape_limit = 4;
        if (shape_limit > MAX_TOKEN_LENGTH) shape_limit = MAX_TOKEN_LENGTH;
    }

    if (static_cast<int>(full_token_ids.size()) <= shape_limit) {
        return {full_token_ids};
    }

    const auto units = splitAtPreferredPunctuation(text);
    std::vector<std::vector<int64_t>> chunks;
    std::string pending_text;

    const auto appendTokenSlices = [&](const std::vector<int64_t>& ids) {
        const size_t effective_begin = 1;
        const size_t effective_end = ids.size() - 1;
        const size_t effective_limit = static_cast<size_t>(shape_limit - 2);
        for (size_t begin = effective_begin; begin < effective_end; begin += effective_limit) {
            const size_t end = std::min(begin + effective_limit, effective_end);
            std::vector<int64_t> chunk;
            chunk.reserve(end - begin + 2);
            chunk.push_back(0);
            chunk.insert(chunk.end(), ids.begin() + begin, ids.begin() + end);
            chunk.push_back(0);
            chunks.push_back(std::move(chunk));
        }
    };

    const auto flushPending = [&]() {
        if (pending_text.empty()) return;
        const auto ids = textToTokenIds(pending_text);
        if (static_cast<int>(ids.size()) <= shape_limit) {
            chunks.push_back(ids);
        } else {
            std::cerr << "[Kokoro] Warning: one punctuation unit exceeds the " << shape_limit
                        << "-token warmup shape; applying token-safe fallback." << std::endl;
            appendTokenSlices(ids);
        }
        pending_text.clear();
    };

    for (const auto& unit : units) {
        const std::string candidate = pending_text + unit;
        const auto candidate_ids = textToTokenIds(candidate);
        if (static_cast<int>(candidate_ids.size()) <= shape_limit) {
            pending_text = candidate;
            continue;
        }

        flushPending();
        const auto unit_ids = textToTokenIds(unit);
        if (static_cast<int>(unit_ids.size()) <= shape_limit) {
            pending_text = unit;
        } else {
            std::cerr << "[Kokoro] Warning: one punctuation unit exceeds the " << shape_limit
                        << "-token warmup shape; applying token-safe fallback." << std::endl;
            appendTokenSlices(unit_ids);
        }
    }
    flushPending();

    if (chunks.empty()) {
        appendTokenSlices(full_token_ids);
    }
    std::cout << "[Kokoro] Long text split into " << chunks.size()
                << " punctuation-aligned chunks (shape limit=" << shape_limit << ")." << std::endl;
    return chunks;
}

std::vector<float> KokoroBackend::runInference(
    const std::vector<int64_t>& token_ids, const std::vector<float>& style_vector, float speed) {
    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // Input 1: input_ids [1, seq_len]
    std::vector<int64_t> ids_shape = {1, static_cast<int64_t>(token_ids.size())};
    auto ids_tensor = Ort::Value::CreateTensor<int64_t>(
        memory_info, const_cast<int64_t*>(token_ids.data()), token_ids.size(), ids_shape.data(), ids_shape.size());

    // Input 2: style [1, 256]
    std::vector<int64_t> style_shape = {1, KokoroVoiceManager::STYLE_DIM};
    auto style_tensor = Ort::Value::CreateTensor<float>(memory_info, const_cast<float*>(style_vector.data()),
        style_vector.size(), style_shape.data(), style_shape.size());

    // Input 3: speed [1]
    std::vector<float> speed_data = {speed};
    std::vector<int64_t> speed_shape = {1};
    auto speed_tensor = Ort::Value::CreateTensor<float>(
        memory_info, speed_data.data(), speed_data.size(), speed_shape.data(), speed_shape.size());

    // Run inference
    const char* input_names[] = {"input_ids", "style", "speed"};
    const char* output_names[] = {"waveform"};

    std::vector<Ort::Value> input_tensors;
    input_tensors.push_back(std::move(ids_tensor));
    input_tensors.push_back(std::move(style_tensor));
    input_tensors.push_back(std::move(speed_tensor));

    std::lock_guard<std::mutex> lock(inference_mutex_);
    auto output_tensors =
        session_->Run(Ort::RunOptions{nullptr}, input_names, input_tensors.data(), 3, output_names, 1);

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
