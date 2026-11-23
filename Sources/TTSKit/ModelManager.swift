import Foundation

/// The two model files the app needs, downloaded from Hugging Face on first run.
public struct ModelSpec: Sendable, Identifiable {
    public let id: String
    public let fileName: String
    public let url: URL
    public let expectedBytes: Int64
    public let label: String

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

    public static let silero = ModelSpec(
        id: "silero", fileName: "ggml-silero-v6.2.0.bin",
        url: URL(string: "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v6.2.0.bin")!,
        expectedBytes: 885_098, label: "Silero VAD v6.2.0 (0.9 MB)")

    public static let all: [ModelSpec] = [.whisper, .silero]
}

public enum ModelDownloadError: LocalizedError {
    case badStatus(Int)
    case wrongSize(Int64, Int64)
    public var errorDescription: String? {
        switch self {
        case .badStatus(let c): return "Download failed (HTTP \(c))."
        case .wrongSize(let got, let want): return "Downloaded \(got) bytes, expected \(want)."
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
