// swift-tools-version:6.0
// TTS Dataset Creator — built with SwiftPM only (no Xcode project).
//
// whisper.cpp/ggml are built by scripts/build_whisper.sh (CMake) into build/whisper/install as static
// libraries and linked here. Run that script once before `swift build` (bundle_app.sh does it for you).
import PackageDescription
import Foundation

let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent().path
let whisperLib = "\(root)/build/whisper/install/lib"

let cxx: [SwiftSetting] = [.interoperabilityMode(.Cxx)]

let package = Package(
    name: "TTSDatasetCreator",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "TTSDatasetCreator", targets: ["TTSDatasetCreator"]),
        .executable(name: "ttsdc", targets: ["ttsdc"]),
    ],
    targets: [
        // C++ core: normalization, cutting, ASR (Whisper turbo, NVIDIA Parakeet)/VAD wrappers, export.
        .target(
            name: "TTSCore",
            exclude: ["vendor"],
            cxxSettings: [
                .headerSearchPath("../../third_party/whisper.cpp/include"),
                .headerSearchPath("../../third_party/whisper.cpp/ggml/include"),
                .unsafeFlags(["-Wno-deprecated-declarations", "-Wno-deprecated-literal-operator"]),
            ],
            linkerSettings: [
                .unsafeFlags(["-L\(whisperLib)"]),
                .linkedLibrary("whisper"),
                .linkedLibrary("parakeet"),
                .linkedLibrary("ggml"),
                .linkedLibrary("ggml-base"),
                .linkedLibrary("ggml-cpu"),
                .linkedLibrary("ggml-metal"),
                .linkedLibrary("ggml-blas"),
                .linkedFramework("Accelerate"),
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("Foundation"),
            ]
        ),
        // C++ unit tests (swift-testing's macro plugin doesn't load with the Command Line Tools alone).
        .executableTarget(name: "TTSCoreTests", dependencies: ["TTSCore"], path: "Tests/TTSCoreTests"),
        // Swift layer shared by the app and the CLI: audio decoding (AVFoundation), models, pipeline.
        .target(name: "TTSKit", dependencies: ["TTSCore"], swiftSettings: cxx),
        .executableTarget(name: "ttsdc", dependencies: ["TTSKit", "TTSCore"], swiftSettings: cxx),
        .executableTarget(name: "TTSDatasetCreator", dependencies: ["TTSKit", "TTSCore"], swiftSettings: cxx),
    ],
    swiftLanguageModes: [.v5],
    cxxLanguageStandard: .cxx17
)
