// The `tiles` wire object a Tiles scene sends to the core: Windows
// `TilesLayerPayloadBuilder.Build` + `TilesLayerWire.From` +
// `MediaCoreCommandBuilder.SerializeTilesLayer`, confirmed against
// contracts/tiles.fixtures.json (TilesFixtureTests on Windows CI).

import Foundation

enum TilesLayerPayload {
    static func layerId(sceneId: String) -> String { "tiles:" + sceneId }

    /// `DynamicGalleryLayoutService.NormalizeAspectPreset`.
    static func normalizeAspect(_ value: String?) -> String {
        switch value {
        case "4:3", "5:4", "1:1", "3:4", "9:16", "custom": return value!
        default: return "16:9"
        }
    }

    /// `SceneWorkspacePolicy.NormalizeRect`: non-finite -> 0/0/1/1, then
    /// `NormalizedCanvasRect.Clamp(.0001, .0001)`.
    static func normalizeRect(_ r: TilesRect) -> TilesRect {
        var v = r
        if !v.x.isFinite { v.x = 0 }
        if !v.y.isFinite { v.y = 0 }
        if !v.width.isFinite { v.width = 1 }
        if !v.height.isFinite { v.height = 1 }
        v.width = min(max(v.width, 0.0001), 1)
        v.height = min(max(v.height, 0.0001), 1)
        v.x = min(max(v.x, 0), 1 - v.width)
        v.y = min(max(v.y, 0), 1 - v.height)
        return v
    }

    /// `TilesLayerPayloadBuilder.FiniteClamp`.
    static func finiteClamp(_ v: Double, _ lo: Double, _ hi: Double, fallback: Double = 0) -> Double {
        v.isFinite ? min(max(v, lo), hi) : fallback
    }

    /// `TilesLayerPayloadBuilder.QualifySourceId`: a bare id becomes "zoom:<id>";
    /// anything already carrying a scheme passes through.
    static func qualify(_ id: String) -> String { id.contains(":") ? id : "zoom:" + id }

    private static func rectObject(_ r: TilesRect) -> JSONObject {
        ["x": r.x, "y": r.y, "w": r.width, "h": r.height]
    }

    /// nil when the scene is not a Tiles scene. Eligible = video-on roster
    /// participants (roster order, qualified) then canonical local ids, deduped
    /// keep-first; no other health filter (the core decides who has frames).
    /// `nameEachTile` is always false on Mac in Phase 3 (owner decision D3); the
    /// saved value is left untouched.
    static func build(sceneId: String, settings: TilesSettings?, roster: [RosterParticipant],
                      localVideoSourceIds: [String], routedSourceIds: Set<String>,
                      identityRoster: [TilesIdentityPolicy.Person]?, meetingId: String?) -> JSONObject? {
        guard let s = settings else { return nil }
        var eligible: [String] = []
        var seen = Set<String>()
        for id in roster.filter(\.hasVideo).map({ qualify($0.id) }) + localVideoSourceIds.filter(TilesLocalSourcePolicy.isLocalSourceId)
            where seen.insert(id).inserted {
            eligible.append(id)
        }
        let people = identityRoster ?? roster.map(TilesIdentityPolicy.person(from:))
        let members = TilesMembershipPolicy.resolve(s, eligible: eligible, routed: routedSourceIds,
                                                    roster: people, meetingId: meetingId)
        var overrides: JSONObject = [:]
        for (key, o) in s.overrides {
            overrides[key] = [
                "rect": o.rect.map { rectObject($0) as Any } ?? NSNull(),
                "cropLeftPercent": o.cropLeftPercent,
                "cropRightPercent": o.cropRightPercent,
                "z": o.z.map { $0 as Any } ?? NSNull(),
            ] as JSONObject
        }
        let style: JSONObject = [
            "tileAspect": normalizeAspect(s.tileAspect),
            "customAspectRatio": s.customAspectRatio,
            "gutterPercent": s.gutterPercent,
            "marginPercent": s.marginPercent,
            "backgroundColor": TilesHex.normalize(s.backgroundColor),
            "backgroundSourceId": s.backgroundSourceId,
            "borderShape": s.borderShape == "rounded" ? "rounded" : "square",
            "borderColor": TilesHex.normalize(s.borderColor),
            "borderThickness": finiteClamp(s.borderThickness, 0, 32),
            "cornerRadius": finiteClamp(s.cornerRadius, 0, 100, fallback: 16),
            "glowColor": TilesHex.normalize(s.glowColor),
            "glowSize": finiteClamp(s.glowSize, 0, 64),
            "glowIntensity": finiteClamp(s.glowIntensity, 0, 100, fallback: 100),
            "glowSoftness": finiteClamp(s.glowSoftness, 0, 100),
            "animateLayout": s.animateLayout,
            "animationDurationMs": min(max(s.animationDurationMs, 100), 2000),
            "fillMode": TilesMembershipPolicy.normalizeMode(s.membershipMode),
            "nameEachTile": false,
        ]
        return [
            "layerId": layerId(sceneId: sceneId),
            "order": s.wallOrder,
            "rect": rectObject(normalizeRect(s.wallRect)),
            "opacity": s.wallVisible ? finiteClamp(s.wallOpacity, 0, 1, fallback: 1) : 0.0,
            "transparentBackground": s.transparentBackground,
            "members": members,
            "overrides": overrides,
            "style": style,
        ]
    }
}
