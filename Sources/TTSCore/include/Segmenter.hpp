// Sentence splitting and cut placement.
//
// Algorithm (validated in spike/FINDINGS.md):
//  1. split the word stream at sentence ends (. ? !), skipping abbreviation periods (Dr., U.S., ...);
//  2. take the sentence end time from the trailing punctuation token's DTW time;
//  3. snap it to the nearest Silero VAD gap (pause);
//  4. cut at the quietest 10 ms RMS frame inside that gap;
//  5. trim each segment to its speech plus 100–200 ms padding and flag anything suspicious.
#pragma once

#include <string>
#include <vector>

#include "Normalizer.hpp"
#include "TTSTypes.hpp"

namespace ttsdc {

/// Per-file analysis kept around so boundary edits can be re-checked cheaply.
struct FileAnalysis {
    double duration = 0;            ///< seconds
    double frameSeconds = 0.01;
    std::vector<float> frameDb;     ///< 10 ms RMS frames (dBFS) of the 16 kHz audio
    std::vector<Interval> speech;   ///< Silero VAD speech intervals
    float noiseFloorDb = -90;
    float speechLevelDb = -20;
    float silenceThresholdDb = -60; ///< frames above this count as non-silent
    double medianSecondsPerWord = 0; ///< file-level speech rate, set by cutSegments (0 = unknown)

    float dbAt(double t) const;
    double speechSecondsIn(double a, double b) const;
};

/// Build the analysis (RMS frames + levels) for a file.
FileAnalysis analyzeAudio(const float* pcm16k, size_t n, const std::vector<Interval>& speech);

/// Index ranges [first, last] of words forming each sentence.
struct SentenceSpan {
    size_t first = 0;
    size_t last = 0;
};
std::vector<SentenceSpan> splitSentences(const std::vector<Word>& words, const PronunciationDictionary& dict);

/// Cut a whole file into segments (raw text filled in, normalized left empty).
std::vector<Segment> cutSegments(const std::vector<Word>& words, FileAnalysis& analysis, const CutOptions& opts,
                                 const PronunciationDictionary& dict);

/// Re-compute cut-related flags and levels for a segment after its bounds or text changed.
/// Normalization flags already on the segment are kept.
void recheckSegment(Segment& seg, const FileAnalysis& analysis, const CutOptions& opts);

/// Join word texts into a sentence string.
std::string joinWords(const std::vector<Word>& words, size_t first, size_t last);

} // namespace ttsdc
