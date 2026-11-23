import AppKit
import SwiftUI
import TTSKit

struct ExportSheet: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) private var dismiss

    private var total: Int { model.readyJobs.reduce(0) { $0 + $1.segments.count } }
    private var flagged: Int { model.readyJobs.reduce(0) { $0 + $1.flaggedCount } }
    private var notReady: Int { model.jobs.count - model.readyJobs.count }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("Export LJSpeech dataset").font(.title2.bold())
            Form {
                Picker("metadata.csv", selection: $model.export.threeColumns) {
                    Text("2 columns: id|normalized").tag(false)
                    Text("3 columns: id|raw|normalized").tag(true)
                }
                Picker("Sample rate", selection: $model.export.sampleRate) {
                    ForEach(ExportSettings.sampleRates, id: \.self) { r in
                        Text(r == 22050 ? "22,050 Hz (default)" : "\(r.formatted()) Hz").tag(r)
                    }
                }
                LabeledContent("Length limits") {
                    HStack {
                        TextField("min", value: $model.cut.minSeconds, format: .number.precision(.fractionLength(0...2)))
                            .frame(width: 60)
                        Text("to")
                        TextField("max", value: $model.cut.maxSeconds, format: .number.precision(.fractionLength(0...2)))
                            .frame(width: 60)
                        Text("seconds (outside = flagged)").foregroundStyle(.secondary)
                    }
                }
                Toggle("Lowercase all normalized text", isOn: $model.export.lowercaseAll)
                Toggle("Drop flagged segments", isOn: $model.export.dropFlagged)
            }
            .onChange(of: model.cut) { _, _ in model.recheckAll() }

            Text(summary).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            if let msg = model.exportMessage {
                Text(msg).foregroundStyle(msg.hasPrefix("Export failed") ? .red : .green)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                if model.isExporting { ProgressView().controlSize(.small); Text("Exporting…") }
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Choose Folder & Export…") { chooseAndExport() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(model.isExporting || total == 0)
            }
        }
        .padding(24)
        .frame(width: 560)
    }

    private var summary: String {
        var s = "\(total) segments from \(model.readyJobs.count) file(s), \(flagged) flagged"
        s += model.export.dropFlagged ? " (will be left out)." : " (will be included)."
        if notReady > 0 { s += " \(notReady) file(s) not ready yet are skipped." }
        s += " Exporting ends the run: the hidden transcript cache for these files is cleared."
        return s
    }

    private func chooseAndExport() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = "Export Here"
        panel.message = "Choose an empty folder for wavs/, metadata.csv and dataset_info.json"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let existing = (try? FileManager.default.contentsOfDirectory(atPath: url.path))?.filter { !$0.hasPrefix(".") } ?? []
        if !existing.isEmpty {
            let alert = NSAlert()
            alert.messageText = "This folder isn't empty"
            alert.informativeText = "Existing files with the same names (metadata.csv, wavs/…) will be overwritten."
            alert.addButton(withTitle: "Export Anyway")
            alert.addButton(withTitle: "Cancel")
            if alert.runModal() != .alertFirstButtonReturn { return }
        }
        model.runExport(to: url)
    }
}
