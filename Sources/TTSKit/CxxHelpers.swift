import TTSCore

// std::string / std::vector access without Swift's CxxStdlib overlay (absent from the Command Line Tools).

extension String {
    init(cxx s: std.string) {
        let n = Int(ttsdc.bridge.length(s))
        self = String(unsafeUninitializedCapacity: n) { buf in
            buf.baseAddress!.withMemoryRebound(to: CChar.self, capacity: n) { p in
                Int(ttsdc.bridge.copyOut(s, p, n))
            }
        }
    }
    var cxx: std.string { withCString { ttsdc.bridge.make($0) } }
}

extension ttsdc.StringList {
    var strings: [String] {
        (0..<Int(ttsdc.bridge.count(self))).map { String(cxx: ttsdc.bridge.at(self, $0)) }
    }
    init(_ a: [String]) {
        self.init()
        for s in a { ttsdc.bridge.append(&self, s.cxx) }
    }
}

extension ttsdc.SegmentList {
    var items: [ttsdc.Segment] { (0..<Int(ttsdc.bridge.count(self))).map { ttsdc.bridge.at(self, $0) } }
}

extension ttsdc.FloatList {
    var array: [Float] {
        let n = Int(ttsdc.bridge.count(self))
        guard n > 0, let p = ttsdc.bridge.data(self) else { return [] }
        return Array(UnsafeBufferPointer(start: p, count: n))
    }
}
