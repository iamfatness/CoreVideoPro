// The per-source grade editor window (grading plan 2026-11-16, Task 6) and the Grade section
// of the Sources inspector's Picture tab (the G4 seam: SourceInspectorGradeSlot draws it).
// A value window: openWindow(id: PopoutWindow.sourceGrade, value: sourceId) focuses the
// existing window for the same source.

import AppKit
import SwiftUI
import UniformTypeIdentifiers

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
                ScrollView { SourceGradeEditorPane(editor: editor) { dismiss() } }   // the advanced panel is tall
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
                Button("Expand / collapse grade") { editor.setAdvancedExpanded(!editor.advancedExpanded) }
                    .buttonStyle(GhostButtonStyle())
                    .accessibilityLabel("Expand or collapse the advanced grade for \(editor.sourceName)")
                Text(editor.indicator).font(.grotesk(11)).foregroundStyle(Studio.secondary)
            }
            if editor.advancedExpanded {
                AdvancedGradePanel(editor: editor)
            }
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
                   step: 1, onEditingChanged: { editing in editing ? editor.beginGesture() : editor.endGesture() })
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
            Text(SourceInspectorGradeCopy.pictureTitle).font(.grotesk(13, .semibold))
            if let context {
                Text(SourceInspectorGradeCopy.pictureBody).font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Text(GradeInspectorSection.line(store.summary(for: context.gradeSourceId)))
                    .font(.grotesk(11)).foregroundStyle(Studio.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Button(SourceInspectorGradeCopy.openButton) {
                    model.openGradeWorkspace(slotId: context.slotId) { openWindow(id: PopoutWindow.sourceGrade, value: $0) }
                }
                .buttonStyle(GhostButtonStyle())
                .accessibilityLabel("Open the grade workspace for input \(context.slotId)")
                Text(SourceInspectorGradeCopy.roiNote).font(.grotesk(10)).foregroundStyle(Studio.textDim)
                    .fixedSize(horizontal: false, vertical: true)
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

/// The advanced panel (grading plan Task 10): bypass, overall intensity, the ordered adjustment
/// stack, primaries controls with the Windows labels/ranges/help (GradeAdjustmentControl.cs),
/// undo/redo, copy/paste and .cvgrade presets. Curves and cube operations are shown and kept
/// (enable, intensity, reorder, duplicate, remove) but edited only in a later update (D9).
struct AdvancedGradePanel: View {
    @ObservedObject var editor: GradeEditorModel
    @State private var presetNotice = ""

    private static let primaries: [(String, WritableKeyPath<GradeOperation, Double>, ClosedRange<Double>, Double, String)] = [
        ("Exposure", \.exposureStops, -8...8, 0.1, "Brighter → right; darker → left."),
        ("Contrast", \.contrast, 0...4, 0.05, "Spread around the pivot; 1 is neutral."),
        ("Contrast pivot (encoded)", \.pivot, 0...1, 0.01, "Brightness level held by contrast."),
        ("Saturation", \.saturation, 0...4, 0.05, "0 removes color; 1 is neutral."),
        ("Warm / cool balance", \.temperature, -1...1, 0.01, "Cool ← → warm; 0 is neutral."),
        ("Green / magenta balance", \.tint, -1...1, 0.01, "Green ← → magenta; 0 is neutral."),
        ("Lift (encoded)", \.lift, -1...1, 0.01, "Adjust black level."),
        ("Gamma", \.gamma, 0.1...4, 0.05, "Adjust midtones; 1 is neutral."),
        ("Gain", \.gain, 0...4, 0.05, "Adjust highlights; 1 is neutral."),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("ADVANCED GRADE · REC.709 SDR").font(.grotesk(12, .semibold))
            Toggle("Bypass grade", isOn: Binding(get: { editor.document?.bypass ?? false },
                                                 set: { editor.setBypass($0) }))
                .font(.grotesk(12)).accessibilityLabel("Bypass grade for \(editor.sourceName)")
            labelledSlider("Overall grade intensity", value: (editor.document?.intensity ?? 1) * 100, range: 0...100, step: 1,
                           format: "%.0f%%") { editor.setIntensity($0 / 100) }
            HStack(spacing: 6) {
                Button("+ Primaries") { editor.addPrimaries() }.buttonStyle(GhostButtonStyle())
                    .accessibilityLabel("Add a primaries adjustment")
                Button("+ Curves") { editor.addCurves() }.buttonStyle(GhostButtonStyle())
                    .accessibilityLabel("Add a curves adjustment")
                Picker("", selection: Binding(get: { editor.selectedAdjustmentId ?? "" },
                                              set: { editor.selectedAdjustmentId = $0.isEmpty ? nil : $0 })) {
                    ForEach(editor.document?.operations ?? []) { Text($0.displayName).tag($0.id) }
                }
                .labelsHidden().frame(maxWidth: 180)
                .accessibilityLabel("Selected adjustment")
            }
            HStack(spacing: 6) {
                Button("Up") { editor.moveSelected(-1) }.accessibilityLabel("Move adjustment up")
                Button("Down") { editor.moveSelected(1) }.accessibilityLabel("Move adjustment down")
                Button("Duplicate") { editor.duplicateSelected() }.accessibilityLabel("Duplicate adjustment")
                Button("Remove") { editor.removeSelected() }.accessibilityLabel("Remove adjustment")
            }
            .buttonStyle(GhostButtonStyle())
            .disabled(editor.selectedAdjustment == nil)
            if let selected = editor.selectedAdjustment {
                Toggle("Adjustment enabled", isOn: Binding(get: { selected.enabled },
                                                           set: { on in editor.editSelected { $0.enabled = on } }))
                    .font(.grotesk(12)).accessibilityLabel("Adjustment enabled")
                labelledSlider("Adjustment intensity", value: selected.intensity * 100, range: 0...100, step: 1,
                               format: "%.0f%%") { value in editor.editSelected { $0.intensity = value / 100 } }
                if selected.kind == "primaries" {
                    ForEach(Self.primaries, id: \.0) { label, path, range, step, help in
                        labelledSlider(label, value: selected[keyPath: path], range: range, step: step, format: "%.2f",
                                       help: help) { value in editor.editSelected { $0[keyPath: path] = value } }
                    }
                } else if selected.kind == "curves" {
                    GradeCurvePanel(editor: editor)
                } else {
                    Text("3D LUT · \(selected.displayName)").font(.grotesk(11)).foregroundStyle(Studio.secondary)
                }
            }
            HStack(spacing: 6) {
                Button("Undo") { editor.undo() }.keyboardShortcut("z", modifiers: .command)
                    .disabled(!editor.history.canUndo).accessibilityLabel("Undo grade edit")
                Button("Redo") { editor.redo() }.keyboardShortcut("z", modifiers: [.command, .shift])
                    .disabled(!editor.history.canRedo).accessibilityLabel("Redo grade edit")
                Button("Reset") { editor.resetAdvanced() }.accessibilityLabel("Reset the advanced grade")
            }
            .buttonStyle(GhostButtonStyle())
            HStack(spacing: 6) {
                Button("Copy grade") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(editor.copyGrade(), forType: .string)
                }
                .accessibilityLabel("Copy grade")
                Button("Paste grade") {
                    if let text = NSPasteboard.general.string(forType: .string) { _ = editor.paste(text) }
                }
                .accessibilityLabel("Paste grade")
                Button("Save preset…") { savePreset() }.accessibilityLabel("Save grade preset")
                Button("Load preset…") { loadPreset() }.accessibilityLabel("Load grade preset")
                Button("Import LUT…") { importLut() }.accessibilityLabel("Import a .cube LUT")
            }
            .buttonStyle(GhostButtonStyle())
            if !presetNotice.isEmpty {
                Text(presetNotice).font(.grotesk(11)).foregroundStyle(Studio.amber)
            }
        }
    }

    private func labelledSlider(_ label: String, value: Double, range: ClosedRange<Double>, step: Double,
                                format: String, help: String? = nil, set: @escaping (Double) -> Void) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                Spacer()
                Text(String(format: format, value)).font(.plexMono(11)).foregroundStyle(Studio.textPrimary)
            }
            Slider(value: Binding(get: { value }, set: set), in: range, step: step,
                   onEditingChanged: { editing in editing ? editor.beginGesture() : editor.endGesture() })
                .accessibilityLabel(label)
            if let help { Text(help).font(.grotesk(10)).foregroundStyle(Studio.textDim) }
        }
    }

    private func savePreset() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.init(filenameExtension: "cvgrade") ?? .json]
        panel.nameFieldStringValue = "\(editor.sourceName).cvgrade"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do { try editor.copyGrade().write(to: url, atomically: true, encoding: .utf8); presetNotice = "" }
        catch { presetNotice = "Preset was not saved: \(error.localizedDescription)" }
    }

    /// ColorGradeEditorWindow.Advanced.cs:94-100: a .cube file up to 2 MB, named after the file.
    private func importLut() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.init(filenameExtension: "cube") ?? .data]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        guard size <= GradeEditorModel.maxCubeFileBytes else { presetNotice = "LUT exceeds 2 MB."; return }
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { presetNotice = "LUT could not be read."; return }
        editor.importCube(text: text, name: url.deletingPathExtension().lastPathComponent)
        presetNotice = editor.selectedAdjustment?.kind == "cube" ? "" : editor.status
    }

    private func loadPreset() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.init(filenameExtension: "cvgrade") ?? .json]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        guard size <= GradePreset.maxBytes else { presetNotice = "Grade document is too large."; return }
        guard let text = try? String(contentsOf: url, encoding: .utf8) else {
            presetNotice = "Preset could not be read."; return
        }
        presetNotice = editor.paste(text) ? "" : editor.status
    }
}
