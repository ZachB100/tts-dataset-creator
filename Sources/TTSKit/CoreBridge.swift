import Foundation
import TTSCore

// Swift-side mirrors of the C++ types, plus a thin wrapper around ttsdc.Engine / ttsdc.Exporter.

public struct SegmentItem: Identifiable, Hashable, Sendable {
    public let id: UUID
    public var start: Double
    public var end: Double
    public var raw: String
    public var normalized: String
    public var flags: [String]
    public var wordCount: Int
    public var speechSeconds: Double

    public var duration: Double { end - start }

    init(_ s: ttsdc.Segment) {
        id = UUID()
        start = s.start
        end = s.end
        raw = String(cxx: s.raw)
        normalized = String(cxx: s.normalized)
        flags = s.flags.strings
        wordCount = Int(s.wordCount)
        speechSeconds = s.speechSeconds
    }

    var core: ttsdc.Segment {
        var s = ttsdc.Segment()
        s.start = start
        s.end = end
        s.raw = (raw).cxx
        s.normalized = (normalized).cxx
        s.flags = ttsdc.StringList(flags)
        s.wordCount = Int32(wordCount)
        s.speechSeconds = speechSeconds
        return s
    }
}

public enum Flags {
    public static func describe(_ code: String) -> String { String(cxx: ttsdc.flag.describe(code.cxx)) }
}

public struct CutSettings: Sendable, Equatable {
    public var minSeconds: Double = 1
    public var maxSeconds: Double = 15
    public var paddingSeconds: Double = 0.15
    public init() {}

    var core: ttsdc.CutOptions {
        var o = ttsdc.CutOptions()
        o.minSeconds = minSeconds
        o.maxSeconds = maxSeconds
        o.paddingSeconds = paddingSeconds
        return o
    }
}

public struct ExportSettings: Sendable, Equatable {
    public var sampleRate: Int = 22050
    public var threeColumns = false
    public var dropFlagged = false
    public var lowercaseAll = false
    public var minSeconds: Double = 1
    public var maxSeconds: Double = 15
    public init() {}
    public static let sampleRates = [16000, 22050, 24000, 44100, 48000]
}

/// Analysis of one processed file; kept for re-checking edited segments.
public final class FileAnalysisBox: @unchecked Sendable {
    let analysis: ttsdc.FileAnalysis
    init(_ a: ttsdc.FileAnalysis) { analysis = a }
    public var silenceThresholdDb: Float { analysis.silenceThresholdDb }
    public var noiseFloorDb: Float { analysis.noiseFloorDb }
    public func db(at t: Double) -> Float { analysis.dbAt(t) }
}

public struct ProcessedFile: Sendable {
    public let cacheKey: String
    public let segments: [SegmentItem]
    public let analysis: FileAnalysisBox
    public let resumedChunks: Int
    public let totalChunks: Int
}

public enum EngineError: LocalizedError {
    case load(String)
    case failed(String)
    case cancelled
    public var errorDescription: String? {
        switch self {
        case .load(let s): return "Couldn't load models: \(s)"
        case .failed(let s): return s
        case .cancelled: return "Cancelled"
        }
    }
}

public enum ProcessingStage: Int, Sendable {
    case idle = 0, hashing, vad, transcribing, cutting, done, failed, cancelled
    public var label: String {
        switch self {
        case .idle: return "Waiting"
        case .hashing: return "Reading"
        case .vad: return "Finding pauses"
        case .transcribing: return "Transcribing"
        case .cutting: return "Cutting"
        case .done: return "Done"
        case .failed: return "Failed"
        case .cancelled: return "Cancelled"
        }
    }
}

/// Owns the loaded models. Use from one background thread at a time; progress/cancel are thread-safe.
public final class CoreEngine: @unchecked Sendable {
    private let engine: ttsdc.Engine
    private let lock = NSLock()

    public let asrModel: AsrModel

    public init(asr: AsrModel, asrModelFile: URL, vadModel: URL) throws {
        asrModel = asr
        guard let e = ttsdc.Engine.create((asr.id).cxx, (asrModelFile.path).cxx, (vadModel.path).cxx) else {
            throw EngineError.load(String(cxx: ttsdc.Engine.lastCreateError()))
        }
        engine = e
        AppPaths.ensure()
        loadDictionary()
    }

    deinit { ttsdc.Engine.destroy(engine) }

    public var modelName: String { String(cxx: engine.modelName()) }
    public var modelId: String { String(cxx: engine.modelId()) }
    public var modelLicense: String { String(cxx: engine.modelLicense()) }
    public var progress: Double { engine.progress() }
    public var stage: ProcessingStage { ProcessingStage(rawValue: Int(engine.stage())) ?? .idle }
    public func cancel() { engine.requestCancel() }

    /// Load ~/Library/Application Support/TTSDatasetCreator/pronunciations.json, writing the defaults first if missing.
    @discardableResult
    public func loadDictionary() -> String? {
        let url = AppPaths.dictionary
        if !FileManager.default.fileExists(atPath: url.path) {
            try? dictionaryJSON.write(to: url, atomically: true, encoding: .utf8)
        }
        var err = std.string()
        if !engine.loadDictionaryFile((url.path).cxx, &err) { return String(cxx: err) }
        return nil
    }

    public var dictionaryJSON: String { String(cxx: engine.dictionaryJSON()) }

    /// The built-in default dictionary.
    public static var defaultDictionaryJSON: String { String(cxx: ttsdc.PronunciationDictionary.defaultJSON()) }

    /// Replace the dictionary and save it. Returns an error message for invalid JSON.
    public func setDictionary(json: String) -> String? {
        var err = std.string()
        if !engine.setDictionaryJSON((json).cxx, &err) { return String(cxx: err) }
        try? json.write(to: AppPaths.dictionary, atomically: true, encoding: .utf8)
        return nil
    }

    /// Blocking. Run off the main thread.
    public func process(_ audio: DecodedAudio, cut: CutSettings, lowercaseAll: Bool) throws -> ProcessedFile {
        lock.lock()
        defer { lock.unlock() }
        let r = audio.samples16k.withUnsafeBufferPointer { buf in
            engine.processFile((audio.url.path).cxx, buf.baseAddress, buf.count, (AppPaths.cache.path).cxx,
                               cut.core, lowercaseAll)
        }
        if !r.ok {
            if r.cancelled { throw EngineError.cancelled }
            throw EngineError.failed(String(cxx: r.error))
        }
        return ProcessedFile(cacheKey: String(cxx: r.cacheKey), segments: r.segments.items.map { SegmentItem($0) },
                             analysis: FileAnalysisBox(r.analysis), resumedChunks: Int(r.resumedChunks),
                             totalChunks: Int(r.totalChunks))
    }

    /// Re-normalize (when raw text changed) and re-check flags after an edit.
    public func refresh(_ seg: inout SegmentItem, analysis: FileAnalysisBox, cut: CutSettings, lowercaseAll: Bool,
                        renormalize: Bool) {
        var c = seg.core
        ttsdc.refreshSegment(&c, engine, analysis.analysis, cut.core, lowercaseAll, renormalize)
        seg.normalized = String(cxx: c.normalized)
        seg.flags = c.flags.strings
        seg.wordCount = Int(c.wordCount)
        seg.speechSeconds = c.speechSeconds
    }

    public func normalize(_ text: String, lowercaseAll: Bool) -> (text: String, flags: [String]) {
        let r = engine.normalize((text).cxx, lowercaseAll)
        return (String(cxx: r.text), r.flags.strings)
    }

    public static func clearCache(key: String) {
        ttsdc.Engine.clearCache((AppPaths.cache.path).cxx, (key).cxx)
    }
}

public struct ExportInput {
    public let sourceName: String
    public let audio: DecodedAudio
    public let segments: [SegmentItem]
    public init(sourceName: String, audio: DecodedAudio, segments: [SegmentItem]) {
        self.sourceName = sourceName
        self.audio = audio
        self.segments = segments
    }
}

public struct ExportResult: Sendable {
    public let written: Int
    public let skipped: Int
    public let totalSeconds: Double
}

public enum DatasetExporter {
    /// Write an LJSpeech folder (wavs/, metadata.csv, dataset_info.json). Blocking.
    public static func export(_ inputs: [ExportInput], to dir: URL, settings: ExportSettings, modelName: String,
                              modelId: String = "", modelLicense: String = "",
                              padding: Double = 0.15) throws -> ExportResult {
        var o = ttsdc.ExportOptions()
        o.sampleRate = Int32(settings.sampleRate)
        o.threeColumns = settings.threeColumns
        o.dropFlagged = settings.dropFlagged
        o.lowercaseAll = settings.lowercaseAll
        o.minSeconds = settings.minSeconds
        o.maxSeconds = settings.maxSeconds
        o.modelName = (modelName).cxx
        o.modelId = (modelId).cxx
        o.modelLicense = (modelLicense).cxx
        o.appVersion = (appVersion).cxx
        var err = std.string()
        guard let ex = ttsdc.Exporter.create((dir.path).cxx, o, &err) else { throw EngineError.failed(String(cxx: err)) }
        defer { ttsdc.Exporter.destroy(ex) }
        for input in inputs {
            var v = ttsdc.SegmentList()
            for s in input.segments { ttsdc.bridge.append(&v, s.core) }
            let ok = input.audio.samples.withUnsafeBufferPointer { buf in
                ex.addFile((input.sourceName).cxx, buf.baseAddress, buf.count, input.audio.sampleRate, v, &err)
            }
            if !ok { throw EngineError.failed(String(cxx: err)) }
        }
        let s = ex.finish()
        if !s.ok { throw EngineError.failed(String(cxx: s.error)) }
        return ExportResult(written: Int(s.written), skipped: Int(s.skipped), totalSeconds: s.totalSeconds)
    }
}
