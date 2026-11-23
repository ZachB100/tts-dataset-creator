# Phase 1 spike — findings

Machine: Apple M1 Pro, 16 GB, macOS 27.2. Date: 2026-11-22.
Test audio: two 5-min excerpts (60–360 s) of `testdata/professional/1.mp3` and `2.mp3`, converted to 16 kHz mono with `afconvert`, plus all of `1.mp3` (11.5 min) for scaling. Clean narration with a digital-silence floor (−82 dBFS) and **no fillers at all**, so this test data can't settle the disfluency question (see Blockers).

## TL;DR

- **One whisper.cpp, no symbol clash.** CrisperWhisper.cpp isn't a real fork. It's a ~3 kLOC wrapper (own decoder loop, prompts, long-form stitching, supervised aligner) that statically links **upstream whisper.cpp** pinned at `080bbbe` plus one **additive** 140-line patch (`patches/whisper-cross-attention.patch`, which only adds new functions). The patch also applies cleanly to whisper.cpp HEAD. Build a single patched `libwhisper`/ggml and run both backends through it:
  - CrisperWhisper models go through the `crisperwhisper::` C++ API (compile its `src/*.cpp` into the core).
  - Stock large-v3-turbo goes through plain `whisper_full()` with built-in DTW word timing.
  - No symbol prefixing, no separate dylibs, no helper process.
- **The CrisperWhisper wrapper itself can't run stock turbo.** It refuses at load because the model vocabulary lacks the `[verbatim_N]`/`<vtx>` tokens. Stock turbo on the *patched* lib behaves exactly like upstream (identical timestamp metrics, below). So "use the fork alone" holds at the library level, not the CLI/API level: our core needs two thin code paths over one lib.
- **Best timestamps for cutting: stock turbo + DTW** (`dtw_aheads_preset = LARGE_V3_TURBO`, flash-attn off). 95–100% of sentence ends land in the real pause. CrisperWhisper large's supervised timestamps are tighter at the median but have 0.3–2.3 s outliers.
- **Silero VAD (whisper.cpp built-in) is usable.** It found 100% of pauses ≥ 250 ms, with boundaries within ±30–60 ms (p90). Recommended cut algorithm: ASR picks the sentence end, snap to the Silero VAD gap, then cut at the quietest RMS frame in that gap.
- **Speed is well inside the 10–20 min/hour target:** CrisperWhisper large ≈ 10 min/h, turbo + DTW ≈ 4.6 min/h.
- **License blocker (CrisperWhisper):** its outputs, and any model trained on datasets that contain them, are non-commercial and can't be released under permissive licenses (details below).
- **Full Xcode is not installed** (Command Line Tools only). This spike didn't need it, but the app will.

## 1. Build

Toolchain: Apple clang 21.0.0 (CLT), CMake 4.5.0. **Full Xcode is missing**: `xcodebuild` errors with "requires Xcode, but active developer directory '/Library/Developer/CommandLineTools'". That didn't matter here, because Metal shaders are embedded and compiled at runtime (`GGML_METAL_EMBED_LIBRARY=ON`), so no `metal` compiler was needed. You'll need Xcode for the SwiftUI app and Swift/C++ interop.

| Repo (in `spike/third_party/`) | Pinned commit | Date |
|---|---|---|
| CrisperWhisper.cpp | `9478d8364094a6bceb3c39b2f4486754ef6b86f5` (v1.2.0+) | 2026-08-14 |
| └ whisper.cpp it fetches + patches | `080bbbe85230f624f0b52127f1ae1218247989f9` | 2026-07-11 |
| whisper.cpp (upstream) | `5eaee0cc9024eb9e9a99469199920c8b7f5cb9ec` (v1.9.6) | 2026-11-20 |

```sh
# CrisperWhisper.cpp (FetchContent pulls whisper.cpp@080bbbe and applies the patch)
cmake -S CrisperWhisper.cpp -B CrisperWhisper.cpp/build -DCMAKE_BUILD_TYPE=Release \
  -DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON -DCRISPERWHISPER_CPU_PROFILE=portable
cmake --build CrisperWhisper.cpp/build -j 8        # -> build/bin/crisper-whisper
# upstream whisper.cpp
cmake -S whisper.cpp -B whisper.cpp/build -DCMAKE_BUILD_TYPE=Release -DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON
cmake --build whisper.cpp/build -j 8 --target whisper-cli whisper-vad-speech-segments
# linking proof: stock whisper-cli built from the *patched* whisper.cpp source
cmake -S CrisperWhisper.cpp/build/_deps/whisper_cpp-src -B build-patched-whisper -DCMAKE_BUILD_TYPE=Release \
  -DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON -DBUILD_SHARED_LIBS=OFF
cmake --build build-patched-whisper -j 8 --target whisper-cli
# patch vs latest upstream
git -C whisper.cpp apply --check ../CrisperWhisper.cpp/patches/whisper-cross-attention.patch   # OK
```

All builds succeeded with no source changes. Logs report `using embedded metal library`, and every run used the GPU. Notes:
- The CrisperWhisper docs only list Windows/Linux/CUDA, but macOS/Metal works.
- `CRISPERWHISPER_CPU_PROFILE=portable` avoids its x86 SIMD flags. On arm64 they're harmless anyway.

## 2. Models

In `~/Library/Application Support/TTSDatasetCreator/models/`. Neither HF repo is gated and no login is needed.

| File | Bytes | Source |
|---|---:|---|
| `ggml-crisperwhisper-large-f16.bin` | 3,094,723,598 | drbaph/CrisperWhisper2.0-GGML |
| `ggml-crisperwhisper-large-f16.bin.metadata.json` | 1,444 | same (alignment heads and suppress list; required next to the .bin) |
| `ggml-crisperwhisper-turbo-f16.bin` (+ `.metadata.json`, 1,436) | 1,624,655,191 | same |
| `ggml-large-v3-turbo.bin` | 1,624,555,275 | ggerganov/whisper.cpp |
| `ggml-silero-v6.2.0.bin` | 885,098 | ggml-org/whisper-vad |

**CrisperWhisper license** (Nyra Health Non-Commercial Research License, Part B; the code is MIT): weights *and Outputs* (transcripts, timestamps) are Non-Commercial Use only. §3.1 makes "any model trained … on Outputs — including any model trained on datasets that incorporate Outputs" a Derivative Work. §3.2 forbids releasing those under MIT/Apache or any commercial-permitting license. **So a TTS voice trained on a CrisperWhisper-made dataset inherits the non-commercial restriction.** The app must say this plainly when the CrisperWhisper backend is chosen. Turbo (MIT) has no such restriction.

## 3. Timestamp quality

Method (`scripts/analyze.py`): 10 ms RMS frames, with a pause defined as a silence run ≥ 100 ms. For every sentence-final word (`.?!`), I check where its end time falls relative to the nearest pause. Two silence thresholds:
- **strict:** floor + 25% of range ≈ −64 dBFS, which counts the low decay tail as speech
- **loose:** −45 dBFS

"In pause" allows the end to fall up to 50 ms before pause start. "Quietest-point OK" means the quietest frame between the word's end and the next word's start is below threshold, i.e. what a naive cutter would hit.

| Backend (5-min excerpt) | Sentence ends | End in pause, strict | End in pause, loose | Early (in speech), loose | Quietest-point OK (strict) | Median end − pause start (loose) |
|---|---|---|---|---|---|---|
| CW large, verbatim, `--word-timestamps`, ex1 | 43 | 27.9% | 46.5% | 48.8% (median 310 ms) | 93.0% | −40 ms |
| CW large, ex2 | 35 | 28.6% | 65.7% | 22.9% (290 ms) | 91.4% | +20 ms |
| Stock turbo + DTW ex1 | 41 | **95.1%** | **97.6%** | 0% | 97.6% | +220 ms |
| Stock turbo + DTW ex2 | 35 | **97.1%** | **100%** | 0% | 100% | +270 ms |
| Stock turbo, segment times (`-ml 1 -sow`, no DTW) ex1 / ex2 | 41 / 35 | 0% / 42.9% | 7.3% / 57.1% | 93% / 40% | 100% / 68.6% | −120 / +20 ms |
| Stock turbo + DTW on **patched** lib ex1 / ex2 | 41 / 35 | 95.1% / 97.1% | 97.6% / 100% | 0% | 97.6% / 100% | identical to upstream |

Reading the results:
- **CW large** is precise at the median, often within ±40 ms of speech offset. But about 25–50% of its sentence ends sit 0.3 s or more before the real offset, and some are 1.6–2.3 s off: compared with DTW, 25 of 35 matched ends in ex1 differ by more than 0.3 s.
- **Turbo DTW end** (taken as the onset of the trailing `.` token) lands reliably *inside* the pause, about 120–270 ms after speech offset. That's exactly what pause selection needs.
- **Plain whisper segment timestamps** (no DTW) are unusable for cutting.

**VAD snapping** (`scripts/vad_snap*.py`): snapping each sentence end to the nearest Silero gap picks the right pause 100% of the time for turbo + DTW. For CW large it agrees with turbo + DTW on 91–94% of sentence ends (nearest-gap or longest-gap-in-window heuristic). The rest are CW alignment outliers, plus 2–8 extra sentence splits.

**Fillers/disfluencies:** zero fillers, repeats or fragments in either backend; this narration has none (`scripts/diff_words.py`). Word-level agreement CW large vs turbo: 98.0% / 97.7%. The differences are informative for later phases:
- **Spoken-form numbers.** CW verbatim writes "forty-three percent", "two thousand eight", "nine to five", "u k", "moneysupermarket dot com". Turbo writes "43", "2008", "**95**" (wrong for "nine to five"), "UK". CW output needs much less number normalization, which also lowers risk.
- **Dropped phrases.** Turbo dropped "and depending on the skills required it can be surprisingly well paid" (ex1) and "this way" (ex2). CW large dropped nothing measurable.
- **CW turbo is unusable as-is.** Its 30 s continuation chunks hit early EOT and drop 30% of words on ex1 (509 vs 752 words, seven gaps of 10–20 s). Not investigated further (`--longform-strategy chunked-lcs` may help).

## 4. Speed (Metal, wall-clock including model load)

| Backend | 5-min excerpts (s, ex1/ex2) | Full 11.5 min | Sec per audio min | ≈ min per audio hour |
|---|---|---|---|---|
| CW large, verbatim + word timestamps | 63.7 / 53.4 | 113.5 s | 9.9–11.7 | **10–12** |
| CW turbo (drops text) | 27.4 / 27.7 | – | 5.5 | 5.5 |
| Stock turbo, flash-attn, no DTW | 20.3 / 17.7 | – | 3.8 | 3.8 |
| Stock turbo, `-nfa` + DTW | 25.4 / 22.6 | 52.2 s | 4.6–4.8 | **4.6** |
| Stock turbo + DTW, patched lib | 27.2 / 26.0 | – | 5.3 | 5.3 |
| Silero VAD (CPU) | 7.5 | – | 1.5 | 1.5 |

Time scales linearly with length. Even CW large plus VAD (about 12 min/h) leaves headroom inside the 10–20 min/h target on an M1 Pro.

## 5. Recommendation: backend and linking

1. **Vendor one whisper.cpp** (pin to a recent upstream commit, currently `5eaee0c`) with `whisper-cross-attention.patch` applied at build time. Build it, ggml and Metal as **static** libraries into the C++ core.
2. **Compile CrisperWhisper.cpp's `src/{crisperwhisper,longform,word_timing,model_metadata,audio}.cpp`** (MIT) into the same core for the CW backend. Don't use its CMake FetchContent; point it at our vendored whisper.
3. **Turbo backend:** call `whisper_full()` directly with these settings:
   - `token_timestamps=true` and `dtw_token_timestamps=true`, `dtw_aheads_preset=WHISPER_AHEADS_LARGE_V3_TURBO`
   - `flash_attn=false`, because DTW silently returns `t_dtw=-1` with flash-attn on
   - the sentence end comes from the trailing punctuation token's `t_dtw`
4. **Rejected:** symbol prefixing, two dylibs and a helper process all solve a duplicate-symbol problem we don't have.
5. **Default backend:** Whisper turbo (commercial-safe, faster, best cut timing). CW large is the opt-in "verbatim" backend, behind a clear licence warning.
6. **Optional hybrid, to test in Phase 2:** CW text plus turbo-DTW or VAD timing.

## 6. VAD

whisper.cpp's Silero (`whisper_vad_*` API, and `whisper_full_params.vad` for VAD-gated decoding) runs on the same lib, so the CW build has it too. On both excerpts:
- **Coverage:** 57/57 RMS pauses ≥ 250 ms matched a VAD gap.
- **Accuracy:** speech-end boundary median 0 / −10 ms, p90 50–60 ms; speech-start median +10 ms, p90 30 ms. Silero's 32 ms frame resolution is good enough to choose the pause, but not to place the cut.
- **Cost:** 1.5 s per audio minute on CPU.

**Use it for gap selection; place the actual cut at the quietest 10 ms RMS frame inside the VAD gap, then apply the 100–200 ms padding.** The CW CLI has no VAD or timestamp tokens; it uses fixed 30 s/26 s windows. Call the Silero API separately rather than through CW.

## Blockers / open items

- **Install full Xcode** before Phase 2 (app, Swift/C++ interop, signing).
- **The CrisperWhisper license makes CW-built datasets, and voices trained on them, non-commercial.** You need to decide whether CW stays in an MIT app as an opt-in backend.
- **No conversational test audio.** The filler/disfluency difference (the whole point of CW) is untested. Please add a few minutes of unscripted speech to `testdata/conversational/`.
- **CW alignment outliers.** Roughly 6–9% of CW sentence ends pick a different pause than turbo + DTW. That threatens the 95% no-adjustment goal unless the hybrid or a better gap heuristic fixes it.
- **The CW turbo model drops ~30% of text** with the default long-form strategy.
- **The metrics are energy-based** on studio audio with a digital-silence floor. There was no listening test and no hand-labelled ground truth; noisier recordings need re-testing.

Reproduce with `spike/scripts/run_all.sh`, `analyze.py`, `vad_eval.py`, `vad_snap.py`, `vad_snap2.py` and `diff_words.py`. Raw outputs are in `spike/work/` (git-ignored, contains audio-derived WAVs).
