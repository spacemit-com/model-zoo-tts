/* Copyright (C) 2025 SpacemiT Co., Ltd.
 * SPDX-License-Identifier: Apache-2.0 */

#ifndef KOKORO_FRONTEND_DATA_HPP
#define KOKORO_FRONTEND_DATA_HPP

#include <cstdint>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tts::kokoro_frontend_data {

using StringMap = std::unordered_map<std::string, std::string>;

// Immutable language data shared by the Kokoro frontends. Keep algorithms and
// model/runtime compatibility rules in their respective backend classes.
const std::vector<std::pair<std::string, int64_t>>& kokoroVocabulary();
const StringMap& pinyinInitialToIpa();
const StringMap& pinyinFinalToIpa();
const std::vector<std::string>& orderedPinyinInitials();
const StringMap& zhBopomofoMap();
const std::unordered_map<std::string, std::vector<std::string>>&
    zhPhrasePinyin();
const std::unordered_set<std::string>& mustErhuaWords();
const std::unordered_set<std::string>& notErhuaWords();
const std::unordered_set<std::string>& englishPronouns();
const StringMap& englishAbbreviationLexiconKeys();
const std::unordered_set<std::string>& englishVowels();
const std::unordered_set<std::string>& englishConsonants();
const StringMap& englishSymbolWords();

}  // namespace tts::kokoro_frontend_data

#endif  // KOKORO_FRONTEND_DATA_HPP
