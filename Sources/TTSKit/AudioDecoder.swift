import AVFoundation
import Foundation
import TTSCore

/// Decoded mono audio at the file's native rate, plus a 16 kHz copy for ASR.
public final class DecodedAudio: @unchecked Sendable {
    public let url: URL
    public let sampleRate: Double
    public let samples: [Float]      // mono, native rate
    public private(set) var samples16k: [Float]   // mono, 16 kHz (released after transcription)

    public var duration: Double { Double(samples.count) / sampleRate }

    /// Drop the 16 kHz copy once transcription is done; review and export use the native-rate samples.
    public func releaseASRCopy() { samples16k = [] }

    init(url: URL, sampleRate: Double, samples: [Float], samples16k: [Float]) {
        self.url = url
        self.sampleRate = sampleRate
        self.samples = samples
        self.samples16k = samples16k
    }
}

public enum AudioDecodeError: LocalizedError {
    case unreadable(String)
    case empty
    public var errorDescription: String? {
        switch self {
        case .unreadable(let s): return "Can't read audio: \(s)"
        case .empty: return "The file contains no audio."
        }
    }
}

public enum AudioDecoder {
    public static let supportedExtensions = ["wav", "flac", "mp3", "m4a", "aif", "aiff", "caf", "aac", "mp4"]

    /// Decode with AVFoundation (WAV/FLAC/MP3/M4A/AIFF/CAF), mix to mono, and resample to 16 kHz in the C++ core.
    public static func decode(_ url: URL) throws -> DecodedAudio {
        let file: AVAudioFile
        do { file = try AVAudioFile(forReading: url) } catch { throw AudioDecodeError.unreadable(error.localizedDescription) }
        let format = file.processingFormat // float32, deinterleaved
        let total = AVAudioFrameCount(file.length)
        guard total > 0 else { throw AudioDecodeError.empty }
        let channels = Int(format.channelCount)
        var mono = [Float](repeating: 0, count: Int(total))
        let block: AVAudioFrameCount = 1 << 18
        guard let buffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: block) else {
            throw AudioDecodeError.unreadable("buffer allocation failed")
        }
        var written = 0
        while written < Int(total) {
            do { try file.read(into: buffer, frameCount: block) } catch {
                throw AudioDecodeError.unreadable(error.localizedDescription)
            }
            let n = Int(buffer.frameLength)
            if n == 0 { break }
            guard let data = buffer.floatChannelData else { throw AudioDecodeError.unreadable("not float PCM") }
            let gain = 1 / Float(channels)
            mono.withUnsafeMutableBufferPointer { out in
                for c in 0..<channels {
                    let ch = data[c]
                    for i in 0..<min(n, out.count - written) { out[written + i] += ch[i] * gain }
                }
            }
            written += n
        }
        if written < mono.count { mono.removeLast(mono.count - written) }
        let rate = format.sampleRate
        let s16 = resampled(mono, from: rate, to: 16000)
        return DecodedAudio(url: url, sampleRate: rate, samples: mono, samples16k: s16)
    }

    /// Resample using the core's windowed-sinc resampler.
    public static func resampled(_ x: [Float], from: Double, to: Double) -> [Float] {
        if from == to { return x }
        let v = x.withUnsafeBufferPointer { ttsdc.resample($0.baseAddress, $0.count, from, to) }
        return v.array
    }
}
