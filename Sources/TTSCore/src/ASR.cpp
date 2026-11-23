#include "ASR.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

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

} // namespace

void setWhisperLogging(bool enabled) {
    gLogging = enabled;
    whisper_log_set(logCallback, nullptr);
    ggml_log_set(logCallback, nullptr);
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
