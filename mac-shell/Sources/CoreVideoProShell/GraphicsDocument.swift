// GraphicsDocument — the schema-3 graphics show document and its atomic store
// (graphics G2, spec 5.6). Port of MC/Services/GraphicsDocumentStore.cs within
// plan default D3:
//
//   * The document is kept as the JSON object it was read from. The Mac TYPES
//     only what it edits (playlist rows) and what it reads (template pins);
//     every other section and every unknown key is written back verbatim, so a
//     Mac without the Designer never strips a Windows show's designs, data,
//     feeds or future sections.
//   * Validation covers the top level, the playlist rows (a port of
//     GraphicsPlaylistRow.IsValid, MC/Models/GraphicsPlaylist.cs:62-74) and the
//     template pins. Unported sections are bounded (count, unique ids) but not
//     re-validated: Windows wrote them. A row's dataBinding is validated in
//     full (GraphicsDataBinding.Valid) and may appear only on a title row
//     without variants; the dataSources and designs sections the Mac writes
//     (G4-G7 Task 7) are validated as Windows validates them.
//   * Load/save semantics match the C# exactly: missing file = new document,
//     invalid primary -> last complete .bak, schema newer than 3 = read-only with
//     the original untouched, v1/v2 load as 3 and the first save keeps one
//     .v2.bak, duplicate keys refused, 2 MiB cap, atomic replace keeping .bak,
//     a save refused when the file changed on disk since load.
//
// Path: ~/Library/Application Support/CoreVideoPro/graphics/default.json, or
// COREVIDEO_GRAPHICS_DOCUMENT_PATH (Windows parity, plan D4).
// Conformance: contracts/graphics-shell/document.vectors.json.

import CryptoKit
import Foundation

struct GraphicsDocumentError: Error, Equatable { let message: String }

// ── playlist rows ────────────────────────────────────────────────────────────

/// One playlist row, held as its JSON object so keys the Mac does not know
/// survive an edit. Accessors default exactly as the C# record does.
struct GraphicsRow: Equatable, Identifiable {
    static let guest = "guest", title = "title", logo = "logo", liveBug = "liveBug", sponsor = "sponsor"
    static let kinds = [guest, title, logo, liveBug, sponsor]
    static let playOutAfterChoices = [0, 8000, 15000]

    var json: JSONObject

    init(json: JSONObject) { self.json = json }
    static func == (a: GraphicsRow, b: GraphicsRow) -> Bool { (a.json as NSDictionary).isEqual(to: b.json) }

    var id: String { get { json["id"] as? String ?? "" } set { json["id"] = newValue } }
    var kind: String { get { json["kind"] as? String ?? Self.title } set { json["kind"] = newValue } }
    var name: String { get { json["name"] as? String ?? "" } set { json["name"] = newValue } }
    var hotkey: String { get { json["hotkey"] as? String ?? "" } set { json["hotkey"] = newValue } }
    var playOutAfterMs: Int { get { (json["playOutAfterMs"] as? NSNumber)?.intValue ?? 0 } set { json["playOutAfterMs"] = newValue } }
    var templateId: String { get { json["templateId"] as? String ?? "" } set { json["templateId"] = newValue } }
    var templateVersion: Int { get { (json["templateVersion"] as? NSNumber)?.intValue ?? 0 } set { json["templateVersion"] = newValue } }
    var fields: JSONObject { get { json["fields"] as? JSONObject ?? [:] } set { json["fields"] = newValue } }
    var followSources: Bool { GraphicsJSON.bool(json["followSources"]) }
    var showClock: Bool { GraphicsJSON.bool(json["showClock"]) }
    var variants: [JSONObject] { json["variants"] as? [JSONObject] ?? [] }
    var hasVariants: Bool { !variants.isEmpty }
    var style: JSONObject? { json["style"] as? JSONObject }
    var motion: JSONObject? { json["motion"] as? JSONObject }
    var placement: JSONObject? { json["placement"] as? JSONObject }
    var clock: JSONObject? { json["clock"] as? JSONObject }
    var image: JSONObject? { json["image"] as? JSONObject }
    var dataBinding: JSONObject? { json["dataBinding"] as? JSONObject }
    /// A remote feed row: needs the feed poller the Mac does not have until G9 (plan D7). Local tables
    /// and crawls, live follow included, play from G4-G7 Slice C.
    var needsFeedPlayback: Bool {
        guard let binding = dataBinding else { return false }
        return GraphicsJSON.bool(binding["remote"])
    }

    var stripRow: GraphicsStripRow {
        GraphicsStripRow(id: id, kind: kind, name: name, variantIds: variants.compactMap { $0["id"] as? String })
    }

    // ── validity (GraphicsPlaylistRow.IsValid) ─────────────────────────────

    static func validVariableId(_ value: String) -> Bool { GraphicsCommandRequest.validId(value) }
    static func validId(_ value: String) -> Bool { value.utf16.count <= 48 && validVariableId(value) }
    static func validHotkey(_ value: String) -> Bool {
        value.isEmpty || (value.hasPrefix("F") && Int(value.dropFirst()).map { (1...12).contains($0) && String($0) == value.dropFirst() } == true)
    }

    var isValid: Bool { Self.validRow(json) }

    static func validRow(_ r: JSONObject) -> Bool {
        typealias V = GraphicsValue
        guard let id = V.string(r, "id", ""), validId(id),
              let kind = V.string(r, "kind", title), kinds.contains(kind),
              let name = V.string(r, "name", ""), (1...128).contains(name.utf16.count),
              let hotkey = V.string(r, "hotkey", ""), validHotkey(hotkey),
              let playOut = V.int(r, "playOutAfterMs", 0), playOutAfterChoices.contains(playOut),
              let templateId = V.string(r, "templateId", ""), templateId.utf16.count <= 64,
              let version = V.int(r, "templateVersion", 0), version >= 0, templateId.isEmpty == (version == 0),
              V.bool(r, "followSources") != nil, V.bool(r, "showClock") != nil
        else { return false }
        // fields: <= 32 keys of 1..64 chars (values are template-typed; the core validates them).
        guard let fields = V.object(r, "fields", [:]), fields.count <= 32, fields.keys.allSatisfy({ (1...64).contains($0.utf16.count) })
        else { return false }
        guard let mappings = V.object(r, "sourceMappings", [:]), mappings.count <= 30,
              mappings.allSatisfy({ validVariableId($0.key) && ($0.value as? String).map(validVariableId) == true })
        else { return false }
        if let style = r["style"] { guard let style = style as? JSONObject, validStyle(style) else { return false } }
        if let path = r["imagePath"], !(path is NSNull) {
            guard let path = path as? String, path.utf16.count <= 2048, !path.contains("\u{0}") else { return false }
        }
        if let motion = r["motion"], !(motion is NSNull) { guard let motion = motion as? JSONObject, validMotion(motion) else { return false } }
        guard let variants = V.array(r, "variants", []), validVariants(variants) else { return false }
        if let placement = r["placement"], !(placement is NSNull) {
            guard let placement = placement as? JSONObject, validPlacement(placement) else { return false }
        }
        if let clock = r["clock"], !(clock is NSNull) { guard let clock = clock as? JSONObject, validClock(clock) else { return false } }
        if let image = r["image"], !(image is NSNull) {
            guard let image = image as? JSONObject, let t = V.string(image, "templateId", ""), (1...64).contains(t.utf16.count),
                  let v = V.int(image, "templateVersion", 0), v >= 1,
                  let a = V.string(image, "assetId", ""), (1...64).contains(a.utf16.count) else { return false }
        }
        // dataBinding: only a title row without variants may carry one (contents: the data slice).
        if let binding = r["dataBinding"], !(binding is NSNull) {
            guard let object = binding as? JSONObject, GraphicsDataBinding(json: object)?.isValid == true,
                  kind == title, variants.isEmpty else { return false }
        }
        return true
    }

    /// LowerThirdAppearance.IsValid with the record's defaults for missing keys.
    static func validStyle(_ s: JSONObject) -> Bool {
        typealias V = GraphicsValue
        func inRange(_ key: String, _ fallback: Double, _ min: Double, _ max: Double) -> Bool {
            guard let value = V.double(s, key, fallback) else { return false }
            return value.isFinite && value >= min && value <= max
        }
        func colour(_ key: String, _ fallback: String) -> Bool {
            guard let c = V.string(s, key, fallback), c.utf16.count == 7, c.hasPrefix("#") else { return false }
            return c.dropFirst().allSatisfy(\.isHexDigit)
        }
        guard let version = V.int(s, "version", 1), version == 1,
              let preset = V.string(s, "preset", "compact-solid"), ["compact-solid", "minimal-accent", "broadcast"].contains(preset),
              let anchor = V.string(s, "anchor", "lower-left"), LowerThirdPositions.all.contains(where: { $0.value == anchor }),
              let font = V.string(s, "fontFamily", "Segoe UI"), !font.trimmingCharacters(in: .whitespaces).isEmpty, font.utf16.count <= 128,
              V.bool(s, "showLogo", true) != nil
        else { return false }
        return inRange("nameSize", 42, 16, 120) && inRange("titleSize", 28, 12, 80) && inRange("backgroundOpacity", 0.9, 0, 1)
            && inRange("padding", 20, 0, 64) && inRange("width", 0.6, 0.15, 0.95) && inRange("cornerRadius", 8, 0, 60)
            && inRange("safeOffsetX", 0.05, 0, 0.25) && inRange("safeOffsetY", 0.06, 0, 0.25) && inRange("logoScale", 1, 0.5, 2)
            && colour("nameColor", "#F4F7FA") && colour("titleColor", "#44C1A1") && colour("backgroundColor", "#0C1118")
            && colour("accentColor", "#44C1A1")
    }

    static func validTransition(_ t: JSONObject) -> Bool {
        typealias V = GraphicsValue
        guard let type = V.string(t, "type", "none"), ["none", "fade", "slide"].contains(type),
              let direction = V.string(t, "direction", "left"), ["left", "right", "up", "down"].contains(direction),
              let easing = V.string(t, "easing", "linear"), ["linear", "ease-in", "ease-out", "ease-in-out"].contains(easing),
              let duration = V.double(t, "durationMs", 0), duration.isFinite, (0...3000).contains(duration)
        else { return false }
        return true
    }

    static func validMotion(_ m: JSONObject) -> Bool {
        guard GraphicsValue.string(m, "layers", "own") != nil,
              let into = GraphicsValue.object(m, "in", [:]), let out = GraphicsValue.object(m, "out", [:]) else { return false }
        return validTransition(into) && validTransition(out)
    }

    static func validVariants(_ variants: [Any]) -> Bool {
        guard variants.count <= 64 else { return false }
        var ids = Set<String>()
        for item in variants {
            guard let v = item as? JSONObject, let id = GraphicsValue.string(v, "id", ""), validId(id), ids.insert(id).inserted,
                  let name = GraphicsValue.string(v, "name", ""), (1...128).contains(name.utf16.count),
                  let title = GraphicsValue.string(v, "title", ""), title.utf16.count <= 256,
                  let cells = GraphicsValue.object(v, "cells", [:]), cells.count <= 30,
                  cells.allSatisfy({ validVariableId($0.key) && ($0.value as? String).map { $0.utf16.count <= 4096 } == true })
            else { return false }
        }
        return true
    }

    static let placementAnchors = ["upper-left", "upper-center", "upper-right", "center-left", "center", "center-right",
                                   "lower-left", "lower-center", "lower-right"]
    static func validPlacement(_ p: JSONObject) -> Bool {
        guard let anchor = GraphicsValue.string(p, "anchor", "lower-left"), placementAnchors.contains(anchor),
              let x = GraphicsValue.double(p, "x", 96), x.isFinite, abs(x) <= 3840,
              let y = GraphicsValue.double(p, "y", -54), y.isFinite, abs(y) <= 2160,
              let scale = GraphicsValue.double(p, "scale", 1), scale.isFinite, (0.1...4).contains(scale) else { return false }
        return true
    }

    static func validClock(_ c: JSONObject) -> Bool {
        typealias V = GraphicsValue
        guard let mode = V.string(c, "mode", "wall"), ["wall", "countdown"].contains(mode),
              let zone = V.string(c, "zone", "local"), ["local", "utc", "fixed"].contains(zone),
              let format = V.string(c, "format", "hms"), ["hms", "hm"].contains(format),
              let finish = V.string(c, "finish", "hold-zero"), ["hold-zero", "hide", "suggest-next"].contains(finish),
              let offset = V.int(c, "offsetMinutes", 0), (-840...840).contains(offset),
              let duration = V.int(c, "durationSeconds", 300), (1...86400).contains(duration)
        else { return false }
        if let restore = c["restoreRemainingSeconds"], !(restore is NSNull) {
            guard GraphicsJSON.isNumber(restore), let value = (restore as? NSNumber)?.doubleValue, value.isFinite,
                  value >= 0, value <= Double(duration) else { return false }
        }
        return true
    }
}

/// Typed reads that tell "absent" (the C# default) from "wrong type" (a
/// JsonException on Windows, so invalid). nil = invalid.
enum GraphicsValue {
    static func string(_ o: JSONObject, _ key: String, _ fallback: String) -> String? {
        guard let value = o[key] else { return fallback }
        return value as? String
    }
    static func int(_ o: JSONObject, _ key: String, _ fallback: Int) -> Int? {
        guard let value = o[key] else { return fallback }
        guard GraphicsJSON.isNumber(value), let number = (value as? NSNumber)?.doubleValue, number.rounded(.towardZero) == number,
              abs(number) <= Double(Int32.max) else { return nil }
        return Int(number)
    }
    static func double(_ o: JSONObject, _ key: String, _ fallback: Double) -> Double? {
        guard let value = o[key] else { return fallback }
        guard GraphicsJSON.isNumber(value) else { return nil }
        return (value as? NSNumber)?.doubleValue
    }
    static func bool(_ o: JSONObject, _ key: String, _ fallback: Bool = false) -> Bool? {
        guard let value = o[key] else { return fallback }
        return GraphicsJSON.isBool(value) ? (value as? NSNumber)?.boolValue : nil
    }
    static func object(_ o: JSONObject, _ key: String, _ fallback: JSONObject) -> JSONObject? {
        guard let value = o[key] else { return fallback }
        return value as? JSONObject
    }
    static func array(_ o: JSONObject, _ key: String, _ fallback: [Any]) -> [Any]? {
        guard let value = o[key] else { return fallback }
        return value as? [Any]
    }
}

// ── the document ─────────────────────────────────────────────────────────────

struct GraphicsTemplateReference: Equatable {
    var templateId: String
    var templateVersion: Int
}

struct GraphicsTemplatePin: Equatable {
    var templateId: String
    var templateVersion: Int
    var manifestHash: String
    var definition: JSONObject
    var packageRoot: String
    static func == (a: GraphicsTemplatePin, b: GraphicsTemplatePin) -> Bool {
        a.templateId == b.templateId && a.templateVersion == b.templateVersion && a.manifestHash == b.manifestHash
            && a.packageRoot == b.packageRoot && (a.definition as NSDictionary).isEqual(to: b.definition)
    }
}

/// A value: JSONSerialization containers are immutable once parsed, so a copy is safe to hand to the document queue.
struct GraphicsDocument: @unchecked Sendable {
    static let currentSchemaVersion = 3
    /// The whole document as read (or a new one). Only `playlist` is rewritten on save.
    var json: JSONObject

    init(json: JSONObject = ["schemaVersion": GraphicsDocument.currentSchemaVersion]) { self.json = json }

    var schemaVersion: Int { (json["schemaVersion"] as? NSNumber)?.intValue ?? 0 }
    var showId: String { json["showId"] as? String ?? "default" }
    var authority: String { json["authority"] as? String ?? "legacy" }

    var playlist: [GraphicsRow] {
        get { (json["playlist"] as? [JSONObject] ?? []).map(GraphicsRow.init(json:)) }
        set { json["playlist"] = newValue.map(\.json) }
    }

    var templates: [GraphicsTemplatePin] {
        (json["templates"] as? [JSONObject] ?? []).map {
            GraphicsTemplatePin(templateId: $0["templateId"] as? String ?? "", templateVersion: ($0["templateVersion"] as? NSNumber)?.intValue ?? 0,
                                manifestHash: $0["manifestHash"] as? String ?? "", definition: $0["definition"] as? JSONObject ?? [:],
                                packageRoot: $0["packageRoot"] as? String ?? "")
        }
    }

    func row(_ id: String) -> GraphicsRow? { playlist.first { $0.id == id } }

    /// The show's datasets (GraphicsDataSource). Setting replaces only this section; an entry whose
    /// value is unchanged keeps the JSON it was read with (Windows' "+00:00" dates stay as written).
    var dataSources: [GraphicsDataSourceRecord] {
        get { (json["dataSources"] as? [JSONObject] ?? []).compactMap(GraphicsDataSourceRecord.init(json:)) }
        set {
            let existing = json["dataSources"] as? [JSONObject] ?? []
            json["dataSources"] = newValue.map { record in
                existing.first { GraphicsDataSourceRecord(json: $0) == record } ?? record.json
            }
        }
    }

    mutating func upsertDataSource(_ record: GraphicsDataSourceRecord) {
        var sources = dataSources
        if let index = sources.firstIndex(where: { $0.id == record.id }) { sources[index] = record } else { sources.append(record) }
        dataSources = sources
    }

    /// Saved designs (GraphicsDesign), verbatim objects.
    var designs: [JSONObject] {
        get { json["designs"] as? [JSONObject] ?? [] }
        set { json["designs"] = newValue }
    }

    mutating func appendDesign(_ design: JSONObject) { designs = designs + [design] }

    /// GraphicsWorkspaceLibrary.ReferencedTemplates: every template version a playlist row, a
    /// row's image, a saved cue's draft (and its image) or the studio lower-third cue names, in
    /// document order without repeats. Cues and the lower third are read verbatim (D3).
    var referencedTemplates: [GraphicsTemplateReference] {
        var out: [GraphicsTemplateReference] = []
        func add(_ object: JSONObject?) {
            guard let object, let id = object["templateId"] as? String else { return }
            let reference = GraphicsTemplateReference(templateId: id, templateVersion: (object["templateVersion"] as? NSNumber)?.intValue ?? 0)
            if !out.contains(reference) { out.append(reference) }
        }
        for row in playlist {
            if !row.templateId.isEmpty { add(["templateId": row.templateId, "templateVersion": row.templateVersion]) }
            add(row.image)
        }
        let cueDrafts = (json["cues"] as? [JSONObject] ?? []).map { $0["draft"] as? JSONObject }
        let lowerThird = ((json["studioLowerThird"] as? JSONObject)?["cue"] as? JSONObject)?["draft"] as? JSONObject
        for draft in cueDrafts + [lowerThird] {
            add(draft)
            add(draft?["image"] as? JSONObject)
        }
        return out
    }

    /// The show's `designs` section as installable manifests (GraphicsDesign.Definition, no
    /// package root): the fallback when a referenced design version has no template pin.
    var designPins: [GraphicsTemplatePin] {
        (json["designs"] as? [JSONObject] ?? []).compactMap {
            guard let id = $0["templateId"] as? String, let definition = $0["definition"] as? JSONObject else { return nil }
            return GraphicsTemplatePin(templateId: id, templateVersion: ($0["templateVersion"] as? NSNumber)?.intValue ?? 0,
                                       manifestHash: "", definition: definition, packageRoot: "")
        }
    }

    /// GraphicsDocumentStore.Validate, in the C#'s check order (the first failing
    /// check names the error), within plan D3.
    static func validate(_ d: JSONObject) throws {
        typealias V = GraphicsValue
        func fail(_ message: String) -> GraphicsDocumentError { GraphicsDocumentError(message: message) }
        func list(_ key: String) throws -> [Any] {
            guard let value = V.array(d, key, []) else { throw fail("Invalid or oversized graphics workspace.") }
            return value
        }
        func ids(_ items: [Any], _ key: String) -> [String] { items.map { (($0 as? JSONObject)?[key] as? String) ?? "" } }
        // Top level (the C#'s first big condition).
        let workspace = "Invalid or oversized graphics workspace."
        guard let schema = V.int(d, "schemaVersion", 0), schema == currentSchemaVersion,
              let showId = V.string(d, "showId", "default"), !showId.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              showId.utf16.count <= 64,
              let authority = V.string(d, "authority", "legacy"), ["legacy", "volt"].contains(authority),
              let cues = V.array(d, "cues", []), cues.count <= 64,
              let templates = V.array(d, "templates", []), templates.count <= 16,
              let presets = V.object(d, "stylePresets", [:]), presets.count <= 64
        else { throw fail(workspace) }
        // Cues and style presets are not re-validated (D3): Windows wrote them.
        guard let templateStyles = V.object(d, "templateStyles", [:]), templateStyles.count <= 64
        else { throw fail("Too many saved template styles.") }
        let playlist = try list("playlist")
        guard playlist.count <= 64, playlist.allSatisfy({ ($0 as? JSONObject).map(GraphicsRow.validRow) == true }),
              Set(ids(playlist, "id")).count == playlist.count
        else { throw fail("Invalid saved graphics playlist.") }
        let layouts = try list("shotLayouts")
        guard layouts.count <= 32, layouts.allSatisfy({ ($0 as? JSONObject).map { GraphicsShotLayout(json: $0).isValid } == true }),
              Set(ids(layouts, "id")).count == layouts.count
        else { throw fail("Invalid saved shot layouts.") }
        let rules = try list("automationRules")
        guard rules.count <= 8, rules.allSatisfy({ $0 is JSONObject }), Set(ids(rules, "id")).count == rules.count,
              Set(ids(rules, "instanceId")).count == rules.count
        else { throw fail("Invalid saved automation rule configuration.") }
        let feeds = try list("feeds")
        guard feeds.count <= 32, feeds.allSatisfy({ $0 is JSONObject }), Set(ids(feeds, "id")).count == feeds.count
        else { throw fail("Invalid saved graphics feed definitions.") }
        let sources = try list("dataSources")
        let pushFeeds = Set(feeds.compactMap { item -> String? in
            guard let feed = item as? JSONObject, feed["format"] as? String == "wss" else { return nil }
            return feed["id"] as? String
        })
        guard sources.count <= 32,
              sources.allSatisfy({ item in
                  guard let object = item as? JSONObject, let record = GraphicsDataSourceRecord(json: object), record.isValid else { return false }
                  return (record.feedSequence != nil) == pushFeeds.contains(record.id)
              }),
              Set(ids(sources, "id")).count == sources.count
        else { throw fail("Invalid graphics datasets.") }
        let columns = try list("sourceColumns")
        guard columns.count <= 30, columns.allSatisfy({ $0 is JSONObject }), Set(ids(columns, "id")).count == columns.count
        else { throw fail("Invalid graphics Sources columns.") }
        let designs = try list("designs")
        guard designs.count <= 32, designs.allSatisfy({ ($0 as? JSONObject).map(GraphicsDesignIds.validDesign) == true }),
              Set(ids(designs, "designId")).count == designs.count,
              Set(ids(designs, "templateId")).count == designs.count
        else { throw fail("Invalid saved graphics designs.") }
        // Template pins: the Mac reads these, so they are validated in full.
        for item in templates {
            guard let t = item as? JSONObject,
                  let id = V.string(t, "templateId", ""), (1...64).contains(id.utf16.count),
                  let version = V.int(t, "templateVersion", 0), version >= 1,
                  let hash = V.string(t, "manifestHash", ""), hash.utf16.count == 64, hash.allSatisfy(\.isHexDigit),
                  let root = V.string(t, "packageRoot", ""), root.utf16.count <= 2048, !root.contains("\u{0}"),
                  let definition = t["definition"] as? JSONObject,
                  let bytes = try? JSONSerialization.data(withJSONObject: definition), bytes.count <= 256 * 1024
            else { throw fail("Invalid saved template document.") }
        }
    }
}

// ── the store ────────────────────────────────────────────────────────────────

struct GraphicsDocumentLoad {
    var document: GraphicsDocument
    var readOnly: Bool
    var recoveredBackup: Bool
    var status: String
}

/// Atomic, version-guarded graphics persistence. Not an actor: callers run it
/// off the main actor (spec 5.10); it holds no lock because the Mac has one
/// owner (GraphicsModel) that confines every load and save to its serial
/// `documentQueue` — hence @unchecked Sendable.
final class GraphicsDocumentStore: @unchecked Sendable {
    static let byteLimit = 2 * 1024 * 1024
    static let maxDepth = 32

    let path: String
    private(set) var loaded = false
    private(set) var readOnly = false
    private var recovered = false
    private var olderOnDisk = false
    private var fingerprint = ""

    init(path: String) { self.path = (path as NSString).standardizingPath }

    static var defaultPath: String {
        if let override = ProcessInfo.processInfo.environment["COREVIDEO_GRAPHICS_DOCUMENT_PATH"], !override.isEmpty { return override }
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        return support.appendingPathComponent("CoreVideoPro/graphics/default.json").path
    }

    func load() -> GraphicsDocumentLoad {
        loaded = true; readOnly = false; recovered = false; olderOnDisk = false
        let fm = FileManager.default
        do {
            if !fm.fileExists(atPath: path) && !fm.fileExists(atPath: path + ".bak") {
                fingerprint = ""
                return GraphicsDocumentLoad(document: GraphicsDocument(), readOnly: false, recoveredBackup: false,
                                            status: "New graphics workspace; automation is unarmed.")
            }
            let primary = try Self.read(path)
            fingerprint = Self.fingerprint(primary)
            if let primary, let result = try? decode(primary, backup: false) { return result }
            if let backup = try Self.read(path + ".bak") {
                let result = try decode(backup, backup: true)
                recovered = true
                return result
            }
            readOnly = true
            return GraphicsDocumentLoad(document: GraphicsDocument(), readOnly: true, recoveredBackup: false,
                                        status: "Graphics document is invalid. Original retained; repair or restore a backup before saving.")
        } catch {
            readOnly = true
            return GraphicsDocumentLoad(document: GraphicsDocument(), readOnly: true, recoveredBackup: false,
                                        status: "Graphics document unavailable: " + Self.message(error))
        }
    }

    private func decode(_ bytes: Data, backup: Bool) throws -> GraphicsDocumentLoad {
        guard let root = try? JSONSerialization.jsonObject(with: bytes) as? JSONObject, GraphicsJSON.depth(root) <= Self.maxDepth
        else { throw GraphicsDocumentError(message: "Graphics document is not valid JSON.") }
        guard GraphicsJSON.uniqueKeys(bytes) else { throw GraphicsDocumentError(message: "Duplicate graphics key.") }
        guard let schema = root["schemaVersion"], GraphicsJSON.isNumber(schema), let number = (schema as? NSNumber)?.doubleValue,
              number.rounded(.towardZero) == number, abs(number) <= Double(Int32.max)
        else { throw GraphicsDocumentError(message: "Missing graphics schema.") }
        let version = Int(number)
        if version > GraphicsDocument.currentSchemaVersion {
            readOnly = true
            return GraphicsDocumentLoad(document: GraphicsDocument(), readOnly: true, recoveredBackup: backup,
                                        status: "Graphics schema \(version) is newer than this build. Read-only; the original document is preserved.")
        }
        guard [1, 2, GraphicsDocument.currentSchemaVersion].contains(version) else { throw GraphicsDocumentError(message: "Unsupported graphics schema.") }
        var json = root
        if version < GraphicsDocument.currentSchemaVersion {
            json["schemaVersion"] = GraphicsDocument.currentSchemaVersion
            olderOnDisk = !backup
        }
        try GraphicsDocument.validate(json)
        return GraphicsDocumentLoad(document: GraphicsDocument(json: json), readOnly: false, recoveredBackup: backup,
                                    status: backup ? "Recovered the last complete graphics backup. Graphics remain hidden and automation unarmed."
                                                   : "Graphics workspace loaded. Restore cues explicitly; automation is unarmed.")
    }

    func save(_ document: GraphicsDocument) throws {
        if !loaded { _ = load() }
        if readOnly { throw GraphicsDocumentError(message: "Graphics document is read-only; original retained.") }
        try GraphicsDocument.validate(document.json)
        guard JSONSerialization.isValidJSONObject(document.json),
              let bytes = try? JSONSerialization.data(withJSONObject: document.json, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
        else { throw GraphicsDocumentError(message: "Graphics document could not be encoded.") }
        if bytes.count > Self.byteLimit { throw GraphicsDocumentError(message: "Graphics document exceeds the 2 MiB limit.") }
        if Self.fingerprint(try Self.read(path)) != fingerprint {
            throw GraphicsDocumentError(message: "Graphics file changed outside this workspace. Reload before saving.")
        }
        let fm = FileManager.default
        let directory = (path as NSString).deletingLastPathComponent
        try fm.createDirectory(atPath: directory, withIntermediateDirectories: true)
        // First schema-3 write over a v1/v2 file keeps that file once, never replacing an earlier copy.
        if olderOnDisk && fm.fileExists(atPath: path) && !fm.fileExists(atPath: path + ".v2.bak") {
            try fm.copyItem(atPath: path, toPath: path + ".v2.bak")
        }
        let temporary = (directory as NSString).appendingPathComponent(".graphics-" + UUID().uuidString.lowercased() + ".tmp")
        defer { try? fm.removeItem(atPath: temporary) }
        guard fm.createFile(atPath: temporary, contents: nil) else { throw GraphicsDocumentError(message: "Could not create a temporary graphics file.") }
        let handle = try FileHandle(forWritingTo: URL(fileURLWithPath: temporary))
        try handle.write(contentsOf: bytes)
        try handle.synchronize()
        try handle.close()
        if fm.fileExists(atPath: path) {
            if recovered {
                // The primary was unreadable: keep it as .corrupt, never as the backup.
                try? fm.removeItem(atPath: path + ".corrupt")
                try fm.copyItem(atPath: path, toPath: path + ".corrupt")
            } else {
                // The current primary becomes the last complete backup (File.Replace's backup).
                let staged = path + ".bak.tmp"
                try? fm.removeItem(atPath: staged)
                try fm.copyItem(atPath: path, toPath: staged)
                try Self.rename(staged, path + ".bak")
            }
        }
        try Self.rename(temporary, path)   // atomic replace
        fingerprint = Self.fingerprint(bytes)
        recovered = false
        olderOnDisk = false
    }

    // ── helpers ──────────────────────────────────────────────────────────────

    static func read(_ path: String) throws -> Data? {
        guard FileManager.default.fileExists(atPath: path) else { return nil }
        let attributes = try FileManager.default.attributesOfItem(atPath: path)
        if (attributes[.size] as? NSNumber)?.intValue ?? 0 > byteLimit {
            throw GraphicsDocumentError(message: "Graphics document exceeds the 2 MiB limit.")
        }
        return try Data(contentsOf: URL(fileURLWithPath: path))
    }

    static func fingerprint(_ bytes: Data?) -> String {
        guard let bytes else { return "" }
        return SHA256.hash(data: bytes).map { String(format: "%02X", $0) }.joined()
    }

    static func rename(_ from: String, _ to: String) throws {
        guard Darwin.rename(from, to) == 0 else {
            throw GraphicsDocumentError(message: "Could not replace \((to as NSString).lastPathComponent): \(String(cString: strerror(errno)))")
        }
    }

    static func message(_ error: Error) -> String {
        (error as? GraphicsDocumentError)?.message ?? error.localizedDescription
    }
}

extension GraphicsJSON {
    /// True when no object anywhere in `data` repeats a key (GraphicsDocumentStore.UniqueKeys).
    /// Call only on bytes JSONSerialization already accepted.
    static func uniqueKeys(_ data: Data) -> Bool {
        let bytes = [UInt8](data)
        var i = 0
        var stack: [Set<String>?] = []   // a key set per open object; nil for an array
        var expectingKey = false
        while i < bytes.count {
            let c = bytes[i]
            switch c {
            case 0x22:  // string
                let start = i
                i += 1
                while i < bytes.count, bytes[i] != 0x22 { i += bytes[i] == 0x5C ? 2 : 1 }
                i += 1
                if expectingKey, let top = stack.last, top != nil {
                    let raw = Data(bytes[start..<min(i, bytes.count)])
                    guard let key = (try? JSONSerialization.jsonObject(with: raw, options: [.fragmentsAllowed])) as? String else { return false }
                    if stack[stack.count - 1]!.insert(key).inserted == false { return false }
                    expectingKey = false
                }
                continue
            case 0x7B: stack.append([]); expectingKey = true
            case 0x5B: stack.append(nil); expectingKey = false
            case 0x7D, 0x5D: _ = stack.popLast(); expectingKey = false
            case 0x2C: expectingKey = stack.last.map { $0 != nil } ?? false
            case 0x3A: expectingKey = false
            default: break
            }
            i += 1
        }
        return true
    }
}

// ── datasets (GraphicsDataSource, MC/Services/GraphicsWorkspaceLibrary.Data.cs:5-12) ──

struct GraphicsDataSourceRecord: Equatable {
    static let maxFeedSequence: Int64 = 9_007_199_254_740_991
    var id: String
    var name: String
    var revision: Int64
    var receivedAt: Date
    var data: GraphicsLocalData
    var feedSequence: Int64?

    init(id: String, name: String, revision: Int64, receivedAt: Date, data: GraphicsLocalData, feedSequence: Int64? = nil) {
        self.id = id; self.name = name; self.revision = revision; self.receivedAt = receivedAt; self.data = data; self.feedSequence = feedSequence
    }

    var isValid: Bool {
        (feedSequence == nil || (1...Self.maxFeedSequence).contains(feedSequence!)) && GraphicsRow.validId(id) &&
            (1...128).contains(name.utf16.count) && revision > 0 && receivedAt != Self.defaultDate && data.isValid
    }

    /// DateTimeOffset default (0001-01-01T00:00:00+00:00), which Valid() refuses.
    static let defaultDate = Date(timeIntervalSince1970: -62_135_596_800)

    var json: JSONObject {
        var out: JSONObject = ["id": id, "name": name, "revision": revision, "receivedAt": Self.iso8601(receivedAt), "data": data.json]
        if let feedSequence { out["feedSequence"] = feedSequence }   // JsonIgnore(WhenWritingNull)
        return out
    }

    init?(json: JSONObject) {
        // System.Text.Json Web defaults, as Windows reads it: names in any case, numbers also as strings.
        let v = { (key: String) in GraphicsDataBinding.value(json, key) }
        guard let id = v("id") as? String, let name = v("name") as? String,
              let revision = GraphicsDataBinding.int64(v("revision")), let at = (v("receivedAt") as? String).flatMap(Self.parseISO8601),
              let dataJSON = v("data") as? JSONObject, let data = GraphicsLocalData(json: dataJSON) else { return nil }
        let sequence = v("feedSequence")
        if sequence != nil && GraphicsDataBinding.int64(sequence) == nil { return nil }
        self.init(id: id, name: name, revision: revision, receivedAt: at, data: data, feedSequence: GraphicsDataBinding.int64(sequence))
    }

    /// ISO 8601 with milliseconds and a zone, which System.Text.Json reads as a DateTimeOffset.
    static func iso8601(_ date: Date) -> String {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        formatter.timeZone = TimeZone(identifier: "UTC")
        return formatter.string(from: date)
    }

    /// System.Text.Json's DateTimeOffset text (up to 7 fractional digits, "Z" or "+hh:mm").
    static func parseISO8601(_ text: String) -> Date? {
        var trimmed = text
        if let dot = text.firstIndex(of: "."), let zone = text[dot...].firstIndex(where: { $0 == "Z" || $0 == "+" || $0 == "-" }) {
            let fraction = text[text.index(after: dot)..<zone]
            trimmed = String(text[..<dot]) + "." + String(fraction.prefix(3)).padding(toLength: 3, withPad: "0", startingAt: 0) + String(text[zone...])
        }
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = trimmed.contains(".") ? [.withInternetDateTime, .withFractionalSeconds] : [.withInternetDateTime]
        return formatter.date(from: trimmed)
    }
}
