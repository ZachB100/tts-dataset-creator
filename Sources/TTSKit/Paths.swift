import Foundation

/// Locations under ~/Library/Application Support/TTSDatasetCreator.
public enum AppPaths {
    public static var support: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("TTSDatasetCreator", isDirectory: true)
    }
    public static var models: URL { support.appendingPathComponent("models", isDirectory: true) }
    /// Hidden transcript cache (per file content hash) so a crash mid-file resumes.
    public static var cache: URL { support.appendingPathComponent(".transcript-cache", isDirectory: true) }
    /// User-editable pronunciation dictionary.
    public static var dictionary: URL { support.appendingPathComponent("pronunciations.json") }

    public static func ensure() {
        for d in [support, models, cache] {
            try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        }
    }
}

public let appVersion = "0.2.0"
