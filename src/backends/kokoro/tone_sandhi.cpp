/* Copyright (C) 2025 SpacemiT Co., Ltd.
    * SPDX-License-Identifier: Apache-2.0 */

#include "backends/kokoro/tone_sandhi.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace tts {

namespace {

const char* BU = "不";
const char* YI = "一";

// Split a UTF-8 string into a vector of single-character UTF-8 strings.
std::vector<std::string> utf8Chars(const std::string& s) {
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

// Number of UTF-8 characters (Chinese char = 1).
size_t charLen(const std::string& s) { return utf8Chars(s).size(); }

// Set the tone digit (last byte) of a TONE3 final, e.g. ("ao3","5")->"ao5".
std::string setTone(const std::string& final, char tone) {
    if (final.empty()) return final;
    std::string r = final;
    r.back() = tone;
    return r;
}

// Last tone digit of a final, or '\0' if empty.
char toneOf(const std::string& final) { return final.empty() ? '\0' : final.back(); }

// Is every byte of s an ASCII digit? (used for "一" in number sequences)
bool isNumeric(const std::string& ch) {
    if (ch.empty()) return false;
    for (char c : ch) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool inX_ENG(const std::string& pos) { return pos == "x" || pos == "eng"; }

}  // namespace

bool ToneSandhi::allToneThree(const std::vector<std::string>& finals) const {
    for (const auto& f : finals) {
        if (toneOf(f) != '3') return false;
    }
    return !finals.empty();
}

bool ToneSandhi::isReduplication(const std::string& word) const {
    auto ch = utf8Chars(word);
    return ch.size() == 2 && ch[0] == ch[1];
}

std::vector<std::string> ToneSandhi::splitWord(const std::string& word) const {
    if (split_word_fn_) {
        auto r = split_word_fn_(word);
        if (r.size() == 2) return r;
    }
    // Fallback: split after first character.
    auto ch = utf8Chars(word);
    if (ch.size() <= 1) return {word};
    return {ch[0], word.substr(ch[0].size())};
}

// misaki: _bu_sandhi
std::vector<std::string> ToneSandhi::buSandhi(const std::string& word, std::vector<std::string> finals) const {
    auto ch = utf8Chars(word);
    // e.g. 看不懂 -> 不 becomes neutral
    if (ch.size() == 3 && ch[1] == BU && finals.size() == 3) {
        finals[1] = setTone(finals[1], '5');
    } else {
        for (size_t i = 0; i < ch.size(); ++i) {
            // "不" before tone4 should be bu2, e.g. 不怕
            if (ch[i] == BU && i + 1 < ch.size() && i + 1 < finals.size() && toneOf(finals[i + 1]) == '4') {
                finals[i] = setTone(finals[i], '2');
            }
        }
    }
    return finals;
}

// misaki: _yi_sandhi
std::vector<std::string> ToneSandhi::yiSandhi(const std::string& word, std::vector<std::string> finals) const {
    auto ch = utf8Chars(word);
    // "一" in number sequences (e.g. 一零零): every non-一 char is numeric
    bool has_yi = false, all_num = true;
    for (const auto& c : ch) {
        if (c == YI) {
            has_yi = true;
        } else if (!isNumeric(c)) {
            all_num = false;
        }
    }
    if (has_yi && all_num) return finals;

    // "一" between reduplication words -> yi5, e.g. 看一看
    if (ch.size() == 3 && ch[1] == YI && ch[0] == ch[2] && finals.size() == 3) {
        finals[1] = setTone(finals[1], '5');
        return finals;
    }
    // ordinal "第一" -> yi1
    if (ch.size() >= 2 && ch[0] == "第" && ch[1] == YI && finals.size() >= 2) {
        finals[1] = setTone(finals[1], '1');
        return finals;
    }
    const std::string punc = "、：，；。？！“”‘’':,;.?!";
    for (size_t i = 0; i < ch.size(); ++i) {
        if (ch[i] == YI && i + 1 < ch.size() && i < finals.size() && i + 1 < finals.size()) {
            char nt = toneOf(finals[i + 1]);
            // "一" before tone4/5 -> yi2, e.g. 一段
            if (nt == '4' || nt == '5') {
                finals[i] = setTone(finals[i], '2');
            } else {
                // "一" before non-tone4 -> yi4, unless next char is punctuation
                if (punc.find(ch[i + 1]) == std::string::npos) {
                    finals[i] = setTone(finals[i], '4');
                }
            }
        }
    }
    return finals;
}

namespace {

// Last <= n UTF-8 chars of a word, joined (mirrors Python word[-n:]).
std::string lastChars(const std::vector<std::string>& ch, size_t n) {
    std::string r;
    size_t start = ch.size() > n ? ch.size() - n : 0;
    for (size_t i = start; i < ch.size(); ++i) r += ch[i];
    return r;
}

// Contains check for a UTF-8 char inside a UTF-8 set string.
bool inSet(const std::string& set, const std::string& ch) {
    if (ch.empty()) return false;
    return set.find(ch) != std::string::npos;
}

// Python str.isnumeric() covers ASCII, full-width digits and CJK numerals.
bool isNumericChar(const std::string& ch) {
    if (isNumeric(ch)) return true;
    static const std::string cjk = "〇一二三四五六七八九十百千万亿零两";
    return inSet(cjk, ch);
}

}  // namespace

// misaki: _neural_sandhi
std::vector<std::string> ToneSandhi::neuralSandhi(
    const std::string& word, const std::string& pos, std::vector<std::string> finals) const {
    const auto& neural = mustNeuralToneWords();
    const auto& not_neural = mustNotNeuralToneWords();
    if (not_neural.count(word)) return finals;

    auto ch = utf8Chars(word);
    if (finals.size() != ch.size()) return finals;  // guard misalignment

    char pos0 = pos.empty() ? '\0' : pos[0];
    // reduplication for n./v./a. e.g. 奶奶, 试试, 旺旺
    for (size_t j = 0; j < ch.size(); ++j) {
        if (j >= 1 && ch[j] == ch[j - 1] && (pos0 == 'n' || pos0 == 'v' || pos0 == 'a')) {
            finals[j] = setTone(finals[j], '5');
        }
    }

    // index of "个"
    int ge_idx = -1;
    for (size_t i = 0; i < ch.size(); ++i) {
        if (ch[i] == "个") {
            ge_idx = static_cast<int>(i);
            break;
        }
    }
    const std::string& last = ch.back();

    if (inSet("吧呢啊呐噻嘛吖嗨哦哒滴哩哟喽啰耶喔诶", last)) {
        finals.back() = setTone(finals.back(), '5');
    } else if (inSet("的地得", last)) {
        finals.back() = setTone(finals.back(), '5');
    } else if (ch.size() == 1 && inSet("了着过", word) && (pos == "ul" || pos == "uz" || pos == "ug")) {
        finals.back() = setTone(finals.back(), '5');
    } else if (ch.size() > 1 && inSet("们子", last) && (pos == "r" || pos == "n")) {
        finals.back() = setTone(finals.back(), '5');
    } else if (ch.size() > 1 && inSet("上下", last) && (pos == "s" || pos == "l" || pos == "f")) {
        finals.back() = setTone(finals.back(), '5');
    } else if (ch.size() > 1 && inSet("来去", last) && inSet("上下进出回过起开", ch[ch.size() - 2])) {
        finals.back() = setTone(finals.back(), '5');
    } else if ((ge_idx >= 1 && (isNumericChar(ch[ge_idx - 1]) || inSet("几有两半多各整每做是", ch[ge_idx - 1]))) ||
        word == "个") {
        if (ge_idx >= 0) finals[ge_idx] = setTone(finals[ge_idx], '5');
    } else {
        if (neural.count(word) || neural.count(lastChars(ch, 2))) {
            finals.back() = setTone(finals.back(), '5');
        }
    }

    // per-subword conventional neutral tone
    auto word_list = splitWord(word);
    if (word_list.size() == 2) {
        size_t n0 = charLen(word_list[0]);
        if (n0 <= finals.size()) {
            std::vector<std::string> f0(finals.begin(), finals.begin() + n0);
            std::vector<std::string> f1(finals.begin() + n0, finals.end());
            auto apply = [&](std::vector<std::string>& fl, const std::string& w) {
                auto wc = utf8Chars(w);
                if (!fl.empty() && (neural.count(w) || neural.count(lastChars(wc, 2)))) {
                    fl.back() = setTone(fl.back(), '5');
                }
            };
            apply(f0, word_list[0]);
            apply(f1, word_list[1]);
            finals.clear();
            finals.insert(finals.end(), f0.begin(), f0.end());
            finals.insert(finals.end(), f1.begin(), f1.end());
        }
    }
    return finals;
}

// misaki: _three_sandhi
std::vector<std::string> ToneSandhi::threeSandhi(const std::string& word, std::vector<std::string> finals) const {
    size_t n = charLen(word);

    if (n == 2 && allToneThree(finals)) {
        finals[0] = setTone(finals[0], '2');
    } else if (n == 3) {
        auto word_list = splitWord(word);
        size_t n0 = word_list.empty() ? 0 : charLen(word_list[0]);
        if (allToneThree(finals)) {
            // disyllabic + monosyllabic, e.g. 蒙古/包
            if (n0 == 2 && finals.size() >= 2) {
                finals[0] = setTone(finals[0], '2');
                finals[1] = setTone(finals[1], '2');
            } else if (n0 == 1 && finals.size() >= 2) {
                // monosyllabic + disyllabic, e.g. 纸/老虎
                finals[1] = setTone(finals[1], '2');
            }
        } else if (n0 >= 1 && n0 < finals.size()) {
            std::vector<std::string> f0(finals.begin(), finals.begin() + n0);
            std::vector<std::string> f1(finals.begin() + n0, finals.end());
            std::vector<std::vector<std::string>*> subs = {&f0, &f1};
            for (size_t i = 0; i < subs.size(); ++i) {
                auto& sub = *subs[i];
                // e.g. 所有/人
                if (allToneThree(sub) && sub.size() == 2) {
                    sub[0] = setTone(sub[0], '2');
                } else if (i == 1 && !allToneThree(sub) && !sub.empty() && toneOf(sub[0]) == '3' && !f0.empty() &&
                    toneOf(f0.back()) == '3') {
                    // e.g. 好/喜欢
                    f0.back() = setTone(f0.back(), '2');
                }
            }
            finals.clear();
            finals.insert(finals.end(), f0.begin(), f0.end());
            finals.insert(finals.end(), f1.begin(), f1.end());
        }
    } else if (n == 4 && finals.size() == 4) {
        // split idiom into two 2-char words
        std::vector<std::string> f0(finals.begin(), finals.begin() + 2);
        std::vector<std::string> f1(finals.begin() + 2, finals.end());
        if (allToneThree(f0)) f0[0] = setTone(f0[0], '2');
        if (allToneThree(f1)) f1[0] = setTone(f1[0], '2');
        finals.clear();
        finals.insert(finals.end(), f0.begin(), f0.end());
        finals.insert(finals.end(), f1.begin(), f1.end());
    }
    return finals;
}

// misaki: modified_tone
std::vector<std::string> ToneSandhi::modifiedTone(
    const std::string& word, const std::string& pos, std::vector<std::string> finals) const {
    finals = buSandhi(word, std::move(finals));
    finals = yiSandhi(word, std::move(finals));
    finals = neuralSandhi(word, pos, std::move(finals));
    finals = threeSandhi(word, std::move(finals));
    return finals;
}

// misaki: _merge_bu
std::vector<ToneSandhi::Seg> ToneSandhi::mergeBu(const std::vector<Seg>& seg) const {
    std::vector<Seg> out;
    for (size_t i = 0; i < seg.size(); ++i) {
        std::string word = seg[i].first;
        const std::string& pos = seg[i].second;
        if (!inX_ENG(pos)) {
            std::string last_word = (i > 0) ? seg[i - 1].first : "";
            if (last_word == BU) word = last_word + word;
        }
        bool has_next = i + 1 < seg.size();
        const std::string next_pos = has_next ? seg[i + 1].second : std::string();
        if (word != BU || !has_next || inX_ENG(next_pos)) {
            out.emplace_back(word, pos);
        }
    }
    return out;
}

// misaki: _merge_yi
std::vector<ToneSandhi::Seg> ToneSandhi::mergeYi(const std::vector<Seg>& seg_in) const {
    // function 1: "听","一","听" -> "听一听"
    std::vector<Seg> seg;
    bool skip_next = false;
    for (size_t i = 0; i < seg_in.size(); ++i) {
        if (skip_next) {
            skip_next = false;
            continue;
        }
        const std::string& word = seg_in[i].first;
        const std::string& pos = seg_in[i].second;
        if (i >= 1 && word == YI && i + 1 < seg_in.size() && seg_in[i - 1].first == seg_in[i + 1].first &&
            seg_in[i - 1].second == "v" && !inX_ENG(seg_in[i + 1].second) && !seg.empty()) {
            seg.back().first = seg.back().first + YI + seg_in[i + 1].first;
            skip_next = true;
        } else {
            seg.emplace_back(word, pos);
        }
    }
    // function 2: merge single "一" with the word behind it
    std::vector<Seg> out;
    for (size_t i = 0; i < seg.size(); ++i) {
        const std::string& word = seg[i].first;
        const std::string& pos = seg[i].second;
        if (!out.empty() && out.back().first == YI && !inX_ENG(pos)) {
            out.back().first = out.back().first + word;
        } else {
            out.emplace_back(word, pos);
        }
    }
    return out;
}

// misaki: _merge_reduplication
std::vector<ToneSandhi::Seg> ToneSandhi::mergeReduplication(const std::vector<Seg>& seg) const {
    std::vector<Seg> out;
    for (const auto& wp : seg) {
        if (!out.empty() && wp.first == out.back().first && !inX_ENG(wp.second)) {
            out.back().first += wp.first;
        } else {
            out.push_back(wp);
        }
    }
    return out;
}

// misaki: _merge_continuous_three_tones
std::vector<ToneSandhi::Seg> ToneSandhi::mergeContinuousThreeTones(const std::vector<Seg>& seg) const {
    std::vector<std::vector<std::string>> sub_finals;
    sub_finals.reserve(seg.size());
    for (const auto& wp : seg) {
        if (inX_ENG(wp.second)) {
            sub_finals.push_back({"0"});
            continue;
        }
        sub_finals.push_back(finals_fn_ ? finals_fn_(wp.first) : std::vector<std::string>{"0"});
    }
    std::vector<Seg> out;
    std::vector<char> merge_last(seg.size(), 0);
    for (size_t i = 0; i < seg.size(); ++i) {
        const auto& wp = seg[i];
        if (!inX_ENG(wp.second) && i >= 1 && allToneThree(sub_finals[i - 1]) && allToneThree(sub_finals[i]) &&
            !merge_last[i - 1]) {
            if (!isReduplication(seg[i - 1].first) && charLen(seg[i - 1].first) + charLen(seg[i].first) <= 3 &&
                !out.empty()) {
                out.back().first += seg[i].first;
                merge_last[i] = 1;
            } else {
                out.push_back(wp);
            }
        } else {
            out.push_back(wp);
        }
    }
    return out;
}

// misaki: _merge_continuous_three_tones_2
std::vector<ToneSandhi::Seg> ToneSandhi::mergeContinuousThreeTones2(const std::vector<Seg>& seg) const {
    std::vector<std::vector<std::string>> sub_finals;
    sub_finals.reserve(seg.size());
    for (const auto& wp : seg) {
        if (inX_ENG(wp.second)) {
            sub_finals.push_back({"0"});
            continue;
        }
        sub_finals.push_back(finals_fn_ ? finals_fn_(wp.first) : std::vector<std::string>{"0"});
    }
    std::vector<Seg> out;
    std::vector<char> merge_last(seg.size(), 0);
    for (size_t i = 0; i < seg.size(); ++i) {
        const auto& wp = seg[i];
        if (!inX_ENG(wp.second) && i >= 1 && !sub_finals[i - 1].empty() && !sub_finals[i].empty() &&
            toneOf(sub_finals[i - 1].back()) == '3' && toneOf(sub_finals[i].front()) == '3' && !merge_last[i - 1]) {
            if (!isReduplication(seg[i - 1].first) && charLen(seg[i - 1].first) + charLen(seg[i].first) <= 3 &&
                !out.empty()) {
                out.back().first += seg[i].first;
                merge_last[i] = 1;
            } else {
                out.push_back(wp);
            }
        } else {
            out.push_back(wp);
        }
    }
    return out;
}

// misaki: _merge_er
std::vector<ToneSandhi::Seg> ToneSandhi::mergeEr(const std::vector<Seg>& seg) const {
    std::vector<Seg> out;
    for (size_t i = 0; i < seg.size(); ++i) {
        if (i >= 1 && seg[i].first == "儿" && !out.empty() && !inX_ENG(out.back().second)) {
            out.back().first += seg[i].first;
        } else {
            out.push_back(seg[i]);
        }
    }
    return out;
}

// misaki: pre_merge_for_modify
std::vector<ToneSandhi::Seg> ToneSandhi::preMergeForModify(std::vector<Seg> seg) const {
    seg = mergeBu(seg);
    seg = mergeYi(seg);
    seg = mergeReduplication(seg);
    seg = mergeContinuousThreeTones(seg);
    seg = mergeContinuousThreeTones2(seg);
    seg = mergeEr(seg);
    return seg;
}

}  // namespace tts
