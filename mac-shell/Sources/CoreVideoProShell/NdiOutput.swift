// NDI output as its own output (macOS parity Phase 4, Task 9 / N1b).
//
// Windows rules (docs/reference/stream-destination-control.md "NDI output"):
// independent of Streaming and Recording; on/off persisted, default off; the
// format is the stream profile (Mac: canvas size at the stream fps); a failure
// while on leaves NDI on and reads "Warning · …"; NDI never starts the footer
// ELAPSED clock, never counts as streaming and never prompts on quit; a stopped
// core reads "Starting", never a stale "Live".
//
// The NDI runtime is the operator's own install, found by the core at run time
// (Task 8); it is never bundled. The graphics-key source is out of scope on Mac.
//
// The control actions (transport.ndi.set / toggle, feedback `ndi`) are the
// control lane's Task 6; it drives AppModel.setNdiOutput(_:) below.

import Foundation

struct NdiOutputSettings: Codable, Equatable {
    var enabled = false
    var programName = "CoreVideo Pro Program"
    var groupName = "public"

    init() {}
    init(from decoder: Decoder) throws {
        // Additive prefs: a missing key keeps its default.
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let defaults = NdiOutputSettings()
        enabled = try c.decodeIfPresent(Bool.self, forKey: .enabled) ?? defaults.enabled
        programName = try c.decodeIfPresent(String.self, forKey: .programName) ?? defaults.programName
        groupName = try c.decodeIfPresent(String.self, forKey: .groupName) ?? defaults.groupName
    }
}

enum NdiOutputPolicy {
    static let nameRequired = "Configure an NDI program name before turning on NDI output."
    static let missingCapability = "NDI output is not available in this core build (missing ndi-output)."
    /// Shown where Windows has the graphics-key rows.
    static let graphicsKeyUnavailable = "NDI graphics key is not available on Mac."

    /// Windows `StudioStreamOutputValidation.ValidateNdiOutput` without the
    /// graphics-key clause. `coreHasNdiOutput == nil` means the core is not up
    /// yet: defer to the core's own sender status (Windows
    /// CoreNotUpYetDefersToTheCoresOwnSenderStatus).
    static func validate(programName: String, coreHasNdiOutput: Bool?) -> String? {
        if programName.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return nameRequired }
        if coreHasNdiOutput == false { return missingCapability }
        return nil
    }

    /// Windows `StudioOutputTelemetry.NdiStatus` without the " + key" suffix.
    /// Reads the sender's `status`, exactly as Windows does (deliberate parity).
    static func status(enabled: Bool, senders: [JSONObject], programName: String, refusalReason: String?) -> String {
        if !enabled {
            if let reason = refusalReason, !reason.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                return "Unavailable \u{00B7} \(reason)"
            }
            return "Off"
        }
        guard let sender = senders.first(where: { $0["destination"] as? String == "ndi" }) else { return "Starting" }
        switch sender["status"] as? String {
        case "live":
            let name = programName.trimmingCharacters(in: .whitespacesAndNewlines)
            return "Live \u{00B7} \(name.isEmpty ? "NDI" : programName)"
        case "warning", "failed":
            let first = ["warning", "lastError", "runtimeDetail", "lastResultCode"]
                .compactMap { sender[$0] as? String }
                .first { !$0.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
            return "Warning \u{00B7} \(first ?? (sender["status"] as? String ?? ""))"
        default:
            return "Starting"
        }
    }

    /// Windows `NormalizeOutputText`: trimmed, or the default when blank.
    static func normalized(_ text: String, _ fallback: String) -> String {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        return trimmed.isEmpty ? fallback : trimmed
    }

    /// The destination row (Windows `BuildNdiDestinationSettings`), only when NDI
    /// is on and its name validates.
    static func destinationSettings(_ s: NdiOutputSettings, streamFps: Int) -> [JSONObject] {
        guard s.enabled, validate(programName: s.programName, coreHasNdiOutput: nil) == nil else { return [] }
        return [[
            "id": "ndi",
            "label": "NDI",
            "ndiName": normalized(s.programName, "CoreVideo Pro Program"),
            "ndiGroup": normalized(s.groupName, "public"),
            "fps": streamFps,
        ]]
    }
}
