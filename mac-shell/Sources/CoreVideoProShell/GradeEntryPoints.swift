// The Sources workspace's three grade entry points (grading Phase 2, Task 4 / G4; spec §5.8):
// the table row "Grade", the toolbar "Grade selected…" and the inspector header "Grade…", plus the
// Picture tab's "Open grade workspace…". All four call openGradeWorkspace(slotId:), so they always
// open the same editor for the same source. Windows: SourcesTableRowControl.cs:74-78,
// SourcesWorkspaceControl.cs:170-178, SourcesWorkspaceControl.Inspector.cs:24-26,139-147.

import SwiftUI

enum SourceInspectorGradeCopy {
    static let pictureTitle = "Source grading"
    static let pictureBody = "Open the full native grade workspace for this source: primaries, curves, adjustment stack, "
        + "LUTs, presets, scopes and drawn ROI. Existing live-edit choice and Apply/Cancel behavior stay intact."
    static let openButton = "Open grade workspace…"
    static let roiNote = "ROI changes scope analysis only. Scene positioning, crop and fit remain in Scene Builder; "
        + "webcam-only overlays remain in their existing output settings."
}

extension SourcesWorkspaceState {
    /// "Grade selected…" is enabled only for an assigned selection (Windows binds IsEnabled to IsAssigned).
    static func gradeSelectedEnabled(selected: Int?, slots: [ShowInputSlot]) -> Bool {
        guard let selected, let slot = slots.first(where: { $0.id == selected }) else { return false }
        return slot.kind != "unassigned"
    }
}

extension AppModel {
    /// Opens (or focuses) the slot's grade editor and hands its window value to `openWindow`.
    /// Returns the refusal text (also shown as `gradeNotice`), or nil when a window was opened.
    @discardableResult
    func openGradeWorkspace(slotId: Int?, openWindow: (String) -> Void) -> String? {
        let sourceId = slotId.flatMap { gradeSourceId(forSlot: $0) }   // the frame key, never zoom:<pid>
        switch openGradeEditor(sourceId: sourceId) {
        case .opened(let editor):
            gradeNotice = ""
            openWindow(editor.sourceId)
            return nil
        case .refused(let text):
            gradeNotice = text
            return text
        }
    }

    /// The row's GRADED chip: the store holds an acknowledged grade for the slot's source.
    func slotIsGraded(_ slotId: Int) -> Bool {
        guard let sourceId = gradeSourceId(forSlot: slotId) else { return false }
        return sourceGrades.isGraded(sourceId)
    }
}

/// Toolbar "Grade selected…" (SourcesWorkspaceControl.cs:170), disabled without an assigned selection.
struct GradeSelectedButton: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        Button("Grade selected…") {
            model.openGradeWorkspace(slotId: model.sourcesWorkspace.selectedSlotId) {
                openWindow(id: PopoutWindow.sourceGrade, value: $0)
            }
        }
        .buttonStyle(GhostButtonStyle())
        .disabled(!SourcesWorkspaceState.gradeSelectedEnabled(selected: model.sourcesWorkspace.selectedSlotId,
                                                              slots: model.slots))
        .accessibilityLabel("Grade selected source")
    }
}

/// Inspector header "Grade…" (SourcesWorkspaceControl.Inspector.cs:24).
struct InspectorGradeHeaderButton: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.openWindow) private var openWindow
    let slotId: Int
    let assigned: Bool

    var body: some View {
        Button("Grade…") {
            model.openGradeWorkspace(slotId: slotId) { openWindow(id: PopoutWindow.sourceGrade, value: $0) }
        }
        .buttonStyle(GhostButtonStyle())
        .disabled(!assigned)
        .accessibilityLabel("Grade input \(slotId) from the inspector")
    }
}

/// The row's GRADED chip (spec §5.8 item 4); redraws when the store changes.
struct SlotGradedChip: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var store: SourceGradeStore
    let slotId: Int

    var body: some View {
        if model.slotIsGraded(slotId) {
            Text("GRADED").font(.plexMono(9, .semibold)).foregroundStyle(Studio.accent)
                .accessibilityLabel("Input \(slotId) has its own grade")
        }
    }
}
