/* Copyright (C) 2025 SpacemiT Co., Ltd.
    * SPDX-License-Identifier: Apache-2.0 */

#include "backends/kokoro/kokoro_zh_backend.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "backends/matcha/tts_model_downloader.hpp"
#include "cpp-pinyin/G2pglobal.h"
#include "cpp-pinyin/Pinyin.h"
#include "cppjieba/Jieba.hpp"
#include "text/text_utils.hpp"
#include "text/token_utils.hpp"

namespace fs = std::filesystem;

namespace tts {

namespace {
// misaki ZH_MAP: pinyin syllable/initial/final/tone -> bopomofo
const std::unordered_map<std::string, std::string> ZH_MAP_DATA = {
    {"b", "ㄅ"}, {"p", "ㄆ"}, {"m", "ㄇ"}, {"f", "ㄈ"}, {"d", "ㄉ"}, {"t", "ㄊ"}, {"n", "ㄋ"}, {"l", "ㄌ"}, {"g", "ㄍ"},
    {"k", "ㄎ"}, {"h", "ㄏ"}, {"j", "ㄐ"}, {"q", "ㄑ"}, {"x", "ㄒ"}, {"zh", "ㄓ"}, {"ch", "ㄔ"}, {"sh", "ㄕ"},
    {"r", "ㄖ"}, {"z", "ㄗ"}, {"c", "ㄘ"}, {"s", "ㄙ"}, {"a", "ㄚ"}, {"o", "ㄛ"}, {"e", "ㄜ"}, {"ie", "ㄝ"},
    {"ai", "ㄞ"}, {"ei", "ㄟ"}, {"ao", "ㄠ"}, {"ou", "ㄡ"}, {"an", "ㄢ"}, {"en", "ㄣ"}, {"ang", "ㄤ"}, {"eng", "ㄥ"},
    {"er", "ㄦ"}, {"i", "ㄧ"}, {"u", "ㄨ"}, {"v", "ㄩ"}, {"ii", "ㄭ"}, {"iii", "十"}, {"ve", "月"}, {"ia", "压"},
    {"ian", "言"}, {"iang", "阳"}, {"iao", "要"}, {"in", "阴"}, {"ing", "应"}, {"iong", "用"}, {"iou", "又"},
    {"ong", "中"}, {"ua", "穵"}, {"uai", "外"}, {"uan", "万"}, {"uang", "王"}, {"uei", "为"}, {"uen", "文"},
    {"ueng", "瓮"}, {"uo", "我"}, {"van", "元"}, {"vn", "云"},
    // Punctuation passthrough
    {";", ";"}, {":", ";"}, {",", ","}, {".", "。"}, {"!", "!"}, {"?", "?"}, {"—", "—"}, {"…", "…"}, {"\"", "\""},
    {"(", "("}, {")", ")"}, {" ", " "},
    // Tone digits passthrough
    {"0", "0"}, {"1", "1"}, {"2", "2"}, {"3", "3"}, {"4", "4"}, {"5", "5"}, {"R", "R"}  // erhua marker
};

// Split UTF-8 string into character vector
std::vector<std::string> utf8CharsHelper(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if (c >= 0xF0)
            len = 4;
        else if (c >= 0xE0)
            len = 3;
        else if (c >= 0xC0)
            len = 2;
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
        const char escaped = input[++i];
        if (escaped == '"' || escaped == '\\' || escaped == '/') {
            out += escaped;
        } else if (escaped == 'u' && i + 4 < input.size()) {
            unsigned codepoint = 0;
            bool valid = true;
            for (size_t j = 1; j <= 4; ++j) {
                const char hex = input[i + j];
                codepoint <<= 4;
                if (hex >= '0' && hex <= '9') {
                    codepoint += hex - '0';
                } else if (hex >= 'a' && hex <= 'f') {
                    codepoint += hex - 'a' + 10;
                } else if (hex >= 'A' && hex <= 'F') {
                    codepoint += hex - 'A' + 10;
                } else {
                    valid = false;
                }
            }
            if (valid) {
                if (codepoint <= 0x7f) {
                    out += static_cast<char>(codepoint);
                } else if (codepoint <= 0x7ff) {
                    out += static_cast<char>(0xc0 | (codepoint >> 6));
                    out += static_cast<char>(0x80 | (codepoint & 0x3f));
                } else {
                    out += static_cast<char>(0xe0 | (codepoint >> 12));
                    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
                    out += static_cast<char>(0x80 | (codepoint & 0x3f));
                }
                i += 4;
            }
        } else if (escaped == 'n') {
            out += '\n';
        } else if (escaped == 't') {
            out += '\t';
        }
    }
    return out;
}

size_t charLenHelper(const std::string& s) { return utf8CharsHelper(s).size(); }

// Kokoro's tokens.txt stores a literal space token as "  16". Parsing with
// operator>> drops that token. Split at the last whitespace delimiter instead,
// preserving everything before it as the token text.
std::unordered_map<std::string, int64_t> readKokoroTokenMap(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Failed to open Kokoro tokens file: " + path);
    }

    std::unordered_map<std::string, int64_t> token_to_id;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t delimiter = line.find_last_of(" \t");
        if (delimiter == std::string::npos || delimiter + 1 >= line.size()) {
            throw std::runtime_error("Invalid Kokoro token entry: " + line);
        }
        const std::string token = line.substr(0, delimiter);
        const int64_t id = std::stoll(line.substr(delimiter + 1));
        token_to_id[token] = id;
    }
    return token_to_id;
}

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
        if (start < 0) {
            cnt += start;
            start = 0;
        }
        std::string grp = d.substr(start, cnt);
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
            if (prev_zero && !out.empty()) out += CN_DIGITS[0];
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
            if (!out.empty()) out += " ";
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
        if (!is_digit) {
            out += ch;
            i++;
            continue;
        }
        // collect digit run (allow one '.')
        std::string num;
        bool has_dot = false;
        size_t j = i;
        while (j < chars.size()) {
            const std::string& d = chars[j];
            if (d.size() == 1 && d[0] >= '0' && d[0] <= '9') {
                num += d;
                j++;
                continue;
            }
            if (d == "." && !has_dot && j + 1 < chars.size() && chars[j + 1].size() == 1 && chars[j + 1][0] >= '0' &&
                chars[j + 1][0] <= '9') {
                has_dot = true;
                num += ".";
                j++;
                continue;
            }
            break;
        }
        bool prev_di = (!out.empty() && (out.size() >= std::string("第").size()) &&
            out.compare(out.size() - std::string("第").size(), std::string("第").size(), "第") == 0);
        bool next_nian = (j < chars.size() && chars[j] == "年");
        if (has_dot) {
            size_t dot = num.find('.');
            out += intToChinese(num.substr(0, dot)) + "点" + digitsToChinese(num.substr(dot + 1));
        } else if (next_nian || (prev_di && num.size() >= 2)) {
            out += digitsToChinese(num);
        } else {
            out += intToChinese(num);
        }
        i = j;
    }
    return out;
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
    static const std::unordered_map<std::string, std::string> kMap = {{"yi", "i"}, {"ya", "ia"}, {"ye", "ie"},
        {"yao", "iao"}, {"you", "iou"}, {"yan", "ian"}, {"yin", "in"}, {"yang", "iang"}, {"ying", "ing"},
        {"yong", "iong"}, {"yu", "v"}, {"yue", "ve"}, {"yuan", "van"}, {"yun", "vn"}, {"wu", "u"}, {"wa", "ua"},
        {"wo", "uo"}, {"wai", "uai"}, {"wei", "uei"}, {"wan", "uan"}, {"wen", "uen"}, {"wang", "uang"},
        {"weng", "ueng"},
        // bare "y"/"w" degenerate cases
        {"y", "i"}, {"w", "u"}};
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
        const std::string subdir_path = getModelDir() + "/" + getModelSubdir();

        if (fs::exists(subdir_path + "/tokens.txt")) {
            token_to_id_ = readKokoroTokenMap(subdir_path + "/tokens.txt");
        } else if (!loadTokenizerJson(subdir_path + "/tokenizer.json")) {
            throw std::runtime_error(
                "Neither a valid tokens.txt nor tokenizer.json was found in " +
                subdir_path);
        }
        if (token_to_id_.find(" ") == token_to_id_.end()) {
            throw std::runtime_error("Kokoro Chinese vocabulary is missing the space token");
        }
        auto sep_it = token_to_id_.find("/");
        separator_id_ = (sep_it != token_to_id_.end()) ? sep_it->second : -1;

        if (fs::exists(subdir_path + "/lexicon-zh.txt")) {
            lexicon_ = text::readLexicon(subdir_path + "/lexicon-zh.txt");
            std::cout << "[KokoroZh] Loaded " << lexicon_.size()
                        << " lexicon entries" << std::endl;
        }

        english_phonemizer_.initEnglish();
        std::string english_lexicon_dir = subdir_path;
        if (!fs::exists(english_lexicon_dir + "/us_gold.json") ||
            !fs::exists(english_lexicon_dir + "/us_silver.json")) {
            const std::string sibling_english_dir = getModelDir() + "/kokoro-v1.0-en";
            if (fs::exists(sibling_english_dir + "/us_gold.json") &&
                fs::exists(sibling_english_dir + "/us_silver.json")) {
                english_lexicon_dir = sibling_english_dir;
            }
        }
        english_phonemizer_.initEnglishLexicon(english_lexicon_dir);

        TTSModelDownloader downloader;
        if (!downloader.ensureCppJieba()) {
            throw std::runtime_error("Failed to obtain cppjieba dictionary.");
        }
        initializeJieba(downloader.getCppJiebaPath());
        initializePinyin();
        initializeToneSandhi();

        zh_map_ = ZH_MAP_DATA;

        return ErrorInfo::ok();
    } catch (const std::exception& e) {
        return ErrorInfo::error(
            ErrorCode::INTERNAL_ERROR, std::string("Failed to initialize Kokoro Chinese frontend: ") + e.what());
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
        const size_t begin = json.find('"', cursor);
        if (begin == std::string::npos) break;
        std::string raw;
        bool escaped = false;
        size_t end = begin + 1;
        for (; end < json.size(); ++end) {
            const char c = json[end];
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
        const size_t colon = json.find(':', end + 1);
        if (colon == std::string::npos) return false;
        const size_t value = json.find_first_of("0123456789", colon + 1);
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

void KokoroZhBackend::initializeJieba(const std::string& dict_dir) {
    std::string dict_path = dict_dir + "/jieba.dict.utf8";
    std::string hmm_path = dict_dir + "/hmm_model.utf8";
    std::string user_dict = dict_dir + "/user.dict.utf8";
    std::string idf_path = dict_dir + "/idf.utf8";
    std::string stop_words = dict_dir + "/stop_words.utf8";
    jieba_ = std::make_unique<cppjieba::Jieba>(dict_path, hmm_path, user_dict, idf_path, stop_words);
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
            [](const std::string& a, const std::string& b) { return charLenHelper(a) < charLenHelper(b); });
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

std::vector<std::string> KokoroZhBackend::utf8Chars(const std::string& s) { return utf8CharsHelper(s); }

std::pair<std::vector<std::string>, std::vector<std::string>> KokoroZhBackend::getInitialsFinals(
    const std::string& word) {
    auto res = pinyin_->hanziToPinyin(word, Pinyin::ManTone::Style::TONE3, Pinyin::Default, true, false, true);
    std::vector<std::string> initials, finals;
    for (const auto& pr : res) {
        std::string py = pr.pinyin;  // e.g. "zhong1", "wo3", "you3"
        // Separate trailing tone digit.
        char tone = '\0';
        if (!py.empty() && py.back() >= '0' && py.back() <= '5') {
            tone = py.back();
            py.pop_back();
        }

        // Real initials only (pypinyin excludes y/w, they map into the final).
        std::string ini, fin;
        for (const auto& prefix : {"zh", "ch", "sh", "b", "p", "m", "f", "d", "t", "n", "l", "g", "k", "h", "j", "q",
                    "x", "r", "z", "c", "s"}) {
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
            if (fin == "u")
                fin = "v";
            else if (fin == "un")
                fin = "vn";
            else if (fin == "ue")
                fin = "ve";
            else if (fin == "uan")
                fin = "van";
            else
                fin = expandContractedFinal(fin);
        } else if (ini != " ") {
            // Expand contracted finals (iu/ui/un) after other real initials.
            fin = expandContractedFinal(fin);
        }

        // ii/iii special cases (zi/ci/si -> ii, zhi/chi/shi/ri -> iii)
        if (fin == "i") {
            if (ini == "z" || ini == "c" || ini == "s")
                fin = "ii";
            else if (ini == "zh" || ini == "ch" || ini == "sh" || ini == "r")
                fin = "iii";
        }

        initials.push_back(ini);
        finals.push_back(tone ? fin + std::string(1, tone) : fin + "5");
    }
    return {initials, finals};
}

std::pair<std::vector<std::string>, std::vector<std::string>> KokoroZhBackend::mergeErhua(
    std::vector<std::string> initials, std::vector<std::string> finals, const std::string& word,
    const std::string& pos) {
    auto ch = utf8Chars(word);
    if (finals.size() != ch.size()) return {initials, finals};

    // For simplicity, skip must_erhua/not_erhua checks (misaki has lists)
    // Just apply the core rule: trailing "儿" with er2/er5 merges into previous final as XRY
    std::vector<std::string> new_ini, new_fin;
    for (size_t i = 0; i < ch.size(); ++i) {
        if (i == ch.size() - 1 && ch[i] == "儿" && (finals[i] == "er2" || finals[i] == "er5") && !new_fin.empty()) {
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
    // pauses ("," = light pause, "." = sentence stop), unlike the "/" word
    // separator which must NOT stand in for punctuation.
    std::string processed = text;
    auto rep = [&](const char* from, const char* to) {
        processed = std::regex_replace(processed, std::regex(from), to);
    };
    rep("、", ", ");
    rep("，", ", ");
    rep("。", ". ");
    rep("．", ". ");
    rep("！", "! ");
    rep("：", ": ");
    rep("；", "; ");
    rep("？", "? ");
    const size_t first_non_space = processed.find_first_not_of(" \t\r\n");
    if (first_non_space == std::string::npos) {
        return {};
    }
    const size_t last_non_space = processed.find_last_not_of(" \t\r\n");
    processed = processed.substr(first_non_space, last_non_space - first_non_space + 1);

    // Arabic digits -> Chinese numerals (cn2an an2cn subset), so jieba +
    // cpp-pinyin can pronounce them (3 -> 三, 2024年 -> 二零二四年).
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
    bool last_token_was_space = false;
    const auto isEnglishWord = [](const std::string& value) {
        bool has_letter = false;
        for (const unsigned char ch : value) {
            if (std::isalpha(ch) != 0) {
                has_letter = true;
                continue;
            }
            if (ch != '\'' && ch != '-') {
                return false;
            }
        }
        return has_letter;
    };
    for (size_t segment_index = 0; segment_index < seg_ts.size(); ++segment_index) {
        const std::string& word = seg_ts[segment_index].first;
        const std::string& pos = seg_ts[segment_index].second;
        if (word.empty()) continue;
        if (word == " ") {
            // Whitespace token (from map_punctuation) -> emit " " (id 16)
            auto it = token_to_id_.find(" ");
            if (it != token_to_id_.end() && !last_token_was_space) {
                token_ids.push_back(it->second);
            }
            last_token_was_space = true;
            continue;
        }
        // cppjieba's Tag() sometimes mislabels ordinary Chinese words as "x"
        // (e.g. "我用" -> pos="x"). Only treat a word as non-Chinese here if it
        // has no CJK bytes; otherwise fall through to the normal pinyin pipeline
        // so tones/separators are emitted correctly.
        bool has_cjk = false;
        for (unsigned char c : word) {
            if (c >= 0x80) {
                has_cjk = true;
                break;
            }
        }
        if ((pos == "x" || pos == "eng") && !has_cjk) {
            // Match Misaki ZHG2P's en_callable contract: pass a complete
            // contiguous English phrase to the English frontend. This preserves
            // word pronunciation, function-word context and phrase-level stress.
            if (isEnglishWord(word)) {
                std::string english_text = word;
                size_t english_end = segment_index;
                while (english_end + 1 < seg_ts.size()) {
                    const std::string& next_word = seg_ts[english_end + 1].first;
                    if (next_word == " " || isEnglishWord(next_word)) {
                        english_text += next_word;
                        ++english_end;
                        continue;
                    }
                    break;
                }
                while (!english_text.empty() && std::isspace(static_cast<unsigned char>(english_text.back())) != 0) {
                    english_text.pop_back();
                }

                const std::string english_phonemes = english_phonemizer_.englishTextToPhonemes(english_text);
                if (!english_phonemes.empty()) {
                    auto space = token_to_id_.find(" ");
                    if (token_ids.size() > 1 && !last_token_was_space && space != token_to_id_.end()) {
                        token_ids.push_back(space->second);
                    }
                    for (const auto& ch : utf8Chars(english_phonemes)) {
                        auto it = token_to_id_.find(ch);
                        if (it != token_to_id_.end()) {
                            token_ids.push_back(it->second);
                            last_token_was_space = ch == " ";
                        }
                    }
                    const size_t next_index = english_end + 1;
                    if (next_index < seg_ts.size()) {
                        bool next_has_cjk = false;
                        for (const unsigned char ch : seg_ts[next_index].first) {
                            if (ch >= 0x80) {
                                next_has_cjk = true;
                                break;
                            }
                        }
                        if (next_has_cjk && !last_token_was_space && space != token_to_id_.end()) {
                            token_ids.push_back(space->second);
                            last_token_was_space = true;
                        }
                    }
                }
                segment_index = english_end;
                first_word = true;
                continue;
            }
            // Punctuation / other non-Chinese: direct token lookup, char by char.
            // misaki's map_punctuation emits "<punct> " (trailing space); jieba's
            // pre-filter drops that space, so re-emit it after sentence/clause
            // punctuation to match the reference prosody.
            static const std::string kSpacedPunc = ",.!?;:";
            for (const auto& ch : utf8Chars(word)) {
                if (ch == " ") {
                    auto sp = token_to_id_.find(" ");
                    if (sp != token_to_id_.end() && !last_token_was_space) {
                        token_ids.push_back(sp->second);
                    }
                    last_token_was_space = true;
                    continue;
                }
                auto it = token_to_id_.find(ch);
                if (it != token_to_id_.end()) {
                    token_ids.push_back(it->second);
                    last_token_was_space = false;
                }
                if (ch.size() == 1 && kSpacedPunc.find(ch) != std::string::npos) {
                    auto sp = token_to_id_.find(" ");
                    if (sp != token_to_id_.end()) {
                        token_ids.push_back(sp->second);
                        last_token_was_space = true;
                    }
                }
            }
            // Punctuation ends a run: the next word starts fresh (no leading "/").
            first_word = true;
            continue;
        }

        // Insert "/" separator only between consecutive Chinese words.
        if (!first_word && separator_id_ >= 0) {
            token_ids.push_back(separator_id_);
        }
        first_word = false;
        last_token_was_space = false;

        // Get initials + finals via cpp-pinyin
        auto [initials, finals] = getInitialsFinals(word);

        // Apply ToneSandhi
        finals = tone_sandhi_.modifiedTone(word, pos, finals);

        // Merge erhua
        std::tie(initials, finals) = mergeErhua(initials, finals, word, pos);

        // Convert to bopomofo via ZH_MAP, then to token IDs
        for (size_t i = 0; i < initials.size() && i < finals.size(); ++i) {
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
                if (it != token_to_id_.end()) token_ids.push_back(it->second);
            }
            if (!fin_no_tone.empty()) {
                std::string fin_bpmf = zhMap(fin_no_tone);
                auto it = token_to_id_.find(fin_bpmf);
                if (it != token_to_id_.end()) token_ids.push_back(it->second);
            }
            if (!fin_r.empty()) {
                auto it = token_to_id_.find(fin_r);
                if (it != token_to_id_.end()) token_ids.push_back(it->second);
            }
            auto it = token_to_id_.find(tone_digit);
            if (it != token_to_id_.end()) token_ids.push_back(it->second);
        }
    }

    const auto space_it = token_to_id_.find(" ");
    while (space_it != token_to_id_.end() && token_ids.size() > 1 && token_ids.back() == space_it->second) {
        token_ids.pop_back();
    }
    token_ids.push_back(0);  // EOS
    return token_ids.size() <= 2 ? std::vector<int64_t>{} : token_ids;
}

}  // namespace tts
