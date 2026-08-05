/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#include <fstream>
#include <iostream>
#include <string>

#include "backends/kokoro/kokoro_phonemizer.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0]
            << " <kokoro-en-model-dir> <golden.tsv>" << std::endl;
        return 2;
    }

    std::ifstream golden(argv[2]);
    if (!golden) {
        std::cerr << "Cannot open golden file: " << argv[2] << std::endl;
        return 2;
    }

    tts::KokoroPhonemizer phonemizer;
    phonemizer.initEnglish();
    phonemizer.initEnglishLexicon(argv[1]);

    int cases = 0;
    int failures = 0;
    std::string line;
    while (std::getline(golden, line)) {
        if (line.empty() || line.front() == '#') continue;
        const size_t first_tab = line.find('\t');
        const size_t second_tab = line.find('\t', first_tab + 1);
        if (first_tab == std::string::npos || second_tab == std::string::npos) {
            std::cerr << "Invalid golden row: " << line << std::endl;
            return 2;
        }
        const std::string text = line.substr(0, first_tab);
        const std::string expected =
            line.substr(first_tab + 1, second_tab - first_tab - 1);
        const std::string actual = phonemizer.englishTextToPhonemes(text);
        if (actual != expected) {
            std::cerr << "Misaki golden mismatch for: " << text << '\n'
                << "expected: " << expected << '\n'
                << "actual:   " << actual << std::endl;
            ++failures;
        }
        ++cases;
    }
    if (failures != 0) {
        std::cerr << "Kokoro English Misaki golden mismatches: " << failures
            << "/" << cases << std::endl;
        return 1;
    }
    std::cout << "Kokoro English Misaki golden cases passed: " << cases
        << std::endl;
    return 0;
}
