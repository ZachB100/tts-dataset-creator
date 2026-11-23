import SwiftUI
import TTSKit

/// Waveform of the selected segment with ~1 s of context and draggable start/end handles.
struct WaveformEditor: View {
    let audio: DecodedAudio
    @Binding var segment: SegmentItem
    let lowerLimit: Double   // previous segment's end
    let upperLimit: Double   // next segment's start
    let thresholdDb: Float
    let onCommit: () -> Void

    @StateObject private var window = StateBox<ClosedRange<Double>>(0...1)
    @StateObject private var peaks = StateBox<[(Float, Float)]>([])
    @StateObject private var peaksKey = StateBox("")
    @StateObject private var gain = StateBox<Float>(1)

    private let context = 1.0

    var body: some View {
        GeometryReader { geo in
            let w = geo.size.width, h = geo.size.height
            ZStack(alignment: .topLeading) {
                Canvas { ctx, size in
                    // outside-segment shading
                    let xs = x(segment.start, w), xe = x(segment.end, w)
                    ctx.fill(Path(CGRect(x: 0, y: 0, width: xs, height: size.height)), with: .color(.gray.opacity(0.15)))
                    ctx.fill(Path(CGRect(x: xe, y: 0, width: max(0, size.width - xe), height: size.height)),
                             with: .color(.gray.opacity(0.15)))
                    // silence threshold lines
                    let thr = CGFloat(min(1, pow(10, thresholdDb / 20) * gain.value)) * size.height / 2
                    var tp = Path()
                    tp.move(to: CGPoint(x: 0, y: size.height / 2 - thr)); tp.addLine(to: CGPoint(x: size.width, y: size.height / 2 - thr))
                    tp.move(to: CGPoint(x: 0, y: size.height / 2 + thr)); tp.addLine(to: CGPoint(x: size.width, y: size.height / 2 + thr))
                    ctx.stroke(tp, with: .color(.green.opacity(0.4)), lineWidth: 0.5)
                    // waveform
                    var p = Path()
                    let n = peaks.value.count
                    for (i, pk) in peaks.value.enumerated() {
                        let px = CGFloat(i) / CGFloat(max(1, n - 1)) * size.width
                        p.move(to: CGPoint(x: px, y: size.height / 2 - CGFloat(pk.1) * size.height / 2))
                        p.addLine(to: CGPoint(x: px, y: size.height / 2 - CGFloat(pk.0) * size.height / 2 + 0.5))
                    }
                    ctx.stroke(p, with: .color(.accentColor), lineWidth: 1)
                    // neighbour limits
                    for lim in [lowerLimit, upperLimit] where window.value.contains(lim) {
                        var lp = Path()
                        lp.move(to: CGPoint(x: x(lim, w), y: 0)); lp.addLine(to: CGPoint(x: x(lim, w), y: size.height))
                        ctx.stroke(lp, with: .color(.secondary), style: StrokeStyle(lineWidth: 1, dash: [3, 3]))
                    }
                }
                handle(isStart: true, width: w, height: h)
                handle(isStart: false, width: w, height: h)
            }
            .background(RoundedRectangle(cornerRadius: 6).fill(Color(nsColor: .textBackgroundColor)))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .onAppear { reload(width: w) }
            .onChange(of: segment.id) { _, _ in reload(width: w) }
            .onChange(of: geo.size.width) { _, nw in reload(width: nw, keepWindow: true) }
        }
    }

    private func x(_ t: Double, _ w: CGFloat) -> CGFloat {
        CGFloat((t - window.value.lowerBound) / max(1e-6, window.value.upperBound - window.value.lowerBound)) * w
    }

    private func t(_ x: CGFloat, _ w: CGFloat) -> Double {
        window.value.lowerBound + Double(x / max(1, w)) * (window.value.upperBound - window.value.lowerBound)
    }

    private func handle(isStart: Bool, width w: CGFloat, height h: CGFloat) -> some View {
        let pos = x(isStart ? segment.start : segment.end, w)
        return ZStack {
            Rectangle().fill(isStart ? Color.green : Color.red).frame(width: 2, height: h)
            RoundedRectangle(cornerRadius: 2).fill(isStart ? Color.green : Color.red)
                .frame(width: 10, height: 18).offset(y: -h / 2 + 9)
        }
        .frame(width: 14, height: h)
        .contentShape(Rectangle())
        .position(x: pos, y: h / 2)
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { g in
                    let tt = t(g.location.x, w)
                    if isStart {
                        segment.start = min(max(tt, max(lowerLimit, window.value.lowerBound), 0), segment.end - 0.1)
                    } else {
                        segment.end = max(min(tt, min(upperLimit, window.value.upperBound), audio.duration), segment.start + 0.1)
                    }
                }
                .onEnded { _ in onCommit() }
        )
        .onHover { inside in if inside { NSCursor.resizeLeftRight.push() } else { NSCursor.pop() } }
        .help(isStart ? "Start" : "End")
    }

    private func reload(width: CGFloat, keepWindow: Bool = false) {
        if !keepWindow || peaksKey.value.isEmpty {
            window.value = max(0, segment.start - context)...min(audio.duration, segment.end + context)
        }
        let cols = max(100, Int(width))
        peaksKey.value = "\(segment.id)-\(cols)"
        let rate = audio.sampleRate
        let a = Int(window.value.lowerBound * rate), b = min(audio.samples.count, Int(window.value.upperBound * rate))
        guard b > a else { peaks.value = []; return }
        var out: [(Float, Float)] = []
        out.reserveCapacity(cols)
        audio.samples.withUnsafeBufferPointer { s in
            for c in 0..<cols {
                let i0 = a + (b - a) * c / cols, i1 = max(i0 + 1, a + (b - a) * (c + 1) / cols)
                var lo: Float = 0, hi: Float = 0
                for i in i0..<min(i1, b) { lo = min(lo, s[i]); hi = max(hi, s[i]) }
                out.append((lo, hi))
            }
        }
        // scale so the loudest sample in view fills ~90 % of the height
        let peak = out.reduce(Float(0)) { max($0, max(-$1.0, $1.1)) }
        let g: Float = peak > 0 ? 0.9 / peak : 1
        peaks.value = out.map { ($0.0 * g, $0.1 * g) }
        gain.value = g
    }
}
