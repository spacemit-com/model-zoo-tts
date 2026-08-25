/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#include "backends/kokoro/kokoro_phonemizer.hpp"

#include "backends/kokoro/kokoro_frontend_data.hpp"

#include <cpp-pinyin/G2pglobal.h>
#include <cpp-pinyin/Pinyin.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef TTS_HAS_ESPEAK_NG
#include <espeak-ng/speak_lib.h>
#endif

#include "backends/matcha/tts_model_downloader.hpp"
#include "text/phoneme_utils.hpp"
#include "text/text_normalizer.hpp"
#include "text/text_utils.hpp"

namespace fs = std::filesystem;

namespace {
#ifdef TTS_HAS_ESPEAK_NG
std::once_flag g_espeak_init_once;
std::mutex g_espeak_mutex;
bool g_espeak_initialized = false;

bool initializeEspeakLibrary() {
    std::call_once(g_espeak_init_once, []() {
        const int sample_rate = espeak_Initialize(
            AUDIO_OUTPUT_SYNCHRONOUS, 0, nullptr, espeakINITIALIZE_DONT_EXIT);
        g_espeak_initialized =
            sample_rate > 0 && espeak_SetVoiceByName("en-us") == EE_OK;
    });
    return g_espeak_initialized;
}
#endif

struct PcloseDeleter {
    void operator()(FILE* p) const { if (p) pclose(p); }
};

// --- English number-to-words (cardinal), used to spell out digit runs before
// lexicon lookup. Mirrors num2words for integers 0..999,999,999. ---
const char* const ONES[] = {"zero", "one", "two", "three", "four", "five", "six",
    "seven", "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen",
    "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
const char* const TENS[] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty",
    "seventy", "eighty", "ninety"};

void appendWord(std::string& out, const std::string& w) {
    if (!out.empty()) out += ' ';
    out += w;
}

// 0..999 -> words
void threeDigitsToWords(int n, std::string& out) {
    if (n >= 100) {
        appendWord(out, ONES[n / 100]);
        appendWord(out, "hundred");
        n %= 100;
        if (n == 0) return;
    }
    if (n >= 20) {
        appendWord(out, TENS[n / 10]);
        if (n % 10) appendWord(out, ONES[n % 10]);
    } else if (n > 0) {
        appendWord(out, ONES[n]);
    }
}

// Non-negative integer (as decimal string) -> space-joined English words.
std::string cardinalToWords(const std::string& digits) {
    // strip leading zeros but keep at least one
    size_t s = 0;
    while (s + 1 < digits.size() && digits[s] == '0') s++;
    std::string d = digits.substr(s);
    if (d == "0") return "zero";
    // group into thousands
    long long n = 0;
    bool ok = true;
    for (char c : d) { if (c < '0' || c > '9') { ok = false; break; } n = n * 10 + (c - '0'); }
    if (!ok) return "";
    static const char* const SCALES[] = {"", "thousand", "million", "billion"};
    // break into groups of 3 from the right
    std::vector<int> groups;
    if (n == 0) return "zero";
    while (n > 0) { groups.push_back(static_cast<int>(n % 1000)); n /= 1000; }
    std::string out;
    for (int gi = static_cast<int>(groups.size()) - 1; gi >= 0; --gi) {
        if (groups[gi] == 0) continue;
        threeDigitsToWords(groups[gi], out);
        if (gi > 0 && gi < 4) appendWord(out, SCALES[gi]);
    }
    return out;
}

// Convert the final component of a cardinal phrase to its ordinal form.
// This matches num2words' compound-ordinal structure: 101 -> "one hundred
// first", 112 -> "one hundred twelfth", and 120 -> "one hundred twentieth".
std::string ordinalToWords(const std::string& digits) {
    std::string words = cardinalToWords(digits);
    if (words.empty()) return "";

    static const std::unordered_map<std::string, std::string> kIrregular = {
        {"one", "first"}, {"two", "second"}, {"three", "third"},
        {"five", "fifth"}, {"eight", "eighth"}, {"nine", "ninth"},
        {"twelve", "twelfth"},
        {"twenty", "twentieth"}, {"thirty", "thirtieth"},
        {"forty", "fortieth"}, {"fifty", "fiftieth"},
        {"sixty", "sixtieth"}, {"seventy", "seventieth"},
        {"eighty", "eightieth"}, {"ninety", "ninetieth"},
    };

    const size_t separator = words.rfind(' ');
    const size_t final_begin = separator == std::string::npos
        ? 0 : separator + 1;
    const std::string final_word = words.substr(final_begin);
    const auto irregular = kIrregular.find(final_word);
    const std::string ordinal = irregular != kIrregular.end()
        ? irregular->second : final_word + "th";
    words.replace(final_begin, std::string::npos, ordinal);
    return words;
}

// 4-digit year reading, e.g. 2024 -> "twenty twenty-four", 1900 -> "nineteen
// hundred", 2000 -> "two thousand", 2005 -> "two thousand five".
std::string yearToWords(const std::string& digits) {
    if (digits.size() != 4) return cardinalToWords(digits);
    int hi = (digits[0]-'0')*10 + (digits[1]-'0');
    int lo = (digits[2]-'0')*10 + (digits[3]-'0');
    // 2000-2009 -> "two thousand [n]"; x000 -> "... thousand"
    if (hi % 10 == 0 && lo == 0) return cardinalToWords(digits);         // 2000
    if (lo == 0) {                                                        // 1900
        std::string out; threeDigitsToWords(hi, out); appendWord(out, "hundred");
        return out;
    }
    if (hi % 10 == 0 && lo < 10) {                                        // 2005
        return cardinalToWords(digits);
    }
    std::string out;
    threeDigitsToWords(hi, out);
    if (lo < 10) { appendWord(out, "o"); appendWord(out, ONES[lo]); }     // 2 0 5 -> "twenty o five" (rare)
    else threeDigitsToWords(lo, out);
    return out;
}

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parseJsonString(const std::string& json, size_t& pos, std::string& out) {
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (pos >= json.size()) return false;
        char escaped = json[pos++];
        switch (escaped) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (pos + 4 > json.size()) return false;
                uint32_t cp = 0;
                for (int i = 0; i < 4; ++i) {
                    int digit = hexDigit(json[pos++]);
                    if (digit < 0) return false;
                    cp = (cp << 4) | static_cast<uint32_t>(digit);
                }
                if (cp >= 0xd800 && cp <= 0xdbff &&
                    pos + 6 <= json.size() &&
                    json[pos] == '\\' && json[pos + 1] == 'u') {
                    pos += 2;
                    uint32_t low = 0;
                    for (int i = 0; i < 4; ++i) {
                        int digit = hexDigit(json[pos++]);
                        if (digit < 0) return false;
                        low = (low << 4) | static_cast<uint32_t>(digit);
                    }
                    if (low < 0xdc00 || low > 0xdfff) return false;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: return false;
        }
    }
    return false;
}

void skipJsonWhitespace(const std::string& json, size_t& pos) {
    while (pos < json.size() &&
            (json[pos] == ' ' || json[pos] == '\t' ||
            json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
}

std::string parentPosTag(const std::string& tag) {
    if (tag.rfind("VB", 0) == 0) return "VERB";
    if (tag.rfind("NN", 0) == 0) return "NOUN";
    if (tag.rfind("RB", 0) == 0 || tag.rfind("ADV", 0) == 0) return "ADV";
    if (tag.rfind("JJ", 0) == 0 || tag.rfind("ADJ", 0) == 0) return "ADJ";
    return tag;
}

void replaceAll(
    std::string& text,
    const std::string& from,
    const std::string& to) {
    size_t position = 0;
    while ((position = text.find(from, position)) != std::string::npos) {
        text.replace(position, from.size(), to);
        position += to.size();
    }
}

std::vector<std::string> splitUtf8Codepoints(const std::string& value) {
    std::vector<std::string> codepoints;
    for (size_t offset = 0; offset < value.size();) {
        const unsigned char byte = static_cast<unsigned char>(value[offset]);
        size_t length = 1;
        if (byte >= 0xf0) length = 4;
        else if (byte >= 0xe0) length = 3;
        else if (byte >= 0xc0) length = 2;
        if (offset + length > value.size()) length = 1;
        codepoints.push_back(value.substr(offset, length));
        offset += length;
    }
    return codepoints;
}

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string applyCapitalizationStress(
    const std::string& ipa, const std::string& word) {
    bool has_letter = false;
    bool has_upper = false;
    bool all_upper = true;
    for (const unsigned char c : word) {
        if (std::isalpha(c) == 0) continue;
        has_letter = true;
        has_upper = has_upper || std::isupper(c) != 0;
        all_upper = all_upper && std::isupper(c) != 0;
    }
    if (!has_letter || !has_upper) return ipa;

    static const std::string kPrimary = "\xcb\x88";
    static const std::string kSecondary = "\xcb\x8c";
    std::string stressed = ipa;
    const bool has_primary = stressed.find(kPrimary) != std::string::npos;
    const bool has_secondary = stressed.find(kSecondary) != std::string::npos;
    if (all_upper && !has_primary && has_secondary) {
        replaceAll(stressed, kSecondary, kPrimary);
        return stressed;
    }
    if (has_primary || has_secondary) return stressed;

    static const std::vector<std::string> kVowels = {
        "A", "I", "O", "Q", "W", "Y", "a", "i", "u",
        "\xc3\xa6", "\xc9\x91", "\xc9\x92", "\xc9\x94",
        "\xc9\x99", "\xc9\x9b", "\xc9\x9c", "\xc9\xaa",
        "\xca\x8a", "\xca\x8c", "\xe1\xb5\xbb"};
    size_t first_vowel = std::string::npos;
    for (const auto& vowel : kVowels) {
        first_vowel = std::min(first_vowel, stressed.find(vowel));
    }
    if (first_vowel != std::string::npos) {
        stressed.insert(first_vowel, all_upper ? kPrimary : kSecondary);
    }
    return stressed;
}

bool parseMisakiValue(
    const std::string& json,
    size_t& pos,
    std::string& ipa,
    std::unordered_map<std::string, std::string>* variants = nullptr) {
    skipJsonWhitespace(json, pos);
    if (pos >= json.size()) return false;
    if (json[pos] == '"') return parseJsonString(json, pos, ipa);
    if (json.compare(pos, 4, "null") == 0) {
        pos += 4;
        ipa.clear();
        return true;
    }
    if (json[pos] != '{') return false;

    ++pos;
    std::string first_value;
    std::string default_value;
    while (true) {
        skipJsonWhitespace(json, pos);
        if (pos >= json.size()) return false;
        if (json[pos] == '}') {
            ++pos;
            ipa = !default_value.empty() ? default_value : first_value;
            return true;
        }
        std::string tag;
        if (!parseJsonString(json, pos, tag)) return false;
        skipJsonWhitespace(json, pos);
        if (pos >= json.size() || json[pos++] != ':') return false;
        std::string value;
        if (!parseMisakiValue(json, pos, value)) return false;
        if (variants != nullptr) {
            (*variants)[tag] = value;
        }
        if (tag == "DEFAULT") default_value = value;
        if (first_value.empty() && !value.empty()) first_value = value;
        skipJsonWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < json.size() && json[pos] == '}') continue;
        return false;
    }
}

bool loadMisakiJson(
    const std::string& path,
    std::unordered_map<std::string, std::string>& out,
    std::unordered_map<
        std::string, std::unordered_map<std::string, std::string>>&
        variants_out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    const std::string json = buffer.str();
    size_t pos = 0;
    skipJsonWhitespace(json, pos);
    if (pos >= json.size() || json[pos++] != '{') return false;
    while (true) {
        skipJsonWhitespace(json, pos);
        if (pos >= json.size()) return false;
        if (json[pos] == '}') return true;
        std::string word;
        if (!parseJsonString(json, pos, word)) return false;
        skipJsonWhitespace(json, pos);
        if (pos >= json.size() || json[pos++] != ':') return false;
        std::string ipa;
        std::unordered_map<std::string, std::string> variants;
        if (!parseMisakiValue(json, pos, ipa, &variants)) return false;
        if (!word.empty() && !ipa.empty()) {
            out.emplace(word, std::move(ipa));
        }
        if (!word.empty() && !variants.empty()) {
            variants_out.emplace(std::move(word), std::move(variants));
        }
        skipJsonWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < json.size() && json[pos] == '}') continue;
        return false;
    }
}
}  // namespace

namespace tts {

// =============================================================================
// Kokoro IPA Vocabulary (114 entries, from official Kokoro-82M config.json)
// =============================================================================

void KokoroPhonemizer::initVocab() {
    for (const auto& [token, id] : kokoro_frontend_data::kokoroVocabulary()) {
        vocab_.emplace(token, id);
    }
}

// =============================================================================
// Constructor / Destructor
// =============================================================================

KokoroPhonemizer::KokoroPhonemizer() {
    initVocab();
}

KokoroPhonemizer::~KokoroPhonemizer() = default;

// =============================================================================
// Public Methods
// =============================================================================

void KokoroPhonemizer::initPinyin() {
    // Reuse TTSModelDownloader to ensure cpp-pinyin is available
    TTSModelDownloader downloader;
    if (!downloader.ensureCppPinyin()) {
        throw std::runtime_error("Failed to download cpp-pinyin dictionary.");
    }

    std::string pinyin_dict_dir = downloader.getCppPinyinPath();
    std::cout << "[KokoroPhonemizer] Using cpp-pinyin dictionary at: " << pinyin_dict_dir << std::endl;

    Pinyin::setDictionaryPath(fs::path(pinyin_dict_dir));
    pinyin_converter_ = std::make_unique<Pinyin::Pinyin>();

    std::cout << "[KokoroPhonemizer] cpp-pinyin initialized successfully." << std::endl;

    initEnglish();
}

void KokoroPhonemizer::initEnglish() {
    // Check espeak-ng availability for English OOV fallback.
    espeak_available_ = isEspeakAvailable();
    if (espeak_available_) {
        std::cout << "[KokoroPhonemizer] espeak-ng detected, English support enabled." << std::endl;
    } else {
        std::cout << "[KokoroPhonemizer] espeak-ng not found, English text will be skipped." << std::endl;
    }
}

void KokoroPhonemizer::initEnglishLexicon(const std::string& model_dir) {
    // Load the official Misaki US JSON dictionaries. Gold is the
    // high-confidence set, silver the fallback; espeak covers OOV words.
    // Legacy converted TSV files remain supported for existing deployments.
    auto loadTsv = [](const std::string& path,
                        std::unordered_map<std::string, std::string>& out) {
        std::ifstream in(path);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            std::string word = line.substr(0, t1);
            // DEFAULT ipa is the second column, up to the next tab (if any).
            size_t t2 = line.find('\t', t1 + 1);
            std::string ipa = (t2 == std::string::npos)
                                    ? line.substr(t1 + 1)
                                    : line.substr(t1 + 1, t2 - t1 - 1);
            if (!ipa.empty()) out.emplace(std::move(word), std::move(ipa));
        }
        return true;
    };

    bool g = loadMisakiJson(
        model_dir + "/us_gold.json", en_gold_, en_gold_variants_);
    bool s = loadMisakiJson(
        model_dir + "/us_silver.json", en_silver_, en_silver_variants_);
    if (!g) g = loadTsv(model_dir + "/lexicon-gold.txt", en_gold_);
    if (!s) s = loadTsv(model_dir + "/lexicon-silver.txt", en_silver_);
    std::cout << "[KokoroPhonemizer] English lexicon: gold=" << en_gold_.size()
                << " silver=" << en_silver_.size() << std::endl;
    if (!g && !s) {
        std::cerr << "[KokoroPhonemizer] Warning: no English lexicon found in "
                    << model_dir << ", falling back to espeak only." << std::endl;
    }
}

bool KokoroPhonemizer::addEnglishPronunciation(
    const std::string& word, const std::string& pronunciation) {
    if (word.empty() || pronunciation.empty()) return false;

    // The public SDK uses the same contract as Matcha: callers provide an
    // English respelling such as "space meet", not model-specific IPA.
    const std::string ipa = englishToIPA(pronunciation);
    if (ipa.empty()) return false;

    en_gold_[word] = ipa;
    std::string lower = word;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    en_gold_[lower] = ipa;
    return true;
}

bool KokoroPhonemizer::lexiconGet(
    const std::string& word,
    std::string& out,
    const std::string& tag,
    bool default_only) const {
    const auto lookup = [&](const auto& base, const auto& variants) {
        const auto variant = variants.find(word);
        if (!default_only && variant != variants.end()) {
            auto pronunciation = variant->second.find(tag);
            if (pronunciation == variant->second.end()) {
                pronunciation = variant->second.find(parentPosTag(tag));
            }
            if (pronunciation != variant->second.end()) {
                out = pronunciation->second;
                return !out.empty();
            }
        }
        const auto value = base.find(word);
        if (value == base.end() || value->second.empty()) return false;
        out = value->second;
        return true;
    };
    return lookup(en_gold_, en_gold_variants_) ||
        lookup(en_silver_, en_silver_variants_);
}

std::string KokoroPhonemizer::englishWordToIPA(
    const std::string& word, const std::string& tag) const {
    if (word.empty()) return "";

    const auto lowerWord = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        return value;
    };
    const auto lookup = [&](const std::string& value, std::string& ipa) {
        if (lexiconGet(value, ipa, tag)) return true;
        const std::string lower = lowerWord(value);
        return lower != value && lexiconGet(lower, ipa, tag);
    };
    const auto known = [&](const std::string& value) {
        std::string ignored;
        return lookup(value, ignored);
    };
    const auto suffixS = [](const std::string& stem) {
        const auto codepoints = splitUtf8Codepoints(stem);
        if (codepoints.empty()) return std::string();
        const std::string& final = codepoints.back();
        if (final == "p" || final == "t" || final == "k" ||
            final == "f" || final == "θ") {
            return stem + "s";
        }
        if (final == "s" || final == "z" || final == "ʃ" ||
            final == "ʒ" || final == "ʧ" || final == "ʤ") {
            return stem + "ᵻz";
        }
        return stem + "z";
    };
    const auto suffixEd = [](const std::string& stem) {
        const auto codepoints = splitUtf8Codepoints(stem);
        if (codepoints.empty()) return std::string();
        const std::string& final = codepoints.back();
        if (final == "p" || final == "k" || final == "f" ||
            final == "θ" || final == "ʃ" || final == "s" ||
            final == "ʧ") {
            return stem + "t";
        }
        if (final == "d") return stem + "ᵻd";
        if (final != "t") return stem + "d";
        if (codepoints.size() > 1) {
            static const std::unordered_set<std::string> kUsTaus = {
                "A", "I", "O", "W", "Y", "i", "u", "æ", "ɑ", "ə",
                "ɛ", "ɪ", "ɹ", "ʊ", "ʌ"};
            if (kUsTaus.count(codepoints[codepoints.size() - 2]) != 0) {
                std::string base;
                for (size_t i = 0; i + 1 < codepoints.size(); ++i) {
                    base += codepoints[i];
                }
                return base + "ɾᵻd";
            }
        }
        return stem + "ᵻd";
    };
    const auto suffixIng = [](const std::string& stem) {
        const auto codepoints = splitUtf8Codepoints(stem);
        if (codepoints.empty()) return std::string();
        if (codepoints.size() > 1 && codepoints.back() == "t") {
            static const std::unordered_set<std::string> kUsTaus = {
                "A", "I", "O", "W", "Y", "i", "u", "æ", "ɑ", "ə",
                "ɛ", "ɪ", "ɹ", "ʊ", "ʌ"};
            if (kUsTaus.count(codepoints[codepoints.size() - 2]) != 0) {
                std::string base;
                for (size_t i = 0; i + 1 < codepoints.size(); ++i) {
                    base += codepoints[i];
                }
                return base + "ɾɪŋ";
            }
        }
        return stem + "ɪŋ";
    };

    std::string ipa;
    if (word.size() > 1 && word.back() == '.') {
        const std::string abbreviation = lowerWord(
            word.substr(0, word.size() - 1));
        const auto& abbreviations =
            kokoro_frontend_data::englishAbbreviationLexiconKeys();
        const auto key = abbreviations.find(abbreviation);
        if (key != abbreviations.end() && lookup(key->second, ipa)) {
            return applyCapitalizationStress(ipa, word);
        }
    }
    if (lookup(word, ipa)) {
        return applyCapitalizationStress(ipa, word);
    }

    const std::string lower = lowerWord(word);
    if (endsWith(lower, "s'") && known(lower.substr(0, lower.size() - 2) + "'s") &&
        lookup(lower.substr(0, lower.size() - 2) + "'s", ipa)) {
        return applyCapitalizationStress(ipa, word);
    }
    if (endsWith(lower, "'") && known(lower.substr(0, lower.size() - 1)) &&
        lookup(lower.substr(0, lower.size() - 1), ipa)) {
        return applyCapitalizationStress(ipa, word);
    }

    std::string stem;
    if (lower.size() >= 3 && endsWith(lower, "s")) {
        if (!endsWith(lower, "ss") && known(lower.substr(0, lower.size() - 1))) {
            stem = lower.substr(0, lower.size() - 1);
        } else if ((endsWith(lower, "'s") ||
                    (lower.size() > 4 && endsWith(lower, "es") &&
                    !endsWith(lower, "ies"))) &&
                    known(lower.substr(0, lower.size() - 2))) {
            stem = lower.substr(0, lower.size() - 2);
        } else if (lower.size() > 4 && endsWith(lower, "ies") &&
                    known(lower.substr(0, lower.size() - 3) + "y")) {
            stem = lower.substr(0, lower.size() - 3) + "y";
        }
        if (!stem.empty() && lookup(stem, ipa)) {
            return applyCapitalizationStress(suffixS(ipa), word);
        }
    }

    stem.clear();
    if (lower.size() >= 4 && endsWith(lower, "d")) {
        if (!endsWith(lower, "dd") && known(lower.substr(0, lower.size() - 1))) {
            stem = lower.substr(0, lower.size() - 1);
        } else if (lower.size() > 4 && endsWith(lower, "ed") &&
                    !endsWith(lower, "eed") &&
                    known(lower.substr(0, lower.size() - 2))) {
            stem = lower.substr(0, lower.size() - 2);
        }
        if (!stem.empty() && lookup(stem, ipa)) {
            return applyCapitalizationStress(suffixEd(ipa), word);
        }
    }

    stem.clear();
    if (lower.size() >= 5 && endsWith(lower, "ing")) {
        const std::string raw = lower.substr(0, lower.size() - 3);
        if (lower.size() > 5 && known(raw)) {
            stem = raw;
        } else if (known(raw + "e")) {
            stem = raw + "e";
        } else if (lower.size() > 5) {
            const std::string doubled = lower.substr(0, lower.size() - 4);
            const char repeated = lower[lower.size() - 4];
            static const std::string kDoublingConsonants = "bcdgklmnprstvxz";
            if (((lower.size() >= 5 && lower[lower.size() - 5] == repeated &&
                    kDoublingConsonants.find(repeated) != std::string::npos) ||
                    endsWith(lower, "cking")) && known(doubled)) {
                stem = doubled;
            }
        }
        if (!stem.empty() && lookup(stem, ipa)) {
            return applyCapitalizationStress(suffixIng(ipa), word);
        }
    }

    return "";
}

bool KokoroPhonemizer::isEspeakAvailable() {
#ifdef TTS_HAS_ESPEAK_NG
    return initializeEspeakLibrary();
#else
    std::string command = "espeak-ng --version 2>/dev/null";
    std::unique_ptr<FILE, PcloseDeleter> pipe(popen(command.c_str(), "r"));
    if (!pipe) return false;

    char buffer[128];
    std::string result;
    if (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }

    int exit_status = pclose(pipe.release());
    return exit_status == 0 && !result.empty();
#endif
}

std::vector<int64_t> KokoroPhonemizer::englishTextToTokenIds(const std::string& text) const {
    if (text.empty()) return {};
    // Pure-English path: lexicon + number spell-out + acronym spell-out +
    // espeak fallback. No Chinese FST (which would turn digits into 汉字).
    std::string ipa = englishToIPA(text);
    if (ipa.empty()) {
        std::cerr << "[KokoroPhonemizer] No IPA output for English text: " << text << std::endl;
        return {};
    }
    if (const char* dump_path = std::getenv("KOKORO_DUMP_PHONEMES")) {
        std::ofstream dump(dump_path, std::ios::app);
        if (dump) dump << ipa << '\n';
    }
    std::vector<int64_t> ids = ipaToTokenIds(ipa);
    std::vector<int64_t> padded;
    padded.reserve(ids.size() + 2);
    padded.push_back(PAD_TOKEN_ID);
    padded.insert(padded.end(), ids.begin(), ids.end());
    padded.push_back(PAD_TOKEN_ID);
    return padded;
}

std::string KokoroPhonemizer::englishTextToPhonemes(
        const std::string& text) const {
    return englishToIPA(text);
}

std::vector<int64_t> KokoroPhonemizer::textToTokenIds(const std::string& text) const {
    if (text.empty()) return {};

    if (!pinyin_converter_) {
        std::cerr << "[KokoroPhonemizer] cpp-pinyin not initialized, call initPinyin() first" << std::endl;
        return {};
    }

    // Step 1: Text normalization
    std::string normalized = text::normalizeText(text, text::Language::ZH);

    // Step 2: Split into UTF-8 characters and segment by language
    auto chars = text::splitUtf8(normalized);
    std::string combined_ipa;

    size_t i = 0;
    while (i < chars.size()) {
        const std::string& ch = chars[i];

        // --- Chinese segment ---
        if (text::isChineseChar(ch)) {
            std::string chinese_segment;
            while (i < chars.size() && text::isChineseChar(chars[i])) {
                chinese_segment += chars[i];
                i++;
            }

            // Convert Chinese segment via cpp-pinyin
            Pinyin::PinyinResVector pinyin_result = pinyin_converter_->hanziToPinyin(
                chinese_segment,
                Pinyin::ManTone::Style::TONE3,
                Pinyin::Error::Default,
                false,  // candidates
                false,  // v_to_u
                true);  // neutral_tone_with_five

            for (const auto& res : pinyin_result) {
                if (!res.error) {
                    std::string ipa = pinyinToIPA(res.pinyin);
                    if (!ipa.empty()) {
                        combined_ipa += ipa;
                    }
                }
            }
            continue;
        }

        // --- English segment ---
        if (text::isEnglishLetter(ch)) {
            std::string english_segment;
            while (i < chars.size() &&
                (text::isEnglishLetter(chars[i]) || chars[i] == " " ||
                chars[i] == "'" || chars[i] == "-")) {
                english_segment += chars[i];
                i++;
            }

            // Trim trailing spaces
            while (!english_segment.empty() && english_segment.back() == ' ') {
                english_segment.pop_back();
            }

            if (!english_segment.empty()) {
                std::string ipa = englishToIPA(english_segment);
                if (!ipa.empty()) {
                    combined_ipa += ipa;
                }
            }
            continue;
        }

        // --- Punctuation / other characters ---
        // Note: digits were already converted to Chinese characters by the FST
        // in step 1, so no digit branch is needed here.
        std::string mapped = text::mapChinesePunctToAscii(ch);
        if (mapped.empty()) mapped = ch;

        if (vocab_.count(mapped)) {
            combined_ipa += mapped;
        }
        i++;
    }

    if (combined_ipa.empty()) {
        std::cerr << "[KokoroPhonemizer] No IPA output for text: " << text << std::endl;
        return {};
    }

    // Step 3: Convert IPA to token IDs
    std::vector<int64_t> ids = ipaToTokenIds(combined_ipa);

    // Step 4: Add start/end padding
    std::vector<int64_t> padded;
    padded.reserve(ids.size() + 2);
    padded.push_back(PAD_TOKEN_ID);
    padded.insert(padded.end(), ids.begin(), ids.end());
    padded.push_back(PAD_TOKEN_ID);

    return padded;
}

// =============================================================================
// English Processing Methods
// =============================================================================

// Spell an all-caps acronym letter by letter (CPU -> C-P-U), using the gold
// single-letter pronunciations. Returns "" if any letter is missing.
std::string KokoroPhonemizer::spellAcronym(const std::string& word) const {
    std::string ipa;
    for (char c : word) {
        if (!std::isalpha(static_cast<unsigned char>(c))) continue;
        std::string letter(1, static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        std::string p;
        if (!lexiconGet(letter, p) || p.empty()) return "";
        ipa += p;
    }
    static const std::string kPrimary = "\xcb\x88";
    static const std::string kSecondary = "\xcb\x8c";
    replaceAll(ipa, kPrimary, kSecondary);
    const size_t last_stress = ipa.rfind(kSecondary);
    if (last_stress != std::string::npos) {
        ipa.replace(last_stress, kSecondary.size(), kPrimary);
    }
    return ipa;
}

// Convert a digit run to IPA by spelling it out to English words, then looking
// each word up. `as_year` uses year reading for 4-digit numbers.
std::string KokoroPhonemizer::numberToIPA(const std::string& digits, bool as_year) const {
    std::string words = (as_year && digits.size() == 4) ? yearToWords(digits)
                                                        : cardinalToWords(digits);
    if (words.empty()) return "";
    std::string ipa;
    std::string cur;
    auto flush = [&](const std::string& w) {
        if (w.empty()) return;
        std::string p = englishWordToIPA(w);
        if (p.empty() && espeak_available_) p = espeakToIPA(w);
        if (!ipa.empty()) ipa += " ";
        ipa += p;
    };
    for (char c : words) {
        if (c == ' ') { flush(cur); cur.clear(); }
        else cur += c;
    }
    flush(cur);
    return ipa;
}

std::string KokoroPhonemizer::ordinalToIPA(const std::string& digits) const {
    const std::string words = ordinalToWords(digits);
    if (words.empty()) return "";

    std::string ipa;
    std::istringstream stream(words);
    for (std::string word; stream >> word;) {
        std::string pronunciation = englishWordToIPA(word);
        if (pronunciation.empty() && espeak_available_) {
            pronunciation = espeakToIPA(word);
        }
        if (!ipa.empty()) ipa += " ";
        ipa += pronunciation;
    }
    return ipa;
}

std::string KokoroPhonemizer::dollarAmountToIPA(
        const std::string& amount) const {
    const size_t dot = amount.find('.');
    const std::string dollars = dot == std::string::npos
        ? amount : amount.substr(0, dot);
    std::string cents = dot == std::string::npos ? "" : amount.substr(dot + 1);
    if (cents.size() == 1) cents += '0';
    if (cents.size() > 2) cents.resize(2);

    // Keep malformed or unreasonably large currency input on the generic
    // number path instead of allowing std::stoll to escape from the frontend.
    if (dollars.size() > 18 || cents.size() > 2) {
        return numberToIPA(amount, false);
    }
    long long dollar_value = 0;
    long long cent_value = 0;
    try {
        dollar_value = dollars.empty() ? 0 : std::stoll(dollars);
        cent_value = cents.empty() ? 0 : std::stoll(cents);
    } catch (const std::exception&) {
        return numberToIPA(amount, false);
    }
    std::string ipa;
    if (dollar_value != 0 || cent_value == 0) {
        ipa = numberToIPA(std::to_string(dollar_value), false);
        ipa += " " + englishWordToIPA(
            dollar_value == 1 ? "dollar" : "dollars");
    }
    if (cent_value != 0) {
        if (!ipa.empty()) ipa += " " + englishWordToIPA("and") + " ";
        ipa += numberToIPA(std::to_string(cent_value), false);
        ipa += " " + englishWordToIPA(cent_value == 1 ? "cent" : "cents");
    }
    return ipa;
}

int KokoroPhonemizer::ipaStartsWithVowel(const std::string& ipa) const {
    const auto& vowels = kokoro_frontend_data::englishVowels();
    const auto& consonants = kokoro_frontend_data::englishConsonants();
    size_t i = 0;
    while (i < ipa.size()) {
        unsigned char c = static_cast<unsigned char>(ipa[i]);
        int char_len = 1;
        if (c >= 0xF0) char_len = 4;
        else if (c >= 0xE0) char_len = 3;
        else if (c >= 0xC0) char_len = 2;
        if (i + char_len > ipa.size()) break;
        std::string ch = ipa.substr(i, char_len);
        if (vowels.count(ch) != 0) return 1;
        if (consonants.count(ch) != 0) return 0;
        i += char_len;
    }
    return -1;
}

std::string KokoroPhonemizer::englishSpecialCase(
    const std::string& word,
    const std::string& tag,
    int next_vowel,
    bool future_to) const {
    if ((word == "a" || word == "A") && tag == "DT") {
        return "\xc9\x90";  // ɐ
    }
    if (word == "an" || word == "An" ||
        (word == "AN" && tag.rfind("NN", 0) != 0)) {
        return "\xc9\x90n";  // ɐn
    }
    if (word == "I" && tag == "PRP") {
        return "\xcb\x8cI";  // ˌI
    }
    if ((word == "by" || word == "By" || word == "BY") &&
        parentPosTag(tag) == "ADV") {
        // Misaki removes the lexical stress for adverbial "by" in sentence
        // context, yielding bI in the final G2P output.
        return "bI";
    }
    if (word == "am" || word == "Am" || word == "AM") {
        if (tag.rfind("NN", 0) == 0) return spellAcronym(word);
        if (word == "am" && next_vowel != -1) return "\xc9\x90m";  // ɐm
        std::string full;
        if (lexiconGet("am", full)) {
            return applyCapitalizationStress(full, word);
        }
        return "";
    }
    if (word == "to" || word == "To" ||
        (word == "TO" && (tag == "TO" || tag == "IN"))) {
        if (next_vowel == 0) return "t\xc9\x99";   // tə
        if (next_vowel == 1) return "t\xca\x8a";   // tʊ
        std::string g;
        if (lexiconGet("to", g)) return g;
        return "";
    }
    if (word == "in" || word == "In" ||
        (word == "IN" && tag != "NNP")) {
        return (next_vowel == -1 || tag != "IN")
            ? "\xcb\x88\xc9\xaan"  // ˈɪn
            : "\xc9\xaan";          // ɪn
    }
    if (word == "the" || word == "The" ||
        (word == "THE" && tag == "DT")) {
        return next_vowel == 1 ? "\xc3\xb0i" : "\xc3\xb0\xc9\x99";  // ði : ðə
    }
    std::string lower = word;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (tag == "IN" && (lower == "vs" || lower == "vs.")) {
        std::string versus;
        if (lexiconGet("versus", versus)) return versus;
    }
    if (word == "used" || word == "Used" || word == "USED") {
        std::string pronunciation;
        if ((tag == "VBD" || tag == "JJ") && future_to &&
            lexiconGet("used", pronunciation, "VBD")) {
            return applyCapitalizationStress(pronunciation, word);
        }
        if (lexiconGet("used", pronunciation, "", true)) {
            return applyCapitalizationStress(pronunciation, word);
        }
    }
    return "";
}

// Lexicon-first English G2P: split into words/numbers/acronyms, look each up in
// the misaki lexicon (high quality, matches training), spell out digits and
// all-caps acronyms, and fall back to espeak for OOV words. Punctuation and
// spacing are preserved. Function words (a/an/the/to/am) use misaki special
// cases resolved right-to-left against the following token's leading phoneme.
std::string KokoroPhonemizer::englishToIPA(const std::string& text) const {
    if (text.empty()) return "";
    // Without a Misaki lexicon, phonemize the complete utterance in one
    // espeak-ng call. Besides avoiding one expensive parser invocation per
    // word, this preserves sentence-level stress and liaison better than
    // concatenating independently phonemized words.
    if (en_gold_.empty() && en_silver_.empty() && espeak_available_) {
        return espeakToIPA(text);
    }

    struct Segment {
        bool is_word;
        std::string word;  // original text for special-case lookup
        std::string tag;   // Penn Treebank-style POS used by Misaki entries
        std::string ipa;   // default IPA (literal text for non-word segments)
    };
    std::vector<Segment> segments;

    size_t i = 0;
    const size_t n = text.size();
    while (i < n) {
        unsigned char ch = static_cast<unsigned char>(text[i]);

        // --- US dollar amount ---
        if (ch == '$' && i + 1 < n &&
            std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
            const size_t start = ++i;
            bool has_dot = false;
            while (i < n) {
                const unsigned char c = static_cast<unsigned char>(text[i]);
                if (std::isdigit(c) || c == ',') {
                    i++;
                    continue;
                }
                if (c == '.' && !has_dot && i + 1 < n &&
                    std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
                    has_dot = true;
                    i++;
                    continue;
                }
                break;
            }
            std::string amount;
            for (const char c : text.substr(start, i - start)) {
                if (c != ',') amount += c;
            }
            segments.push_back(
                {true, "$" + amount, "CD", dollarAmountToIPA(amount)});
            continue;
        }

        // --- Digit run (optionally with , . inside) ---
        if (std::isdigit(ch)) {
            size_t start = i;
            bool has_dot = false;
            while (i < n) {
                unsigned char c = static_cast<unsigned char>(text[i]);
                if (std::isdigit(c)) { i++; continue; }
                if (c == ',' && i + 1 < n && std::isdigit(static_cast<unsigned char>(text[i+1]))) { i++; continue; }
                if (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(text[i+1]))) { has_dot = true; i++; continue; }
                break;
            }
            std::string num = text.substr(start, i - start);
            std::string ordinal_suffix;
            if (i + 1 < n) {
                const std::string suffix = text.substr(i, 2);
                if (suffix == "st" || suffix == "nd" || suffix == "rd" ||
                    suffix == "th") {
                    ordinal_suffix = suffix;
                    i += 2;
                }
            }
            // strip thousands separators for the integer part
            std::string clean;
            for (char c : num) if (c != ',') clean += c;
            std::string ipa;
            if (!ordinal_suffix.empty()) {
                ipa = ordinalToIPA(clean);
            } else if (has_dot) {
                // integer.fraction -> "<int> point <digit> <digit> ..."
                size_t dot = clean.find('.');
                ipa = numberToIPA(clean.substr(0, dot), false);
                std::string pt = englishWordToIPA("point");
                replaceAll(pt, "\xcb\x88", "");
                if (!ipa.empty()) ipa += " ";
                ipa += pt;
                for (size_t k = dot + 1; k < clean.size(); ++k) {
                    std::string d(1, clean[k]);
                    ipa += " " + numberToIPA(d, false);
                }
            } else {
                const bool as_year = clean.size() == 4 &&
                    num.find(',') == std::string::npos;
                ipa = numberToIPA(clean, as_year);
            }
            segments.push_back({true, num, "CD", ipa});
            continue;
        }

        // --- Word (letters + internal apostrophes/hyphens) ---
        if (std::isalpha(ch)) {
            size_t start = i;
            while (i < n) {
                char c = text[i];
                if (std::isalpha(static_cast<unsigned char>(c))) { i++; continue; }
                if (c == '.' && i + 1 < n &&
                    std::isalpha(static_cast<unsigned char>(text[i + 1]))) {
                    i++;
                    continue;
                }
                if (c == '\'' && i > start &&
                    (i + 1 == n ||
                        !std::isalpha(
                            static_cast<unsigned char>(text[i + 1])))) {
                    i++;
                    continue;
                }
                if ((c == '\'' || c == '-') && i + 1 < n &&
                    std::isalpha(static_cast<unsigned char>(text[i + 1]))) {
                    i++;
                    continue;
                }
                break;
            }
            const std::string without_final_dot = text.substr(start, i - start);
            std::string lower_without_dot = without_final_dot;
            std::transform(
                lower_without_dot.begin(), lower_without_dot.end(),
                lower_without_dot.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            if (i < n && text[i] == '.' &&
                (lower_without_dot == "vs" ||
                    kokoro_frontend_data::englishAbbreviationLexiconKeys()
                        .count(lower_without_dot) != 0 ||
                    without_final_dot.find('.') != std::string::npos)) {
                i++;
            }
            std::string word = text.substr(start, i - start);

            segments.push_back({true, word, "", ""});
            continue;
        }

        const std::string symbol(1, static_cast<char>(ch));
        const auto& symbols = kokoro_frontend_data::englishSymbolWords();
        const auto symbol_word = symbols.find(symbol);
        if (symbol_word != symbols.end() &&
            (symbol == "%" || symbol == "&" || symbol == "+" ||
                symbol == "@")) {
            segments.push_back({true, symbol_word->second, "SYM", ""});
            i++;
            continue;
        }

        // Pass through spaces and punctuation verbatim.
        segments.push_back(
            {false, "", "", std::string(1, static_cast<char>(ch))});
        i++;
    }

    const auto lowerWord = [](std::string word) {
        std::transform(
            word.begin(), word.end(), word.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        return word;
    };
    const auto isAllUpper = [](const std::string& word) {
        bool has_letter = false;
        for (const unsigned char c : word) {
            if (std::isalpha(c) == 0) continue;
            has_letter = true;
            if (std::isupper(c) == 0) return false;
        }
        return has_letter;
    };
    const auto isPronoun = [](const std::string& word) {
        return kokoro_frontend_data::englishPronouns().count(word) != 0;
    };

    std::vector<size_t> words;
    for (size_t segment = 0; segment < segments.size(); ++segment) {
        if (segments[segment].is_word) words.push_back(segment);
    }
    for (size_t word_index = 0; word_index < words.size(); ++word_index) {
        Segment& segment = segments[words[word_index]];
        if (segment.tag == "CD" || segment.tag == "SYM") continue;
        const std::string lower = lowerWord(segment.word);
        const std::string previous = word_index == 0
            ? "" : lowerWord(segments[words[word_index - 1]].word);
        const std::string next = word_index + 1 == words.size()
            ? "" : lowerWord(segments[words[word_index + 1]].word);

        if (lower == "a" || lower == "an" || lower == "the") {
            segment.tag = segment.word == "A" && previous != ""
                ? "NNP" : "DT";
        } else if (segment.word == "I" || isPronoun(lower)) {
            segment.tag = "PRP";
        } else if (lower == "am") {
            segment.tag = isAllUpper(segment.word) ? "NNP" : "VBP";
        } else if (lower == "is") {
            segment.tag = "VBZ";
        } else if (lower == "to") {
            segment.tag = "TO";
        } else if (lower == "in" || lower == "with") {
            segment.tag = isAllUpper(segment.word) ? "RB" : "IN";
        } else if (lower == "by") {
            segment.tag = "RB";
        } else if (lower == "vs" || lower == "vs.") {
            segment.tag = "IN";
        } else if (lower == "use" && word_index == 0) {
            segment.tag = "VB";
        } else if (lower == "used") {
            segment.tag = isPronoun(previous) ? "VBD" : "VBN";
        } else if (lower == "read" && isPronoun(previous)) {
            segment.tag = "VBP";
        } else if (endsWith(lower, "ing")) {
            segment.tag = "VBG";
        } else if (endsWith(lower, "ed")) {
            segment.tag = "VBD";
        } else if (lower.size() > 2 && endsWith(lower, "s") &&
                    !endsWith(lower, "ss")) {
            segment.tag = "NNS";
        } else if (lower == "record" || lower == "project" ||
                lower == "live" ||
                lower == "present" || lower == "object" ||
                lower == "permit" || lower == "increase") {
            const bool verb_context =
                isPronoun(previous) ||
                (word_index == 0 &&
                    (next == "a" || next == "an" || next == "the"));
            segment.tag = verb_context ? "VB" : "NN";
        } else if (previous == "to") {
            segment.tag = "VB";
        } else if (previous == "a" || previous == "an" ||
                previous == "the") {
            segment.tag = "NN";
        } else if (isAllUpper(segment.word) && segment.word.size() > 1) {
            segment.tag = "NNP";
        } else if (!segment.word.empty() &&
                std::isupper(
                    static_cast<unsigned char>(segment.word.front())) != 0) {
            segment.tag = "NNP";
        } else {
            segment.tag = "NN";
        }
    }

    // Right-to-left pass: resolve POS-sensitive entries and function words
    // against Misaki's future_vowel/future_to context.
    int next_vowel = -1;
    bool future_to = false;
    for (size_t s = segments.size(); s-- > 0;) {
        Segment& seg = segments[s];
        if (seg.is_word) {
            if (seg.tag != "CD") {
                seg.ipa = englishSpecialCase(
                    seg.word, seg.tag, next_vowel, future_to);
                const std::string lookup_tag =
                    next_vowel == -1 ? "None" : seg.tag;
                if (seg.ipa.empty()) {
                    seg.ipa = englishWordToIPA(seg.word, lookup_tag);
                }
                if (seg.ipa.empty() && isAllUpper(seg.word) &&
                    seg.word.size() >= 2) {
                    seg.ipa = spellAcronym(seg.word);
                }
                if (seg.ipa.empty() && espeak_available_) {
                    seg.ipa = espeakToIPA(seg.word);
                }
                if (seg.tag == "SYM") {
                    seg.ipa = " " + seg.ipa + " ";
                }
            }
            int v = ipaStartsWithVowel(seg.ipa);
            if (v != -1) next_vowel = v;
            const std::string lower = lowerWord(seg.word);
            future_to =
                lower == "to" && (seg.tag == "TO" || seg.tag == "IN");
        } else {
            for (char c : seg.ipa) {
                if (c == ';' || c == ':' || c == ',' || c == '.' ||
                    c == '!' || c == '?') {
                    next_vowel = -1;
                    future_to = false;
                    break;
                }
            }
        }
    }

    std::string out;
    for (const auto& segment : segments) {
        out += segment.ipa;
    }
    std::string compact;
    compact.reserve(out.size());
    for (const char character : out) {
        if (character == ' ' && !compact.empty() && compact.back() == ' ') {
            continue;
        }
        compact += character;
    }
    out = std::move(compact);
    // Misaki English v1.0 normalizes the US flap and glottal stop to Kokoro's
    // training symbols after token resolution.
    replaceAll(out, "ɾ", "T");
    replaceAll(out, "ʔ", "t");
    return out;
}

std::string KokoroPhonemizer::espeakToIPA(const std::string& text) const {
    if (!espeak_available_) {
        std::cerr << "[KokoroPhonemizer] espeak-ng not available, skipping English: " << text << std::endl;
        return "";
    }

    if (text.empty()) return "";

#ifdef TTS_HAS_ESPEAK_NG
    std::string result;
    {
        // espeak-ng exposes global voice state and a shared phoneme buffer.
        std::lock_guard<std::mutex> lock(g_espeak_mutex);
        if (!initializeEspeakLibrary()) return "";

        const void* cursor = text.c_str();
        for (int segment = 0; cursor != nullptr && segment < 64; ++segment) {
            const char* phonemes = espeak_TextToPhonemes(
                &cursor, espeakCHARS_UTF8, 0x03);
            if (phonemes == nullptr) break;
            result += phonemes;
        }
    }
    if (result.empty()) return "";
#else
    // Escape single quotes for shell safety
    std::string escaped = text;
    std::string::size_type pos = 0;
    while ((pos = escaped.find("'", pos)) != std::string::npos) {
        escaped.replace(pos, 1, "'\"'\"'");
        pos += 5;
    }

    // Call espeak-ng for IPA conversion
    std::string command = "echo '" + escaped + "' | espeak-ng -q --ipa=3 -v en-us";

    std::unique_ptr<FILE, PcloseDeleter> pipe(popen(command.c_str(), "r"));
    if (!pipe) {
        std::cerr << "[KokoroPhonemizer] Failed to run espeak-ng" << std::endl;
        return "";
    }

    char buffer[4096];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }

    int exit_status = pclose(pipe.release());
    if (exit_status != 0 || result.empty()) {
        return "";
    }
#endif

    // Clean espeak output and convert to Kokoro-compatible IPA
    result = cleanEspeakIPA(result);
    result = text::convertToGruutEnUs(result);

    return result;
}

std::string KokoroPhonemizer::cleanEspeakIPA(const std::string& ipa) const {
    std::string result;
    result.reserve(ipa.size());

    size_t i = 0;
    bool last_was_space = false;

    while (i < ipa.size()) {
        unsigned char c = static_cast<unsigned char>(ipa[i]);

        // Skip newlines and carriage returns
        if (c == '\n' || c == '\r') {
            i++;
            continue;
        }

        // Determine UTF-8 character byte length
        int char_len = 1;
        if (c >= 0xF0) char_len = 4;
        else if (c >= 0xE0) char_len = 3;
        else if (c >= 0xC0) char_len = 2;

        if (i + char_len > ipa.size()) break;

        std::string ch = ipa.substr(i, char_len);

        // Skip syllable boundary dot
        if (ch == ".") {
            i += char_len;
            continue;
        }

        // Skip zero-width characters (U+200B, U+200C, U+200D, U+FEFF)
        if (ch == "\xe2\x80\x8b" || ch == "\xe2\x80\x8c" ||
            ch == "\xe2\x80\x8d" || ch == "\xef\xbb\xbf") {
            i += char_len;
            continue;
        }

        // Handle spaces: collapse consecutive spaces
        if (ch == " ") {
            if (!last_was_space && !result.empty()) {
                result += ' ';
                last_was_space = true;
            }
            i += char_len;
            continue;
        }

        result += ch;
        last_was_space = false;
        i += char_len;
    }

    // Trim trailing space
    if (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }

    // Trim leading space
    if (!result.empty() && result.front() == ' ') {
        result.erase(result.begin());
    }

    return result;
}

// =============================================================================
// Private Methods
// =============================================================================

PinyinParts KokoroPhonemizer::parsePinyin(const std::string& pinyin) const {
    PinyinParts parts;

    if (pinyin.empty()) return parts;

    std::string py = pinyin;

    // Extract tone number from the end
    char last = py.back();
    if (last >= '1' && last <= '5') {
        parts.tone = last - '0';
        py.pop_back();
    }

    if (py.empty()) return parts;

    // Match longest initial first
    for (const auto& ini : kokoro_frontend_data::orderedPinyinInitials()) {
        if (py.size() >= ini.size() && py.substr(0, ini.size()) == ini) {
            parts.initial = ini;
            parts.final_ = py.substr(ini.size());
            return parts;
        }
    }

    // No initial matched -- zero-initial syllable (e.g. "a", "o", "e", "an")
    parts.final_ = py;
    return parts;
}

std::string KokoroPhonemizer::toneToArrow(int tone) const {
    switch (tone) {
        case 1: return "\xe2\x86\x92";  // U+2192
        case 2: return "\xe2\x86\x97";  // U+2197
        case 3: return "\xe2\x86\x93";  // U+2193
        case 4: return "\xe2\x86\x98";  // U+2198
        case 5: return "";               // neutral tone
        default: return "";
    }
}

std::string KokoroPhonemizer::pinyinToIPA(const std::string& pinyin) const {
    PinyinParts parts = parsePinyin(pinyin);

    std::string ipa;

    // Special cases for retroflex/dental sibilant + "i"
    if (parts.final_ == "i") {
        if (parts.initial == "zh" || parts.initial == "ch" ||
            parts.initial == "sh" || parts.initial == "r") {
            auto ini_it = kokoro_frontend_data::pinyinInitialToIpa().find(parts.initial);
            if (ini_it != kokoro_frontend_data::pinyinInitialToIpa().end()) {
                ipa = ini_it->second;
            }
            ipa += "\xc9\xbb";  // syllabic retroflex vowel
            ipa += toneToArrow(parts.tone);
            return ipa;
        }
        if (parts.initial == "z" || parts.initial == "c" || parts.initial == "s") {
            auto ini_it = kokoro_frontend_data::pinyinInitialToIpa().find(parts.initial);
            if (ini_it != kokoro_frontend_data::pinyinInitialToIpa().end()) {
                ipa = ini_it->second;
            }
            ipa += "\xc9\xb9";  // syllabic dental vowel
            ipa += toneToArrow(parts.tone);
            return ipa;
        }
    }

    // Handle "j/q/x + u" -> actually "j/q/x + u-umlaut"
    if ((parts.initial == "j" || parts.initial == "q" || parts.initial == "x") &&
        !parts.final_.empty() && parts.final_[0] == 'u') {
        std::string adjusted_final = "v" + parts.final_.substr(1);
        auto fin_it = kokoro_frontend_data::pinyinFinalToIpa().find(adjusted_final);
        if (fin_it != kokoro_frontend_data::pinyinFinalToIpa().end()) {
            auto ini_it = kokoro_frontend_data::pinyinInitialToIpa().find(parts.initial);
            if (ini_it != kokoro_frontend_data::pinyinInitialToIpa().end()) {
                ipa = ini_it->second;
            }
            ipa += fin_it->second;
            ipa += toneToArrow(parts.tone);
            return ipa;
        }
    }

    // General case: look up initial + final
    if (!parts.initial.empty()) {
        auto ini_it = kokoro_frontend_data::pinyinInitialToIpa().find(parts.initial);
        if (ini_it != kokoro_frontend_data::pinyinInitialToIpa().end()) {
            ipa = ini_it->second;
        }
    }

    if (!parts.final_.empty()) {
        auto fin_it = kokoro_frontend_data::pinyinFinalToIpa().find(parts.final_);
        if (fin_it != kokoro_frontend_data::pinyinFinalToIpa().end()) {
            ipa += fin_it->second;
        } else {
            // Fallback: try individual characters
            for (char c : parts.final_) {
                std::string sc(1, c);
                auto it = kokoro_frontend_data::pinyinFinalToIpa().find(sc);
                if (it != kokoro_frontend_data::pinyinFinalToIpa().end()) {
                    ipa += it->second;
                } else {
                    ipa += sc;  // Pass through as-is
                }
            }
        }
    }

    // Append tone arrow
    ipa += toneToArrow(parts.tone);

    return ipa;
}

std::vector<int64_t> KokoroPhonemizer::ipaToTokenIds(const std::string& ipa) const {
    // Per-Unicode-character lookup, matching official Kokoro tokenizer
    std::vector<int64_t> ids;
    ids.reserve(ipa.size());

    size_t i = 0;
    while (i < ipa.size()) {
        // Determine UTF-8 character byte length
        unsigned char c = static_cast<unsigned char>(ipa[i]);
        int char_len = 1;
        if (c >= 0xF0) char_len = 4;
        else if (c >= 0xE0) char_len = 3;
        else if (c >= 0xC0) char_len = 2;

        if (i + char_len <= ipa.size()) {
            std::string token = ipa.substr(i, char_len);
            auto it = vocab_.find(token);
            if (it != vocab_.end()) {
                ids.push_back(it->second);
            }
            // Not found -- skip silently (matches official behavior)
        }
        i += char_len;
    }

    return ids;
}

}  // namespace tts
