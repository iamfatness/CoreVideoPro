// Graphics G1 Task 5 — Graphics tab model, On Air strip actions, legacy drawer
// fixes (L5/L7/L8/L18a). Plan D9.

import Foundation

/// A fake core for graphics transcripts: records every graphics command and
/// answers with a `graphics` node the test scripts. `react` mutates the node in
/// response to a command (e.g. a hide drops programRevision); the default
/// accepts everything and changes nothing.
@MainActor
final class ScriptedGraphicsCore {
    var node: JSONObject
    private(set) var commands: [JSONObject] = []
    var react: (JSONObject, inout JSONObject) -> (accepted: Bool, reason: String) = { _, _ in (true, "") }
    var hold = false
    private var held: [CheckedContinuation<Void, Never>] = []
    private var revision: Int64 = 100

    init(epoch: String = "epoch-1", instances: [JSONObject] = []) {
        node = ["schemaVersion": 1, "coreEpoch": epoch, "instances": instances, "operations": [] as [Any]]
    }

    /// Command types in order ("graphics-instance-trigger:hide:<instance>").
    var transcript: [String] {
        commands.map { command in
            let g = command["graphics"] as? JSONObject ?? [:]
            var parts = [command["type"] as? String ?? "?"]
            if let action = g["action"] as? String { parts.append(action) }
            if let id = g["instanceId"] as? String { parts.append(id) }
            return parts.joined(separator: ":")
        }
    }

    func releaseAll() { let h = held; held = []; h.forEach { $0.resume() } }

    func send(_ body: JSONObject) async throws -> JSONObject {
        let command = (body["commands"] as? [JSONObject])?.first ?? [:]
        commands.append(command)
        if hold { await withCheckedContinuation { held.append($0) } }
        let g = command["graphics"] as? JSONObject ?? [:]
        let outcome = react(command, &node)
        revision += 1
        // Like the core: the result names the instance's revision after the command.
        let instanceId = g["instanceId"] as? String ?? ""
        let current = (node["instances"] as? [JSONObject] ?? []).first { $0["instanceId"] as? String == instanceId }?["revision"]
        let result: JSONObject = ["requestId": g["requestId"] as? String ?? "", "clientId": g["clientId"] as? String ?? "",
                                  "instanceId": instanceId, "accepted": outcome.accepted,
                                  "reason": outcome.reason, "currentRevision": (current as? NSNumber)?.int64Value ?? revision]
        var ops = node["operations"] as? [JSONObject] ?? []
        ops.append(result)
        node["operations"] = Array(ops.suffix(8))
        node["lastResult"] = result
        return ["snapshot": ["graphics": node, "profile": ["capabilityStates": ["graphics-instances": ["state": "available"]]]]]
    }

    static func instance(_ id: String, revision: Int64 = 3, program: Int64 = 0, cue: Int64 = 0, visibility: String = "hidden",
                         showPending: Bool = false, cuePending: Bool = false, name: String = "", template: String = "compact-solid") -> JSONObject {
        ["instanceId": id, "revision": revision, "programRevision": program, "cueRevision": cue, "visibility": visibility,
         "showPending": showPending, "cuePending": cuePending, "templateId": template, "templateVersion": 1, "fields": ["name": name]]
    }

    /// Default reaction: hide takes an instance off Program, uncue clears its cue.
    static func liveReaction(_ command: JSONObject, _ node: inout JSONObject) -> (accepted: Bool, reason: String) {
        let g = command["graphics"] as? JSONObject ?? [:]
        guard let id = g["instanceId"] as? String, var instances = node["instances"] as? [JSONObject],
              let index = instances.firstIndex(where: { $0["instanceId"] as? String == id }) else { return (true, "") }
        switch g["action"] as? String {
        case "hide": instances[index]["programRevision"] = 0; instances[index]["visibility"] = "hidden"; instances[index]["showPending"] = false
        case "uncue": instances[index]["cueRevision"] = 0
        default: break
        }
        node["instances"] = instances
        return (true, "")
    }
}

extension ShellTests {
    static var graphicsOperateCases: [(String, () -> Void)] {
        [
            ("graphics/capability-gate", testGraphicsCapabilityGate),
            ("graphics/strip-hide-transcript", testGraphicsStripHideTranscript),
            ("graphics/strip-clear-all-includes-show-pending", testGraphicsClearAllIncludesShowPending),
            ("graphics/strip-clear-cued-skips-social", testGraphicsClearCuedSkipsSocial),
            ("graphics/strip-truth-is-the-snapshot", testGraphicsStripTruthIsTheSnapshot),
            ("graphics/status-lines", testGraphicsStatusLines),
            ("graphics/playout-runs-one-at-a-time", testGraphicsPlayoutRunsOneAtATime),
            ("overlays/timing-presets-match-windows", testOverlayTimingPresetsMatchWindows),
            ("overlays/old-preset-loads-as-custom", testOverlayOldPresetLoadsAsCustom),
            ("overlays/timing-resent-live-while-on-air", testOverlayTimingResentLiveWhileOnAir),
            ("overlays/position-list-matches-windows", testOverlayPositionListMatchesWindows),
            ("overlays/brand-kit-every-core-generation", testBrandKitSentOnEveryCoreGeneration),
        ]
    }

    @MainActor
    static func graphicsModel(_ core: ScriptedGraphicsCore, running: Bool = true) -> GraphicsModel {
        let model = GraphicsModel(coreRunning: { running }, send: { try await core.send($0) },
                                  documentPath: (graphicsTempDir() as NSString).appendingPathComponent("default.json"))
        model.saveDelayNs = 0
        model.observe(snapshot: ["graphics": core.node, "profile": ["capabilityStates": ["graphics-instances": ["state": "available"]]]])
        return model
    }

    @MainActor
    static func settle(_ model: GraphicsModel) { spin(until: { !model.busy }, limit: 3); RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }

    // ── capability ───────────────────────────────────────────────────────────

    @MainActor
    static func testGraphicsCapabilityGate() {
        expectEqual(GraphicsModel.capability("graphics-instances", in: ["profile": ["capabilityStates": ["graphics-instances": ["state": "available"]]]]),
                    true, "capabilityStates available")
        expectEqual(GraphicsModel.capability("graphics-instances", in: ["profile": ["capabilityStates": ["graphics-instances": ["state": "unavailable"]]]]),
                    false, "capabilityStates unavailable")
        expectEqual(GraphicsModel.capability("graphics-instances", in: ["profile": ["capabilityStates": ["other": ["state": "available"]],
                                                                                    "capabilities": ["graphics-instances"]]]),
                    false, "capabilityStates wins over the flat list when present (HasAvailableCapability)")
        expectEqual(GraphicsModel.capability("graphics-instances", in: ["profile": ["capabilities": ["graphics-instances"]]]),
                    true, "flat list when there are no states")
        expect(GraphicsModel.capability("graphics-instances", in: [:]) == nil, "no profile: unknown, keep what we had")

        let core = ScriptedGraphicsCore()
        let model = graphicsModel(core)
        expect(model.available, "capable + schema 1 + epoch = available")
        expect(model.unavailableReason == nil, "Operate|Design shown")
        model.observe(snapshot: ["profile": ["capabilityStates": ["graphics-instances": ["state": "unavailable"]]]])
        expect(!model.available, "capability withdrawn")
        expectEqual(model.unavailableReason, GraphicsModel.needsNewerCoreText, "the pane says why")
        let stopped = graphicsModel(ScriptedGraphicsCore(), running: false)
        expect(!stopped.available, "no running core: unavailable")
    }

    // ── strip actions ────────────────────────────────────────────────────────

    @MainActor
    static func testGraphicsStripHideTranscript() {
        let core = ScriptedGraphicsCore(instances: [ScriptedGraphicsCore.instance("lower", revision: 7, program: 7, visibility: "on-air", name: "Ada")])
        core.react = ScriptedGraphicsCore.liveReaction
        let model = graphicsModel(core)
        expectEqual(model.strip.onProgram.map(\.instanceId), ["lower"], "one Program chip")
        model.hide("lower")
        settle(model)
        expectEqual(core.transcript, ["graphics-instance-trigger:hide:lower"], "Hide = one trigger hide")
        let g = core.commands.first?["graphics"] as? JSONObject ?? [:]
        expectEqual((g["expectedRevision"] as? NSNumber)?.int64Value, 7, "at the instance's current revision")
        expectEqual(g["clientId"] as? String, "graphics-playout", "as the Windows playout client")
        expectEqual(g["coreEpoch"] as? String, "epoch-1", "for the observed epoch")
        expect((g["requestId"] as? String).map(GraphicsCommandRequest.validId) == true, "with a valid fresh request id")
        expect(model.strip.onProgram.isEmpty, "the reply's snapshot clears the chip")
    }

    @MainActor
    static func testGraphicsClearAllIncludesShowPending() {
        let core = ScriptedGraphicsCore(instances: [
            ScriptedGraphicsCore.instance("a", program: 3, visibility: "on-air"),
            ScriptedGraphicsCore.instance("b", program: 0, showPending: true),
            ScriptedGraphicsCore.instance("c", program: 0, cue: 3),
            ScriptedGraphicsCore.instance("d", program: 2, visibility: "building-out"),
        ])
        core.react = ScriptedGraphicsCore.liveReaction
        let model = graphicsModel(core)
        model.clearAll()
        settle(model)
        expectEqual(core.transcript, ["graphics-instance-trigger:hide:a", "graphics-instance-trigger:hide:b"],
                    "Clear all hides on-Program AND show-pending instances, nothing else (GraphicsPlayoutService.cs:180)")
        expectEqual(model.status, "All graphics hidden.", "Windows' reason")
    }

    @MainActor
    static func testGraphicsClearCuedSkipsSocial() {
        let core = ScriptedGraphicsCore(instances: [
            ScriptedGraphicsCore.instance("playlist-x", cue: 3),
            ScriptedGraphicsCore.instance("social-review-card", cue: 3, template: "youtube-chat-card"),
            ScriptedGraphicsCore.instance("on", program: 3, visibility: "on-air"),
        ])
        core.react = ScriptedGraphicsCore.liveReaction
        let model = graphicsModel(core)
        model.clearCued()
        settle(model)
        expectEqual(core.transcript, ["graphics-instance-trigger:uncue:playlist-x"], "Clear cued uncues cued instances but never a social card")
        expectEqual(model.status, "Preview cleared.", "Windows' reason")
        let single = ScriptedGraphicsCore(instances: [ScriptedGraphicsCore.instance("q", cue: 4)])
        let m2 = graphicsModel(single)
        m2.clearCue("q")
        settle(m2)
        expectEqual(single.transcript, ["graphics-instance-trigger:uncue:q"], "a chip's Clear cue is one uncue")
    }

    /// An ACCEPTED hide whose snapshot still shows the instance on Program keeps
    /// the chip: strip truth is the core's instances, never a local acceptance.
    @MainActor
    static func testGraphicsStripTruthIsTheSnapshot() {
        let core = ScriptedGraphicsCore(instances: [ScriptedGraphicsCore.instance("lower", program: 3, visibility: "on-air")])
        let model = graphicsModel(core)   // default reaction accepts and changes nothing
        model.hide("lower")
        settle(model)
        expectEqual(model.strip.onProgram.map(\.instanceId), ["lower"], "accepted is not on-air truth: the chip stays")
    }

    @MainActor
    static func testGraphicsStatusLines() {
        let core = ScriptedGraphicsCore(instances: [ScriptedGraphicsCore.instance("lower", program: 3, visibility: "on-air")])
        core.react = { _, _ in (false, "revision-conflict") }
        let model = graphicsModel(core)
        model.hide("lower")
        settle(model)
        expectEqual(model.status, "revision-conflict", "a native refusal shows verbatim")
        model.report(GraphicsOperationResult(accepted: false, reason: "graphics-command-overloaded"))
        expectEqual(model.status, GraphicsModel.busyText, "overloaded reads as busy")
        var restarted = core.node
        restarted["coreEpoch"] = "epoch-2"
        restarted["instances"] = [] as [Any]
        model.observe(snapshot: ["graphics": restarted])
        expectEqual(model.status, GraphicsModel.restartedText, "an epoch change says graphics were cleared")
        expect(model.strip.onProgram.isEmpty, "and the strip follows the new core")
        model.hide("ghost")
        settle(model)
        expectEqual(model.status, "That graphic no longer exists.", "a stale chip is refused locally")
        expectEqual(core.commands.count, 1, "without sending anything")
    }

    @MainActor
    static func testGraphicsPlayoutRunsOneAtATime() {
        let core = ScriptedGraphicsCore(instances: [
            ScriptedGraphicsCore.instance("a", program: 3, visibility: "on-air"),
            ScriptedGraphicsCore.instance("b", program: 3, visibility: "on-air"),
        ])
        core.react = ScriptedGraphicsCore.liveReaction
        core.hold = true
        let model = graphicsModel(core)
        model.hide("a")
        model.hide("b")
        spin(until: { core.commands.count == 1 }, limit: 2)
        RunLoop.main.run(until: Date().addingTimeInterval(0.05))
        expectEqual(core.commands.count, 1, "the second Hide waits for the first (one operation at a time, not refused)")
        expect(model.busy, "busy while running")
        core.hold = false
        core.releaseAll()
        spin(until: { core.commands.count == 2 }, limit: 2)
        settle(model)
        expectEqual(core.transcript, ["graphics-instance-trigger:hide:a", "graphics-instance-trigger:hide:b"], "then runs")
        expect(!model.busy, "idle after")
    }

    // ── legacy drawer (D9) ───────────────────────────────────────────────────

    static func testOverlayTimingPresetsMatchWindows() {
        expectEqual(MotionTiming.allCases.map(\.label), ["Quick", "Smooth", "Gentle", "Custom"], "Windows preset list")
        expect(MotionTiming.quick.timing! == (220, 160), "Quick 220/160")
        expect(MotionTiming.smooth.timing! == (420, 320), "Smooth 420/320")
        expect(MotionTiming.gentle.timing! == (700, 550), "Gentle 700/550")
        expect(MotionTiming.custom.timing == nil, "Custom keeps the numbers")
        let fresh = OverlaysState()
        expectEqual(fresh.buildInMs, 220, "default build in = Quick")
        expectEqual(fresh.buildOutMs, 160, "default build out = Quick")
        expectEqual(fresh.motionTiming, MotionTiming.quick.rawValue, "default preset Quick")
    }

    static func testOverlayOldPresetLoadsAsCustom() {
        for old in ["snap", "standard", "gentle"] {
            var saved = OverlaysState()
            saved.motionTiming = old
            saved.buildInMs = 1600
            saved.buildOutMs = 900
            let loaded = saved.migratingLegacyTiming()
            expectEqual(loaded.motionTiming, MotionTiming.custom.rawValue, "old preset \(old) loads as Custom")
            expectEqual(loaded.buildInMs, 1600, "\(old): saved build in kept, never silently retimed")
            expectEqual(loaded.buildOutMs, 900, "\(old): saved build out kept")
        }
        var current = OverlaysState()
        current.motionTiming = MotionTiming.smooth.rawValue
        current.buildInMs = 420
        current.buildOutMs = 320
        expectEqual(current.migratingLegacyTiming(), current, "a current preset is untouched")
        var wild = OverlaysState()
        wild.buildInMs = 9000
        wild.buildOutMs = 1
        let clamped = wild.migratingLegacyTiming()
        expect(clamped.buildInMs == 2000 && clamped.buildOutMs == 50, "saved numbers clamp to the core's 50-2000")
        expectEqual(LowerThirdPositions.migrate("center"), "lower-left", "a saved center loads as lower-left")
        expectEqual(LowerThirdPositions.migrate("upper-right"), "upper-right", "a valid position is kept")
    }

    @MainActor
    static func testOverlayTimingResentLiveWhileOnAir() {
        let model = AppModel()
        var sent: [JSONObject] = []
        model.overlayCommandObserver = { sent.append($0) }
        model.lowerThirdPhase = "hidden"
        model.applyMotionTiming(.smooth)
        expect(sent.isEmpty, "off air: a timing change sends nothing")
        expect(model.overlays.buildInMs == 420 && model.overlays.buildOutMs == 320, "preset moves the numbers")
        model.lowerThirdPhase = "on-air"
        model.applyMotionTiming(.gentle)
        expectEqual(sent.count, 1, "on air: one live update (Windows ApplyProgramLowerThirdTimingChange)")
        let update = sent.last ?? [:]
        expectEqual(update["type"] as? String, "set-overlay-asset", "the key's own command")
        expectEqual(update["overlayId"] as? String, "key:lower-third", "the lower-third key")
        expectEqual(update["buildInMs"] as? Int, 700, "new build in")
        expectEqual(update["buildOutMs"] as? Int, 550, "new build out")
        expect(update["keyPhase"] == nil, "no keyPhase: the core keeps the key on air, no re-animation")
        model.setBuildInMs(300)
        expectEqual(model.overlays.motionTiming, MotionTiming.custom.rawValue, "a manual edit is Custom")
        expectEqual(sent.last?["buildInMs"] as? Int, 300, "and is re-sent live too")
        model.setBuildOutMs(5000)
        expectEqual(model.overlays.buildOutMs, 2000, "clamped to the core range")
    }

    static func testOverlayPositionListMatchesWindows() {
        expectEqual(LowerThirdPositions.all.map(\.value), ["lower-left", "lower-right", "upper-left", "upper-right"],
                    "Windows LowerThirdLayoutHelper.LowerThirdPositionOptions")
    }

    @MainActor
    static func testBrandKitSentOnEveryCoreGeneration() {
        let model = AppModel()
        var sent: [JSONObject] = []
        model.overlayCommandObserver = { sent.append($0) }
        model.overlays.brandName = "Show"
        model.onConnected()
        model.onConnected()
        let kits = sent.filter { $0["type"] as? String == "set-brand-kit" }
        expectEqual(kits.count, 2, "one set-brand-kit per core generation, with no edit (L18a)")
        expectEqual(kits.last?["name"] as? String, "Show", "carrying the operator's kit")
    }
}
