// Proof shots for the per-source grade UI (grading plan 2026-11-16, Tasks 6 and 10).
// COREVIDEO_SHELL_TAB_SNAPSHOT=<dir> COREVIDEO_SHELL_GRADE=staged|staged-advanced renders,
// offscreen and inside the tab harness (harness-isolated prefs, no core, nothing sent):
// the Sources inspector's Grade section for an assigned and an unassigned input, and the editor
// for a staged capture source.

import AppKit
import SwiftUI

@MainActor
enum GradeSnapshotHarness {
    static func render(model: AppModel, dir: String) {
        guard let mode = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_GRADE"] else { return }
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam-1") else { return }
        editor.lut = "warm-film"; editor.exposure = 4; editor.saturation = -6
        if mode == "staged-advanced" { stageAdvanced(editor) }
        let pane = SourceGradeEditorPane(editor: editor) {}
            .environmentObject(model)
        write(pane, width: 520, to: dir + "/grade-editor-\(mode).png")
        let inspector = VStack(alignment: .leading, spacing: 14) {
            SourceInspectorGradeSlot(context: model.sourceGradeContext(slotId: 3))
            Divider()
            SourceInspectorGradeSlot(context: model.sourceGradeContext(slotId: 6))
        }
        .padding(14)
        .environmentObject(model)
        write(inspector, width: 360, to: dir + "/grade-inspector-section.png")
    }

    /// Three adjustments, one of them a pasted curves operation (the D9 line).
    private static func stageAdvanced(_ editor: GradeEditorModel) {
        editor.setAdvancedExpanded(true)
        editor.addPrimaries()
        editor.editSelected { $0.saturation = 0.6; $0.name = "Desaturate" }
        editor.addPrimaries()
        var curves = GradeOperation(); curves.kind = "curves"; curves.name = "Soft shoulder"
        curves.curves[0] = [GradeCurvePoint(x: 0, y: 0), GradeCurvePoint(x: 0.6, y: 0.7), GradeCurvePoint(x: 1, y: 0.95)]
        var document = editor.document ?? AdvancedGradeDocument()
        document.operations.append(curves)
        if let json = try? document.canonicalJSON() { _ = editor.paste(json) }
        editor.selectedAdjustmentId = curves.id
    }

    private static func write<V: View>(_ view: V, width: CGFloat, to path: String) {
        let host = NSHostingView(rootView: view.frame(width: width, alignment: .top)
            .background(Studio.background).environment(\.colorScheme, .dark))
        host.frame = NSRect(x: 0, y: 0, width: width, height: 100)
        host.layoutSubtreeIfNeeded()
        let height = max(100, host.fittingSize.height)
        let window = NSWindow(contentRect: NSRect(x: -10_000, y: -10_000, width: width, height: height),
                              styleMask: [.borderless], backing: .buffered, defer: false)
        window.appearance = NSAppearance(named: .darkAqua)
        window.contentView = host
        host.frame = NSRect(x: 0, y: 0, width: width, height: height)
        host.layoutSubtreeIfNeeded()
        host.display()
        guard let bitmap = host.bitmapImageRepForCachingDisplay(in: host.bounds) else { return }
        host.cacheDisplay(in: host.bounds, to: bitmap)
        if let png = bitmap.representation(using: .png, properties: [:]) {
            try? png.write(to: URL(fileURLWithPath: path))
            print("grade snapshot \(path) size=\(Int(width))x\(Int(height))")
        }
    }
}
