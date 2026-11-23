// Plain data types shared by the core and the Swift layer.
// Everything here is a copyable struct so Swift's C++ interop can import it as a value type.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ttsdc {

/// One recognised word. Times are seconds from the start of the file.
struct Word {
    std::string text;   ///< word as transcribed, including attached punctuation ("world.")
    double start = 0;   ///< DTW time of the word's first token
    double end = 0;     ///< DTW time of the word's last token (for "world." this is the "." token)
    float prob = 0;     ///< mean token probability
};

/// Speech interval from Silero VAD, seconds.
struct Interval {
    double start = 0;
    double end = 0;
};

/// One dataset segment (an utterance), produced by the cutter.
struct Segment {
    double start = 0;        ///< cut start, seconds (after trimming + padding)
    double end = 0;          ///< cut end, seconds
    double speechStart = 0;  ///< detected speech onset inside the segment
    double speechEnd = 0;    ///< detected speech offset inside the segment
    double speechSeconds = 0;///< total VAD speech time inside the segment
    int wordCount = 0;
    std::string raw;         ///< raw ASR sentence text
    std::string normalized;  ///< normalized text
    std::vector<std::string> flags; ///< machine-readable flag codes, see flags.hpp
    float startCutDb = -120; ///< RMS level (dBFS) of the 10 ms frame at the start cut point
    float endCutDb = -120;   ///< RMS level (dBFS) of the 10 ms frame at the end cut point
};

/// Tunable cutting parameters.
struct CutOptions {
    double paddingSeconds = 0.15;     ///< silence kept before/after speech, clamped to 0.10–0.20 s
    double fadeSeconds = 0.010;       ///< fade-in/out applied at export
    double minSeconds = 1.0;          ///< segments shorter than this are flagged
    double maxSeconds = 15.0;         ///< segments longer than this are flagged
    double maxGapSearchSeconds = 1.5; ///< how far from the ASR sentence end a VAD gap may be
    double slowSpeechFactor = 1.35;   ///< flag if speech time > factor x the file's median time per word (+0.6 s)
    double droppedGapSeconds = 1.5;   ///< flag if this much speech lies between two consecutive word times
};

/// Options for text normalization.
struct NormalizeOptions {
    bool lowercaseAll = false;
};

/// Result of normalizing one piece of text.
struct NormalizeResult {
    std::string text;
    std::vector<std::string> flags;
};

/// Named vector types so Swift can construct them.
using StringList = std::vector<std::string>;
using SegmentList = std::vector<Segment>;
using FloatList = std::vector<float>;

/// Export settings.
struct ExportOptions {
    int sampleRate = 22050;      ///< 16000, 22050, 24000, 44100 or 48000
    bool threeColumns = false;   ///< id|raw|normalized instead of id|normalized
    bool dropFlagged = false;    ///< skip segments that carry any flag
    bool lowercaseAll = false;   ///< recorded in dataset_info.json (text is already lowercased by the caller)
    double minSeconds = 1.0;
    double maxSeconds = 15.0;
    double fadeSeconds = 0.010;
    std::string modelName;       ///< recorded in dataset_info.json
    std::string modelId;         ///< short ASR model id, recorded in dataset_info.json
    std::string modelLicense;    ///< ASR weights licence, recorded in dataset_info.json
    std::string appVersion;
};

/// Summary returned by an export.
struct ExportSummary {
    bool ok = false;
    std::string error;
    int written = 0;
    int skipped = 0;
    double totalSeconds = 0;
};

} // namespace ttsdc
