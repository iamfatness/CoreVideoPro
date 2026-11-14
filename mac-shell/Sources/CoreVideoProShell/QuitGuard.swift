// QuitGuard — quitting while recording or streaming asks first and finishes the
// files before the media core goes away (T1.8 parity with the WinUI close
// guard, #461: CloseGuardPolicy / OutputShutdownCoordinator, CLAUDE.md "Engine
// teardown order" / docs/reference/engine-teardown-order.md).
//
// The pieces here are pure so ShellTests can drive them without a window or a
// core: the decision, the single-prompt gate, and the bounded finish wait. The
// AppModel wires them to the real stop commands and the real snapshots.

import Foundation

struct QuitOutputs: Equatable {
    var recordingStatus = "idle"     // RecordingLifecycleReadModel.status
    var recordingDesired = false
    var streamingDesired = false
    var streamStatus = ""            // RTMP sender status, "" when absent
}

enum QuitGuardPolicy {
    /// Windows OutputShutdownCoordinator bound: how long Stop-and-quit waits for
    /// the terminal lifecycle before it gives up (loudly) and quits anyway.
    static let finishTimeout: TimeInterval = 15
    /// Windows ShutdownBudget: after stdin closes the core gets this long to
    /// exit on its own (it finalizes on EOF) before it is terminated.
    static let coreExitGrace: TimeInterval = 2

    /// Lifecycle states in which media may still be written. `stopping` and
    /// `finalizing` COUNT as live (Windows ruling), and an unreadable lifecycle
    /// ("unknown") asks rather than guesses.
    private static let liveRecording: Set<String> = [
        "requested", "preparing", "producing", "starting", "live", "recording",
        "warning", "stopping", "finalizing", "unknown",
    ]
    private static let liveStream: Set<String> = [
        "starting", "live", "warning", "stopping", "connecting", "streaming",
    ]

    static func recordingLive(_ o: QuitOutputs) -> Bool {
        o.recordingDesired || liveRecording.contains(o.recordingStatus)
    }

    static func streamLive(_ o: QuitOutputs) -> Bool {
        o.streamingDesired || liveStream.contains(o.streamStatus)
    }

    /// Recording or streaming live asks; anything else (idle, completed,
    /// failed, interrupted, virtual camera alone) quits at once.
    static func shouldAsk(_ o: QuitOutputs) -> Bool { recordingLive(o) || streamLive(o) }

    /// Terminal recording evidence. `idle` is NOT terminal: a core that never
    /// saw the stop also reads idle, and only a completed/failed writer has
    /// closed the file.
    static func recordingTerminal(_ status: String) -> Bool {
        status == "completed" || status == "failed" || status == "interrupted"
    }
}

/// One prompt at a time. A second quit request while the dialog is open or while
/// outputs finish is ignored (never a force-exit) — the Windows rule.
struct QuitGate {
    enum Phase: Equatable { case idle, asking, finishing }
    enum Decision: Equatable { case quitNow, ask, ignore }

    private(set) var phase: Phase = .idle

    mutating func request(_ outputs: QuitOutputs) -> Decision {
        guard phase == .idle else { return .ignore }
        guard QuitGuardPolicy.shouldAsk(outputs) else { return .quitNow }
        phase = .asking
        return .ask
    }

    /// "Stop and quit" — only meaningful while the dialog is open.
    mutating func confirm() -> Bool {
        guard phase == .asking else { return false }
        phase = .finishing
        return true
    }

    /// "Keep running".
    mutating func cancel() {
        if phase == .asking { phase = .idle }
    }

    mutating func reset() { phase = .idle }
}

enum QuitFinishOutcome: Equatable { case finished, timedOut }

enum QuitFlow {
    /// Runs `op` but gives up after `seconds`; nil means the deadline won. The
    /// operation is not cancellable (a bridge request cannot be), so it is left to
    /// finish on its own and its late result is dropped.
    static func race<T: Sendable>(_ seconds: TimeInterval,
                                  _ op: @escaping @Sendable () async -> T) async -> T? {
        await withCheckedContinuation { (continuation: CheckedContinuation<T?, Never>) in
            let once = OnceBox()
            Task {
                let value = await op()
                if once.claim() { continuation.resume(returning: value) }
            }
            Task {
                try? await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
                if once.claim() { continuation.resume(returning: nil) }
            }
        }
    }

    private final class OnceBox: @unchecked Sendable {
        private let lock = NSLock()
        private var taken = false
        func claim() -> Bool {
            lock.lock(); defer { lock.unlock() }
            if taken { return false }
            taken = true
            return true
        }
    }

    /// Sends the stops, then waits for `isFinished` (fresh terminal evidence).
    /// ONE deadline covers the whole flow: every stop and every evidence check is
    /// raced against the time that remains, and each is told that remainder so it
    /// can bound its own request. A stop that did not land is re-sent once a
    /// second while still unfinished (Windows: re-sent while intent is set).
    static func finishOutputs(
        stop: @escaping @Sendable (_ remaining: TimeInterval) async -> Bool,
        isFinished: @escaping @Sendable (_ remaining: TimeInterval) async -> Bool,
        timeout: TimeInterval = QuitGuardPolicy.finishTimeout,
        poll: TimeInterval = 0.1,
        resendEvery: TimeInterval = 1.0
    ) async -> QuitFinishOutcome {
        let started = Date()
        func remaining() -> TimeInterval { timeout - Date().timeIntervalSince(started) }
        var stopLanded = false
        var lastSend = Date.distantPast
        while remaining() > 0 {
            if !stopLanded, Date().timeIntervalSince(lastSend) >= resendEvery {
                let budget = remaining()
                lastSend = Date()
                stopLanded = await race(budget) { await stop(budget) } ?? false
            } else if stopLanded {
                let budget = remaining()
                if budget > 0, await race(budget, { await isFinished(budget) }) == true {
                    return .finished
                }
            }
            if remaining() <= 0 { break }
            try? await Task.sleep(nanoseconds: UInt64(min(poll, max(0, remaining())) * 1_000_000_000))
        }
        return .timedOut
    }
}
