// ControlApi.swift — the local control API's WIRE CONTRACT, ported from the
// Windows `native-shell/CoreVideoPro.Control` project (plain net9.0):
//
//   ControlActionRegistry.cs  -> ControlManifest.generated.swift (via contracts/control.manifest.json)
//   ControlCatalog.TryBind    -> ControlCatalog.bind / coerce
//   ControlManifest.cs        -> ControlCatalog.manifestJSON()  (PascalCase, like Windows)
//   Http/HttpControlRouter.cs -> ControlRouter                  (routes, statuses, bodies)
//   CoreSnapshotObserver.cs + SupportBundleLogRedactor.cs
//                             -> ControlSnapshotRedactor        (walk the JSON, never regex the text)
//
// Transport (sockets, HTTP parsing, auth, WebSocket) is ControlHttpServer.swift;
// the AppModel adapter is ControlSurface.swift. This file is pure: no AppKit, no
// sockets, no main actor — every rule here is unit-tested in ControlApiTests.swift.
//
// The catalog is GENERATED (contracts/generate.mjs) from the Windows registry
// export plus contracts/control.platforms.json, which says per action whether
// macOS answers it. An id the Mac supports binds exactly as on Windows. An id in
// the shared manifest the Mac does not support answers 422 with a generated
// reason ("not available on macOS yet (<workstream>)") and never reaches the
// surface. An id in neither file — including the retired
// `automation.magic|toggle|autoTake.set` — keeps Windows' own "Unknown action"
// 400. GET /manifest lists only supported actions and feedback fields: nothing
// is advertised that the Mac cannot perform.

import Foundation

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Catalog
// ─────────────────────────────────────────────────────────────────────────────

enum ControlParamType: String {
    // Raw values are the manifest's lower-cased type names (Windows
    // `p.Type.ToString().ToLowerInvariant()`); `coerceName` is the C# enum name
    // Windows' bind error quotes ("must be Bool").
    case string, int, double, bool

    var coerceName: String {
        switch self {
        case .string: return "String"
        case .int: return "Int"
        case .double: return "Double"
        case .bool: return "Bool"
        }
    }
}

struct ControlParam {
    let name: String
    let type: ControlParamType
    var required = true
    var description: String?
}

struct ControlAction {
    let id: String
    let title: String
    let description: String
    var params: [ControlParam] = []
}

enum ControlCatalog {
    /// Every Windows action in Windows' registry order, with its macOS availability.
    static let all: [ControlManifestEntry] = ControlManifestGenerated.entries

    /// The actions the Mac answers (Windows text verbatim, Windows order).
    static let actions: [ControlAction] = all.filter { $0.availability == .supported }.map(\.action)

    /// Windows `ControlManifest.StateFields`, filtered to the fields the Mac
    /// `/state` publishes truthfully (same relative order). Advertising a field the
    /// Mac never sends would invite a feedback that can never light.
    static let feedbackFields: [String] =
        ControlManifestGenerated.feedbackFields.filter { $0.availability == .supported }.map(\.field)

    static let oscRoot = ControlManifestGenerated.root

    private static let byId: [String: ControlManifestEntry] =
        Dictionary(all.map { ($0.action.id, $0) }, uniquingKeysWith: { first, _ in first })

    /// A SUPPORTED action (an unsupported id is not bindable here).
    static func action(_ id: String) -> ControlAction? {
        guard let entry = byId[id], entry.availability == .supported else { return nil }
        return entry.action
    }

    /// nil = the id is not in the shared manifest at all.
    static func availability(_ id: String) -> ControlMacAvailability? { byId[id]?.availability }

    /// The operator's sentence for an id the Mac does not answer; nil when supported.
    static func unavailableMessage(_ id: String, _ availability: ControlMacAvailability) -> String? {
        switch availability {
        case .supported: return nil
        case .blocked(_, let label): return "'\(id)' is not available on macOS yet (\(label))."
        case .notApplicable(let reason): return "'\(id)' is not available on macOS (\(reason))."
        }
    }

    /// Windows `OscAddressMap.ActionIdToAddress`.
    static func oscAddress(_ id: String) -> String { oscRoot + "/" + id.replacingOccurrences(of: ".", with: "/") }

    /// Windows `ControlCatalog.TryBind`: positional args against the schema.
    /// Extra trailing args are ignored; a missing REQUIRED arg is an error; a
    /// missing optional arg is nil. Errors never echo caller data (args can carry
    /// destination credentials on Windows), only the schema.
    ///
    /// Availability first: a known id the Mac does not support fails with 422 and
    /// its reason, before any arg is looked at; an id in neither file is Windows' 400.
    static func bind(_ id: String, _ raw: [Any?]) -> Result<[Any?], ControlBindError> {
        guard let action = action(id) else {
            if let availability = availability(id), let message = unavailableMessage(id, availability) {
                return .failure(ControlBindError(message: message, status: 422))
            }
            return .failure(ControlBindError(message: "Unknown action '\(id)'."))
        }
        var bound: [Any?] = []
        for (index, param) in action.params.enumerated() {
            let value = index < raw.count ? raw[index] : nil
            guard let value else {
                if param.required {
                    return .failure(ControlBindError(
                        message: "Action '\(id)' requires parameter '\(param.name)' at position \(index)."))
                }
                bound.append(nil)
                continue
            }
            guard let coerced = coerce(value, to: param.type) else {
                return .failure(ControlBindError(
                    message: "Action '\(id)' parameter '\(param.name)' must be \(param.type.coerceName)."))
            }
            bound.append(coerced)
        }
        return .success(bound)
    }

    /// Windows `ControlCatalog.TryCoerce`, case for case. Transport values are
    /// Bool, Int64 (a JSON integer), Double (any other JSON number) or String.
    static func coerce(_ raw: Any, to type: ControlParamType) -> Any? {
        switch type {
        case .string:
            // `raw as string ?? Convert.ToString(raw, InvariantCulture)`.
            switch raw {
            case let text as String: return text
            case let flag as Bool: return flag ? "True" : "False"
            case let integer as Int64: return String(integer)
            case let number as Double: return invariantString(number)
            // OSC `f` (D14): .NET float.ToString is shortest round-trip, integral floats without ".0".
            case let number as Float:
                if number == number.rounded(), abs(number) < 1e7 { return String(Int64(number)) }
                return String(number)
            default: return nil
            }
        case .int:
            switch raw {
            case let flag as Bool: return flag ? 1 : 0
            case let integer as Int64: return Int(Int32(truncatingIfNeeded: integer))
            case let number as Double:
                let rounded = number.rounded(.toNearestOrEven)   // Math.Round default
                guard rounded.isFinite, abs(rounded) <= Double(Int32.max) else { return nil }
                return Int(rounded)
            case let number as Float:
                let rounded = number.rounded(.toNearestOrEven)   // MathF.Round default
                guard rounded.isFinite, abs(rounded) <= Float(Int32.max) else { return nil }
                return Int(rounded)
            case let text as String:
                return Int32(text.trimmingCharacters(in: .whitespaces)).map { Int($0) }
            default: return nil
            }
        case .double:
            switch raw {
            case is Bool: return nil
            case let integer as Int64: return Double(integer)
            case let number as Double: return number
            case let number as Float: return Double(number)
            case let text as String: return Double(text.trimmingCharacters(in: .whitespaces))
            default: return nil
            }
        case .bool:
            switch raw {
            case let flag as Bool: return flag
            case let integer as Int64: return integer != 0
            case let number as Double: return abs(number) > Double.leastNonzeroMagnitude
            case let number as Float: return abs(number) > Float.leastNonzeroMagnitude
            case let text as String:
                // bool.TryParse is case-insensitive and trims; then "1"/"0".
                switch text.trimmingCharacters(in: .whitespaces).lowercased() {
                case "true": return true
                case "false": return false
                default: break
                }
                if text == "1" { return true }
                if text == "0" { return false }
                return nil
            default: return nil
            }
        }
    }

    private static func invariantString(_ number: Double) -> String {
        if number == number.rounded(), abs(number) < 1e15 { return String(Int64(number)) }
        return String(number)
    }

    /// Windows `ControlManifest.Build(...).ToJson()`: PascalCase keys (Windows
    /// serializes the record with no naming policy), WhenWritingNull omits a nil
    /// param description. Written in Windows' declaration order.
    static func manifestJSON() -> String {
        let actionsJSON = actions.map { action -> String in
            let params = action.params.map { param -> String in
                var fields = ["\"Name\":\(ControlJSON.string(param.name))",
                              "\"Type\":\(ControlJSON.string(param.type.rawValue))",
                              "\"Required\":\(param.required)"]
                if let description = param.description {
                    fields.append("\"Description\":\(ControlJSON.string(description))")
                }
                return "{" + fields.joined(separator: ",") + "}"
            }
            return "{\"Id\":\(ControlJSON.string(action.id)),"
                + "\"Title\":\(ControlJSON.string(action.title)),"
                + "\"Description\":\(ControlJSON.string(action.description)),"
                + "\"OscAddress\":\(ControlJSON.string(oscAddress(action.id))),"
                + "\"Params\":[" + params.joined(separator: ",") + "]}"
        }
        return "{\"Version\":\(ControlManifestGenerated.version),\"Root\":\(ControlJSON.string(oscRoot)),"
            + "\"Actions\":[" + actionsJSON.joined(separator: ",") + "],"
            + "\"FeedbackFields\":[" + feedbackFields.map(ControlJSON.string).joined(separator: ",") + "]}"
    }
}

/// Small JSON helpers shared by the router and the server.
enum ControlJSON {
    static let writeOptions: JSONSerialization.WritingOptions = [.sortedKeys, .withoutEscapingSlashes]

    static func string(_ text: String) -> String {
        let data = (try? JSONSerialization.data(withJSONObject: text,
                                                options: [.fragmentsAllowed, .withoutEscapingSlashes])) ?? Data("\"\"".utf8)
        return String(decoding: data, as: UTF8.self)
    }

    static func encode(_ object: Any) -> String? {
        guard JSONSerialization.isValidJSONObject(object),
              let data = try? JSONSerialization.data(withJSONObject: object, options: writeOptions) else { return nil }
        return String(decoding: data, as: UTF8.self)
    }

    static func error(_ message: String) -> String { "{\"ok\":false,\"error\":\(string(message))}" }

    /// Windows `JsonElementToValue`: string, bool, Int64 (else Double), null;
    /// arrays and objects travel as their raw JSON text.
    static func transportValue(_ value: Any) -> Any? {
        switch value {
        case is NSNull: return nil
        case let text as String: return text
        case let number as NSNumber:
            if CFGetTypeID(number) == CFBooleanGetTypeID() { return number.boolValue }
            if CFNumberIsFloatType(number) { return number.doubleValue }
            return number.int64Value
        default:
            guard let data = try? JSONSerialization.data(withJSONObject: value, options: [.withoutEscapingSlashes])
            else { return nil }
            return String(decoding: data, as: UTF8.self)
        }
    }

    static let isoFormatter: ISO8601DateFormatter = {
        let formatter = ISO8601DateFormatter()
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        formatter.timeZone = TimeZone(identifier: "UTC")
        return formatter
    }()
}

struct ControlBindError: Error, Equatable { let message: String; var status: Int = 400 }

/// Windows `ControlInvokeResult`.
struct ControlInvokeResult: Equatable {
    var ok: Bool
    var error: String?
    var previewSceneId: String?
    /// Raw JSON a read action answers with; embedded as-is under `data`.
    var data: String?

    static let success = ControlInvokeResult(ok: true)
    static func fail(_ error: String) -> ControlInvokeResult { ControlInvokeResult(ok: false, error: error) }
    static func appliedPreview(_ id: String) -> ControlInvokeResult { ControlInvokeResult(ok: true, previewSceneId: id) }
    static func withData(_ json: String) -> ControlInvokeResult { ControlInvokeResult(ok: true, data: json) }
}

/// What the shell already holds from its last core sync. `state` is the core's
/// own session-state object (the `snapshot` of the sync reply), UNREDACTED —
/// the router redacts it at the boundary (Windows CoreSnapshotObserver).
struct ControlSnapshotObservation {
    var state: [String: Any]?
    var receivedAt: Date?
    var unavailableReason: String?

    static func unavailable(_ reason: String) -> ControlSnapshotObservation {
        ControlSnapshotObservation(state: nil, receivedAt: nil, unavailableReason: reason)
    }
}

/// Windows `IControlSurface` + `INativeSnapshotObserver`. `invoke` and
/// `controlState` hop to wherever the model lives (the main actor for
/// AppModel); `snapshotObservation` must NOT — it reads a lock-protected copy,
/// like Windows reads the reference the bridge already published.
protocol ControlSurface: AnyObject {
    func invoke(_ actionId: String, _ args: [Any?]) async -> ControlInvokeResult
    func controlState() async -> [String: Any]
    /// nil = this surface does not observe a media core ("not-observed").
    var observesCore: Bool { get }
    func snapshotObservation() throws -> ControlSnapshotObservation
}

struct ControlResponse: Equatable {
    var status: Int
    var body: String
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Router
// ─────────────────────────────────────────────────────────────────────────────

/// Windows `HttpControlRouter`: transport-independent (method, path, body) ->
/// response. Routes: GET /manifest, GET /state, GET /snapshot, POST /invoke;
/// anything else 404 `{"ok":false,"error":"Not found"}`.
final class ControlRouter {
    /// Windows `HttpControlRouter.SnapshotStaleAfter`.
    static let snapshotStaleAfterMs = 2000.0

    let surface: ControlSurface

    init(surface: ControlSurface) { self.surface = surface }

    func handle(method: String, path: String, body: Data?) async -> ControlResponse {
        let route = Self.normalize(path)
        if method == "GET" && route == "/manifest" {
            return ControlResponse(status: 200, body: ControlCatalog.manifestJSON())
        }
        if method == "GET" && route == "/state" {
            return ControlResponse(status: 200, body: await stateJSON())
        }
        if method == "GET" && route == "/snapshot" {
            return ControlResponse(status: 200, body: snapshotEnvelope(servedAt: Date()))
        }
        if method == "POST" && route == "/invoke" {
            return await invoke(body)
        }
        return ControlResponse(status: 404, body: ControlJSON.error("Not found"))
    }

    func stateJSON() async -> String {
        ControlJSON.encode(await surface.controlState()) ?? "{}"
    }

    /// Windows `NormalizePath`: drop the query, trim trailing slashes.
    static func normalize(_ path: String) -> String {
        var trimmed = String(path.split(separator: "?", maxSplits: 1, omittingEmptySubsequences: false).first ?? "/")
        while trimmed.hasSuffix("/") { trimmed.removeLast() }
        return trimmed.isEmpty ? "/" : trimmed
    }

    private func invoke(_ body: Data?) async -> ControlResponse {
        func fail(_ status: Int, _ message: String) -> ControlResponse {
            ControlResponse(status: status, body: ControlJSON.error(message))
        }
        guard let body, let text = String(data: body, encoding: .utf8),
              !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            return fail(400, "Request body is required.")
        }
        let parsed: Any
        do {
            parsed = try JSONSerialization.jsonObject(with: body, options: [.fragmentsAllowed])
        } catch {
            return fail(400, "Invalid JSON: \((error as NSError).userInfo[NSDebugDescriptionErrorKey] as? String ?? error.localizedDescription)")
        }
        let root = parsed as? [String: Any] ?? [:]
        guard let actionId = root["action"] as? String,
              !actionId.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            return fail(400, "Missing 'action'.")
        }
        let rawArgs = (root["args"] as? [Any] ?? []).map(ControlJSON.transportValue)
        let bound: [Any?]
        switch ControlCatalog.bind(actionId, rawArgs) {
        case .success(let values): bound = values
        case .failure(let error): return fail(error.status, error.message)
        }
        let result = await surface.invoke(actionId, bound)
        guard result.ok else { return fail(422, result.error ?? "Invocation failed.") }
        if let preview = result.previewSceneId, !preview.isEmpty {
            return ControlResponse(status: 200, body: "{\"ok\":true,\"previewSceneId\":\(ControlJSON.string(preview))}")
        }
        if let data = result.data, !data.isEmpty {
            // Read actions answer with their own JSON; embedded as-is, not re-encoded.
            return ControlResponse(status: 200, body: "{\"ok\":true,\"data\":" + data + "}")
        }
        return ControlResponse(status: 200, body: "{\"ok\":true}")
    }

    /// Windows `BuildSnapshotEnvelope`: always states availability and age; the
    /// snapshot itself is the core's own document (redacted, never re-shaped).
    func snapshotEnvelope(servedAt: Date) -> String {
        guard surface.observesCore else {
            return Self.unavailable(servedAt, "not-observed", "This control surface does not observe a media core.")
        }
        let observation: ControlSnapshotObservation
        do {
            observation = try surface.snapshotObservation()
        } catch {
            return Self.unavailable(servedAt, "observer-failed",
                                    (error as? LocalizedError)?.errorDescription ?? "\(error)")
        }
        guard let state = observation.state, !state.isEmpty else {
            return Self.unavailable(servedAt, "no-snapshot",
                                    observation.unavailableReason ?? "The shell has not received a core snapshot yet.")
        }
        // Fail closed (Windows CoreSnapshotObserver): a document that cannot be
        // walked and re-encoded cannot be proven redacted, so it is withheld.
        let redacted = ControlSnapshotRedactor.redact(state)
        guard let redactedState = redacted as? [String: Any], JSONSerialization.isValidJSONObject(redactedState) else {
            return Self.unavailable(servedAt, "no-snapshot",
                                    "The core snapshot could not be redacted and was withheld.")
        }
        var envelope: [String: Any] = [
            "ok": true,
            "available": true,
            "servedUtc": ControlJSON.isoFormatter.string(from: servedAt),
            "staleAfterMs": Self.snapshotStaleAfterMs,
            "redacted": true,
            "nodes": redactedState.keys.sorted(),
            "snapshot": redactedState,
        ]
        if let received = observation.receivedAt {
            let ageMs = (servedAt.timeIntervalSince(received) * 1000 * 1000).rounded() / 1000
            envelope["receivedUtc"] = ControlJSON.isoFormatter.string(from: received)
            envelope["ageMs"] = ageMs
            envelope["stale"] = ageMs > Self.snapshotStaleAfterMs
        } else {
            // A snapshot with no receipt time cannot be aged; say so rather than
            // implying freshness by omission.
            envelope["receivedUtc"] = NSNull()
            envelope["ageMs"] = NSNull()
            envelope["stale"] = true
            envelope["staleReason"] = "The shell did not record when this snapshot arrived."
        }
        return ControlJSON.encode(envelope)
            ?? Self.unavailable(servedAt, "no-snapshot", "The core snapshot could not be redacted and was withheld.")
    }

    private static func unavailable(_ servedAt: Date, _ reasonCode: String, _ reason: String) -> String {
        let envelope: [String: Any] = [
            "ok": true,
            "available": false,
            "servedUtc": ControlJSON.isoFormatter.string(from: servedAt),
            "receivedUtc": NSNull(),
            "ageMs": NSNull(),
            "stale": true,
            "staleAfterMs": snapshotStaleAfterMs,
            "reasonCode": reasonCode,
            "reason": reason,
            "nodes": [String](),
            "snapshot": NSNull(),
        ]
        return ControlJSON.encode(envelope) ?? "{\"ok\":true,\"available\":false}"
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Snapshot redaction
// ─────────────────────────────────────────────────────────────────────────────

/// Port of Windows `CoreSnapshotObserver` (walk + secret-NAMED values) and the
/// `SupportBundleLogRedactor` value filter it applies per string. The Mac
/// support-bundle `Redactor` (Diagnostics.swift) is deliberately NOT reused: it
/// is a different, stricter filter (presence markers, a high-entropy catch-all,
/// substring names) and the served snapshot must follow the Windows contract.
enum ControlSnapshotRedactor {
    static let placeholder = "[redacted]"
    static let filterName = "support-bundle-log-redactor"

    /// Windows `SecretNameSuffixes`: SUFFIXES, not substrings, so the core's
    /// `keyPhase` / `keyer` / `keyPosition` / `keyLength` diagnostics survive.
    private static let secretNameSuffixes =
        ["key", "token", "secret", "password", "passphrase", "jwt", "zak", "credential"]
    /// Windows `SecretNames` (exact, case-insensitive).
    private static let secretNames: Set<String> = ["pwd", "authorization", "auth", "signature", "sig"]

    static func isSecretName(_ name: String) -> Bool {
        let lowered = name.lowercased()
        return secretNames.contains(lowered) || secretNameSuffixes.contains { lowered.hasSuffix($0) }
    }

    /// Windows `WriteRedacted`: secret-named values (of any type, objects as a
    /// unit) become the placeholder; every surviving string goes through the
    /// value filter; numbers, booleans and structure are untouched.
    static func redact(_ value: Any) -> Any {
        if let object = value as? [String: Any] {
            var result: [String: Any] = [:]
            for (key, child) in object {
                result[key] = isSecretName(key) ? placeholder : redact(child)
            }
            return result
        }
        if let array = value as? [Any] { return array.map(redact) }
        if let text = value as? String { return redactText(text) }
        return value
    }

    // Windows `SupportBundleLogRedactor` rules, same patterns, same order.
    private static func rule(_ pattern: String, _ options: NSRegularExpression.Options) -> NSRegularExpression {
        // A malformed literal is a programming error; failing at first use beats
        // serving an unfiltered snapshot.
        // swiftlint:disable:next force_try
        try! NSRegularExpression(pattern: pattern, options: options)
    }
    private static let rtmpUrlPath = rule(#"(rtmps?://[^/\s"']+/)\S+"#, [.caseInsensitive])
    private static let secretJsonField = rule(
        #""(?=\w*(?:key|token|passphrase|password|secret|pwd|zak|jwt))((?>\w+))"\s*:\s*"([^"]*)""#,
        [.caseInsensitive])
    private static let secretAssignment = rule(
        #"((?:key|token|passphrase|password|secret|pwd|zak|jwt))=([^&\s"',}\]]+)"#, [.caseInsensitive])
    private static let bearerToken = rule(#"\bBearer\s+[A-Za-z0-9\-_.=+/]+"#, [])

    /// Windows `SupportBundleLogRedactor.Redact`, applied to ONE string value.
    static func redactText(_ text: String) -> String {
        guard !text.isEmpty else { return text }
        var result = text
        func apply(_ regex: NSRegularExpression, _ template: String) {
            result = regex.stringByReplacingMatches(
                in: result, options: [], range: NSRange(result.startIndex..., in: result), withTemplate: template)
        }
        apply(rtmpUrlPath, "$1" + placeholder)
        apply(secretJsonField, "\"$1\":\"" + placeholder + "\"")
        apply(secretAssignment, "$1=" + placeholder)
        apply(bearerToken, "Bearer " + placeholder)
        return redactJwts(result)
    }

    /// Windows `JwtShaped` — `\beyJ[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{8,}\.[A-Za-z0-9_\-]{4,}\b`
    /// — as a LINEAR scan. Windows had to run that pattern with
    /// RegexOptions.NonBacktracking (#998): many "eyJ" starts in one long run each
    /// rescan the run. ICU has no non-backtracking mode, so the same leftmost
    /// matches are found by walking runs of the token class once.
    static func redactJwts(_ text: String) -> String {
        let scalars = Array(text.unicodeScalars)
        let count = scalars.count
        guard count >= 3 else { return text }
        func isTokenChar(_ index: Int) -> Bool {
            guard index >= 0, index < count else { return false }
            let value = scalars[index].value
            return (value >= 0x41 && value <= 0x5A) || (value >= 0x61 && value <= 0x7A)
                || (value >= 0x30 && value <= 0x39) || value == 0x5F || value == 0x2D
        }
        func isWord(_ index: Int) -> Bool {   // .NET \w
            guard index >= 0, index < count else { return false }
            switch scalars[index].properties.generalCategory {
            case .uppercaseLetter, .lowercaseLetter, .titlecaseLetter, .modifierLetter, .otherLetter,
                 .nonspacingMark, .decimalNumber, .connectorPunctuation: return true
            default: return false
            }
        }
        func runEnd(_ start: Int) -> Int {
            var end = start
            while end < count && isTokenChar(end) { end += 1 }
            return end
        }
        var matches: [(Int, Int)] = []
        var position = 0
        while position < count {
            guard isTokenChar(position) else { position += 1; continue }
            let segmentEnd = runEnd(position)
            // Leftmost "eyJ" with \b before it and >= 8 token chars after it in
            // this segment (the first two quantifiers end at a '.').
            var start: Int?
            var probe = position
            while probe + 11 <= segmentEnd {
                if scalars[probe] == "e", scalars[probe + 1] == "y", scalars[probe + 2] == "J", !isWord(probe - 1) {
                    start = probe
                    break
                }
                probe += 1
            }
            guard let matchStart = start, segmentEnd < count, scalars[segmentEnd] == "." else {
                position = segmentEnd
                continue
            }
            let secondEnd = runEnd(segmentEnd + 1)
            guard secondEnd - (segmentEnd + 1) >= 8, secondEnd < count, scalars[secondEnd] == "." else {
                position = segmentEnd
                continue
            }
            let thirdStart = secondEnd + 1
            let thirdEnd = runEnd(thirdStart)
            // {4,} is greedy then \b: the longest prefix ending on a word boundary.
            var matchEnd: Int?
            var end = thirdEnd
            while end >= thirdStart + 4 {
                if isWord(end - 1) != isWord(end) { matchEnd = end; break }
                end -= 1
            }
            guard let finish = matchEnd else {
                position = segmentEnd
                continue
            }
            matches.append((matchStart, finish))
            position = finish
        }
        guard !matches.isEmpty else { return text }
        var output = String.UnicodeScalarView()
        var cursor = 0
        for (from, to) in matches {
            output.append(contentsOf: scalars[cursor..<from])
            output.append(contentsOf: placeholder.unicodeScalars)
            cursor = to
        }
        output.append(contentsOf: scalars[cursor..<count])
        return String(output)
    }
}
