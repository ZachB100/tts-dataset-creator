// Flag codes attached to segments. Kept as strings so Swift can display/filter them easily.
#pragma once

#include <string>

namespace ttsdc::flag {

// cutting
inline constexpr const char* tooShort = "too_short";           // shorter than min seconds
inline constexpr const char* tooLong = "too_long";             // longer than max seconds
inline constexpr const char* nonSilentCut = "non_silent_cut";   // a cut point is not in silence
inline constexpr const char* slowSpeech = "possible_dropped_words"; // too much speech for the word count
inline constexpr const char* noGap = "no_pause_found";          // no VAD gap near the sentence end

// normalization
inline constexpr const char* ambiguousNumber = "ambiguous_number"; // e.g. "1/2", "2000"
inline constexpr const char* spelledAcronym = "spelled_acronym";   // unknown ALL-CAPS word spelled out
inline constexpr const char* letterA = "letter_a";                 // standalone letter "a" (reads as the article)
inline constexpr const char* digitsLeft = "digits_left";           // digits survived normalization
inline constexpr const char* symbol = "symbol";                    // unusual symbol replaced/kept

/// Human-readable description for a flag code.
std::string describe(const std::string& code);

} // namespace ttsdc::flag
