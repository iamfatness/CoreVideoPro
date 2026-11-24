// Graphics G4-G7 Task 4 — countdown transport on clock rows. Transcripts:
// contracts/graphics-shell/clock-transcripts.vectors.json, recorded by
// GraphicsShellVectorTests.ClockTranscripts from the real GraphicsPlayoutService.

import Foundation

extension ShellTests {
    static var graphicsClockCases: [(String, () -> Void)] {
        [
            ("graphics-clock/transcripts", testClockTranscriptsReplay),
            ("graphics-clock/readout-vectors", testClockReadoutVectors),
            ("graphics-clock/transport-only-for-countdown-rows", testTransportShownOnlyForCountdownRows),
            ("graphics-clock/after-core-restart", testClockTransportAfterCoreRestart),
        ]
    }

    @MainActor
    static func testClockTranscriptsReplay() {
        let rows = namesClockRows()
        var checked = 0
        for vector in loadGraphicsVectors("clock-transcripts") where vector["operation"] as? String == "clock" {
            let id = vector["id"] as? String ?? "?"
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            let steps = expected["steps"] as? [JSONObject] ?? []
            let core = ScriptedGraphicsCore(replaying: steps, initialNode: input["initialNode"] as? JSONObject ?? [:])
            let model = graphicsModel(core)
            model.document.playlist = rows
            let action = (input["intent"] as? JSONObject)?["action"] as? String ?? ""
            let result = awaitMain { await model.clock(rowId: "countdown", action: action).value }
            expectEqual(core.commands.count, steps.count, "\(id): command count")
            expectEqual(core.replayFaults, [], "\(id): commands as recorded")
            expectEqual(result?.reason, expected["reason"] as? String, "\(id): reason")
            expectEqual(result?.accepted, expected["accepted"] as? Bool, "\(id): accepted")
            checked += 1
        }
        expect(checked >= 7, "every clock transcript replayed (\(checked))")
    }

    static func testClockReadoutVectors() {
        guard let row = namesClockRows().first(where: { $0.id == "countdown" }) else { expect(false, "countdown row"); return }
        var checked = 0
        for vector in loadGraphicsVectors("clock-transcripts") where vector["operation"] as? String == "readout" {
            let id = vector["id"] as? String ?? "?"
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            var r = row
            var clock = r.clock ?? [:]
            clock["durationSeconds"] = input["durationSeconds"]
            r.json["clock"] = clock
            var instance: GraphicsInstance?
            if let c = input["clock"] as? JSONObject {
                instance = GraphicsState(node: ["schemaVersion": 1, "coreEpoch": "e", "instances": [["instanceId": "playlist-countdown", "clock": c]]])
                    .instance("playlist-countdown")
            }
            expectEqual(GraphicsClockReadout.text(row: r, instance: instance), expected["text"] as? String ?? "", "\(id)")
            checked += 1
        }
        expect(checked >= 6, "every readout vector replayed (\(checked))")
    }

    static func testTransportShownOnlyForCountdownRows() {
        let rows = namesClockRows()
        expect(GraphicsModel.showsClockTransport(rows.first { $0.id == "countdown" }!), "a countdown row shows the transport")
        expect(!GraphicsModel.showsClockTransport(rows.first { $0.id == "panel-names" }!), "a names list does not")
        var wall = rows.first { $0.id == "countdown" }!
        var clock = wall.clock ?? [:]; clock["mode"] = "wall"; wall.json["clock"] = clock
        expect(!GraphicsModel.showsClockTransport(wall), "a wall clock does not")
    }

    /// A transport press after a core restart goes to the new core's instance (its revision), and is
    /// refused while the new core has not created the row's instance yet; nothing is replayed.
    @MainActor
    static func testClockTransportAfterCoreRestart() {
        let core = ScriptedGraphicsCore(instances: [ScriptedGraphicsCore.instance("playlist-countdown", revision: 7, program: 7, visibility: "visible", template: "clock-panel")])
        let model = graphicsModel(core)
        model.document.playlist = namesClockRows()
        _ = awaitMain { await model.clock(rowId: "countdown", action: "start").value }
        model.coreGenerationChanged()
        let fresh = ScriptedGraphicsCore(epoch: "epoch-2")
        core.node = fresh.node
        model.observe(snapshot: ["graphics": core.node, "profile": ["capabilityStates": ["graphics-instances": ["state": "available"]]]])
        let refused = awaitMain { await model.clock(rowId: "countdown", action: "pause").value }
        expectEqual(refused?.reason, "That row is not on Preview or Program yet. Cue or Show it first.", "no instance on the new core: refused, not replayed")
        expectEqual(core.commands.count, 1, "only the first core's start was sent")
        core.node["instances"] = [ScriptedGraphicsCore.instance("playlist-countdown", revision: 2, program: 2, visibility: "visible", template: "clock-panel")]
        model.observe(snapshot: ["graphics": core.node, "profile": ["capabilityStates": ["graphics-instances": ["state": "available"]]]])
        _ = awaitMain { await model.clock(rowId: "countdown", action: "pause").value }
        let last = core.commands.last?["graphics"] as? JSONObject ?? [:]
        expectEqual(last["coreEpoch"] as? String, "epoch-2", "sent to the new core")
        expectEqual((last["expectedRevision"] as? NSNumber)?.intValue, 2, "with the new instance's revision")
    }
}
