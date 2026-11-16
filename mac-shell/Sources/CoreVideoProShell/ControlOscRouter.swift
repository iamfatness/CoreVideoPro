// ControlOscRouter.swift — OSC address mapping, routing and feedback encoding, pure
// (no sockets). Ports of Windows OscAddressMap, OscControlRouter and OscFeedback,
// pinned by the shared fixtures (osc.addresses / osc.routes / osc.feedback).
// Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Task 18).

import Foundation

enum ControlOscAddressMap {
    /// Windows AddressToActionId: nil outside `/cvp/`, for an empty tail, and for the
    /// `state/` feedback namespace; otherwise the tail with `/` as `.` (so "/cvp/state",
    /// with no slash after state, is "state", as on Windows).
    static func actionId(for address: String) -> String? {
        let prefix = ControlCatalog.oscRoot + "/"
        guard address.hasPrefix(prefix) else { return nil }
        let tail = String(address.dropFirst(prefix.count))
        guard !tail.isEmpty, !tail.hasPrefix("state/") else { return nil }
        return tail.replacingOccurrences(of: "/", with: ".")
    }

    static func stateAddress(_ field: String) -> String { ControlCatalog.oscRoot + "/state/" + field }
}

final class ControlOscRouter: @unchecked Sendable {
    let surface: ControlSurface

    init(surface: ControlSurface) { self.surface = surface }

    /// nil = not an action (ignored). An unsupported or unbindable action answers
    /// `.fail` without reaching the surface (D1 applied to OSC: logged, never sent).
    func route(_ message: ControlOscMessage) async -> ControlInvokeResult? {
        guard let id = ControlOscAddressMap.actionId(for: message.address),
              ControlCatalog.availability(id) != nil else { return nil }
        switch ControlCatalog.bind(id, ControlOscCodec.transportValues(message.args)) {
        case .failure(let error): return .fail(error.message)
        case .success(let bound): return await surface.invoke(id, bound)
        }
    }
}

enum ControlOscFeedback {
    private enum Encoding { case flag, string, int, float32 }

    /// Windows OscFeedback.Encode order (C/Osc/OscFeedback.cs), scalar lines; field = state key
    /// except `ndi`, which Windows reads from `ndiOutputOn`.
    private static let scalars: [(field: String, key: String, encoding: Encoding)] = [
        ("recording", "recording", .flag), ("streaming", "streaming", .flag), ("ndi", "ndiOutputOn", .flag),
        ("engineOn", "engineOn", .flag), ("canTake", "canTake", .flag), ("lowerThirdOnAir", "lowerThirdOnAir", .flag),
        ("zoomStatus", "zoomStatus", .string), ("engineStatus", "engineStatus", .string),
        ("commandStatus", "commandStatus", .string), ("activeSceneId", "activeSceneId", .string),
        ("previewSceneId", "previewSceneId", .string), ("previewTilesNamesOn", "previewTilesNamesOn", .flag),
        ("viewMode", "viewMode", .string), ("multiviewLayoutMode", "multiviewLayoutMode", .string),
        ("multiviewTileCount", "multiviewTileCount", .int), ("automationOn", "automationOn", .flag),
        ("autoTake", "autoTake", .flag), ("autoAssignInputs", "autoAssignInputs", .flag),
        ("autoLowerThirds", "autoLowerThirds", .flag), ("autoCaptions", "autoCaptions", .flag),
        ("audioMonitorOn", "audioMonitorOn", .flag), ("audioMonitorVolume", "audioMonitorVolume", .float32),
        ("masterLimiterOn", "masterLimiterOn", .flag),
    ]

    private static let supported: Set<String> =
        Set(ControlManifestGenerated.feedbackFields.filter { $0.availability == .supported }.map(\.field))

    private static func value(_ raw: Any?, _ encoding: Encoding) -> ControlOscArg {
        switch encoding {
        case .flag: return .int((raw as? Bool ?? false) ? 1 : 0)
        case .string: return .string(raw as? String ?? "")
        case .int: return .int(Int32(clamping: (raw as? Int) ?? 0))
        case .float32: return .float(Float((raw as? Double) ?? (raw as? Int).map(Double.init) ?? 0))
        }
    }

    /// The lines whose platform row is supported (graphics/* and ohg/* never are on the Mac).
    static func encode(_ state: [String: Any]) -> [ControlOscMessage] {
        var out: [ControlOscMessage] = []
        for line in scalars where supported.contains(line.field) {
            out.append(ControlOscMessage(address: ControlOscAddressMap.stateAddress(line.field),
                                         args: [value(state[line.key], line.encoding)]))
        }
        for input in state["inputs"] as? [[String: Any]] ?? [] {
            let slot = input["slot"] as? Int ?? 0
            let lines: [(String, ControlOscArg)] = [
                ("inShow", value(input["inShow"], .flag)),
                ("kind", value(input["kind"], .string)),
                ("source", value(input["sourceId"], .string)),   // a null id is ""
                ("name", value(input["name"], .string)),
            ]
            for (suffix, arg) in lines where supported.contains("input/{slot}/" + suffix) {
                out.append(ControlOscMessage(address: ControlOscAddressMap.stateAddress("input/\(slot)/\(suffix)"), args: [arg]))
            }
        }
        return out
    }
}
