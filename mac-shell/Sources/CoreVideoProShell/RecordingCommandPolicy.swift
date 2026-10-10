// ── ISO capture preflight (E11/E16) ──────────────────────────────────────

enum IsoCapturePreflight {
    /// Warn iff `zoomIsoCount > 0` AND (`captureIntended == false` OR `rawMediaActive == false`).
    /// `rawMediaActive == nil` with intent ON is unobserved → no warning (E11's round-2 ruling).
    /// Intent OFF is itself a fact — the shell owns the toggle — so it warns regardless of observation.
    static func warning(zoomIsoCount: Int, captureIntended: Bool, rawMediaActive: Bool?) -> String? {
        guard zoomIsoCount > 0 else { return nil }
        // Intent OFF: warn regardless of observation (shell owns the toggle)
        if !captureIntended {
            let plural = zoomIsoCount == 1 ? "source" : "sources"
            return "\(zoomIsoCount) Zoom ISO \(plural) armed with Capture off — their ISO files will not start until frames flow. Turn Capture on, then record."
        }
        // Intent ON: only warn if observed OFF (not if nil = unobserved)
        if rawMediaActive == false {
            let plural = zoomIsoCount == 1 ? "source" : "sources"
            return "\(zoomIsoCount) Zoom ISO \(plural) armed with Capture off — their ISO files will not start until frames flow. Turn Capture on, then record."
        }
        return nil
    }
}

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
