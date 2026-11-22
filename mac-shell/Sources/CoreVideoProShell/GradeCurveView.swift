// The curve editor panel and its AppKit canvas (grading Phase 2, Task 2). Every rule lives in
// GradeCurveEditing / GradeEditorModel; this view only forwards the pointer and keys. AppKit
// (not SwiftUI) because the Mac target is macOS 13 and SwiftUI key handling is 14+ (plan P8).
// Layout follows GradeAdjustmentControl.cs:20-60: channel picker, the curve, point input/output,
// "Reset channel", the help line.

import AppKit
import SwiftUI

struct GradeCurvePanel: View {
    @ObservedObject var editor: GradeEditorModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Picker("Curve channel", selection: $editor.curveChannel) {
                ForEach(0..<4, id: \.self) { Text(GradeEditorModel.curveChannels[$0]).tag($0) }
            }
            .pickerStyle(.segmented)
            .accessibilityLabel("Curve channel")
            GradeCurveCanvas(editor: editor, points: editor.curveEditing?.points ?? GradeCurveEditing.identity,
                             selected: editor.curveSelection)
                .frame(height: 220)
                .accessibilityLabel("Grade curve")
                .accessibilityHint(GradeEditorModel.curveHelp)
            if let editing = editor.curveEditing, editing.points.indices.contains(editing.selectedIndex) {
                let point = editing.points[editing.selectedIndex]
                let interior = editing.selectedIndex > 0 && editing.selectedIndex < editing.points.count - 1
                pointSlider("Point input", value: point.x) { editor.curveSetSelectedPoint(x: $0, y: point.y) }
                    .disabled(!interior)
                pointSlider("Point output", value: point.y) { editor.curveSetSelectedPoint(x: point.x, y: $0) }
            }
            Button("Reset channel") { editor.curveResetChannel() }
                .buttonStyle(GhostButtonStyle()).accessibilityLabel("Reset curve channel")
            Text(GradeEditorModel.curveHelp).font(.grotesk(10)).foregroundStyle(Studio.textDim)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func pointSlider(_ label: String, value: Double, set: @escaping (Double) -> Void) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(label).font(.grotesk(12)).foregroundStyle(Studio.secondary)
                Spacer()
                Text(String(format: "%.1f%%", value * 100)).font(.plexMono(11))
            }
            Slider(value: Binding(get: { value }, set: set), in: 0...1, step: 0.001,
                   onEditingChanged: { editing in editing ? editor.beginGesture() : editor.endGesture() })
                .accessibilityLabel(label)
        }
    }
}

struct GradeCurveCanvas: NSViewRepresentable {
    let editor: GradeEditorModel
    let points: [GradeCurvePoint]
    let selected: Int

    func makeNSView(context: Context) -> GradeCurveNSView { GradeCurveNSView() }
    func updateNSView(_ view: GradeCurveNSView, context: Context) {
        view.editor = editor
        view.points = points
        view.selected = selected
        view.needsDisplay = true
    }
}

final class GradeCurveNSView: NSView {
    weak var editor: GradeEditorModel?
    var points: [GradeCurvePoint] = GradeCurveEditing.identity
    var selected = -1

    override var isFlipped: Bool { true }                 // y down, as GradeCurveControl's canvas
    override var acceptsFirstResponder: Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        NSColor(red: 24 / 255, green: 25 / 255, blue: 28 / 255, alpha: 1).setFill()
        bounds.fill()
        NSColor(red: 48 / 255, green: 49 / 255, blue: 54 / 255, alpha: 1).setStroke()
        for i in 1..<4 {
            let f = CGFloat(i) / 4
            let grid = NSBezierPath()
            grid.move(to: NSPoint(x: bounds.width * f, y: 0)); grid.line(to: NSPoint(x: bounds.width * f, y: bounds.height))
            grid.move(to: NSPoint(x: 0, y: bounds.height * f)); grid.line(to: NSPoint(x: bounds.width, y: bounds.height * f))
            grid.lineWidth = 1; grid.stroke()
        }
        let line = NSBezierPath()
        for (i, point) in points.enumerated() {
            let p = location(point)
            if i == 0 { line.move(to: p) } else { line.line(to: p) }
        }
        NSColor.white.setStroke(); line.lineWidth = 2; line.stroke()
        for (i, point) in points.enumerated() {
            (i == selected ? NSColor.systemTeal : NSColor.white).setFill()
            let p = location(point)
            NSBezierPath(ovalIn: NSRect(x: p.x - 5, y: p.y - 5, width: 10, height: 10)).fill()
        }
    }

    private func location(_ point: GradeCurvePoint) -> NSPoint {
        NSPoint(x: point.x * bounds.width, y: (1 - point.y) * bounds.height)
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        guard let editor else { return }
        if event.clickCount == 2 { editor.curveRemoveSelected(); return }   // DoubleTapped (:44)
        editor.beginGesture()                                               // one undo step per gesture
        editor.curvePress(at: convert(event.locationInWindow, from: nil), size: bounds.size)
    }
    override func mouseDragged(with event: NSEvent) {
        editor?.curveDrag(to: convert(event.locationInWindow, from: nil), size: bounds.size)
    }
    override func mouseUp(with event: NSEvent) { editor?.endGesture() }

    override func keyDown(with event: NSEvent) {
        let key: CurveKey?
        switch event.keyCode {
        case 115: key = .home
        case 119: key = .end
        case 123: key = .left
        case 124: key = .right
        case 125: key = .down
        case 126: key = .up
        case 51, 117: key = .delete
        default: key = nil
        }
        guard let key, let editor else { super.keyDown(with: event); return }
        editor.curveKey(key, shift: event.modifierFlags.contains(.shift), command: event.modifierFlags.contains(.command))
    }
}
