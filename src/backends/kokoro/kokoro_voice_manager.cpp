/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#include "backends/kokoro/kokoro_voice_manager.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace tts {

bool KokoroVoiceManager::loadVoice(const std::string& voice_path) {
    std::ifstream file(voice_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[KokoroVoiceManager] Failed to open voice file: " << voice_path << std::endl;
        return false;
    }

    std::streamoff data_offset = 0;
    char magic[6] = {};
    file.read(magic, sizeof(magic));
    const bool is_npy = file.gcount() == static_cast<std::streamsize>(sizeof(magic)) &&
        std::memcmp(magic, "\x93NUMPY", sizeof(magic)) == 0;
    file.clear();
    file.seekg(0, std::ios::beg);

    if (is_npy) {
        file.seekg(6, std::ios::beg);
        uint8_t major = 0;
        uint8_t minor = 0;
        file.read(reinterpret_cast<char*>(&major), 1);
        file.read(reinterpret_cast<char*>(&minor), 1);
        (void)minor;
        uint32_t header_len = 0;
        if (major == 1) {
            uint16_t short_len = 0;
            file.read(reinterpret_cast<char*>(&short_len), sizeof(short_len));
            header_len = short_len;
        } else {
            file.read(reinterpret_cast<char*>(&header_len), sizeof(header_len));
        }
        std::string header(header_len, '\0');
        file.read(header.data(), header.size());
        if (!file || header.find("'descr': '<f4'") == std::string::npos ||
            header.find("256") == std::string::npos) {
            std::cerr << "[KokoroVoiceManager] Unsupported npy voice header: "
                        << voice_path << std::endl;
            return false;
        }
        data_offset = file.tellg();
    }

    file.seekg(0, std::ios::end);
    const std::streamoff file_size = file.tellg();
    const std::streamoff payload_size = file_size - data_offset;
    file.seekg(data_offset, std::ios::beg);

    size_t num_floats = static_cast<size_t>(payload_size) / sizeof(float);
    if (num_floats == 0 || num_floats % STYLE_DIM != 0) {
        std::cerr << "[KokoroVoiceManager] Invalid voice payload size: " << payload_size
            << " bytes (not divisible by " << STYLE_DIM * sizeof(float) << ")" << std::endl;
        return false;
    }

    style_data_.resize(num_floats);
    file.read(reinterpret_cast<char*>(style_data_.data()), payload_size);

    if (!file.good()) {
        std::cerr << "[KokoroVoiceManager] Failed to read voice file: " << voice_path << std::endl;
        style_data_.clear();
        return false;
    }

    num_rows_ = static_cast<int>(num_floats / STYLE_DIM);
    std::cout << "[KokoroVoiceManager] Loaded voice: " << voice_path
        << " (" << num_rows_ << " style vectors)" << std::endl;

    return true;
}

std::vector<float> KokoroVoiceManager::getStyleVector(int token_len) const {
    if (style_data_.empty()) {
        return std::vector<float>(STYLE_DIM, 0.0f);
    }

    // Official KPipeline uses pack[len(phonemes)-1].
    int row = std::min(token_len - 1, num_rows_ - 1);
    row = std::max(row, 0);

    size_t offset = static_cast<size_t>(row) * STYLE_DIM;
    return std::vector<float>(style_data_.begin() + offset,
        style_data_.begin() + offset + STYLE_DIM);
}

}  // namespace tts
