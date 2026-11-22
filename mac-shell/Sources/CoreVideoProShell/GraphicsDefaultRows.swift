// GraphicsDefaultRows — Windows' five default playlist rows for the "interview"
// starter (GraphicsWorkspaceLibrary.DefaultPlaylistFor + GraphicsShowStarters.Prepare,
// MC/Services/GraphicsWorkspaceLibrary.Playlist.cs:11-34). Plan D6: the Mac adds
// them only on an explicit "Add default rows", never automatically (Windows seeds
// through its starter picker; the Mac has none until G8). Built from the core's
// own template catalog, so field defaults follow the installed templates exactly
// as on Windows. The JSON matches what the Windows store writes, key for key.
// Conformance: contracts/graphics-shell/playout.vectors.json "default-rows-interview".

import Foundation

enum GraphicsDefaultRows {
    static let starterName = "Interview", lowerThird = "minimal-accent", accent = "#44C1A1"
    static let heading = "In conversation", secondary = "Stories worth sharing"
    static let unavailable = "Interview is unavailable in this version of CoreVideo. Update the app and try again."

    /// LowerThirdAppearance.FromPreset, serialized as Windows writes it (isValid included).
    static func appearance(preset id: String) -> JSONObject {
        var style: JSONObject = [
            "version": 1, "preset": "compact-solid", "anchor": "lower-left", "fontFamily": "Segoe UI",
            "nameSize": 42, "titleSize": 28, "nameColor": "#F4F7FA", "titleColor": "#44C1A1",
            "backgroundColor": "#0C1118", "accentColor": "#44C1A1", "backgroundOpacity": 0.9, "padding": 20,
            "width": 0.6, "cornerRadius": 8, "safeOffsetX": 0.05, "safeOffsetY": 0.06, "showLogo": true, "logoScale": 1, "isValid": true,
        ]
        switch id {
        case "minimal-accent":
            style["preset"] = id; style["backgroundOpacity"] = 0; style["nameSize"] = 40; style["titleSize"] = 26; style["cornerRadius"] = 0
        case "broadcast":
            style["preset"] = id; style["nameSize"] = 52; style["titleSize"] = 32; style["padding"] = 24; style["width"] = 0.75
            style["cornerRadius"] = 0; style["backgroundOpacity"] = 1
        default: break
        }
        return style
    }

    /// One prepared starter graphic: its template pin, fields and style (Prepare).
    struct Graphic { var templateId: String; var templateVersion: Int; var fields: JSONObject; var style: JSONObject }

    static func prepare(_ state: GraphicsState) -> [Graphic]? {
        var graphics: [Graphic] = []
        for id in [lowerThird, "centered-title", "corner-logo", "clock-panel"] {
            guard let template = state.templates.filter({ $0.templateId == id }).max(by: { $0.templateVersion < $1.templateVersion })
            else { return nil }
            var fields: JSONObject
            if template.runtime == "declarative" {
                fields = [:]
                for field in template.fields { fields[field.id] = field.defaultValue ?? "" }
            } else {
                fields = ["name": "Alex Rivera", "title": "Host · " + starterName]
            }
            func sample(_ key: String, _ value: String) { if fields[key] != nil { fields[key] = value } }
            sample("name", "Alex Rivera"); sample("title", "Host · " + starterName)
            sample("heading", heading); sample("subtitle", secondary)
            sample("label", "LIVE"); sample("time", "12:34:56")
            var style = appearance(preset: template.templateId)
            style["accentColor"] = accent; style["backgroundColor"] = "#172433"; style["nameColor"] = "#FFFFFF"; style["titleColor"] = "#E1E9F2"
            graphics.append(Graphic(templateId: template.templateId, templateVersion: template.templateVersion, fields: fields, style: style))
        }
        return graphics
    }

    /// The five rows, or nil when the core's catalog lacks a starter template.
    static func interview(_ state: GraphicsState) -> [GraphicsRow]? {
        guard let g = prepare(state) else { return nil }
        func row(_ id: String, _ kind: String, _ name: String, _ hotkey: String, _ playOut: Int, _ graphic: Graphic?,
                 followSources: Bool = false, showClock: Bool = false) -> GraphicsRow {
            var sponsorStyle = appearance(preset: lowerThird)
            sponsorStyle["accentColor"] = accent
            return GraphicsRow(json: [
                "id": id, "kind": kind, "name": name, "hotkey": hotkey, "playOutAfterMs": playOut,
                "templateId": graphic?.templateId ?? "", "templateVersion": graphic?.templateVersion ?? 0,
                "fields": graphic?.fields ?? [:], "style": graphic?.style ?? sponsorStyle,
                "imagePath": NSNull(), "image": NSNull(), "followSources": followSources, "dataBinding": NSNull(),
                "sourceMappings": [:] as JSONObject, "showClock": showClock, "motion": NSNull(), "variants": [] as [Any],
                "placement": NSNull(), "clock": NSNull(),
            ])
        }
        var liveBug = g[3]
        if liveBug.fields["label"] != nil { liveBug.fields["label"] = "LIVE" }
        return [
            row("guest", GraphicsRow.guest, "Guest name", "F1", 8000, g[0], followSources: true),
            row("title", GraphicsRow.title, "Show title", "F2", 0, g[1]),
            row("logo", GraphicsRow.logo, "Logo", "F3", 0, g[2]),
            row("live-bug", GraphicsRow.liveBug, "LIVE bug", "F4", 0, liveBug, showClock: true),
            row("sponsor", GraphicsRow.sponsor, "Sponsor card", "F5", 15000, nil),
        ]
    }
}
