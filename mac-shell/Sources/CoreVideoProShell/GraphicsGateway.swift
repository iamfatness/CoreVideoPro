// GraphicsGateway — the ONE sender of graphics-* commands (graphics G1, spec 5.3).
//
// Port of MC/Services/GraphicsCommandGateway.cs. Availability (:39-41): a running
// core advertising `graphics-instances`, with a published graphics node at
// schemaVersion 1 and a non-empty coreEpoch; otherwise every intent is refused
// `graphics-native-state-unavailable` and NOTHING is sent. One command in flight
// (:61). The result is read back by requestId + clientId from
// graphics.operations[], then lastResult, and must carry the observed epoch
// (:65-67). An accepted result is admission only; on-air truth stays the core's
// instances (GraphicsState).
//
// The request pre-check is the THIN port of GraphicsCommandRequest.TryBuild
// (:104-273) described in plan default D1: native validation stays
// authoritative (:78-79). Conformance: contracts/graphics-shell/gateway.vectors.json.

import CryptoKit
import Foundation

enum GraphicsJSON {
    static func string(_ value: Any?, default fallback: String = "") -> String { value as? String ?? fallback }
    static func int(_ value: Any?) -> Int64 {
        guard let number = value as? NSNumber, !isBool(number) else { return 0 }
        return number.int64Value
    }
    static func bool(_ value: Any?) -> Bool {
        guard let number = value as? NSNumber, isBool(number) else { return false }
        return number.boolValue
    }
    static func isBool(_ value: Any?) -> Bool {
        guard let number = value as? NSNumber else { return false }
        return CFGetTypeID(number) == CFBooleanGetTypeID()
    }
    static func isNumber(_ value: Any?) -> Bool { value is NSNumber && !isBool(value) }
    static func parseObject(_ text: String) -> JSONObject? {
        guard let data = text.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed]) as? JSONObject
        else { return nil }
        return object
    }
    static func serialize(_ object: Any) -> String {
        guard JSONSerialization.isValidJSONObject(object),
              let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]) else { return "" }
        return String(data: data, encoding: .utf8) ?? ""
    }
    static func sha256Hex(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }
    /// The top-level member names of a JSON object, in document order and WITH
    /// duplicates. JSONSerialization keeps only the last duplicate and forgets the
    /// order, but GraphicsCommandRequest.TryBuild refuses a duplicate field and
    /// reports the FIRST faulty property in document order, so the pre-check has
    /// to see both. Returns nil when `text` is not an object; the value grammar is
    /// left to JSONSerialization (call this only on text it already parsed).
    static func topLevelKeys(_ text: String) -> [String]? {
        let bytes = Array(text.utf8)
        var i = 0
        func skipSpace() { while i < bytes.count, [0x20, 0x09, 0x0A, 0x0D].contains(bytes[i]) { i += 1 } }
        /// Scans a string starting at the opening quote; returns its raw text with quotes.
        func scanString() -> String? {
            guard i < bytes.count, bytes[i] == 0x22 else { return nil }
            let start = i
            i += 1
            while i < bytes.count {
                if bytes[i] == 0x5C { i += 2; continue }
                if bytes[i] == 0x22 { i += 1; return String(decoding: bytes[start..<i], as: UTF8.self) }
                i += 1
            }
            return nil
        }
        func skipValue() -> Bool {
            skipSpace()
            var depth = 0
            while i < bytes.count {
                switch bytes[i] {
                case 0x22: guard scanString() != nil else { return false }; if depth == 0 { return true }; continue
                case 0x7B, 0x5B: depth += 1
                case 0x7D, 0x5D:
                    if depth == 0 { return true }
                    depth -= 1
                    if depth == 0 { i += 1; return true }
                case 0x2C: if depth == 0 { return true }
                default: break
                }
                i += 1
            }
            return depth == 0
        }
        skipSpace()
        guard i < bytes.count, bytes[i] == 0x7B else { return nil }
        i += 1
        var keys: [String] = []
        skipSpace()
        if i < bytes.count, bytes[i] == 0x7D { return keys }
        while i < bytes.count {
            skipSpace()
            guard let raw = scanString(),
                  let key = (try? JSONSerialization.jsonObject(with: Data(raw.utf8), options: [.fragmentsAllowed])) as? String
            else { return nil }
            keys.append(key)
            skipSpace()
            guard i < bytes.count, bytes[i] == 0x3A else { return nil }
            i += 1
            guard skipValue() else { return nil }
            skipSpace()
            guard i < bytes.count else { return nil }
            if bytes[i] == 0x7D { return keys }
            guard bytes[i] == 0x2C else { return nil }
            i += 1
        }
        return nil
    }
    /// Nesting depth of a parsed JSON value (an object or array counts one level).
    static func depth(_ value: Any) -> Int {
        if let object = value as? JSONObject { return 1 + (object.values.map(depth).max() ?? 0) }
        if let array = value as? [Any] { return 1 + (array.map(depth).max() ?? 0) }
        return 0
    }
}

enum GraphicsCommandRequest {
    static let unavailable = "graphics-native-state-unavailable"

    /// Allowed fields per action (GraphicsCommandGateway.cs:82-93).
    static let parameters: [String: Set<String>] = [
        "graphics.automation.configure": ["configuration", "expectedAutomationRevision"],
        "graphics.automation.control": ["action", "mode", "expectedAutomationRevision", "expectedActivationRevision"],
        "graphics.authority.set": ["family", "authority"],
        "graphics.instance.create": ["templateId", "templateVersion", "scope", "sceneId", "sourceId", "fields", "style", "palette",
                                     "animations", "binding", "placement", "image"],
        "graphics.instance.update": ["label", "rackOrder", "templateVersion", "target", "sourceId", "fields", "style", "palette",
                                     "animations", "binding", "placement", "image"],
        "graphics.instance.cue": ["rideTake", "cueOwner"],
        "graphics.instance.trigger": ["action", "target", "cueOwner"],
        "graphics.clock.control": ["action", "configuration", "expectedClockRevision"],
        "graphics.instance.remove": [],
        "graphics.program.clear": ["channel"],
        "graphics.template.install": ["manifest", "packageRoot", "packagePath"],
        "graphics.template.list": ["includeThumbnails"],
        "graphics.template.remove": ["templateId", "templateVersion"],
    ]
    static let common: Set<String> = ["schemaVersion", "requestId", "clientId", "coreEpoch", "instanceId", "expectedRevision"]
    private static let integerFields: Set<String> = ["rackOrder", "schemaVersion", "expectedRevision", "expectedClockRevision",
                                                     "expectedAutomationRevision", "expectedActivationRevision", "templateVersion"]
    private static let objectFields: Set<String> = ["fields", "style", "palette", "animations", "binding", "placement", "configuration", "image"]

    struct Built { var wire: JSONObject; var requestId: String; var clientId: String }
    struct Refusal: Error, Equatable { var error: String; var requestId: String }

    /// Id grammar (GraphicsCommandGateway.cs:346-347): 1-64 chars, a-z first, then a-z 0-9 -.
    static func validId(_ value: String) -> Bool {
        guard (1...64).contains(value.count), let first = value.unicodeScalars.first, ("a"..."z").contains(first) else { return false }
        return value.unicodeScalars.allSatisfy { ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "-" }
    }
    private static func integer(_ value: Any?, minimum: Double) -> Bool {
        guard GraphicsJSON.isNumber(value), let number = (value as? NSNumber)?.doubleValue,
              number.isFinite, number.rounded(.towardZero) == number else { return false }
        return number >= minimum && number <= 9007199254740991
    }
    private static func text(_ body: JSONObject, _ key: String) -> String { body[key] as? String ?? "" }
    private static func curated(_ id: String) -> Bool { ["compact-solid", "minimal-accent", "broadcast"].contains(id) }

    /// Thin pre-check (plan D1). Returns the wire command or the refusal reason
    /// plus whatever requestId had been read when it refused (C# reports it).
    static func build(action: String, request: String, observed: GraphicsState) -> Result<Built, Refusal> {
        guard let specific = parameters[action] else { return .failure(Refusal(error: "unsupported-graphics-action", requestId: "")) }
        let limit = action == "graphics.template.install" ? 2 * 1024 * 1024 : action == "graphics.automation.configure" ? 65536 : 32768
        var requestId = ""
        func refuse(_ error: String = "invalid-graphics-request") -> Result<Built, Refusal> {
            .failure(Refusal(error: error, requestId: requestId))
        }
        if request.utf16.count > limit || request.utf8.count > limit { return refuse() }
        guard let body = GraphicsJSON.parseObject(request), let keys = GraphicsJSON.topLevelKeys(request) else { return refuse() }
        if GraphicsJSON.depth(body) > (action == "graphics.automation.configure" ? 8 : 4) { return refuse() }
        // Identity contract (Lifecycle.cs GraphicsOperationIdentityContract) + schema 1.
        guard let rid = body["requestId"] as? String, !rid.isEmpty, let clientId = body["clientId"] as? String, !clientId.isEmpty,
              let epoch = body["coreEpoch"] as? String, !epoch.isEmpty,
              GraphicsJSON.isNumber(body["schemaVersion"]), (body["schemaVersion"] as? NSNumber)?.doubleValue == 1
        else { return refuse() }
        requestId = rid
        guard validId(rid), validId(clientId) else { return refuse() }
        guard epoch == observed.coreEpoch else { return refuse("core-epoch-conflict") }
        var seen = Set<String>()
        for name in keys {
            guard seen.insert(name).inserted, common.contains(name) || specific.contains(name), let value = body[name]
            else { return refuse("unsupported-or-duplicate-request-field") }
            if name == "includeThumbnails" || name == "rideTake" {
                guard GraphicsJSON.isBool(value) else { return refuse() }
                continue
            }
            if objectFields.contains(name) { continue }
            if name == "manifest" {
                guard let manifest = value as? String, !manifest.isEmpty, manifest.utf8.count <= 256 * 1024 else { return refuse() }
                continue
            }
            if integerFields.contains(name) {
                guard integer(value, minimum: 0) else { return refuse() }
            } else {
                let cap = name == "packagePath" || name == "packageRoot" ? 2048 : 256
                guard let string = value as? String, string.utf8.count <= cap, !string.contains("\u{0}") else { return refuse() }
            }
        }
        // The resolved show palette (graphics styles slice 1, GraphicsCommandRequest.ValidPalette).
        if let palette = body["palette"], !(palette is NSNull) {
            guard let map = palette as? JSONObject,
                  map.allSatisfy({ GraphicsPalette.tokens.contains($0.key) && GraphicsPalette.isColor($0.value as? String) })
            else { return refuse("invalid-graphics-palette") }
        }
        if let owner = body["cueOwner"] {
            guard let owner = owner as? String, validId(owner),
                  action != "graphics.instance.trigger" || text(body, "action") == "uncue" else { return refuse() }
        }
        let library = action.hasPrefix("graphics.automation.") || action == "graphics.authority.set" || action.hasPrefix("graphics.template.")
        if action == "graphics.authority.set" {
            guard ["lower-third", "bug"].contains(text(body, "family")), ["legacy", "instances"].contains(text(body, "authority"))
            else { return refuse() }
        }
        if action == "graphics.template.install" {
            let hasManifest = body["manifest"] != nil, hasPath = body["packagePath"] != nil
            if hasManifest == hasPath { return refuse() }
            if hasPath && (text(body, "packagePath").trimmingCharacters(in: .whitespaces).isEmpty || body["packageRoot"] != nil) { return refuse() }
        }
        if !library && action != "graphics.program.clear" {
            guard let id = body["instanceId"] as? String, validId(id) else { return refuse() }
        }
        if !library && action != "graphics.instance.create" && action != "graphics.program.clear" {
            guard integer(body["expectedRevision"], minimum: 1) else { return refuse() }
        }
        if action == "graphics.instance.create" {
            let templateId = text(body, "templateId")
            let version = (body["templateVersion"] as? NSNumber)?.doubleValue ?? -1
            let advertised = observed.templates.contains {
                $0.templateId == templateId && Double($0.templateVersion) == version && $0.runtime == "declarative"
            }
            guard (curated(templateId) && version == 1) || advertised else { return refuse() }
            let scope = text(body, "scope"), scene = text(body, "sceneId")
            guard scope == "global" || scope == "scene", !(scope == "scene" && scene.isEmpty), !(scope == "global" && !scene.isEmpty)
            else { return refuse() }
        }
        if action == "graphics.instance.update", !["cue", "program"].contains(text(body, "target")) { return refuse() }
        if action == "graphics.instance.trigger" {
            guard ["show", "hide", "clear", "uncue", "pause", "resume", "restart"].contains(text(body, "action")) else { return refuse() }
            if body["target"] != nil, text(body, "target") != "cue" || text(body, "action") != "show" { return refuse() }
        }
        if action == "graphics.program.clear", text(body, "channel") != "all" { return refuse() }
        let wire: JSONObject = ["type": action.replacingOccurrences(of: ".", with: "-"), "graphics": body]
        return .success(Built(wire: wire, requestId: rid, clientId: clientId))
    }
}

/// The one boundary the playout, the strip and (later) the control surface use.
@MainActor
final class GraphicsGateway {
    static let socialClientId = "graphics-social-review"
    static let capability = "graphics-instances"
    static let transportFailedPrefix = "graphics-transport-failed"

    /// True when a core is running and advertises `graphics-instances`.
    var capable: () -> Bool
    /// The latest decoded graphics node (nil before the first snapshot).
    var observe: () -> GraphicsState?
    /// Sends one media-core-sync body; returns the core's reply (with `snapshot`).
    var send: (JSONObject) async throws -> JSONObject
    /// Every reply's snapshot goes back to the model so state stays current.
    var onSnapshot: (JSONObject) -> Void = { _ in }
    var elapsedMs: () -> Int = { 0 }
    private(set) var inFlight = false

    init(capable: @escaping () -> Bool, observe: @escaping () -> GraphicsState?,
         send: @escaping (JSONObject) async throws -> JSONObject) {
        self.capable = capable
        self.observe = observe
        self.send = send
    }

    var available: Bool { capable() && observe()?.isUsable == true }

    func invoke(_ action: String, _ body: JSONObject) async -> GraphicsOperationResult {
        await invoke(action, request: GraphicsJSON.serialize(body))
    }

    func invoke(_ action: String, request: String) async -> GraphicsOperationResult {
        await invokeCore(action, request: request, socialReview: false)
    }

    /// The one path that may carry clientId graphics-social-review (SocialPlayout
    /// only); `invoke` keeps refusing it. The gate is symmetric, as on Windows
    /// (GraphicsCommandGateway.cs:45-46): this path refuses any OTHER client.
    func invokeSocial(_ action: String, _ body: JSONObject) async -> GraphicsOperationResult {
        await invokeCore(action, request: GraphicsJSON.serialize(body), socialReview: true)
    }

    private func invokeCore(_ action: String, request: String, socialReview: Bool) async -> GraphicsOperationResult {
        guard capable(), let observed = observe(), observed.isUsable else {
            return .refused(GraphicsCommandRequest.unavailable)
        }
        let built: GraphicsCommandRequest.Built
        switch GraphicsCommandRequest.build(action: action, request: request, observed: observed) {
        case .failure(let refusal): return .refused(refusal.error, requestId: refusal.requestId)
        case .success(let value): built = value
        }
        if (built.clientId == Self.socialClientId) != socialReview {
            return .refused("social-graphics-require-operator-review", requestId: built.requestId)
        }
        if inFlight { return .refused("graphics-command-overloaded", requestId: built.requestId) }
        inFlight = true
        defer { inFlight = false }
        let reply: JSONObject
        do {
            reply = try await send(["type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": [built.wire]])
        } catch {
            return .refused("\(Self.transportFailedPrefix): \(error.localizedDescription)", requestId: built.requestId)
        }
        let snapshot = reply["snapshot"] as? JSONObject ?? [:]
        if !snapshot.isEmpty { onSnapshot(snapshot) }
        let state = GraphicsState(node: snapshot["graphics"] as? JSONObject ?? [:])
        let result = state.operations.last { $0.requestId == built.requestId && $0.clientId == built.clientId } ?? state.lastResult
        guard state.coreEpoch == observed.coreEpoch, let result, result.requestId == built.requestId,
              result.clientId.isEmpty || result.clientId == built.clientId
        else { return .refused("graphics-result-not-correlated", requestId: built.requestId) }
        return result
    }
}
