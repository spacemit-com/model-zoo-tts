/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef KOKORO_PHONEMIZER_HPP
#define KOKORO_PHONEMIZER_HPP

#include <cstdint>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declaration for cpp-pinyin
namespace Pinyin {
class Pinyin;
}  // namespace Pinyin

namespace tts {

// =============================================================================
// KokoroPhonemizer - Kokoro TTS phonemizer
// =============================================================================
//
// Converts Chinese/English/mixed text to Kokoro token IDs.
// Chinese: cpp-pinyin → Pinyin-to-IPA mapping (Misaki G2P compatible).
// English: espeak-ng → IPA cleanup → Gruut en-US normalization.
// Has a hardcoded 114-entry IPA vocabulary (Kokoro's fixed vocab).
//

struct PinyinParts {
    std::string initial;  // initial consonant
    std::string final_;   // final
    int tone = 5;         // tone 1-5 (5 = neutral)
};

class KokoroPhonemizer {
public:
    static constexpr int PAD_TOKEN_ID = 0;
    static constexpr int SPACE_TOKEN_ID = 16;
    static constexpr int MAX_TOKEN_LENGTH = 512;

    KokoroPhonemizer();
    ~KokoroPhonemizer();

    /// @brief Initialize cpp-pinyin converter (must be called before textToTokenIds)
    void initPinyin();

    /// @brief Initialize English fallback support without constructing a second
    /// Chinese pinyin converter.
    void initEnglish();

    /// @brief Load the official Misaki English lexicon (gold + silver JSON).
    /// @param model_dir Directory containing us_gold.json / us_silver.json
    void initEnglishLexicon(const std::string& model_dir);

    /// @brief Convert text to Kokoro token IDs (padded with 0 at start/end)
    /// @param text Input Chinese/English/mixed text
    /// @return Token IDs vector, padded [0, ...ids..., 0]
    std::vector<int64_t> textToTokenIds(const std::string& text) const;

    /// @brief English-only path: lexicon + number/acronym spell-out + espeak
    /// fallback, no Chinese FST. Used by the English backend.
    std::vector<int64_t> englishTextToTokenIds(const std::string& text) const;

    /// @brief Convert an English phrase to the official Misaki-compatible IPA
    /// sequence without applying a model-specific token vocabulary.
    std::string englishTextToPhonemes(const std::string& text) const;

    /// @brief Get vocabulary size
    int getVocabSize() const { return static_cast<int>(vocab_.size()); }

    /// @brief Check if espeak-ng is available (kept for optional English fallback)
    static bool isEspeakAvailable();

    /// @brief Add a custom English pronunciation. `pronunciation` is an English
    /// phrase (the same public contract as Matcha); it is converted to Kokoro IPA.
    bool addEnglishPronunciation(const std::string& word,
            const std::string& pronunciation);

private:
    /// @brief Convert a single pinyin syllable to IPA
    std::string pinyinToIPA(const std::string& pinyin) const;

    /// @brief Convert English text to IPA via espeak-ng + Gruut normalization
    std::string englishToIPA(const std::string& text) const;

    /// @brief Look up one English word in the misaki lexicon (gold -> silver ->
    /// stem). Returns misaki IPA (with stress marks) or "" if not found.
    std::string englishWordToIPA(
        const std::string& word, const std::string& tag = "") const;

    /// @brief misaki get_special_case: function words whose pronunciation is
    /// fixed or context-dependent (a, an, the, to, am), overriding the lexicon.
    /// next_vowel: whether the following token starts with a vowel phoneme
    /// (-1 unknown, 0 no, 1 yes). Returns "" if word has no special case.
    std::string englishSpecialCase(
        const std::string& word,
        const std::string& tag,
        int next_vowel,
        bool future_to) const;

    /// @brief Whether the first vowel/consonant phoneme in an IPA string is a
    /// vowel. Returns -1 if no vowel/consonant phoneme is present.
    int ipaStartsWithVowel(const std::string& ipa) const;

    /// @brief espeak-ng IPA for one OOV word (previous englishToIPA body).
    std::string espeakToIPA(const std::string& text) const;

    /// @brief Spell an all-caps acronym letter by letter (CPU -> C-P-U IPA).
    std::string spellAcronym(const std::string& word) const;

    /// @brief Convert a digit run to IPA via English number words + lexicon.
    std::string numberToIPA(const std::string& digits, bool as_year) const;

    /// @brief Lexicon helper: raw gold/silver lookup with dict-value DEFAULT.
    bool lexiconGet(
        const std::string& word,
        std::string& out,
        const std::string& tag = "",
        bool default_only = false) const;

    /// @brief Clean espeak-ng IPA output (remove syllable dots, zero-width chars, etc.)
    std::string cleanEspeakIPA(const std::string& ipa) const;

    /// @brief Parse pinyin string into initial + final + tone
    PinyinParts parsePinyin(const std::string& pinyin) const;

    /// @brief Convert tone number (1-5) to arrow marker
    std::string toneToArrow(int tone) const;

    /// @brief Convert IPA string to token IDs
    std::vector<int64_t> ipaToTokenIds(const std::string& ipa) const;

    // Initialize vocabulary
    void initVocab();

    // IPA token -> ID mapping (hardcoded Kokoro vocabulary)
    std::unordered_map<std::string, int64_t> vocab_;

    // cpp-pinyin converter
    std::unique_ptr<Pinyin::Pinyin> pinyin_converter_;

    // misaki-derived English lexicon (word -> misaki IPA). gold checked first.
    std::unordered_map<std::string, std::string> en_gold_;
    std::unordered_map<std::string, std::string> en_silver_;
    std::unordered_map<
        std::string, std::unordered_map<std::string, std::string>>
        en_gold_variants_;
    std::unordered_map<
        std::string, std::unordered_map<std::string, std::string>>
        en_silver_variants_;

    // Whether espeak-ng is available for English processing
    bool espeak_available_ = false;

    // Pinyin -> IPA mapping tables
    static const std::unordered_map<std::string, std::string> initial_to_ipa_;
    static const std::unordered_map<std::string, std::string> final_to_ipa_;

    // Ordered initials list for longest-match parsing
    static const std::vector<std::string> initials_ordered_;
};

}  // namespace tts

#endif  // KOKORO_PHONEMIZER_HPP
