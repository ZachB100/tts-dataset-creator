import AppKit
import Foundation
import SwiftUI
import TTSKit

enum JobStatus: Equatable {
    case queued
    case decoding
    case processing(stage: String, progress: Double)
    case done
    case failed(String)
    case cancelled

    var label: String {
        switch self {
        case .queued: return "Queued"
        case .decoding: return "Decoding"
        case .processing(let stage, let p): return "\(stage) \(Int(p * 100))%"
        case .done: return "Ready for review"
        case .failed(let e): return "Failed: \(e)"
        case .cancelled: return "Cancelled"
        }
    }
    var kind: Int {
        switch self {
        case .queued: return 0
        case .decoding: return 1
        case .processing: return 2
        case .done: return 3
        case .failed: return 4
        case .cancelled: return 5
        }
    }
    var isActive: Bool {
        switch self {
        case .decoding, .processing: return true
        default: return false
        }
    }
}

final class FileJob: ObservableObject, Identifiable, @unchecked Sendable { // only mutated on the main actor
    let id = UUID()
    let url: URL
    @Published var status: JobStatus = .queued {
        // tell the app model when the kind of status changes (not on every progress tick),
        // so toolbar state like "Export" enablement refreshes
        didSet { if oldValue.kind != status.kind { onStatusKindChange?() } }
    }
    var onStatusKindChange: (() -> Void)?
    @Published var segments: [SegmentItem] = []
    var audio: DecodedAudio?
    var analysis: FileAnalysisBox?
    var cacheKey: String?
    /// ASR model that produced `segments` (recorded in dataset_info.json).
    var asrModelName = "", asrModelId = "", asrModelLicense = ""

    init(url: URL) { self.url = url }
    var name: String { url.lastPathComponent }
    var flaggedCount: Int { segments.filter { !$0.flags.isEmpty }.count }
}

enum ModelState: Equatable {
    case checking
    case missing
    case downloading
    case loading
    case ready
    case failed(String)
}

@MainActor
final class AppModel: ObservableObject {
    @Published var modelState: ModelState = .checking
    @Published var downloadProgress: [String: Double] = [:]
    @Published var jobs: [FileJob] = []
    @Published var selectedJobID: FileJob.ID?
    @Published var cut = CutSettings()
    @Published var export = ExportSettings()
    @Published var exportMessage: String?
    @Published var isExporting = false
    /// Transcription backend/model; Whisper turbo unless the user picks another in Settings.
    @Published private(set) var asrModel: AsrModel = .default

    private(set) var engine: CoreEngine?
    private var runner: Task<Void, Never>?
    private var downloaders: [ModelDownloader] = []
    private var pollTimer: Timer?

    init() {
        loadSettings()
        AppPaths.ensure()
    }

    // MARK: models

    var requiredModels: [ModelSpec] { ModelSpec.required(for: asrModel) }

    func checkModels() {
        if requiredModels.allSatisfy(\.isInstalled) { loadEngine() } else { modelState = .missing }
    }

    /// True while a file is being decoded or transcribed (the model can't be switched then).
    var isProcessing: Bool { jobs.contains { $0.status.isActive } }

    /// Switch the transcription model. Files already transcribed keep their text; new files use the new model.
    func selectAsrModel(_ m: AsrModel) {
        guard m != asrModel, !isProcessing, modelState != .downloading else { return }
        asrModel = m
        UserDefaults.standard.set(m.id, forKey: "asrModel")
        runner?.cancel()
        runner = nil
        engine = nil // free the old model before loading the next one
        checkModels()
    }

    func downloadModels() {
        modelState = .downloading
        let specs = requiredModels
        Task {
            do {
                for spec in specs where !spec.isInstalled {
                    downloadProgress[spec.id] = 0
                    let d = ModelDownloader(spec: spec) { p in
                        Task { @MainActor in self.downloadProgress[spec.id] = p }
                    }
                    downloaders.append(d)
                    try await d.run()
                    downloadProgress[spec.id] = 1
                }
                downloaders.removeAll()
                loadEngine()
            } catch {
                downloaders.removeAll()
                modelState = .failed(error.localizedDescription)
            }
        }
    }

    func cancelDownloads() {
        downloaders.forEach { $0.cancel() }
    }

    private func loadEngine() {
        modelState = .loading
        let asr = asrModel, w = asr.spec.localURL, v = ModelSpec.silero.localURL
        Task.detached(priority: .userInitiated) {
            do {
                let e = try CoreEngine(asr: asr, asrModelFile: w, vadModel: v)
                await MainActor.run {
                    guard self.asrModel == asr else { return } // switched again while loading
                    self.engine = e
                    self.modelState = .ready
                    self.startQueue()
                }
            } catch {
                await MainActor.run { self.modelState = .failed(error.localizedDescription) }
            }
        }
    }

    /// Free the Whisper context before the process exits (ggml's Metal teardown asserts otherwise).
    func shutdown() {
        engine?.cancel()
        runner?.cancel()
        engine = nil
    }

    // MARK: queue

    func addFiles(_ urls: [URL]) {
        let existing = Set(jobs.map(\.url))
        for u in urls where !existing.contains(u) {
            let j = FileJob(url: u)
            j.onStatusKindChange = { [weak self] in self?.objectWillChange.send() }
            jobs.append(j)
            if selectedJobID == nil { selectedJobID = j.id }
        }
        startQueue()
    }

    func cancel(_ job: FileJob) {
        switch job.status {
        case .queued: job.status = .cancelled
        case .decoding, .processing: engine?.cancel(); job.status = .cancelled
        default: break
        }
    }

    func retry(_ job: FileJob) {
        guard case .cancelled = job.status else {
            if case .failed = job.status { job.status = .queued; startQueue() }
            return
        }
        job.status = .queued
        startQueue()
    }

    func remove(_ job: FileJob) {
        cancel(job)
        jobs.removeAll { $0.id == job.id }
        if selectedJobID == job.id { selectedJobID = jobs.first?.id }
    }

    private func startQueue() {
        guard engine != nil, runner == nil else { return }
        runner = Task { [weak self] in
            while let self, let job = self.jobs.first(where: { $0.status == .queued }) {
                await self.run(job)
            }
            self?.runner = nil
        }
    }

    private func run(_ job: FileJob) async {
        guard let engine else { return }
        job.status = .decoding
        let url = job.url
        let cut = self.cut
        // poll the engine's thread-safe progress while it works
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { _ in
            Task { @MainActor in
                if case .processing = job.status {
                    job.status = .processing(stage: engine.stage.label, progress: engine.progress)
                }
            }
        }
        defer {
            pollTimer?.invalidate()
            pollTimer = nil
        }
        do {
            let audio = try await Task.detached(priority: .userInitiated) { try AudioDecoder.decode(url) }.value
            if job.status == .cancelled { return }
            job.audio = audio
            job.status = .processing(stage: "Starting", progress: 0)
            let result = try await Task.detached(priority: .userInitiated) {
                try engine.process(audio, cut: cut, lowercaseAll: false)
            }.value
            audio.releaseASRCopy()
            job.analysis = result.analysis
            job.cacheKey = result.cacheKey
            job.asrModelName = engine.modelName
            job.asrModelId = engine.modelId
            job.asrModelLicense = engine.modelLicense
            job.segments = result.segments
            job.status = .done
        } catch EngineError.cancelled {
            job.status = .cancelled
        } catch {
            if job.status != .cancelled { job.status = .failed(error.localizedDescription) }
        }
    }

    // MARK: editing

    func binding(job: FileJob, segment id: SegmentItem.ID) -> Binding<SegmentItem>? {
        guard job.segments.contains(where: { $0.id == id }) else { return nil }
        return Binding(
            get: { job.segments.first(where: { $0.id == id }) ?? job.segments[0] },
            set: { new in
                if let i = job.segments.firstIndex(where: { $0.id == id }) { job.segments[i] = new }
            })
    }

    /// Raw text changed: regenerate the normalized text and flags.
    func rawEdited(job: FileJob, id: SegmentItem.ID) { refresh(job: job, id: id, renormalize: true) }
    /// Normalized text or bounds changed: re-check flags only.
    func boundsOrNormalizedEdited(job: FileJob, id: SegmentItem.ID) { refresh(job: job, id: id, renormalize: false) }

    private func refresh(job: FileJob, id: SegmentItem.ID, renormalize: Bool) {
        guard let engine, let a = job.analysis, let i = job.segments.firstIndex(where: { $0.id == id }) else { return }
        var s = job.segments[i]
        engine.refresh(&s, analysis: a, cut: cut, lowercaseAll: false, renormalize: renormalize)
        job.segments[i] = s
    }

    func delete(job: FileJob, ids: Set<SegmentItem.ID>) {
        job.segments.removeAll { ids.contains($0.id) }
    }

    /// Length limits changed: re-flag every segment.
    func recheckAll() {
        guard let engine else { return }
        for job in jobs {
            guard let a = job.analysis else { continue }
            job.segments = job.segments.map { seg in
                var s = seg
                engine.refresh(&s, analysis: a, cut: cut, lowercaseAll: false, renormalize: false)
                return s
            }
        }
        saveSettings()
    }

    // MARK: export

    var readyJobs: [FileJob] { jobs.filter { $0.status == .done } }

    func runExport(to dir: URL) {
        let settings = export
        let inputs: [ExportInput] = readyJobs.compactMap { job in
            guard let audio = job.audio else { return nil }
            let segs = job.segments.map { s -> SegmentItem in
                var c = s
                if settings.lowercaseAll { c.normalized = c.normalized.lowercased() }
                return c
            }
            return ExportInput(sourceName: job.name, audio: audio, segments: segs)
        }
        let keys = readyJobs.compactMap(\.cacheKey)
        isExporting = true
        exportMessage = nil
        // normally one model; if files were transcribed with different models, record them all
        func joined(_ k: KeyPath<FileJob, String>) -> String {
            var seen: [String] = []
            for j in readyJobs where !j[keyPath: k].isEmpty && !seen.contains(j[keyPath: k]) { seen.append(j[keyPath: k]) }
            return seen.joined(separator: "; ")
        }
        let modelName = joined(\.asrModelName), modelId = joined(\.asrModelId), modelLicense = joined(\.asrModelLicense)
        saveSettings()
        Task.detached(priority: .userInitiated) {
            do {
                let r = try DatasetExporter.export(inputs, to: dir, settings: settings, modelName: modelName,
                                                   modelId: modelId, modelLicense: modelLicense)
                keys.forEach { CoreEngine.clearCache(key: $0) } // a successful export ends the run
                await MainActor.run {
                    self.isExporting = false
                    self.exportMessage = String(format: "Exported %d clips (%.1f min), skipped %d, to %@", r.written,
                                                r.totalSeconds / 60, r.skipped, dir.path)
                    NSWorkspace.shared.activateFileViewerSelecting([dir.appendingPathComponent("metadata.csv")])
                }
            } catch {
                await MainActor.run {
                    self.isExporting = false
                    self.exportMessage = "Export failed: \(error.localizedDescription)"
                }
            }
        }
    }

    // MARK: settings persistence

    private func loadSettings() {
        let d = UserDefaults.standard
        if let id = d.string(forKey: "asrModel"), let m = AsrModel.find(id) { asrModel = m }
        if d.object(forKey: "minSeconds") != nil { cut.minSeconds = d.double(forKey: "minSeconds") }
        if d.object(forKey: "maxSeconds") != nil { cut.maxSeconds = d.double(forKey: "maxSeconds") }
        if d.object(forKey: "sampleRate") != nil { export.sampleRate = d.integer(forKey: "sampleRate") }
        export.threeColumns = d.bool(forKey: "threeColumns")
        export.dropFlagged = d.bool(forKey: "dropFlagged")
        export.lowercaseAll = d.bool(forKey: "lowercaseAll")
        export.minSeconds = cut.minSeconds
        export.maxSeconds = cut.maxSeconds
    }

    func saveSettings() {
        let d = UserDefaults.standard
        export.minSeconds = cut.minSeconds
        export.maxSeconds = cut.maxSeconds
        d.set(cut.minSeconds, forKey: "minSeconds")
        d.set(cut.maxSeconds, forKey: "maxSeconds")
        d.set(export.sampleRate, forKey: "sampleRate")
        d.set(export.threeColumns, forKey: "threeColumns")
        d.set(export.dropFlagged, forKey: "dropFlagged")
        d.set(export.lowercaseAll, forKey: "lowercaseAll")
    }
}
