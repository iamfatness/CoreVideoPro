// The per-source grade editor window (grading plan 2026-11-16, Task 6) and the Grade section
// of the Sources inspector's Picture tab (the G4 seam: SourceInspectorGradeSlot draws it).
// A value window: openWindow(id: PopoutWindow.sourceGrade, value: sourceId) focuses the
// existing window for the same source.

import SwiftUI

extension PopoutWindow {
    static let sourceGrade = "popout-source-grade"
}

struct SourceGradeEditorWindow: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    let sourceId: String
    @State private var refusal: String?
    @State private var editor: GradeEditorModel?

    var body: some View {
        Group {
            if let editor {
                SourceGradeEditorPane(editor: editor) { dismiss() }
            } else {
                VStack(alignment: .leading, spacing: 10) {
                    Text(refusal ?? "").font(.grotesk(12)).foregroundStyle(Studio.amber)
                        .fixedSize(horizontal: false, vertical: true)
                    Button("Close") { dismiss() }.buttonStyle(GhostButtonStyle())
                }
                .padding(14)
            }
        }
        .onAppear {
            switch model.openGradeEditor(sourceId: sourceId) {
            case .opened(let opened):
                editor = opened
                opened.onClose = { [weak model] in
                    model?.closeGradeEditor(sourceId: opened.sourceId)
                    dismiss()
                }
            case .refused(let text):
                refusal = text   // e.g. a window macOS restored at launch for a fourth source
            }
        }
        .onDisappear { if editor != nil { model.closeGradeEditor(sourceId: sourceId) } }
    }
}

struct SourceGradeEditorPane: View {
    @ObservedObject var editor: GradeEditorModel
    var close: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(editor.headerTitle).font(.grotesk(15, .semibold))
            Text(GradeEditorModel.previewNotice).font(.grotesk(11)).foregroundStyle(Studio.amber)
                .fixedSize(horizontal: false, vertical: true)
            MonoLabel("BUILT-IN LOOK")
            Picker("", selection: Binding(get: { editor.lut }, set: { editor.lut = $0; editor.edited() })) {
                ForEach(SourceColorGrade.looks, id: \.self) { Text($0).tag($0) }
            }
            .labelsHidden()
            .accessibilityLabel("Built-in look for \(editor.sourceName)")
            axis("Exposure", \.exposure)
            axis("Contrast", \.contrast)
            axis("Saturation", \.saturation)
            axis("Temperature", \.temperature)
            Toggle("Apply edits live", isOn: $editor.liveEditing)
                .font(.grotesk(12))
                .accessibilityLabel("Apply edits live for \(editor.sourceName)")
            HStack(spacing: 8) {
                Button("Apply Live") { Task { await editor.applyLive() } }
                    .buttonStyle(GhostButtonStyle()).disabled(editor.isApplying)
                Button("Apply and Done") { Task { _ = await editor.applyAndDone() } }
                    .buttonStyle(GhostButtonStyle()).disabled(editor.isApplying)
                Button("Close / discard draft") { close() }
                    .buttonStyle(GhostButtonStyle())
                Spacer()
                Button("Reset") { editor.reset() }
                    .buttonStyle(GhostButtonStyle()).disabled(editor.isApplying)
            }
            Text(GradeEditorModel.resetNotice).font(.grotesk(10)).foregroundStyle(Studio.secondary)
            Text(editor.status).font(.grotesk(11)).foregroundStyle(Studio.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func axis(_ label: String, _ path: ReferenceWritableKeyPath<GradeEditorModel, Int>) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                Spacer()
                Text(editor[keyPath: path] > 0 ? "+\(editor[keyPath: path])" : "\(editor[keyPath: path])")
                    .font(.plexMono(11))
                    .foregroundStyle(editor[keyPath: path] == 0 ? Studio.textDim : Studio.textPrimary)
            }
            Slider(value: Binding(get: { Double(editor[keyPath: path]) },
                                  set: { editor[keyPath: path] = Int($0.rounded()); editor.edited() }),
                   in: Double(SourceColorGrade.editorRange.lowerBound)...Double(SourceColorGrade.editorRange.upperBound),
                   step: 1)
                .accessibilityLabel("\(label) for \(editor.sourceName)")
            Text(GradeEditorModel.help[label] ?? "").font(.grotesk(10)).foregroundStyle(Studio.textDim)
        }
    }
}

/// The Sources inspector's Grade section (Picture tab, below Key): the current grade in one
/// line, its live state, and the button that opens the editor window.
struct GradeInspectorSection: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow
    @ObservedObject var store: SourceGradeStore     // redraws when a grade is applied or re-asserted
    let context: SourceGradeContext?

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("Grade").font(.grotesk(13, .semibold))
            if let context {
                Text(GradeInspectorSection.line(store.summary(for: context.gradeSourceId)))
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Button("Grade…") {
                    switch model.openGradeEditor(sourceId: context.gradeSourceId) {
                    case .opened(let editor):
                        model.gradeNotice = ""
                        openWindow(id: PopoutWindow.sourceGrade, value: editor.sourceId)
                    case .refused(let text):
                        model.gradeNotice = text
                    }
                }
                .buttonStyle(GhostButtonStyle())
                .accessibilityLabel("Grade input \(context.slotId)")
            } else {
                Text("Choose a source for this input to grade it.")
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
            }
            if !model.gradeNotice.isEmpty {
                Text(model.gradeNotice).font(.grotesk(11)).foregroundStyle(Studio.amber)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    /// One line for the inspector (spec §5.8).
    static func line(_ summary: SourceGradeSummary?) -> String {
        guard let summary else { return "No grade of its own: follows the Program Look." }
        if summary.identityChanged { return "Source identity changed. Reopen its grade editor." }
        return summary.active ? summary.line : summary.line + " · waiting for this source's video"
    }
}
