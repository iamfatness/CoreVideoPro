// GuardedLayoutTake — the guarded Shot Layout Take (G4-G7 Task 15). Ports of
// ITransportHost.CreateGuardedTake (StudioViewModel.Transport.cs:324-334), the graphicsLayout wire
// (MediaCoreCommandBuilder.BuildTakeTransitionCommand :141-152) and TransportCoordinator.Layouts.cs
// (ResolveGuardedTakeAsync / FinishGuardedTakeAsync). AppModel.take() drives it.
//
// HOW WINDOWS KEEPS ORDINARY SCENE SYNCS OUT WHILE A GUARDED TAKE IS PENDING (read 2026-11-25,
// StudioViewModel.SyncActiveSceneAsync :9062-9070 and its callers). The core's own rule is in
// GuardedLayoutTake.cpp handleGuardedLayoutBatch: while `pendingLayoutTake_` is set, ANY batch that
// carries a load-scene-graph or a begin-take-transition without the pending guard is swallowed whole
// and recorded as the protocol failure "guarded-layout-take-pending" (a batch with neither passes).
// Windows never sends one: SyncActiveSceneAsync, the single funnel every scene/output sync goes
// through, throws MediaCoreSyncInFlightException at capture time when
// `_transportCoordinator.GuardedTakePending` and the call is not the guarded Take itself, so nothing
// reaches the bridge. Callers treat that exception as backpressure: the coalesced production sync and
// the multiviewer resend re-arm and retry, the record/stream paths retry their bounded loop, and a
// few fire-and-forget callers (e.g. the NDI settings sync) only log it. So Windows HOLDS the sync, it
// does not send it and tolerate the core's refusal. The Mac keeps plan D10 in the same spirit and a
// little stricter: pushScenes() returns early while a guarded Take is pending and records that one
// sync is owed, and resolving the Take (committed, refused, or failed) runs exactly that one sync —
// no caller has to own a retry. The spine/output ticks carry no load-scene-graph, so they pass.

import Foundation

/// GraphicsLayoutTakeRequest: the guard the Take transition carries (operationId = the attempt id).
struct GraphicsLayoutTakeRequest: Equatable {
    let operationId: String
    let coreEpoch: String
    let sceneId: String
    let previewRevision: Int64
    let keepProgramScene: Bool
    let playOutOthers: Bool
    let cues: [GraphicsLayoutCue]

    /// The `graphicsLayout` object of begin-take-transition.
    var wire: JSONObject {
        ["coreEpoch": coreEpoch, "sceneId": sceneId, "previewRevision": previewRevision, "keepProgramScene": keepProgramScene,
         "playOutOthers": playOutOthers, "cues": cues.map { ["instanceId": $0.instanceId, "cueRevision": $0.cueRevision] as JSONObject }]
    }
}

/// TransportCoordinator.PendingLayoutTake: the request, the Program it replaced, the Take's rollback
/// and whether the next round must (re)send the batch.
struct PendingGuardedTake {
    let request: GraphicsLayoutTakeRequest
    let transition: JSONObject
    let previousProgramSceneId: String
    let rollback: () -> Bool
    var submitNeeded = true
}

extension GraphicsModel {
    /// ITransportHost.CreateGuardedTake: nil when the prepared layout is not current, or (a layout with a
    /// scene) the core's Preview is not that scene at a real revision.
    func createGuardedTake(snapshot: JSONObject, activeSceneId: String) -> GraphicsLayoutTakeRequest? {
        guard let prepared = playout.preparedLayout,
              playout.isPreparedLayoutCurrent(prepared, previewStamp: layoutPreviewStamp()) else { return nil }
        let preview = snapshot["previewScene"] as? JSONObject
        let previewSceneId = preview?["sceneId"] as? String
        let revision = GraphicsDataBinding.int64(preview?["revision"]) ?? 0
        let keepProgram = prepared.layout.sceneId.isEmpty
        if !keepProgram && (previewSceneId != prepared.layout.sceneId || revision <= 0) { return nil }
        return GraphicsLayoutTakeRequest(operationId: prepared.attemptId, coreEpoch: prepared.coreEpoch,
                                         sceneId: keepProgram ? activeSceneId : prepared.layout.sceneId, previewRevision: revision,
                                         keepProgramScene: keepProgram, playOutOthers: prepared.layout.playOutOthers, cues: prepared.cues)
    }

    /// ITransportHost.FinishGuardedTakeAsync: a committed Take confirms the prepared layout; anything else
    /// cancels it (releasing the attempt's own cues).
    func finishGuardedTake(operationId: String, committed: Bool) async {
        if committed { playout.confirmLayoutTake(attemptId: operationId) }
        else if playout.preparedLayout?.attemptId == operationId { report(await playout.cancelLayout()) }
    }
}

extension AppModel {
    static let guardedTakeRounds = 16
    static let guardedTakeRoundDelayNs: UInt64 = 125_000_000

    /// ResolveGuardedTakeAsync: up to 16 rounds, each sending the batch (first round, or after a `ready`
    /// receipt: the same transition, a fresh batch) or polling, then reading the operation's receipt.
    func resolveGuardedTake() async -> TakeResult {
        guard var pending = pendingGuardedTake else { return .failed("No Shot Layout Take is pending.") }
        let request = pending.request
        var latest: JSONObject?
        for round in 0..<Self.guardedTakeRounds {
            let sending = pending.submitNeeded
            pending.submitNeeded = false
            pendingGuardedTake = pending
            var observation: JSONObject?
            do {
                let commands = sending ? guardedSceneSyncBatch(transition: pending.transition) : []
                observation = try await outputRequest(["type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": commands])["snapshot"] as? JSONObject
            } catch {
                // The request may have been committed. Read the operation receipt before doing anything else.
            }
            latest = observation ?? latest
            if let graphics = observation?["graphics"] as? JSONObject {
                if graphics["coreEpoch"] as? String != request.coreEpoch {
                    return await finishGuardedTake(pending, refusal: "Core restarted. Cue the Shot Layout again.")
                }
                let receipt = (graphics["layoutTakeReceipts"] as? [JSONObject] ?? []).first { $0["operationId"] as? String == request.operationId }
                switch receipt?["disposition"] as? String {
                case "committed": return await finishGuardedTake(pending, refusal: nil)
                case "refused": return await finishGuardedTake(pending, refusal: "Shot Layout Take refused. \(receipt?["reason"] as? String ?? "")")
                case let disposition:
                    // A cold source is prepared by the render tick; the core retained the original batch.
                    pending.submitNeeded = disposition == "ready"
                    pendingGuardedTake = pending
                }
            }
            if round < Self.guardedTakeRounds - 1 { await timerSleep(nanoseconds: Self.guardedTakeRoundDelayNs) }
        }
        if let latest, let graphics = latest["graphics"] as? JSONObject, graphics["coreEpoch"] as? String == request.coreEpoch,
           !(graphics["layoutTakeReceipts"] as? [JSONObject] ?? []).contains(where: { $0["operationId"] as? String == request.operationId }) {
            return await finishGuardedTake(pending, refusal: "The core could not confirm the Take. Cue the Shot Layout again.",
                                           restore: latest["sceneId"] as? String == pending.previousProgramSceneId)
        }
        let unconfirmed = "Shot Layout Take is awaiting confirmation. Retry Take to check the same operation."
        takeStatus = unconfirmed
        return .failed(unconfirmed)
    }

    /// FinishGuardedTakeAsync, then the one scene sync held while the Take was pending (D10).
    private func finishGuardedTake(_ pending: PendingGuardedTake, refusal: String?, restore: Bool = true) async -> TakeResult {
        if refusal != nil && restore { _ = pending.rollback() }
        pendingGuardedTake = nil
        await graphics.finishGuardedTake(operationId: pending.request.operationId, committed: refusal == nil)
        runOwedSceneSync()
        guard let refusal else {
            takeStatus = "Program updated"
            return .success
        }
        takeStatus = refusal
        return .failed(refusal)
    }
}
