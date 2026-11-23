// ttsdc — headless front end over the same core as the app.
//   ttsdc -o OUT_DIR [options] AUDIO...
import Foundation
import TTSKit

setvbuf(stdout, nil, _IOLBF, 0)

/// Thread-safe Bool for the progress ticker.
final class Flag: @unchecked Sendable {
    private let lock = NSLock()
    private var v = false
    var value: Bool {
        get { lock.lock(); defer { lock.unlock() }; return v }
        set { lock.lock(); v = newValue; lock.unlock() }
    }
}

/// Monotonic counter for download progress lines.
final class Counter: @unchecked Sendable {
    private let lock = NSLock()
    private var v = -1
    /// True when `n` is greater than the last value seen.
    func advance(to n: Int) -> Bool {
        lock.lock(); defer { lock.unlock() }
        if n > v { v = n; return true }
        return false
    }
}

final class ErrorBox: @unchecked Sendable {
    var error: Error?
}

func usage() -> Never {
    print("""
    usage: ttsdc -o OUT_DIR [options] AUDIO...

    Turns single-speaker recordings into an LJSpeech dataset (wavs/, metadata.csv, dataset_info.json).

    options:
      -o, --out DIR        output folder (created)
      --rate N             export sample rate: 16000, 22050 (default), 24000, 44100, 48000
      --three-columns      metadata.csv as id|raw|normalized (default id|normalized)
      --drop-flagged       leave flagged segments out of the export
      --lowercase          lowercase all normalized text
      --min S / --max S    length limits in seconds for flagging (default 1 / 15)
      --padding S          silence kept around speech, 0.10–0.20 (default 0.15)
      --model ID           transcription model (default whisper-large-v3-turbo):
                             whisper-large-v3-turbo   Whisper large-v3-turbo, DTW timestamps (MIT)
                             parakeet-tdt-0.6b-v2     NVIDIA Parakeet TDT 0.6B v2, English (CC-BY-4.0)
                             parakeet-tdt-0.6b-v3     NVIDIA Parakeet TDT 0.6B v3, multilingual (CC-BY-4.0)
      --download           download missing model files from Hugging Face first
      --models DIR         model folder (default ~/Library/Application Support/TTSDatasetCreator/models)
      --keep-cache         don't clear the transcript cache after export
      --report FILE        write a TSV of every segment with its flags and cut levels
    """)
    exit(2)
}

var args = Array(CommandLine.arguments.dropFirst())
var outDir: URL?
var exportSettings = ExportSettings()
var cut = CutSettings()
var modelsDir = AppPaths.models
var keepCache = false
var reportPath: URL?
var asr = AsrModel.default
var download = false
var inputs: [URL] = []

func value(_ i: inout Int) -> String {
    i += 1
    guard i < args.count else { usage() }
    return args[i]
}

var i = 0
while i < args.count {
    let a = args[i]
    switch a {
    case "-o", "--out": outDir = URL(fileURLWithPath: value(&i))
    case "--rate": exportSettings.sampleRate = Int(value(&i)) ?? 0
    case "--three-columns": exportSettings.threeColumns = true
    case "--drop-flagged": exportSettings.dropFlagged = true
    case "--lowercase": exportSettings.lowercaseAll = true
    case "--min": cut.minSeconds = Double(value(&i)) ?? 1
    case "--max": cut.maxSeconds = Double(value(&i)) ?? 15
    case "--padding": cut.paddingSeconds = Double(value(&i)) ?? 0.15
    case "--models": modelsDir = URL(fileURLWithPath: value(&i))
    case "--model":
        let id = value(&i)
        guard let m = AsrModel.find(id) else {
            print("unknown --model \(id); choose one of: \(AsrModel.all.map(\.id).joined(separator: ", "))")
            exit(2)
        }
        asr = m
    case "--download": download = true
    case "--keep-cache": keepCache = true
    case "--report": reportPath = URL(fileURLWithPath: value(&i))
    case "-h", "--help": usage()
    default:
        if a.hasPrefix("-") { print("unknown option \(a)"); usage() }
        inputs.append(URL(fileURLWithPath: a))
    }
    i += 1
}
guard let outDir, !inputs.isEmpty else { usage() }
guard ExportSettings.sampleRates.contains(exportSettings.sampleRate) else {
    print("unsupported --rate \(exportSettings.sampleRate)")
    exit(2)
}
exportSettings.minSeconds = cut.minSeconds
exportSettings.maxSeconds = cut.maxSeconds

if download {
    guard modelsDir.standardizedFileURL == AppPaths.models.standardizedFileURL else {
        print("--download only writes to the default model folder; drop --models")
        exit(2)
    }
    for spec in ModelSpec.required(for: asr) where !spec.isInstalled {
        print("downloading \(spec.label) from \(spec.url.host ?? "")…")
        let done = DispatchSemaphore(value: 0)
        let decile = Counter()
        let d = ModelDownloader(spec: spec) { p in
            if decile.advance(to: Int(p * 10)) { print(String(format: "  %3.0f%%", p * 100)) }
        }
        let failure = ErrorBox()
        Task {
            do { try await d.run() } catch { failure.error = error }
            done.signal()
        }
        done.wait()
        if let e = failure.error { print("download failed: \(e.localizedDescription)"); exit(1) }
        print("  saved \(spec.localURL.path)")
    }
}

print("ASR model: \(asr.name). Licence: \(asr.license)")
let asrURL = modelsDir.appendingPathComponent(asr.spec.fileName)
let vadURL = modelsDir.appendingPathComponent(ModelSpec.silero.fileName)
for m in [asrURL, vadURL] where !FileManager.default.fileExists(atPath: m.path) {
    print("missing model \(m.path) — re-run with --download, or open the app once to download it")
    exit(1)
}

let t0 = Date()
var engineRef: CoreEngine?
do { engineRef = try CoreEngine(asr: asr, asrModelFile: asrURL, vadModel: vadURL) } catch {
    print(error.localizedDescription)
    exit(1)
}
var engine: CoreEngine { engineRef! }

/// Free the models before exiting: ggml's Metal device asserts if its buffers outlive static teardown.
func quit(_ code: Int32) -> Never {
    engineRef = nil
    exit(code)
}
print(String(format: "models loaded in %.1f s (%@)", Date().timeIntervalSince(t0), engine.modelName))

// Ctrl-C cancels the current file; the transcript cache keeps finished chunks for the next run.
signal(SIGINT, SIG_IGN)
let sigint = DispatchSource.makeSignalSource(signal: SIGINT, queue: .global())
sigint.setEventHandler {
    print("\ncancelling… (finished chunks stay cached; re-run to resume)")
    engineRef?.cancel()
}
sigint.resume()

var exportInputs: [ExportInput] = []
var cacheKeys: [String] = []
var report = "file\tid\tstart\tend\tduration\tstart_db\tend_db\tflags\traw\tnormalized\n"

for url in inputs {
    let tf = Date()
    let audio: DecodedAudio
    do { audio = try AudioDecoder.decode(url) } catch {
        print("\(url.lastPathComponent): \(error.localizedDescription)")
        quit(1)
    }
    print(String(format: "%@: %.1f min at %.0f Hz, decoded in %.1f s", url.lastPathComponent, audio.duration / 60,
                 audio.sampleRate, Date().timeIntervalSince(tf)))
    let done = DispatchSemaphore(value: 0)
    let ticker = DispatchQueue.global()
    let finished = Flag()
    ticker.async {
        var last = ""
        while !finished.value {
            let line = String(format: "  %@ %3.0f%%", engine.stage.label, engine.progress * 100)
            if line != last { print(line); last = line }
            Thread.sleep(forTimeInterval: 2)
        }
        done.signal()
    }
    let tp = Date()
    let result: ProcessedFile
    do { result = try engine.process(audio, cut: cut, lowercaseAll: exportSettings.lowercaseAll) } catch {
        finished.value = true
        done.wait()
        print("\(url.lastPathComponent): \(error.localizedDescription)")
        quit(1)
    }
    finished.value = true
    done.wait()
    let secs = Date().timeIntervalSince(tp)
    let flagged = result.segments.filter { !$0.flags.isEmpty }.count
    print(String(format: "  %d segments (%d flagged) in %.1f s = %.1f min per audio hour; %d/%d chunks from cache",
                 result.segments.count, flagged, secs, secs / audio.duration * 60, result.resumedChunks, result.totalChunks))
    print(String(format: "  noise floor %.1f dBFS, silence threshold %.1f dBFS",
                 result.analysis.noiseFloorDb, result.analysis.silenceThresholdDb))
    var counts: [String: Int] = [:]
    for s in result.segments { for f in s.flags { counts[f, default: 0] += 1 } }
    for (f, c) in counts.sorted(by: { $0.key < $1.key }) { print("    \(f): \(c)") }
    for (k, s) in result.segments.enumerated() {
        let row = [url.lastPathComponent, String(format: "%04d", k + 1), String(format: "%.3f", s.start),
                   String(format: "%.3f", s.end), String(format: "%.2f", s.duration),
                   String(format: "%.1f", result.analysis.db(at: s.start + 0.005)),
                   String(format: "%.1f", result.analysis.db(at: s.end - 0.005)), s.flags.joined(separator: ","),
                   s.raw, s.normalized]
        report += row.joined(separator: "\t") + "\n"
    }
    exportInputs.append(ExportInput(sourceName: url.lastPathComponent, audio: audio, segments: result.segments))
    cacheKeys.append(result.cacheKey)
}

do {
    let r = try DatasetExporter.export(exportInputs, to: outDir, settings: exportSettings, modelName: engine.modelName,
                                       modelId: engine.modelId, modelLicense: engine.modelLicense)
    print(String(format: "exported %d segments (%.1f min), skipped %d → %@", r.written, r.totalSeconds / 60, r.skipped,
                 outDir.path))
} catch {
    print("export failed: \(error.localizedDescription)")
    quit(1)
}
if let reportPath { try? report.write(to: reportPath, atomically: true, encoding: .utf8) }
if !keepCache { for k in cacheKeys { CoreEngine.clearCache(key: k) } }

quit(0)
