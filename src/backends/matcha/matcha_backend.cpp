/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#include "backends/matcha/matcha_backend.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>  // NOLINT(build/c++17)
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <regex>
#include <string>
#include <vector>

#include "audio/audio_processor.hpp"
#include "backends/matcha/tts_model_downloader.hpp"
#include "text/text_normalizer.hpp"
#include "text/token_utils.hpp"
#include "vocoder/vocoder.hpp"

namespace fs = std::filesystem;

namespace {
struct PcloseDeleter {
    void operator()(FILE* p) const { if (p) pclose(p); }
};
}  // namespace

namespace tts {

// =============================================================================
// 构造与析构
// =============================================================================

MatchaBackend::MatchaBackend(BackendType type)
    : type_(type)
    , initialized_(false)
    , current_speed_(1.0f)
    , current_speaker_(0) {
}

MatchaBackend::~MatchaBackend() {
    shutdown();
}

// =============================================================================
// 生命周期管理
// =============================================================================

ErrorInfo MatchaBackend::initialize(const TtsConfig& config) {
    if (initialized_) {
        return ErrorInfo::error(ErrorCode::ALREADY_STARTED, "Backend already initialized");
    }

    config_ = config;
    createInternalConfig();

    // 检查并下载模型（如果需要）
    tts::TTSModelDownloader downloader;
    std::string language = (type_ == BackendType::MATCHA_ZH) ? "zh" :
        (type_ == BackendType::MATCHA_EN) ? "en" : "zh-en";
    if (!downloader.ensureModelsExist(language)) {
        return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND,
            "Failed to download TTS models for language: " + language);
    }

    try {
        // 初始化 ONNX Runtime
        // 暂时抑制 stderr 避免 ONNX schema 警告
        int stderr_fd = dup(STDERR_FILENO);
        int devnull_fd = open("/dev/null", O_WRONLY);
        dup2(devnull_fd, STDERR_FILENO);

        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "MatchaBackend");

        // 恢复 stderr
        dup2(stderr_fd, STDERR_FILENO);
        close(stderr_fd);
        close(devnull_fd);

        Ort::SessionOptions session_options;
        session_options.SetIntraOpNumThreads(3);
        session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        // RISC-V 特定: 禁用内存池以避免对齐问题
        #if defined(__riscv) || defined(__riscv__)
        session_options.DisableMemPattern();
        session_options.DisableCpuMemArena();
        #endif

        // 加载声学模型
        acoustic_model_ = std::make_unique<Ort::Session>(
            *env_, internal_config_.acoustic_model_path.c_str(), session_options);

        // 加载声码器模型
        vocoder_model_ = std::make_unique<Ort::Session>(
            *env_, internal_config_.vocoder_path.c_str(), session_options);

        // 加载 token 映射
        if (type_ == BackendType::MATCHA_ZH_EN) {
            token_to_id_ = text::readZhEnTokenToIdMap(internal_config_.tokens_path);
        } else {
            token_to_id_ = text::readTokenToIdMap(internal_config_.tokens_path);
        }

        // 提取模型元数据
        extractModelMetadata();

        // 派生类特有的初始化
        auto err = initializeLanguageSpecific(config);
        if (!err.isOk()) {
            return err;
        }

        // 预热模型
        if (config.enable_warmup) {
            warmUpModels();
        }

        initialized_ = true;
        current_speed_ = config.speech_rate;
        current_speaker_ = config.speaker_id;

        return ErrorInfo::ok();
    } catch (const std::exception& e) {
        return ErrorInfo::error(ErrorCode::MODEL_NOT_FOUND,
            std::string("Failed to initialize TTS model: ") + e.what());
    }
}

void MatchaBackend::shutdown() {
    if (initialized_) {
        shutdownLanguageSpecific();
        acoustic_model_.reset();
        vocoder_model_.reset();
        env_.reset();
        token_to_id_.clear();
        initialized_ = false;
    }
}

bool MatchaBackend::isInitialized() const {
    return initialized_;
}

// =============================================================================
// 后端信息
// =============================================================================

BackendType MatchaBackend::getType() const {
    return type_;
}

std::string MatchaBackend::getName() const {
    switch (type_) {
        case BackendType::MATCHA_ZH:    return "Matcha-TTS (Chinese)";
        case BackendType::MATCHA_EN:    return "Matcha-TTS (English)";
        case BackendType::MATCHA_ZH_EN: return "Matcha-TTS (Chinese-English)";
        default:                        return "Matcha-TTS";
    }
}

std::string MatchaBackend::getVersion() const {
    return "2.0.0";  // 新版本号表示重构后的后端
}

bool MatchaBackend::supportsStreaming() const {
    return false;  // 当前版本不支持流式
}

int MatchaBackend::getNumSpeakers() const {
    return num_speakers_;
}

int MatchaBackend::getSampleRate() const {
    // 返回输出采样率（如果有重采样）
    if (config_.output_sample_rate > 0) {
        return config_.output_sample_rate;
    }
    return sample_rate_;
}

// =============================================================================
// 离线合成
// =============================================================================

ErrorInfo MatchaBackend::synthesize(const std::string& text, SynthesisResult& result) {
    if (!initialized_) {
        return ErrorInfo::error(ErrorCode::NOT_INITIALIZED, "Backend not initialized");
    }

    if (text.empty()) {
        return ErrorInfo::error(ErrorCode::INVALID_TEXT, "Empty text");
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        // 0. 文本规范化 (处理数字、公式、货币、日期等)
        text::Language norm_lang;
        switch (type_) {
            case BackendType::MATCHA_ZH:
                norm_lang = text::Language::ZH;
                break;
            case BackendType::MATCHA_EN:
                norm_lang = text::Language::EN;
                break;
            case BackendType::MATCHA_ZH_EN:
            default:
                norm_lang = text::Language::AUTO;
                break;
        }
        std::string normalized_text = text::normalizeText(text, norm_lang);

        // 1. 文本转 token IDs (派生类实现)
        std::vector<int64_t> token_ids = textToTokenIds(normalized_text);

        if (token_ids.empty()) {
            result.audio = AudioChunk::fromFloat({}, sample_rate_, true);
            result.success = true;
            return ErrorInfo::ok();
        }

        int output_sample_rate = sample_rate_;
        std::vector<float> audio_samples =
            synthesizeTokenIdsToAudio(token_ids, &output_sample_rate);

        // 记录结束时间
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

        // 填充结果
        result.audio = AudioChunk::fromFloat(audio_samples, output_sample_rate, true);
        result.audio_duration_ms = result.audio.getDurationMs();
        result.processing_time_ms = duration.count();
        result.calculateRTF();
        result.success = true;

        // 添加句子信息
        SentenceInfo sentence;
        sentence.text = text;
        sentence.begin_time_ms = 0;
        sentence.end_time_ms = result.audio_duration_ms;
        sentence.is_final = true;
        result.sentences.push_back(sentence);

        // 触发回调
        if (callback_) {
            notifyAudioChunk(result.audio);
        }

        return ErrorInfo::ok();
    } catch (const std::exception& e) {
        return ErrorInfo::error(ErrorCode::SYNTHESIS_FAILED,
            std::string("Synthesis failed: ") + e.what());
    }
}

ErrorInfo MatchaBackend::synthesizeToFile(const std::string& text, const std::string& file_path) {
    SynthesisResult result;
    auto err = synthesize(text, result);
    if (!err.isOk()) {
        return err;
    }
    return saveToFile(result.audio, file_path);
}

// =============================================================================
// 动态配置
// =============================================================================

ErrorInfo MatchaBackend::setSpeed(float speed) {
    if (speed <= 0.0f || speed > 10.0f) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "Speed must be between 0.1 and 10.0");
    }
    current_speed_ = speed;
    return ErrorInfo::ok();
}

ErrorInfo MatchaBackend::setSpeaker(int speaker_id) {
    if (speaker_id < 0) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "Speaker ID must be non-negative");
    }
    if (speaker_id >= num_speakers_) {
        return ErrorInfo::error(ErrorCode::INVALID_CONFIG, "Speaker ID out of range");
    }
    current_speaker_ = speaker_id;
    return ErrorInfo::ok();
}

// =============================================================================
// 受保护的辅助方法
// =============================================================================

std::string MatchaBackend::getModelDir() const {
    std::string model_dir = config_.model_dir;
    if (model_dir.empty()) {
        model_dir = "~/.cache/models/tts/matcha-tts";
    }
    // 展开 ~
    if (!model_dir.empty() && model_dir[0] == '~') {
        const char* home = getenv("HOME");
        if (home) {
            model_dir = std::string(home) + model_dir.substr(1);
        }
    }
    return model_dir;
}

std::vector<int64_t> MatchaBackend::addBlankTokens(const std::vector<int64_t>& tokens) {
    // Matcha 模型需要在 phoneme 之间插入 blank tokens
    // 使用模型元数据中的 pad_id (遵循 sherpa-onnx 方法)
    std::vector<int64_t> result(tokens.size() * 2 + 1, pad_id_);

    int32_t i = 1;
    for (auto token : tokens) {
        result[i] = token;
        i += 2;
    }

    return result;
}

std::vector<float> MatchaBackend::synthesizeTokenIdsToAudio(
    const std::vector<int64_t>& token_ids,
    int* output_sample_rate) {
    if (output_sample_rate != nullptr) {
        *output_sample_rate = sample_rate_;
    }
    if (token_ids.empty()) {
        return {};
    }

    // 添加 blank tokens (根据后端类型)
    std::vector<int64_t> final_tokens;
    if (usesBlankTokens()) {
        final_tokens = addBlankTokens(token_ids);
    } else {
        final_tokens = token_ids;
    }

    // 运行声学模型
    std::vector<float> mel = runAcousticModel(final_tokens, current_speaker_, current_speed_);
    if (mel.empty()) {
        return {};
    }

    // 运行声码器
    std::vector<float> audio_samples = runVocoder(mel, mel_dim_);

    // 重采样（如果需要）
    if (config_.output_sample_rate > 0 && config_.output_sample_rate != sample_rate_) {
        audio_samples = audio::resampleAudio(audio_samples, sample_rate_, config_.output_sample_rate);
        if (output_sample_rate != nullptr) {
            *output_sample_rate = config_.output_sample_rate;
        }
    }

    return audio_samples;
}

bool MatchaBackend::checkEspeakNgAvailable() {
    std::string command = "echo 'test' | espeak-ng -q --ipa=3 2>/dev/null";

    std::unique_ptr<FILE, PcloseDeleter> pipe(popen(command.c_str(), "r"));
    if (!pipe) {
        return false;
    }

    char buffer[128];
    std::string result;
    if (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }

    int exit_status = pclose(pipe.release());
    return exit_status == 0 && !result.empty();
}

std::string MatchaBackend::processEnglishTextToPhonemes(const std::string& text) {
    if (text.empty()) {
        return "";
    }

    // 转义单引号
    std::string escaped_text = text;
    std::string::size_type pos = 0;
    while ((pos = escaped_text.find("'", pos)) != std::string::npos) {
        escaped_text.replace(pos, 1, "'\"'\"'");
        pos += 5;
    }

    // 使用 espeak-ng 转换为 IPA (使用 en-us 美式英语)
    std::string command = "echo '" + escaped_text + "' | espeak-ng -q --ipa=3 -v en-us";

    std::unique_ptr<FILE, PcloseDeleter> pipe(popen(command.c_str(), "r"));
    if (!pipe) {
        std::cerr << "Error: Failed to run espeak-ng command" << std::endl;
        return "";
    }

    char buffer[4096];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }

    int exit_status = pclose(pipe.release());
    if (exit_status != 0) {
        return "";
    }

    // 清理结果 - 移除换行符和多余空白
    result.erase(std::remove_if(result.begin(), result.end(),
        [](char c) { return c == '\n' || c == '\r'; }), result.end());

    // 替换多个连续空格为单个空格
    std::regex multi_space("\\s+");
    result = std::regex_replace(result, multi_space, " ");

    // 去除首尾空白
    size_t start = result.find_first_not_of(" \t");
    if (start == std::string::npos) {
        return "";
    }
    size_t end = result.find_last_not_of(" \t");
    result = result.substr(start, end - start + 1);

    return result;
}

// =============================================================================
// 私有方法
// =============================================================================

void MatchaBackend::createInternalConfig() {
    std::string model_dir = getModelDir();
    std::string subdir = getModelSubdir();

    internal_config_.acoustic_model_path = model_dir + "/" + subdir + "/model-steps-3.onnx";
    internal_config_.tokens_path = model_dir + "/" + subdir + "/tokens.txt";

    if (type_ == BackendType::MATCHA_ZH) {
        internal_config_.language = "zh";
        internal_config_.lexicon_path = model_dir + "/" + subdir + "/lexicon.txt";
        internal_config_.dict_dir = model_dir + "/" + subdir + "/dict";
        internal_config_.vocoder_path = model_dir + "/vocos-22khz-univ.onnx";
        sample_rate_ = 22050;
    } else if (type_ == BackendType::MATCHA_EN) {
        internal_config_.language = "en";
        internal_config_.lexicon_path = "";
        internal_config_.vocoder_path = model_dir + "/vocos-22khz-univ.onnx";
        sample_rate_ = 22050;
    } else if (type_ == BackendType::MATCHA_ZH_EN) {
        internal_config_.language = "zh-en";
        internal_config_.tokens_path = model_dir + "/" + subdir + "/vocab_tts.txt";
        internal_config_.lexicon_path = "";
        internal_config_.vocoder_path = model_dir + "/vocos-16khz-univ.onnx";
        sample_rate_ = 16000;
    }

    internal_config_.sample_rate = sample_rate_;
    internal_config_.speaker_id = config_.speaker_id;
    internal_config_.length_scale = 1.0f / config_.speech_rate;
    internal_config_.noise_scale = config_.noise_scale;
    internal_config_.output_sample_rate = config_.output_sample_rate;
    internal_config_.target_rms = config_.target_rms;
    internal_config_.compression_ratio = config_.compression_ratio;
    internal_config_.use_rms_norm = config_.use_rms_norm;
    internal_config_.remove_clicks = config_.remove_clicks;
    internal_config_.enable_warmup = config_.enable_warmup;
}

void MatchaBackend::extractModelMetadata() {
    Ort::AllocatorWithDefaultOptions allocator;

    // 读取声学模型元数据
    try {
        Ort::ModelMetadata acoustic_meta = acoustic_model_->GetModelMetadata();

        // 读取 pad_id
        try {
            auto pad_id_value = acoustic_meta.LookupCustomMetadataMapAllocated("pad_id", allocator);
            if (pad_id_value) {
                pad_id_ = std::stoi(pad_id_value.get());
            }
        } catch (...) {
            pad_id_ = 0;
        }
    } catch (...) {
        pad_id_ = 0;
    }

    // 读取 vocoder 元数据
    try {
        Ort::ModelMetadata vocoder_meta = vocoder_model_->GetModelMetadata();

        auto read_meta_int = [&](const char* key, int32_t& value, int32_t default_val) {
            try {
                auto key_alloc = vocoder_meta.LookupCustomMetadataMapAllocated(key, allocator);
                if (key_alloc) {
                    value = std::stoi(key_alloc.get());
                } else {
                    value = default_val;
                }
            } catch (...) {
                value = default_val;
            }
        };

        read_meta_int("n_fft", istft_n_fft_, 1024);
        read_meta_int("hop_length", istft_hop_length_, 256);
        read_meta_int("win_length", istft_win_length_, 1024);
    } catch (...) {
        istft_n_fft_ = 1024;
        istft_hop_length_ = 256;
        istft_win_length_ = 1024;
    }

    mel_dim_ = 80;
    num_speakers_ = 1;
}

void MatchaBackend::warmUpModels() {
    std::cout << "Warming up TTS models..." << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        // 使用小输入预热
        std::vector<int64_t> small_tokens = {1, 2, 3};
        std::vector<int64_t> tokens = usesBlankTokens() ? addBlankTokens(small_tokens) : small_tokens;
        runAcousticModel(tokens, 0, 1.0f);

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
        std::cout << "TTS models warmed up in " << duration.count() << "ms" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Warning: TTS warm-up failed: " << e.what() << std::endl;
    }
}

std::vector<float> MatchaBackend::runAcousticModel(
    const std::vector<int64_t>& tokens, int speaker_id, float speed) {
    std::vector<int64_t> token_shape = {1, static_cast<int64_t>(tokens.size())};
    std::vector<int64_t> length_data = {static_cast<int64_t>(tokens.size())};
    std::vector<int64_t> length_shape = {1};
    std::vector<float> noise_scale_data = {internal_config_.noise_scale};
    std::vector<int64_t> noise_scale_shape = {1};
    std::vector<float> length_scale_data = {internal_config_.length_scale / speed};
    std::vector<int64_t> length_scale_shape = {1};

    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    std::vector<Ort::Value> input_tensors;

    input_tensors.emplace_back(Ort::Value::CreateTensor<int64_t>(
        memory_info, const_cast<int64_t*>(tokens.data()), tokens.size(),
        token_shape.data(), token_shape.size()));

    input_tensors.emplace_back(Ort::Value::CreateTensor<int64_t>(
        memory_info, length_data.data(), 1,
        length_shape.data(), length_shape.size()));

    input_tensors.emplace_back(Ort::Value::CreateTensor<float>(
        memory_info, noise_scale_data.data(), 1,
        noise_scale_shape.data(), noise_scale_shape.size()));

    input_tensors.emplace_back(Ort::Value::CreateTensor<float>(
        memory_info, length_scale_data.data(), 1,
        length_scale_shape.data(), length_scale_shape.size()));

    const char* input_names[] = {"x", "x_length", "noise_scale", "length_scale"};
    const char* output_names[] = {"mel"};

    std::lock_guard<std::mutex> lock(inference_mutex_);
    auto output_tensors = acoustic_model_->Run(
        Ort::RunOptions{nullptr},
        input_names, input_tensors.data(), 4,
        output_names, 1);

    float* mel_data = output_tensors[0].GetTensorMutableData<float>();
    auto mel_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
    size_t mel_size = std::accumulate(mel_shape.begin(), mel_shape.end(),
        static_cast<size_t>(1), std::multiplies<size_t>());

    return std::vector<float>(mel_data, mel_data + mel_size);
}

std::vector<float> MatchaBackend::runVocoder(const std::vector<float>& mel, int mel_dim) {
    int64_t num_frames = mel.size() / mel_dim;
    std::vector<int64_t> input_shape = {1, mel_dim, num_frames};

    auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
        memory_info, const_cast<float*>(mel.data()), mel.size(),
        input_shape.data(), input_shape.size());

    const char* input_names[] = {"mels"};
    const char* output_names[] = {"mag", "x", "y"};

    std::lock_guard<std::mutex> lock(inference_mutex_);
    auto output_tensors = vocoder_model_->Run(
        Ort::RunOptions{nullptr},
        input_names, &input_tensor, 1,
        output_names, 3);

    float* mag_data = output_tensors[0].GetTensorMutableData<float>();
    float* x_data = output_tensors[1].GetTensorMutableData<float>();
    float* y_data = output_tensors[2].GetTensorMutableData<float>();

    auto vocoder_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
    int32_t n_fft_bins = vocoder_shape[1];
    int32_t vocoder_frames = vocoder_shape[2];

    // 重建复数 STFT
    std::vector<float> stft_real(vocoder_frames * n_fft_bins);
    std::vector<float> stft_imag(vocoder_frames * n_fft_bins);

    for (int32_t frame = 0; frame < vocoder_frames; ++frame) {
        for (int32_t bin = 0; bin < n_fft_bins; ++bin) {
            int32_t vocoder_idx = bin * vocoder_frames + frame;
            int32_t stft_idx = frame * n_fft_bins + bin;

            stft_real[stft_idx] = mag_data[vocoder_idx] * x_data[vocoder_idx];
            stft_imag[stft_idx] = mag_data[vocoder_idx] * y_data[vocoder_idx];
        }
    }

    // 使用 vocoder 模块进行 ISTFT
    vocoder::ISTFTConfig istft_config;
    istft_config.n_fft = istft_n_fft_;
    istft_config.hop_length = istft_hop_length_;
    istft_config.win_length = istft_win_length_;

    std::vector<float> audio = vocoder::istft(
        stft_real, stft_imag, vocoder_frames, n_fft_bins, istft_config);

    // 应用音频后处理
    audio::AudioProcessConfig audio_config;
    audio_config.target_rms = internal_config_.target_rms;
    audio_config.compression_ratio = internal_config_.compression_ratio;
    audio_config.compression_threshold = internal_config_.compression_threshold;
    audio_config.use_rms_norm = internal_config_.use_rms_norm;
    audio_config.remove_clicks = internal_config_.remove_clicks;

    audio = audio::processAudio(audio, audio_config);

    return audio;
}

}  // namespace tts
