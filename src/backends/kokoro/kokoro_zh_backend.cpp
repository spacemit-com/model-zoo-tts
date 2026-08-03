/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#include "backends/kokoro/kokoro_zh_backend.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cppjieba/Jieba.hpp"
#include "cpp-pinyin/Pinyin.h"
#include "cpp-pinyin/G2pglobal.h"
#include "backends/matcha/tts_model_downloader.hpp"
#include "text/text_normalizer.hpp"
#include "text/text_utils.hpp"
#include "text/token_utils.hpp"

namespace fs = std::filesystem;

namespace tts {

namespace {
// misaki ZH_MAP: pinyin syllable/initial/final/tone -> bopomofo
const std::unordered_map<std::string, std::string> ZH_MAP_DATA = {
    {"b", "ㄅ"}, {"p", "ㄆ"}, {"m", "ㄇ"}, {"f", "ㄈ"}, {"d", "ㄉ"}, {"t", "ㄊ"}, {"n", "ㄋ"}, {"l", "ㄌ"},
    {"g", "ㄍ"}, {"k", "ㄎ"}, {"h", "ㄏ"}, {"j", "ㄐ"}, {"q", "ㄑ"}, {"x", "ㄒ"}, {"zh", "ㄓ"}, {"ch", "ㄔ"},
    {"sh", "ㄕ"}, {"r", "ㄖ"}, {"z", "ㄗ"}, {"c", "ㄘ"}, {"s", "ㄙ"},
    {"a", "ㄚ"}, {"o", "ㄛ"}, {"e", "ㄜ"}, {"ie", "ㄝ"}, {"ai", "ㄞ"}, {"ei", "ㄟ"}, {"ao", "ㄠ"}, {"ou", "ㄡ"},
    {"an", "ㄢ"}, {"en", "ㄣ"}, {"ang", "ㄤ"}, {"eng", "ㄥ"}, {"er", "ㄦ"},
    {"i", "ㄧ"}, {"u", "ㄨ"}, {"v", "ㄩ"}, {"ii", "ㄭ"}, {"iii", "十"}, {"ve", "月"},
    {"ia", "压"}, {"ian", "言"}, {"iang", "阳"}, {"iao", "要"}, {"in", "阴"}, {"ing", "应"}, {"iong", "用"},
    {"iou", "又"}, {"ong", "中"}, {"ua", "穵"}, {"uai", "外"}, {"uan", "万"}, {"uang", "王"}, {"uei", "为"},
    {"uen", "文"}, {"ueng", "瓮"}, {"uo", "我"}, {"van", "元"}, {"vn", "云"},
    // Punctuation passthrough
    {";", ";"}, {":", ";"}, {",", ","}, {".", "。"}, {"!", "!"}, {"?", "?"}, {"—", "—"}, {"…", "…"},
    {"\"", "\""}, {"(", "("}, {")", ")"}, {" ", " "},
    // Tone digits passthrough
    {"0", "0"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}, {"5", "5"},
    {"R", "R"}  // erhua marker
};

// Split UTF-8 string into character vector
std::vector<std::string> utf8CharsHelper(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        if (i + len > s.size()) len = 1;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

std::string jsonUnescape(const std::string& input) {
    std::string out;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '\\' || i + 1 >= input.size()) {
            out += input[i];
            continue;
        }
        char e = input[++i];
        if (e == '"' || e == '\\' || e == '/') {
            out += e;
        } else if (e == 'u' && i + 4 < input.size()) {
            unsigned cp = 0;
            bool valid = true;
            for (size_t j = 1; j <= 4; ++j) {
                char h = input[i + j];
                cp <<= 4;
                if (h >= '0' && h <= '9') cp += h - '0';
                else if (h >= 'a' && h <= 'f') cp += h - 'a' + 10;
                else if (h >= 'A' && h <= 'F') cp += h - 'A' + 10;
                else valid = false;
            }
            if (valid) {
                if (cp <= 0x7f) {
                    out += static_cast<char>(cp);
                } else if (cp <= 0x7ff) {
                    out += static_cast<char>(0xc0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3f));
                } else {
                    out += static_cast<char>(0xe0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (cp & 0x3f));
                }
                i += 4;
            }
        } else if (e == 'n') {
            out += '\n';
        } else if (e == 't') {
            out += '\t';
        }
    }
    return out;
}

size_t charLenHelper(const std::string& s) { return utf8CharsHelper(s).size(); }

// --- Arabic digit -> Chinese numeral (cn2an an2cn, pragmatic subset) ---
const char* const CN_DIGITS[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
const char* const CN_UNITS[] = {"", "十", "百", "千"};
const char* const CN_BIG[] = {"", "万", "亿"};

// Read an integer string with value semantics (12345 -> 一万二千三百四十五).
std::string intToChinese(const std::string& s) {
    // strip leading zeros
    size_t st = 0;
    while (st + 1 < s.size() && s[st] == '0') st++;
    std::string d = s.substr(st);
    if (d == "0") return CN_DIGITS[0];
    int len = static_cast<int>(d.size());
    if (len > 12) {
        // too large for grouped reading -> read digit by digit
        std::string out;
        for (char c : d) out += CN_DIGITS[c - '0'];
        return out;
    }
    // process in groups of 4 (万/亿)
    std::string out;
    int ngroups = (len + 3) / 4;
    bool prev_zero = false;
    for (int g = ngroups - 1; g >= 0; --g) {
        int start = len - (g + 1) * 4;
        int cnt = 4;
        if (start < 0) { cnt += start; start = 0; }
        std::string grp = d.substr(start, cnt);
        int group_value = std::stoi(grp);
        // read this 4-digit group
        std::string gout;
        int glen = static_cast<int>(grp.size());
        bool gz = false;
        bool any = false;
        for (int i = 0; i < glen; ++i) {
            int digit = grp[i] - '0';
            int unit = glen - 1 - i;  // 0..3
            if (digit == 0) {
                gz = true;
            } else {
                if (gz && any) gout += CN_DIGITS[0];
                gout += CN_DIGITS[digit];
                gout += CN_UNITS[unit];
                gz = false;
                any = true;
            }
        }
        if (any) {
            if (!out.empty() && (prev_zero || group_value < 1000)) {
                out += CN_DIGITS[0];
            }
            out += gout + CN_BIG[g];
            prev_zero = false;
        } else {
            prev_zero = true;
        }
    }
    // "一十" at the very start -> "十" (十几 reads without 一)
    std::string yishi = std::string(CN_DIGITS[1]) + CN_UNITS[1];  // 一十
    if (out.size() >= yishi.size() && out.compare(0, yishi.size(), yishi) == 0) {
        out = out.substr(std::string(CN_DIGITS[1]).size());
    }
    return out;
}

// Read digits one by one (2024 -> 二 零 二 四, for years / after 第 etc.).
// Separate each numeral with a space so jieba keeps them as single-char words;
// cpp-pinyin mispronounces/drops syllables on odd multi-char numeral runs like
// "二零二四". The spaces become whitespace tokens (natural, negligible).
std::string digitsToChinese(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c >= '0' && c <= '9') {
            out += CN_DIGITS[c - '0'];
        }
    }
    return out;
}

// Convert all Arabic digit runs in a UTF-8 string to Chinese numerals.
// Heuristics (subset of cn2an): a run immediately followed by 年 or preceded by
// 第 is read digit-by-digit; a run with a decimal point reads "<int>点<digits>";
// otherwise value semantics.
std::string normalizeArabicNumbers(const std::string& text) {
    auto chars = utf8CharsHelper(text);
    std::string out;
    size_t i = 0;
    while (i < chars.size()) {
        const std::string& ch = chars[i];
        bool is_digit = (ch.size() == 1 && ch[0] >= '0' && ch[0] <= '9');
        if (!is_digit) { out += ch; i++; continue; }
        // collect digit run (allow one '.')
        std::string num;
        bool has_dot = false;
        size_t j = i;
        while (j < chars.size()) {
            const std::string& d = chars[j];
            if (d.size() == 1 && d[0] >= '0' && d[0] <= '9') { num += d; j++; continue; }
            if (d == "." && !has_dot && j + 1 < chars.size() &&
                chars[j+1].size() == 1 && chars[j+1][0] >= '0' && chars[j+1][0] <= '9') {
                has_dot = true; num += "."; j++; continue;
            }
            break;
        }
        bool next_nian = (j < chars.size() && chars[j] == "年");
        if (has_dot) {
            size_t dot = num.find('.');
            out += intToChinese(num.substr(0, dot)) + "点" +
                    digitsToChinese(num.substr(dot + 1));
        } else if (next_nian) {
            out += digitsToChinese(num);
        } else {
            out += intToChinese(num);
        }
        i = j;
    }
    return out;
}

std::string normalizeSemanticForms(std::string text) {
    // Thousands separators.
    text = std::regex_replace(text, std::regex("([0-9]),(?=[0-9]{3}(?:\\D|$))"), "$1");
    // Fractions and percentages must be rewritten before the generic number FST.
    text = std::regex_replace(
        text, std::regex("([0-9]+)[/]([0-9]+)"), "$2分之$1");
    text = std::regex_replace(
        text, std::regex("([0-9]+(?:\\.[0-9]+)?)%"), "百分之$1");
    // Clock time.
    text = std::regex_replace(
        text, std::regex("([0-9]{1,2}):([0-9]{2})"), "$1点$2分");
    // Currency.
    text = std::regex_replace(
        text, std::regex("\\$([0-9]+(?:\\.[0-9]+)?)"), "$1美元");
    text = std::regex_replace(
        text, std::regex("(?:¥|￥)([0-9]+(?:\\.[0-9]+)?)"), "$1元");
    // Temperature and common metric units.
    text = std::regex_replace(
        text, std::regex("([0-9]+(?:\\.[0-9]+)?)(?:℃|°C)"), "$1摄氏度");
    text = std::regex_replace(
        text, std::regex("([0-9]+(?:\\.[0-9]+)?)(?:℉|°F)"), "$1华氏度");
    static const std::vector<std::pair<std::string, std::string>> kUnits = {
        {"kg", "千克"}, {"km", "千米"}, {"cm", "厘米"}, {"mm", "毫米"},
        {"ml", "毫升"}, {"ms", "毫秒"}, {"GB", "吉字节"}, {"MB", "兆字节"}
    };
    for (const auto& unit : kUnits) {
        text = std::regex_replace(
            text, std::regex("([0-9]+(?:\\.[0-9]+)?)" + unit.first + "\\b"),
            "$1" + unit.second);
    }
    // A leading minus is a sign; hyphens in identifiers such as GPT-4 stay.
    text = std::regex_replace(
        text, std::regex("(^|[^A-Za-z0-9])[-]([0-9])"), "$1负$2");
    return text;
}

// Expand contracted finals after a real initial to their pypinyin FINALS form:
//   iu -> iou (liu->liou), ui -> uei (gui->guei), un -> uen (kun->kuen->uen form)
// pypinyin emits the full phonetic final; cpp-pinyin emits the written form.
std::string expandContractedFinal(const std::string& fin) {
    if (fin == "iu") return "iou";
    if (fin == "ui") return "uei";
    if (fin == "un") return "uen";
    return fin;
}

// Reconstruct the pypinyin FINALS form of a zero-initial syllable (no tone).
// pypinyin treats y/w as spelling markers, not initials; the phonetic final is:
//   yi->i  ya->ia  ye->ie  yao->iao  you->iou  yan->ian  yin->in
//   yang->iang  ying->ing  yong->iong  yu->v  yue->ve  yuan->van  yun->vn
//   wu->u  wa->ua  wo->uo  wai->uai  wei->uei  wan->uan  wen->uen
//   wang->uang  weng->ueng
// Non y/w zero-initial syllables (a, o, e, ai, ei, ao, ou, an, en, ang, eng,
// er) pass through unchanged.
std::string normalizeZeroInitialFinal(const std::string& syl) {
    static const std::unordered_map<std::string, std::string> kMap = {
        {"yi", "i"}, {"ya", "ia"}, {"ye", "ie"}, {"yao", "iao"}, {"you", "iou"},
        {"yan", "ian"}, {"yin", "in"}, {"yang", "iang"}, {"ying", "ing"},
        {"yong", "iong"}, {"yu", "v"}, {"yue", "ve"}, {"yuan", "van"}, {"yun", "vn"},
        {"wu", "u"}, {"wa", "ua"}, {"wo", "uo"}, {"wai", "uai"}, {"wei", "uei"},
        {"wan", "uan"}, {"wen", "uen"}, {"wang", "uang"}, {"weng", "ueng"},
        // bare "y"/"w" degenerate cases
        {"y", "i"}, {"w", "u"}
    };
    auto it = kMap.find(syl);
    if (it != kMap.end()) return it->second;
    return syl;
}

}  // namespace

KokoroZhBackend::KokoroZhBackend() : KokoroBackend(BackendType::KOKORO_ZH) {}
KokoroZhBackend::~KokoroZhBackend() { shutdownLanguageSpecific(); }

std::string KokoroZhBackend::getModelSubdir() const { return "kokoro-v1.1-zh"; }
std::string KokoroZhBackend::getModelFile() const { return "kokoro-v1.1-zh.q.onnx"; }
std::string KokoroZhBackend::getLanguage() const { return "zh"; }
std::string KokoroZhBackend::getVoiceName() const { return "zf_001"; }

std::string KokoroZhBackend::getConvFallbackFilter() const {
    return "/decoder/generator/conv_post/Conv;"
            "/decoder/generator/Conv;"
            "/decoder/generator/Conv_1;"
            "/decoder/generator/ConvTranspose;"
            "/decoder/generator/ConvTranspose_1";
}

ErrorInfo KokoroZhBackend::initializeLanguageSpecific(const TtsConfig& config) {
    (void)config;
    try {
        const std::string subdir_path = getActiveModelDir();

        if (fs::exists(subdir_path + "/tokens.txt")) {
            token_to_id_ = text::readTokenToIdMap(subdir_path + "/tokens.txt");
        } else if (!loadTokenizerJson(subdir_path + "/tokenizer.json")) {
            throw std::runtime_error(
                "Neither a valid tokens.txt nor tokenizer.json was found in " +
                subdir_path);
        }
        auto sep_it = token_to_id_.find("/");
        separator_id_ = (sep_it != token_to_id_.end()) ? sep_it->second : -1;

        if (fs::exists(subdir_path + "/lexicon-zh.txt")) {
            lexicon_ = text::readLexicon(subdir_path + "/lexicon-zh.txt");
            std::cout << "[KokoroZh] Loaded " << lexicon_.size()
                        << " lexicon entries" << std::endl;
        }

        TTSModelDownloader downloader;
        if (!downloader.ensureCppJieba()) {
            throw std::runtime_error("Failed to obtain cppjieba dictionary.");
        }
        initializeJieba(downloader.getCppJiebaPath());
        initializePinyin();
        english_frontend_.initPinyin();
        const std::string kokoro_root =
            fs::path(subdir_path).parent_path().string();
        const fs::path local_lexicon_dir = subdir_path;
        const fs::path installed_english_dir =
            fs::path(kokoro_root) / "kokoro-v1.0-en";
        const auto has_english_lexicons = [](const fs::path& directory) {
            return fs::is_regular_file(directory / "us_gold.json") &&
                fs::is_regular_file(directory / "us_silver.json");
        };
        if (has_english_lexicons(local_lexicon_dir)) {
            english_frontend_.initEnglishLexicon(
                local_lexicon_dir.string());
        } else if (has_english_lexicons(installed_english_dir)) {
            english_frontend_.initEnglishLexicon(
                installed_english_dir.string());
        } else {
            std::cerr << "[KokoroZh] English lexicons unavailable; "
                            "mixed English will use espeak-ng fallback."
                        << std::endl;
        }
        initializeToneSandhi();

        zh_map_ = ZH_MAP_DATA;

        return ErrorInfo::ok();
    } catch (const std::exception& e) {
        return ErrorInfo::error(ErrorCode::INTERNAL_ERROR,
            std::string("Failed to initialize Kokoro Chinese frontend: ") + e.what());
    }
}

bool KokoroZhBackend::loadTokenizerJson(const std::string& path) {
    std::ifstream file(path);
    if (!file) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();

    std::unordered_map<std::string, int64_t> parsed;
    size_t cursor = 0;
    while (cursor < json.size()) {
        size_t begin = json.find('"', cursor);
        if (begin == std::string::npos) break;
        std::string raw;
        bool escaped = false;
        size_t end = begin + 1;
        for (; end < json.size(); ++end) {
            char c = json[end];
            if (escaped) {
                raw += '\\';
                raw += c;
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                break;
            } else {
                raw += c;
            }
        }
        if (end >= json.size()) return false;
        size_t colon = json.find(':', end + 1);
        if (colon == std::string::npos) return false;
        size_t value = json.find_first_of("0123456789", colon + 1);
        if (value == std::string::npos) return false;
        size_t value_end = value;
        while (value_end < json.size() &&
                json[value_end] >= '0' && json[value_end] <= '9') {
            ++value_end;
        }
        parsed[jsonUnescape(raw)] =
            std::stoll(json.substr(value, value_end - value));
        cursor = value_end;
    }
    if (parsed.count("$") == 0 || parsed.count("ㄅ") == 0) return false;
    token_to_id_ = std::move(parsed);
    std::cout << "[KokoroZh] Loaded " << token_to_id_.size()
                << " tokenizer entries from " << path << std::endl;
    return true;
}

void KokoroZhBackend::shutdownLanguageSpecific() {
    jieba_.reset();
    pinyin_.reset();
    lexicon_.clear();
    token_to_id_.clear();
    zh_map_.clear();
}

ErrorInfo KokoroZhBackend::updateLanguageLexicon(
    const std::vector<LexiconEntry>& entries) {
    for (const auto& entry : entries) {
        if (entry.word.empty() || entry.phoneme.empty()) {
            return ErrorInfo::error(
                ErrorCode::INVALID_CONFIG, "Kokoro lexicon entry is empty");
        }
        if (entry.locale == "en") {
            if (!english_frontend_.addEnglishPronunciation(
                    entry.word, entry.phoneme)) {
                return ErrorInfo::error(
                    ErrorCode::INVALID_CONFIG,
                    "Invalid Kokoro English lexicon entry: " + entry.word);
            }
            continue;
        }

        std::istringstream stream(entry.phoneme);
        std::vector<std::string> syllables;
        for (std::string syllable; stream >> syllable;) {
            if (syllable.empty() || syllable.back() < '0' ||
                syllable.back() > '5') {
                return ErrorInfo::error(
                    ErrorCode::INVALID_CONFIG,
                    "Kokoro Chinese pronunciation must use tone-number pinyin: " +
                    entry.word);
            }
            syllables.push_back(syllable);
        }
        const auto characters = utf8Chars(entry.word);
        if (syllables.size() != characters.size()) {
            return ErrorInfo::error(
                ErrorCode::INVALID_CONFIG,
                "Kokoro Chinese lexicon syllable count does not match word: " +
                entry.word);
        }
        lexicon_[entry.word] = entry.phoneme;
        if (jieba_) jieba_->InsertUserWord(entry.word);
    }
    return ErrorInfo::ok();
}

void KokoroZhBackend::initializeJieba(const std::string& dict_dir) {
    std::string dict_path  = dict_dir + "/jieba.dict.utf8";
    std::string hmm_path   = dict_dir + "/hmm_model.utf8";
    std::string user_dict  = dict_dir + "/user.dict.utf8";
    std::string idf_path   = dict_dir + "/idf.utf8";
    std::string stop_words = dict_dir + "/stop_words.utf8";
    jieba_ = std::make_unique<cppjieba::Jieba>(
        dict_path, hmm_path, user_dict, idf_path, stop_words);
}

void KokoroZhBackend::initializePinyin() {
    // Reuse the shared downloader to locate the cpp-pinyin dictionary.
    TTSModelDownloader downloader;
    if (!downloader.ensureCppPinyin()) {
        throw std::runtime_error("Failed to obtain cpp-pinyin dictionary.");
    }
    std::string pinyin_dict_dir = downloader.getCppPinyinPath();
    std::cout << "[KokoroZh] Using cpp-pinyin dictionary at: " << pinyin_dict_dir << std::endl;
    Pinyin::setDictionaryPath(fs::path(pinyin_dict_dir));
    pinyin_ = std::make_unique<Pinyin::Pinyin>();
}

void KokoroZhBackend::initializeToneSandhi() {
    // Inject split_word callback (jieba CutForSearch)
    tone_sandhi_.setSplitWordFn([this](const std::string& word) -> std::vector<std::string> {
        std::vector<std::string> cuts;
        jieba_->CutForSearch(word, cuts, true);
        if (cuts.size() < 2) return {word};
        // Sort by length, take first (shortest) as sub0
        std::sort(cuts.begin(), cuts.end(),
                    [](const std::string& a, const std::string& b) {
                        return charLenHelper(a) < charLenHelper(b);
                    });
        std::string sub0 = cuts[0];
        size_t pos = word.find(sub0);
        if (pos == 0) {
            return {sub0, word.substr(sub0.size())};
        } else {
            return {word.substr(0, word.size() - sub0.size()), sub0};
        }
    });

    // Inject finals callback (cpp-pinyin TONE3)
    tone_sandhi_.setFinalsFn([this](const std::string& word) -> std::vector<std::string> {
        auto [initials, finals] = getInitialsFinals(word);
        return finals;
    });
}

std::vector<std::string> KokoroZhBackend::getChunkingUnits(
        const std::string& text) {
    if (!jieba_) {
        return KokoroBackend::getChunkingUnits(text);
    }

    std::vector<std::string> words;
    jieba_->Cut(text, words, true);
    if (words.empty()) {
        return KokoroBackend::getChunkingUnits(text);
    }

    std::vector<std::string> units;
    size_t offset = 0;
    for (const auto& word : words) {
        if (word.empty()) {
            continue;
        }
        const size_t position = text.find(word, offset);
        if (position == std::string::npos) {
            return KokoroBackend::getChunkingUnits(text);
        }
        if (position > offset) {
            const auto gap_units = KokoroBackend::getChunkingUnits(
                text.substr(offset, position - offset));
            units.insert(units.end(), gap_units.begin(), gap_units.end());
        }
        units.push_back(word);
        offset = position + word.size();
    }
    if (offset < text.size()) {
        const auto tail_units =
            KokoroBackend::getChunkingUnits(text.substr(offset));
        units.insert(units.end(), tail_units.begin(), tail_units.end());
    }
    return units;
}

std::vector<std::string> KokoroZhBackend::utf8Chars(const std::string& s) {
    return utf8CharsHelper(s);
}

std::pair<std::vector<std::string>, std::vector<std::string>>
KokoroZhBackend::getInitialsFinals(const std::string& word) {
    static const std::unordered_map<std::string, std::vector<std::string>>
        kPhrasePinyin = {
            {"开户行", {"kai1", "hu4", "hang2"}},
            {"发卡行", {"fa4", "ka3", "hang2"}},
            {"放款行", {"fang4", "kuan3", "hang2"}},
            {"行号", {"hang2", "hao4"}},
            {"各地", {"ge4", "di4"}},
            {"借还款", {"jie4", "huan2", "kuan3"}},
            {"时间为", {"shi2", "jian1", "wei2"}},
            {"为准", {"wei2", "zhun3"}},
            {"色差", {"se4", "cha1"}},
            {"掺和", {"chan1", "huo5"}},
            {"好吃", {"hao3", "chi1"}},
        };

    std::vector<std::string> raw_pinyins;
    auto custom = lexicon_.find(word);
    if (custom != lexicon_.end()) {
        std::istringstream stream(custom->second);
        for (std::string syllable; stream >> syllable;) {
            raw_pinyins.push_back(syllable);
        }
    } else {
        auto phrase = kPhrasePinyin.find(word);
        if (phrase != kPhrasePinyin.end()) {
            raw_pinyins = phrase->second;
        } else {
            auto converted = pinyin_->hanziToPinyin(
                word, Pinyin::ManTone::Style::TONE3,
                Pinyin::Default, false, false, true);
            for (const auto& item : converted) {
                if (!item.error && !item.pinyin.empty()) {
                    raw_pinyins.push_back(item.pinyin);
                }
            }
        }
    }

    // cpp-pinyin may collapse or drop unusual multi-character phrases.
    // Preserve every Han character by retrying character-by-character.
    const auto word_chars = utf8Chars(word);
    if (raw_pinyins.size() != word_chars.size()) {
        raw_pinyins.clear();
        for (const auto& character : word_chars) {
            auto converted = pinyin_->hanziToPinyin(
                character, Pinyin::ManTone::Style::TONE3,
                Pinyin::Default, false, false, true);
            if (!converted.empty() && !converted.front().error &&
                !converted.front().pinyin.empty()) {
                raw_pinyins.push_back(converted.front().pinyin);
            }
        }
    }

    std::vector<std::string> initials, finals;
    for (const auto& raw_pinyin : raw_pinyins) {
        std::string py = raw_pinyin;  // e.g. "zhong1", "wo3", "you3"
        // Separate trailing tone digit.
        char tone = '\0';
        if (!py.empty() && py.back() >= '0' && py.back() <= '5') {
            tone = py.back();
            py.pop_back();
        }

        // Real initials only (pypinyin excludes y/w, they map into the final).
        std::string ini, fin;
        for (const auto& prefix : {"zh", "ch", "sh", "b", "p", "m", "f", "d", "t", "n", "l",
                                    "g", "k", "h", "j", "q", "x", "r", "z", "c", "s"}) {
            size_t pl = strlen(prefix);
            if (py.size() > pl && py.compare(0, pl, prefix) == 0) {
                ini = prefix;
                fin = py.substr(pl);
                break;
            }
        }
        if (ini.empty()) {
            // Zero-initial syllable (incl. y/w). Reconstruct pypinyin final.
            ini = " ";
            fin = normalizeZeroInitialFinal(py);
        }

        if (ini == "j" || ini == "q" || ini == "x") {
            // After j/q/x, written "u" is phonetic ü (v):
            //   ju/qu/xu -> v, jun -> vn, jue -> ve
            if (fin == "u") fin = "v";
            else if (fin == "un") fin = "vn";
            else if (fin == "ue") fin = "ve";
            else if (fin == "uan") fin = "van";
            else fin = expandContractedFinal(fin);
        } else if (ini != " ") {
            // Expand contracted finals (iu/ui/un) after other real initials.
            fin = expandContractedFinal(fin);
        }

        // ii/iii special cases (zi/ci/si -> ii, zhi/chi/shi/ri -> iii)
        if (fin == "i") {
            if (ini == "z" || ini == "c" || ini == "s") fin = "ii";
            else if (ini == "zh" || ini == "ch" || ini == "sh" || ini == "r") fin = "iii";
        }

        initials.push_back(ini);
        finals.push_back(tone ? fin + std::string(1, tone) : fin + "5");
    }
    return {initials, finals};
}

std::pair<std::vector<std::string>, std::vector<std::string>>
KokoroZhBackend::mergeErhua(std::vector<std::string> initials,
                            std::vector<std::string> finals,
                            const std::string& word,
                            const std::string& pos) {
    auto ch = utf8Chars(word);
    if (finals.size() != ch.size()) return {initials, finals};

    static const std::unordered_set<std::string> kMustErhua = {
        "小院儿", "胡同儿", "范儿", "老汉儿", "撒欢儿", "寻老礼儿",
        "妥妥儿", "媳妇儿"
    };
    static const std::unordered_set<std::string> kNotErhua = {
        "虐儿", "为儿", "护儿", "瞒儿", "救儿", "替儿", "有儿", "一儿",
        "我儿", "俺儿", "妻儿", "拐儿", "聋儿", "乞儿", "患儿", "幼儿",
        "孤儿", "婴儿", "婴幼儿", "连体儿", "脑瘫儿", "流浪儿", "体弱儿",
        "混血儿", "蜜雪儿", "舫儿", "祖儿", "美儿", "应采儿", "可儿",
        "侄儿", "孙儿", "侄孙儿", "女儿", "男儿", "红孩儿", "花儿",
        "虫儿", "马儿", "鸟儿", "猪儿", "猫儿", "狗儿", "少儿"
    };

    bool not_erhua = kNotErhua.count(word) != 0;
    if (!not_erhua) {
        for (const auto& lexical : kNotErhua) {
            if (word.size() >= lexical.size() &&
                word.compare(word.size() - lexical.size(), lexical.size(),
                                lexical) == 0) {
                not_erhua = true;
                break;
            }
        }
    }

    if (!finals.empty() && ch.back() == "儿" && finals.back() == "er1") {
        finals.back() = "er2";
    }
    // For lexical 儿 (花儿、鸟儿、女儿, ...), Misaki keeps a standalone
    // er2 syllable. ToneSandhi's generic suffix rule may have made it er5.
    if (!finals.empty() && ch.back() == "儿" && not_erhua) {
        finals.back() = "er2";
    }
    if (kMustErhua.count(word) == 0 &&
        (not_erhua || pos == "a" || pos == "j" || pos == "nr")) {
        return {initials, finals};
    }

    std::vector<std::string> new_ini, new_fin;
    for (size_t i = 0; i < ch.size(); ++i) {
        if (i == ch.size() - 1 && ch[i] == "儿" &&
            (finals[i] == "er2" || finals[i] == "er5") && !new_fin.empty()) {
            // Merge: previous final "aoX" -> "aoRX"
            std::string& prev = new_fin.back();
            if (!prev.empty() && prev.back() >= '0' && prev.back() <= '5') {
                char tone = prev.back();
                prev = prev.substr(0, prev.size() - 1) + "R" + tone;
            }
        } else {
            new_ini.push_back(initials[i]);
            new_fin.push_back(finals[i]);
        }
    }
    return {new_ini, new_fin};
}

std::string KokoroZhBackend::zhMap(const std::string& pinyin) {
    auto it = zh_map_.find(pinyin);
    return it != zh_map_.end() ? it->second : "❓";
}

std::vector<int64_t> KokoroZhBackend::textToTokenIds(const std::string& text) {
    std::vector<int64_t> token_ids;
    if (!jieba_ || !pinyin_) {
        std::cerr << "[KokoroZh] Error: frontend not initialized" << std::endl;
        return token_ids;
    }

    // Punctuation normalization (misaki map_punctuation): full-width CJK
    // punctuation -> half-width + trailing space. The half-width symbol and the
    // space are both real tokens in tokens.txt and produce natural prosody
    // pauses (", " = light pause, "." = sentence stop), unlike the "/" word
    // separator which must NOT stand in for punctuation.
    std::string processed = normalizeSemanticForms(text);
    // Reuse the same date -> phone -> number FST chain as Matcha. The local
    // fallback below only handles digits that remain after those contextual
    // rules.
    processed = text::normalizeText(processed, text::Language::ZH);
    auto rep = [&](const char* from, const char* to) {
        processed = std::regex_replace(processed, std::regex(from), to);
    };
    rep("、", ", "); rep("，", ", ");
    rep("。", ". "); rep("．", ". ");
    rep("！", "! ");
    rep("：", ": ");
    rep("；", "; ");
    rep("？", "? ");
    rep("《", " “"); rep("》", "” ");
    rep("「", " “"); rep("」", "” ");
    rep("『", " “"); rep("』", "” ");
    rep("（", " ("); rep("）", ") ");

    // Fallback for any numeric run the FST intentionally left untouched.
    processed = normalizeArabicNumbers(processed);

    // 1. Jieba POS tagging
    std::vector<std::pair<std::string, std::string>> seg_cut;
    jieba_->Tag(processed, seg_cut);

    // 2. ToneSandhi pre-merge
    std::vector<ToneSandhi::Seg> seg_ts;
    for (const auto& wp : seg_cut) seg_ts.push_back(wp);
    seg_ts = tone_sandhi_.preMergeForModify(seg_ts);

    // 3. Process each word
    token_ids.push_back(0);  // BOS
    bool first_word = true;  // reset after punctuation: no "/" right after punc
    bool previous_numeric_expression = false;
    for (const auto& [word, pos] : seg_ts) {
        if (word.empty()) continue;
        if (word == " ") {
            // Whitespace token (from map_punctuation) -> emit " " (id 16)
            auto it = token_to_id_.find(" ");
            if (it != token_to_id_.end()) token_ids.push_back(it->second);
            previous_numeric_expression = false;
            continue;
        }
        // cppjieba's Tag() sometimes mislabels ordinary Chinese words as "x"
        // (e.g. "我用" -> pos="x"). Only treat a word as non-Chinese here if it
        // has no CJK bytes; otherwise fall through to the normal pinyin pipeline
        // so tones/separators are emitted correctly.
        const auto word_chars = utf8Chars(word);
        bool has_cjk = std::any_of(
            word_chars.begin(), word_chars.end(),
            [](const std::string& ch) { return text::isChineseChar(ch); });
        if ((pos == "x" || pos == "eng") && !has_cjk) {
            const bool has_ascii_letter = std::any_of(
                word.begin(), word.end(),
                [](unsigned char c) { return std::isalpha(c) != 0; });
            if (has_ascii_letter) {
                // Reuse the same C++ English frontend as Kokoro v1.0. Token IDs
                // for the shared IPA alphabet are stable in v1.1-zh.
                auto english_ids = english_frontend_.englishTextToTokenIds(word);
                if (english_ids.size() >= 2 &&
                    english_ids.front() == 0 && english_ids.back() == 0) {
                    english_ids.erase(english_ids.begin());
                    english_ids.pop_back();
                }
                token_ids.insert(token_ids.end(), english_ids.begin(), english_ids.end());
                first_word = true;
                previous_numeric_expression = false;
                continue;
            }
            // Punctuation / other non-Chinese: direct token lookup, char by char.
            // misaki's map_punctuation emits "<punct> " (trailing space); jieba's
            // pre-filter drops that space, so re-emit it after sentence/clause
            // punctuation to match the reference prosody.
            static const std::string kSpacedPunc = ",.!?;:";
            for (const auto& ch : utf8Chars(word)) {
                auto it = token_to_id_.find(ch);
                if (it != token_to_id_.end()) token_ids.push_back(it->second);
                if (ch.size() == 1 && kSpacedPunc.find(ch) != std::string::npos) {
                    auto sp = token_to_id_.find(" ");
                    if (sp != token_to_id_.end()) token_ids.push_back(sp->second);
                }
            }
            // Punctuation ends a run: the next word starts fresh (no leading "/").
            first_word = true;
            previous_numeric_expression = false;
            continue;
        }

        const auto is_numeric_expression_char = [](const std::string& ch) {
            static const std::string kChars =
                "零〇一二三四五六七八九十百千万亿年月日号点时分秒";
            return kChars.find(ch) != std::string::npos;
        };
        const bool current_numeric_expression =
            !word_chars.empty() &&
            std::all_of(
                word_chars.begin(), word_chars.end(),
                is_numeric_expression_char);

        // Insert "/" separator only between consecutive Chinese words.
        if (!first_word && separator_id_ >= 0 &&
            !(previous_numeric_expression && current_numeric_expression)) {
            token_ids.push_back(separator_id_);
        }
        first_word = false;
        previous_numeric_expression = current_numeric_expression;

        // Get initials + finals via cpp-pinyin
        auto [initials, finals] = getInitialsFinals(word);

        // Apply ToneSandhi
        finals = tone_sandhi_.modifiedTone(word, pos, finals);

        // Merge erhua
        std::tie(initials, finals) = mergeErhua(initials, finals, word, pos);

        // Convert to bopomofo via ZH_MAP, then to token IDs
        for (size_t i = 0; i < initials.size() && i < finals.size(); ++i) {
            if (finals[i].empty()) {
                std::cerr << "[KokoroZh] Empty final in '" << word << "'"
                            << std::endl;
                return {};
            }
            std::string ini_bpmf = zhMap(initials[i]);
            std::string fin_no_tone = finals[i].substr(0, finals[i].size() - 1);
            std::string tone_digit(1, finals[i].back());

            // Handle erhua "R" inside final (e.g. "aoR3" -> "ao"+"R"+"3")
            size_t r_pos = fin_no_tone.find('R');
            std::string fin_part1, fin_r;
            if (r_pos != std::string::npos) {
                fin_part1 = fin_no_tone.substr(0, r_pos);
                fin_r = "R";
                fin_no_tone = fin_part1;
            }

            // Emit: initial (if not space), final, [R], tone
            if (ini_bpmf != " ") {
                auto it = token_to_id_.find(ini_bpmf);
                if (it == token_to_id_.end()) {
                    std::cerr << "[KokoroZh] Unknown initial in '" << word
                                << "': " << initials[i] << std::endl;
                    return {};
                }
                token_ids.push_back(it->second);
            }
            if (!fin_no_tone.empty()) {
                std::string fin_bpmf = zhMap(fin_no_tone);
                auto it = token_to_id_.find(fin_bpmf);
                if (it == token_to_id_.end()) {
                    std::cerr << "[KokoroZh] Unknown final in '" << word
                                << "': " << fin_no_tone << std::endl;
                    return {};
                }
                token_ids.push_back(it->second);
            }
            if (!fin_r.empty()) {
                auto it = token_to_id_.find(fin_r);
                if (it == token_to_id_.end()) return {};
                token_ids.push_back(it->second);
            }
            auto it = token_to_id_.find(tone_digit);
            if (it == token_to_id_.end()) return {};
            token_ids.push_back(it->second);
        }
    }

    token_ids.push_back(0);  // EOS
    return token_ids.size() <= 2 ? std::vector<int64_t>{} : token_ids;
}

}  // namespace tts
