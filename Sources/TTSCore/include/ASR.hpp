// Speech recognition and voice activity detection.
//
// AsrBackend is a deliberately small interface with two implementations:
//   - WhisperTurboBackend: whisper.cpp, large-v3-turbo, DTW token timestamps (default; see spike/FINDINGS.md)
//   - ParakeetBackend: NVIDIA Parakeet TDT 0.6B (v2 English / v3 multilingual) through whisper.cpp's libparakeet,
//     with TDT token timestamps.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "TTSTypes.hpp"

struct whisper_vad_context;

namespace ttsdc {

/// Progress in [0, 1]. Return value ignored.
using ProgressFn = std::function<void(double)>;
/// Polled frequently; return true to abort.
using CancelFn = std::function<bool()>;

class AsrBackend {
public:
    virtual ~AsrBackend() = default;
    /// Human-readable description recorded in caches and dataset_info.json.
    virtual std::string modelName() const = 0;
    /// Short stable id ("whisper-large-v3-turbo", "parakeet-tdt-0.6b-v2", ...).
    virtual std::string modelId() const = 0;
    /// Weights licence, recorded in dataset_info.json.
    virtual std::string modelLicense() const = 0;
    /// Part of the transcript cache key: changes whenever the model or the way words/times are produced changes,
    /// so switching backends never reuses another model's transcript.
    virtual std::string cacheTag() const = 0;
    /// Audio per transcribe() call (the engine splits long files at long pauses near this length).
    virtual double chunkSeconds() const { return 300.0; }
    /// Transcribe 16 kHz mono audio. `timeOffset` is added to all word times.
    /// `prompt` is previous text for continuity. Returns false on error/cancel.
    virtual bool transcribe(const float* pcm16k, size_t n, double timeOffset, const std::string& prompt,
                            std::vector<Word>& words, const ProgressFn& progress, const CancelFn& cancel,
                            std::string& error) = 0;
};

struct WhisperOptions {
    std::string language = "en";
    int threads = 0;        ///< 0 = automatic
    bool useGpu = true;
};

/// Creates the whisper.cpp large-v3-turbo backend (DTW timestamps, flash attention off).
/// Returns null and sets `error` if the model can't be loaded.
std::unique_ptr<AsrBackend> makeWhisperTurboBackend(const std::string& modelPath, const WhisperOptions& opts,
                                                    std::string& error);

/// Model ids accepted by makeAsrBackend().
inline constexpr const char* kWhisperTurboId = "whisper-large-v3-turbo";
inline constexpr const char* kParakeetV2Id = "parakeet-tdt-0.6b-v2";
inline constexpr const char* kParakeetV3Id = "parakeet-tdt-0.6b-v3";

/// Creates an NVIDIA Parakeet TDT backend (libparakeet from the vendored whisper.cpp). `modelId` is
/// kParakeetV2Id or kParakeetV3Id and is used for the cache tag and dataset_info.json.
std::unique_ptr<AsrBackend> makeParakeetBackend(const std::string& modelPath, const std::string& modelId,
                                                const WhisperOptions& opts, std::string& error);

/// Creates the backend for `modelId` (one of the k*Id constants). Unknown id -> null + error.
std::unique_ptr<AsrBackend> makeAsrBackend(const std::string& modelId, const std::string& modelPath,
                                           std::string& error);

/// Groups subword tokens into words: a token starting with a space starts a new word, unless it is only
/// punctuation (which attaches to the previous word). Times are seconds; `start`/`end` of each word are the
/// first token's start and the last token's end. Exposed for tests.
struct AsrToken {
    std::string text; ///< decoded piece with a leading space for word starts (" And", "so", ",")
    double start = 0, end = 0;
    float prob = 0;
};
std::vector<Word> groupTokensIntoWords(const std::vector<AsrToken>& tokens);

/// Silero VAD through whisper.cpp's whisper_vad_* API.
class SileroVad {
public:
    ~SileroVad();
    static std::unique_ptr<SileroVad> load(const std::string& modelPath, std::string& error);
    /// Speech intervals for 16 kHz mono audio.
    bool detect(const float* pcm16k, size_t n, std::vector<Interval>& speech, std::string& error);

private:
    SileroVad() = default;
    ::whisper_vad_context* ctx_ = nullptr;
};

/// Silences whisper.cpp / ggml logging (call once). Pass true to re-enable.
void setWhisperLogging(bool enabled);

} // namespace ttsdc
