// GraphicsLayouts — Shot Layouts (G4-G7 Task 13): a scene to Preview plus the playlist rows that ride
// the Take with it. Ports of MC/Models/GraphicsPlaylist.cs ShotLayout (IsValid), ShotLayoutRowSettings
// (IsValid), GraphicsWorkspaceLibrary.DefaultShotLayouts / SaveShotLayoutsAsync, the WinUI control's
// Slug and GraphicsPlayoutActions LayoutSet / LayoutRemove. The layout's JSON is kept as read, so a
// Windows layout round-trips byte-faithfully. Pinned by contracts/graphics-shell/layouts.vectors.json.

import Foundation

/// ShotLayoutRowSettings: an optional data pin and typed overrides for one row of a layout.
struct GraphicsLayoutRowSettings: Equatable {
    var json: JSONObject
    init(json: JSONObject) { self.json = json }
    static func == (a: GraphicsLayoutRowSettings, b: GraphicsLayoutRowSettings) -> Bool { (a.json as NSDictionary).isEqual(to: b.json) }

    var dataPin: (sourceId: String, revision: Int64)? {
        guard let pin = json["dataPin"] as? JSONObject else { return nil }
        return (pin["sourceId"] as? String ?? "", GraphicsDataBinding.int64(pin["revision"]) ?? 0)
    }
    /// The overrides (ordinal key order: the document's own order is kept in `json`).
    var overrides: [(key: String, value: Any)] {
        (json["overrides"] as? JSONObject ?? [:]).sorted { $0.key < $1.key }.map { ($0.key, $0.value) }
    }

    var isValid: Bool {
        if let raw = json["dataPin"], !(raw is NSNull) {
            guard raw is JSONObject, let pin = dataPin, GraphicsRow.validId(pin.sourceId), pin.revision > 0 else { return false }
        }
        guard let overrides = (json["overrides"] ?? [:] as JSONObject) as? JSONObject, overrides.count <= 32 else { return false }
        let hasPin = json["dataPin"] is JSONObject
        guard hasPin || !overrides.isEmpty else { return false }
        return overrides.allSatisfy { GraphicsRow.validVariableId($0.key) && Self.isScalar($0.value) }
    }

    /// A boolean, a finite number, or a string of at most 4096 UTF-8 bytes.
    static func isScalar(_ value: Any) -> Bool {
        if let n = value as? NSNumber { return CFGetTypeID(n) == CFBooleanGetTypeID() || n.doubleValue.isFinite }
        if let s = value as? String { return s.utf8.count <= 4096 }
        return false
    }
}

/// ShotLayout: one button that cues a scene to Preview and stages rows with it.
struct GraphicsShotLayout: Equatable, Identifiable {
    var json: JSONObject
    init(json: JSONObject) { self.json = json }
    static func == (a: GraphicsShotLayout, b: GraphicsShotLayout) -> Bool { (a.json as NSDictionary).isEqual(to: b.json) }

    var id: String { get { json["id"] as? String ?? "" } set { json["id"] = newValue } }
    var name: String { get { json["name"] as? String ?? "" } set { json["name"] = newValue } }
    var sceneId: String { get { json["sceneId"] as? String ?? "" } set { json["sceneId"] = newValue } }
    var rowIds: [String] { get { json["rowIds"] as? [String] ?? [] } set { json["rowIds"] = newValue } }
    var hotkey: String { get { json["hotkey"] as? String ?? "" } set { json["hotkey"] = newValue } }
    var playOutOthers: Bool { get { GraphicsJSON.bool(json["playOutOthers"]) } set { json["playOutOthers"] = newValue } }
    var rowSettings: [String: GraphicsLayoutRowSettings] {
        get { (json["rowSettings"] as? JSONObject ?? [:]).compactMapValues { ($0 as? JSONObject).map(GraphicsLayoutRowSettings.init(json:)) } }
        set { json["rowSettings"] = newValue.mapValues(\.json) }
    }

    static func validHotkey(_ value: String) -> Bool {
        guard !value.isEmpty else { return true }
        guard value.hasPrefix("Shift+F"), let n = Int(value.dropFirst(7)), String(n) == value.dropFirst(7) else { return false }
        return (1...11).contains(n)
    }

    var isValid: Bool {
        typealias V = GraphicsValue
        guard let id = V.string(json, "id", ""), GraphicsRow.validId(id),
              let name = V.string(json, "name", ""), (1...128).contains(name.utf16.count),
              let scene = V.string(json, "sceneId", ""), scene.utf16.count <= 64,
              let hotkey = V.string(json, "hotkey", ""), Self.validHotkey(hotkey),
              let rows = V.array(json, "rowIds", []), rows.count <= 16
        else { return false }
        let ids = rows.compactMap { $0 as? String }
        guard ids.count == rows.count, ids.allSatisfy(GraphicsRow.validId), Set(ids).count == ids.count else { return false }
        if let flag = json["playOutOthers"], !(flag is NSNull) {
            guard let n = flag as? NSNumber, CFGetTypeID(n) == CFBooleanGetTypeID() else { return false }
        }
        guard let settings = V.object(json, "rowSettings", [:]), settings.count <= 16 else { return false }
        return settings.allSatisfy { key, value in
            ids.contains(key) && (value as? JSONObject).map { GraphicsLayoutRowSettings(json: $0).isValid } == true
        }
    }
}

enum GraphicsLayouts {
    static let invalidText = "That shot layout is not valid."
    static let unknownText = "That shot layout does not exist."

    /// GraphicsWorkspaceLibrary.DefaultShotLayouts: Windows' scene ids, kept on a Mac show so the file is
    /// identical on both platforms (owner decision 2); a missing scene is refused at Cue.
    static func defaultLayouts() -> [GraphicsShotLayout] {
        func layout(_ id: String, _ name: String, _ scene: String, _ rows: [String], _ hotkey: String, _ playOut: Bool = false) -> GraphicsShotLayout {
            GraphicsShotLayout(json: ["id": id, "name": name, "sceneId": scene, "rowIds": rows, "rowSettings": [:] as JSONObject,
                                      "hotkey": hotkey, "playOutOthers": playOut])
        }
        return [layout("open", "Open", "intro", ["title", "logo"], "Shift+F1"),
                layout("two-up", "Two-up", "interview", ["guest"], "Shift+F2"),
                layout("panel", "Panel", "panel", ["guest", "logo"], "Shift+F3"),
                layout("clean", "Clean", "", [], "Shift+F11", true)]
    }

    /// The WinUI control's Slug: lower-case a-z0-9 runs joined by dashes, "layout" in front of a
    /// leading digit or an empty result, at most 40 characters, made unique with -2, -3, ...
    static func slug(_ name: String, taken: [String]) -> String {
        let mapped = String(GraphicsNamesList.lowerInvariant(name).utf16.map { unit -> Character in
            (unit >= 0x61 && unit <= 0x7A) || (unit >= 0x30 && unit <= 0x39) ? Character(Unicode.Scalar(unit)!) : "-"
        })
        var slug = mapped.split(separator: "-").joined(separator: "-")
        if slug.isEmpty || !("a"..."z").contains(slug.first!) { slug = "layout" + (slug.isEmpty ? "" : "-" + slug) }
        if slug.count > 40 { slug = String(slug.prefix(40)); while slug.hasSuffix("-") { slug.removeLast() } }
        let used = Set(taken)
        var candidate = slug, n = 2
        while used.contains(candidate) { candidate = "\(slug)-\(n)"; n += 1 }
        return candidate
    }

    /// SaveShotLayoutsAsync's checks (nil = acceptable).
    static func saveRefusal(_ layouts: [GraphicsShotLayout], playlist: [GraphicsRow]) -> String? {
        guard layouts.count <= 32, layouts.allSatisfy(\.isValid), Set(layouts.map(\.id)).count == layouts.count else {
            return "A shot layout is invalid or duplicated. Previous layouts retained."
        }
        let rowIds = Set(playlist.map(\.id))
        var missing: [String] = []
        for id in layouts.flatMap(\.rowIds) where !rowIds.contains(id) && !missing.contains(id) { missing.append(id) }
        if !missing.isEmpty {
            return "Shot layouts reference playlist rows that do not exist: \(missing.joined(separator: ", ")). Previous layouts retained."
        }
        let keys = layouts.map(\.hotkey).filter { !$0.isEmpty }
        if Set(keys).count != keys.count { return "Two shot layouts share a hotkey. Previous layouts retained." }
        return nil
    }
}

extension GraphicsDocument {
    var shotLayouts: [GraphicsShotLayout] {
        get { (json["shotLayouts"] as? [JSONObject] ?? []).map(GraphicsShotLayout.init(json:)) }
        set { json["shotLayouts"] = newValue.map(\.json) }
    }
}

extension GraphicsModel {
    /// GraphicsPlayoutActions LayoutSet: validity, the scene check against the Mac's scenes, the hotkey moves
    /// from any other layout, then the store's layout checks. nil = saved; else Windows' message.
    func setLayout(_ layout: GraphicsShotLayout) -> String? {
        guard layout.isValid else { return GraphicsLayouts.invalidText }
        if !layout.sceneId.isEmpty && !sceneIds().contains(layout.sceneId) { return "Scene '\(layout.sceneId)' is not in this show." }
        var layouts = document.shotLayouts.map { other -> GraphicsShotLayout in
            var other = other
            if other.id != layout.id && !layout.hotkey.isEmpty && other.hotkey == layout.hotkey { other.hotkey = "" }
            return other
        }
        if let index = layouts.firstIndex(where: { $0.id == layout.id }) { layouts[index] = layout } else { layouts.append(layout) }
        return saveLayouts(layouts)
    }

    func removeLayout(id: String) -> String? {
        let layouts = document.shotLayouts.filter { $0.id != id }
        if layouts.count == document.shotLayouts.count { return GraphicsLayouts.unknownText }
        return saveLayouts(layouts)
    }

    private func saveLayouts(_ layouts: [GraphicsShotLayout]) -> String? {
        if let refusal = GraphicsLayouts.saveRefusal(layouts, playlist: document.playlist) { return refusal }
        document.shotLayouts = layouts
        scheduleSave()
        return nil
    }
}
