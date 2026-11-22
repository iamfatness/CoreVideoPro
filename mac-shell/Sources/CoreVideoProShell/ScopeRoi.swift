// The scope region of interest (grading Phase 2; spec §3.6). GradeScopeRoi is a twin of the C#
// record (native-shell/CoreVideoPro.MediaCore/Models/GradeScopeRoi.cs) and of the core's
// validation (native/src/compositor/ScopeRoi.h:13-16): an editor measurement preference in
// source-image coordinates, never part of a grade.

import CoreGraphics
import Foundation

struct GradeScopeRoi: Equatable, Codable {
    var enabled = false
    var x = 0.0, y = 0.0, width = 1.0, height = 1.0
    var revision: Int64 = 0
    var shape = "rectangle"

    private var hasValidFields: Bool {
        [x, y, width, height].allSatisfy(\.isFinite) && x >= 0 && y >= 0 && width > 0 && height > 0 && revision >= 0
            && (shape == "rectangle" || shape == "circle")
    }
    var isValid: Bool { hasValidFields && x + width <= 1.000000001 && y + height <= 1.000000001 }
    private var isValidMeasurement: Bool { hasValidFields && x + width <= 1.0000005 && y + height <= 1.0000005 }

    /// C# MatchesMeasurement: the native JSON reports six significant digits; tolerate only that.
    func matchesMeasurement(_ other: GradeScopeRoi?) -> Bool {
        guard isValid, let other, other.isValidMeasurement else { return false }
        return enabled == other.enabled && shape == other.shape && revision == other.revision
            && abs(x - other.x) <= 0.0000005 && abs(y - other.y) <= 0.0000005
            && abs(width - other.width) <= 0.0000005 && abs(height - other.height) <= 0.0000005
    }

    static let circleSampleCount: Int = {
        var n = 0
        for py in 0..<144 { for px in 0..<256 {
            let nx = (Double(px) + 0.5) / 256 * 2 - 1, ny = (Double(py) + 0.5) / 144 * 2 - 1
            if nx * nx + ny * ny <= 1 { n += 1 }
        } }
        return n
    }()
    var expectedSampleCount: Int { enabled && shape == "circle" ? Self.circleSampleCount : 256 * 144 }

    /// C# Pixels / ScopeRoi.h pixels: the selected pixel rectangle of a w x h source.
    func pixels(width w: Int, height h: Int) -> (x: Int, y: Int, width: Int, height: Int)? {
        guard isValid, w > 0, h > 0 else { return nil }
        if !enabled { return (0, 0, w, h) }
        let left = min(max(Int((x * Double(w)).rounded(.down)), 0), w - 1)
        let top = min(max(Int((y * Double(h)).rounded(.down)), 0), h - 1)
        let right = min(max(Int(((x + width) * Double(w)).rounded(.up)), left + 1), w)
        let bottom = min(max(Int(((y + height) * Double(h)).rounded(.up)), top + 1), h)
        return (left, top, right - left, bottom - top)
    }

    /// The `scopeRoi` object of set-grade-preview (GradePreviewController.h configure).
    var wire: JSONObject {
        ["enabled": enabled, "x": x, "y": y, "width": width, "height": height,
         "revision": NSNumber(value: revision), "shape": shape]
    }

    init(enabled: Bool = false, x: Double = 0, y: Double = 0, width: Double = 1, height: Double = 1,
         revision: Int64 = 0, shape: String = "rectangle") {
        self.enabled = enabled; self.x = x; self.y = y; self.width = width; self.height = height
        self.revision = revision; self.shape = shape
    }

    /// From an event's `scopes.roi` node.
    init?(json: JSONObject?) {
        guard let json else { return nil }
        func number(_ key: String, _ fallback: Double) -> Double { (json[key] as? NSNumber)?.doubleValue ?? fallback }
        self.init(enabled: json["enabled"] as? Bool ?? false, x: number("x", 0), y: number("y", 0),
                  width: number("width", 1), height: number("height", 1),
                  revision: (json["revision"] as? NSNumber)?.int64Value ?? 0, shape: json["shape"] as? String ?? "rectangle")
    }
}

/// ScopeRoiGeometry.cs: source-coordinate drawing geometry shared by pointer and precision edits.
enum ScopeRoiGeometry {
    /// The fitted picture inside a host (letterbox / pillarbox excluded).
    static func fitImage(host: CGSize, source: (Int, Int)) -> CGRect {
        guard host.width > 0, host.height > 0, source.0 > 0, source.1 > 0 else { return .zero }
        let scale = min(Double(host.width) / Double(source.0), Double(host.height) / Double(source.1))
        let w = Double(source.0) * scale, h = Double(source.1) * scale
        return CGRect(x: (Double(host.width) - w) / 2, y: (Double(host.height) - h) / 2, width: w, height: h)
    }

    /// Draw (start → end, normalized): at least one source pixel; a circle is round in pixels and
    /// anchored at the start corner in the drag direction.
    static func draw(start: (Double, Double), end: (Double, Double), source: (Int, Int), shape: String,
                     revision: Int64 = 0) -> GradeScopeRoi {
        let (sw, sh) = (Double(max(1, source.0)), Double(max(1, source.1)))
        let sx = min(max(start.0, 0), 1), sy = min(max(start.1, 0), 1)
        let ex = min(max(end.0, 0), 1), ey = min(max(end.1, 0), 1)
        var x = min(sx, ex), y = min(sy, ey)
        var w = max(1 / sw, abs(ex - sx)), h = max(1 / sh, abs(ey - sy))
        if shape == "circle" {
            let diameter = max(1, min(w * sw, h * sh))
            w = diameter / sw; h = diameter / sh
            x = ex < sx ? sx - w : sx
            y = ey < sy ? sy - h : sy
        }
        return GradeScopeRoi(enabled: true, x: min(max(x, 0), 1 - w), y: min(max(y, 0), 1 - h), width: w, height: h,
                             revision: revision, shape: shape == "circle" ? "circle" : "rectangle")
    }

    /// CircleSize: a diameter in source pixels, clamped to fit, both axes linked.
    static func circleSize(_ roi: GradeScopeRoi, diameter: Double, source: (Int, Int)) -> GradeScopeRoi {
        let (sw, sh) = (Double(max(1, source.0)), Double(max(1, source.1)))
        var next = roi
        next.x = min(roi.x, 1 - 1 / sw); next.y = min(roi.y, 1 - 1 / sh)
        let d = min(max(diameter.isFinite ? diameter : 1, 1), max(1, min((1 - next.x) * sw, (1 - next.y) * sh)))
        next.width = d / sw; next.height = d / sh
        return next
    }
}

/// The pointer and key rules of ScopeRoiControl.cs, without a view. Host coordinates are top-left
/// origin, y down (the overlay NSView is flipped). Modes: 0 draw, 1 move, 2-9 resize
/// (2 right-bottom, 3 left-top, 4 right-top, 5 left-bottom, 6 left, 7 top, 8 right, 9 bottom).
struct ScopeRoiGesture {
    static let handleBand = 14.0
    var source: (Int, Int)
    var roi = GradeScopeRoi()
    var drawingShape: String?
    private(set) var dragging = false
    private var start = (0.0, 0.0)
    private var before = GradeScopeRoi()
    private var mode = 0

    init(source: (Int, Int)) { self.source = source }

    private func normalize(_ p: CGPoint, _ image: CGRect) -> (Double, Double) {
        (min(max(Double(p.x - image.minX) / Double(image.width), 0), 1),
         min(max(Double(p.y - image.minY) / Double(image.height), 0), 1))
    }

    /// BeginDrawingGesture (:83-97).
    mutating func begin(at p: CGPoint, host: CGSize) -> Bool {
        let image = ScopeRoiGeometry.fitImage(host: host, source: source)
        // Inclusive of the right/bottom edge like WinRT Rect.Contains (CGRect.contains is not),
        // so a region touching the picture's edge can still be grabbed by its edge handles.
        guard image.width > 0, p.x >= image.minX, p.x <= image.maxX, p.y >= image.minY, p.y <= image.maxY else { return false }
        start = normalize(p, image); before = roi; mode = 0
        if drawingShape == nil {
            let bx = Self.handleBand / Double(image.width), by = Self.handleBand / Double(image.height)
            guard roi.enabled, start.0 >= roi.x - bx, start.0 <= roi.x + roi.width + bx,
                  start.1 >= roi.y - by, start.1 <= roi.y + roi.height + by else { return false }
            let left = abs((start.0 - roi.x) * Double(image.width)) < Self.handleBand
            let right = abs((start.0 - roi.x - roi.width) * Double(image.width)) < Self.handleBand
            let top = abs((start.1 - roi.y) * Double(image.height)) < Self.handleBand
            let bottom = abs((start.1 - roi.y - roi.height) * Double(image.height)) < Self.handleBand
            mode = left && top ? 3 : right && top ? 4 : left && bottom ? 5 : right && bottom ? 2
                : left ? 6 : top ? 7 : right ? 8 : bottom ? 9 : 1
        }
        dragging = true
        return true
    }

    /// UpdateDrawingGesture (:103-131). Returns the new region, or nil when nothing changed.
    mutating func update(to p: CGPoint, host: CGSize) -> GradeScopeRoi? {
        let image = ScopeRoiGeometry.fitImage(host: host, source: source)
        guard dragging, image.width > 0 else { return nil }
        let q = normalize(p, image)
        let (sw, sh) = (Double(max(1, source.0)), Double(max(1, source.1)))
        var minW = 1 / sw, minH = 1 / sh
        var next: GradeScopeRoi
        if mode == 1 {
            next = before
            next.x = min(max(before.x + q.0 - start.0, 0), 1 - before.width)
            next.y = min(max(before.y + q.1 - start.1, 0), 1 - before.height)
        } else if mode >= 2 {
            minW = min(minW, before.width); minH = min(minH, before.height)
            var l = before.x, t = before.y, r = before.x + before.width, b = before.y + before.height
            if [3, 5, 6].contains(mode) { l = min(max(q.0, 0), r - minW) }
            if [2, 4, 8].contains(mode) { r = min(max(q.0, l + minW), 1) }
            if [3, 4, 7].contains(mode) { t = min(max(q.1, 0), b - minH) }
            if [2, 5, 9].contains(mode) { b = min(max(q.1, t + minH), 1) }
            next = before; next.x = l; next.y = t; next.width = r - l; next.height = b - t
            if before.shape == "circle" {
                var diameter = [6, 8].contains(mode) ? (r - l) * sw : [7, 9].contains(mode) ? (b - t) * sh : min((r - l) * sw, (b - t) * sh)
                let movesLeft = [3, 5, 6].contains(mode), movesTop = [3, 4, 7].contains(mode)
                diameter = max(1, min(diameter, min((movesLeft ? r : 1 - l) * sw, (movesTop ? b : 1 - t) * sh)))
                let w = diameter / sw, h = diameter / sh
                next.x = movesLeft ? r - w : l; next.y = movesTop ? b - h : t; next.width = w; next.height = h
            }
        } else {
            if abs(q.0 - start.0) * Double(image.width) < 3 && abs(q.1 - start.1) * Double(image.height) < 3 { return nil }
            next = ScopeRoiGeometry.draw(start: start, end: q, source: source, shape: drawingShape ?? "rectangle", revision: roi.revision)
        }
        guard next.isValid else { return nil }
        roi = next
        return next
    }

    /// CompleteDrawingGesture: back to Select / move.
    mutating func complete() { dragging = false; drawingShape = nil }
    /// Escape / capture lost: the region from before the gesture.
    mutating func cancel() {
        if dragging { roi = before }
        dragging = false; drawingShape = nil
    }

    /// OnKeyDown (:132-143): arrows move one source pixel; Shift+arrows resize (a circle in both axes).
    mutating func key(dx: Int, dy: Int, shift: Bool) -> Bool {
        guard roi.enabled, source.0 > 0, source.1 > 0, dx != 0 || dy != 0 else { return false }
        let (sw, sh) = (Double(source.0), Double(source.1))
        var next = roi
        if shift {
            if roi.shape == "circle" {
                next = ScopeRoiGeometry.circleSize(roi, diameter: roi.width * sw + Double(dx != 0 ? dx : dy), source: source)
            } else {
                next.width = min(max(roi.width + Double(dx) / sw, min(1 / sw, 1 - roi.x)), 1 - roi.x)
                next.height = min(max(roi.height + Double(dy) / sh, min(1 / sh, 1 - roi.y)), 1 - roi.y)
            }
        } else {
            next.x = min(max(roi.x + Double(dx) / sw, 0), 1 - roi.width)
            next.y = min(max(roi.y + Double(dy) / sh, 0), 1 - roi.height)
        }
        guard next != roi else { return false }
        roi = next
        return true
    }
}
