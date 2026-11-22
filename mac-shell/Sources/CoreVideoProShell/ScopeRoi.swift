// The scope region of interest (grading Phase 2; spec §3.6). GradeScopeRoi is a twin of the C#
// record (native-shell/CoreVideoPro.MediaCore/Models/GradeScopeRoi.cs) and of the core's
// validation (native/src/compositor/ScopeRoi.h:13-16): an editor measurement preference in
// source-image coordinates, never part of a grade.

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
