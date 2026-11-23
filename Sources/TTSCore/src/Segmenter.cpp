#include "Segmenter.hpp"

#include <algorithm>
#include <cmath>

#include "DSP.hpp"
#include "Flags.hpp"

namespace ttsdc {

namespace {

void addFlag(std::vector<std::string>& flags, const char* f) {
    if (std::find(flags.begin(), flags.end(), f) == flags.end()) flags.emplace_back(f);
}
void removeFlag(std::vector<std::string>& flags, const char* f) {
    flags.erase(std::remove(flags.begin(), flags.end(), std::string(f)), flags.end());
}

double percentile(std::vector<float> v, double p) {
    if (v.empty()) return 0;
    size_t k = static_cast<size_t>(std::clamp(p, 0.0, 1.0) * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

/// Strip leading opening and trailing closing quotes/brackets.
std::string stripQuotes(const std::string& w) {
    size_t a = 0, b = w.size();
    auto isQ = [](unsigned char c) { return c == '"' || c == '\'' || c == '(' || c == ')' || c == '[' || c == ']'; };
    while (a < b && isQ(w[a])) ++a;
    while (b > a && isQ(w[b - 1])) --b;
    // UTF-8 curly quotes
    while (b >= a + 3 && (unsigned char)w[b - 3] == 0xE2 && (unsigned char)w[b - 2] == 0x80 &&
           ((unsigned char)w[b - 1] == 0x9D || (unsigned char)w[b - 1] == 0x99))
        b -= 3;
    while (a + 3 <= b && (unsigned char)w[a] == 0xE2 && (unsigned char)w[a + 1] == 0x80 &&
           ((unsigned char)w[a + 2] == 0x9C || (unsigned char)w[a + 2] == 0x98))
        a += 3;
    return w.substr(a, b - a);
}

bool startsUpper(const std::string& w) {
    std::string s = stripQuotes(w);
    return !s.empty() && s[0] >= 'A' && s[0] <= 'Z';
}

struct Gap {
    double start, end;
};

std::vector<Gap> gapsFrom(const FileAnalysis& a) {
    std::vector<Gap> g;
    double prev = 0;
    for (auto& s : a.speech) {
        if (s.start > prev) g.push_back({prev, s.start});
        prev = std::max(prev, s.end);
    }
    if (a.duration > prev) g.push_back({prev, a.duration});
    return g;
}

/// Quietest frame in [a, b]; among frames within 1 dB of the minimum, the one nearest the middle.
double quietestPoint(const FileAnalysis& an, double a, double b) {
    size_t fa = static_cast<size_t>(std::max(0.0, a / an.frameSeconds));
    size_t fb = std::min(an.frameDb.size(), static_cast<size_t>(std::max(0.0, b / an.frameSeconds)) + 1);
    if (fa >= fb) return std::clamp((a + b) / 2, 0.0, an.duration);
    float mn = 1e9f;
    for (size_t f = fa; f < fb; ++f) mn = std::min(mn, an.frameDb[f]);
    double mid = (a + b) / 2, best = mid, bestDist = 1e9;
    for (size_t f = fa; f < fb; ++f) {
        if (an.frameDb[f] > mn + 1.0f) continue;
        double t = (f + 0.5) * an.frameSeconds;
        if (std::abs(t - mid) < bestDist) bestDist = std::abs(t - mid), best = t;
    }
    return std::clamp(best, 0.0, an.duration);
}

} // namespace

float FileAnalysis::dbAt(double t) const {
    if (frameDb.empty()) return -120;
    long f = static_cast<long>(t / frameSeconds);
    f = std::clamp(f, 0L, static_cast<long>(frameDb.size()) - 1);
    return frameDb[static_cast<size_t>(f)];
}

double FileAnalysis::speechSecondsIn(double a, double b) const {
    double s = 0;
    for (auto& iv : speech) {
        double lo = std::max(a, iv.start), hi = std::min(b, iv.end);
        if (hi > lo) s += hi - lo;
    }
    return s;
}

FileAnalysis analyzeAudio(const float* pcm, size_t n, const std::vector<Interval>& speech) {
    FileAnalysis a;
    a.duration = n / 16000.0;
    a.frameDb = frameRmsDb(pcm, n, 16000.0, a.frameSeconds);
    a.speech = speech;
    a.noiseFloorDb = static_cast<float>(percentile(a.frameDb, 0.05));
    a.speechLevelDb = static_cast<float>(percentile(a.frameDb, 0.95));
    // "strict" threshold from the spike: floor + 25 % of the floor-to-speech range (≈ -64 dBFS on studio audio)
    a.silenceThresholdDb = a.noiseFloorDb + 0.25f * (a.speechLevelDb - a.noiseFloorDb);
    return a;
}

std::string joinWords(const std::vector<Word>& words, size_t first, size_t last) {
    std::string s;
    for (size_t k = first; k <= last && k < words.size(); ++k) s += (s.empty() ? "" : " ") + words[k].text;
    return s;
}

std::vector<SentenceSpan> splitSentences(const std::vector<Word>& words, const PronunciationDictionary& dict) {
    std::vector<SentenceSpan> out;
    if (words.empty()) return out;
    size_t first = 0;
    for (size_t k = 0; k < words.size(); ++k) {
        std::string w = stripQuotes(words[k].text);
        bool isEnd = false;
        if (!w.empty()) {
            char last = w.back();
            bool ellipsis = w.size() >= 3 && w.compare(w.size() - 3, 3, "...") == 0;
            bool utf8Ellipsis = w.size() >= 3 && (unsigned char)w[w.size() - 3] == 0xE2 &&
                                (unsigned char)w[w.size() - 2] == 0x80 && (unsigned char)w[w.size() - 1] == 0xA6;
            bool nextUpper = k + 1 < words.size() && startsUpper(words[k + 1].text);
            if (last == '?' || last == '!') isEnd = true;
            else if (ellipsis || utf8Ellipsis) isEnd = nextUpper || k + 1 == words.size();
            else if (last == '.') {
                // strip leading punctuation like "(" already done; look at the word itself
                std::string spoken;
                bool initial = w.size() == 2 && w[0] >= 'A' && w[0] <= 'Z'; // "J." in "J. K. Rowling"
                if (dict.isTitle(w) || initial) isEnd = false;
                else if (dict.isAbbreviation(w)) isEnd = nextUpper; // "in the U.S. The next..." splits
                else isEnd = true;
            }
        }
        if (isEnd || k + 1 == words.size()) {
            out.push_back({first, k});
            first = k + 1;
        }
    }
    return out;
}

void recheckSegment(Segment& seg, const FileAnalysis& an, const CutOptions& opts) {
    double dur = seg.end - seg.start;
    removeFlag(seg.flags, flag::tooShort);
    removeFlag(seg.flags, flag::tooLong);
    removeFlag(seg.flags, flag::nonSilentCut);
    if (dur < opts.minSeconds) addFlag(seg.flags, flag::tooShort);
    if (dur > opts.maxSeconds) addFlag(seg.flags, flag::tooLong);
    // level of the first/last 10 ms frame inside the segment
    seg.startCutDb = an.dbAt(seg.start + an.frameSeconds * 0.5);
    seg.endCutDb = an.dbAt(std::max(seg.start, seg.end - an.frameSeconds * 0.5));
    if (seg.startCutDb > an.silenceThresholdDb || seg.endCutDb > an.silenceThresholdDb) addFlag(seg.flags, flag::nonSilentCut);
    seg.speechSeconds = an.speechSecondsIn(seg.start, seg.end);
    // word count from the raw text
    int wc = 0;
    bool inWord = false;
    for (char c : seg.raw) {
        bool sp = c == ' ' || c == '\t' || c == '\n';
        if (!sp && !inWord) ++wc;
        inWord = !sp;
    }
    seg.wordCount = wc;
    if (an.medianSecondsPerWord > 0 && wc > 0) {
        double expected = opts.slowSpeechFactor * an.medianSecondsPerWord * wc + 0.6;
        if (seg.speechSeconds > expected) addFlag(seg.flags, flag::slowSpeech);
        else removeFlag(seg.flags, flag::slowSpeech);
    }
}

std::vector<Segment> cutSegments(const std::vector<Word>& words, FileAnalysis& an, const CutOptions& optsIn,
                                 const PronunciationDictionary& dict) {
    CutOptions opts = optsIn;
    opts.paddingSeconds = std::clamp(opts.paddingSeconds, 0.10, 0.20);
    std::vector<Segment> segs;
    auto spans = splitSentences(words, dict);
    if (spans.empty()) return segs;
    auto gaps = gapsFrom(an);

    // 1. one cut point between each pair of consecutive sentences
    std::vector<double> cuts;            // cuts[i] separates sentence i and i+1
    std::vector<bool> cutFlagged;        // no VAD gap found near the sentence end
    for (size_t i = 0; i + 1 < spans.size(); ++i) {
        double tEnd = words[spans[i].last].end;            // DTW time of the trailing punctuation token
        double tNext = words[spans[i + 1].first].start;
        const Gap* best = nullptr;
        double bestDist = 1e9;
        for (auto& g : gaps) {
            double d = (tEnd >= g.start && tEnd <= g.end) ? 0 : std::min(std::abs(tEnd - g.start), std::abs(tEnd - g.end));
            if (d < bestDist) bestDist = d, best = &g;
        }
        double cut;
        bool flagged = false;
        if (best && bestDist <= opts.maxGapSearchSeconds) {
            cut = quietestPoint(an, best->start, best->end);
        } else {
            cut = quietestPoint(an, tEnd - 0.3, std::max(tEnd + 0.3, std::min(tNext, tEnd + 0.6)));
            flagged = true;
        }
        // keep cuts monotonic
        if (!cuts.empty() && cut <= cuts.back()) {
            cut = std::min(an.duration, cuts.back() + an.frameSeconds);
            flagged = true;
        }
        cuts.push_back(cut);
        cutFlagged.push_back(flagged);
    }

    // 2. trim each sentence's span to its speech and add padding
    for (size_t i = 0; i < spans.size(); ++i) {
        double lo = i == 0 ? 0.0 : cuts[i - 1];
        double hi = i + 1 == spans.size() ? an.duration : cuts[i];
        Segment s;
        s.raw = joinWords(words, spans[i].first, spans[i].last);

        // speech extent: VAD intervals overlapping [lo, hi]
        double sp0 = 1e18, sp1 = -1;
        for (auto& iv : an.speech) {
            double a = std::max(lo, iv.start), b = std::min(hi, iv.end);
            if (b > a) sp0 = std::min(sp0, a), sp1 = std::max(sp1, b);
        }
        if (sp1 < 0) { // no VAD speech: fall back to word times
            sp0 = std::clamp(words[spans[i].first].start, lo, hi);
            sp1 = std::clamp(words[spans[i].last].end, lo, hi);
        }
        // extend over energy above the silence threshold (VAD boundaries are ±30–60 ms)
        while (sp1 + an.frameSeconds < hi && an.dbAt(sp1 + an.frameSeconds * 0.5) > an.silenceThresholdDb) sp1 += an.frameSeconds;
        while (sp0 - an.frameSeconds > lo && an.dbAt(sp0 - an.frameSeconds * 0.5) > an.silenceThresholdDb) sp0 -= an.frameSeconds;
        s.speechStart = sp0;
        s.speechEnd = sp1;
        s.start = std::max(lo, sp0 - opts.paddingSeconds);
        s.end = std::min(hi, sp1 + opts.paddingSeconds);
        if (s.end <= s.start) s.end = std::min(an.duration, s.start + 0.05);

        recheckSegment(s, an, opts);
        if ((i > 0 && cutFlagged[i - 1]) || (i + 1 < spans.size() && cutFlagged[i])) addFlag(s.flags, flag::noGap);

        // dropped-word detector: lots of speech between two consecutive word timestamps
        for (size_t k = spans[i].first; k < spans[i].last; ++k) {
            double a = words[k].end, b = words[k + 1].start;
            if (b - a > opts.droppedGapSeconds && an.speechSecondsIn(a, b) > opts.droppedGapSeconds) {
                addFlag(s.flags, flag::slowSpeech);
                break;
            }
        }
        segs.push_back(std::move(s));
    }

    // 3. file-level speech rate, then the slow-speech check
    std::vector<float> rates;
    for (auto& s : segs)
        if (s.wordCount >= 4) rates.push_back(static_cast<float>(s.speechSeconds / s.wordCount));
    if (rates.size() >= 3) {
        an.medianSecondsPerWord = percentile(rates, 0.5);
        for (auto& s : segs) {
            bool hadGapFlag = std::find(s.flags.begin(), s.flags.end(), flag::slowSpeech) != s.flags.end();
            recheckSegment(s, an, opts);
            if (hadGapFlag) addFlag(s.flags, flag::slowSpeech);
        }
    }
    return segs;
}

} // namespace ttsdc
