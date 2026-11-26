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

// ── the resolver (GraphicsLayoutResolver.cs, Task 14) ───────────────────────

/// GraphicsLayoutResolver.ResolvedRow: the row with its cue fields resolved (data pin, then overrides).
struct GraphicsResolvedRow: Equatable {
    var row: GraphicsRow
    var identity: GraphicsSourceIdentity?
    var dataRevision: Int64?
    var dataReceivedAt: Date?
}

extension GraphicsLayouts {
    /// GraphicsLayoutResolver.Resolve: pure preflight for one row of a data-aware layout. Throws
    /// GraphicsDataError with Windows' message. (The Sources-column mapping is out of scope, owner
    /// decision 6.)
    static func resolve(document: GraphicsDocument, state: GraphicsState, layout: GraphicsShotLayout,
                        entry: GraphicsPlaylistEntry) throws -> GraphicsResolvedRow {
        guard var row = document.row(entry.rowId) else { throw GraphicsDataError(message: "That playlist row no longer exists.") }
        let image = row.image
        let templateId = image.map { $0["templateId"] as? String ?? "" } ?? row.templateId
        let version = image.map { ($0["templateVersion"] as? NSNumber)?.intValue ?? 0 } ?? row.templateVersion
        let template = state.template(templateId, version)
        let pin = document.templates.first { $0.templateId == templateId && $0.templateVersion == version }
        if templateId.isEmpty || (template == nil && pin == nil) { throw GraphicsDataError(message: "\(row.name): the selected template is unavailable.") }
        if let template, let pin, template.manifestHash != pin.manifestHash {
            throw GraphicsDataError(message: "\(row.name): the installed template differs from its saved pin.")
        }
        let identity = state.sources.first { $0.videoOn && $0.identity?.sourceId == entry.sourceId }?.identity
        if !entry.sourceId.isEmpty && !entry.isVariant && identity == nil {
            throw GraphicsDataError(message: "\(row.name): the selected source is unavailable.")
        }
        let descriptors = GraphicsRowFields.templateFields(row, state: state, pins: document.templates)
        let declarative = template?.runtime == "declarative" || (template == nil && pin?.definition["layers"] != nil)
        var values = GraphicsPlayout.cueFields(row, entry, declarative: declarative, descriptors: descriptors)
        let settings = layout.rowSettings[row.id]
        if let dataPin = settings?.dataPin { try applyDataPin(document, row, dataPin, &values) }
        for (id, value) in settings?.overrides ?? [] {
            guard let field = descriptors.first(where: { $0.id == id }), GraphicsRowFields.validLayoutOverride(field, value) else {
                throw GraphicsDataError(message: "\(row.name): '\(id)' is unavailable or the value does not match the selected template.")
            }
            values[id] = value
        }
        let binding = row.dataBinding.flatMap(GraphicsDataBinding.init(json:))
        let revision = settings?.dataPin?.revision ?? binding?.revision
        let received = document.dataSources.first { $0.id == binding?.sourceId && $0.revision == revision }?.receivedAt
        row.fields = values
        return GraphicsResolvedRow(row: row, identity: identity, dataRevision: revision, dataReceivedAt: received)
    }

    private static func applyDataPin(_ document: GraphicsDocument, _ row: GraphicsRow, _ pin: (sourceId: String, revision: Int64),
                                     _ values: inout JSONObject) throws {
        guard var binding = row.dataBinding.flatMap(GraphicsDataBinding.init(json:)), binding.sourceId == pin.sourceId else {
            throw GraphicsDataError(message: "\(row.name): the selected data source changed.")
        }
        if binding.revision == pin.revision { return }
        guard let source = document.dataSources.first(where: { $0.id == pin.sourceId && $0.revision == pin.revision }) else {
            throw GraphicsDataError(message: "\(row.name): data revision \(pin.revision) is unavailable.")
        }
        binding.revision = pin.revision
        do {
            for (id, value) in try GraphicsDataTable.fields(source.data, binding) { values[id] = value }
        } catch let error as GraphicsDataError {
            throw GraphicsDataError(message: "\(row.name): \(error.message)")
        }
    }
}

// ── the band and the editor, pure (Task 16) ─────────────────────────────────

/// One button of the SHOT LAYOUTS header (GraphicsPlayoutControl.RefreshLayouts :228-251).
struct GraphicsLayoutHeaderButton: Equatable {
    enum Kind: Equatable { case take, cancel, edit }
    let kind: Kind
    let title: String
    let enabled: Bool
}

extension GraphicsLayouts {
    /// LayoutSummary (GraphicsPlayoutControl.cs:198-206): "<scene> · <graphics>", "(missing)" for a
    /// scene or row that is gone.
    static func summary(_ layout: GraphicsShotLayout, rows: [GraphicsRow], scenes: [(id: String, name: String)]) -> String {
        let scene = layout.sceneId.isEmpty ? "No scene change"
            : scenes.first { $0.id == layout.sceneId }?.name ?? layout.sceneId + " (missing)"
        let names = layout.rowIds.map { id in rows.first { $0.id == id }?.name ?? id + " (missing)" }
        let graphics = names.isEmpty ? (layout.playOutOthers ? "plays everything out" : "no graphics")
            : names.joined(separator: ", ") + (layout.playOutOthers ? " · hides the rest" : "")
        return scene + " · " + graphics
    }

    /// The header: Take ("Check Take" while a guarded Take awaits its receipt, "Take" for the prepared
    /// guarded layout, "Cue and Take" for a selected legacy layout; never for an unprepared guarded one),
    /// "Cancel layout" while progress shows (off while a Take is pending), "Edit layouts" always.
    static func headerButtons(selected: GraphicsShotLayout?, prepared: GraphicsPreparedLayout?, takePending: Bool,
                              hasProgress: Bool, busy: Bool) -> [GraphicsLayoutHeaderButton] {
        var buttons: [GraphicsLayoutHeaderButton] = []
        if let selected = prepared?.layout ?? selected, selected.rowSettings.isEmpty || prepared != nil {
            let title = takePending ? "Check Take" : !selected.rowSettings.isEmpty ? "Take" : "Cue and Take"
            buttons.append(GraphicsLayoutHeaderButton(kind: .take, title: title, enabled: !busy))
        }
        if hasProgress { buttons.append(GraphicsLayoutHeaderButton(kind: .cancel, title: "Cancel layout", enabled: !takePending)) }
        buttons.append(GraphicsLayoutHeaderButton(kind: .edit, title: "Edit layouts", enabled: !busy))
        return buttons
    }

    static let hotkeyChoices = ["None"] + (1...11).map { "Shift+F\($0)" }
}

/// AddLayoutRowEditor (GraphicsPlayoutControl.Layouts.cs:18-110) as a pure model: the data revision
/// picker, one override per overridable field, the invalid and unavailable saved overrides.
struct GraphicsLayoutRowEditor {
    struct RevisionOption: Equatable {
        let label: String
        let pin: GraphicsLayoutPin?
    }
    struct FieldEditor {
        let field: GraphicsTemplateField
        let title: String
        var enabled: Bool
        var value: Any
    }
    struct Unavailable {
        let key: String
        let value: Any
        var remove = false
    }

    let row: GraphicsRow
    /// False when the row has nothing to edit (no data binding, no overridable field, nothing saved).
    let visible: Bool
    let revisionOptions: [RevisionOption]
    var selectedRevision: Int
    /// "Remove unavailable data pin": a saved pin on a row that is no longer a data row.
    let hasOrphanPin: Bool
    var removePin = false
    var fieldEditors: [FieldEditor]
    /// Saved overrides the template no longer accepts as they are: kept verbatim unless replaced.
    let invalid: [(key: String, value: Any)]
    var unavailable: [Unavailable]
    var included: Bool
    private let saved: GraphicsLayoutRowSettings?

    /// `fields`: the row's template fields (only canOverrideLayoutField ones are used).
    init(row: GraphicsRow, fields: [GraphicsTemplateField], dataSources: [GraphicsDataSourceRecord], saved: GraphicsLayoutRowSettings?, included: Bool) {
        self.row = row; self.saved = saved; self.included = included
        let fields = fields.filter(GraphicsRowFields.canOverrideLayoutField)
        let binding = row.dataBinding.flatMap(GraphicsDataBinding.init(json:))
        visible = !(row.dataBinding == nil && fields.isEmpty && saved == nil)
        let savedPin = saved?.dataPin.map { GraphicsLayoutPin(sourceId: $0.sourceId, revision: $0.revision) }
        var options: [RevisionOption] = []
        if let binding {
            options = [RevisionOption(label: "Use published data (revision \(binding.revision))", pin: nil),
                       RevisionOption(label: "Pin revision \(binding.revision)", pin: GraphicsLayoutPin(sourceId: binding.sourceId, revision: binding.revision))]
            if let loaded = dataSources.first(where: { $0.id == binding.sourceId }), loaded.revision != binding.revision {
                options.append(RevisionOption(label: "Pin revision \(loaded.revision)", pin: GraphicsLayoutPin(sourceId: binding.sourceId, revision: loaded.revision)))
            }
            if let savedPin, !options.contains(where: { $0.pin == savedPin }) {
                options.append(RevisionOption(label: "Revision \(savedPin.revision) unavailable", pin: savedPin))
            }
        }
        revisionOptions = options
        selectedRevision = max(0, options.firstIndex { $0.pin == savedPin } ?? 0)
        hasOrphanPin = binding == nil && savedPin != nil
        let overrides = Dictionary((saved?.overrides ?? []).map { ($0.key, $0.value) }, uniquingKeysWith: { a, _ in a })
        var invalid: [(key: String, value: Any)] = []
        fieldEditors = fields.map { field in
            var value = overrides[field.id]
            let hasOverride = value != nil
            if value == nil || !GraphicsRowFields.validLayoutOverride(field, value!) {
                if hasOverride { invalid.append((field.id, value!)) }
                value = row.fields[field.id] ?? field.defaultValue
            }
            let replace = invalid.contains { $0.key == field.id }
            return FieldEditor(field: field, title: (replace ? "Replace invalid " : "Override ") + GraphicsRowFields.humanize(field.id),
                               enabled: hasOverride && !replace, value: value ?? "")
        }
        self.invalid = invalid
        unavailable = (saved?.overrides ?? []).filter { key, _ in !fields.contains { $0.id == key } }.map { Unavailable(key: $0.key, value: $0.value) }
    }

    /// The settings to save for this row: nil when unticked or when there is no pin and no override.
    func read() throws -> GraphicsLayoutRowSettings? {
        guard included else { return nil }
        var overrides: JSONObject = [:]
        for (key, value) in invalid { overrides[key] = value }
        for item in unavailable where !item.remove { overrides[item.key] = item.value }
        for editor in fieldEditors where editor.enabled {
            guard GraphicsRowFields.validLayoutOverride(editor.field, editor.value) else {
                throw GraphicsDataError(message: "\(row.name): check the value for \(editor.field.id).")
            }
            overrides[editor.field.id] = editor.value
        }
        let pin: GraphicsLayoutPin? = !revisionOptions.isEmpty
            ? revisionOptions[min(max(selectedRevision, 0), revisionOptions.count - 1)].pin
            : removePin ? nil : saved?.dataPin.map { GraphicsLayoutPin(sourceId: $0.sourceId, revision: $0.revision) }
        if pin == nil && overrides.isEmpty { return nil }
        var json: JSONObject = ["overrides": overrides]
        if let pin { json["dataPin"] = ["sourceId": pin.sourceId, "revision": pin.revision] }
        return GraphicsLayoutRowSettings(json: json)
    }
}

struct GraphicsLayoutPin: Equatable {
    let sourceId: String
    let revision: Int64
}
