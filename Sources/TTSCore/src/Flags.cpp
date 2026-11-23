#include "Flags.hpp"

#include <map>

namespace ttsdc::flag {

std::string describe(const std::string& code) {
    static const std::map<std::string, std::string> d = {
        {tooShort, "Shorter than the minimum length"},
        {tooLong, "Longer than the maximum length"},
        {nonSilentCut, "A cut point is not in silence"},
        {slowSpeech, "More speech than the words explain: the transcript may be missing words"},
        {noGap, "No pause found near the sentence end"},
        {ambiguousNumber, "Ambiguous number (date, fraction or year?): check the reading"},
        {spelledAcronym, "Unknown all-caps word spelled out letter by letter"},
        {letterA, "Standalone letter \"a\" may be read as the article"},
        {digitsLeft, "Digits remain in the normalized text"},
        {symbol, "Unusual symbol removed or replaced"},
    };
    auto it = d.find(code);
    return it == d.end() ? code : it->second;
}

} // namespace ttsdc::flag
