// Speech recognition and voice activity detection.
//
// AsrBackend is a deliberately small interface; the only implementation is WhisperTurboBackend
// (whisper.cpp, large-v3-turbo, DTW token timestamps). See spike/FINDINGS.md for why.
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
    /// Stable identifier recorded in caches and dataset_info.json.
    virtual std::string modelName() const = 0;
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
