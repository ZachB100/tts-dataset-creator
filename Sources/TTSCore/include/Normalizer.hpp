// English text normalization for TTS datasets: numbers, abbreviations, acronyms.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "TTSTypes.hpp"

namespace ttsdc {

/// Number-to-words helpers (American English, hyphenated tens, no "and").
namespace numwords {
std::string cardinal(uint64_t n);                ///< 1234 -> "one thousand two hundred thirty-four"
std::string ordinal(uint64_t n);                 ///< 21 -> "twenty-first"
std::string year(int n);                         ///< 1999 -> "nineteen ninety-nine", 2008 -> "two thousand eight"
std::string decade(int n);                       ///< 1990 -> "nineteen nineties", 80 -> "eighties"
std::string digits(const std::string& s);        ///< "0420" -> "zero four two zero"
std::string decimal(const std::string& s);       ///< "3.14" -> "three point one four" (commas allowed)
std::string pluralOrdinalDenominator(uint64_t d, bool plural); ///< 4 -> "quarter(s)", 3 -> "third(s)"
} // namespace numwords

/// Editable pronunciation dictionary: written form -> spoken form.
/// Keys are matched case-sensitively first; keys written in lowercase also match capitalised words.
class PronunciationDictionary {
public:
    PronunciationDictionary();                       ///< loaded with the built-in defaults
    static std::string defaultJSON();                ///< the built-in default dictionary as JSON text
    bool loadJSON(const std::string& json, std::string& error); ///< replaces entries
    bool loadFile(const std::string& path, std::string& error);
    bool saveFile(const std::string& path, std::string& error) const;
    std::string toJSON() const;
    void set(const std::string& written, const std::string& spoken);
    void remove(const std::string& written);
    /// Lookup; returns true and sets `spoken` on a hit.
    bool lookup(const std::string& written, std::string& spoken) const;
    /// Titles (Dr., Mr., ...) never end a sentence even when followed by a capitalised word.
    bool isTitle(const std::string& written) const;
    /// True for keys ending in '.', i.e. abbreviations whose period isn't a sentence end.
    bool isAbbreviation(const std::string& written) const;
    size_t size() const { return entries_.size(); }
    std::vector<std::string> keys() const;

private:
    std::map<std::string, std::string> entries_;
    std::set<std::string> titles_;
};

class Normalizer {
public:
    explicit Normalizer(const PronunciationDictionary& dict) : dict_(dict) {}
    NormalizeResult normalize(const std::string& text, const NormalizeOptions& opts = {}) const;

private:
    const PronunciationDictionary& dict_;
};

/// Convenience used by Swift: normalize with a dictionary loaded from JSON text (empty = defaults).
NormalizeResult normalizeText(const std::string& text, bool lowercaseAll, const PronunciationDictionary& dict);

} // namespace ttsdc
