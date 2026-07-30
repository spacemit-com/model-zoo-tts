/* Copyright (C) 2025 SpacemiT Co., Ltd.
    * SPDX-License-Identifier: Apache-2.0 */

#include "backends/kokoro/kokoro_model_downloader.hpp"

#include <curl/curl.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace tts {

KokoroModelDownloader::KokoroModelDownloader(const std::string& cache_dir) {
    if (!cache_dir.empty()) {
        cache_dir_ = cache_dir;
    } else {
        const char* home = std::getenv("HOME");
        cache_dir_ = home
            ? std::string(home) + "/.cache/models/tts/kokoro-tts"
            : "./.cache/models/tts/kokoro-tts";
    }
    while (cache_dir_.size() > 1 && cache_dir_.back() == '/') {
        cache_dir_.pop_back();
    }
}

std::string KokoroModelDownloader::archiveName(const std::string& language) const {
    if (language == "en") return "kokoro-v1.0-en.tar.gz";
    if (language == "zh") return "kokoro-v1.1-zh.tar.gz";
    return "";
}

std::string KokoroModelDownloader::subdirName(const std::string& language) const {
    if (language == "en") return "kokoro-v1.0-en";
    if (language == "zh") return "kokoro-v1.1-zh";
    return "";
}

std::string KokoroModelDownloader::modelFileRel(const std::string& language) const {
    if (language == "en") return "kokoro-v1.0-en/kokoro-v1.0-en.q.onnx";
    if (language == "zh") return "kokoro-v1.1-zh/kokoro-v1.1-zh.q.onnx";
    return "";
}

namespace {

bool fileReady(const std::string& path, uintmax_t minimum_size) {
    std::error_code ec;
    return fs::is_regular_file(path, ec) && !ec &&
        fs::file_size(path, ec) >= minimum_size && !ec;
}

}  // namespace

bool KokoroModelDownloader::validateRequiredFiles(const std::string& language) const {
    std::vector<std::pair<std::string, uintmax_t>> files;
    if (language == "en") {
        files = {
            {"kokoro-v1.0-en/kokoro-v1.0-en.q.onnx", 1024U * 1024U},
            {"kokoro-v1.0-en/voices/af_heart.bin", 1024U},
            {"kokoro-v1.0-en/us_gold.json", 1024U},
            {"kokoro-v1.0-en/us_silver.json", 1024U},
        };
    } else if (language == "zh") {
        files = {
            {"kokoro-v1.1-zh/kokoro-v1.1-zh.q.onnx", 1024U * 1024U},
            {"kokoro-v1.1-zh/tokenizer.json", 128U},
            {"kokoro-v1.1-zh/config.json", 32U},
            {"kokoro-v1.1-zh/voices/zf_001.npy", 1024U},
            {"kokoro-v1.1-zh/us_gold.json", 1024U},
            {"kokoro-v1.1-zh/us_silver.json", 1024U},
        };
    } else {
        return false;
    }

    bool ready = true;
    for (const auto& file : files) {
        const std::string path = cache_dir_ + "/" + file.first;
        if (!fileReady(path, file.second)) {
            std::cerr << "[Kokoro] Missing or incomplete required file: "
                        << path << std::endl;
            ready = false;
        }
    }
    return ready;
}

bool KokoroModelDownloader::ensureModelsExist(const std::string& language) {
    if (!ensureCacheDir()) {
        return false;
    }

    const std::string subdir = subdirName(language);
    if (subdir.empty()) {
        std::cerr << "[Kokoro] Unsupported language: " << language << std::endl;
        return false;
    }

    if (validateRequiredFiles(language)) {
        std::cout << "[Kokoro] All required files for " << language
                    << " are ready in " << cache_dir_ + "/" + subdir << std::endl;
        return true;
    }

    if (!downloadLanguageModel(language)) {
        std::cerr << "[Kokoro] Failed to download " << language << " model package" << std::endl;
        return false;
    }

    if (!validateRequiredFiles(language)) {
        std::cerr << "[Kokoro] Package validation failed after extracting "
                    << archiveName(language) << std::endl;
        return false;
    }

    std::cout << "[Kokoro] All required files for " << language
                << " are ready in " << cache_dir_ + "/" + subdir << std::endl;
    return true;
}

bool KokoroModelDownloader::ensureCacheDir() {
    try {
        if (!fs::exists(cache_dir_)) {
            fs::create_directories(cache_dir_);
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Kokoro] Failed to create cache directory: " << e.what() << std::endl;
        return false;
    }
}

// CURL write callback
static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    std::ofstream* file = static_cast<std::ofstream*>(userp);
    size_t total_size = size * nmemb;
    file->write(static_cast<const char*>(contents), total_size);
    return total_size;
}

// CURL progress callback
static int progressCallback(void* clientp, double dltotal, double dlnow,
                            double ultotal, double ulnow) {
    (void)clientp;
    (void)ultotal;
    (void)ulnow;
    if (dltotal > 0) {
        int progress = static_cast<int>((dlnow / dltotal) * 100);
        double dl_mb = dlnow / (1024.0 * 1024.0);
        double total_mb = dltotal / (1024.0 * 1024.0);
        std::cout << "\r[Kokoro] Download progress: " << progress << "% ("
            << std::fixed << std::setprecision(1) << dl_mb << "/"
            << total_mb << " MB)" << std::flush;
    }
    return 0;
}

bool KokoroModelDownloader::downloadFile(const std::string& url, const std::string& dest_path) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "[Kokoro] Failed to initialize CURL" << std::endl;
        return false;
    }

    std::ofstream file(dest_path, std::ios::binary);
    if (!file) {
        std::cerr << "[Kokoro] Failed to open file for writing: " << dest_path << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

#if LIBCURL_VERSION_NUM >= 0x072000
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCallback);
#else
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progressCallback);
#endif
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "kokoro-tts/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);

    int64_t http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);
    file.close();

    if (res != CURLE_OK) {
        std::cerr << "\n[Kokoro] Download failed: " << curl_easy_strerror(res) << std::endl;
        fs::remove(dest_path);
        return false;
    }

    if (http_code != 200) {
        std::cerr << "\n[Kokoro] HTTP error " << http_code << " for " << url << std::endl;
        fs::remove(dest_path);
        return false;
    }

    std::cout << std::endl;
    return true;
}

bool KokoroModelDownloader::extractTarArchive(const std::string& archive_path) {
    const pid_t child = fork();
    if (child < 0) {
        std::perror("[Kokoro] fork failed while extracting archive");
        return false;
    }
    if (child == 0) {
        execlp("tar", "tar", "-xzf", archive_path.c_str(), "-C",
                cache_dir_.c_str(), static_cast<char*>(nullptr));
        std::perror("[Kokoro] failed to execute tar");
        _exit(127);
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        std::perror("[Kokoro] waitpid failed while extracting archive");
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WIFEXITED(status)) {
            std::cerr << "[Kokoro] tar exited with status "
                        << WEXITSTATUS(status) << std::endl;
        } else if (WIFSIGNALED(status)) {
            std::cerr << "[Kokoro] tar terminated by signal "
                        << WTERMSIG(status) << std::endl;
        }
        return false;
    }
    return true;
}

bool KokoroModelDownloader::downloadLanguageModel(const std::string& language) {
    std::string archive = archiveName(language);
    if (archive.empty()) {
        std::cerr << "[Kokoro] Unknown language: " << language << std::endl;
        return false;
    }

    std::string url = std::string(BASE_URL) + "/" + archive;
    std::string archive_path = cache_dir_ + "/" + archive;
    std::string partial_path = archive_path + ".part";
    std::error_code ec;
    fs::remove(partial_path, ec);

    std::cout << "[Kokoro] Downloading " << language << " model from " << url << " ..." << std::endl;
    if (!downloadFile(url, partial_path)) {
        fs::remove(partial_path, ec);
        return false;
    }
    fs::rename(partial_path, archive_path, ec);
    if (ec) {
        std::cerr << "[Kokoro] Failed to finalize downloaded archive: "
                    << ec.message() << std::endl;
        fs::remove(partial_path, ec);
        return false;
    }

    std::cout << "[Kokoro] Extracting " << archive << " ..." << std::endl;
    if (!extractTarArchive(archive_path)) {
        std::cerr << "[Kokoro] Failed to extract " << archive << std::endl;
        fs::remove(archive_path, ec);
        return false;
    }

    fs::remove(archive_path, ec);
    if (!validateRequiredFiles(language)) {
        return false;
    }
    std::cout << "[Kokoro] " << language << " model downloaded and extracted successfully!" << std::endl;
    return true;
}

}  // namespace tts
