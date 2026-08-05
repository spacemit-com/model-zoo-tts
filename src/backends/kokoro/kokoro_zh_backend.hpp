/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef KOKORO_ZH_BACKEND_HPP
#define KOKORO_ZH_BACKEND_HPP

#include <cstdint>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "backends/kokoro/kokoro_backend.hpp"
#include "backends/kokoro/kokoro_phonemizer.hpp"
#include "backends/kokoro/tone_sandhi.hpp"

// Forward declarations
namespace cppjieba { class Jieba; }
namespace Pinyin { class Pinyin; }

namespace tts {

// =============================================================================
// Kokoro Chinese Backend (v1.1-zh with misaki-aligned ToneSandhi frontend)
// =============================================================================
//
// Reimplements the misaki v1.1 ZHFrontend pipeline in pure C++:
//   1. jieba POS tagging (Tag)
//   2. ToneSandhi.preMergeForModify (merge 不/一/reduplication/三声/儿)
//   3. For each word:
//      - cpp-pinyin -> initials + finals (TONE3)
//      - ToneSandhi.modifiedTone (bu/yi/neural/three sandhi)
//      - merge_erhua (儿化)
//      - ZH_MAP (pinyin -> bopomofo)
//      - insert "/" between words
//   4. Token lookup (bopomofo -> id)
//
// Model: kokoro-v1.1-zh (24000Hz)
// Assets: tokens.txt, lexicon-zh.txt (68004 entries, used as pinyin cache),
//         dict/ (jieba)
//

class KokoroZhBackend : public KokoroBackend {
public:
    KokoroZhBackend();
    ~KokoroZhBackend() override;

protected:
    std::vector<int64_t> textToTokenIds(const std::string& text) override;
    std::vector<std::string> getChunkingUnits(
        const std::string& text) override;
    std::string prepareTextForChunking(
        const std::string& text) const override;
    std::string getModelSubdir() const override;
    std::string getModelFile() const override;
    std::string getLanguage() const override;
    std::string getVoiceName() const override;
    std::string getConvFallbackFilter() const override;
    ErrorInfo initializeLanguageSpecific(const TtsConfig& config) override;
    void shutdownLanguageSpecific() override;
    ErrorInfo updateLanguageLexicon(
        const std::vector<LexiconEntry>& entries) override;

private:
    void initializeJieba(const std::string& dict_dir);
    void initializePinyin();
    void initializeToneSandhi();
    bool loadTokenizerJson(const std::string& path);

    // Split a word into UTF-8 characters.
    std::vector<std::string> utf8Chars(const std::string& s);

    // Get initials and finals (TONE3) for a word via cpp-pinyin.
    // Returns (initials, finals) both size == #chars.
    std::pair<std::vector<std::string>, std::vector<std::string>>
        getInitialsFinals(const std::string& word);

    // misaki _merge_erhua: merge trailing "儿" into previous final as "XRY".
    std::pair<std::vector<std::string>, std::vector<std::string>>
        mergeErhua(std::vector<std::string> initials,
                    std::vector<std::string> finals,
                    const std::string& word,
                    const std::string& pos);

    // Pinyin -> Bopomofo mapping (misaki ZH_MAP).
    std::string zhMap(const std::string& pinyin);

    std::unique_ptr<cppjieba::Jieba> jieba_;
    std::unique_ptr<Pinyin::Pinyin> pinyin_;
    ToneSandhi tone_sandhi_;
    KokoroPhonemizer english_frontend_;

    std::unordered_map<std::string, int64_t> token_to_id_;  // tokens.txt
    std::unordered_map<std::string, std::string> lexicon_;  // lexicon-zh.txt (optional cache)
    std::unordered_map<std::string, std::string> custom_zh_lexicon_;
    int64_t separator_id_ = -1;                             // id of "/"

};

}  // namespace tts

#endif  // KOKORO_ZH_BACKEND_HPP
