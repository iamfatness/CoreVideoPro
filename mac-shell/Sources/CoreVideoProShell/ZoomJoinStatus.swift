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
    /// The core's helper-connect budget (ZoomEngineRuntime::Config::connectTimeoutMs).
    static let coreConnectTimeoutMs = 30_000

    /// The bridge must outlast the core's WORST honest join, or the shell says
    /// "join failed: timed out" while the core is still waiting. Every rejoin
    /// after a failure/timeout restarts the helper, so the worst case is
    /// connect (30 s) + auth wait (45 s) + join wait (45 s) = 120 s, plus 30 s
    /// for the old helper's terminate and pipe/scheduling slack = 150 s. Read
    /// from the same environment the core gets, so an override moves both.
    static func joinRequestTimeout(environment: [String: String]) -> TimeInterval {
        let wait = environment["COREVIDEO_ZOOM_JOIN_WAIT_MS"].flatMap(Int.init) ?? Int(coreJoinWaitMs)!
        let connect = environment["COREVIDEO_ZOOM_ENGINE_CONNECT_TIMEOUT_MS"].flatMap(Int.init)
            ?? coreConnectTimeoutMs
        return TimeInterval(max(0, connect) + 2 * max(0, wait)) / 1000 + 30
    }

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

    /// #475 / Tier 0.13 (Windows ZoomJoinReconciliation.AccountBusyElsewhere):
    /// the one failure the takeover retry answers — this Zoom account is
    /// already in the meeting and the engine cancelled the SDK's prompt.
    /// Matched on the phrase the core's single mapping produces
    /// (modules/ZoomJoinFailureMessage.h), never on the wire reason. Narrow on
    /// purpose: offering the takeover after any other failure would invite an
    /// operator to evict their own Zoom client for nothing.
    static let accountBusyElsewhere = "already in this meeting"

    /// Windows CanEndOtherSessionAndRetry.
    static func canEndOtherSessionAndRetry(_ snapshot: JSONObject) -> Bool {
        snapshot["meetingState"] as? String == "error"
            && (snapshot["warnings"] as? [String] ?? []).contains { $0.contains(accountBusyElsewhere) }
    }

    /// The join's answer in a failed snapshot: Zoom's own reason beats a
    /// timeout; nil when the snapshot is not a failure.
    static func failure(from snapshot: JSONObject) -> String? {
        guard snapshot["meetingState"] as? String == "error" else { return nil }
        let warnings = (snapshot["warnings"] as? [String] ?? []).filter { !$0.isEmpty }
        // The actionable collision sentence wins (Windows `actionable`): it
        // names the button the operator has, whatever the SDK said after it.
        if let busy = warnings.last(where: { $0.contains(accountBusyElsewhere) }) { return busy }
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

/// The pane's join line over time. Fix round 1 (review I1): while a join is
/// in flight the POLLED snapshot still describes the previous attempt — the
/// core keeps meetingState "error" with the last warnings until the new
/// zoom-join reaches it (ensureJoinCredentials can spend a token refresh
/// first) — so the 504 -> sign in -> join again flow showed "Zoom requires
/// you to sign in…" for the whole new join. Only the join's own response
/// decides while it is in flight; after it, polls take over again (which is
/// how the core's late verdict still replaces a timeout).
struct ZoomJoinStatusTracker: Equatable {
    private(set) var line = ""
    private(set) var joinInFlight = false
    /// Windows CanEndOtherZoomSession: true only after a join failed because
    /// this account is already in the meeting; cleared at the start of EVERY
    /// join so it can never outlive the failure it answers.
    private(set) var canEndOtherSession = false

    mutating func beginJoin(signedIn: Bool) {
        joinInFlight = true
        canEndOtherSession = false
        line = ZoomJoinStatus.atJoin(signedIn: signedIn)
    }

    /// No ZAK came back: this join is a guest join after all.
    mutating func joiningAsGuest() {
        if joinInFlight { line = ZoomJoinStatus.signInMayBeRequired }
    }

    /// A polled zoom-snapshot.
    mutating func observe(_ snapshot: JSONObject) {
        guard !joinInFlight else { return }
        line = ZoomJoinStatus.reconcile(current: line, snapshot: snapshot)
        noteTakeoverOffer(snapshot)
    }

    /// The zoom-join reply (nil when the request itself failed).
    mutating func joinAnswered(_ snapshot: JSONObject?) {
        joinInFlight = false
        if let snapshot {
            line = ZoomJoinStatus.reconcile(current: line, snapshot: snapshot)
            noteTakeoverOffer(snapshot)
        }
    }

    /// A late core verdict can still be the collision (Windows
    /// ObserveLateJoinAsync); joining clears the offer.
    private mutating func noteTakeoverOffer(_ snapshot: JSONObject) {
        if ZoomJoinStatus.canEndOtherSessionAndRetry(snapshot) {
            canEndOtherSession = true
        } else if ["in_meeting", "in-meeting"].contains(snapshot["meetingState"] as? String ?? "") {
            canEndOtherSession = false
        }
    }

    /// Sign-in completed: the guest hint no longer applies.
    mutating func signedIn() {
        if line == ZoomJoinStatus.signInMayBeRequired { line = "" }
    }
}
