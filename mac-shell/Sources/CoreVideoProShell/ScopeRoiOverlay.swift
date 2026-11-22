// G7 (grading Phase 2, Task 9): the editor-only region guide over the private preview — the AppKit
// twin of ScopeRoiControl.cs. No pixels enter the UI: it draws an outline over the fitted IOSurface
// presenter (`SurfaceView`, contentsGravity .resizeAspect = ScopeRoiGeometry.fitImage) and turns
// pointer and key input into regions through `ScopeRoiGesture`, which carries every rule.

import AppKit
import SwiftUI

struct ScopeRoiOverlay: NSViewRepresentable {
    @ObservedObject var editor: GradeEditorModel

    func makeNSView(context: Context) -> ScopeRoiOverlayView {
        let view = ScopeRoiOverlayView()
        view.editor = editor
        return view
    }

    func updateNSView(_ view: ScopeRoiOverlayView, context: Context) {
        view.editor = editor
        view.refresh()
    }
}

final class ScopeRoiOverlayView: NSView {
    weak var editor: GradeEditorModel?
    private var gesture = ScopeRoiGesture(source: (0, 0))
    /// ScopeRoiControl.cs:17-18: teal 68,193,161, 2 px; 10 px handles.
    static let stroke = NSColor(srgbRed: 68 / 255, green: 193 / 255, blue: 161 / 255, alpha: 1)

    override var isFlipped: Bool { true }              // ScopeRoiGesture works y-down
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        setAccessibilityElement(true)
        setAccessibilityRole(.group)
        setAccessibilityLabel("Scope region: drag to draw, drag inside to move, drag any edge or corner to resize. "
            + "Arrows move; Shift and arrows resize.")
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    /// Update (ScopeRoiControl.cs:46): the editor's region and the picture's pixel size. A moment
    /// with no texture keeps the last size so a gesture in flight can finish.
    func refresh() {
        guard let editor else { return }
        if editor.previewPixelWidth > 0, editor.previewPixelHeight > 0 {
            gesture.source = (editor.previewPixelWidth, editor.previewPixelHeight)
        }
        if !gesture.dragging { gesture.roi = editor.scopeRoi }
        if gesture.drawingShape != editor.roiDrawingShape {
            gesture.cancel()                               // SetDrawingTool cancels a gesture
            gesture.drawingShape = editor.roiDrawingShape
            // ArmRoi focuses the guide ONCE (Roi.cs:44) so Escape works; never on later updates,
            // which would pull focus back from a slider the operator is using.
            if editor.roiDrawingShape != nil { window?.makeFirstResponder(self) }
        }
        needsDisplay = true
    }

    /// Only intercept the pointer while it can do something: a tool is armed or a region is shown.
    override func hitTest(_ point: NSPoint) -> NSView? {
        guard let editor, editor.roiDrawingShape != nil || (editor.scopeRoi.enabled && editor.scopeRoiGuideVisible)
        else { return nil }
        return super.hitTest(point)
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let editor else { return }
        let roi = gesture.dragging ? gesture.roi : editor.scopeRoi
        let image = ScopeRoiGeometry.fitImage(host: bounds.size, source: gesture.source)
        let guideShown = editor.roiDrawingShape != nil || editor.scopeRoiGuideVisible
        guard guideShown, roi.enabled, image.width > 0 else { return }
        let rect = CGRect(x: image.minX + roi.x * image.width, y: image.minY + roi.y * image.height,
                          width: roi.width * image.width, height: roi.height * image.height)
        Self.stroke.setStroke()
        let outline = roi.shape == "circle" ? NSBezierPath(ovalIn: rect) : NSBezierPath(rect: rect)
        outline.lineWidth = 2
        outline.stroke()
        ("SCOPE ROI" as NSString).draw(at: CGPoint(x: rect.minX + 5, y: rect.minY + 5), withAttributes: [
            .font: NSFont.systemFont(ofSize: 11), .foregroundColor: Self.stroke])
        Self.stroke.setFill()
        for (fx, fy) in [(0.0, 0.0), (0.5, 0), (1, 0), (1, 0.5), (1, 1), (0.5, 1), (0, 1), (0, 0.5)] {
            NSBezierPath(rect: CGRect(x: rect.minX + fx * rect.width - 5, y: rect.minY + fy * rect.height - 5,
                                      width: 10, height: 10)).fill()
        }
    }

    override func mouseDown(with event: NSEvent) {
        guard let editor else { return }
        gesture.roi = editor.scopeRoi
        gesture.drawingShape = editor.roiDrawingShape
        guard gesture.begin(at: convert(event.locationInWindow, from: nil), host: bounds.size) else { return }
        window?.makeFirstResponder(self)
    }

    override func mouseDragged(with event: NSEvent) {
        guard let next = gesture.update(to: convert(event.locationInWindow, from: nil), host: bounds.size) else { return }
        editor?.setScopeRoi(next)                          // RoiChanged on every move, like Windows
        needsDisplay = true
    }

    override func mouseUp(with event: NSEvent) {
        guard gesture.dragging else { return }
        gesture.complete()
        editor?.roiDrawingShape = nil                      // CompleteDrawingGesture: back to Select / move
    }

    override func keyDown(with event: NSEvent) {
        guard let editor else { super.keyDown(with: event); return }
        if event.keyCode == 53 {                           // Escape: CompleteDrawingGesture(cancel: true)
            let was = gesture.dragging
            gesture.cancel()
            if was { editor.setScopeRoi(gesture.roi) }
            editor.roiDrawingShape = nil
            return
        }
        let dx = event.keyCode == 123 ? -1 : event.keyCode == 124 ? 1 : 0
        let dy = event.keyCode == 126 ? -1 : event.keyCode == 125 ? 1 : 0
        gesture.roi = editor.scopeRoi
        guard dx != 0 || dy != 0, gesture.key(dx: dx, dy: dy, shift: event.modifierFlags.contains(.shift))
        else { super.keyDown(with: event); return }
        editor.setScopeRoi(gesture.roi)
    }
}
