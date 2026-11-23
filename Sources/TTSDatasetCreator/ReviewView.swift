import SwiftUI
import TTSKit

/// Review table for one file plus the waveform editor for the selected segment.
struct ReviewView: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var job: FileJob
    @StateObject private var player = SegmentPlayer()
    @StateObject private var selection = StateBox<Set<SegmentItem.ID>>([])
    @StateObject private var flaggedOnly = StateBox(false)

    private var rows: [SegmentItem] { flaggedOnly.value ? job.segments.filter { !$0.flags.isEmpty } : job.segments }
    private var selectedID: SegmentItem.ID? { selection.value.count == 1 ? selection.value.first : nil }

    var body: some View {
        VStack(spacing: 0) {
            editor
                .frame(height: 230)
            Divider()
            table
        }
        .onDisappear { player.stop() }
    }

    @ViewBuilder private var editor: some View {
        if let id = selectedID, let audio = job.audio, let binding = model.binding(job: job, segment: id),
           let idx = job.segments.firstIndex(where: { $0.id == id }) {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text("Segment \(idx + 1)").font(.headline)
                    Text(String(format: "%.2f – %.2f s  (%.2f s)", binding.wrappedValue.start, binding.wrappedValue.end,
                                binding.wrappedValue.duration))
                        .monospacedDigit().foregroundStyle(.secondary)
                    Spacer()
                    Button {
                        player.toggle(id, audio: audio, start: binding.wrappedValue.start, end: binding.wrappedValue.end)
                    } label: {
                        Label(player.playingID == id ? "Stop" : "Play", systemImage: player.playingID == id ? "stop.fill" : "play.fill")
                    }
                    .keyboardShortcut(.space, modifiers: [])
                }
                WaveformEditor(audio: audio, segment: binding,
                               lowerLimit: idx > 0 ? job.segments[idx - 1].end : 0,
                               upperLimit: idx + 1 < job.segments.count ? job.segments[idx + 1].start : audio.duration,
                               thresholdDb: job.analysis?.silenceThresholdDb ?? -60) {
                    model.boundsOrNormalizedEdited(job: job, id: id)
                }
                Text("Drag the handles to move the start and end. Cuts should sit in silence.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(10)
        } else {
            VStack {
                Text(selection.value.count > 1 ? "\(selection.value.count) segments selected" : "Select a segment to see its waveform")
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    private var table: some View {
        VStack(spacing: 0) {
            HStack {
                Toggle("Flagged only", isOn: $flaggedOnly.value).toggleStyle(.checkbox)
                Spacer()
                Text("\(job.segments.count) segments, \(job.flaggedCount) flagged, \(String(format: "%.1f", job.segments.reduce(0) { $0 + $1.duration } / 60)) min")
                    .foregroundStyle(.secondary)
                Button(role: .destructive) {
                    model.delete(job: job, ids: selection.value)
                    selection.value = []
                } label: { Label("Delete", systemImage: "trash") }
                    .disabled(selection.value.isEmpty)
                    .keyboardShortcut(.delete, modifiers: [.command])
            }
            .padding(8)
            Table(rows, selection: $selection.value) {
                TableColumn("#") { seg in
                    Text("\((job.segments.firstIndex(where: { $0.id == seg.id }) ?? 0) + 1)").monospacedDigit()
                }
                .width(34)
                TableColumn("Length") { seg in
                    Text(String(format: "%.1f s", seg.duration)).monospacedDigit()
                        .foregroundStyle(seg.duration < model.cut.minSeconds || seg.duration > model.cut.maxSeconds ? .orange : .primary)
                }
                .width(54)
                TableColumn("Flags") { seg in FlagBadges(flags: seg.flags) }
                    .width(min: 40, ideal: 70)
                TableColumn("") { seg in
                    Button {
                        if let audio = job.audio { player.toggle(seg.id, audio: audio, start: seg.start, end: seg.end) }
                    } label: { Image(systemName: player.playingID == seg.id ? "stop.fill" : "play.fill") }
                        .buttonStyle(.borderless)
                        .help("Play")
                }
                .width(24)
                TableColumn("Raw transcript") { seg in
                    if let b = model.binding(job: job, segment: seg.id) {
                        CommitTextField(text: b.raw) { model.rawEdited(job: job, id: seg.id) }
                    }
                }
                TableColumn("Normalized") { seg in
                    if let b = model.binding(job: job, segment: seg.id) {
                        CommitTextField(text: b.normalized) { model.boundsOrNormalizedEdited(job: job, id: seg.id) }
                    }
                }
                TableColumn("") { seg in
                    Button {
                        model.delete(job: job, ids: [seg.id])
                        selection.value.remove(seg.id)
                    } label: { Image(systemName: "trash") }
                        .buttonStyle(.borderless)
                        .help("Delete segment")
                }
                .width(24)
            }
        }
    }
}

/// Text field that calls `onCommit` when editing ends (Return or focus loss).
struct CommitTextField: View {
    @Binding var text: String
    var onCommit: () -> Void
    @StateObject private var draft = StateBox("")
    @FocusState private var focused: Bool

    var body: some View {
        TextField("", text: $draft.value)
            .textFieldStyle(.plain)
            .focused($focused)
            .onAppear { draft.value = text }
            .onChange(of: text) { _, new in if !focused { draft.value = new } }
            .onSubmit(commit)
            .onChange(of: focused) { _, isFocused in if !isFocused { commit() } }
            .help(text)
    }

    private func commit() {
        guard draft.value != text else { return }
        text = draft.value
        onCommit()
    }
}

struct FlagBadges: View {
    let flags: [String]
    var body: some View {
        if flags.isEmpty {
            Image(systemName: "checkmark").foregroundStyle(.green.opacity(0.7))
        } else {
            HStack(spacing: 2) {
                Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                Text("\(flags.count)").font(.caption).monospacedDigit()
            }
            .help(flags.map { Flags.describe($0) }.joined(separator: "\n"))
        }
    }
}
