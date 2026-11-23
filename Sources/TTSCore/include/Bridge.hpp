// Helpers for Swift. The Command Line Tools ship without Swift's CxxStdlib overlay, so Swift can't
// convert std::string <-> String or iterate std::vector directly; these tiny helpers fill the gap.
#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "TTSTypes.hpp"

namespace ttsdc::bridge {

inline std::string make(const char* s) { return s ? std::string(s) : std::string(); }
inline size_t length(const std::string& s) { return s.size(); }
/// Copies up to `cap` bytes into `out` and returns the count copied.
inline size_t copyOut(const std::string& s, char* out, size_t cap) {
    size_t n = s.size() < cap ? s.size() : cap;
    if (n) std::memcpy(out, s.data(), n);
    return n;
}

inline size_t count(const StringList& v) { return v.size(); }
inline std::string at(const StringList& v, size_t i) { return v[i]; }
inline void append(StringList& v, const std::string& s) { v.push_back(s); }

inline size_t count(const SegmentList& v) { return v.size(); }
inline Segment at(const SegmentList& v, size_t i) { return v[i]; }
inline void append(SegmentList& v, const Segment& s) { v.push_back(s); }

inline size_t count(const FloatList& v) { return v.size(); }
inline const float* data(const FloatList& v) { return v.data(); }

} // namespace ttsdc::bridge
