import Foundation

import CryptoKit

/// A model file downloaded from Hugging Face on first use.
public struct ModelSpec: Sendable, Identifiable, Hashable {
    public let id: String
    public let fileName: String
    public let url: URL
    public let expectedBytes: Int64
    public let label: String
    /// SHA-256 of the file, checked after download when set.
    public var sha256: String? = nil

    public var localURL: URL { AppPaths.models.appendingPathComponent(fileName) }

    public var isInstalled: Bool {
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: localURL.path),
              let size = attrs[.size] as? NSNumber else { return false }
        return size.int64Value == expectedBytes
    }

    public static let whisper = ModelSpec(
        id: "whisper", fileName: "ggml-large-v3-turbo.bin",
        url: URL(string: "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-turbo.bin")!,
        expectedBytes: 1_624_555_275, label: "Whisper large-v3-turbo (1.6 GB)")

    /// NVIDIA Parakeet TDT 0.6B v2 (English), f16 GGML. NVIDIA publishes only the .nemo checkpoint and ggml-org
    /// hosts v3 only, so this is a third-party conversion (same converter output as ggml-org's v3), pinned by hash.
    public static let parakeetV2 = ModelSpec(
        id: "parakeet-v2", fileName: "ggml-parakeet-tdt-0.6b-v2-f16.bin",
        url: URL(string: "https://huggingface.co/JoaoZaokk/parakeet-tdt-0.6b-v2-ggml/resolve/main/ggml-parakeet-tdt-0.6b-v2-f16.bin")!,
        expectedBytes: 1_237_448_348, label: "Parakeet TDT 0.6B v2 (1.2 GB)",
        sha256: "1af0aed998520343d0f9b9b96b3b351accccb2d87a53c98f08015c5b408256cd")

    /// NVIDIA Parakeet TDT 0.6B v3 (25 European languages), f16 GGML from ggml-org.
    public static let parakeetV3 = ModelSpec(
        id: "parakeet-v3", fileName: "ggml-parakeet-tdt-0.6b-v3-f16.bin",
        url: URL(string: "https://huggingface.co/ggml-org/parakeet-GGUF/resolve/main/ggml-parakeet-tdt-0.6b-v3-f16.bin")!,
        expectedBytes: 1_255_897_319, label: "Parakeet TDT 0.6B v3 (1.3 GB)",
        sha256: "833bffc9513b2cae867ee9e51633cfd11e4d51aaa5597c8ac02159385a2b426f")

    public static let silero = ModelSpec(
        id: "silero", fileName: "ggml-silero-v6.2.0.bin",
        url: URL(string: "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin")!,
        expectedBytes: 885_098, label: "Silero VAD v6.2.0 (0.9 MB)")

    /// Files needed to run with `asr`.
    public static func required(for asr: AsrModel) -> [ModelSpec] { [asr.spec, .silero] }
}

/// A selectable transcription backend/model.
public struct AsrModel: Sendable, Identifiable, Hashable {
    /// Id understood by the C++ core (makeAsrBackend) and the `ttsdc --model` option.
    public let id: String
    public let name: String
    public let summary: String
    public let license: String
    public let licenseURL: URL
    public let spec: ModelSpec

    public static let whisperTurbo = AsrModel(
        id: "whisper-large-v3-turbo", name: "Whisper large-v3-turbo",
        summary: "Default. Best sentence-end timing for cutting (DTW timestamps). Tends to drop fillers and writes numbers as digits.",
        license: "MIT (OpenAI). Commercial use allowed.",
        licenseURL: URL(string: "https://github.com/openai/whisper/blob/main/LICENSE")!, spec: .whisper)

    public static let parakeetV2 = AsrModel(
        id: "parakeet-tdt-0.6b-v2", name: "NVIDIA Parakeet TDT 0.6B v2 (English)",
        summary: "English only. Fast, with punctuation and casing. Word times come from the TDT decoder.",
        license: "CC-BY-4.0 (NVIDIA). Commercial use allowed with attribution: credit NVIDIA's parakeet-tdt-0.6b-v2 when you publish a dataset or voice made with it.",
        licenseURL: URL(string: "https://huggingface.co/nvidia/parakeet-tdt-0.6b-v2")!, spec: .parakeetV2)

    public static let parakeetV3 = AsrModel(
        id: "parakeet-tdt-0.6b-v3", name: "NVIDIA Parakeet TDT 0.6B v3 (multilingual)",
        summary: "25 European languages with automatic language detection. Text normalization in this app is English only.",
        license: "CC-BY-4.0 (NVIDIA). Commercial use allowed with attribution: credit NVIDIA's parakeet-tdt-0.6b-v3 when you publish a dataset or voice made with it.",
        licenseURL: URL(string: "https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3")!, spec: .parakeetV3)

    public static let all: [AsrModel] = [.whisperTurbo, .parakeetV2, .parakeetV3]
    public static let `default` = whisperTurbo

    public static func find(_ id: String) -> AsrModel? { all.first { $0.id == id } }
}

public enum ModelDownloadError: LocalizedError {
    case badStatus(Int)
    case wrongSize(Int64, Int64)
    case wrongHash(String)
    public var errorDescription: String? {
        switch self {
        case .badStatus(let c): return "Download failed (HTTP \(c))."
        case .wrongSize(let got, let want): return "Downloaded \(got) bytes, expected \(want)."
        case .wrongHash(let name): return "\(name) failed its SHA-256 check; the download was discarded."
        }
    }
}

/// Downloads a model with progress. Writes to a temporary file and moves it into place when complete.
public final class ModelDownloader: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    private let spec: ModelSpec
    private let onProgress: @Sendable (Double) -> Void
    private var continuation: CheckedContinuation<Void, Error>?
    private var session: URLSession?
    private var task: URLSessionDownloadTask?

    public init(spec: ModelSpec, onProgress: @escaping @Sendable (Double) -> Void) {
        self.spec = spec
        self.onProgress = onProgress
    }

    public func run() async throws {
        AppPaths.ensure()
        try await withCheckedThrowingContinuation { (c: CheckedContinuation<Void, Error>) in
            continuation = c
            let s = URLSession(configuration: .default, delegate: self, delegateQueue: nil)
            session = s
            let t = s.downloadTask(with: spec.url)
            task = t
            t.resume()
        }
    }

    public func cancel() { task?.cancel() }

    public func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData _: Int64,
                           totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        let total = totalBytesExpectedToWrite > 0 ? totalBytesExpectedToWrite : spec.expectedBytes
        onProgress(Double(totalBytesWritten) / Double(total))
    }

    public func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        do {
            if let http = downloadTask.response as? HTTPURLResponse, http.statusCode != 200 {
                throw ModelDownloadError.badStatus(http.statusCode)
            }
            let size = (try FileManager.default.attributesOfItem(atPath: location.path)[.size] as? NSNumber)?.int64Value ?? 0
            if size != spec.expectedBytes { throw ModelDownloadError.wrongSize(size, spec.expectedBytes) }
            if let want = spec.sha256, try sha256Hex(of: location) != want {
                throw ModelDownloadError.wrongHash(spec.fileName)
            }
            let dest = spec.localURL
            try? FileManager.default.removeItem(at: dest)
            try FileManager.default.moveItem(at: location, to: dest)
            finish(nil)
        } catch {
            finish(error)
        }
    }

    public func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error { finish(error) }
    }

    private func finish(_ error: Error?) {
        guard let c = continuation else { return }
        continuation = nil
        session?.finishTasksAndInvalidate()
        if let error { c.resume(throwing: error) } else { c.resume() }
    }
}

/// Streaming SHA-256 of a file, hex.
public func sha256Hex(of url: URL) throws -> String {
    let h = try FileHandle(forReadingFrom: url)
    defer { try? h.close() }
    var hasher = SHA256()
    while let data = try h.read(upToCount: 8 << 20), !data.isEmpty { hasher.update(data: data) }
    return hasher.finalize().map { String(format: "%02x", $0) }.joined()
}
