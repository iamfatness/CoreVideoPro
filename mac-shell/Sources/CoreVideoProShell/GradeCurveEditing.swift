// The curve editor's geometry and keyboard rules (grading Phase 2, Task 2; spec §3.2). A pure port
// of the Windows GradeCurveControl (native-shell/CoreVideoPro.WinUI/Controls/GradeCurveControl.cs),
// so every rule is tested without a window; GradeCurveView is the thin AppKit view over it.
// View coordinates are top-left origin, y DOWN (the Windows canvas; the NSView is flipped).

import CoreGraphics
import Foundation

enum CurveKey { case home, end, left, right, up, down, delete }

struct GradeCurveEditing: Equatable {
    static let identity = [GradeCurvePoint(x: 0, y: 0), GradeCurvePoint(x: 1, y: 1)]
    static let hitRadius = 13.0            // GradeCurveControl.cs:87
    static let maxPoints = 16

    var points: [GradeCurvePoint] = GradeCurveEditing.identity
    var selectedIndex = -1

    init(points: [GradeCurvePoint] = GradeCurveEditing.identity, selectedIndex: Int = -1) {
        self.points = points
        self.selectedIndex = selectedIndex < points.count ? selectedIndex : -1
    }

    /// OnPressed (:84-99): select the nearest point within 13 px, else add one (fewer than 16, x
    /// clamped 0.001…0.999, no other point within 0.002 in x) and select it. True when a point was added.
    mutating func press(at p: CGPoint, size: CGSize) -> Bool {
        guard size.width > 0, size.height > 0 else { return false }
        var closest = -1
        var distance = Self.hitRadius
        for (i, point) in points.enumerated() {
            let dx = Double(p.x) - point.x * Double(size.width)
            let dy = Double(p.y) - (1 - point.y) * Double(size.height)
            let d = (dx * dx + dy * dy).squareRoot()
            if d < distance { closest = i; distance = d }
        }
        var added = false
        if closest < 0, points.count < Self.maxPoints {
            let x = min(max(Double(p.x) / Double(size.width), 0.001), 0.999)
            let y = min(max(1 - Double(p.y) / Double(size.height), 0), 1)
            if points.allSatisfy({ abs($0.x - x) >= 0.002 }) {
                points.append(GradeCurvePoint(x: x, y: y))
                points.sort { $0.x < $1.x }
                closest = points.firstIndex { $0.x == x } ?? -1
                added = true
            }
        }
        selectedIndex = closest
        return added
    }

    /// OnMoved (:100-105): the selected point follows the pointer.
    mutating func drag(to p: CGPoint, size: CGSize) -> Bool {
        guard size.width > 0, size.height > 0 else { return false }
        return setSelected(x: Double(p.x) / Double(size.width), y: 1 - Double(p.y) / Double(size.height))
    }

    /// SetSelected (:76-83): endpoints keep x = 0 / 1; an interior x stays between its neighbours by
    /// min(quarter gap, 0.000001) — copied as written; y is clamped to 0…1.
    @discardableResult
    mutating func setSelected(x: Double, y: Double) -> Bool {
        guard points.indices.contains(selectedIndex), x.isFinite, y.isFinite else { return false }
        let i = selectedIndex
        var nx = x
        if i == 0 { nx = 0 } else if i == points.count - 1 { nx = 1 } else {
            let gap = min((points[i + 1].x - points[i - 1].x) / 4, 0.000001)
            nx = min(max(nx, points[i - 1].x + gap), points[i + 1].x - gap)
        }
        let next = GradeCurvePoint(x: nx, y: min(max(y, 0), 1))
        guard next != points[i] else { return false }
        points[i] = next
        return true
    }

    /// RemoveSelected (:84-89): interior points only; nothing is selected afterwards.
    @discardableResult
    mutating func removeSelected() -> Bool {
        guard selectedIndex > 0, selectedIndex < points.count - 1 else { return false }
        points.remove(at: selectedIndex)
        selectedIndex = -1
        return true
    }

    /// HandleEditingKey (:52-65), Command in place of Control (spec §5.4). True when the CURVE changed.
    mutating func key(_ key: CurveKey, shift: Bool, command: Bool) -> Bool {
        switch key {
        case .home: selectedIndex = 0; return false
        case .end: selectedIndex = points.count - 1; return false
        case .left where command, .right where command:
            selectedIndex = min(max(selectedIndex + (key == .left ? -1 : 1), 0), points.count - 1)
            return false
        case .delete: return removeSelected()
        case .left, .right, .up, .down:
            guard points.indices.contains(selectedIndex) else { selectedIndex = 0; return false }
            let step = shift ? 0.01 : 0.001
            let p = points[selectedIndex]
            return setSelected(x: p.x + (key == .left ? -step : key == .right ? step : 0),
                               y: p.y + (key == .down ? -step : key == .up ? step : 0))
        }
    }

    mutating func reset() {
        selectedIndex = -1
        points = Self.identity
    }
}
