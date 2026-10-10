// Main-actor owned command intent. Observed media status remains independent.
struct RecordingCommandPolicy {
    struct Operation {
        let token: UInt64
        let stop: Bool
        let previousDesired: Bool
    }

    private(set) var desired = false
    private var status = "idle"
    private var generation: UInt64 = 0
    private var pending: UInt64?
    private var awaitingStartProgress = false

    private var observedLive: Bool { status == "recording" || status == "warning" }

    mutating func observe(_ status: String) {
        self.status = status
        if status == "starting" || observedLive { awaitingStartProgress = false }
        guard pending == nil else { return }
        // A poll issued before Start can finish after its acknowledgement.
        // Idle/completed from that poll cannot revoke the newer Start intent.
        if awaitingStartProgress && (status == "idle" || status == "completed") { return }
        reconcile(fallback: desired)
    }

    mutating func begin() -> Operation {
        generation &+= 1
        let operation = Operation(token: generation, stop: observedLive || desired,
                                  previousDesired: desired)
        pending = generation
        desired = !operation.stop
        awaitingStartProgress = !operation.stop
        return operation
    }

    // Returns false for a completion superseded by a later command or exit.
    @discardableResult
    mutating func finish(_ operation: Operation, failed: Bool) -> Bool {
        guard pending == operation.token else { return false }
        pending = nil
        if failed {
            awaitingStartProgress = false
            // Stop remains a safety intent even when its acknowledgement is lost.
            reconcile(fallback: operation.stop ? false : (operation.previousDesired || observedLive))
        }
        return true
    }

    mutating func interrupted() {
        generation &+= 1
        pending = nil
        status = "interrupted"
        desired = false
        awaitingStartProgress = false
    }

    private mutating func reconcile(fallback: Bool) {
        // Live polls cannot re-arm a stopped intent. begin() independently uses
        // observedLive to offer Stop retries while media is still being written.
        switch status {
        case "idle", "completed", "failed", "interrupted", "stopping", "finalizing": desired = false
        default: desired = fallback
        }
    }
}

// ── ISO capture preflight (E11/E16) ──────────────────────────────────────

enum IsoCapturePreflight {
    /// Warn iff `zoomIsoCount > 0` AND (`captureIntended == false` OR `rawMediaActive == false`).
    /// `rawMediaActive == nil` with intent ON is unobserved → no warning (E11's round-2 ruling).
    /// Intent OFF is itself a fact — the shell owns the toggle — so it warns regardless of observation.
    /// The intent-ON/observed-OFF branch gets its OWN tail: Capture is actually on, so telling the
    /// operator to "turn Capture on" would be asking them to flip a switch that already reads on —
    /// the real instruction there is to re-arm it.
    static func warning(zoomIsoCount: Int, captureIntended: Bool, rawMediaActive: Bool?) -> String? {
        guard zoomIsoCount > 0 else { return nil }
        let plural = zoomIsoCount == 1 ? "source" : "sources"
        let lead = "\(zoomIsoCount) Zoom ISO \(plural) armed with Capture off"
        // Intent OFF: warn regardless of observation (shell owns the toggle)
        if !captureIntended {
            return "\(lead) — their ISO files will not start until frames flow. Turn Capture on, then record."
        }
        // Intent ON: only warn if observed OFF (not if nil = unobserved). Capture
        // IS on here, so the fix is re-arming it, not turning it on.
        if rawMediaActive == false {
            return "\(zoomIsoCount) Zoom ISO \(plural) armed, but Capture is on and the engine reports raw " +
                "media inactive — re-arm Capture, then record."
        }
        return nil
    }
}
