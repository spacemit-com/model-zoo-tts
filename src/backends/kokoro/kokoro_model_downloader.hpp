/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef KOKORO_MODEL_DOWNLOADER_HPP
#define KOKORO_MODEL_DOWNLOADER_HPP

#include <string>

namespace tts {

// =============================================================================
// KokoroModelDownloader - Kokoro model auto-downloader
// =============================================================================
//
// Downloads and extracts the per-language Kokoro model tar.gz package (matches
// the matcha download approach).
//   en -> kokoro-v1.0-en.tar.gz   (kokoro-v1.0-en/)
//   zh -> kokoro-v1.1-zh.tar.gz   (kokoro-v1.1-zh/)
// Source: https://archive.spacemit.com/spacemit-ai/model_zoo/tts/kokoro/
// Cache dir: ~/.cache/models/tts/kokoro-tts/
//

class KokoroModelDownloader {
public:
    static constexpr const char* BASE_URL =
        "https://archive.spacemit.com/spacemit-ai/model_zoo/tts/kokoro";

    explicit KokoroModelDownloader(const std::string& cache_dir = "");
    ~KokoroModelDownloader() = default;

    /// @brief Ensure the model files for a language exist, downloading and
    ///        extracting them if not.
    /// @param language "en" or "zh"
    /// @return true if all files are ready
    bool ensureModelsExist(const std::string& language);

    std::string getCacheDir() const { return cache_dir_; }

private:
    std::string cache_dir_;  // ~/.cache/models/tts/kokoro-tts/

    bool ensureCacheDir();
    bool downloadFile(const std::string& url, const std::string& dest_path);
    bool downloadLanguageModel(const std::string& language);
    bool extractTarArchive(const std::string& archive_path);
    bool validateRequiredFiles(const std::string& language) const;

    // Per-language archive name, subdirectory name, and model file relative path.
    std::string archiveName(const std::string& language) const;
    std::string subdirName(const std::string& language) const;
    std::string modelFileRel(const std::string& language) const;
};

}  // namespace tts

#endif  // KOKORO_MODEL_DOWNLOADER_HPP
