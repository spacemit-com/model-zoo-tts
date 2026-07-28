/* Copyright (C) 2025 SpacemiT Co., Ltd.
    * SPDX-License-Identifier: Apache-2.0 */

#ifndef TONE_SANDHI_HPP
#define TONE_SANDHI_HPP

#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tts {

// Mandarin tone sandhi, ported from misaki v1.1 (PaddleSpeech ToneSandhi).
//
// Operates on TONE3-style finals (e.g. "ao3", "i5"): the trailing character is
// the tone digit 1-5 (5 = neutral). A word is a UTF-8 string; finals has one
// entry per Chinese character in the word.
//
// Two external dependencies are injected as callbacks so this unit does not
// link cppjieba / cpp-pinyin directly:
//   - split_word_fn(word): jieba cut_for_search based sub-word split
//     (see misaki _split_word). Returns two sub-words.
//   - finals_fn(word): TONE3 finals for a word, neutral_tone_with_five=true
//     (used by the continuous-three-tone pre-merge passes).
class ToneSandhi {
    public:
    using SplitWordFn = std::function<std::vector<std::string>(const std::string&)>;
    using FinalsFn = std::function<std::vector<std::string>(const std::string&)>;

    ToneSandhi() = default;

    void setSplitWordFn(SplitWordFn fn) { split_word_fn_ = std::move(fn); }
    void setFinalsFn(FinalsFn fn) { finals_fn_ = std::move(fn); }

    // (word, pos) pair used by the segmentation-level pre-merge passes.
    using Seg = std::pair<std::string, std::string>;

    // misaki: modified_tone(word, pos, finals) -> finals
    // Applies _bu_sandhi, _yi_sandhi, _neural_sandhi, _three_sandhi in order.
    std::vector<std::string> modifiedTone(
        const std::string& word, const std::string& pos, std::vector<std::string> finals) const;

    // misaki: pre_merge_for_modify(seg) -> seg
    // Fixes jieba segmentation for sandhi (merges 不/一/reduplication/
    // continuous third tones/儿).
    std::vector<Seg> preMergeForModify(std::vector<Seg> seg) const;

    // Individual rules (public for unit testing).
    std::vector<std::string> buSandhi(const std::string& word, std::vector<std::string> finals) const;
    std::vector<std::string> yiSandhi(const std::string& word, std::vector<std::string> finals) const;
    std::vector<std::string> neuralSandhi(
        const std::string& word, const std::string& pos, std::vector<std::string> finals) const;
    std::vector<std::string> threeSandhi(const std::string& word, std::vector<std::string> finals) const;

    static const std::unordered_set<std::string>& mustNeuralToneWords();
    static const std::unordered_set<std::string>& mustNotNeuralToneWords();

    private:
    // pre-merge helpers
    std::vector<Seg> mergeBu(const std::vector<Seg>& seg) const;
    std::vector<Seg> mergeYi(const std::vector<Seg>& seg) const;
    std::vector<Seg> mergeReduplication(const std::vector<Seg>& seg) const;
    std::vector<Seg> mergeContinuousThreeTones(const std::vector<Seg>& seg) const;
    std::vector<Seg> mergeContinuousThreeTones2(const std::vector<Seg>& seg) const;
    std::vector<Seg> mergeEr(const std::vector<Seg>& seg) const;

    // rule helpers
    std::vector<std::string> splitWord(const std::string& word) const;
    bool allToneThree(const std::vector<std::string>& finals) const;
    bool isReduplication(const std::string& word) const;

    SplitWordFn split_word_fn_;
    FinalsFn finals_fn_;
};

}  // namespace tts

#endif  // TONE_SANDHI_HPP
