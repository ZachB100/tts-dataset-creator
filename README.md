# TTS Dataset Creator

A native macOS app (Apple Silicon, macOS 14+) that turns single-speaker recordings into
[LJSpeech](https://keithito.com/LJ-Speech-Dataset/)-format TTS datasets: pick files, transcribe, review, export.
Everything runs locally: Whisper large-v3-turbo (default) or NVIDIA Parakeet TDT through
[whisper.cpp](https://github.com/ggml-org/whisper.cpp) on Metal, plus Silero VAD for finding pauses.

```
dataset/
  wavs/<source>_0001.wav     mono 16-bit, 22.05 kHz by default
  metadata.csv               id|normalized   (or id|raw|normalized)
  dataset_info.json          model, sample rate, settings
```

## How it works

1. **Decode** (AVFoundation: WAV, FLAC, MP3, M4A, AIFF, CAF), mix to mono, resample to 16 kHz for ASR.
2. **VAD**: Silero finds speech and pauses.
3. **Transcribe** with `whisper_full()` on large-v3-turbo with DTW token timestamps
   (`dtw_aheads_preset = LARGE_V3_TURBO`, flash attention off; DTW returns -1 with it on).
   Or, if picked in Settings (or `ttsdc --model`), with **NVIDIA Parakeet TDT 0.6B** v2 (English) or v3
   (multilingual) through whisper.cpp's `libparakeet`. Parakeet writes punctuation and casing itself, and its
   word times come from the TDT decoder (80 ms frames). A sentence-final `.` gets the end of the last spoken token's
   predicted duration, which usually reaches a little way into the pause (median about 0.3 s after speech offset on
   the test narration); the VAD snapping below works unchanged.
   Audio is processed in roughly 5-minute chunks split at long pauses. Each finished chunk is saved to a hidden
   cache keyed by the file's SHA-256 plus the model (so switching models never reuses another model's
   transcript), and a crash or cancel resumes where it stopped. The cache is cleared after a successful export.
4. **Cut**: split at `.` `?` `!` (not at abbreviation periods such as Dr. or U.S.). Take the sentence end from the
   trailing punctuation token's DTW time, snap it to the nearest VAD pause, then cut at the quietest 10 ms frame in
   that pause. Each clip is trimmed to its speech plus 100–200 ms of padding and gets 10 ms fades at export.
5. **Normalize** English text: numbers in context (cardinals, years, money, %, ordinals, dates, times, decimals,
   phone numbers), abbreviations and acronyms through an editable pronunciation dictionary, unknown ALL-CAPS words
   spelled out. Sentence casing and punctuation are kept.
6. **Flag** segments for review: too short or long, a cut that isn't in silence, no pause near the sentence end,
   possibly dropped words (more speech than the words account for), ambiguous numbers (`1/2`, a bare `2000`),
   spelled-out acronyms, a standalone letter "a", and leftover digits.

The reasoning behind these choices is in `spike/FINDINGS.md`.

## Build from source

You need the Xcode **Command Line Tools** (not Xcode) and CMake:

```bash
xcode-select --install
```

```bash
brew install cmake
```

Build the app; the script builds whisper.cpp first if needed:

```bash
scripts/bundle_app.sh
```

```bash
open "build/TTS Dataset Creator.app"
```

On first launch the app downloads the selected transcription model and `ggml-silero-v6.2.0.bin` from Hugging Face
into `~/Library/Application Support/TTSDatasetCreator/models/`. Files already there are reused.

| Model (`--model` id) | File | Source | Weights licence |
|---|---|---|---|
| `whisper-large-v3-turbo` (default) | `ggml-large-v3-turbo.bin`, 1.6 GB | `ggerganov/whisper.cpp` | MIT (OpenAI) |
| `parakeet-tdt-0.6b-v2` (English) | `ggml-parakeet-tdt-0.6b-v2-f16.bin`, 1.2 GB | `JoaoZaokk/parakeet-tdt-0.6b-v2-ggml` | CC-BY-4.0 (NVIDIA) |
| `parakeet-tdt-0.6b-v3` (25 languages) | `ggml-parakeet-tdt-0.6b-v3-f16.bin`, 1.3 GB | `ggml-org/parakeet-GGUF` | CC-BY-4.0 (NVIDIA) |

**Parakeet licence:** NVIDIA releases both checkpoints under CC-BY-4.0, so commercial use, including datasets
and voices built from the transcripts, is allowed **with attribution** (credit NVIDIA's `parakeet-tdt-0.6b-v2`/`-v3`).
`dataset_info.json` records `asr_model`, `asr_model_id` and `asr_model_license`.

**Where the Parakeet files come from:** NVIDIA publishes only `.nemo` checkpoints. ggml-org hosts an official GGML
conversion of v3 only. The v2 file is a third-party conversion. Its v3 conversion is byte-identical to ggml-org's,
which suggests the same converter, but the v2 file itself has not been reproduced from NVIDIA's `.nemo`. Both
downloads are pinned by SHA-256 in `ModelManager.swift` and checked after download. To produce v2 yourself, run
upstream whisper.cpp's `models/convert-parakeet-to-ggml.py` (needs PyTorch) on `nvidia/parakeet-tdt-0.6b-v2`.

The vendored `libparakeet` supports only the **TDT** (token-and-duration transducer) architecture. CTC/RNNT-only
Parakeet variants and Canary aren't supported.

Run the core unit tests:

```bash
scripts/test.sh
```

### Command-line use

The same core is exposed as `ttsdc` (also inside the app bundle at `Contents/MacOS/ttsdc`):

```bash
swift build -c release --product ttsdc
```

```bash
.build/release/ttsdc -o ~/datasets/narrator --three-columns --report report.tsv recording1.mp3 recording2.wav
```

Run `ttsdc --help` for all options (sample rate, length limits, lowercase, drop flagged, padding).
Pick the transcription model with `--model parakeet-tdt-0.6b-v2` (see the table above); `--download` fetches
missing model files first.
Ctrl-C cancels; re-running the same command resumes from the cache.

## Project layout

| Path | What |
|---|---|
| `third_party/whisper.cpp` | upstream whisper.cpp v1.9.6, vendored at `5eaee0cc9024eb9e9a99469199920c8b7f5cb9ec` (MIT) |
| `scripts/build_whisper.sh` | CMake build of whisper.cpp + ggml as static libs (Metal, `GGML_METAL_EMBED_LIBRARY=ON`) |
| `Sources/TTSCore` | C++ core: ASR backends (Whisper turbo, Parakeet), VAD wrapper, cutter, normalizer, resampler, LJSpeech exporter |
| `Sources/TTSKit` | Swift layer shared by app and CLI: AVFoundation decoding, model downloads, C++ bridge |
| `Sources/TTSDatasetCreator` | SwiftUI app |
| `Sources/ttsdc` | command-line front end |
| `Tests/TTSCoreTests` | C++ unit tests (normalization, sentence splitting, cutting, resampling, export) |

### Build notes (Command Line Tools only, no Xcode)

- **whisper.cpp is built with CMake, not as SwiftPM targets.** ggml's Metal backend embeds its shader source
  through a CMake-generated assembly step, and its CPU backend picks per-architecture sources and flags. Rebuilding
  that in `Package.swift` would mean tracking upstream's CMake logic by hand. The script produces static `.a` files
  that `Package.swift` links with `-L build/whisper/install/lib`. CPU code targets
  `armv8.4-a+dotprod+fp16`, so builds run on every Apple Silicon Mac. Set `REBUILD_WHISPER=1` to force a rebuild.
- **Unit tests are a C++ executable**, not swift-testing: the `@Test` macro plugin doesn't load with the
  Command Line Tools.
- **No CxxStdlib overlay.** The Command Line Tools ship without Swift's `CxxStdlib` module, so Swift can't convert
  `std::string` directly. `Sources/TTSCore/include/Bridge.hpp` and `Sources/TTSKit/CxxHelpers.swift` provide the
  conversions.
- **No `@State`.** In the macOS 27 SDK, SwiftUI's `@State` is a macro whose plugin ships only with Xcode. Views keep
  local state in a small `@StateObject` box (`StateBox.swift`) instead.
- The app is ad-hoc signed (`codesign -s -`). Downloaded copies will need right-click → Open the first time
  (they aren't notarized).

## Licences

MIT (see `LICENSE`). whisper.cpp/ggml (including libparakeet) and nlohmann/json are MIT; their notices ship in the app bundle under
`Contents/Resources/Licenses`. Whisper large-v3-turbo weights are MIT (OpenAI); Silero VAD is MIT. NVIDIA Parakeet
TDT 0.6B v2/v3 weights are CC-BY-4.0 (attribution required); they aren't bundled and are downloaded only if selected.
