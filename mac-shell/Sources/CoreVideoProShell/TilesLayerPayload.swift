// The `tiles` wire object a Tiles scene sends to the core (Windows
// `TilesLayerPayloadBuilder` + `MediaCoreCommandBuilder.SerializeTilesLayer`).

import Foundation

enum TilesLayerPayload {
    /// `DynamicGalleryLayoutService.NormalizeAspectPreset`.
    static func normalizeAspect(_ value: String?) -> String {
        switch value {
        case "4:3", "5:4", "1:1", "3:4", "9:16", "custom": return value!
        default: return "16:9"
        }
    }
}
