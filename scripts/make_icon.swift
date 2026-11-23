// Draws the app icon (AppKit only) into an .iconset folder: swift scripts/make_icon.swift OUT.iconset
import AppKit

let out = URL(fileURLWithPath: CommandLine.arguments[1])
try? FileManager.default.createDirectory(at: out, withIntermediateDirectories: true)

func draw(_ px: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(px)
    let rect = NSRect(x: s * 0.1, y: s * 0.1, width: s * 0.8, height: s * 0.8)
    let path = NSBezierPath(roundedRect: rect, xRadius: s * 0.18, yRadius: s * 0.18)
    NSGradient(starting: NSColor(calibratedRed: 0.16, green: 0.35, blue: 0.85, alpha: 1),
               ending: NSColor(calibratedRed: 0.45, green: 0.2, blue: 0.75, alpha: 1))!.draw(in: path, angle: -60)
    // waveform bars
    let heights: [CGFloat] = [0.18, 0.32, 0.5, 0.36, 0.62, 0.44, 0.28, 0.52, 0.3, 0.16]
    let barW = rect.width / CGFloat(heights.count * 2 + 1)
    NSColor.white.setFill()
    for (i, h) in heights.enumerated() {
        let bh = rect.height * h
        let x = rect.minX + barW * CGFloat(i * 2 + 1)
        NSBezierPath(roundedRect: NSRect(x: x, y: rect.midY - bh / 2, width: barW, height: bh),
                     xRadius: barW / 2, yRadius: barW / 2).fill()
    }
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

for size in [16, 32, 128, 256, 512] {
    try! draw(size).write(to: out.appendingPathComponent("icon_\(size)x\(size).png"))
    try! draw(size * 2).write(to: out.appendingPathComponent("icon_\(size)x\(size)@2x.png"))
}
