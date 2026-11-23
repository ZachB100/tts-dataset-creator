import AppKit
import SwiftUI
import TTSKit

/// Edits pronunciations.json (written form -> spoken form; "titles" never end a sentence).
struct DictionaryEditor: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @StateObject private var text = StateBox("")
    @StateObject private var message = StateBox<String?>(nil)
    @StateObject private var isError = StateBox(false)

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Pronunciation dictionary").font(.title2.bold())
            Text("Maps written forms to spoken forms, e.g. \"NASA\": \"nasa\", \"Dr.\": \"doctor\". Keys are case-sensitive; lowercase keys also match capitalised words. Unknown ALL-CAPS words are spelled out and flagged.")
                .foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            TextEditor(text: $text.value)
                .font(.system(.body, design: .monospaced))
                .frame(minHeight: 360)
                .border(Color.secondary.opacity(0.3))
            if let msg = message.value {
                Text(msg).foregroundStyle(isError.value ? .red : .green).fixedSize(horizontal: false, vertical: true)
            }
            HStack {
                Button("Restore Defaults") { text.value = CoreEngine.defaultDictionaryJSON }
                Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([AppPaths.dictionary]) }
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Save") { save(renormalize: false) }
                Button("Save & Re-normalize All") { save(renormalize: true) }
                    .keyboardShortcut(.defaultAction)
                    .help("Regenerates every segment's normalized text from its raw text (replaces manual edits)")
            }
        }
        .padding(20)
        .frame(width: 640)
        .onAppear { text.value = model.engine?.dictionaryJSON ?? "" }
    }

    private func save(renormalize: Bool) {
        guard let engine = model.engine else { return }
        if let err = engine.setDictionary(json: text.value) {
            message.value = "Invalid JSON: \(err)"
            isError.value = true
            return
        }
        isError.value = false
        message.value = "Saved to \(AppPaths.dictionary.path)"
        if renormalize {
            for job in model.jobs {
                for seg in job.segments { model.rawEdited(job: job, id: seg.id) }
            }
            message.value! += ". All segments re-normalized."
        }
    }
}
