// CoreVideo Tiles shell policies, ported from the Windows services of the same
// names (native-shell/CoreVideoPro.WinUI/Services/Tiles*.cs).

import Foundation

enum TilesMembershipPolicy {
    /// `TilesMembershipPolicy.NormalizeMode`: "manual" | "routed" | else "eligible".
    static func normalizeMode(_ mode: String?) -> String {
        switch mode?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "manual": return "manual"
        case "routed": return "routed"
        default: return "eligible"
        }
    }
}
