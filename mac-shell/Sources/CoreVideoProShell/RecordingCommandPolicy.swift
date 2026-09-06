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
            // "starting" has no observed media proof. A failed safety Stop
            // must not re-arm that pending start through another output command.
            reconcile(fallback: operation.stop ? false : operation.previousDesired)
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
        switch status {
        case "recording", "warning": desired = true
        case "idle", "completed", "failed", "interrupted", "stopping", "finalizing": desired = false
        default: desired = fallback
        }
    }
}
