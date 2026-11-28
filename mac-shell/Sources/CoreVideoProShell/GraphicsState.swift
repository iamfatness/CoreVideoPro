// GraphicsState — typed read of the core's `graphics` snapshot node, and the On
// Air strip projected from it (graphics G1, spec 5.3/5.4).
//
// Windows reference: MC/Models/NativeGraphicsState.cs (the model, missing fields
// default exactly as there) and MC/Services/GraphicsPlayoutService.cs:261-295
// (strip projection). ON-AIR TRUTH IS ONLY THE CORE'S INSTANCES: a locally
// accepted command never puts a chip on the strip (GraphicsPlayoutService.cs:19-21).
// Conformance: contracts/graphics-shell/state.vectors.json (exported from the C#).

import Foundation

struct GraphicsOperationResult: Equatable {
    var clientId = ""
    var instanceId = ""
    var currentRevision: Int64 = 0
    var requestId = ""
    var accepted = false
    var reason = ""

    init(clientId: String = "", instanceId: String = "", currentRevision: Int64 = 0, requestId: String = "",
         accepted: Bool = false, reason: String = "") {
        self.clientId = clientId; self.instanceId = instanceId; self.currentRevision = currentRevision
        self.requestId = requestId; self.accepted = accepted; self.reason = reason
    }

    init(json: JSONObject) {
        clientId = GraphicsJSON.string(json["clientId"])
        instanceId = GraphicsJSON.string(json["instanceId"])
        currentRevision = GraphicsJSON.int(json["currentRevision"])
        requestId = GraphicsJSON.string(json["requestId"])
        accepted = GraphicsJSON.bool(json["accepted"])
        reason = GraphicsJSON.string(json["reason"])
    }

    static func refused(_ reason: String, requestId: String = "") -> GraphicsOperationResult {
        GraphicsOperationResult(requestId: requestId, accepted: false, reason: reason)
    }
}

struct GraphicsTemplateField: Equatable {
    var id = ""
    var type = ""
    var defaultValue: Any?
    /// NativeGraphicsTemplateField constraints (GraphicsMappedValues.ConvertCell reads them).
    var required = false
    var maxLength: Int?
    var minimum: Double?
    var maximum: Double?
    var choices: [Any] = []
    /// metadata.label when it is a string (the editor's friendly name; GraphicsRowFields.LabelOf).
    var label: String?
    static func == (a: GraphicsTemplateField, b: GraphicsTemplateField) -> Bool { a.id == b.id && a.type == b.type }
}

struct GraphicsTemplate {
    var templateId = ""
    var templateVersion = 0
    var name = ""
    var family = ""
    var runtime = ""
    var manifestHash = ""
    var packageRoot = ""
    var definition: JSONObject?
    var fields: [GraphicsTemplateField] = []
    var defaultPlacement: JSONObject?
    var animations: JSONObject?
}

struct GraphicsSourceIdentity: Equatable {
    var sourceId = ""
    var instanceId = ""
    var processEpoch = ""
    var generation: Int64 = 0
    var json: JSONObject {
        ["sourceId": sourceId, "instanceId": instanceId, "processEpoch": processEpoch, "generation": generation]
    }
}

struct GraphicsSource: Equatable {
    var identity: GraphicsSourceIdentity?
    var displayName = ""
    var videoOn = false
}

/// NativeGraphicsClock: what the core says a clock instance shows right now.
struct GraphicsInstanceClock: Equatable {
    var text = ""
    var state = ""
    var finishAction = ""
    var remainingSeconds = 0.0
}

struct GraphicsInstance {
    var instanceId = ""
    var label = ""
    var templateId = ""
    var templateVersion = 0
    var scope = ""
    var sceneId = ""
    var sourceId = ""
    var binding: GraphicsSourceIdentity?
    var revision: Int64 = 0
    var programRevision: Int64 = 0
    var cueRevision: Int64 = 0
    var cueOwner = ""
    var cuePending = false
    var cuePreparation = "unprepared"
    var cueWarning = ""
    var preparation = "unprepared"
    var visibility = "hidden"
    var cued = false
    var showPending = false
    var firstDrawSubmittedFrame: Int64 = 0
    var currentlyComposed = false
    var warning = ""
    var crawlPlayback = ""
    var crawlPendingRevision: Int64 = 0
    /// `fields` as published (name/title plus any declarative fields).
    var fields: JSONObject = [:]
    var fieldName: String { GraphicsJSON.string(fields["name"]) }
    var animations: JSONObject?
    var clockConfigurationRevision: Int64 = 0
    /// The template versions the cue and Program copies use (eviction keeps them), and the images.
    var cueTemplateVersion = 0
    var programTemplateVersion = 0
    var image: GraphicsTemplateReference?
    var cueImage: GraphicsTemplateReference?
    var programImage: GraphicsTemplateReference?
    var programFields: JSONObject?
    /// The instance's live clock (NativeGraphicsClock), nil when its template has none.
    var clock: GraphicsInstanceClock?
    var clockRequestedConfiguration: JSONObject?
    /// Resolved show palettes of the draft, cue and Program revisions (graphics styles slice 1).
    var palette: [String: String]?
    var cuePalette: [String: String]?
    var programPalette: [String: String]?

    /// On Program: an accepted Program revision that is not hidden or building
    /// out (GraphicsPlayoutService.IsOnProgram, :96).
    var isOnProgram: Bool { programRevision > 0 && visibility != "hidden" && visibility != "building-out" }
    /// On Preview: a cue revision (GraphicsPlayoutService.IsOnPreview, :97).
    var isOnPreview: Bool { cueRevision > 0 }
}

struct GraphicsState {
    var schemaVersion = 0
    var coreEpoch = ""
    var instances: [GraphicsInstance] = []
    var operations: [GraphicsOperationResult] = []
    var lastResult: GraphicsOperationResult?
    var templates: [GraphicsTemplate] = []
    var templateInstalls: [(requestId: String, state: String, reason: String)] = []
    var sources: [GraphicsSource] = []
    var familyAuthorities: [String: String] = [:]

    /// Usable for commands: schema 1 with a non-empty epoch (gateway rule,
    /// GraphicsCommandGateway.cs:40). Capability and a running core are the
    /// gateway's other two conditions.
    var isUsable: Bool { schemaVersion == 1 && !coreEpoch.isEmpty }

    func instance(_ id: String) -> GraphicsInstance? { instances.first { $0.instanceId == id } }
    func template(_ id: String, _ version: Int) -> GraphicsTemplate? {
        templates.first { $0.templateId == id && $0.templateVersion == version }
    }

    init() {}

    init(node: JSONObject) {
        schemaVersion = Int(GraphicsJSON.int(node["schemaVersion"]))
        coreEpoch = GraphicsJSON.string(node["coreEpoch"])
        instances = (node["instances"] as? [JSONObject] ?? []).map(Self.instance)
        operations = (node["operations"] as? [JSONObject] ?? []).map(GraphicsOperationResult.init(json:))
        lastResult = (node["lastResult"] as? JSONObject).map(GraphicsOperationResult.init(json:))
        templates = (node["templates"] as? [JSONObject] ?? []).map(Self.template)
        templateInstalls = (node["templateInstalls"] as? [JSONObject] ?? []).map {
            (GraphicsJSON.string($0["requestId"]), GraphicsJSON.string($0["state"]), GraphicsJSON.string($0["reason"]))
        }
        sources = (node["sources"] as? [JSONObject] ?? []).map {
            GraphicsSource(identity: ($0["identity"] as? JSONObject).map(Self.identity),
                           displayName: GraphicsJSON.string($0["displayName"]), videoOn: GraphicsJSON.bool($0["videoOn"]))
        }
        for (family, value) in node["familyAuthorities"] as? JSONObject ?? [:] {
            if let authority = value as? String { familyAuthorities[family] = authority }
        }
    }

    static func identity(_ json: JSONObject) -> GraphicsSourceIdentity {
        GraphicsSourceIdentity(sourceId: GraphicsJSON.string(json["sourceId"]), instanceId: GraphicsJSON.string(json["instanceId"]),
                               processEpoch: GraphicsJSON.string(json["processEpoch"]), generation: GraphicsJSON.int(json["generation"]))
    }

    private static func instance(_ json: JSONObject) -> GraphicsInstance {
        var i = GraphicsInstance()
        i.instanceId = GraphicsJSON.string(json["instanceId"])
        i.label = GraphicsJSON.string(json["label"])
        i.templateId = GraphicsJSON.string(json["templateId"])
        i.templateVersion = Int(GraphicsJSON.int(json["templateVersion"]))
        i.scope = GraphicsJSON.string(json["scope"])
        i.sceneId = GraphicsJSON.string(json["sceneId"])
        i.sourceId = GraphicsJSON.string(json["sourceId"])
        i.binding = (json["binding"] as? JSONObject).map(identity)
        i.revision = GraphicsJSON.int(json["revision"])
        i.programRevision = GraphicsJSON.int(json["programRevision"])
        i.cueRevision = GraphicsJSON.int(json["cueRevision"])
        i.palette = GraphicsPalette.fromWire(json["palette"])
        i.cuePalette = GraphicsPalette.fromWire(json["cuePalette"])
        i.programPalette = GraphicsPalette.fromWire(json["programPalette"])
        i.cueOwner = GraphicsJSON.string(json["cueOwner"])
        i.cuePending = GraphicsJSON.bool(json["cuePending"])
        i.cuePreparation = GraphicsJSON.string(json["cuePreparation"], default: "unprepared")
        i.cueWarning = GraphicsJSON.string(json["cueWarning"])
        i.preparation = GraphicsJSON.string(json["preparation"], default: "unprepared")
        i.visibility = GraphicsJSON.string(json["visibility"], default: "hidden")
        i.cued = GraphicsJSON.bool(json["cued"])
        i.showPending = GraphicsJSON.bool(json["showPending"])
        i.firstDrawSubmittedFrame = GraphicsJSON.int(json["firstDrawSubmittedFrame"])
        i.currentlyComposed = GraphicsJSON.bool(json["currentlyComposed"])
        i.warning = GraphicsJSON.string(json["warning"])
        i.crawlPlayback = GraphicsJSON.string(json["crawlPlayback"])
        i.crawlPendingRevision = GraphicsJSON.int(json["crawlPendingRevision"])
        i.fields = json["fields"] as? JSONObject ?? [:]
        i.animations = json["animations"] as? JSONObject
        i.clockConfigurationRevision = GraphicsJSON.int(json["clockConfigurationRevision"])
        i.clockRequestedConfiguration = json["clockRequestedConfiguration"] as? JSONObject
        i.cueTemplateVersion = Int(GraphicsJSON.int(json["cueTemplateVersion"]))
        i.programTemplateVersion = Int(GraphicsJSON.int(json["programTemplateVersion"]))
        func reference(_ key: String) -> GraphicsTemplateReference? {
            guard let o = json[key] as? JSONObject, let id = o["templateId"] as? String else { return nil }
            return GraphicsTemplateReference(templateId: id, templateVersion: Int(GraphicsJSON.int(o["templateVersion"])))
        }
        i.image = reference("image"); i.cueImage = reference("cueImage"); i.programImage = reference("programImage")
        i.programFields = json["programFields"] as? JSONObject
        i.clock = (json["clock"] as? JSONObject).map {
            GraphicsInstanceClock(text: GraphicsJSON.string($0["text"]), state: GraphicsJSON.string($0["state"]),
                                  finishAction: GraphicsJSON.string($0["finishAction"]),
                                  remainingSeconds: ($0["remainingSeconds"] as? NSNumber)?.doubleValue ?? 0)
        }
        return i
    }

    /// A manifest's `fields` array (the core's template list and a saved pin's definition alike).
    static func fields(_ json: Any?) -> [GraphicsTemplateField] {
        (json as? [JSONObject] ?? []).map {
            GraphicsTemplateField(id: GraphicsJSON.string($0["id"]), type: GraphicsJSON.string($0["type"]), defaultValue: $0["default"],
                                  required: GraphicsJSON.bool($0["required"]), maxLength: ($0["maxLength"] as? NSNumber)?.intValue,
                                  minimum: ($0["minimum"] as? NSNumber)?.doubleValue, maximum: ($0["maximum"] as? NSNumber)?.doubleValue,
                                  choices: $0["choices"] as? [Any] ?? [], label: ($0["metadata"] as? JSONObject)?["label"] as? String)
        }
    }

    private static func template(_ json: JSONObject) -> GraphicsTemplate {
        var t = GraphicsTemplate()
        t.templateId = GraphicsJSON.string(json["templateId"])
        t.templateVersion = Int(GraphicsJSON.int(json["templateVersion"]))
        t.name = GraphicsJSON.string(json["name"])
        t.family = GraphicsJSON.string(json["family"])
        t.runtime = GraphicsJSON.string(json["runtime"])
        t.manifestHash = GraphicsJSON.string(json["manifestHash"])
        t.packageRoot = GraphicsJSON.string(json["packageRoot"])
        t.definition = json["definition"] as? JSONObject
        t.fields = fields(json["fields"])
        t.defaultPlacement = json["defaultPlacement"] as? JSONObject
        t.animations = json["animations"] as? JSONObject
        return t
    }
}

// ── On Air strip ─────────────────────────────────────────────────────────────

/// A playlist row as the strip needs it (id prefix, kind, name, names-list ids).
struct GraphicsStripRow: Equatable {
    var id: String
    var kind: String
    var name: String
    var variantIds: [String] = []
    var hasVariants: Bool { !variantIds.isEmpty }
}

struct GraphicsStripItem: Equatable, Identifiable {
    var instanceId: String
    var label: String
    var owner: String       // "playlist" | "design" (automation "auto" chips: not on Mac yet)
    var rowId: String
    var sourceId: String
    var revision: Int64
    var visibility: String
    var cuePending: Bool
    var id: String { instanceId }
}

struct GraphicsStrip: Equatable {
    static let empty = GraphicsStrip()
    var coreEpoch = ""
    var onProgram: [GraphicsStripItem] = []
    var onPreview: [GraphicsStripItem] = []

    static let playlistPrefix = "playlist-"
    static let socialPrefix = "social-review-"
    static let socialTemplateId = "youtube-chat-card"
    // Both provider cards are social: the cued list filters them by the
    // social-review- prefix (Windows filters only the YouTube template, owner
    // decision N4; the Mac filters both).
    static let twitchSocialTemplateId = "twitch-chat-card"

    /// The row that created an instance id (GraphicsPlayoutService.RowOf, :74-75).
    static func row(of instanceId: String, in rows: [GraphicsStripRow]) -> GraphicsStripRow? {
        rows.first { instanceId == playlistPrefix + $0.id || instanceId.hasPrefix(playlistPrefix + $0.id + "-") }
    }

    /// GraphicsPlayoutService.Observe strip projection (:268-287), without the
    /// automation chips (no native automation on Mac yet).
    static func project(_ state: GraphicsState, rows: [GraphicsStripRow]) -> GraphicsStrip {
        func item(_ i: GraphicsInstance) -> GraphicsStripItem {
            let row = Self.row(of: i.instanceId, in: rows)
            var source = i.binding?.sourceId ?? i.sourceId
            if source.isEmpty, let row, row.hasVariants {
                source = row.variantIds.map { "variant:" + $0 }
                    .first { GraphicsPlayoutIds.instanceId(rowId: row.id, sourceId: $0) == i.instanceId } ?? ""
            }
            let label: String
            if i.templateId == socialTemplateId { label = "YouTube chat" }
            else if i.templateId == twitchSocialTemplateId { label = "Twitch chat" }
            else if let row {
                label = (row.kind == "guest" || row.hasVariants) && !i.fieldName.isEmpty ? row.name + " · " + i.fieldName : row.name
            } else {
                label = !i.label.isEmpty ? i.label : !i.fieldName.isEmpty ? i.fieldName : i.templateId
            }
            return GraphicsStripItem(instanceId: i.instanceId, label: label, owner: row != nil ? "playlist" : "design",
                                     rowId: row?.id ?? "", sourceId: source, revision: i.revision,
                                     visibility: i.visibility, cuePending: i.cuePending)
        }
        return GraphicsStrip(
            coreEpoch: state.coreEpoch,
            onProgram: state.instances.filter(\.isOnProgram).map(item),
            onPreview: state.instances.filter { $0.isOnPreview && !$0.instanceId.hasPrefix(socialPrefix) }.map(item))
    }
}

/// Instance identity shared by the strip and playout (GraphicsPlayoutService.InstanceIdFor, :93-94).
enum GraphicsPlayoutIds {
    static func instanceId(rowId: String, sourceId: String) -> String {
        guard !sourceId.isEmpty else { return GraphicsStrip.playlistPrefix + rowId }
        return GraphicsStrip.playlistPrefix + rowId + "-" + String(GraphicsJSON.sha256Hex(Data(sourceId.utf8)).prefix(12))
    }
}
