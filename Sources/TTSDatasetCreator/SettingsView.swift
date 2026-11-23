import SwiftUI
import TTSKit

/// Settings window (⌘,): transcription backend/model picker.
struct SettingsView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Form {
            Section {
                AsrModelPicker()
            } header: {
                Text("Transcription model")
            } footer: {
                Text("Files already transcribed keep their text. Each model has its own transcript cache, so switching never reuses another model's transcript.")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .frame(width: 560)
        .fixedSize(horizontal: false, vertical: true)
    }
}

/// Radio list of the ASR models, with the selected model's licence and install state.
struct AsrModelPicker: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        let selection = Binding<String>(
            get: { model.asrModel.id },
            set: { id in if let m = AsrModel.find(id) { model.selectAsrModel(m) } })
        VStack(alignment: .leading, spacing: 10) {
            Picker("Model", selection: selection) {
                ForEach(AsrModel.all) { m in Text(m.name).tag(m.id) }
            }
            .pickerStyle(.radioGroup)
            .disabled(model.isProcessing || model.modelState == .downloading)
            if model.isProcessing {
                Text("Finish or cancel the files being transcribed to switch models.")
                    .font(.caption).foregroundStyle(.orange)
            }
            AsrModelDetails(asr: model.asrModel)
        }
    }
}

struct AsrModelDetails: View {
    let asr: AsrModel

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(asr.summary).fixedSize(horizontal: false, vertical: true)
            Text("Licence: \(asr.license)").fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 12) {
                Link("Model card", destination: asr.licenseURL)
                Text(asr.spec.isInstalled ? "Installed" : "Not downloaded yet: \(asr.spec.label)")
                    .foregroundStyle(.secondary)
            }
        }
        .font(.callout)
    }
}
