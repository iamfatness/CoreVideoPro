// CoreVideo Tiles settings on the Mac shell: a field-for-field port of Windows
// `DynamicGallerySettings` (native-shell/CoreVideoPro.WinUI/Models/ProductionModels.cs)
// with the same defaults, and the restore clamps of
// `ScenePersistenceService.FromPersisted` (ScenePersistenceService.cs). Field names
// match Windows (camelCase) so show files stay portable later (spec decision 4).

import Foundation

struct TilesRect: Codable, Equatable {
    var x = 0.0, y = 0.0, width = 1.0, height = 1.0
}

struct TilesMemberOverride: Codable, Equatable {
    var rect: TilesRect?
    var cropLeftPercent = 0.0
    var cropRightPercent = 0.0
    var z: Int?
}

/// The working (clamped) settings a Tiles scene carries. A value type: copying a
/// scene copies its settings, never a reference.
struct TilesSettings: Equatable {
    var wallRect = TilesRect()
    var wallOrder = 0
    var wallOpacity = 1.0
    var wallVisible = true
    var wallLocked = false
    var transparentBackground = false
    var backgroundColor = "#000000"
    var backgroundSourceId = ""
    var overrides: [String: TilesMemberOverride] = [:]
    var autoFill = true
    var membershipMode = "eligible"
    var manualSlots: [String?] = []
    var excludedSourceIds: [String] = []
    var omittedSourceIds: [String] = []
    /// Meeting number the Zoom identities were saved in (legacy session ids are
    /// valid only in this meeting).
    var boundMeetingId: String?
    var maxTiles = 16
    var tileAspect = "16:9"
    var customAspectRatio = 16.0 / 9.0
    var gutterPercent = 0.741
    var marginPercent = 0.741
    var borderShape = "square"
    var borderColor = "#000000"
    var borderThickness = 0.0
    var cornerRadius = 16.0
    var glowColor = "#FFFFFF"
    var glowSize = 0.0
    var glowIntensity = 100.0
    var glowSoftness = 0.0
    var animateLayout = false
    var animationDurationMs = 350
    /// Persisted untouched; the Mac payload always sends false in Phase 3 (D3).
    var nameEachTile = false

    init() {}

    /// `ScenePersistenceService.FromPersisted`, clamp for clamp.
    init(persisted p: PersistedTilesSettings) {
        wallRect = p.wallRect
        wallOrder = p.wallOrder
        wallOpacity = p.wallOpacity.isFinite ? min(max(p.wallOpacity, 0), 1) : 1
        wallVisible = p.wallVisible
        wallLocked = p.wallLocked
        transparentBackground = p.transparentBackground
        autoFill = p.autoFill
        membershipMode = TilesMembershipPolicy.normalizeMode(p.membershipMode ?? (p.autoFill ? "eligible" : "manual"))
        backgroundColor = p.backgroundColor
        backgroundSourceId = p.backgroundSourceId
        // Dictionary order is unspecified in both languages; Windows' Take(64)
        // keeps an arbitrary 64 too. Sorted here so the cap is deterministic.
        overrides = Dictionary(uniqueKeysWithValues: p.overrides.sorted { $0.key < $1.key }.prefix(64).map { ($0.key, $0.value) })
        manualSlots = Array(p.manualSlots.prefix(64))
        excludedSourceIds = Self.nonBlankDistinct(p.excludedSourceIds)
        omittedSourceIds = Self.nonBlankDistinct(p.omittedSourceIds)
        let meeting = p.boundMeetingId?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        boundMeetingId = meeting.isEmpty ? nil : meeting
        maxTiles = min(max(p.maxTiles, 1), 64)
        tileAspect = TilesLayerPayload.normalizeAspect(p.tileAspect)
        customAspectRatio = Self.clamp(p.customAspectRatio, 0.25, 4)
        gutterPercent = Self.clamp(p.gutterPercent, 0, 10)
        marginPercent = Self.clamp(p.marginPercent, 0, 20)
        borderShape = p.borderShape == "rounded" ? "rounded" : "square"
        borderColor = TilesHex.normalize(p.borderColor)
        borderThickness = Self.clamp(p.borderThickness, 0, 32)
        cornerRadius = Self.clamp(p.cornerRadius, 0, 100)
        glowColor = TilesHex.normalize(p.glowColor)
        glowSize = Self.clamp(p.glowSize, 0, 64)
        glowIntensity = Self.clamp(p.glowIntensity, 0, 100)
        glowSoftness = Self.clamp(p.glowSoftness, 0, 100)
        animateLayout = p.animateLayout
        animationDurationMs = min(max(p.animationDurationMs, 100), 2000)
        nameEachTile = p.nameEachTile
    }

    var persisted: PersistedTilesSettings {
        var p = PersistedTilesSettings()
        p.wallRect = wallRect; p.wallOrder = wallOrder; p.wallOpacity = wallOpacity
        p.wallVisible = wallVisible; p.wallLocked = wallLocked; p.transparentBackground = transparentBackground
        p.backgroundColor = backgroundColor; p.backgroundSourceId = backgroundSourceId
        p.overrides = overrides; p.autoFill = autoFill; p.membershipMode = membershipMode
        p.manualSlots = manualSlots; p.excludedSourceIds = excludedSourceIds; p.omittedSourceIds = omittedSourceIds
        p.boundMeetingId = boundMeetingId; p.maxTiles = maxTiles; p.tileAspect = tileAspect
        p.customAspectRatio = customAspectRatio; p.gutterPercent = gutterPercent; p.marginPercent = marginPercent
        p.borderShape = borderShape; p.borderColor = borderColor; p.borderThickness = borderThickness
        p.cornerRadius = cornerRadius; p.glowColor = glowColor; p.glowSize = glowSize
        p.glowIntensity = glowIntensity; p.glowSoftness = glowSoftness; p.animateLayout = animateLayout
        p.animationDurationMs = animationDurationMs; p.nameEachTile = nameEachTile
        return p
    }

    /// C# `Math.Clamp` on a double: NaN passes through there; JSON cannot carry
    /// NaN, so a non-finite value here only comes from code and takes the low bound.
    private static func clamp(_ v: Double, _ lo: Double, _ hi: Double) -> Double {
        v.isFinite ? min(max(v, lo), hi) : (v == .infinity ? hi : lo)
    }

    private static func nonBlankDistinct(_ ids: [String]) -> [String] {
        var seen = Set<String>()
        var out: [String] = []
        for id in ids where !id.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty && seen.insert(id).inserted {
            out.append(id)
            if out.count == 1024 { break }
        }
        return out
    }
}

/// What the prefs file stores. Every key is optional on decode (an older build
/// omits keys, a newer one adds some): a missing key takes the `TilesSettings()`
/// default and an unknown key is ignored, so one partial `tiles` object can never
/// fail the scene decode and drop the whole prefs file (Review Focus 1). A key of
/// the wrong JSON type also falls back to its default rather than throwing.
struct PersistedTilesSettings: Codable, Equatable {
    var wallRect = TilesRect()
    var wallOrder = 0
    var wallOpacity = 1.0
    var wallVisible = true
    var wallLocked = false
    var transparentBackground = false
    var backgroundColor = "#000000"
    var backgroundSourceId = ""
    var overrides: [String: TilesMemberOverride] = [:]
    var autoFill = true
    var membershipMode: String?
    var manualSlots: [String?] = []
    var excludedSourceIds: [String] = []
    var omittedSourceIds: [String] = []
    var boundMeetingId: String?
    var maxTiles = 16
    var tileAspect = "16:9"
    var customAspectRatio = 16.0 / 9.0
    var gutterPercent = 0.741
    var marginPercent = 0.741
    var borderShape = "square"
    var borderColor = "#000000"
    var borderThickness = 0.0
    var cornerRadius = 16.0
    var glowColor = "#FFFFFF"
    var glowSize = 0.0
    var glowIntensity = 100.0
    var glowSoftness = 0.0
    var animateLayout = false
    var animationDurationMs = 350
    var nameEachTile = false

    init() {}

    private enum CodingKeys: String, CodingKey {
        case wallRect, wallOrder, wallOpacity, wallVisible, wallLocked, transparentBackground
        case backgroundColor, backgroundSourceId, overrides, autoFill, membershipMode, manualSlots
        case excludedSourceIds, omittedSourceIds, boundMeetingId, maxTiles, tileAspect, customAspectRatio
        case gutterPercent, marginPercent, borderShape, borderColor, borderThickness, cornerRadius
        case glowColor, glowSize, glowIntensity, glowSoftness, animateLayout, animationDurationMs, nameEachTile
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        func value<T: Decodable>(_ key: CodingKeys, _ fallback: T) -> T {
            ((try? c.decodeIfPresent(T.self, forKey: key)) ?? nil) ?? fallback
        }
        let d = PersistedTilesSettings()
        wallRect = value(.wallRect, d.wallRect)
        wallOrder = value(.wallOrder, d.wallOrder)
        wallOpacity = value(.wallOpacity, d.wallOpacity)
        wallVisible = value(.wallVisible, d.wallVisible)
        wallLocked = value(.wallLocked, d.wallLocked)
        transparentBackground = value(.transparentBackground, d.transparentBackground)
        backgroundColor = value(.backgroundColor, d.backgroundColor)
        backgroundSourceId = value(.backgroundSourceId, d.backgroundSourceId)
        overrides = value(.overrides, d.overrides)
        autoFill = value(.autoFill, d.autoFill)
        membershipMode = (try? c.decodeIfPresent(String.self, forKey: .membershipMode)) ?? nil
        manualSlots = value(.manualSlots, d.manualSlots)
        excludedSourceIds = value(.excludedSourceIds, d.excludedSourceIds)
        omittedSourceIds = value(.omittedSourceIds, d.omittedSourceIds)
        boundMeetingId = (try? c.decodeIfPresent(String.self, forKey: .boundMeetingId)) ?? nil
        maxTiles = value(.maxTiles, d.maxTiles)
        tileAspect = value(.tileAspect, d.tileAspect)
        customAspectRatio = value(.customAspectRatio, d.customAspectRatio)
        gutterPercent = value(.gutterPercent, d.gutterPercent)
        marginPercent = value(.marginPercent, d.marginPercent)
        borderShape = value(.borderShape, d.borderShape)
        borderColor = value(.borderColor, d.borderColor)
        borderThickness = value(.borderThickness, d.borderThickness)
        cornerRadius = value(.cornerRadius, d.cornerRadius)
        glowColor = value(.glowColor, d.glowColor)
        glowSize = value(.glowSize, d.glowSize)
        glowIntensity = value(.glowIntensity, d.glowIntensity)
        glowSoftness = value(.glowSoftness, d.glowSoftness)
        animateLayout = value(.animateLayout, d.animateLayout)
        animationDurationMs = value(.animationDurationMs, d.animationDurationMs)
        nameEachTile = value(.nameEachTile, d.nameEachTile)
    }
}

/// Windows colour helpers used by Tiles.
enum TilesHex {
    /// `SourceRouteVisualDefaults.BorderColor`, the fallback of `NormalizeBorderColor`.
    static let fallback = "#44C1A1"

    /// `SceneRoutingService.NormalizeBorderColor`: blank -> fallback; trim; prefix
    /// "#"; length 7 or 9 -> uppercased, else fallback. (No hex-digit check, like Windows.)
    static func normalize(_ value: String?) -> String {
        let trimmed = value?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if trimmed.isEmpty { return fallback }
        let color = trimmed.hasPrefix("#") ? trimmed : "#" + trimmed
        return color.count == 7 || color.count == 9 ? color.uppercased() : fallback
    }

    /// `HexColor.TryParse`: exactly 6 hex digits, optional leading '#'.
    static func parse(_ value: String?) -> (r: UInt8, g: UInt8, b: UInt8)? {
        guard var s = value?.trimmingCharacters(in: .whitespacesAndNewlines) else { return nil }
        if s.hasPrefix("#") { s.removeFirst() }
        guard s.count == 6, s.allSatisfy(\.isHexDigit), let n = UInt32(s, radix: 16) else { return nil }
        return (UInt8((n >> 16) & 0xFF), UInt8((n >> 8) & 0xFF), UInt8(n & 0xFF))
    }

    static func format(r: UInt8, g: UInt8, b: UInt8) -> String {
        String(format: "#%02X%02X%02X", r, g, b)
    }
}
