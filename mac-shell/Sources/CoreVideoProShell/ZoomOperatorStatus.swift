// ZoomOperatorStatus — the Zoom tab's "Zoom status" card reflects whether the
// Zoom SDK can actually join (Tier 0.15; Windows SettingsPage.xaml:148-167,
// SettingsViewModel.cs:265-299 FormatZoomOperatorStatus / Guidance).
//
// The Mac card said "Ready to join Zoom" regardless; a missing engine or a
// failed SDK (bad credentials end in meetingState "error", which the core
// reports as readiness "blocked") was visible only on the Health tab.

import Foundation

enum ZoomOperatorStatus {
    enum Readiness: Equatable { case ready, warning, blocked }

    /// The core's own verdict (snapshot zoom.readiness, CoreStateCapture
    /// zoomReadinessState): status "blocked" — no engine, or an engine whose
    /// meeting state is "error". An absent node (no snapshot yet) is not
    /// evidence of a block.
    static func readiness(snapshot: JSONObject) -> Readiness {
        guard let zoom = snapshot["zoom"] as? JSONObject,
              let readiness = zoom["readiness"] as? JSONObject else { return .ready }
        switch readiness["status"] as? String ?? "" {
        case "blocked": return .blocked
        case "warning": return .warning
        default: return .ready
        }
    }

    /// The first blocked check's label (e.g. "COREVIDEO_ZOOM_ENGINE_PATH is not
    /// set: no Zoom engine, so Join is refused"), or nil.
    static func blockingReason(snapshot: JSONObject) -> String? {
        guard let zoom = snapshot["zoom"] as? JSONObject,
              let readiness = zoom["readiness"] as? JSONObject,
              let checks = readiness["checks"] as? [JSONObject] else { return nil }
        return checks.first { $0["status"] as? String == "blocked" }?["label"] as? String
    }

    static func isBlocked(_ readiness: Readiness, inMeeting: Bool) -> Bool {
        !inMeeting && readiness == .blocked
    }

    /// Windows FormatZoomOperatorStatus, verbatim.
    static func chipLabel(_ readiness: Readiness, inMeeting: Bool) -> String {
        if inMeeting { return "Connected to Zoom" }
        return readiness == .blocked ? "Zoom is unavailable" : "Ready to join Zoom"
    }

    /// Windows FormatZoomOperatorGuidance; the Mac's diagnostics are the
    /// Health tab, not a section inside Settings.
    static func guidance(_ readiness: Readiness, inMeeting: Bool) -> String {
        if inMeeting { return "Zoom is connected and working." }
        return readiness == .blocked
            ? "CoreVideo cannot join Zoom yet. Open Health for details."
            : "Enter a meeting link or ID above when you are ready to connect."
    }
}
