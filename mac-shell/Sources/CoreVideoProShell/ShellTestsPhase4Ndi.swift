// macOS parity Phase 4, Task 9 (N1b): NDI output as its own output. Kept in its
// own file so parallel lanes extending ShellTests.swift do not collide.
//
// Every expected string is a literal copied from the Windows tests
// (MediaCore.Tests/NdiOutputTelemetryTests.cs, WinUI.Tests/NdiOutputValidationTests.cs,
// NdiOutputDestinationTests.cs); every other expectation is a fixture constant.
//
// Not here, by the lane split the coordinator confirmed: transport.ndi.set /
// toggle and the `ndi` feedback flag are the control lane's Task 6, which drives
// AppModel.setNdiOutput(_:) through the control surface.

import Foundation

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var ndiPhase4Cases: [(String, () -> Void)] {
        [
            ("ndi/status-table-matches-windows", testNdiStatusTableMatchesWindows),
            ("ndi/validation-matches-windows", testNdiValidationMatchesWindows),
            ("ndi/destination-row-shape", testNdiDestinationRowShape),
            ("ndi/toggle-on-alone-sends-start-with-only-ndi", testNdiToggleOnAloneSendsStartWithOnlyNdi),
            ("ndi/refused-on-changes-nothing-but-the-reason", testNdiRefusedOnChangesNothingButTheReason),
            ("ndi/stream-stop-keeps-ndi", testNdiStreamStopKeepsNdi),
            ("ndi/all-off-sends-stop", testNdiAllOffSendsStop),
            ("ndi/rearmed-on-core-generation", testNdiRearmedOnCoreGeneration),
            ("ndi/stopped-core-reads-starting", testNdiStoppedCoreReadsStarting),
            ("ndi/blank-name-while-on-turns-ndi-off-only", testNdiBlankNameWhileOnTurnsNdiOffOnly),
            ("ndi/not-streaming-in-footer-or-telemetry", testNdiIsNotStreaming),
            ("ndi/prefs-default-off-and-additive", testNdiPrefsDefaultOffAndAdditive),
            ("ndi/rename-while-off-sends-nothing", testNdiRenameWhileOffSendsNothing),
            ("ndi/rename-while-on-updates-the-source", testNdiRenameWhileOnUpdatesTheSource),
            ("ndi/record-start-and-stop-keep-ndi", testNdiRecordStartAndStopKeepNdi),
            ("ndi/never-asks-on-quit", testNdiNeverAsksOnQuit),
            ("ndi/pressure-is-not-stream-health", testNdiPressureIsNotStreamHealth),
            ("ndi/failed-update-is-loud", testNdiFailedUpdateIsLoud),
        ]
    }

    private static func ndiSender(_ status: String, warning: String? = nil, destination: String = "ndi") -> JSONObject {
        var sender: JSONObject = ["senderId": destination, "destination": destination, "status": status]
        if let warning { sender["warning"] = warning }
        return sender
    }

    // ── ports of NdiOutputTelemetryTests (the " + key" case does not apply) ──

    static func testNdiStatusTableMatchesWindows() {
        func status(_ enabled: Bool, _ senders: [JSONObject], _ reason: String? = nil) -> String {
            NdiOutputPolicy.status(enabled: enabled, senders: senders, programName: "Studio A", refusalReason: reason)
        }
        expectEqual(status(false, []), "Off", "OffReadsOff")
        expectEqual(status(true, []), "Starting", "OnWithNoSenderYetReadsStarting")
        expectEqual(status(true, [ndiSender("live")]), "Live \u{00B7} Studio A", "LiveSenderNamesTheProgram")
        expectEqual(status(true, [ndiSender("warning", warning: "runtime missing")]),
                    "Warning \u{00B7} runtime missing", "WarningCarriesTheSenderReason")
        expectEqual(status(false, [], "NDI program name is blank"),
                    "Unavailable \u{00B7} NDI program name is blank", "RefusalWhileOffReadsUnavailable")
        expectEqual(status(true, [ndiSender("live", destination: "rtmp")]), "Starting", "StreamSendersAreIgnored")
        // Mac: no graphics key, so a live key sender never appends " + key".
        expectEqual(status(true, [ndiSender("live"), ndiSender("live", destination: "ndi-graphics")]),
                    "Live \u{00B7} Studio A", "no + key suffix on Mac")
        // Deliberate parity (M9c): Windows reads `status`, not the lifecycle.
        var preparing = ndiSender("live")
        preparing["lifecycle"] = ["state": "preparing"]
        expectEqual(status(true, [preparing]), "Live \u{00B7} Studio A", "status, not lifecycle, as Windows")
        expectEqual(status(true, [["destination": "ndi", "status": "failed", "lastError": "send failed"]]),
                    "Warning \u{00B7} send failed", "failed falls through to lastError")
    }

    // ── ports of NdiOutputValidationTests (the graphics-key clauses do not apply) ──

    static func testNdiValidationMatchesWindows() {
        expect(NdiOutputPolicy.validate(programName: "Studio A", coreHasNdiOutput: true) == nil, "ValidNameIsAccepted")
        for blank in ["", "   "] {
            expectEqual(NdiOutputPolicy.validate(programName: blank, coreHasNdiOutput: true),
                        "Configure an NDI program name before turning on NDI output.",
                        "BlankProgramNameIsRefusedWithTheNameReason '\(blank)'")
        }
        expectEqual(NdiOutputPolicy.validate(programName: "Studio A", coreHasNdiOutput: false),
                    "NDI output is not available in this core build (missing ndi-output).",
                    "CoreWithoutNdiOutputIsRefusedWithTheMissingCapabilityReason")
        expect(NdiOutputPolicy.validate(programName: "Studio A", coreHasNdiOutput: nil) == nil,
               "CoreNotUpYetDefersToTheCoresOwnSenderStatus")
    }

    static func testNdiDestinationRowShape() {
        var settings = NdiOutputSettings()
        expect(NdiOutputPolicy.destinationSettings(settings, streamFps: 30).isEmpty, "Off_SelectsNothing")
        settings.enabled = true
        settings.programName = "  Studio A  "
        settings.groupName = " "
        let rows = NdiOutputPolicy.destinationSettings(settings, streamFps: 30)
        expectEqual(rows.count, 1, "On_WithName_SelectsProgramOnly")
        expectEqual(rows.first?["id"] as? String, "ndi", "id")
        expectEqual(rows.first?["label"] as? String, "NDI", "label")
        expectEqual(rows.first?["ndiName"] as? String, "Studio A", "name trimmed (NormalizeOutputText)")
        expectEqual(rows.first?["ndiGroup"] as? String, "public", "blank group falls back to public")
        expectEqual(rows.first?["fps"] as? Int, 30, "the stream fps")
        settings.programName = " "
        expect(NdiOutputPolicy.destinationSettings(settings, streamFps: 30).isEmpty,
               "On_BlankName_ValidatedOnly_SelectsNothing")
    }

    // ── AppModel wiring ──────────────────────────────────────────────────────

    @MainActor
    private static func ndiModel() -> (AppModel, () -> [JSONObject]) {
        let model = AppModel()
        final class Box: @unchecked Sendable {
            let lock = NSLock()
            var bodies: [JSONObject] = []
        }
        let box = Box()
        model.quitRequestForTesting = { body, _ in
            box.lock.withLock { box.bodies.append(body) }
            return [:]
        }
        model.streamSecretSaver = { _, _ in }
        return (model, { box.lock.withLock { box.bodies } })
    }

    private static func ndiCommands(_ bodies: [JSONObject]) -> [JSONObject] {
        bodies.flatMap { $0["commands"] as? [JSONObject] ?? [] }
    }

    @MainActor
    static func testNdiToggleOnAloneSendsStartWithOnlyNdi() {
        let (model, bodies) = ndiModel()
        model.coreHasNdiOutput = true
        model.ndiOutput.programName = "Studio A"
        expect(model.setNdiOutput(true) == nil, "no refusal")
        spin(until: { !bodies().isEmpty }, limit: 3)
        let sent = ndiCommands(bodies())
        expectEqual(sent.map { $0["type"] as? String ?? "" }, ["start-program-output"], "one full-state start")
        expectEqual(sent.first?["destinations"] as? [String], ["ndi"], "NDI alone: Record and Stream are untouched")
        let rows = sent.first?["destinationSettings"] as? [JSONObject] ?? []
        expectEqual(rows.map { $0["id"] as? String ?? "" }, ["ndi"], "its row rides the start")
        expectEqual(rows.first?["ndiName"] as? String, "Studio A", "the operator's program name")
        expect(!model.streamingDesired && !model.recordingDesired, "NDI never sets Stream or Record intent")
        expectEqual(model.ndiOutputStatus, "Starting", "no sender yet")
        // Idempotent: the same value sends nothing more.
        expect(model.setNdiOutput(true) == nil, "same value")
        spin(until: { false }, limit: 0.2)
        expectEqual(ndiCommands(bodies()).count, 1, "same value is idempotent")
    }

    @MainActor
    static func testNdiRefusedOnChangesNothingButTheReason() {
        let (model, bodies) = ndiModel()
        model.coreHasNdiOutput = false
        expectEqual(model.setNdiOutput(true),
                    "NDI output is not available in this core build (missing ndi-output).", "refusal returned")
        spin(until: { false }, limit: 0.2)
        expect(bodies().isEmpty, "nothing is sent")
        expect(!model.ndiOutput.enabled, "NDI stays off")
        expectEqual(model.ndiOutputStatus,
                    "Unavailable \u{00B7} NDI output is not available in this core build (missing ndi-output).",
                    "the reason reaches the status line")
        expect(model.setNdiOutput(false) == nil, "off always succeeds")
    }

    /// Review focus 2: NDI on, streaming, not recording; stop streaming.
    @MainActor
    static func testNdiStreamStopKeepsNdi() {
        let (model, bodies) = ndiModel()
        model.coreHasNdiOutput = true
        model.streamKey = "rtmp-key-1"
        model.setNdiOutput(true)
        model.toggleStreaming()                          // start
        spin(until: { ndiCommands(bodies()).count >= 2 }, limit: 3)
        let start = ndiCommands(bodies()).last
        expectEqual(start?["destinations"] as? [String], ["rtmp", "ndi"], "streaming adds rtmp beside NDI")
        model.toggleStreaming()                          // stop
        spin(until: { ndiCommands(bodies()).count >= 3 }, limit: 3)
        let stop = ndiCommands(bodies()).last
        expectEqual(stop?["type"] as? String, "start-program-output", "never stop-encoder-session while NDI is on")
        expectEqual(stop?["destinations"] as? [String], ["ndi"], "what remains is NDI alone")
        expect(!ndiCommands(bodies()).contains { $0["type"] as? String == "stop-encoder-session" },
               "no stop-encoder-session at all")
    }

    @MainActor
    static func testNdiAllOffSendsStop() {
        let (model, bodies) = ndiModel()
        model.coreHasNdiOutput = true
        model.setNdiOutput(true)
        spin(until: { !bodies().isEmpty }, limit: 3)
        model.setNdiOutput(false)
        spin(until: { ndiCommands(bodies()).count >= 2 }, limit: 3)
        expectEqual(ndiCommands(bodies()).last?["type"] as? String, "stop-encoder-session",
                    "NDI off with nothing else running stops the session")
        // And a stream stop with nothing else left still stops (today's rule).
        let (plain, plainBodies) = ndiModel()
        plain.streamKey = "rtmp-key-1"
        plain.toggleStreaming()
        spin(until: { !plainBodies().isEmpty }, limit: 3)
        plain.toggleStreaming()
        spin(until: { ndiCommands(plainBodies()).count >= 2 }, limit: 3)
        expectEqual(ndiCommands(plainBodies()).last?["type"] as? String, "stop-encoder-session",
                    "stream stop with NDI off and not recording stops the session")
    }

    /// Review focus 1: a new core generation re-arms a persisted-on NDI output.
    @MainActor
    static func testNdiRearmedOnCoreGeneration() {
        let (model, bodies) = ndiModel()
        model.ndiOutput.enabled = true                   // restored from prefs, as at launch
        model.handleBridgeStatus(.exited(code: 9))             // the old core died
        model.onConnected()                              // the respawned core's handshake
        model.stopTimersForTesting()
        // onConnected sends other requests through the same seam: wait for THE start.
        spin(until: { ndiCommands(bodies()).contains { $0["type"] as? String == "start-program-output" } }, limit: 3)
        let start = ndiCommands(bodies()).first { $0["type"] as? String == "start-program-output" }
        expectEqual(start?["destinations"] as? [String], ["ndi"], "NDI is re-armed on the new core")
        // Off stays off across a generation.
        let (off, offBodies) = ndiModel()
        off.onConnected()
        off.stopTimersForTesting()
        spin(until: { false }, limit: 0.3)
        expect(!ndiCommands(offBodies()).contains { $0["type"] as? String == "start-program-output" },
               "an off NDI output sends nothing on a new core")
    }

    @MainActor
    static func testNdiStoppedCoreReadsStarting() {
        let (model, _) = ndiModel()
        model.coreHasNdiOutput = true
        model.ndiOutput.programName = "Studio A"
        model.setNdiOutput(true)
        model.applySnapshotForTesting(["outputSenderSession": ["senders": [ndiSender("live")]]])
        expectEqual(model.ndiOutputStatus, "Live \u{00B7} Studio A", "live from the core's row")
        model.handleBridgeStatus(.exited(code: 9))
        expectEqual(model.ndiOutputStatus, "Starting", "a stopped core reads Starting, never a stale Live")
        expect(model.ndiOutput.enabled, "NDI stays on across a core exit (unlike Record/Stream intent)")
    }

    @MainActor
    static func testNdiBlankNameWhileOnTurnsNdiOffOnly() {
        let (model, bodies) = ndiModel()
        model.coreHasNdiOutput = true
        model.streamKey = "rtmp-key-1"
        model.setNdiOutput(true)
        model.toggleStreaming()
        spin(until: { ndiCommands(bodies()).count >= 2 }, limit: 3)
        model.commitNdiSettings(programName: "  ", groupName: "public")
        spin(until: { ndiCommands(bodies()).count >= 3 }, limit: 3)
        expect(!model.ndiOutput.enabled, "an invalid name turns NDI off")
        expect(model.streamingDesired, "and never stops the stream (NdiFailureTurnsOffNdiOnlyAndNeverStopsTheStream)")
        expectEqual(ndiCommands(bodies()).last?["destinations"] as? [String], ["rtmp"], "the stream continues alone")
        expectEqual(model.ndiOutputStatus,
                    "Unavailable \u{00B7} Configure an NDI program name before turning on NDI output.",
                    "the reason is shown")
    }

    @MainActor
    static func testNdiIsNotStreaming() {
        let (model, _) = ndiModel()
        model.coreHasNdiOutput = true
        model.setNdiOutput(true)
        model.applySnapshotForTesting(["outputSenderSession": ["status": "live", "activeSenderCount": 1,
                                                                "senders": [ndiSender("live")]]])
        expect(!model.streamingDesired, "NDI live is not streaming intent")
        expect(model.destinationRows.values.allSatisfy { $0.label != "Live" },
               "no stream destination row reads Live from an NDI sender")
        expect(model.ndiOutputStatus.hasPrefix("Live"), "while the NDI tile does")
    }

    /// Review finding A: Windows OnNdiOutputOptionChanged saves and returns while off.
    @MainActor
    static func testNdiRenameWhileOffSendsNothing() {
        let (model, bodies) = ndiModel()
        model.coreHasNdiOutput = true
        model.commitNdiSettings(programName: "Studio B", groupName: "studio")
        spin(until: { false }, limit: 0.3)
        expect(bodies().isEmpty, "an edit while NDI is off sends no output command (no stray stop)")
        expectEqual(model.ndiOutput.programName, "Studio B", "the edit is kept")
        // With a live stream it must not re-send the stream's full state either.
        let (streaming, streamBodies) = ndiModel()
        streaming.streamKey = "rtmp-key-1"
        streaming.toggleStreaming()
        spin(until: { !streamBodies().isEmpty }, limit: 3)
        streaming.commitNdiSettings(programName: "Studio B", groupName: "public")
        spin(until: { false }, limit: 0.3)
        expectEqual(ndiCommands(streamBodies()).count, 1, "only the stream start; the NDI edit sent nothing")
    }

    @MainActor
    static func testNdiRenameWhileOnUpdatesTheSource() {
        let (model, bodies) = ndiModel()
        model.coreHasNdiOutput = true
        model.ndiOutput.programName = "Studio A"
        model.setNdiOutput(true)
        spin(until: { !bodies().isEmpty }, limit: 3)
        model.commitNdiSettings(programName: "Studio B", groupName: "studio")
        spin(until: { ndiCommands(bodies()).count >= 2 }, limit: 3)
        let row = (ndiCommands(bodies()).last?["destinationSettings"] as? [JSONObject])?.first
        expectEqual(row?["ndiName"] as? String, "Studio B", "the live source takes the new name")
        expectEqual(row?["ndiGroup"] as? String, "studio", "and the new group")
        expect(model.ndiOutput.enabled, "a valid rename keeps NDI on")
    }

    /// Record starts beside NDI and stops without touching it.
    @MainActor
    static func testNdiRecordStartAndStopKeepNdi() {
        let (model, bodies) = ndiModel()
        model.coreHasNdiOutput = true
        model.setNdiOutput(true)
        spin(until: { !bodies().isEmpty }, limit: 3)
        model.toggleRecording()                          // start
        spin(until: { ndiCommands(bodies()).contains { $0["type"] as? String == "set-recording-targets" } }, limit: 3)
        let start = ndiCommands(bodies()).last { $0["type"] as? String == "start-program-output" }
        expectEqual(start?["destinations"] as? [String], ["recording", "ndi"], "Record joins NDI in one full-state start")
        let before = ndiCommands(bodies()).count
        model.toggleRecording()                          // stop
        spin(until: { ndiCommands(bodies()).count > before }, limit: 3)
        let after = ndiCommands(bodies()).dropFirst(before).map { $0["type"] as? String ?? "" }
        expectEqual(after, ["stop-recording-session"], "the Record stop is only stop-recording-session")
        expect(!ndiCommands(bodies()).contains { $0["type"] as? String == "stop-encoder-session" },
               "never stop-encoder-session while NDI is on")
    }

    @MainActor
    static func testNdiNeverAsksOnQuit() {
        let (model, _) = ndiModel()
        model.coreHasNdiOutput = true
        model.setNdiOutput(true)
        model.applySnapshotForTesting(["outputSenderSession": ["status": "live", "activeSenderCount": 1,
                                                                "senders": [ndiSender("live")]]])
        expect(model.ndiOutputStatus.hasPrefix("Live"), "precondition: NDI reads live")
        expect(!QuitGuardPolicy.shouldAsk(model.quitOutputs), "NDI alone never prompts on quit")
        expect(!QuitGuardPolicy.streamLive(model.quitOutputs), "NDI is not a live stream to the quit guard")
    }

    @MainActor
    static func testNdiPressureIsNotStreamHealth() {
        let pressured: JSONObject = ["destination": "ndi", "status": "live",
                                     "lifecycle": ["state": "producing"],
                                     "backpressure": ["appliedDivisor": 2, "level": 1]]
        var rtmp = pressured
        rtmp["destination"] = "rtmp"
        let snapshot: JSONObject = ["outputSenderSession": ["senders": [pressured, rtmp]]]
        let ids = StudioHealthReadout.pressuredSenders(snapshot).map { $0["destination"] as? String ?? "" }
        expectEqual(ids, ["rtmp"], "a pressured NDI sender never reads as stream backpressure")
    }

    @MainActor
    static func testNdiFailedUpdateIsLoud() {
        let model = AppModel()
        struct Refused: Error, LocalizedError { var errorDescription: String? { "core busy" } }
        model.quitRequestForTesting = { _, _ in throw Refused() }
        model.coreHasNdiOutput = true
        model.setNdiOutput(true)
        spin(until: { model.warnings.contains { $0.contains("NDI output update failed: core busy") } }, limit: 3)
        expect(model.warnings.contains { $0.contains("NDI output update failed: core busy") },
               "a failed NDI update says so")
        expect(model.ndiOutput.enabled, "NDI stays on (Windows), and the next core generation re-arms it")
    }

    static func testNdiPrefsDefaultOffAndAdditive() {
        let empty = try? JSONDecoder().decode(NdiOutputSettings.self, from: Data("{}".utf8))
        expectEqual(empty, NdiOutputSettings(), "a missing key keeps its default (additive prefs)")
        expect(NdiOutputSettings().enabled == false, "default off")
        let partial = try? JSONDecoder().decode(NdiOutputSettings.self, from: Data(#"{"enabled":true}"#.utf8))
        expectEqual(partial?.programName, "CoreVideo Pro Program", "a partial record keeps the default name")
        expectEqual(partial?.groupName, "public", "and the default group")
    }
}
