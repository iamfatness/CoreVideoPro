// GraphicsPalette — the show palette model (graphics styles spec 4.1, slice 1).
// Port of MC/Models/GraphicsPalette.cs (GraphicsPalette, GraphicsPaletteMigration).
// Model only on the Mac in slice 1: parse / validate / resolve / persist the same
// `palette` section of the show document and send the resolved map with each
// playlist instance, exactly as Windows does. No Mac palette UI yet.
// Conformance: contracts/graphics-shell/palette.vectors.json (ShellTestsGraphicsPalette.swift).

import Foundation

struct GraphicsPalette: Equatable {
    static let auto = "auto"
    static let primary = "brand.primary", secondary = "brand.secondary", accent = "brand.accent", surface = "brand.surface"
    static let onPrimary = "brand.onPrimary", onPrimaryMuted = "brand.onPrimaryMuted", onSecondary = "brand.onSecondary", onAccent = "brand.onAccent"
    static let teamA = "brand.teamA", teamB = "brand.teamB"
    static let tokens = [primary, secondary, accent, surface, onPrimary, onPrimaryMuted, onSecondary, onAccent, teamA, teamB]
    static let required = [primary, secondary, accent, surface, onPrimary, onPrimaryMuted, onSecondary, onAccent]
    static let onColourPlates: [String: String] = [onPrimary: primary, onPrimaryMuted: primary, onSecondary: secondary, onAccent: accent]
    static let white = "#FFFFFFFF", nearBlack = "#111111FF"
    static let mutedOnDark = "#E1E9F2FF", mutedOnLight = "#3A4350FF"

    /// The whole `palette` object as read, so keys a newer build wrote survive.
    var json: JSONObject

    init(json: JSONObject) { self.json = json }
    init(colors: [String: String], version: Int = 1) { json = ["version": version, "colors": colors] }
    static func == (a: GraphicsPalette, b: GraphicsPalette) -> Bool { (a.json as NSDictionary).isEqual(to: b.json) }

    /// GraphicsPalette.Default: the look built-in graphics had before the palette.
    static let stock = GraphicsPalette(colors: [
        primary: "#172433E6", secondary: "#E1E9F2FF", accent: "#44C1A1FF", surface: "#172433B3",
        onPrimary: auto, onPrimaryMuted: auto, onSecondary: auto, onAccent: auto])

    var version: Int { (json["version"] as? NSNumber)?.intValue ?? 1 }
    var colors: [String: String] {
        get { (json["colors"] as? JSONObject ?? [:]).compactMapValues { $0 as? String } }
        set { json["colors"] = newValue }
    }

    var isValid: Bool {
        guard version == 1, let raw = json["colors"] as? JSONObject, raw.count <= Self.tokens.count else { return false }
        for (token, value) in raw {
            guard Self.tokens.contains(token), let text = value as? String,
                  Self.isColor(text) || (text == Self.auto && Self.onColourPlates[token] != nil) else { return false }
        }
        return Self.required.allSatisfy { raw[$0] != nil }
    }

    static func isColor(_ value: String?) -> Bool {
        guard let value, value.utf8.count == 9, value.hasPrefix("#") else { return false }
        return value.dropFirst().allSatisfy(\.isHexDigit)
    }

    /// GraphicsPalette.TryNormalize: #RGB, #RRGGBB or #RRGGBBAA (the # optional) to upper-case #RRGGBBAA.
    static func normalize(_ text: String) -> String? {
        var hex = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if hex.hasPrefix("#") { hex.removeFirst() }
        guard !hex.isEmpty, hex.allSatisfy(\.isHexDigit) else { return nil }
        switch hex.count {
        case 3: hex = hex.map { "\($0)\($0)" }.joined() + "FF"
        case 6: hex += "FF"
        case 8: break
        default: return nil
        }
        return "#" + hex.uppercased()
    }

    static func fromHex(_ rrggbb: String, opacity: Double = 1) -> String? {
        let bare = rrggbb.trimmingCharacters(in: .whitespacesAndNewlines).replacingOccurrences(of: "#", with: "")
        guard bare.count == 6, let value = normalize(bare) else { return nil }
        let alpha = Int((min(max(opacity, 0), 1) * 255).rounded(.toNearestOrAwayFromZero))
        return String(value.prefix(7)) + String(format: "%02X", alpha)
    }

    /// WCAG 2.1 contrast of two colours' RGB (alpha ignored).
    static func contrast(_ a: String, _ b: String) -> Double {
        func luminance(_ c: String) -> Double {
            let chars = Array(c)
            func channel(_ offset: Int) -> Double {
                let s = Double(Int(String(chars[offset..<offset + 2]), radix: 16) ?? 0) / 255
                return s <= 0.04045 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4)
            }
            return 0.2126 * channel(1) + 0.7152 * channel(3) + 0.0722 * channel(5)
        }
        let x = luminance(a), y = luminance(b)
        return (max(x, y) + 0.05) / (min(x, y) + 0.05)
    }

    static func autoOn(_ plate: String) -> String { contrast(white, plate) >= contrast(nearBlack, plate) ? white : nearBlack }

    func with(_ token: String, _ value: String) -> GraphicsPalette {
        var copy = self, map = colors
        if value.isEmpty && !Self.required.contains(token) { map.removeValue(forKey: token) } else { map[token] = value }
        copy.colors = map
        return copy
    }

    /// GraphicsPalette.Resolve: row override -> this palette, automatic on-colours against their resolved plate.
    func resolve(_ rowOverrides: [String: String]? = nil) -> [String: String] {
        let palette = colors
        func layer(_ token: String) -> String? {
            if let o = rowOverrides?[token], Self.isColor(o) { return o.uppercased() }
            guard let v = palette[token] else { return nil }
            return v == Self.auto ? Self.auto : Self.isColor(v) ? v.uppercased() : nil
        }
        var resolved: [String: String] = [:]
        for token in [Self.primary, Self.secondary, Self.accent, Self.surface, Self.teamA, Self.teamB] {
            if let value = layer(token), value != Self.auto { resolved[token] = value }
        }
        for token in [Self.onPrimary, Self.onSecondary, Self.onAccent] {
            var value = layer(token) ?? Self.auto
            if value == Self.auto {
                guard let plate = resolved[Self.onColourPlates[token]!] else { continue }
                value = Self.autoOn(plate)
            }
            resolved[token] = value
        }
        var muted = layer(Self.onPrimaryMuted) ?? Self.auto
        if muted == Self.auto, let name = resolved[Self.onPrimary] {
            muted = Self.contrast(name, Self.white) <= Self.contrast(name, Self.nearBlack) ? Self.mutedOnDark : Self.mutedOnLight
        }
        if muted != Self.auto { resolved[Self.onPrimaryMuted] = muted }
        return resolved
    }

    static func validOverrides(_ value: Any?) -> Bool {
        guard let value, !(value is NSNull) else { return true }
        guard let map = value as? JSONObject, map.count <= tokens.count else { return false }
        return map.allSatisfy { tokens.contains($0.key) && isColor($0.value as? String) }
    }

    /// The resolved map a core node reported (nil for null or absent).
    static func fromWire(_ value: Any?) -> [String: String]? { (value as? JSONObject)?.compactMapValues { $0 as? String } }

    static func sameResolved(_ a: [String: String]?, _ b: [String: String]?) -> Bool {
        (a?.count ?? 0) == (b?.count ?? 0) && (a ?? [:]).allSatisfy { b?[$0.key]?.uppercased() == $0.value.uppercased() }
    }
}

/// GraphicsPaletteMigration.
enum GraphicsPaletteMigration {
    static func declarative(_ templateId: String) -> Bool { !["compact-solid", "minimal-accent", "broadcast"].contains(templateId) }

    static func fromBrandKit(brandColor: String, accentColor: String, backgroundColor: String, stock: Bool) -> GraphicsPalette {
        guard !stock, let primary = GraphicsPalette.fromHex(backgroundColor, opacity: 0.9), let secondary = GraphicsPalette.fromHex(brandColor),
              let accent = GraphicsPalette.fromHex(accentColor), let surface = GraphicsPalette.fromHex(backgroundColor, opacity: 0.7)
        else { return .stock }
        return GraphicsPalette(colors: [
            GraphicsPalette.primary: primary, GraphicsPalette.secondary: secondary, GraphicsPalette.accent: accent, GraphicsPalette.surface: surface,
            GraphicsPalette.onPrimary: GraphicsPalette.auto, GraphicsPalette.onPrimaryMuted: GraphicsPalette.auto,
            GraphicsPalette.onSecondary: GraphicsPalette.auto, GraphicsPalette.onAccent: GraphicsPalette.auto])
    }

    /// GraphicsPaletteMigration.AppearanceTokens, with LowerThirdAppearance's defaults for missing keys.
    static func appearanceTokens(_ style: JSONObject) -> [String: String] {
        func colour(_ key: String, _ fallback: String) -> String { style[key] as? String ?? fallback }
        let opacity = (style["backgroundOpacity"] as? NSNumber)?.doubleValue ?? 0.9
        var out: [String: String] = [:]
        out[GraphicsPalette.primary] = GraphicsPalette.fromHex(colour("backgroundColor", "#0C1118"), opacity: opacity)
        out[GraphicsPalette.onPrimary] = GraphicsPalette.fromHex(colour("nameColor", "#F4F7FA"))
        out[GraphicsPalette.onPrimaryMuted] = GraphicsPalette.fromHex(colour("titleColor", "#44C1A1"))
        out[GraphicsPalette.accent] = GraphicsPalette.fromHex(colour("accentColor", "#44C1A1"))
        return out
    }

    /// Overrides that keep a declarative row's pre-palette colours (nil = follows the palette).
    static func migratedOverrides(templateId: String, style: JSONObject?, brandOverrides: Any?, palette: GraphicsPalette) -> [String: String]? {
        if let existing = brandOverrides as? JSONObject { return existing.compactMapValues { $0 as? String } }
        // A row without a style key has LowerThirdAppearance's defaults, as the C# record does.
        let style = style ?? [:]
        guard !templateId.isEmpty, declarative(templateId), GraphicsRow.validStyle(style) else { return nil }
        let resolved = palette.resolve()
        let overrides = appearanceTokens(style).filter { resolved[$0.key]?.uppercased() != $0.value.uppercased() }
        return overrides.isEmpty ? nil : overrides
    }
}

extension GraphicsRow {
    /// Per-row colour overrides laid over the show palette (nil = follow the palette).
    var brandOverrides: [String: String]? {
        get { (json["brandOverrides"] as? JSONObject)?.compactMapValues { $0 as? String } }
        set { json["brandOverrides"] = newValue }
    }
}

extension GraphicsDocument {
    /// The show palette; nil for a show written before the palette.
    var palette: GraphicsPalette? {
        get { (json["palette"] as? JSONObject).map(GraphicsPalette.init(json:)) }
        set { json["palette"] = newValue?.json }
    }
    /// The palette playout resolves against. The Mac has no app default store yet (model only),
    /// so a pre-palette show resolves against the stock default, as Windows does without one.
    var effectivePalette: GraphicsPalette { palette ?? .stock }

    /// A row's resolved palette. A pre-palette show is migrated exactly as GraphicsWorkspaceLibrary
    /// does at load (each declarative row keeps its appearance colours as overrides where they
    /// differ from the effective palette); the Mac applies it on read and never rewrites the file.
    func resolvedPalette(for row: GraphicsRow) -> [String: String] {
        let effective = effectivePalette
        let overrides = palette != nil ? row.brandOverrides
            : GraphicsPaletteMigration.migratedOverrides(templateId: row.templateId, style: row.style, brandOverrides: row.json["brandOverrides"], palette: effective)
        return effective.resolve(overrides)
    }
}
