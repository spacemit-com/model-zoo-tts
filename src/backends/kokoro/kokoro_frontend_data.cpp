/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#include "backends/kokoro/kokoro_frontend_data.hpp"

namespace tts::kokoro_frontend_data {

const std::vector<std::pair<std::string, int64_t>>& kokoroVocabulary() {
    // Official Kokoro v1.0 vocabulary from hexgrad/Kokoro-82M config.json.
    static const std::vector<std::pair<std::string, int64_t>> kVocabulary = {
        {";", 1}, {":", 2}, {",", 3}, {".", 4}, {"!", 5}, {"?", 6},
        {"—", 9}, {"…", 10}, {"\"", 11}, {"(", 12}, {")", 13},
        {"“", 14}, {"”", 15}, {" ", 16}, {"̃", 17},
        {"ʣ", 18}, {"ʥ", 19}, {"ʦ", 20}, {"ʨ", 21},
        {"ᵝ", 22}, {"ꭧ", 23},
        {"A", 24}, {"I", 25}, {"O", 31}, {"Q", 33}, {"S", 35},
        {"T", 36}, {"W", 39}, {"Y", 41}, {"ᵊ", 42},
        {"a", 43}, {"b", 44}, {"c", 45}, {"d", 46}, {"e", 47},
        {"f", 48}, {"h", 50}, {"i", 51}, {"j", 52}, {"k", 53},
        {"l", 54}, {"m", 55}, {"n", 56}, {"o", 57}, {"p", 58},
        {"q", 59}, {"r", 60}, {"s", 61}, {"t", 62}, {"u", 63},
        {"v", 64}, {"w", 65}, {"x", 66}, {"y", 67}, {"z", 68},
        {"ɑ", 69}, {"ɐ", 70}, {"ɒ", 71}, {"æ", 72},
        {"β", 75}, {"ɔ", 76}, {"ɕ", 77}, {"ç", 78},
        {"ɖ", 80}, {"ð", 81}, {"ʤ", 82}, {"ə", 83},
        {"ɚ", 85}, {"ɛ", 86}, {"ɜ", 87}, {"ɟ", 90},
        {"ɡ", 92}, {"ɥ", 99}, {"ɨ", 101}, {"ɪ", 102},
        {"ʝ", 103}, {"ɯ", 110}, {"ɰ", 111}, {"ŋ", 112},
        {"ɳ", 113}, {"ɲ", 114}, {"ɴ", 115}, {"ø", 116},
        {"ɸ", 118}, {"θ", 119}, {"œ", 120}, {"ɹ", 123},
        {"ɾ", 125}, {"ɻ", 126}, {"ʁ", 128}, {"ɽ", 129},
        {"ʂ", 130}, {"ʃ", 131}, {"ʈ", 132}, {"ʧ", 133},
        {"ʊ", 135}, {"ʋ", 136}, {"ʌ", 138}, {"ɣ", 139},
        {"ɤ", 140}, {"χ", 142}, {"ʎ", 143}, {"ʒ", 147},
        {"ʔ", 148}, {"ˈ", 156}, {"ˌ", 157}, {"ː", 158},
        {"ʰ", 162}, {"ʲ", 164}, {"↓", 169}, {"→", 171},
        {"↗", 172}, {"↘", 173}, {"ᵻ", 177},
    };
    return kVocabulary;
}

const StringMap& pinyinInitialToIpa() {
    static const StringMap kMap = {
        {"b", "p"}, {"p", "pʰ"}, {"m", "m"}, {"f", "f"},
        {"d", "t"}, {"t", "tʰ"}, {"n", "n"}, {"l", "l"},
        {"g", "k"}, {"k", "kʰ"}, {"h", "x"}, {"j", "tɕ"},
        {"q", "tɕʰ"}, {"x", "ɕ"}, {"zh", "ʈʂ"},
        {"ch", "ʈʂʰ"}, {"sh", "ʂ"}, {"r", "ɻ"},
        {"z", "ts"}, {"c", "tsʰ"}, {"s", "s"}, {"y", "j"},
        {"w", "w"},
    };
    return kMap;
}

const StringMap& pinyinFinalToIpa() {
    static const StringMap kMap = {
        {"a", "a"}, {"ai", "ai"}, {"an", "an"}, {"ang", "aŋ"},
        {"ao", "au"}, {"e", "ɤ"}, {"ei", "ei"}, {"en", "ən"},
        {"eng", "əŋ"}, {"er", "əɻ"}, {"i", "i"},
        {"ia", "ja"}, {"ian", "jɛn"}, {"iang", "jaŋ"},
        {"iao", "jau"}, {"ie", "je"}, {"in", "in"}, {"ing", "iŋ"},
        {"iong", "jʊŋ"}, {"iu", "jou"}, {"o", "o"},
        {"ong", "ʊŋ"}, {"ou", "ou"}, {"u", "u"}, {"ua", "wa"},
        {"uai", "wai"}, {"uan", "wan"}, {"uang", "waŋ"},
        {"ue", "ɥe"}, {"ui", "wei"}, {"un", "wən"},
        {"uo", "wo"}, {"v", "y"}, {"ve", "ɥe"},
        {"van", "ɥɛn"}, {"vn", "yn"},
    };
    return kMap;
}

const std::vector<std::string>& orderedPinyinInitials() {
    static const std::vector<std::string> kInitials = {
        "zh", "ch", "sh", "b", "p", "m", "f", "d", "t", "n", "l",
        "g", "k", "h", "j", "q", "x", "r", "z", "c", "s", "y", "w",
    };
    return kInitials;
}

const StringMap& zhBopomofoMap() {
    static const StringMap kMap = {
        {"b", "ㄅ"}, {"p", "ㄆ"}, {"m", "ㄇ"}, {"f", "ㄈ"},
        {"d", "ㄉ"}, {"t", "ㄊ"}, {"n", "ㄋ"}, {"l", "ㄌ"},
        {"g", "ㄍ"}, {"k", "ㄎ"}, {"h", "ㄏ"}, {"j", "ㄐ"},
        {"q", "ㄑ"}, {"x", "ㄒ"}, {"zh", "ㄓ"}, {"ch", "ㄔ"},
        {"sh", "ㄕ"}, {"r", "ㄖ"}, {"z", "ㄗ"}, {"c", "ㄘ"},
        {"s", "ㄙ"}, {"a", "ㄚ"}, {"o", "ㄛ"}, {"e", "ㄜ"},
        {"ie", "ㄝ"}, {"ai", "ㄞ"}, {"ei", "ㄟ"}, {"ao", "ㄠ"},
        {"ou", "ㄡ"}, {"an", "ㄢ"}, {"en", "ㄣ"}, {"ang", "ㄤ"},
        {"eng", "ㄥ"}, {"er", "ㄦ"}, {"i", "ㄧ"}, {"u", "ㄨ"},
        {"v", "ㄩ"}, {"ii", "ㄭ"}, {"iii", "十"}, {"ve", "月"},
        {"ia", "压"}, {"ian", "言"}, {"iang", "阳"}, {"iao", "要"},
        {"in", "阴"}, {"ing", "应"}, {"iong", "用"}, {"iou", "又"},
        {"ong", "中"}, {"ua", "穵"}, {"uai", "外"}, {"uan", "万"},
        {"uang", "王"}, {"uei", "为"}, {"uen", "文"}, {"ueng", "瓮"},
        {"uo", "我"}, {"van", "元"}, {"vn", "云"},
        {";", ";"}, {":", ";"}, {",", ","}, {".", "。"}, {"!", "!"},
        {"?", "?"}, {"—", "—"}, {"…", "…"}, {"\"", "\""},
        {"(", "("}, {")", ")"}, {" ", " "}, {"0", "0"}, {"1", "1"},
        {"2", "2"}, {"3", "3"}, {"4", "4"}, {"5", "5"}, {"R", "R"},
    };
    return kMap;
}

const std::unordered_map<std::string, std::vector<std::string>>&
zhPhrasePinyin() {
    // Misaki v1.1 zh_frontend.py phrases_dict.
    static const std::unordered_map<std::string, std::vector<std::string>> kMap = {
        {"开户行", {"kai1", "hu4", "hang2"}},
        {"发卡行", {"fa4", "ka3", "hang2"}},
        {"放款行", {"fang4", "kuan3", "hang2"}},
        {"茧行", {"jian3", "hang2"}},
        {"行号", {"hang2", "hao4"}}, {"各地", {"ge4", "di4"}},
        {"借还款", {"jie4", "huan2", "kuan3"}},
        {"时间为", {"shi2", "jian1", "wei2"}},
        {"为准", {"wei2", "zhun3"}}, {"色差", {"se4", "cha1"}},
        {"嗲", {"dia3"}}, {"呗", {"bei5"}}, {"不", {"bu4"}},
        {"咯", {"zuo5"}}, {"咧", {"lei5"}},
        {"掺和", {"chan1", "huo5"}}, {"地", {"de5"}},
    };
    return kMap;
}

const std::unordered_set<std::string>& mustErhuaWords() {
    static const std::unordered_set<std::string> kWords = {
        "小院儿", "胡同儿", "范儿", "老汉儿", "撒欢儿",
        "寻老礼儿", "妥妥儿", "媳妇儿",
    };
    return kWords;
}

const std::unordered_set<std::string>& notErhuaWords() {
    static const std::unordered_set<std::string> kWords = {
        "虐儿", "为儿", "护儿", "瞒儿", "救儿", "替儿", "有儿",
        "一儿", "我儿", "俺儿", "妻儿", "拐儿", "聋儿", "乞儿",
        "患儿", "幼儿", "孤儿", "婴儿", "婴幼儿", "连体儿",
        "脑瘫儿", "流浪儿", "体弱儿", "混血儿", "蜜雪儿",
        "舫儿", "祖儿", "美儿", "应采儿", "可儿", "侄儿", "孙儿",
        "侄孙儿", "女儿", "男儿", "红孩儿", "花儿", "虫儿",
        "马儿", "鸟儿", "猪儿", "猫儿", "狗儿", "少儿",
    };
    return kWords;
}

const std::unordered_set<std::string>& englishPronouns() {
    static const std::unordered_set<std::string> kWords = {
        "i", "we", "you", "he", "she", "it", "they",
    };
    return kWords;
}

const StringMap& englishAbbreviationLexiconKeys() {
    // spaCy keeps these title abbreviations as one token, so their period is
    // lexical rather than a sentence boundary in official Misaki. Values are
    // the keys used by Misaki's case-sensitive lexicon.
    static const StringMap kWords = {
        {"dr", "Dr."}, {"jr", "Jr."}, {"mr", "Mr."},
        {"mrs", "Mrs."}, {"ms", "Ms."}, {"prof", "prof"},
        {"sr", "Sr."},
    };
    return kWords;
}

const std::unordered_set<std::string>& englishVowels() {
    static const std::unordered_set<std::string> kVowels = {
        "A", "I", "O", "Q", "W", "Y", "a", "i", "u", "æ", "ɑ",
        "ɒ", "ɔ", "ə", "ɛ", "ɜ", "ɪ", "ʊ", "ʌ", "ᵻ",
    };
    return kVowels;
}

const std::unordered_set<std::string>& englishConsonants() {
    static const std::unordered_set<std::string> kConsonants = {
        "b", "d", "f", "h", "j", "k", "l", "m", "n", "p", "s", "t",
        "v", "w", "z", "ð", "ŋ", "ɡ", "ɹ", "ɾ", "ʃ", "ʒ", "ʤ",
        "ʧ", "θ",
    };
    return kConsonants;
}

const StringMap& englishSymbolWords() {
    static const StringMap kSymbols = {
        {"%", "percent"}, {"&", "and"}, {"+", "plus"}, {"@", "at"},
        {".", "dot"}, {"/", "slash"},
    };
    return kSymbols;
}

}  // namespace tts::kokoro_frontend_data
