import SwiftUI
import TTSKit
import UniformTypeIdentifiers

struct ContentView: View {
    @EnvironmentObject var model: AppModel
    @StateObject private var showImporter = StateBox(false)
    @StateObject private var showExport = StateBox(false)
    @StateObject private var showDictionary = StateBox(false)

    var body: some View {
        Group {
            if model.modelState == .ready {
                main
            } else {
                ModelSetupView()
            }
        }
    }

    private var main: some View {
        NavigationSplitView {
            QueueSidebar(showImporter: $showImporter.value)
                .navigationSplitViewColumnWidth(min: 220, ideal: 260)
        } detail: {
            if let job = model.jobs.first(where: { $0.id == model.selectedJobID }) {
                JobDetail(job: job)
            } else {
                EmptyState(showImporter: $showImporter.value)
            }
        }
        .toolbar {
            ToolbarItemGroup {
                SettingsLink { Label(model.asrModel.name, systemImage: "waveform.badge.mic") }
                    .help("Transcription model: \(model.asrModel.name). Change it in Settings.")
                Button { showImporter.value = true } label: { Label("Add Files", systemImage: "plus") }
                    .help("Add audio files to the queue")
                Button { showDictionary.value = true } label: { Label("Pronunciations", systemImage: "character.book.closed") }
                    .help("Edit the pronunciation dictionary")
                Button { showExport.value = true } label: { Label("Export", systemImage: "square.and.arrow.up") }
                    .disabled(model.readyJobs.isEmpty || model.isExporting)
                    .help("Export an LJSpeech dataset")
            }
        }
        .fileImporter(isPresented: $showImporter.value, allowedContentTypes: [.audio], allowsMultipleSelection: true) { result in
            if case .success(let urls) = result { model.addFiles(urls) }
        }
        .sheet(isPresented: $showExport.value) { ExportSheet() }
        .sheet(isPresented: $showDictionary.value) { DictionaryEditor() }
    }
}

struct EmptyState: View {
    @Binding var showImporter: Bool
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "waveform").font(.system(size: 48)).foregroundStyle(.secondary)
            Text("Add single-speaker recordings to build a dataset").font(.title3)
            Text("WAV, FLAC, MP3, M4A, AIFF or CAF. You can also drop files on the sidebar.")
                .foregroundStyle(.secondary)
            Button("Add Files…") { showImporter = true }.controlSize(.large)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

struct QueueSidebar: View {
    @EnvironmentObject var model: AppModel
    @Binding var showImporter: Bool

    var body: some View {
        List(selection: $model.selectedJobID) {
            ForEach(model.jobs) { job in
                QueueRow(job: job).tag(job.id)
                    .contextMenu {
                        Button("Remove from Queue") { model.remove(job) }
                    }
            }
        }
        .overlay {
            if model.jobs.isEmpty {
                Text("No files yet").foregroundStyle(.secondary)
            }
        }
        .dropDestination(for: URL.self) { urls, _ in
            let audio = urls.filter { AudioDecoder.supportedExtensions.contains($0.pathExtension.lowercased()) }
            model.addFiles(audio)
            return !audio.isEmpty
        }
    }
}

struct QueueRow: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var job: FileJob

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(job.name).lineLimit(1).truncationMode(.middle)
                Spacer()
                switch job.status {
                case .queued, .decoding, .processing:
                    Button { model.cancel(job) } label: { Image(systemName: "xmark.circle.fill") }
                        .buttonStyle(.borderless).help("Cancel")
                case .cancelled, .failed:
                    Button { model.retry(job) } label: { Image(systemName: "arrow.clockwise") }
                        .buttonStyle(.borderless).help("Retry (finished parts are cached)")
                case .done:
                    if job.flaggedCount > 0 {
                        Text("\(job.flaggedCount) flagged").font(.caption).foregroundStyle(.orange)
                    }
                }
            }
            switch job.status {
            case .processing(_, let p):
                ProgressView(value: p).progressViewStyle(.linear)
                Text(job.status.label).font(.caption).foregroundStyle(.secondary)
            case .decoding:
                ProgressView().progressViewStyle(.linear)
                Text(job.status.label).font(.caption).foregroundStyle(.secondary)
            case .done:
                Text("\(job.segments.count) segments").font(.caption).foregroundStyle(.secondary)
            default:
                Text(job.status.label).font(.caption).foregroundStyle(.secondary).lineLimit(2)
            }
        }
        .padding(.vertical, 2)
    }
}

struct JobDetail: View {
    @ObservedObject var job: FileJob
    var body: some View {
        if job.status == .done, job.audio != nil {
            ReviewView(job: job)
        } else {
            VStack(spacing: 10) {
                if job.status.isActive || job.status == .queued { ProgressView() }
                Text(job.name).font(.title3)
                Text(job.status.label).foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}
