// ControlSurface+Scenes.swift — scene.cueWithRoutes, scene.route.set (RC7, Task 4) and
// scene.restoreStarters (Task 5). Windows: StudioControlSurface.cs:316-324,845-873 and
// CoreVideoPro.Control/SceneCueWithRoutesArgs.cs. Route ids on the Mac are SceneLayer ids
// ("<scene>-layer-<n>", plan decision D-C2, the rule Volt already uses): a Companion button
// made on Windows with a Windows route id answers "Unknown route ..." here, truthfully.

import Foundation

/// An ordered `routeId -> slot` map (Windows' Dictionary keeps insertion order, and
/// Refuse reports the FIRST unknown route in that order).
struct SceneRouteMap: Equatable {
    private(set) var entries: [(route: String, slot: Int?)] = []

    init() {}
    init(_ pairs: [(String, Int?)]) { for (route, slot) in pairs { self[route] = slot } }

    /// Assigning an existing key keeps its position (Dictionary indexer semantics).
    subscript(route: String) -> Int?? {
        get { entries.first { $0.route == route }.map { $0.slot } }
        set {
            if let index = entries.firstIndex(where: { $0.route == route }) {
                if let value = newValue { entries[index].slot = value } else { entries.remove(at: index) }
            } else if let value = newValue {
                entries.append((route, value))
            }
        }
    }

    var count: Int { entries.count }

    static func == (lhs: SceneRouteMap, rhs: SceneRouteMap) -> Bool {
        lhs.entries.map(\.route) == rhs.entries.map(\.route) && lhs.entries.map(\.slot) == rhs.entries.map(\.slot)
    }
}

/// Windows SceneCueWithRoutesArgs, twin for twin.
enum SceneRoutesArgs {
    static let shapeError = "routes must be a JSON object of routeId to Show Input slot (or null)."
    static let notJsonPrefix = "routes is not JSON: "

    /// TryParseRoutes. The whole document is validated first (JsonDocument.Parse), then
    /// each property in order: a blank key, then null, then an Int32 integer literal.
    /// A JSON syntax error answers `routes is not JSON: <detail>`; the detail is this
    /// parser's own (System.Text.Json's wording is not portable; the vectors carry only
    /// the prefix for that case).
    /// The outcome of `parse`: the routes, or Windows' refusal sentence.
    enum Parsed { case routes(SceneRouteMap), refused(String) }

    static func parse(_ raw: String) -> Parsed {
        if raw.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return .refused(shapeError) }
        let value: JSONValue
        do { value = try JSONValue.parseDocument(raw) } catch {
            return .refused(notJsonPrefix + ((error as? JSONValue.SyntaxError)?.message ?? "invalid JSON."))
        }
        guard case .object(let members) = value else { return .refused(shapeError) }
        var routes = SceneRouteMap()
        for (name, member) in members {
            if name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                return .refused("routes keys must be non-empty route ids.")
            }
            switch member {
            case .null:
                routes[name] = .some(nil)
            case .number(let literal):
                // JsonElement.TryGetInt32: an integer literal (no fraction, no exponent) in Int32.
                guard !literal.contains(where: { $0 == "." || $0 == "e" || $0 == "E" }),
                      let slot = Int32(literal) else {
                    return .refused("routes '\(name)' must be an integer slot or null.")
                }
                routes[name] = .some(Int(slot))
            default:
                return .refused("routes '\(name)' must be an integer slot or null.")
            }
        }
        return .routes(routes)
    }

    /// Refuse: nil when the cue may run. Scene, then every route, then every slot.
    static func refuse(sceneId: String, knownScenes: [String], knownRouteIds: [String], knownSlots: [Int],
                       routes: SceneRouteMap) -> String? {
        if sceneId.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty || !knownScenes.contains(sceneId) {
            return "Unknown scene '\(sceneId)'."
        }
        for entry in routes.entries where !knownRouteIds.contains(entry.route) {
            return "Unknown route '\(entry.route)' on scene '\(sceneId)'."
        }
        for entry in routes.entries {
            if let slot = entry.slot, !knownSlots.contains(slot) {
                return "Unknown Show Input slot \(slot) for route '\(entry.route)'."
            }
        }
        return nil
    }
}

/// A strict RFC 8259 document parser that keeps object member ORDER and number
/// LITERALS (JSONSerialization keeps neither). Only what the route parse needs.
indirect enum JSONValue {
    case null, bool(Bool), number(String), string(String), array([JSONValue]), object([(String, JSONValue)])

    struct SyntaxError: Error { let message: String }

    static func parseDocument(_ text: String) throws -> JSONValue {
        var parser = Parser(Array(text.utf8))
        parser.skipWhitespace()
        let value = try parser.value(depth: 0)
        parser.skipWhitespace()
        guard parser.atEnd else { throw parser.error("unexpected data after the JSON value") }
        return value
    }

    private struct Parser {
        let bytes: [UInt8]
        var index = 0
        init(_ bytes: [UInt8]) { self.bytes = bytes }

        var atEnd: Bool { index >= bytes.count }
        func error(_ what: String) -> SyntaxError { SyntaxError(message: "\(what) at byte \(index).") }

        mutating func skipWhitespace() {
            while !atEnd, [0x20, 0x09, 0x0A, 0x0D].contains(bytes[index]) { index += 1 }
        }

        mutating func value(depth: Int) throws -> JSONValue {
            guard depth < 64 else { throw error("maximum depth exceeded") }
            guard !atEnd else { throw error("unexpected end of data") }
            switch bytes[index] {
            case UInt8(ascii: "{"): return try object(depth: depth)
            case UInt8(ascii: "["): return try array(depth: depth)
            case UInt8(ascii: "\""): return .string(try string())
            case UInt8(ascii: "t"): try literal("true"); return .bool(true)
            case UInt8(ascii: "f"): try literal("false"); return .bool(false)
            case UInt8(ascii: "n"): try literal("null"); return .null
            case UInt8(ascii: "-"), UInt8(ascii: "0")...UInt8(ascii: "9"): return .number(try number())
            default: throw error("invalid start of a value")
            }
        }

        mutating func literal(_ word: String) throws {
            let expected = Array(word.utf8)
            guard index + expected.count <= bytes.count, Array(bytes[index..<index + expected.count]) == expected else {
                throw error("invalid literal, expected '\(word)'")
            }
            index += expected.count
        }

        mutating func number() throws -> String {
            let start = index
            if bytes[index] == UInt8(ascii: "-") { index += 1 }
            func digits() -> Int { let s = index; while !atEnd, (0x30...0x39).contains(bytes[index]) { index += 1 }; return index - s }
            guard !atEnd else { throw error("incomplete number") }
            if bytes[index] == UInt8(ascii: "0") {
                index += 1
                if !atEnd, (0x30...0x39).contains(bytes[index]) { throw error("leading zero in a number") }
            } else if digits() == 0 { throw error("invalid number") }
            if !atEnd, bytes[index] == UInt8(ascii: ".") {
                index += 1
                guard digits() > 0 else { throw error("expected a digit after the decimal point") }
            }
            if !atEnd, bytes[index] == UInt8(ascii: "e") || bytes[index] == UInt8(ascii: "E") {
                index += 1
                if !atEnd, bytes[index] == UInt8(ascii: "+") || bytes[index] == UInt8(ascii: "-") { index += 1 }
                guard digits() > 0 else { throw error("expected a digit in the exponent") }
            }
            return String(decoding: bytes[start..<index], as: UTF8.self)
        }

        mutating func string() throws -> String {
            index += 1   // opening quote
            var out: [UInt8] = []
            while true {
                guard !atEnd else { throw error("unterminated string") }
                let byte = bytes[index]
                if byte == UInt8(ascii: "\"") { index += 1; break }
                if byte < 0x20 { throw error("control character in a string") }
                if byte == UInt8(ascii: "\\") {
                    index += 1
                    guard !atEnd else { throw error("unterminated escape") }
                    let escape = bytes[index]; index += 1
                    switch escape {
                    case UInt8(ascii: "\""), UInt8(ascii: "\\"), UInt8(ascii: "/"): out.append(escape)
                    case UInt8(ascii: "b"): out.append(0x08)
                    case UInt8(ascii: "f"): out.append(0x0C)
                    case UInt8(ascii: "n"): out.append(0x0A)
                    case UInt8(ascii: "r"): out.append(0x0D)
                    case UInt8(ascii: "t"): out.append(0x09)
                    case UInt8(ascii: "u"):
                        var scalar = try hex4()
                        if (0xD800...0xDBFF).contains(scalar) {
                            guard index + 1 < bytes.count, bytes[index] == UInt8(ascii: "\\"),
                                  bytes[index + 1] == UInt8(ascii: "u") else { throw error("unpaired surrogate") }
                            index += 2
                            let low = try hex4()
                            guard (0xDC00...0xDFFF).contains(low) else { throw error("invalid surrogate pair") }
                            scalar = 0x10000 + ((scalar - 0xD800) << 10) + (low - 0xDC00)
                        } else if (0xDC00...0xDFFF).contains(scalar) {
                            throw error("unpaired surrogate")
                        }
                        out.append(contentsOf: Array(String(Character(Unicode.Scalar(scalar)!)).utf8))
                    default: throw error("invalid escape")
                    }
                    continue
                }
                out.append(byte); index += 1
            }
            guard let text = String(bytes: out, encoding: .utf8) else { throw error("invalid UTF-8 in a string") }
            return text
        }

        mutating func hex4() throws -> UInt32 {
            guard index + 4 <= bytes.count,
                  let value = UInt32(String(decoding: bytes[index..<index + 4], as: UTF8.self), radix: 16) else {
                throw error("invalid \\u escape")
            }
            index += 4
            return value
        }

        mutating func array(depth: Int) throws -> JSONValue {
            index += 1
            var items: [JSONValue] = []
            skipWhitespace()
            if !atEnd, bytes[index] == UInt8(ascii: "]") { index += 1; return .array(items) }
            while true {
                skipWhitespace()
                items.append(try value(depth: depth + 1))
                skipWhitespace()
                guard !atEnd else { throw error("unterminated array") }
                if bytes[index] == UInt8(ascii: ",") { index += 1; continue }
                if bytes[index] == UInt8(ascii: "]") { index += 1; return .array(items) }
                throw error("expected ',' or ']'")
            }
        }

        mutating func object(depth: Int) throws -> JSONValue {
            index += 1
            var members: [(String, JSONValue)] = []
            skipWhitespace()
            if !atEnd, bytes[index] == UInt8(ascii: "}") { index += 1; return .object(members) }
            while true {
                skipWhitespace()
                guard !atEnd, bytes[index] == UInt8(ascii: "\"") else { throw error("expected a property name") }
                let name = try string()
                skipWhitespace()
                guard !atEnd, bytes[index] == UInt8(ascii: ":") else { throw error("expected ':'") }
                index += 1
                skipWhitespace()
                members.append((name, try value(depth: depth + 1)))
                skipWhitespace()
                guard !atEnd else { throw error("unterminated object") }
                if bytes[index] == UInt8(ascii: ",") { index += 1; continue }
                if bytes[index] == UInt8(ascii: "}") { index += 1; return .object(members) }
                throw error("expected ',' or '}'")
            }
        }
    }
}

extension AppModelControlSurface {
    /// Windows CuePreviewWithRoutes, all-or-nothing: every route and slot is checked
    /// with nothing changed; only then is the scene put on Preview and each layer's
    /// slot written (nil = follow the active speaker, the Mac's empty-route value), in
    /// ONE scene sync.
    @MainActor
    static func cueWithRoutes(_ model: AppModel, sceneId: String, routes: SceneRouteMap) -> ControlInvokeResult {
        guard let layers = model.sceneLayerCandidates(sceneId) else { return .fail("Unknown scene '\(sceneId)'.") }
        if let refusal = SceneRoutesArgs.refuse(sceneId: sceneId, knownScenes: model.scenes.map(\.id),
                                                knownRouteIds: layers.map { $0.id }, knownSlots: model.slots.map(\.id),
                                                routes: routes) {
            return .fail(refusal)
        }
        guard model.applyPreviewRoutes(sceneId: sceneId, routes: routes) else {
            return .fail("Scene '\(sceneId)' was not queued on Preview.")
        }
        return .appliedPreview(sceneId)
    }
}

extension AppModel {
    /// scene.cueWithRoutes' apply half, run only after every route and slot was
    /// checked: Preview moves to the scene and each named layer's slot is written
    /// (nil = follow the active speaker; a slot route is no longer a media route),
    /// in ONE scene sync. False when Preview did not land on the scene.
    func applyPreviewRoutes(sceneId: String, routes: SceneRouteMap) -> Bool {
        previewSceneId = sceneId
        ensureLayers(for: sceneId)
        guard previewSceneId == sceneId,
              let sceneIndex = scenes.firstIndex(where: { $0.id == sceneId }) else { syncScenes(); return false }
        for entry in routes.entries {
            guard let layerIndex = scenes[sceneIndex].layers.firstIndex(where: { $0.id == entry.route }) else { continue }
            scenes[sceneIndex].layers[layerIndex].slotId = entry.slot
            scenes[sceneIndex].layers[layerIndex].mediaAssetId = nil
        }
        syncScenes()
        return true
    }
}
