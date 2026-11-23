#include "ASR.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

#include "parakeet.h"
#include "whisper.h"

namespace ttsdc {

namespace {

bool gLogging = false;

void logCallback(ggml_log_level level, const char* text, void*) {
    if (gLogging || level == GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
}

int defaultThreads() {
    unsigned n = std::thread::hardware_concurrency();
    return static_cast<int>(std::clamp(n ? n / 2 : 4u, 2u, 8u));
}

class WhisperTurboBackend final : public AsrBackend {
public:
    WhisperTurboBackend(whisper_context* ctx, WhisperOptions opts) : ctx_(ctx), opts_(std::move(opts)) {}
    ~WhisperTurboBackend() override { whisper_free(ctx_); }

    std::string modelName() const override { return "whisper-large-v3-turbo (whisper.cpp v1.9.6, DTW)"; }
    std::string modelId() const override { return kWhisperTurboId; }
    std::string modelLicense() const override { return "MIT (OpenAI)"; }
    // unchanged from the first release so existing caches stay valid
    std::string cacheTag() const override { return "turbo-dtw-v1"; }

    bool transcribe(const float* pcm, size_t n, double timeOffset, const std::string& prompt, std::vector<Word>& words,
                    const ProgressFn& progress, const CancelFn& cancel, std::string& error) override {
        struct Callbacks {
            const ProgressFn* progress;
            const CancelFn* cancel;
        } cb{&progress, &cancel};

        whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
        p.beam_search.beam_size = 5; // same as whisper-cli defaults used in the spike
        p.greedy.best_of = 5;
        p.n_threads = opts_.threads > 0 ? opts_.threads : defaultThreads();
        p.language = opts_.language.c_str();
        p.translate = false;
        p.print_progress = false;
        p.print_realtime = false;
        p.print_special = false;
        p.print_timestamps = false;
        p.token_timestamps = true; // DTW times are filled in when the context has dtw_token_timestamps
        p.initial_prompt = prompt.empty() ? nullptr : prompt.c_str();
        p.progress_callback = [](whisper_context*, whisper_state*, int pct, void* ud) {
            auto* c = static_cast<Callbacks*>(ud);
            if (*c->progress) (*c->progress)(pct / 100.0);
        };
        p.progress_callback_user_data = &cb;
        p.encoder_begin_callback = [](whisper_context*, whisper_state*, void* ud) {
            auto* c = static_cast<Callbacks*>(ud);
            return !(*c->cancel && (*c->cancel)());
        };
        p.encoder_begin_callback_user_data = &cb;
        p.abort_callback = [](void* ud) {
            auto* c = static_cast<Callbacks*>(ud);
            return *c->cancel && (*c->cancel)();
        };
        p.abort_callback_user_data = &cb;

        int rc = whisper_full(ctx_, p, pcm, static_cast<int>(n));
        if (cancel && cancel()) {
            error = "cancelled";
            return false;
        }
        if (rc != 0) {
            error = "whisper_full failed (" + std::to_string(rc) + ")";
            return false;
        }

        const whisper_token eot = whisper_token_eot(ctx_);
        const int nseg = whisper_full_n_segments(ctx_);
        Word cur;
        int curTokens = 0;
        double curProb = 0;
        bool dtwMissing = false;
        auto flush = [&]() {
            if (curTokens == 0) return;
            cur.prob = static_cast<float>(curProb / curTokens);
            // strip whitespace
            size_t a = cur.text.find_first_not_of(' ');
            cur.text = a == std::string::npos ? "" : cur.text.substr(a);
            if (!cur.text.empty()) words.push_back(cur);
            cur = Word();
            curTokens = 0;
            curProb = 0;
        };
        for (int s = 0; s < nseg; ++s) {
            const int nt = whisper_full_n_tokens(ctx_, s);
            for (int t = 0; t < nt; ++t) {
                whisper_token_data d = whisper_full_get_token_data(ctx_, s, t);
                if (d.id >= eot) continue; // special/timestamp tokens
                std::string text = whisper_full_get_token_text(ctx_, s, t);
                if (text.empty()) continue;
                if (d.t_dtw < 0) dtwMissing = true;
                double time = timeOffset + std::max<int64_t>(0, d.t_dtw) / 100.0;
                bool startsWord = text[0] == ' ' || curTokens == 0;
                // a token that is only punctuation never starts a new word, even with a leading space
                bool punctOnly = text.find_first_not_of(" .,!?;:\"')]}-") == std::string::npos;
                if (startsWord && !(punctOnly && curTokens > 0)) {
                    flush();
                    cur.start = time;
                }
                cur.text += text;
                cur.end = time;
                curProb += d.p;
                ++curTokens;
            }
            // segment boundaries end words too
            flush();
        }
        flush();
        if (dtwMissing) {
            error = "DTW timestamps missing (flash attention must be off)";
            return false;
        }
        return true;
    }

private:
    whisper_context* ctx_;
    WhisperOptions opts_;
};

/// NVIDIA Parakeet TDT through libparakeet. TDT emits each token with a frame index and a duration
/// (frames of 80 ms: 10 ms mel hop x 8 subsampling), so word times come straight from the decoder; no DTW.
/// Punctuation and casing are produced by the model. Punctuation tokens get the end time (frame + predicted duration)
/// of the preceding token (libparakeet's refine_timestamps_tdt), which usually reaches into the following pause;
/// the segmenter snaps it to the VAD gap as for Whisper.
class ParakeetBackend final : public AsrBackend {
public:
    ParakeetBackend(parakeet_context* ctx, std::string id, WhisperOptions opts)
        : ctx_(ctx), id_(std::move(id)), opts_(std::move(opts)) {}
    ~ParakeetBackend() override { parakeet_free(ctx_); }

    std::string modelName() const override { return id_ + " (NVIDIA Parakeet TDT, whisper.cpp v1.9.6 libparakeet)"; }
    std::string modelId() const override { return id_; }
    std::string modelLicense() const override { return "CC-BY-4.0 (NVIDIA)"; }
    std::string cacheTag() const override { return id_ + "-tdt-v1"; }
    // Full attention is used up to 8192 encoder frames (~10.9 min); 5-minute chunks stay well inside that.
    double chunkSeconds() const override { return 300.0; }

    bool transcribe(const float* pcm, size_t n, double timeOffset, const std::string& /*prompt: unsupported*/,
                    std::vector<Word>& words, const ProgressFn& progress, const CancelFn& cancel,
                    std::string& error) override {
        struct Callbacks {
            const ProgressFn* progress;
            const CancelFn* cancel;
            double nFrames;
        } cb{&progress, &cancel, std::max(1.0, n / 1280.0)}; // encoder frames (80 ms each)

        parakeet_full_params p = parakeet_full_default_params(PARAKEET_SAMPLING_GREEDY);
        p.n_threads = opts_.threads > 0 ? opts_.threads : defaultThreads();
        p.no_context = true;
        // encoder ~ first 40 %, then decoding progress by frame position
        p.progress_callback = [](parakeet_context*, parakeet_state*, int pct, void* ud) {
            auto* c = static_cast<Callbacks*>(ud);
            if (*c->progress) (*c->progress)(0.4 * pct / 100.0);
        };
        p.progress_callback_user_data = &cb;
        p.new_token_callback = [](parakeet_context*, parakeet_state*, const parakeet_token_data* d, void* ud) {
            auto* c = static_cast<Callbacks*>(ud);
            if (*c->progress) (*c->progress)(0.4 + 0.6 * std::min(1.0, d->frame_index / c->nFrames));
        };
        p.new_token_callback_user_data = &cb;
        p.encoder_begin_callback = [](parakeet_context*, parakeet_state*, void* ud) {
            auto* c = static_cast<Callbacks*>(ud);
            return !(*c->cancel && (*c->cancel)());
        };
        p.encoder_begin_callback_user_data = &cb;
        p.abort_callback = [](void* ud) {
            auto* c = static_cast<Callbacks*>(ud);
            return *c->cancel && (*c->cancel)();
        };
        p.abort_callback_user_data = &cb;

        int rc = parakeet_full(ctx_, p, pcm, static_cast<int>(n));
        if (cancel && cancel()) {
            error = "cancelled";
            return false;
        }
        if (rc != 0) {
            error = "parakeet_full failed (" + std::to_string(rc) + ")";
            return false;
        }

        std::vector<AsrToken> toks;
        const int nseg = parakeet_full_n_segments(ctx_);
        char buf[256];
        for (int s = 0; s < nseg; ++s) {
            const int nt = parakeet_full_n_tokens(ctx_, s);
            for (int t = 0; t < nt; ++t) {
                parakeet_token_data d = parakeet_full_get_token_data(ctx_, s, t);
                const char* piece = parakeet_token_to_str(ctx_, d.id);
                if (!piece) continue;
                // is_first=false keeps the leading space of word-start pieces, which groupTokensIntoWords uses
                parakeet_token_to_text(piece, false, buf, sizeof buf);
                AsrToken tok;
                tok.text = buf;
                if (tok.text.empty()) continue;
                tok.start = timeOffset + d.t0 / 100.0; // t0/t1 are 10 ms mel frames
                tok.end = timeOffset + d.t1 / 100.0;
                tok.prob = d.p;
                toks.push_back(std::move(tok));
            }
        }
        std::vector<Word> ws = groupTokensIntoWords(toks);
        words.insert(words.end(), ws.begin(), ws.end());
        if (*cb.progress) (*cb.progress)(1.0);
        return true;
    }

private:
    parakeet_context* ctx_;
    std::string id_;
    WhisperOptions opts_;
};

} // namespace

std::vector<Word> groupTokensIntoWords(const std::vector<AsrToken>& tokens) {
    std::vector<Word> out;
    Word cur;
    int n = 0;
    double probSum = 0;
    bool pendingSpace = false;
    auto flush = [&]() {
        if (n == 0) return;
        size_t a = cur.text.find_first_not_of(' ');
        cur.text = a == std::string::npos ? "" : cur.text.substr(a);
        cur.prob = static_cast<float>(probSum / n);
        if (!cur.text.empty()) out.push_back(cur);
        cur = Word();
        n = 0;
        probSum = 0;
    };
    for (auto& t : tokens) {
        const size_t firstChar = t.text.find_first_not_of(' ');
        if (firstChar == std::string::npos) {
            // a bare word-boundary piece ("▁" alone): the next piece starts a word; no text of its own
            pendingSpace = true;
            continue;
        }
        bool startsWord = t.text[0] == ' ' || pendingSpace || n == 0;
        pendingSpace = false;
        bool punctOnly = t.text.find_first_not_of(" .,!?;:\"')]}-") == std::string::npos;
        if (startsWord && !(punctOnly && n > 0)) {
            flush();
            cur.start = t.start;
        }
        // a punctuation piece that came with its own space ("▁,") attaches without the space
        cur.text += (punctOnly && n > 0) ? t.text.substr(firstChar) : t.text;
        cur.end = std::max(cur.end, t.end);
        probSum += t.prob;
        ++n;
    }
    flush();
    return out;
}

void setWhisperLogging(bool enabled) {
    gLogging = enabled;
    whisper_log_set(logCallback, nullptr);
    ggml_log_set(logCallback, nullptr);
    parakeet_log_set(logCallback, nullptr);
}

std::unique_ptr<AsrBackend> makeParakeetBackend(const std::string& modelPath, const std::string& modelId,
                                                const WhisperOptions& opts, std::string& error) {
    parakeet_context_params cp = parakeet_context_default_params();
    cp.use_gpu = opts.useGpu;
    parakeet_context* ctx = parakeet_init_from_file_with_params(modelPath.c_str(), cp);
    if (!ctx) {
        error = "could not load Parakeet model at " + modelPath;
        return nullptr;
    }
    return std::make_unique<ParakeetBackend>(ctx, modelId, opts);
}

std::unique_ptr<AsrBackend> makeAsrBackend(const std::string& modelId, const std::string& modelPath,
                                           std::string& error) {
    if (modelId == kWhisperTurboId) return makeWhisperTurboBackend(modelPath, WhisperOptions{}, error);
    if (modelId == kParakeetV2Id || modelId == kParakeetV3Id)
        return makeParakeetBackend(modelPath, modelId, WhisperOptions{}, error);
    error = "unknown ASR model '" + modelId + "'";
    return nullptr;
}

std::unique_ptr<AsrBackend> makeWhisperTurboBackend(const std::string& modelPath, const WhisperOptions& opts,
                                                    std::string& error) {
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = opts.useGpu;
    cp.flash_attn = false; // DTW silently returns t_dtw = -1 with flash attention on
    cp.dtw_token_timestamps = true;
    cp.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3_TURBO;
    whisper_context* ctx = whisper_init_from_file_with_params(modelPath.c_str(), cp);
    if (!ctx) {
        error = "could not load Whisper model at " + modelPath;
        return nullptr;
    }
    return std::make_unique<WhisperTurboBackend>(ctx, opts);
}

SileroVad::~SileroVad() {
    if (ctx_) whisper_vad_free(ctx_);
}

std::unique_ptr<SileroVad> SileroVad::load(const std::string& modelPath, std::string& error) {
    whisper_vad_context_params cp = whisper_vad_default_context_params();
    cp.n_threads = 4;
    cp.use_gpu = false;
    whisper_vad_context* ctx = whisper_vad_init_from_file_with_params(modelPath.c_str(), cp);
    if (!ctx) {
        error = "could not load Silero VAD model at " + modelPath;
        return nullptr;
    }
    std::unique_ptr<SileroVad> v(new SileroVad());
    v->ctx_ = ctx;
    return v;
}

bool SileroVad::detect(const float* pcm, size_t n, std::vector<Interval>& speech, std::string& error) {
    whisper_vad_params p = whisper_vad_default_params(); // threshold 0.5, min speech 250 ms, min silence 100 ms, pad 30 ms
    whisper_vad_segments* segs = whisper_vad_segments_from_samples(ctx_, p, pcm, static_cast<int>(n));
    if (!segs) {
        error = "VAD failed";
        return false;
    }
    int ns = whisper_vad_segments_n_segments(segs);
    speech.clear();
    for (int i = 0; i < ns; ++i) {
        Interval iv;
        iv.start = whisper_vad_segments_get_segment_t0(segs, i) / 100.0; // centiseconds -> seconds
        iv.end = whisper_vad_segments_get_segment_t1(segs, i) / 100.0;
        speech.push_back(iv);
    }
    whisper_vad_free_segments(segs);
    return true;
}

} // namespace ttsdc
