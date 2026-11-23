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

// ── AppModel wiring ─────────────────────────────────────────────────────────

extension AppModel {
    /// NDI goes out at the stream fps (Windows sends the stream profile's fps).
    /// The Mac stream rows pin 30 until Task 7 adds the stream fps picker.
    var ndiStreamFps: Int { 30 }

    /// NDI is in the output set (on, with a name that validates).
    var ndiOutputActive: Bool {
        !NdiOutputPolicy.destinationSettings(ndiOutput, streamFps: ndiStreamFps).isEmpty
    }

    /// The footer tile / Settings card status line.
    var ndiOutputStatus: String {
        NdiOutputPolicy.status(enabled: ndiOutput.enabled, senders: ndiSenders,
                               programName: ndiOutput.programName, refusalReason: ndiRefusalReason)
    }

    /// The NDI toggle (UI, and the control lane's transport.ndi.set/toggle).
    /// Only turning ON is validated; a refusal changes nothing but the reason
    /// and is returned. Off always succeeds; the same value is idempotent.
    /// Never touches Stream or Record (Windows StudioViewModel.SetNdiOutput).
    @discardableResult
    func setNdiOutput(_ on: Bool) -> String? {
        if on, let refusal = NdiOutputPolicy.validate(programName: ndiOutput.programName,
                                                       coreHasNdiOutput: coreHasNdiOutput) {
            ShellLog.write("ndi: on refused (\(refusal))")
            ndiRefusalReason = refusal
            return refusal
        }
        if on { ndiRefusalReason = nil }
        guard ndiOutput.enabled != on else { return nil }
        ndiOutput.enabled = on
        persistPrefsNow()
        sendNdiOutputSync()
        return nil
    }

    /// Program name / group committed (Return or focus loss, Windows
    /// OnNdiTextBoxKeyDown). While NDI is on, a name that no longer validates
    /// turns NDI off with the reason; otherwise the live source is updated.
    func commitNdiSettings(programName: String, groupName: String) {
        guard programName != ndiOutput.programName || groupName != ndiOutput.groupName else { return }
        ndiOutput.programName = programName
        ndiOutput.groupName = groupName
        // Windows OnNdiOutputOptionChanged: save, and with NDI off that is all.
        // An edit while off must never touch the outputs (it would stop an idle
        // session, or re-send a live stream's full state for nothing).
        guard ndiOutput.enabled else {
            persistPrefsNow()
            return
        }
        if let reason = NdiOutputPolicy.validate(programName: programName, coreHasNdiOutput: coreHasNdiOutput) {
            ShellLog.write("ndi: turned off (\(reason))")
            ndiRefusalReason = reason
            ndiOutput.enabled = false
        }
        persistPrefsNow()
        sendNdiOutputSync()
    }

    /// One full-state output sync for what should be live now. With nothing left
    /// (NDI off, not recording, not streaming) the session is stopped.
    func sendNdiOutputSync() {
        guard bridge != nil || quitRequestForTesting != nil else { return }
        let nothingLeft = !ndiOutputActive && !recordingDesired && !streamingDesired
        let command: JSONObject = nothingLeft
            ? ["type": "stop-encoder-session", "reason": "NDI output turned off."]
            : startProgramOutput(recording: recordingDesired, streaming: streamingDesired)
        Task {
            do {
                _ = try await self.outputRequest([
                    "type": "media-core-sync", "elapsedMs": self.elapsedMs(), "commands": [command],
                ])
            } catch {
                // Loud, never a silent "Starting" forever. NDI stays on, as on
                // Windows; the next core generation re-arms it.
                ShellLog.write("ndi: output update failed (\(error.localizedDescription))")
                self.pushWarning("NDI output update failed: \(error.localizedDescription)")
            }
        }
    }

    /// Every core generation: a persisted-on NDI output is started again.
    func rearmNdiOutput() {
        guard ndiOutputActive else { return }
        sendNdiOutputSync()
    }

    func applyNdiSenders(_ session: JSONObject) {
        let rows = (session["senders"] as? [JSONObject] ?? []).filter { $0["destination"] as? String == "ndi" }
        if !NdiOutputPolicy.sameRows(rows, ndiSenders) { ndiSenders = rows }
    }

    func applyNdiCapability(_ snapshot: JSONObject) {
        guard let states = (snapshot["profile"] as? JSONObject)?["capabilityStates"] as? JSONObject else { return }
        let has = (states["ndi-output"] as? JSONObject)?["state"] as? String == "available"
        if has != coreHasNdiOutput { coreHasNdiOutput = has }
    }
}

extension NdiOutputPolicy {
    /// Change gate for @Published sender rows (snapshots arrive several times a
    /// second): only the fields the status line reads.
    static func sameRows(_ a: [JSONObject], _ b: [JSONObject]) -> Bool {
        let keys = ["destination", "status", "warning", "lastError", "runtimeDetail", "lastResultCode"]
        func project(_ rows: [JSONObject]) -> [[String]] { rows.map { row in keys.map { row[$0] as? String ?? "" } } }
        return project(a) == project(b)
    }
}
