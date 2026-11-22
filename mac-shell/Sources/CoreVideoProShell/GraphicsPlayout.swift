// GraphicsPlayout — playlist playout over native graphics instances (graphics
// G1-G3). Port of MC/Services/GraphicsPlayoutService.cs.
//
// G1 (this file's first slice) carries the plumbing and the strip's safety
// actions: Hide (:309-316), Clear cue (:318-324), Clear all (:178-185) and Clear
// cued (:186-194). Every operation runs one at a time through `run` (:359-370)
// and every command goes out through `send` (:374-379), which stamps the
// identity (schemaVersion 1, clientId graphics-playout, a fresh requestId and
// the epoch the operation STARTED on). A core whose epoch moved mid-operation
// refuses the rest of the sequence: "no automatic replay" (:372-373).
//
// ON-AIR TRUTH IS ONLY THE CORE'S INSTANCES. A command's acceptance never puts
// a chip on the strip; the next snapshot does.

import Foundation

struct GraphicsPlayoutFailure: Error { let message: String }

@MainActor
final class GraphicsPlayout {
    static let clientId = "graphics-playout"
    static let unavailableMessage = "Native Graphics is unavailable."
    static let coreChangedMessage = "Native core changed. Graphics playout cancelled; no automatic replay."

    /// The one gateway call (GraphicsGateway.invoke).
    let invoke: (String, JSONObject) async -> GraphicsOperationResult
    /// The latest graphics node.
    let observe: () -> GraphicsState?
    /// Injected sleep (nanoseconds) for the cue wait and auto-out (plan D10).
    var sleep: (UInt64) async -> Void = { try? await Task.sleep(nanoseconds: $0) }
    var newRequestId: () -> String = { "playout-" + UUID().uuidString.lowercased().replacingOccurrences(of: "-", with: "") }
    /// Called whenever busy or status changes.
    var onChange: () -> Void = {}

    private(set) var pending = 0
    var busy: Bool { pending > 0 }
    private(set) var status = ""
    /// The epoch the running operation started on ("" between operations).
    private(set) var epoch = ""
    private var gateHeld = false
    private var gateWaiters: [CheckedContinuation<Void, Never>] = []

    init(invoke: @escaping (String, JSONObject) async -> GraphicsOperationResult, observe: @escaping () -> GraphicsState?) {
        self.invoke = invoke
        self.observe = observe
    }

    // ── strip actions (G1) ────────────────────────────────────────────────

    func hide(_ instanceId: String) async -> GraphicsOperationResult { await run { try await self.hideCore(instanceId) } }
    func clearCue(_ instanceId: String) async -> GraphicsOperationResult { await run { try await self.uncueCore(instanceId) } }

    /// Hide every instance on Program or still show-pending (:178-185).
    func clearAll() async -> GraphicsOperationResult {
        await run {
            let state = try self.available()
            var result = GraphicsOperationResult(accepted: true, reason: "Nothing is on Program.")
            for instance in state.instances where instance.isOnProgram || instance.showPending {
                result = try await self.hideCore(instance.instanceId)
                if !result.accepted { return result }
            }
            // Windows reports "All graphics hidden." for any accepted run, even an empty one (:184).
            if result.accepted { result.reason = "All graphics hidden." }
            return result
        }
    }

    /// Uncue every cued instance except a social card (:186-194).
    func clearCued() async -> GraphicsOperationResult {
        await run {
            let state = try self.available()
            var result = GraphicsOperationResult(accepted: true, reason: "Preview is already clear.")
            for instance in state.instances where instance.isOnPreview && !instance.instanceId.hasPrefix(GraphicsStrip.socialPrefix) {
                result = try await self.uncueCore(instance.instanceId)
                if !result.accepted { return result }
            }
            if result.accepted { result.reason = "Preview cleared." }   // as Windows (:193)
            return result
        }
    }

    // ── cores (callers already hold the gate) ────────────────────────────────

    func hideCore(_ instanceId: String) async throws -> GraphicsOperationResult {
        cancelAutoOut(instanceId)
        let state = try available()
        epoch = state.coreEpoch
        guard let instance = state.instance(instanceId) else { return refuse("That graphic no longer exists.") }
        guard instance.programRevision > 0 || instance.showPending else { return refuse("That graphic is not on Program.") }
        return try await send("graphics.instance.trigger",
                              ["instanceId": instanceId, "expectedRevision": instance.revision, "action": "hide"])
    }

    func uncueCore(_ instanceId: String) async throws -> GraphicsOperationResult {
        let state = try available()
        epoch = state.coreEpoch
        guard let instance = state.instance(instanceId) else { return refuse("That graphic no longer exists.") }
        guard instance.isOnPreview else { return refuse("That graphic is not on Preview.") }
        return try await send("graphics.instance.trigger",
                              ["instanceId": instanceId, "expectedRevision": instance.revision, "action": "uncue"])
    }

    // ── auto-out hooks (filled in by the playlist slice) ─────────────────────

    func cancelAutoOut(_ instanceId: String) {}
    func cancelAllAutoOut() {}

    /// A new core epoch: drop every local timer (:126). In-flight sequences
    /// refuse on their own at the next `current(epoch)` check.
    func epochChanged() { cancelAllAutoOut() }

    // ── plumbing ─────────────────────────────────────────────────────────────

    func run(_ action: @escaping () async throws -> GraphicsOperationResult) async -> GraphicsOperationResult {
        pending += 1
        onChange()
        await acquire()
        let result: GraphicsOperationResult
        do { result = try await action() }
        catch let failure as GraphicsPlayoutFailure { result = refuse(failure.message) }
        catch is CancellationError { result = refuse("Graphics playout cancelled. Retry explicitly when ready.") }
        catch { result = refuse(error.localizedDescription) }
        epoch = ""
        release()
        pending -= 1
        status = result.reason
        onChange()
        return result
    }

    func available() throws -> GraphicsState {
        guard let state = observe(), !state.coreEpoch.isEmpty else { throw GraphicsPlayoutFailure(message: Self.unavailableMessage) }
        return state
    }

    func current(_ epoch: String) throws -> GraphicsState {
        guard let state = observe(), state.coreEpoch == epoch else { throw GraphicsPlayoutFailure(message: Self.coreChangedMessage) }
        return state
    }

    func send(_ action: String, _ body: JSONObject) async throws -> GraphicsOperationResult {
        let epoch = self.epoch.isEmpty ? try available().coreEpoch : self.epoch
        _ = try current(epoch)
        var body = body
        body["schemaVersion"] = 1
        body["clientId"] = Self.clientId
        if body["requestId"] == nil { body["requestId"] = newRequestId() }
        body["coreEpoch"] = epoch
        return await invoke(action, body)
    }

    func refuse(_ reason: String) -> GraphicsOperationResult { GraphicsOperationResult(accepted: false, reason: reason) }

    private func acquire() async {
        if !gateHeld { gateHeld = true; return }
        await withCheckedContinuation { gateWaiters.append($0) }
    }

    private func release() {
        if gateWaiters.isEmpty { gateHeld = false } else { gateWaiters.removeFirst().resume() }
    }
}
