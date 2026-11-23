#include "Engine.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../vendor/json.hpp"
#include "ASR.hpp"
#include "DSP.hpp"
#include "Flags.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace ttsdc {

namespace {

std::string gCreateError;

void mergeFlags(std::vector<std::string>& into, const std::vector<std::string>& add) {
    for (auto& f : add)
        if (std::find(into.begin(), into.end(), f) == into.end()) into.push_back(f);
}

bool writeFileAtomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

struct Chunk {
    double start = 0, end = 0;
    bool done = false;
    std::vector<Word> words;
};

/// Chunk boundaries at the longest VAD pause near every `target` seconds.
std::vector<Chunk> planChunks(double duration, const std::vector<Interval>& speech, double target) {
    std::vector<Chunk> chunks;
    double start = 0;
    const double slackBefore = std::min(60.0, target * 0.2), slackAfter = std::min(30.0, target * 0.1);
    while (duration - start > target + slackBefore) {
        double lo = start + target - slackBefore, hi = start + target + slackAfter;
        double bestLen = -1, bestCut = start + target;
        for (size_t i = 0; i + 1 < speech.size(); ++i) {
            double gs = speech[i].end, ge = speech[i + 1].start;
            double mid = (gs + ge) / 2;
            if (mid < lo || mid > hi) continue;
            if (ge - gs > bestLen) bestLen = ge - gs, bestCut = mid;
        }
        chunks.push_back({start, bestCut, false, {}});
        start = bestCut;
    }
    chunks.push_back({start, duration, false, {}});
    return chunks;
}

json wordsToJson(const std::vector<Word>& ws) {
    json a = json::array();
    for (auto& w : ws) a.push_back({w.text, w.start, w.end, w.prob});
    return a;
}

std::vector<Word> wordsFromJson(const json& a) {
    std::vector<Word> ws;
    for (auto& e : a) {
        Word w;
        w.text = e[0].get<std::string>();
        w.start = e[1].get<double>();
        w.end = e[2].get<double>();
        w.prob = e[3].get<float>();
        ws.push_back(w);
    }
    return ws;
}

std::string isoNow() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
    return buf;
}

} // namespace

std::string hashFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = f.gcount();
        if (got > 0) CC_SHA256_Update(&ctx, buf.data(), static_cast<CC_LONG>(got));
    }
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(md, &ctx);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (unsigned char c : md) s += hex[c >> 4], s += hex[c & 15];
    return s;
}

Engine* Engine::create(const std::string& asrModelId, const std::string& asrModelPath, const std::string& vadModelPath) {
    setWhisperLogging(false);
    std::unique_ptr<Engine> e(new Engine());
    std::string err;
    e->vad_ = SileroVad::load(vadModelPath, err);
    if (!e->vad_) {
        gCreateError = err;
        return nullptr;
    }
    e->asr_ = makeAsrBackend(asrModelId, asrModelPath, err);
    if (!e->asr_) {
        gCreateError = err;
        return nullptr;
    }
    return e.release();
}

std::string Engine::lastCreateError() { return gCreateError; }
void Engine::destroy(Engine* e) { delete e; }
Engine::~Engine() = default;

std::string Engine::modelName() const { return asr_ ? asr_->modelName() : ""; }
std::string Engine::modelId() const { return asr_ ? asr_->modelId() : ""; }
std::string Engine::modelLicense() const { return asr_ ? asr_->modelLicense() : ""; }

bool Engine::loadDictionaryFile(const std::string& path, std::string& error) {
    std::lock_guard<std::mutex> lock(dictMutex_);
    return dict_.loadFile(path, error);
}
std::string Engine::dictionaryJSON() const {
    std::lock_guard<std::mutex> lock(dictMutex_);
    return dict_.toJSON();
}
bool Engine::setDictionaryJSON(const std::string& j, std::string& error) {
    std::lock_guard<std::mutex> lock(dictMutex_);
    return dict_.loadJSON(j, error);
}
NormalizeResult Engine::normalize(const std::string& text, bool lowercaseAll) const {
    std::lock_guard<std::mutex> lock(dictMutex_);
    return normalizeText(text, lowercaseAll, dict_);
}

void Engine::clearCache(const std::string& cacheDir, const std::string& cacheKey) {
    if (cacheKey.empty()) return;
    std::error_code ec;
    fs::remove(fs::path(cacheDir) / (cacheKey + ".json"), ec);
}

FileResult Engine::processFile(const std::string& audioPath, const float* pcm, size_t n, const std::string& cacheDir,
                               const CutOptions& cut, bool lowercaseAll) {
    // C++ exceptions must not unwind into Swift (that aborts the process); report them as a failed file
    try {
        return processFileImpl(audioPath, pcm, n, cacheDir, cut, lowercaseAll);
    } catch (const std::exception& e) {
        FileResult r;
        r.error = std::string("internal error: ") + e.what();
        stage_.store(static_cast<int>(Stage::failed));
        return r;
    } catch (...) {
        FileResult r;
        r.error = "internal error";
        stage_.store(static_cast<int>(Stage::failed));
        return r;
    }
}

FileResult Engine::processFileImpl(const std::string& audioPath, const float* pcm, size_t n,
                                   const std::string& cacheDir, const CutOptions& cut, bool lowercaseAll) {
    FileResult r;
    cancel_.store(false);
    progress_.store(0);
    auto fail = [&](const std::string& msg) {
        r.error = msg;
        r.cancelled = cancel_.load();
        stage_.store(static_cast<int>(r.cancelled ? Stage::cancelled : Stage::failed));
        return r;
    };

    stage_.store(static_cast<int>(Stage::hashing));
    std::string hash = hashFile(audioPath);
    if (hash.empty()) return fail("cannot read " + audioPath);
    // keyed by content hash + backend/model tag: switching models never reuses another model's transcript
    const std::string cacheVersion = asr_->cacheTag();
    r.cacheKey = hash + "-" + cacheVersion;
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    const std::string cachePath = (fs::path(cacheDir) / (r.cacheKey + ".json")).string();
    const double duration = n / 16000.0;

    // ---- load cache
    std::vector<Interval> speech;
    std::vector<Chunk> chunks;
    bool haveVad = false;
    {
        std::ifstream f(cachePath);
        if (f) {
            try {
                json j = json::parse(f);
                if (j.value("samples", 0ull) == n && j.value("version", "") == cacheVersion) {
                    for (auto& v : j["vad"]) speech.push_back({v[0].get<double>(), v[1].get<double>()});
                    haveVad = true;
                    for (auto& c : j["chunks"]) {
                        Chunk ch;
                        ch.start = c["start"].get<double>();
                        ch.end = c["end"].get<double>();
                        ch.done = c["done"].get<bool>();
                        if (ch.done) ch.words = wordsFromJson(c["words"]);
                        chunks.push_back(std::move(ch));
                    }
                }
            } catch (...) {
                speech.clear();
                chunks.clear();
                haveVad = false;
            }
        }
    }
    auto saveCache = [&]() {
        json j;
        j["version"] = cacheVersion;
        j["model"] = modelName();
        j["source"] = fs::path(audioPath).filename().string();
        j["samples"] = n;
        j["vad"] = json::array();
        for (auto& s : speech) j["vad"].push_back({s.start, s.end});
        j["chunks"] = json::array();
        for (auto& c : chunks) {
            json cj = {{"start", c.start}, {"end", c.end}, {"done", c.done}};
            if (c.done) cj["words"] = wordsToJson(c.words);
            j["chunks"].push_back(cj);
        }
        writeFileAtomic(cachePath, j.dump());
    };

    // ---- VAD
    if (!haveVad) {
        stage_.store(static_cast<int>(Stage::vad));
        std::string err;
        if (!vad_->detect(pcm, n, speech, err)) return fail(err);
        if (cancel_.load()) return fail("cancelled");
        chunks = planChunks(duration, speech, asr_->chunkSeconds());
        saveCache();
    }
    if (chunks.empty()) {
        chunks = planChunks(duration, speech, asr_->chunkSeconds());
        saveCache();
    }
    progress_.store(0.05);

    // ---- transcription, chunk by chunk
    stage_.store(static_cast<int>(Stage::transcribing));
    r.totalChunks = static_cast<int>(chunks.size());
    double doneSeconds = 0;
    for (auto& c : chunks)
        if (c.done) doneSeconds += c.end - c.start, ++r.resumedChunks;
    std::string prompt;
    for (size_t ci = 0; ci < chunks.size(); ++ci) {
        Chunk& c = chunks[ci];
        if (!c.done) {
            size_t a = static_cast<size_t>(std::max(0.0, c.start) * 16000), b = std::min(n, static_cast<size_t>(c.end * 16000));
            double chunkLen = c.end - c.start;
            ProgressFn prog = [&](double p) {
                progress_.store(0.05 + 0.93 * std::min(1.0, (doneSeconds + p * chunkLen) / std::max(1e-9, duration)));
            };
            CancelFn canc = [&]() { return cancel_.load(); };
            std::string err;
            std::vector<Word> words;
            if (b > a && !asr_->transcribe(pcm + a, b - a, c.start, prompt, words, prog, canc, err)) return fail(err);
            c.words = std::move(words);
            c.done = true;
            doneSeconds += chunkLen;
            saveCache();
        }
        // previous text as a prompt keeps punctuation/casing style consistent across chunks
        std::string text = joinWords(c.words, 0, c.words.empty() ? 0 : c.words.size() - 1);
        prompt = text.size() > 200 ? text.substr(text.size() - 200) : text;
        if (auto sp = prompt.find(' '); sp != std::string::npos && text.size() > 200) prompt = prompt.substr(sp + 1);
    }
    progress_.store(0.98);

    // ---- cutting + normalization
    stage_.store(static_cast<int>(Stage::cutting));
    std::vector<Word> words;
    for (auto& c : chunks) words.insert(words.end(), c.words.begin(), c.words.end());
    r.analysis = analyzeAudio(pcm, n, speech);
    {
        std::lock_guard<std::mutex> lock(dictMutex_);
        r.segments = cutSegments(words, r.analysis, cut, dict_);
        for (auto& s : r.segments) {
            NormalizeResult nr = normalizeText(s.raw, lowercaseAll, dict_);
            s.normalized = nr.text;
            mergeFlags(s.flags, nr.flags);
        }
    }
    progress_.store(1.0);
    stage_.store(static_cast<int>(Stage::done));
    r.ok = true;
    return r;
}

void refreshSegment(Segment& seg, const Engine& engine, const FileAnalysis& analysis, const CutOptions& cut,
                    bool lowercaseAll, bool renormalize) {
    static const char* normFlags[] = {flag::ambiguousNumber, flag::spelledAcronym, flag::letterA, flag::digitsLeft,
                                      flag::symbol};
    if (renormalize) {
        for (auto* f : normFlags) seg.flags.erase(std::remove(seg.flags.begin(), seg.flags.end(), std::string(f)), seg.flags.end());
        NormalizeResult nr = engine.normalize(seg.raw, lowercaseAll);
        seg.normalized = nr.text;
        mergeFlags(seg.flags, nr.flags);
    } else {
        // normalized text edited by hand: only re-check for leftover digits
        seg.flags.erase(std::remove(seg.flags.begin(), seg.flags.end(), std::string(flag::digitsLeft)), seg.flags.end());
        if (std::any_of(seg.normalized.begin(), seg.normalized.end(), [](char c) { return c >= '0' && c <= '9'; }))
            seg.flags.push_back(flag::digitsLeft);
    }
    recheckSegment(seg, analysis, cut);
}

// ------------------------------------------------------------------ export

std::string sanitizeId(const std::string& filename) {
    std::string stem = fs::path(filename).stem().string();
    std::string out;
    for (unsigned char c : stem) {
        if (std::isalnum(c) || c == '-' || c == '_') out += static_cast<char>(c);
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out.empty() ? "audio" : out;
}

std::string sanitizeCsvText(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '|' || c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && !out.empty() && out.back() == ' ') continue;
        out += c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t a = out.find_first_not_of(' ');
    return a == std::string::npos ? "" : out.substr(a);
}

Exporter* Exporter::create(const std::string& outDir, const ExportOptions& opts, std::string& error) {
    static const int rates[] = {16000, 22050, 24000, 44100, 48000};
    if (std::find(std::begin(rates), std::end(rates), opts.sampleRate) == std::end(rates)) {
        error = "unsupported sample rate " + std::to_string(opts.sampleRate);
        return nullptr;
    }
    std::error_code ec;
    fs::create_directories(fs::path(outDir) / "wavs", ec);
    if (ec) {
        error = "cannot create " + outDir + ": " + ec.message();
        return nullptr;
    }
    auto* e = new Exporter();
    e->outDir_ = outDir;
    e->opts_ = opts;
    return e;
}

void Exporter::destroy(Exporter* e) { delete e; }

bool Exporter::addFile(const std::string& sourceName, const float* pcm, size_t n, double rate,
                       const std::vector<Segment>& segments, std::string& error) {
    const std::string stem = sanitizeId(sourceName);
    // disambiguate two sources with the same stem
    std::string base = stem;
    int dup = 1;
    while (std::any_of(sources_.begin(), sources_.end(), [&](auto& s) { return s.first == base; }))
        base = stem + "_" + std::to_string(++dup);
    int count = 0;
    for (auto& s : segments) {
        if (opts_.dropFlagged && !s.flags.empty()) {
            ++skipped_;
            continue;
        }
        if (sanitizeCsvText(s.normalized).empty()) {
            ++skipped_;
            continue;
        }
        size_t a = static_cast<size_t>(std::max(0.0, s.start) * rate);
        size_t b = std::min(n, static_cast<size_t>(std::max(0.0, s.end) * rate));
        if (b <= a) {
            ++skipped_;
            continue;
        }
        std::vector<float> out = resample(pcm + a, b - a, rate, opts_.sampleRate);
        applyFades(out, static_cast<size_t>(opts_.fadeSeconds * opts_.sampleRate));
        char id[512];
        snprintf(id, sizeof id, "%s_%04d", base.c_str(), ++count);
        std::string path = (fs::path(outDir_) / "wavs" / (std::string(id) + ".wav")).string();
        if (!writeWav16(path, out.data(), out.size(), opts_.sampleRate, error)) return false;
        rows_.push_back({id, sanitizeCsvText(s.raw), sanitizeCsvText(s.normalized)});
        seconds_ += static_cast<double>(out.size()) / opts_.sampleRate;
    }
    sources_.push_back({base, count});
    return true;
}

ExportSummary Exporter::finish() {
    ExportSummary sum;
    std::string csv;
    for (auto& r : rows_) {
        csv += r.id + "|";
        if (opts_.threeColumns) csv += r.raw + "|";
        csv += r.normalized + "\n";
    }
    if (!writeFileAtomic((fs::path(outDir_) / "metadata.csv").string(), csv)) {
        sum.error = "cannot write metadata.csv";
        return sum;
    }
    nlohmann::ordered_json j;
    j["format"] = "LJSpeech";
    j["created"] = isoNow();
    j["generator"] = "TTS Dataset Creator " + opts_.appVersion;
    j["asr_model"] = opts_.modelName;
    if (!opts_.modelId.empty()) j["asr_model_id"] = opts_.modelId;
    if (!opts_.modelLicense.empty()) j["asr_model_license"] = opts_.modelLicense;
    j["sample_rate"] = opts_.sampleRate;
    j["channels"] = 1;
    j["sample_format"] = "pcm_s16le";
    j["metadata_columns"] = opts_.threeColumns ? json::array({"id", "raw", "normalized"}) : json::array({"id", "normalized"});
    j["settings"] = {{"min_seconds", opts_.minSeconds},     {"max_seconds", opts_.maxSeconds},
                     {"fade_seconds", opts_.fadeSeconds},   {"lowercase_all", opts_.lowercaseAll},
                     {"drop_flagged", opts_.dropFlagged},   {"language", "en"}};
    j["segments"] = rows_.size();
    j["skipped"] = skipped_;
    j["total_seconds"] = std::round(seconds_ * 100) / 100;
    j["sources"] = json::array();
    for (auto& [name, count] : sources_) j["sources"].push_back({{"id_prefix", name}, {"segments", count}});
    if (!writeFileAtomic((fs::path(outDir_) / "dataset_info.json").string(), j.dump(2) + "\n")) {
        sum.error = "cannot write dataset_info.json";
        return sum;
    }
    sum.ok = true;
    sum.written = static_cast<int>(rows_.size());
    sum.skipped = skipped_;
    sum.totalSeconds = seconds_;
    return sum;
}

} // namespace ttsdc
