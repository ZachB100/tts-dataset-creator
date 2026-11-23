import AVFoundation
import Foundation
import TTSKit

/// Plays a slice of a decoded file (with the same 10 ms fades the export applies).
@MainActor
final class SegmentPlayer: ObservableObject {
    @Published private(set) var playingID: UUID?
    private let engine = AVAudioEngine()
    private let node = AVAudioPlayerNode()
    private var format: AVAudioFormat?
    private var generation = 0

    init() {
        engine.attach(node)
    }

    func toggle(_ id: UUID, audio: DecodedAudio, start: Double, end: Double) {
        if playingID == id { stop(); return }
        play(id, audio: audio, start: start, end: end)
    }

    func play(_ id: UUID, audio: DecodedAudio, start: Double, end: Double) {
        stop()
        let rate = audio.sampleRate
        let a = max(0, Int(start * rate)), b = min(audio.samples.count, Int(end * rate))
        guard b > a, let fmt = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 1),
              let buf = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: AVAudioFrameCount(b - a)) else { return }
        buf.frameLength = AVAudioFrameCount(b - a)
        let out = buf.floatChannelData![0]
        let fade = min(Int(0.01 * rate), (b - a) / 2)
        audio.samples.withUnsafeBufferPointer { src in
            for i in 0..<(b - a) {
                var g: Float = 1
                if i < fade { g = Float(i) / Float(fade) }
                if i >= (b - a) - fade { g = Float((b - a) - 1 - i) / Float(fade) }
                out[i] = src[a + i] * g
            }
        }
        if format != fmt {
            engine.stop()
            engine.disconnectNodeOutput(node)
            engine.connect(node, to: engine.mainMixerNode, format: fmt)
            format = fmt
        }
        do {
            if !engine.isRunning { try engine.start() }
        } catch { return }
        generation += 1
        let gen = generation
        node.scheduleBuffer(buf, at: nil, options: []) { [weak self] in
            Task { @MainActor in
                guard let self, self.generation == gen else { return }
                self.playingID = nil
            }
        }
        node.play()
        playingID = id
    }

    func stop() {
        generation += 1
        node.stop()
        playingID = nil
    }
}
