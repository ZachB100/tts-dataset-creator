import AppKit
import SwiftUI
import TTSKit

@main
struct TTSDatasetCreatorApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @StateObject private var model = AppModel()

    var body: some Scene {
        Window("TTS Dataset Creator", id: "main") {
            ContentView()
                .environmentObject(model)
                .frame(minWidth: 1000, minHeight: 640)
                .onAppear {
                    delegate.model = model
                    if model.modelState == .checking { model.checkModels() }
                }
        }
        .commands {
            CommandGroup(replacing: .newItem) {}
        }
        Settings {
            SettingsView().environmentObject(model)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var model: AppModel? {
        didSet { if let model, !pendingURLs.isEmpty { MainActor.assumeIsolated { model.addFiles(pendingURLs) }; pendingURLs = [] } }
    }
    private var pendingURLs: [URL] = []

    /// Files dropped on the Dock icon or opened with `open -a "TTS Dataset Creator" file.mp3`.
    func application(_ application: NSApplication, open urls: [URL]) {
        let audio = urls.filter { AudioDecoder.supportedExtensions.contains($0.pathExtension.lowercased()) }
        if let model { MainActor.assumeIsolated { model.addFiles(audio) } } else { pendingURLs += audio }
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // also makes the bare executable (swift run) behave like a normal app
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    func applicationWillTerminate(_ notification: Notification) {
        MainActor.assumeIsolated { model?.shutdown() }
    }
}
