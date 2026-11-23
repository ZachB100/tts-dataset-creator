// Swift-facing facade over the core: transcription with resume cache, cutting, normalization, export.
//
// Engine and Exporter are imported into Swift as reference types (SWIFT_IMMORTAL_REFERENCE): Swift never
// copies or ref-counts them; create()/destroy() manage their lifetime explicitly.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Bridge.hpp"
#include "Normalizer.hpp"
#include "Segmenter.hpp"
#include "TTSTypes.hpp"

#if defined(__has_attribute) && __has_attribute(swift_attr)
#define TTSDC_IMMORTAL_REFERENCE                                                                     \
    __attribute__((swift_attr("import_reference"))) __attribute__((swift_attr("retain:immortal"))) \
    __attribute__((swift_attr("release:immortal")))
#else
#define TTSDC_IMMORTAL_REFERENCE
#endif

namespace ttsdc {

class AsrBackend;
class SileroVad;

/// Processing stages reported to the UI.
enum class Stage : int { idle = 0, hashing, vad, transcribing, cutting, done, failed, cancelled };

/// Everything the UI needs about one processed file.
struct FileResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::string cacheKey;
    std::vector<Segment> segments;
    FileAnalysis analysis;
    int resumedChunks = 0;   ///< chunks taken from the cache instead of being transcribed
    int totalChunks = 0;
};

/// SHA-256 of the file's bytes, hex. Empty on error.
std::string hashFile(const std::string& path);

class TTSDC_IMMORTAL_REFERENCE Engine {
public:
    /// Loads the ASR model (`asrModelId`: "whisper-large-v3-turbo", "parakeet-tdt-0.6b-v2" or
    /// "parakeet-tdt-0.6b-v3") and Silero VAD. Returns null on failure (see lastCreateError()).
    static Engine* create(const std::string& asrModelId, const std::string& asrModelPath, const std::string& vadModelPath);
    static std::string lastCreateError();
    static void destroy(Engine* e);

    /// Blocking: VAD -> chunked ASR transcription (cached per chunk) -> cutting -> normalization.
    /// `cacheDir` holds <hash>.json; a crash or cancel resumes from the last finished chunk.
    FileResult processFile(const std::string& audioPath, const float* pcm16k, size_t n, const std::string& cacheDir,
                           const CutOptions& cut, bool lowercaseAll);

    /// Thread-safe controls/progress for the file currently being processed.
    void requestCancel() { cancel_.store(true); }
    double progress() const { return progress_.load(); }
    int stage() const { return stage_.load(); }

    std::string modelName() const;
    std::string modelId() const;
    std::string modelLicense() const;

    /// Pronunciation dictionary used for normalization and sentence splitting.
    bool loadDictionaryFile(const std::string& path, std::string& error);
    std::string dictionaryJSON() const;
    bool setDictionaryJSON(const std::string& json, std::string& error);
    NormalizeResult normalize(const std::string& text, bool lowercaseAll) const;

    /// Removes the transcript cache entry for a file (after a successful export).
    static void clearCache(const std::string& cacheDir, const std::string& cacheKey);

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine();

private:
    Engine() = default;
    FileResult processFileImpl(const std::string& audioPath, const float* pcm16k, size_t n,
                               const std::string& cacheDir, const CutOptions& cut, bool lowercaseAll);
    std::unique_ptr<AsrBackend> asr_;
    std::unique_ptr<SileroVad> vad_;
    PronunciationDictionary dict_;
    mutable std::mutex dictMutex_;
    std::atomic<bool> cancel_{false};
    std::atomic<double> progress_{0};
    std::atomic<int> stage_{0};
};

/// Applies normalization (with the engine's dictionary) to a segment and merges flags,
/// then re-checks the cut flags. Used after the user edits raw text or bounds.
void refreshSegment(Segment& seg, const Engine& engine, const FileAnalysis& analysis, const CutOptions& cut,
                    bool lowercaseAll, bool renormalize);

/// LJSpeech writer: wavs/<id>.wav + metadata.csv + dataset_info.json.
class TTSDC_IMMORTAL_REFERENCE Exporter {
public:
    static Exporter* create(const std::string& outDir, const ExportOptions& opts, std::string& error);
    static void destroy(Exporter* e);

    /// Export the segments of one source file. `pcm` is mono float at `rate` (the source rate).
    bool addFile(const std::string& sourceName, const float* pcm, size_t n, double rate,
                 const std::vector<Segment>& segments, std::string& error);
    /// Writes metadata.csv and dataset_info.json.
    ExportSummary finish();

    Exporter(const Exporter&) = delete;
    Exporter& operator=(const Exporter&) = delete;

private:
    Exporter() = default;
    struct Row {
        std::string id, raw, normalized;
    };
    std::string outDir_;
    ExportOptions opts_;
    std::vector<Row> rows_;
    std::vector<std::pair<std::string, int>> sources_;
    int skipped_ = 0;
    double seconds_ = 0;
};

/// Filename stem made safe for IDs: "My Talk (final).mp3" -> "My_Talk_final".
std::string sanitizeId(const std::string& filename);
/// Text made safe for metadata.csv (no '|' or newlines).
std::string sanitizeCsvText(const std::string& text);

} // namespace ttsdc
