import AppKit
import SwiftUI
import TTSKit

/// First-run model manager: downloads the selected ASR model (Whisper large-v3-turbo by default, or NVIDIA
/// Parakeet) and Silero VAD from Hugging Face.
struct ModelSetupView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            Text("Speech models").font(.largeTitle.bold())
            Text("TTS Dataset Creator transcribes on this Mac and finds pauses with Silero VAD (MIT licence). Choose a transcription model; it is downloaded once from Hugging Face into Application Support. You can change it later in Settings.")
                .fixedSize(horizontal: false, vertical: true)
                .foregroundStyle(.secondary)

            AsrModelPicker()

            ForEach(model.requiredModels) { spec in
                HStack(alignment: .center, spacing: 12) {
                    Image(systemName: spec.isInstalled ? "checkmark.circle.fill" : "arrow.down.circle")
                        .foregroundStyle(spec.isInstalled ? .green : .secondary)
                        .font(.title2)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(spec.label)
                        if let p = model.downloadProgress[spec.id], !spec.isInstalled {
                            ProgressView(value: p)
                        } else {
                            Text(spec.isInstalled ? "Installed" : spec.fileName).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
            }

            switch model.modelState {
            case .checking, .loading:
                HStack { ProgressView().controlSize(.small); Text("Loading models…") }
            case .missing:
                Button("Download Models") { model.downloadModels() }.controlSize(.large).keyboardShortcut(.defaultAction)
            case .downloading:
                Button("Cancel Download") { model.cancelDownloads() }
            case .failed(let e):
                Text(e).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
                HStack {
                    Button("Try Again") { model.checkModels() }
                    if model.requiredModels.contains(where: { !$0.isInstalled }) {
                        Button("Download Models") { model.downloadModels() }
                    }
                }
            case .ready:
                EmptyView()
            }

            Button("Show Models Folder") {
                AppPaths.ensure()
                NSWorkspace.shared.open(AppPaths.models)
            }
            .buttonStyle(.link)
        }
        .padding(40)
        .frame(maxWidth: 560)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
