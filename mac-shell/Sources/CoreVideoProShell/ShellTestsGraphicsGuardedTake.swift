// G4-G7 Task 15 — the guarded Shot Layout Take in AppModel.take(): the batch carries the prepared
// cues under the attempt id, the core's layoutTakeReceipts resolve it (committed / refused / warming
// -> ready -> replay of the SAME operation), and ordinary scene syncs are held while it is pending.
// Every wait runs on the injected timer (timerSleepForTesting), never the wall clock.

import Foundation

extension ShellTests {
    static var graphicsGuardedTakeCases: [(String, () -> Void)] {
        [
            ("graphics-guarded-take/batch-shape", testGuardedTakeBatchShape),
            ("graphics-guarded-take/committed-confirms", testCommittedReceiptConfirms),
            ("graphics-guarded-take/refused-rolls-back", testRefusedReceiptRollsBackScenes),
            ("graphics-guarded-take/warming-ready-replays-same-operation", testWarmingThenReadyReplaysTheSameOperation),
            ("graphics-guarded-take/epoch-change", testEpochChangeDuringResolution),
            ("graphics-guarded-take/no-receipt-after-16-rounds", testNoReceiptAfter16Rounds),
            ("graphics-guarded-take/keep-program-scene", testKeepProgramSceneDoesNotSwap),
            ("graphics-guarded-take/refused-while-preparing", testTakeRefusedWhilePreparing),
            ("graphics-guarded-take/holds-scene-sync", testGuardedTakeHoldsSceneSync),
            ("graphics-guarded-take/control-take-uses-the-guard", testControlTakeUsesTheGuard),
            ("graphics-guarded-take/stale-guard-refused", testStalePreparedLayoutRefusesTheTake),
        ]
    }

    /// A core for the guarded Take: graphics-* bodies go to a scripted graphics core; every
    /// media-core-sync is recorded and answered with a snapshot whose graphics node carries the next
    /// scripted receipt for the operation ("none" = no receipt, "epoch:<id>" = a new core, "throw" = a
    /// transport failure). The last script entry repeats.
    @MainActor
    final class GuardedTakeCore {
        var bodies: [JSONObject] = []
        var script: [String] = []
        var programSceneId = "s2"
        var operationId = ""
        let graphics = ScriptedGraphicsCore()

        func handle(_ body: JSONObject) throws -> JSONObject {
            bodies.append(body)
            let commands = body["commands"] as? [JSONObject] ?? []
            if let transition = commands.first(where: { $0["type"] as? String == "begin-take-transition" }) {
                operationId = transition["operationId"] as? String ?? ""
            }
            let step = script.count > 1 ? script.removeFirst() : script.first ?? "none"
            if step == "throw" { throw BridgeError.remote("media core did not answer") }
            var node = graphics.node
            if step.hasPrefix("epoch:") { node["coreEpoch"] = String(step.dropFirst(6)) }
            else if step != "none" {
                let parts = step.split(separator: ":", maxSplits: 1).map(String.init)
                node["layoutTakeReceipts"] = [["operationId": operationId, "disposition": parts[0], "reason": parts.count > 1 ? parts[1] : ""]]
            }
            return ["snapshot": ["sceneId": programSceneId, "graphics": node]]
        }

        var transitions: [JSONObject] {
            bodies.compactMap { ($0["commands"] as? [JSONObject])?.first { $0["type"] as? String == "begin-take-transition" } }
        }
    }

    static let guardedAttempt = "layout-0123456789abcdef0123456789abcdef"

    /// sceneEditModel (Program s2, Preview s1) with a prepared guarded layout for `sceneId` whose one cue
    /// (playlist-title @ 3) the graphics state shows owned by the attempt; the core's Preview is s1 @ 7.
    @MainActor
    static func guardedTakeApp(sceneId: String = "s1", script: [String]) -> (AppModel, GuardedTakeCore) {
        let model = sceneEditModel()
        let core = GuardedTakeCore()
        core.script = script
        core.graphics.node["instances"] = [[
            "instanceId": "playlist-title", "revision": 3, "cueRevision": 3, "cued": true, "cueOwner": guardedAttempt,
            "cuePreparation": "ready", "templateId": "centered-title", "templateVersion": 1, "scope": "global",
        ]]
        model.quitRequestForTesting = { body, _ in
            let first = (body["commands"] as? [JSONObject])?.first?["type"] as? String ?? ""
            if first.hasPrefix("graphics-") { return try await core.graphics.send(body) }
            return try await MainActor.run { try core.handle(body) }
        }
        model.status = .connected(renderer: "test")
        model.timerSleepForTesting = { _ in }
        spin(until: { model.graphics.documentLoaded }, limit: 3)
        model.graphics.saveDelayNs = 0
        model.graphics.observe(snapshot: ["graphics": core.graphics.node, "profile": ["capabilityStates": ["graphics-instances": ["state": "available"]]]])
        var layout = GraphicsShotLayout(json: [:])
        layout.id = "final"; layout.name = "Final"; layout.sceneId = sceneId; layout.rowIds = ["title"]
        layout.json["rowSettings"] = ["title": ["overrides": ["heading": "Final"]]]
        model.graphics.document.shotLayouts = [layout]
        model.lastSnapshot = ["sceneId": "s2", "previewScene": ["sceneId": "s1", "revision": 7]]
        model.graphics.playout.preparedLayout = GraphicsPreparedLayout(
            layout: layout, coreEpoch: "epoch-1", attemptId: guardedAttempt,
            cues: [GraphicsLayoutCue(instanceId: "playlist-title", cueRevision: 3)], previewStamp: model.layoutPreviewStamp())
        return (model, core)
    }

    @MainActor
    static func testGuardedTakeBatchShape() {
        let (model, core) = guardedTakeApp(script: ["committed"])
        let box = runTake(model)
        expectEqual(box.result, .success, "the guarded Take commits")
        guard let first = core.bodies.first, let commands = first["commands"] as? [JSONObject] else { expect(false, "a batch was sent"); return }
        expectEqual(commands.first?["type"] as? String, "begin-take-transition", "the transition leads")
        expectEqual(commands.first?["operationId"] as? String, guardedAttempt, "operationId = the attempt id")
        let guardNode = commands.first?["graphicsLayout"] as? JSONObject
        expectEqual(guardNode?["coreEpoch"] as? String, "epoch-1", "guard epoch")
        expectEqual(guardNode?["sceneId"] as? String, "s1", "guard scene = the layout's scene")
        expectEqual((guardNode?["previewRevision"] as? NSNumber)?.int64Value, 7, "the core's Preview revision")
        expectEqual(guardNode?["keepProgramScene"] as? Bool, false, "swaps")
        expectEqual(guardNode?["playOutOthers"] as? Bool, false, "playOutOthers")
        expect(jsonEqual(guardNode?["cues"], [["instanceId": "playlist-title", "cueRevision": 3]]), "the prepared cues (\(String(describing: guardNode?["cues"])))")
        let types = commands.compactMap { $0["type"] as? String }
        expectEqual(types.filter { $0 == "load-scene-graph" }.count, 1, "exactly one load-scene-graph")
        expectEqual(commands.first { $0["type"] as? String == "load-scene-graph" }?["sceneId"] as? String, "s1", "the layout scene loads")
        expect(!types.contains { $0.hasPrefix("graphics-") }, "no graphics-* command in the batch")
    }

    @MainActor
    static func testCommittedReceiptConfirms() {
        let (model, core) = guardedTakeApp(script: ["committed"])
        expectEqual(runTake(model).result, .success, "committed")
        expectEqual(model.takeStatus, "Program updated", "Windows success status")
        expect(model.graphics.playout.preparedLayout == nil, "the prepared layout is confirmed (cleared)")
        expect(model.pendingGuardedTake == nil, "nothing pending")
        expectEqual(model.programSceneId, "s1", "Program is the layout scene")
        expectEqual(core.transitions.count, 1, "one transition")
    }

    @MainActor
    static func testRefusedReceiptRollsBackScenes() {
        let (model, core) = guardedTakeApp(script: ["refused:layout-scene-changed"])
        let result = runTake(model).result
        let text = "Shot Layout Take refused. layout-scene-changed"
        expectEqual(result, .failed(text), "Windows refusal text")
        expectEqual(model.takeStatus, text, "status")
        expectEqual(model.programSceneId, "s2", "Program restored")
        expectEqual(model.previewSceneId, "s1", "Preview restored")
        spin(until: { model.graphics.playout.preparedLayout == nil }, limit: 2)
        expect(model.graphics.playout.preparedLayout == nil, "the prepared layout is cancelled")
        let uncue = core.graphics.commands.first { ($0["graphics"] as? JSONObject)?["action"] as? String == "uncue" }?["graphics"] as? JSONObject
        expectEqual(uncue?["cueOwner"] as? String, guardedAttempt, "its owned cue released")
    }

    @MainActor
    static func testWarmingThenReadyReplaysTheSameOperation() {
        let (model, core) = guardedTakeApp(script: ["warming", "warming", "ready", "committed"])
        expectEqual(runTake(model).result, .success, "committed after warming")
        expectEqual(core.transitions.count, 2, "sent, then replayed once on ready")
        expectEqual(Set(core.transitions.compactMap { $0["operationId"] as? String }), [guardedAttempt], "the SAME operation id")
        expectEqual(core.bodies.count, 4, "send, poll, poll, replay")
        expectEqual((core.bodies[1]["commands"] as? [Any])?.count, 0, "a warming round only polls")
    }

    @MainActor
    static func testEpochChangeDuringResolution() {
        let (model, _) = guardedTakeApp(script: ["warming", "epoch:epoch-2"])
        let text = "Core restarted. Cue the Shot Layout again."
        expectEqual(runTake(model).result, .failed(text), "Windows text")
        expectEqual(model.programSceneId, "s2", "rolled back")
        expect(model.pendingGuardedTake == nil, "not pending")
    }

    @MainActor
    static func testNoReceiptAfter16Rounds() {
        let text = "The core could not confirm the Take. Cue the Shot Layout again."
        // Program still the previous scene: rolled back.
        do {
            let (model, core) = guardedTakeApp(script: ["none"])
            var sleeps: [UInt64] = []
            model.timerSleepForTesting = { sleeps.append($0) }
            expectEqual(runTake(model).result, .failed(text), "no receipt")
            expectEqual(core.bodies.count, 16, "16 rounds")
            // (The timer seam is shared with the shell's other debounces; count the Take's 125 ms rounds.)
            expectEqual(sleeps.filter { $0 == 125_000_000 }.count, 15, "15 waits of 125 ms between them")
            expectEqual(model.programSceneId, "s2", "rolled back: the core still shows the previous Program")
        }
        // The core moved to the new scene: no rollback.
        do {
            let (model, core) = guardedTakeApp(script: ["none"])
            core.programSceneId = "s1"
            expectEqual(runTake(model).result, .failed(text), "no receipt")
            expectEqual(model.programSceneId, "s1", "kept: the core already shows the taken scene")
        }
        // No observation at all: the guard stays pending and the next Take only reads the receipt.
        do {
            let (model, core) = guardedTakeApp(script: ["throw"])
            let waiting = "Shot Layout Take is awaiting confirmation. Retry Take to check the same operation."
            expectEqual(runTake(model).result, .failed(waiting), "awaiting confirmation")
            expect(model.pendingGuardedTake != nil, "the guard stays pending")
            expectEqual(model.takeButtonTitle, "Check Take", "the Take button reads Check Take")
            expect(model.canTake, "Take is available to check")
            let sent = core.transitions.count
            core.script = ["committed"]
            expectEqual(runTake(model).result, .success, "the retry resolves")
            expectEqual(core.transitions.count, sent, "without a new transition")
            expectEqual(model.programSceneId, "s1", "Program is the layout scene")
        }
    }

    @MainActor
    static func testKeepProgramSceneDoesNotSwap() {
        let (model, core) = guardedTakeApp(sceneId: "", script: ["committed"])
        model.previewSceneId = "s2"; model.lastSnapshot = ["sceneId": "s2"]
        model.graphics.playout.preparedLayout = GraphicsPreparedLayout(
            layout: model.graphics.document.shotLayouts[0], coreEpoch: "epoch-1", attemptId: guardedAttempt,
            cues: [GraphicsLayoutCue(instanceId: "playlist-title", cueRevision: 3)], previewStamp: model.layoutPreviewStamp())
        expect(model.canTake, "Take is available with Preview equal to Program (a prepared layout)")
        expectEqual(runTake(model).result, .success, "committed")
        expectEqual(model.programSceneId, "s2", "Program unchanged")
        let commands = core.bodies.first?["commands"] as? [JSONObject] ?? []
        let guardNode = commands.first?["graphicsLayout"] as? JSONObject
        expectEqual(guardNode?["keepProgramScene"] as? Bool, true, "keepProgramScene")
        expectEqual(guardNode?["sceneId"] as? String, "s2", "the guard names the current Program scene")
        expectEqual((guardNode?["previewRevision"] as? NSNumber)?.int64Value, 0, "no Preview revision needed")
        expectEqual(commands.first { $0["type"] as? String == "load-scene-graph" }?["sceneId"] as? String, "s2", "loads the current Program")
    }

    @MainActor
    static func testTakeRefusedWhilePreparing() {
        let (model, core) = guardedTakeApp(script: ["committed"])
        model.graphics.playout.layoutPreparing = true
        model.graphics.playout.onChange()
        expect(!model.canTake, "Take unavailable while preparing")
        expectEqual(runTake(model).result, .failed("Wait for all Shot Layout graphics to prepare."), "Windows text")
        expectEqual(core.bodies.count, 0, "nothing sent")
    }

    @MainActor
    static func testStalePreparedLayoutRefusesTheTake() {
        let (model, core) = guardedTakeApp(script: ["committed"])
        model.lastSnapshot = ["sceneId": "s2", "previewScene": ["sceneId": "s1", "revision": 0]]
        model.graphics.playout.preparedLayout = GraphicsPreparedLayout(
            layout: model.graphics.document.shotLayouts[0], coreEpoch: "epoch-1", attemptId: guardedAttempt,
            cues: [GraphicsLayoutCue(instanceId: "playlist-title", cueRevision: 3)], previewStamp: model.layoutPreviewStamp())
        expectEqual(runTake(model).result, .failed("The Shot Layout cue changed. Cancel and cue it again."), "revision 0 yields no guard")
        expectEqual(core.bodies.count, 0, "nothing sent")
        expectEqual(model.programSceneId, "s2", "no swap")
    }

    /// Review Focus 3: a Preview change while the guarded Take warms sends nothing (the core would
    /// refuse any scene load as guarded-layout-take-pending); exactly one sync follows the receipt.
    @MainActor
    static func testGuardedTakeHoldsSceneSync() {
        let (model, core) = guardedTakeApp(script: ["warming", "warming", "warming", "committed"])
        var rounds = 0
        model.timerSleepForTesting = { [weak model] ns in
            guard ns == AppModel.guardedTakeRoundDelayNs else { return }   // the Take's rounds only
            rounds += 1
            if rounds == 1 {
                model?.selectPreviewScene("s3")
                try? await Task.sleep(nanoseconds: 250_000_000)   // the 120 ms scene debounce fires while warming
            }
        }
        expectEqual(runTake(model).result, .success, "committed")
        let duringTake = core.bodies.count
        expectEqual(rounds, 3, "three warming rounds waited")
        expectEqual(duringTake, 4, "send + 3 polls; no scene sync went out while pending")
        spin(until: { core.bodies.count > duringTake }, limit: 1)
        spin(until: { false }, limit: 0.3)
        expectEqual(core.bodies.count, duringTake + 1, "exactly one owed sync after the receipt")
        let owed = core.bodies.last?["commands"] as? [JSONObject] ?? []
        expectEqual(owed.first { $0["type"] as? String == "set-preview-scene" }?["sceneId"] as? String, "s3", "carrying the newer Preview")
        expect(!owed.contains { $0["type"] as? String == "begin-take-transition" }, "an ordinary sync")
    }

    @MainActor
    static func testControlTakeUsesTheGuard() {
        let (model, core) = guardedTakeApp(script: ["committed"])
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(surface, "transport.take"), .success, "remote Take succeeds")
        expectEqual(core.transitions.first?["operationId"] as? String, guardedAttempt, "through the same guard")
        expect(model.graphics.playout.preparedLayout == nil, "confirmed")
    }
}
