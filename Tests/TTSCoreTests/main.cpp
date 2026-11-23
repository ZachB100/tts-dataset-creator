// Core unit tests. Run: swift run TTSCoreTests   (exit code 0 = all passed)
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "ASR.hpp"
#include "DSP.hpp"
#include "Engine.hpp"
#include "Flags.hpp"
#include "Normalizer.hpp"
#include "Segmenter.hpp"

using namespace ttsdc;

static int gFailed = 0, gPassed = 0;
static std::string gCurrent;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) ++gPassed;                                                              \
        else {                                                                            \
            ++gFailed;                                                                    \
            fprintf(stderr, "  FAIL [%s] %s:%d: %s\n", gCurrent.c_str(), __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                                \
    do {                                                                                              \
        auto _a = (a);                                                                                \
        auto _b = (b);                                                                                \
        if (_a == _b) ++gPassed;                                                                      \
        else {                                                                                        \
            ++gFailed;                                                                                \
            fprintf(stderr, "  FAIL [%s] %s:%d\n    got:      \"%s\"\n    expected: \"%s\"\n", gCurrent.c_str(), \
                    __FILE__, __LINE__, toStr(_a).c_str(), toStr(_b).c_str());                        \
        }                                                                                             \
    } while (0)

static std::string toStr(const std::string& s) { return s; }
static std::string toStr(const char* s) { return s; }
static std::string toStr(size_t v) { return std::to_string(v); }
static std::string toStr(int v) { return std::to_string(v); }
static std::string toStr(bool v) { return v ? "true" : "false"; }

struct TestCase {
    const char* name;
    std::function<void()> fn;
};
static std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
#define TEST(name)                         \
    static void name();                    \
    static Reg reg_##name(#name, name);    \
    static void name()

static const PronunciationDictionary& dict() {
    static PronunciationDictionary d;
    return d;
}
static std::string N(const std::string& s) { return normalizeText(s, false, dict()).text; }
static bool hasFlag(const std::string& s, const char* f) {
    auto r = normalizeText(s, false, dict());
    for (auto& x : r.flags)
        if (x == f) return true;
    return false;
}
static bool noDigits(const std::string& s) {
    for (char c : s)
        if (c >= '0' && c <= '9') return false;
    return true;
}

// ------------------------------------------------------------------ number words

TEST(cardinals) {
    CHECK_EQ(numwords::cardinal(0), "zero");
    CHECK_EQ(numwords::cardinal(7), "seven");
    CHECK_EQ(numwords::cardinal(13), "thirteen");
    CHECK_EQ(numwords::cardinal(21), "twenty-one");
    CHECK_EQ(numwords::cardinal(100), "one hundred");
    CHECK_EQ(numwords::cardinal(115), "one hundred fifteen");
    CHECK_EQ(numwords::cardinal(1234), "one thousand two hundred thirty-four");
    CHECK_EQ(numwords::cardinal(1000000), "one million");
    CHECK_EQ(numwords::cardinal(2000005), "two million five");
    CHECK_EQ(numwords::cardinal(1001001), "one million one thousand one");
}

TEST(ordinals) {
    CHECK_EQ(numwords::ordinal(1), "first");
    CHECK_EQ(numwords::ordinal(2), "second");
    CHECK_EQ(numwords::ordinal(3), "third");
    CHECK_EQ(numwords::ordinal(5), "fifth");
    CHECK_EQ(numwords::ordinal(12), "twelfth");
    CHECK_EQ(numwords::ordinal(20), "twentieth");
    CHECK_EQ(numwords::ordinal(21), "twenty-first");
    CHECK_EQ(numwords::ordinal(100), "one hundredth");
    CHECK_EQ(numwords::ordinal(103), "one hundred third");
}

TEST(years) {
    CHECK_EQ(numwords::year(1999), "nineteen ninety-nine");
    CHECK_EQ(numwords::year(2008), "two thousand eight");
    CHECK_EQ(numwords::year(2000), "two thousand");
    CHECK_EQ(numwords::year(2010), "twenty ten");
    CHECK_EQ(numwords::year(2024), "twenty twenty-four");
    CHECK_EQ(numwords::year(1905), "nineteen oh five");
    CHECK_EQ(numwords::year(1900), "nineteen hundred");
    CHECK_EQ(numwords::year(1066), "ten sixty-six");
    CHECK_EQ(numwords::decade(1990), "nineteen nineties");
    CHECK_EQ(numwords::decade(80), "eighties");
}

TEST(digitsAndDecimals) {
    CHECK_EQ(numwords::digits("0420"), "zero four two zero");
    CHECK_EQ(numwords::decimal("3.14"), "three point one four");
    CHECK_EQ(numwords::decimal("0.5"), "zero point five");
    CHECK_EQ(numwords::decimal("1,234.5"), "one thousand two hundred thirty-four point five");
}

// ------------------------------------------------------------------ normalization in context

TEST(cardinalsInText) {
    CHECK_EQ(N("I have 3 cats."), "I have three cats.");
    CHECK_EQ(N("About 1,500 people came."), "About one thousand five hundred people came.");
    CHECK_EQ(N("It was -5 outside."), "It was minus five outside.");
    CHECK_EQ(N("There were 2000 people."), "There were two thousand people.");
    CHECK_EQ(N("We sold 1500 units."), "We sold one thousand five hundred units.");
}

TEST(yearsInText) {
    CHECK_EQ(N("It happened in 1999."), "It happened in nineteen ninety-nine.");
    CHECK_EQ(N("Since 2008, prices rose."), "Since two thousand eight, prices rose.");
    CHECK_EQ(N("Back in the 1990s it was cheap."), "Back in the nineteen nineties it was cheap.");
    CHECK_EQ(N("The '90s were fun."), "The nineties were fun.");
    CHECK_EQ(N("From 1990-1995 we grew."), "From nineteen ninety to nineteen ninety-five we grew.");
    CHECK(!hasFlag("It happened in 1999.", flag::ambiguousNumber));
}

TEST(ambiguousFlagged) {
    CHECK(hasFlag("Add 1/2 a cup.", flag::ambiguousNumber));
    CHECK_EQ(N("Add 1/2 a cup."), "Add one half a cup.");
    CHECK_EQ(N("Use 3/4 of it."), "Use three quarters of it.");
    CHECK(hasFlag("The number 2000 is round.", flag::ambiguousNumber));
    CHECK(hasFlag("Call it 1984 then.", flag::ambiguousNumber));
}

TEST(money) {
    CHECK_EQ(N("It costs $5."), "It costs five dollars.");
    CHECK_EQ(N("It costs $1."), "It costs one dollar.");
    CHECK_EQ(N("Only $5.50 today."), "Only five dollars and fifty cents today.");
    CHECK_EQ(N("Just $0.99 each."), "Just ninety-nine cents each.");
    CHECK_EQ(N("They raised $2.5 million."), "They raised two point five million dollars.");
    CHECK_EQ(N("A $3bn deal."), "A three billion dollars deal.");
    CHECK_EQ(N("It was \xC2\xA3" "20."), "It was twenty pounds.");
    CHECK_EQ(N("Pay \xE2\x82\xAC" "15 now."), "Pay fifteen euros now.");
    CHECK_EQ(N("Revenue was $1,200,000."), "Revenue was one million two hundred thousand dollars.");
}

TEST(percent) {
    CHECK_EQ(N("Up 43% this year."), "Up forty-three percent this year.");
    CHECK_EQ(N("Rates hit 2.5%."), "Rates hit two point five percent.");
    CHECK_EQ(N("Between 10-20% of them."), "Between ten to twenty percent of them.");
}

TEST(ordinalsInText) {
    CHECK_EQ(N("She came 1st and he came 22nd."), "She came first and he came twenty-second.");
    CHECK_EQ(N("The 3rd time."), "The third time.");
}

TEST(dates) {
    CHECK_EQ(N("On March 5, 1999 we met."), "On March fifth, nineteen ninety-nine we met.");
    CHECK_EQ(N("On Jan. 3rd we left."), "On January third we left.");
    CHECK_EQ(N("On 5 March we left."), "On the fifth of March we left.");
    CHECK_EQ(N("On the 5th of March we left."), "On the fifth of March we left.");
    CHECK_EQ(N("Due 12/25/2020."), "Due December twenty-fifth, twenty twenty.");
    CHECK(hasFlag("Due 5/3/2020.", flag::ambiguousNumber));
    CHECK_EQ(N("Logged 2024-03-05."), "Logged March fifth, twenty twenty-four.");
}

TEST(times) {
    CHECK_EQ(N("Meet at 3:30."), "Meet at three thirty.");
    CHECK_EQ(N("Meet at 3:05 today."), "Meet at three oh five today.");
    CHECK_EQ(N("Meet at 3:00 today."), "Meet at three o'clock today.");
    CHECK_EQ(N("Meet at 10am today."), "Meet at ten a m today.");
    CHECK_EQ(N("Meet at 7:15 p.m. today."), "Meet at seven fifteen p m today.");
    CHECK_EQ(N("We open at 9 a.m."), "We open at nine a m.");
    CHECK_EQ(N("It works nine to five, 9-5."), "It works nine to five, nine to five.");
}

TEST(decimalsAndDigits) {
    CHECK_EQ(N("Pi is 3.14."), "Pi is three point one four.");
    CHECK_EQ(N("Agent 007 arrived."), "Agent zero zero seven arrived.");
    CHECK_EQ(N("Call 555-1234 now."), "Call five five five, one two three four now.");
    CHECK_EQ(N("Call (555) 123-4567 now."), "Call five five five, one two three, four five six seven now.");
    CHECK_EQ(N("Call 555-123-4567."), "Call five five five, one two three, four five six seven.");
}

TEST(units) {
    CHECK_EQ(N("It is 5km away."), "It is five kilometers away.");
    CHECK_EQ(N("It is 1 km away."), "It is one kilometer away.");
    CHECK_EQ(N("Drive 60 mph there."), "Drive sixty miles per hour there.");
    CHECK_EQ(N("A 2GB file."), "A two gigabytes file.");
    CHECK_EQ(N("We hit 10k users."), "We hit ten thousand users.");
}

TEST(abbreviations) {
    CHECK_EQ(N("Dr. Smith is here."), "Doctor Smith is here.");
    CHECK_EQ(N("Ask Mr. Jones and Mrs. Jones."), "Ask mister Jones and missus Jones.");
    CHECK_EQ(N("Fruit, e.g., apples."), "Fruit, for example, apples.");
    CHECK_EQ(N("Apples, pears, etc."), "Apples, pears, et cetera.");
    CHECK_EQ(N("He moved to the U.S."), "He moved to the u s.");
    CHECK_EQ(N("The U.S. economy grew."), "The u s economy grew.");
    CHECK_EQ(N("Room No. 5 is free."), "Room number five is free.");
    CHECK_EQ(N("I said no."), "I said no.");
    CHECK_EQ(N("Tom & Jerry."), "Tom and Jerry.");
    CHECK_EQ(N("Ranked #1 in sales."), "Ranked number one in sales.");
}

TEST(acronyms) {
    CHECK_EQ(N("NASA launched it."), "Nasa launched it.");
    CHECK_EQ(N("NASA's rocket."), "Nasa's rocket.");
    CHECK_EQ(N("The FBI said so."), "The f b i said so.");
    CHECK(hasFlag("The FBI said so.", flag::spelledAcronym));
    CHECK(!hasFlag("NASA launched it.", flag::spelledAcronym));
    CHECK_EQ(N("Two CEOs met."), "Two c e os met.");
    CHECK_EQ(N("The XYZ's plan."), "The x y z's plan.");
    CHECK_EQ(N("It's OK."), "It's okay.");
    CHECK_EQ(N("COVID-19 spread."), "Covid nineteen spread.");
    CHECK_EQ(N("An MP3 file."), "An m p three file.");
}

TEST(letterA) {
    CHECK(hasFlag("Plan A failed.", flag::letterA));
    CHECK(!hasFlag("A cat sat.", flag::letterA));
    CHECK(hasFlag("The ABC show.", flag::letterA)); // "a b c" contains a standalone "a"
    CHECK(!hasFlag("The FBI said so.", flag::letterA));
}

TEST(casingAndPunctuation) {
    CHECK_EQ(N("Hello, world! Is it 5?"), "Hello, world! Is it five?");
    CHECK_EQ(N("\"Quote,\" she said."), "\"Quote,\" she said.");
    auto r = normalizeText("Dr. Who has 2 TVs.", true, dict());
    CHECK_EQ(r.text, "doctor who has two t vs.");
}

TEST(noDigitsLeftOnMixedText) {
    const char* samples[] = {
        "In 2023, 45% of 1,200 users paid $9.99 on 3/14 at 10:30am.",
        "The 1980s and '70s, 2nd place, 3.5 stars, #2, 5km, 12/25/2021, (555) 555-0100.",
        "Version 2.0 of the API costs $3m and 4K screens use 8GB.",
    };
    for (auto* s : samples) {
        auto r = normalizeText(s, false, dict());
        CHECK(noDigits(r.text));
        if (!noDigits(r.text)) fprintf(stderr, "    left digits: %s\n", r.text.c_str());
    }
    CHECK(!hasFlag("No digits here.", flag::digitsLeft));
}

TEST(pipeIsRemoved) {
    auto r = normalizeText("a | b", false, dict());
    CHECK(r.text.find('|') == std::string::npos);
}

TEST(dictionaryEditable) {
    PronunciationDictionary d;
    std::string err;
    CHECK(d.loadJSON(R"({"entries": {"GIF": "jif", "Dr.": "drive"}, "titles": []})", err));
    CHECK_EQ(normalizeText("A GIF on Elm Dr.", false, d).text, "A jif on Elm drive.");
    d.set("SQL", "sequel");
    CHECK_EQ(normalizeText("Learn SQL.", false, d).text, "Learn sequel.");
    // round trip
    PronunciationDictionary e;
    CHECK(e.loadJSON(d.toJSON(), err));
    CHECK_EQ(e.size(), d.size());
    // flat format is accepted too
    PronunciationDictionary f;
    CHECK(f.loadJSON(R"({"ACME": "acme"})", err));
    CHECK_EQ(normalizeText("ACME rules.", false, f).text, "Acme rules.");
    CHECK(!f.loadJSON("{not json", err));
}

// ------------------------------------------------------------------ sentence splitting

static std::vector<Word> words(std::initializer_list<const char*> ws) {
    std::vector<Word> v;
    double t = 0;
    for (auto* w : ws) {
        Word x;
        x.text = w;
        x.start = t;
        x.end = t + 0.3;
        t += 0.4;
        v.push_back(x);
    }
    return v;
}

TEST(sentenceSplit) {
    auto w = words({"Dr.", "Smith", "went", "to", "the", "U.S.", "The", "end.", "Really?", "Yes!", "Mr.", "X."});
    auto spans = splitSentences(w, dict());
    CHECK_EQ(spans.size(), size_t(5));
    if (spans.size() == 5) {
        CHECK_EQ(joinWords(w, spans[0].first, spans[0].last), "Dr. Smith went to the U.S.");
        CHECK_EQ(joinWords(w, spans[1].first, spans[1].last), "The end.");
        CHECK_EQ(joinWords(w, spans[2].first, spans[2].last), "Really?");
        CHECK_EQ(joinWords(w, spans[4].first, spans[4].last), "Mr. X.");
    }
    auto w2 = words({"We", "met", "J.", "K.", "Rowling", "e.g.", "yesterday."});
    CHECK_EQ(splitSentences(w2, dict()).size(), size_t(1));
    auto w3 = words({"He", "said", "\"Stop.\"", "Then", "left."});
    CHECK_EQ(splitSentences(w3, dict()).size(), size_t(2));
}

// ------------------------------------------------------------------ cutting

// Synthetic 16 kHz audio: tone bursts (speech) separated by silence.
struct Synth {
    std::vector<float> pcm;
    std::vector<Interval> speech;
};
static Synth synth(const std::vector<std::pair<double, double>>& bursts, double total, float noise = 0.0005f) {
    Synth s;
    s.pcm.assign(static_cast<size_t>(total * 16000), 0.0f);
    unsigned seed = 1;
    for (auto& x : s.pcm) {
        seed = seed * 1103515245 + 12345;
        x = noise * (((seed >> 16) & 0x7fff) / 16384.0f - 1.0f);
    }
    for (auto& [a, b] : bursts) {
        for (size_t i = static_cast<size_t>(a * 16000); i < static_cast<size_t>(b * 16000) && i < s.pcm.size(); ++i)
            s.pcm[i] += 0.3f * std::sin(2 * M_PI * 220 * i / 16000.0);
        s.speech.push_back({a, b});
    }
    return s;
}

TEST(cutsLandInSilence) {
    // three sentences: speech 0.5–2.5, 3.2–5.0, 5.8–8.0; DTW ends land a bit after speech offset (as in the spike)
    auto s = synth({{0.5, 2.5}, {3.2, 5.0}, {5.8, 8.0}}, 9.0);
    std::vector<Word> w;
    auto add = [&](const char* t, double a, double b) { w.push_back({t, a, b, 0.9f}); };
    add("Hello", 0.5, 0.9); add("there", 1.0, 1.5); add("my", 1.6, 1.9); add("friend.", 2.0, 2.7);
    add("How", 3.2, 3.5); add("are", 3.6, 4.0); add("you", 4.1, 4.5); add("today?", 4.6, 5.2);
    add("Fine", 5.8, 6.5); add("thanks", 6.6, 7.2); add("a", 7.3, 7.5); add("lot.", 7.6, 8.15);
    FileAnalysis an = analyzeAudio(s.pcm.data(), s.pcm.size(), s.speech);
    CutOptions opts;
    auto segs = cutSegments(w, an, opts, dict());
    CHECK_EQ(segs.size(), size_t(3));
    if (segs.size() != 3) return;
    for (auto& g : segs) {
        CHECK(g.startCutDb < an.silenceThresholdDb);
        CHECK(g.endCutDb < an.silenceThresholdDb);
        bool nonSilent = false;
        for (auto& f : g.flags) nonSilent |= f == flag::nonSilentCut;
        CHECK(!nonSilent);
    }
    // padding: ~150 ms around speech, clipped to the cut
    CHECK(std::abs(segs[0].start - 0.35) < 0.03);
    CHECK(std::abs(segs[0].end - 2.65) < 0.03);
    CHECK(segs[0].end <= segs[1].start);
    CHECK(std::abs(segs[1].start - 3.05) < 0.03);
    CHECK_EQ(segs[1].raw, "How are you today?");
    CHECK(segs[2].end <= 9.0);
}

TEST(parakeetStyleEndsLandInSilence) {
    // Worst case for Parakeet TDT: the sentence-final "." carries the last spoken token's end, quantized to 80 ms
    // frames, so it can sit at (or a frame before) the speech offset instead of inside the pause.
    auto s = synth({{0.5, 2.5}, {3.2, 5.0}, {5.8, 8.0}}, 9.0);
    std::vector<Word> w;
    auto add = [&](const char* t, double a, double b) { w.push_back({t, a, b, 0.9f}); };
    add("Hello", 0.48, 0.88); add("there", 0.96, 1.52); add("my", 1.60, 1.92); add("friend.", 2.00, 2.48);
    add("How", 3.20, 3.52); add("are", 3.60, 4.00); add("you", 4.08, 4.48); add("today?", 4.56, 5.04);
    add("Fine", 5.84, 6.48); add("thanks", 6.56, 7.20); add("a", 7.28, 7.52); add("lot.", 7.60, 7.92);
    FileAnalysis an = analyzeAudio(s.pcm.data(), s.pcm.size(), s.speech);
    auto segs = cutSegments(w, an, CutOptions(), dict());
    CHECK_EQ(segs.size(), size_t(3));
    if (segs.size() != 3) return;
    for (auto& g : segs) {
        CHECK(g.startCutDb < an.silenceThresholdDb);
        CHECK(g.endCutDb < an.silenceThresholdDb);
        for (auto& f : g.flags) CHECK(f != std::string(flag::nonSilentCut) && f != std::string(flag::noGap));
    }
    CHECK(std::abs(segs[0].end - 2.65) < 0.03); // padding is measured from the real speech offset
    CHECK(segs[0].end <= segs[1].start);
    CHECK(segs[1].end <= segs[2].start);
    CHECK_EQ(segs[2].raw, "Fine thanks a lot.");
}

TEST(tokenGrouping) {
    // pieces as libparakeet decodes them: word starts carry a leading space
    std::vector<AsrToken> t = {{" And", 0.24, 0.56, 1}, {" so", 0.56, 0.88, 1},   {",", 0.88, 0.88, 0.8f},
                               {" f", 1.28, 1.36, 1},   {"ell", 1.36, 1.52, 1},   {"ow", 1.52, 1.76, 1},
                               {" U", 1.80, 1.90, 1},   {".", 1.90, 1.90, 1},     {"S", 1.90, 2.00, 1},
                               {".", 2.00, 2.00, 1},    {" Yes", 2.40, 2.70, 1},  {" ?", 2.70, 2.70, 0.5f}, {" ", 2.70, 2.70, 1},
                               {"", 2.70, 2.70, 1}};
    auto w = groupTokensIntoWords(t);
    CHECK_EQ(w.size(), size_t(5));
    if (w.size() != 5) return;
    CHECK_EQ(w[0].text, "And");
    CHECK_EQ(w[1].text, "so,");
    CHECK(std::abs(w[1].end - 0.88) < 1e-9);
    CHECK_EQ(w[2].text, "fellow");
    CHECK(std::abs(w[2].start - 1.28) < 1e-9 && std::abs(w[2].end - 1.76) < 1e-9);
    CHECK_EQ(w[3].text, "U.S.");
    CHECK_EQ(w[4].text, "Yes?"); // a spaced punctuation piece attaches without its space
    CHECK(std::abs(w[4].prob - 0.75f) < 1e-6);
    auto spans = splitSentences(w, dict());
    CHECK_EQ(spans.size(), size_t(2)); // "U.S." is an abbreviation, not a sentence end
    CHECK(groupTokensIntoWords({}).empty());
    CHECK(groupTokensIntoWords({{" ", 0, 0, 1}, {" ", 0, 0, 1}}).empty());
    auto w2 = groupTokensIntoWords({{" New", 0, 0.2f, 1}, {" ", 0.2, 0.2, 1}, {"York", 0.2, 0.5, 1}});
    CHECK_EQ(w2.size(), size_t(2)); // a bare boundary piece splits words
}

TEST(asrBackendFactory) {
    std::string err;
    CHECK(makeAsrBackend("no-such-model", "/nonexistent", err) == nullptr);
    CHECK(err.find("unknown ASR model") != std::string::npos);
    err.clear();
    setWhisperLogging(false);
    CHECK(makeAsrBackend(kParakeetV2Id, "/nonexistent/ggml-parakeet.bin", err) == nullptr);
    CHECK(err.find("Parakeet") != std::string::npos);
    CHECK(Engine::create(kParakeetV3Id, "/nonexistent/p.bin", "/nonexistent/vad.bin") == nullptr);
}

// Optional: real models. Set TTSDC_MODELS to the model folder to run (skipped otherwise).
TEST(parakeetModelLoads) {
    const char* dir = getenv("TTSDC_MODELS");
    if (!dir) return;
    std::string path = std::string(dir) + "/ggml-parakeet-tdt-0.6b-v2-f16.bin";
    if (!std::filesystem::exists(path)) return;
    std::string err;
    auto b = makeAsrBackend(kParakeetV2Id, path, err);
    CHECK(b != nullptr);
    if (!b) return;
    CHECK_EQ(b->modelId(), std::string(kParakeetV2Id));
    CHECK(b->cacheTag() != std::string("turbo-dtw-v1"));
    CHECK(b->modelLicense().find("CC-BY-4.0") != std::string::npos);
    // 2 s of near-silence: must not fail, and cancel must abort
    std::vector<float> pcm(32000, 0.0f);
    std::vector<Word> w;
    double lastP = -1;
    CHECK(b->transcribe(pcm.data(), pcm.size(), 10.0, "", w, [&](double p) { lastP = p; }, [] { return false; }, err));
    CHECK(lastP == 1.0);
    for (auto& x : w) CHECK(x.start >= 10.0);
    w.clear();
    CHECK(!b->transcribe(pcm.data(), pcm.size(), 0, "", w, nullptr, [] { return true; }, err));
    CHECK_EQ(err, "cancelled");
}

TEST(trimLongSilence) {
    // a 4 s pause between sentences must not end up inside the segments
    auto s = synth({{0.5, 2.0}, {6.0, 8.0}}, 9.0);
    std::vector<Word> w = {{"One", 0.5, 1.0, 1}, {"two.", 1.2, 2.2, 1}, {"Three", 6.0, 6.5, 1}, {"four.", 6.8, 8.2, 1}};
    FileAnalysis an = analyzeAudio(s.pcm.data(), s.pcm.size(), s.speech);
    auto segs = cutSegments(w, an, CutOptions{}, dict());
    CHECK_EQ(segs.size(), size_t(2));
    if (segs.size() != 2) return;
    CHECK(segs[0].end < 2.25);
    CHECK(segs[1].start > 5.75);
}

TEST(lengthFlags) {
    auto s = synth({{0.5, 0.9}, {1.6, 18.0}}, 19.0);
    std::vector<Word> w = {{"Hi.", 0.5, 1.0, 1}};
    for (int k = 0; k < 40; ++k) w.push_back({k == 39 ? "end." : "word", 1.6 + k * 0.41, 1.9 + k * 0.41, 1});
    FileAnalysis an = analyzeAudio(s.pcm.data(), s.pcm.size(), s.speech);
    auto segs = cutSegments(w, an, CutOptions{}, dict());
    CHECK_EQ(segs.size(), size_t(2));
    if (segs.size() != 2) return;
    auto has = [](const Segment& g, const char* f) {
        for (auto& x : g.flags) if (x == f) return true;
        return false;
    };
    CHECK(has(segs[0], flag::tooShort));
    CHECK(has(segs[1], flag::tooLong));
}

TEST(nonSilentCutFlagged) {
    // continuous "speech" with no pause: the cut can't be silent
    auto s = synth({{0.2, 6.0}}, 6.5);
    std::vector<Word> w = {{"One", 0.3, 0.8, 1}, {"two.", 1.0, 3.0, 1}, {"Three", 3.1, 3.5, 1}, {"four.", 3.8, 5.9, 1}};
    FileAnalysis an = analyzeAudio(s.pcm.data(), s.pcm.size(), s.speech);
    auto segs = cutSegments(w, an, CutOptions{}, dict());
    CHECK_EQ(segs.size(), size_t(2));
    bool flagged = false;
    for (auto& f : segs[0].flags) flagged |= (f == flag::nonSilentCut || f == flag::noGap);
    CHECK(flagged);
}

TEST(droppedWordsFlagged) {
    // 6 s of speech, but the transcript has a 3 s hole between two words
    std::vector<std::pair<double, double>> bursts;
    for (int k = 0; k < 6; ++k) bursts.push_back({0.5 + k * 3.0, 0.5 + k * 3.0 + 2.2});
    auto s = synth(bursts, 19.0);
    std::vector<Word> w;
    // sentences 1..5 normal (about 6 words / 2.2 s), sentence 6 drops most of its words
    for (int k = 0; k < 5; ++k)
        for (int j = 0; j < 6; ++j) w.push_back({j == 5 ? "end." : "word", 0.5 + k * 3.0 + j * 0.35, 0.5 + k * 3.0 + j * 0.35 + 0.3, 1});
    w.push_back({"short.", 15.5, 17.8, 1});
    FileAnalysis an = analyzeAudio(s.pcm.data(), s.pcm.size(), s.speech);
    auto segs = cutSegments(w, an, CutOptions{}, dict());
    CHECK_EQ(segs.size(), size_t(6));
    if (segs.size() != 6) return;
    bool flagged = false;
    for (auto& f : segs[5].flags) flagged |= f == flag::slowSpeech;
    CHECK(flagged);
    bool clean = true;
    for (auto& f : segs[0].flags) clean &= f != flag::slowSpeech;
    CHECK(clean);
}

// ------------------------------------------------------------------ DSP + export

TEST(resampler) {
    // 1 kHz sine at 48 kHz -> 22.05 kHz keeps amplitude and frequency
    std::vector<float> x(48000);
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * std::sin(2 * M_PI * 1000 * i / 48000.0);
    auto y = resample(x.data(), x.size(), 48000, 22050);
    CHECK(std::abs(static_cast<int>(y.size()) - 22050) <= 1);
    double peak = 0;
    for (size_t i = 1000; i < y.size() - 1000; ++i) peak = std::max(peak, std::abs(static_cast<double>(y[i])));
    CHECK(std::abs(peak - 0.5) < 0.01);
    // zero crossings ~ 2 per cycle
    int zc = 0;
    for (size_t i = 1001; i < y.size() - 1000; ++i) zc += (y[i - 1] < 0) != (y[i] < 0);
    double secs = (y.size() - 2001) / 22050.0;
    CHECK(std::abs(zc / secs - 2000) < 10);
    // 15 kHz tone at 48 kHz is above the 8 kHz Nyquist of 16 kHz and must be filtered out
    for (size_t i = 0; i < x.size(); ++i) x[i] = 0.5f * std::sin(2 * M_PI * 15000 * i / 48000.0);
    auto z = resample(x.data(), x.size(), 48000, 16000);
    double rms = 0;
    for (size_t i = 500; i < z.size() - 500; ++i) rms += z[i] * z[i];
    rms = std::sqrt(rms / (z.size() - 1000));
    CHECK(rms < 0.005);
}

TEST(exportLJSpeech) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "ttsdc-export-test";
    fs::remove_all(dir);
    std::vector<float> pcm(48000 * 4, 0.0f);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = 0.2f * std::sin(2 * M_PI * 300 * i / 48000.0);
    Segment a, b, c;
    a.start = 0.0; a.end = 1.5; a.raw = "It costs $5."; a.normalized = "It costs five dollars.";
    b.start = 1.6; b.end = 3.0; b.raw = "Bad | pipe"; b.normalized = "Bad | pipe"; b.flags = {flag::symbol};
    c.start = 3.1; c.end = 3.9; c.raw = "Last."; c.normalized = "Last.";
    for (bool three : {false, true}) {
        ExportOptions o;
        o.sampleRate = 22050;
        o.threeColumns = three;
        o.dropFlagged = !three;
        o.modelName = "test-model";
        o.modelId = "parakeet-tdt-0.6b-v2";
        o.modelLicense = "CC-BY-4.0 (NVIDIA)";
        std::string err;
        Exporter* e = Exporter::create(dir.string(), o, err);
        CHECK(e != nullptr);
        if (!e) return;
        CHECK(e->addFile("My Talk (final).mp3", pcm.data(), pcm.size(), 48000, {a, b, c}, err));
        ExportSummary s = e->finish();
        Exporter::destroy(e);
        CHECK(s.ok);
        CHECK_EQ(s.written, three ? 3 : 2);
        std::ifstream f(dir / "metadata.csv");
        std::string line1, line2;
        std::getline(f, line1);
        std::getline(f, line2);
        if (three) {
            CHECK_EQ(line1, "My_Talk_final_0001|It costs $5.|It costs five dollars.");
            CHECK_EQ(line2, "My_Talk_final_0002|Bad pipe|Bad pipe");
        } else {
            CHECK_EQ(line1, "My_Talk_final_0001|It costs five dollars.");
            CHECK_EQ(line2, "My_Talk_final_0002|Last.");
        }
        std::vector<float> wav;
        int rate = 0;
        CHECK(readWav16((dir / "wavs" / "My_Talk_final_0001.wav").string(), wav, rate, err));
        CHECK_EQ(rate, 22050);
        CHECK(std::abs(static_cast<int>(wav.size()) - static_cast<int>(1.5 * 22050)) <= 2);
        CHECK(std::abs(wav.front()) < 0.01f); // faded in
        CHECK(std::abs(wav.back()) < 0.01f);  // faded out
        CHECK(fs::exists(dir / "dataset_info.json"));
        {
            std::ifstream fi(dir / "dataset_info.json");
            std::string info((std::istreambuf_iterator<char>(fi)), std::istreambuf_iterator<char>());
            CHECK(info.find("\"asr_model_id\": \"parakeet-tdt-0.6b-v2\"") != std::string::npos);
            CHECK(info.find("\"asr_model_license\": \"CC-BY-4.0 (NVIDIA)\"") != std::string::npos);
        }
        fs::remove_all(dir);
    }
    CHECK_EQ(sanitizeId("a b/c.wav"), "c");
    CHECK_EQ(sanitizeId("Interview #3 - Part 2.m4a"), "Interview_3_-_Part_2");
}

TEST(hashing) {
    namespace fs = std::filesystem;
    fs::path p = fs::temp_directory_path() / "ttsdc-hash-test.txt";
    { std::ofstream f(p); f << "abc"; }
    CHECK_EQ(hashFile(p.string()), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    fs::remove(p);
    CHECK_EQ(hashFile("/nonexistent/file"), "");
}

int main(int argc, char** argv) {
    std::string only = argc > 1 ? argv[1] : "";
    for (auto& t : registry()) {
        if (!only.empty() && only != t.name) continue;
        gCurrent = t.name;
        int before = gFailed;
        t.fn();
        printf("%s %s\n", gFailed == before ? "ok  " : "FAIL", t.name);
    }
    printf("\n%d checks passed, %d failed\n", gPassed, gFailed);
    return gFailed == 0 ? 0 : 1;
}
