// ZoomJoinStatus — what the Zoom pane says about a join, decided in one pure
// place so the rules are tests rather than live meetings.
//
// Mac 1.0.0, 2026-11-15: a join logged "Timed out waiting for Zoom meeting join
// result." while the engine reported, a moment later, meeting_failed 504
// MEETING_FAIL_APP_CAN_NOT_ANONYMOUS_JOIN_MEETING (a guest join of a meeting
// that requires sign-in). The core now replaces the timeout with that verdict
// when it arrives, phrased by its single mapping (native
// modules/ZoomJoinFailureMessage.h) — the shell does NOT re-map reasons, it
// only decides which warning is the join's answer and keeps showing it.

import Foundation

enum ZoomJoinStatus {
    /// Same as the Windows shell (MediaCorePaths.cs): the core waits up to this
    /// for SDK auth, then up to this again for the join verdict.
    static let coreJoinWaitMs = "45000"
    /// The bridge must outlast both core waits (auth + join) plus slack, or the
    /// shell gives up on a join the core is still honestly waiting on.
    static let joinRequestTimeout: TimeInterval = 100

    static let signInMayBeRequired =
        "Joining as a guest. Zoom may require you to sign in for this meeting — "
        + "if it does, use Sign in with Zoom and join again."

    static func isTimeout(_ warning: String) -> Bool {
        warning.hasPrefix("Timed out waiting for Zoom ")
    }

    /// The line to show as a join starts.
    static func atJoin(signedIn: Bool) -> String {
        signedIn ? "" : signInMayBeRequired
    }

    /// The join's answer in a failed snapshot: Zoom's own reason beats a
    /// timeout; nil when the snapshot is not a failure.
    static func failure(from snapshot: JSONObject) -> String? {
        guard snapshot["meetingState"] as? String == "error" else { return nil }
        let warnings = (snapshot["warnings"] as? [String] ?? []).filter { !$0.isEmpty }
        if let reason = warnings.last(where: { !isTimeout($0) }) { return reason }
        return warnings.last ?? "Zoom did not say why the join failed."
    }

    /// Next pane line from the current one and a fresh snapshot. A failure
    /// shows its reason (so a later snapshot carrying Zoom's verdict replaces
    /// the timeout text); a joined meeting clears it; idle/joining keep what
    /// is showing, so the reason survives until the next join.
    static func reconcile(current: String, snapshot: JSONObject) -> String {
        if let failure = failure(from: snapshot) { return failure }
        switch snapshot["meetingState"] as? String {
        case "in_meeting", "in-meeting": return ""
        default: return current
        }
    }
}
