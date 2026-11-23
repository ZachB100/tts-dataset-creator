# TTS Dataset Creator

A native macOS app (Apple Silicon, macOS 14+) that turns single-speaker recordings into
[LJSpeech](https://keithito.com/LJ-Speech-Dataset/)-format TTS datasets: pick files, transcribe, review, export.
Everything runs locally: Whisper large-v3-turbo through [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
on Metal, plus Silero VAD for finding pauses.

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
   Audio is processed in roughly 5-minute chunks split at long pauses. Each finished chunk is saved to a hidden
   cache keyed by the file's SHA-256, so a crash or cancel resumes where it stopped.
   The cache is cleared after a successful export.
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

On first launch the app downloads `ggml-large-v3-turbo.bin` (1.6 GB) and `ggml-silero-v6.2.0.bin` from Hugging Face
into `~/Library/Application Support/TTSDatasetCreator/models/`. Files already there are reused.

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
Ctrl-C cancels; re-running the same command resumes from the cache.

## Project layout

| Path | What |
|---|---|
| `third_party/whisper.cpp` | upstream whisper.cpp v1.9.6, vendored at `5eaee0cc9024eb9e9a99469199920c8b7f5cb9ec` (MIT) |
| `scripts/build_whisper.sh` | CMake build of whisper.cpp + ggml as static libs (Metal, `GGML_METAL_EMBED_LIBRARY=ON`) |
| `Sources/TTSCore` | C++ core: ASR/VAD wrappers, cutter, normalizer, resampler, LJSpeech exporter |
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

MIT (see `LICENSE`). whisper.cpp/ggml and nlohmann/json are MIT; their notices ship in the app bundle under
`Contents/Resources/Licenses`. Whisper large-v3-turbo weights are MIT (OpenAI); Silero VAD is MIT.
