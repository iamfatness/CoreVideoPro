// Shell test suite — run with COREVIDEO_SHELL_TESTS=1.
//
// Why it lives in the app binary rather than an XCTest target: this machine has
// Command Line Tools only, no Xcode, so `swift test` cannot find XCTest. A suite
// that runs in CI but not on the owner's machine is worse than none — the bugs
// get found on the rig. This mirrors the C++ side, where the suite is a plain
// executable that prints "511 tests passed", and extends the existing
// COREVIDEO_SHELL_SELFCHECK pattern. It also gets internal access for free, so
// nothing has to be made public just to be tested.
//
// The suite is deliberately biased toward code that HAS BROKEN in production
// rather than toward coverage percentage. Every case below maps to a real defect
// that reached the owner in a live session or shipped to main.

import Foundation

enum ShellTests {
    // ── tiny harness ─────────────────────────────────────────────────────────
    private static var failures: [String] = []
    private static var checks = 0

    private static func testMonitorControlProjection() {
        var projection = MonitorControlProjection()
        expect(!projection.legacyCoreConfirmed, "timeout cannot authorize an unversioned write")
        projection.observe(["monitorEnabled": false, "monitorVolume": 0.5])
        expect(projection.legacyCoreConfirmed, "successful old-core snapshot permits legacy fallback")
        func mix(_ revision: Int, _ enabled: Bool, _ result: JSONObject? = nil) -> JSONObject {
            ["monitorEnabled": enabled, "monitorVolume": 0.5,
             "monitorControl": ["authorityEpoch": "core-1", "revision": revision,
                                "recentResults": result.map { [$0] } ?? []] as JSONObject]
        }
        projection.observe(mix(0, false))
        expect(!projection.legacyCoreConfirmed, "new-core snapshot revokes fallback")
        var alreadyApplied = projection
        alreadyApplied.edit(.init(enabled: false, volume: 0.5))
        alreadyApplied.observe(mix(0, false))
        expect(alreadyApplied.draft == nil, "matching snapshot clears unsent draft")
        expectEqual(alreadyApplied.notice, "", "matching snapshot clears pending label")
        projection.edit(.init(enabled: true, volume: 0.5))
        projection.observe(mix(1, false))
        let first = projection.nextCommand()
        let operation = first?["operationId"] as? String ?? ""
        expectEqual(first?["expectedRevision"] as? Int64, 0, "first editor uses observed revision")
        expect(projection.nextCommand() == nil, "pending edit cannot be sent twice")
        projection.observe(mix(1, true, ["operationId": operation, "status": "applied"]))
        expect(projection.draft == nil, "applied snapshot clears draft")
        projection.edit(.init(enabled: false, volume: 0.5))
        let second = projection.nextCommand()
        expectEqual(second?["expectedRevision"] as? Int64, 1, "second editor rebases on applied revision")
        projection.observe(mix(0, false))
        expectEqual(projection.revision, 1, "stale snapshot cannot rewind revision")
        projection.observe(mix(1, true, ["operationId": second?["operationId"] as? String ?? "",
                                          "status": "conflict"]))
        expect(projection.draft != nil, "conflict retains local draft")
        expect(projection.notice.contains("conflicted"), "conflict is visible")
        projection.resetForProcess()
        expectEqual(projection.authorityEpoch, "", "restart waits for a new core epoch")
        expect(!projection.legacyCoreConfirmed, "restart revokes old-core fallback")
        projection.observe(["monitorEnabled": true, "monitorVolume": 0.5,
                            "monitorControl": ["authorityEpoch": "core-2", "revision": 0]])
        expect(!projection.observe(mix(8, false)),
               "retired monitor epoch cannot overwrite restarted core")
        let afterRestart = projection.nextCommand()
        expectEqual(afterRestart?["authorityEpoch"] as? String, "core-2",
                    "held edit rebases on restarted core")
    }

    private static func testAudioRouteControlProjection() {
        var projection = AudioRouteControlProjection()
        func matrix(_ epoch: String, _ revision: Int, _ gain: Double?,
                    _ result: JSONObject? = nil) -> JSONObject {
            let sends: [JSONObject] = gain.map { [["sourceId": "zoom:42", "busId": "mon", "gainDb": $0]] } ?? []
            return ["sends": sends,
                    "control": ["authorityEpoch": epoch, "revision": revision,
                                "recentResults": result.map { [$0] } ?? []] as JSONObject]
        }
        projection.observe(matrix("core-1", 0, nil))
        projection.edit(source: "zoom:42", bus: "mon", enabled: true, gainDb: -6)
        let first = projection.nextCommand()
        let firstId = first?["operationId"] as? String ?? ""
        expectEqual(first?["expectedRevision"] as? Int64, 0, "route edit uses observed revision")
        expect(projection.nextCommand() == nil, "pending route cannot submit twice")
        // Snapshot repairs a lost acknowledgement without resending.
        projection.observe(matrix("core-1", 1, -6,
                                  ["operationId": firstId, "status": "applied"]))
        expect(projection.drafts.isEmpty, "applied route clears the draft")
        projection.edit(source: "zoom:42", bus: "mon", enabled: false, gainDb: 0)
        let second = projection.nextCommand()
        expectEqual(second?["expectedRevision"] as? Int64, 1, "second edit uses applied revision")
        projection.observe(matrix("core-1", 2, -3,
                                  ["operationId": second?["operationId"] as? String ?? "",
                                   "status": "conflict"]))
        expect(!projection.drafts.isEmpty, "conflict keeps local draft")
        expect(projection.notice.contains("conflicted"), "conflict is visible")
        expect(projection.nextCommand() == nil, "conflicted draft waits for operator revision")
        projection.observe(matrix("core-1", 1, -6))
        expectEqual(projection.revision, 2, "stale route snapshot cannot rewind")
        expect(!projection.observe(matrix("core-1", 1, -6)),
               "stale route snapshot cannot hydrate the visible grid")
        projection.resetForProcess()
        projection.observe(matrix("core-2", 0, nil))
        expect(!projection.observe(matrix("core-1", 9, -12)),
               "retired route epoch cannot overwrite restarted core")
        expect(projection.nextCommand() == nil, "old-epoch draft cannot overwrite restarted core")
        projection.edit(source: "zoom:42", bus: "mon", enabled: true, gainDb: -6)
        expectEqual(projection.nextCommand()?["authorityEpoch"] as? String, "core-2",
                    "reviewed route edit uses restarted epoch")
    }

    private static func testZoomRosterProjection() {
        var projection = ZoomRosterProjection()
        func roster(_ epoch: String, _ revision: Int) -> JSONObject {
            ["rosterEpoch": epoch, "rosterRevision": revision,
             "participants": [["userId": "42", "displayName": "Guest"]]]
        }
        expect(projection.observe(roster("1:1:engine", 1)), "first roster barrier applies")
        expect(projection.observe(roster("1:1:engine", 3)), "gap is repaired by full snapshot")
        expectEqual(projection.gaps, 1, "missing roster revision counted")
        expect(projection.isRetiredOrOlder(roster("1:1:engine", 2)),
               "late roster cannot repaint other Zoom status")
        expect(!projection.observe(roster("1:1:engine", 2)), "old roster cannot repaint shell")
        expect(projection.observe(roster("1:2:engine", 1)), "new meeting epoch applies")
        expect(!projection.observe(roster("1:1:engine", 4)), "retired meeting rejected")
        projection.resetForProcess()
        expect(!projection.observe(roster("1:2:engine", 5)), "retired process response rejected")
        expect(projection.observe(roster("2:1:engine", 1)), "new process barrier applies")
        expect(projection.notice.isEmpty, "fresh roster clears reconciliation notice")
    }

    private static func expect(_ condition: Bool, _ what: String,
                               _ file: StaticString = #file, _ line: UInt = #line) {
        checks += 1
        if !condition {
            failures.append("\(what)  (\(URL(fileURLWithPath: "\(file)").lastPathComponent):\(line))")
        }
    }

    private static func expectEqual<T: Equatable>(_ actual: T, _ expected: T, _ what: String,
                                                  _ file: StaticString = #file,
                                                  _ line: UInt = #line) {
        checks += 1
        if actual != expected {
            failures.append("\(what): expected \(expected), got \(actual)  "
                            + "(\(URL(fileURLWithPath: "\(file)").lastPathComponent):\(line))")
        }
    }

    // ── prefs: the data-loss class ───────────────────────────────────────────

    private static func testRecordingCommandRetriesAndSupersession() {
        var policy = RecordingCommandPolicy()
        policy.observe("recording")
        let stop = policy.begin()
        expect(stop.stop, "observed live recording selects Stop")
        expect(!policy.desired, "pending Stop expresses stopped intent")
        policy.observe("recording")
        expect(!policy.desired, "live polls do not undo pending Stop intent")
        expect(policy.finish(stop, failed: true), "failed Stop owns its completion")
        expect(!policy.desired, "failed Stop preserves stopped intent despite observed media")
        policy.observe("recording")
        expect(!policy.desired, "live poll after failed Stop cannot re-arm recording")
        let retry = policy.begin()
        expect(retry.stop, "retry of failed Stop sends Stop, matching Stop Rec label")
        policy.observe("finalizing")
        policy.finish(retry, failed: true)
        expect(!policy.desired, "lost Stop reply after observed finalization stays stopped")

        policy.observe("completed")
        let firstStart = policy.begin()
        expect(!firstStart.stop, "completed recording selects Start")
        let cancelStart = policy.begin()
        expect(cancelStart.stop, "pending Start can be cancelled")
        let newestStart = policy.begin()
        expect(!newestStart.stop, "new Start after cancellation expresses fresh intent")
        expect(!policy.finish(firstStart, failed: true), "old Start failure is ignored")
        expect(!policy.finish(cancelStart, failed: false), "old Stop success is ignored")
        expect(policy.desired, "old completions cannot clear newest Start intent")
        policy.interrupted()
        expect(!policy.finish(newestStart, failed: true), "process exit invalidates pending completion")
        expect(!policy.desired, "process exit clears recording intent")

        policy.observe("idle")
        let failedStart = policy.begin()
        policy.finish(failedStart, failed: true)
        expect(!policy.desired, "failed Start while idle permits retrying Start")
        let acceptedStart = policy.begin()
        policy.observe("recording")
        policy.finish(acceptedStart, failed: true)
        expect(policy.desired, "lost Start reply preserves observed recording")

        policy.observe("completed")
        let acknowledgedStart = policy.begin()
        policy.finish(acknowledgedStart, failed: false)
        policy.observe("idle")
        expect(policy.desired, "old idle poll cannot undo acknowledged Start")
        policy.observe("completed")
        expect(policy.desired, "old completed poll cannot undo acknowledged Start")
        policy.observe("starting")
        expect(policy.desired, "fresh starting progress keeps Start intent")
        let stopWhileStarting = policy.begin()
        policy.finish(stopWhileStarting, failed: true)
        expect(!policy.desired, "failed Stop while starting does not re-arm unproven media")
        policy.observe("starting")
        expect(!policy.desired, "starting polls cannot re-arm stopped intent")
        policy.observe("recording")
        expect(policy.begin().stop, "observed media still permits explicit Stop retry")

        var stopped = RecordingCommandPolicy()
        stopped.observe("recording")
        let acknowledgedStop = stopped.begin()
        stopped.finish(acknowledgedStop, failed: false)
        stopped.observe("recording")
        expect(!stopped.desired, "stale live poll after acknowledged Stop cannot re-arm recording")
        stopped.observe("warning")
        expect(!stopped.desired, "degraded live poll also preserves stopped intent")
        expect(stopped.begin().stop, "acknowledged Stop still offers explicit Stop while media is live")
        stopped.observe("completed")
        let restarted = stopped.begin()
        expect(!restarted.stop && stopped.desired, "fresh Start after completion clears stopped intent")
        stopped.finish(restarted, failed: false)
        stopped.observe("recording")
        expect(stopped.desired, "fresh recording after explicit Start remains armed")
    }

    // #976: a retained terminal state from the PREVIOUS take must not cancel a new Start.
    private static func testRecordingRetainedTerminalStateDoesNotCancelNewStart() {
        for terminal in ["failed", "interrupted"] {
            var policy = RecordingCommandPolicy()
            policy.observe(terminal, sessionId: "take-1")
            let start = policy.begin()
            expect(!start.stop && policy.desired, "\(terminal): Start arms intent")
            policy.observe(terminal, sessionId: "take-1")
            expect(policy.desired, "\(terminal): retained previous-take state cannot cancel pending Start")
            policy.finish(start, failed: false)
            policy.observe(terminal, sessionId: "take-1")
            expect(policy.desired, "\(terminal): retained state after ack cannot cancel the Start")
            policy.observe("starting", sessionId: "take-2")
            policy.observe("recording", sessionId: "take-2")
            expect(policy.desired, "\(terminal): new take goes live with intent intact")
            policy.observe("failed", sessionId: "take-2")
            expect(!policy.desired, "\(terminal): the NEW take's own failure still clears intent")
        }
        var missing = RecordingCommandPolicy()
        missing.observe("failed", sessionId: "take-1")
        missing.finish(missing.begin(), failed: false)
        missing.observe("failed", sessionId: nil)
        expect(!missing.desired, "missing identity is never proof of an old failure")
        var bounded = RecordingCommandPolicy()
        bounded.observe("failed", sessionId: "take-1")
        bounded.finish(bounded.begin(), failed: false)
        bounded.observe("recording", sessionId: nil)
        bounded.observe("failed", sessionId: "take-1")
        expect(!bounded.desired, "supersession ends once the new take shows progress")
        var reused = RecordingCommandPolicy()
        reused.observe("failed", sessionId: "take-1")
        reused.finish(reused.begin(), failed: false)
        reused.observe("failed", sessionId: "take-9")
        expect(!reused.desired, "a different session's failure cancels the Start")
        expectEqual(RecordingLifecycleReadModel.sessionId(
            ["lifecycle": ["sessionId": "abc"], "sessionId": "zzz"]), "abc", "lifecycle sessionId read")
        expectEqual(RecordingLifecycleReadModel.sessionId(["sessionId": "zzz"]), "zzz", "legacy sessionId read")
        expect(RecordingLifecycleReadModel.sessionId(["status": "idle"]) == nil, "no sessionId -> nil")
    }

    private static func testBridgeGenerationRejectsStaleWork() {
        var policy = BridgeGenerationPolicy()
        expect(!policy.canWrite(policy.generation), "stopped bridge rejects writes")
        let first = policy.begin()
        expect(policy.isCurrent(first), "launched child owns current generation")
        expect(!policy.canWrite(first), "unvalidated handshake cannot receive commands")
        expect(policy.acceptHandshake(first), "current handshake is accepted")
        expect(policy.canWrite(first), "validated current child accepts commands")

        let recovery = policy.invalidate(stopped: false)
        expect(!policy.isCurrent(first), "old stdout and exit callbacks are stale after exit")
        expect(!policy.canWrite(first), "queued old writes cannot cross process exit")
        expect(policy.canRelaunch(recovery), "current recovery token can relaunch")
        let second = policy.begin()
        expect(!policy.canRelaunch(recovery), "new child invalidates old relaunch timer")
        expect(!policy.acceptHandshake(first), "late old handshake cannot mark new child ready")
        expect(!policy.canWrite(second), "new child still requires its own handshake")
        expect(policy.acceptHandshake(second), "replacement handshake is independent")
        expect(!policy.canWrite(first), "old queued writes cannot enter ready replacement")
        expect(policy.canWrite(second), "fresh commands can use ready replacement")

        let pendingRecovery = policy.invalidate(stopped: false)
        let stopped = policy.invalidate(stopped: true)
        expect(!policy.canRelaunch(pendingRecovery), "stop invalidates scheduled relaunch")
        expect(!policy.canRelaunch(stopped), "stop never schedules a new process")
        expect(!policy.acceptHandshake(second), "late handshake cannot revive stopped bridge")

        let incompatible = policy.begin()
        _ = policy.invalidate(stopped: true)
        expect(policy.stopped, "rejected handshake leaves bridge stopped")
        expect(!policy.canWrite(incompatible), "rejected handshake blocks captured handles immediately")
        expect(!policy.canRelaunch(policy.generation), "incompatible protocol does not auto-restart")
    }

    private static func testSharedLifecycleContracts() {
        expectEqual(RecordingLifecycleReadModel.status(["status": "recording", "lifecycle": NSNull()]),
                    "unknown", "malformed lifecycle never falls back to legacy live status")
        for health in ["healthy", "degraded", "unknown", "failed"] {
            let lifecycle: [String: Any] = ["sessionId": "test", "desiredActive": true,
                "state": "live", "health": health, "finalized": false]
            let expected = health == "healthy" || health == "degraded" ? "recording" : health
            expectEqual(RecordingLifecycleReadModel.status(["lifecycle": lifecycle]), expected,
                        "live state requires observed healthy or degraded media")
        }
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        do {
            let lifecycleData = try Data(contentsOf: root.appendingPathComponent("contracts/lifecycle.fixtures.json"))
            let identityData = try Data(contentsOf: root.appendingPathComponent("contracts/identity.fixtures.json"))
            let lifecycleRows = try JSONSerialization.jsonObject(with: lifecycleData) as! [[String: Any]]
            let evidenceData = try Data(contentsOf: root.appendingPathComponent("contracts/evidence.fixtures.json"))
            let evidenceRows = try JSONSerialization.jsonObject(with: evidenceData) as! [[String: Any]]
            let identityRows = try JSONSerialization.jsonObject(with: identityData) as! [[String: Any]]
            let graphicsData = try Data(contentsOf: root.appendingPathComponent("contracts/graphics.fixtures.json"))
            let graphicsRows = try JSONSerialization.jsonObject(with: graphicsData) as! [[String: Any]]
            let data = try JSONSerialization.data(withJSONObject: lifecycleRows + identityRows + evidenceRows + graphicsRows)
            guard let fixtures = try JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
                expect(false, "lifecycle fixtures must be an array"); return
            }
            expect(!fixtures.isEmpty, "lifecycle fixtures are not empty")
            for fixture in fixtures {
                guard let id = fixture["id"] as? String, let contract = fixture["contract"] as? String,
                      let accepted = fixture["accepted"] as? Bool, let json = fixture["json"] as? String,
                      let payloadData = json.data(using: .utf8) else {
                    expect(false, "malformed lifecycle fixture"); continue
                }
                let value = try JSONSerialization.jsonObject(with: payloadData, options: [.fragmentsAllowed])
                let validate: ([String: Any]) -> Bool
                switch contract {
                case "GraphicsOperationIdentity": validate = validateGraphicsOperationIdentity
                case "GraphicsRevisionIdentity": validate = validateGraphicsRevisionIdentity
                case "GraphicsPreparationFact": validate = validateGraphicsPreparationFact
                case "GraphicsCompositionFact": validate = validateGraphicsCompositionFact
                case "ProtocolVersion": validate = validateProtocolVersion
                case "OutputLifecycle": validate = validateOutputLifecycle
                case "OperationStatus": validate = validateOperationStatus
                case "ProtocolFailure": validate = validateProtocolFailure
                case "EntityIdentity": validate = validateEntityIdentity
                case "EntityRevision": validate = validateEntityRevision
                case "SourceInstanceIdentity": validate = validateSourceInstanceIdentity
                case "ParticipantBindingIdentity": validate = validateParticipantBindingIdentity
                case "ControlRevision": validate = validateControlRevision
                case "PlanGeneration": validate = validatePlanGeneration
                case "ControlOperationIdentity": validate = validateControlOperationIdentity
                case "ZoomRosterSnapshotRevision": validate = validateZoomRosterSnapshotRevision
                case "ZoomRosterParticipantFact": validate = validateZoomRosterParticipantFact
                case "AcceptedOperationObservation": validate = validateAcceptedOperationObservation
                case "AppliedOperationObservation": validate = validateAppliedOperationObservation
                case "RenderedMediaObservation": validate = validateRenderedMediaObservation
                case "DeliveredMediaObservation": validate = validateDeliveredMediaObservation
                case "PresentedMediaObservation": validate = validatePresentedMediaObservation
                case "MuxedMediaObservation": validate = validateMuxedMediaObservation
                case "CommittedMediaObservation": validate = validateCommittedMediaObservation
                case "CompletedOutputObservation": validate = validateCompletedOutputObservation
                case "ResourceLeaseDescriptor": validate = validateResourceLeaseDescriptor
                case "DestinationProgress": validate = validateDestinationProgress
                case "DeliveryEvidenceObservation": validate = validateDeliveryEvidenceObservation
                case "ArtifactValidationResult": validate = validateArtifactValidationResult
                default: expect(false, "unknown contract \(contract)"); continue
                }
                expectEqual((value as? [String: Any]).map(validate) ?? false, accepted, id)
                if accepted {
                    let encoded: Data
                    switch contract {
                    case "GraphicsOperationIdentity": encoded = try JSONEncoder().encode(JSONDecoder().decode(GraphicsOperationIdentity.self, from: payloadData))
                    case "GraphicsRevisionIdentity": encoded = try JSONEncoder().encode(JSONDecoder().decode(GraphicsRevisionIdentity.self, from: payloadData))
                    case "GraphicsPreparationFact": encoded = try JSONEncoder().encode(JSONDecoder().decode(GraphicsPreparationFact.self, from: payloadData))
                    case "GraphicsCompositionFact": encoded = try JSONEncoder().encode(JSONDecoder().decode(GraphicsCompositionFact.self, from: payloadData))
                    case "ProtocolVersion": encoded = try JSONEncoder().encode(JSONDecoder().decode(ProtocolVersion.self, from: payloadData))
                    case "OutputLifecycle": encoded = try JSONEncoder().encode(JSONDecoder().decode(OutputLifecycle.self, from: payloadData))
                    case "OperationStatus": encoded = try JSONEncoder().encode(JSONDecoder().decode(OperationStatus.self, from: payloadData))
                    case "ProtocolFailure": encoded = try JSONEncoder().encode(JSONDecoder().decode(ProtocolFailure.self, from: payloadData))
                    case "EntityIdentity": encoded = try JSONEncoder().encode(JSONDecoder().decode(EntityIdentity.self, from: payloadData))
                    case "EntityRevision": encoded = try JSONEncoder().encode(JSONDecoder().decode(EntityRevision.self, from: payloadData))
                    case "SourceInstanceIdentity": encoded = try JSONEncoder().encode(JSONDecoder().decode(SourceInstanceIdentity.self, from: payloadData))
                    case "ParticipantBindingIdentity": encoded = try JSONEncoder().encode(JSONDecoder().decode(ParticipantBindingIdentity.self, from: payloadData))
                    case "ControlRevision": encoded = try JSONEncoder().encode(JSONDecoder().decode(ControlRevision.self, from: payloadData))
                    case "PlanGeneration": encoded = try JSONEncoder().encode(JSONDecoder().decode(PlanGeneration.self, from: payloadData))
                    case "ControlOperationIdentity": encoded = try JSONEncoder().encode(JSONDecoder().decode(ControlOperationIdentity.self, from: payloadData))
                    case "ZoomRosterSnapshotRevision": encoded = try JSONEncoder().encode(JSONDecoder().decode(ZoomRosterSnapshotRevision.self, from: payloadData))
                    case "ZoomRosterParticipantFact": encoded = try JSONEncoder().encode(JSONDecoder().decode(ZoomRosterParticipantFact.self, from: payloadData))
                    case "AcceptedOperationObservation": encoded = try JSONEncoder().encode(JSONDecoder().decode(AcceptedOperationObservation.self, from: payloadData))
                    case "AppliedOperationObservation": encoded = try JSONEncoder().encode(JSONDecoder().decode(AppliedOperationObservation.self, from: payloadData))
                    case "RenderedMediaObservation": encoded = try JSONEncoder().encode(JSONDecoder().decode(RenderedMediaObservation.self, from: payloadData))
                    case "DeliveredMediaObservation": encoded = try JSONEncoder().encode(JSONDecoder().decode(DeliveredMediaObservation.self, from: payloadData))
                    case "PresentedMediaObservation": encoded = try JSONEncoder().encode(JSONDecoder().decode(PresentedMediaObservation.self, from: payloadData))
                    case "MuxedMediaObservation": encoded = try JSONEncoder().encode(JSONDecoder().decode(MuxedMediaObservation.self, from: payloadData))
                    case "CommittedMediaObservation": encoded = try JSONEncoder().encode(JSONDecoder().decode(CommittedMediaObservation.self, from: payloadData))
                    case "CompletedOutputObservation": encoded = try JSONEncoder().encode(JSONDecoder().decode(CompletedOutputObservation.self, from: payloadData))
                    case "ResourceLeaseDescriptor": encoded = try JSONEncoder().encode(JSONDecoder().decode(ResourceLeaseDescriptor.self, from: payloadData))
                    case "DestinationProgress": encoded = try JSONEncoder().encode(JSONDecoder().decode(DestinationProgress.self, from: payloadData))
                    case "DeliveryEvidenceObservation": encoded = try JSONEncoder().encode(JSONDecoder().decode(DeliveryEvidenceObservation.self, from: payloadData))
                    case "ArtifactValidationResult": encoded = try JSONEncoder().encode(JSONDecoder().decode(ArtifactValidationResult.self, from: payloadData))
                    default: expect(false, "unknown fixture type"); continue
                    }
                    let roundTrip = try JSONSerialization.jsonObject(with: encoded) as? [String: Any]
                    expect(roundTrip.map(validate) ?? false, "\(id) round trip")
                }
            }
        } catch { expect(false, "lifecycle fixtures: \(error)") }
    }

    /// Shipping `colorGrade` as a non-optional field silently reset EVERY saved
    /// setting, because synthesized Decodable ignores property defaults and
    /// load() swallows the throw with `try?`.
    private static func testPrefsSurviveAnOlderFile() {
        let older = """
        {"version":1,"displayName":"operator","streamUrl":"rtmp://custom/live",
         "joinMeetingId":"8675309","webinar":true}
        """.data(using: .utf8)!
        guard let prefs = try? JSONDecoder().decode(ShellPrefs.self, from: older) else {
            expect(false, "a prefs file written before newer fields existed must still decode")
            return
        }
        expectEqual(prefs.displayName, "operator", "existing value survives")
        expectEqual(prefs.streamUrl, "rtmp://custom/live", "existing value survives")
        expectEqual(prefs.joinMeetingId, "8675309", "existing value survives")
        expectEqual(prefs.webinar, true, "existing value survives")
        expectEqual(prefs.colorGrade, [], "a field absent from the file takes its default")
        expectEqual(prefs.autoHoldSeconds, 4.0, "an untouched default stays correct")
    }

    /// Guards the OTHER half: a field added to the struct but forgotten in the
    /// hand-written decoder encodes fine and never restores. Caught a real one
    /// during the VST merge (`vstChannelSelections`).
    private static func testPrefsRoundTripKeepsEveryField() {
        let roundTrip = ShellPrefs.roundTripSelfCheck()
        let detail = roundTrip ?? "ok"
        expect(roundTrip == nil, "every ShellPrefs field must survive encode->decode: \(detail)")
    }

    /// A corrupt or truncated file must fall back to defaults, not crash.
    private static func testPrefsToleratesGarbage() {
        let garbage = "{not json at all".data(using: .utf8)!
        expect((try? JSONDecoder().decode(ShellPrefs.self, from: garbage)) == nil,
               "garbage must fail to decode rather than produce nonsense")
    }

    // ── roster: the empty-meeting class ──────────────────────────────────────

    /// The shell read id/name/hasVideo; both engine paths emit
    /// userId/displayName/videoOn. The roster was empty in a live meeting.
    private static func testRosterParsesTheEngineWireShape() {
        let wire: [JSONObject] = [[
            "userId": 101, "displayName": "Producer", "videoOn": true,
            "muted": false, "talking": true, "sharingScreen": true,
        ]]
        let roster = RosterParticipant.parse(wire, assignedIds: ["101"])
        expectEqual(roster.count, 1, "engine-shaped participant must parse")
        guard let p = roster.first else { return }
        expectEqual(p.id, "101", "numeric userId becomes a string id")
        expectEqual(p.name, "Producer", "displayName is the name")
        expectEqual(p.hasVideo, true, "videoOn maps to hasVideo")
        expectEqual(p.talking, true, "talking maps through")
        expectEqual(p.sharingScreen, true, "sharingScreen maps through")
        expectEqual(p.assigned, true, "assignedIds marks the slot as taken")
    }

    /// The legacy spelling must keep working — the mock/stub core emits it.
    private static func testRosterParsesTheLegacyShape() {
        let wire: [JSONObject] = [[
            "id": "guest-1", "name": "Guest", "hasVideo": true,
            "isMuted": true, "isTalking": false,
        ]]
        let roster = RosterParticipant.parse(wire)
        expectEqual(roster.count, 1, "legacy-shaped participant must parse")
        expectEqual(roster.first?.name ?? "", "Guest", "legacy name key")
        expectEqual(roster.first?.hasVideo ?? false, true, "legacy hasVideo key")
        expectEqual(roster.first?.muted ?? false, true, "legacy isMuted key")
    }

    /// A participant with no usable id is dropped, not turned into a ghost tile.
    private static func testRosterDropsEntriesWithoutAnId() {
        let roster = RosterParticipant.parse([["displayName": "No Id"], ["userId": 7]])
        expectEqual(roster.count, 1, "an entry with no id must be dropped")
        expectEqual(roster.first?.id ?? "", "7", "the valid entry survives")
    }

    /// A missing name must fall back to the id, never to an empty label — a
    /// blank tile in the multiview is indistinguishable from a broken one.
    private static func testRosterNameFallsBackToId() {
        let roster = RosterParticipant.parse([["userId": 42, "videoOn": true]])
        expectEqual(roster.first?.name ?? "", "42", "name falls back to the id")
    }

    // ── redaction: the secret-leak class ─────────────────────────────────────

    /// A support bundle gets emailed to strangers. Secrets must not survive it,
    /// and diagnostics must: over-redaction makes the bundle useless.
    private static func testRedactionRemovesSecretsAndKeepsDiagnostics() {
        let sample = """
        streamKey=NOTAREALKEY0000aaaaBBBBccccDDDDeeeeFFFF
        userZak=ZAK_aBcDeF1234567890abcdefGHIJ
        Authorization: Bearer eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIxIn0.sig
        keyframeIntervalSeconds=2 renderPlanSignature=991817
        artifact /Users/operator/Movies/CoreVideoPro/Program.mp4
        """
        let cleaned = Redactor.redactLines(sample)
        expect(!cleaned.contains("NOTAREALKEY0000aaaaBBBBccccDDDDeeeeFFFF"), "stream key must not survive")
        expect(!cleaned.contains("ZAK_aBcDeF1234567890abcdefGHIJ"), "userZak must not survive")
        expect(!cleaned.contains("eyJhbGciOiJIUzI1NiIs"), "a JWT must not survive")
        expect(cleaned.contains("keyframeIntervalSeconds=2"),
               "a benign 'key'-containing field must NOT be redacted")
        expect(cleaned.contains("renderPlanSignature=991817"),
               "a signature is diagnostics, not a secret")
        expect(cleaned.contains("Program.mp4"), "file paths must stay readable")
    }

    // ── colour grade: the dead-travel class ──────────────────────────────────

    /// The UI range must match the core's clamp, or the operator drags into a
    /// region where nothing happens.
    @MainActor
    private static func testColorGradeNeutralDetection() {
        let model = AppModel()
        expect(model.gradeIsNeutral, "a fresh model is neutral")
        model.gradeSaturation = -10
        expect(!model.gradeIsNeutral, "a non-zero axis is not neutral")
        model.resetColorGrade()
        expect(model.gradeIsNeutral, "reset returns to neutral")
    }


    // ── wire contract: the silently-discarded-command class ──────────────────

    /// JsonRpcServer discards a payload-wrapped command that arrives without the
    /// wrapper, answering "<type> requires a payload." connect-capture-device
    /// shipped exactly that way: the button did nothing and the UI still showed
    /// the device connected.
    private static func testPayloadWrappedCommandsCarryTheWrapper() {
        let connect = CoreCommands.connectCaptureDevice(deviceId: "cam-1",
                                                        outputSourceId: "cam-1")
        expectEqual(connect["type"] as? String ?? "", "connect-capture-device", "type")
        guard let payload = connect["payload"] as? JSONObject else {
            expect(false, "connect-capture-device MUST carry a payload wrapper")
            return
        }
        expectEqual(payload["deviceId"] as? String ?? "", "cam-1", "deviceId in payload")
        // Without outputSourceId the core keys frames by its own id and the
        // multiview lookup misses — the "pink tiles" failure.
        expectEqual(payload["outputSourceId"] as? String ?? "", "cam-1",
                    "outputSourceId must be forwarded")

        let disconnect = CoreCommands.disconnectCaptureDevice(deviceId: "cam-1")
        expect(disconnect["payload"] is JSONObject,
               "disconnect-capture-device MUST carry a payload wrapper")
    }

    /// The shell's idea of which commands need a wrapper is re-derived from the
    /// CORE's source, so a guard added in C++ cannot drift away from Swift.
    private static func testPayloadContractMatchesTheCoreSource() {
        guard let fromCore = CoreCommands.payloadWrappedTypesFromCoreSource() else {
            FileHandle.standardError.write(
                "  SKIPPED payload-contract cross-check (core source not alongside binary)\n"
                    .data(using: .utf8)!)
            return
        }
        let missing = fromCore.subtracting(CoreCommands.payloadWrappedTypes)
        let extra = CoreCommands.payloadWrappedTypes.subtracting(fromCore)
        expect(missing.isEmpty,
               "the core requires a payload for \(missing.sorted()) but the shell does not list it")
        expect(extra.isEmpty,
               "the shell lists \(extra.sorted()) as payload-wrapped but the core does not")
    }

    /// Root-level commands must NOT be wrapped — a payload here is just as wrong.
    private static func testRootLevelCommandsAreNotWrapped() {
        let grade = CoreCommands.setColorGrade(exposure: 1, contrast: -2,
                                               saturation: 3, temperature: -4)
        expect(grade["payload"] == nil, "set-color-grade must NOT be payload-wrapped")
        expectEqual(grade["saturation"] as? Double ?? 0, 3, "axis at the command root")
        let sync = CoreCommands.mediaCoreSync(elapsedMs: 12, commands: [grade])
        expectEqual((sync["commands"] as? [JSONObject])?.count ?? 0, 1, "sync carries commands")
    }

    /// A ZAK is only sent when OAuth produced one; an empty string must never be
    /// put on the wire in its place.
    private static func testJoinOmitsAnAbsentZak() {
        let guest = CoreCommands.zoomJoin(meetingId: "123", displayName: "Op",
                                          passcode: "", webinar: false)
        let guestPayload = guest["payload"] as? JSONObject ?? [:]
        expect(guestPayload["userZak"] == nil, "no ZAK key when OAuth returned none")
        let host = CoreCommands.zoomJoin(meetingId: "123", displayName: "Op",
                                         passcode: "", webinar: false, userZak: "ZAK")
        let hostPayload = host["payload"] as? JSONObject ?? [:]
        expectEqual(hostPayload["userZak"] as? String ?? "", "ZAK", "ZAK forwarded when present")
        expectEqual(hostPayload["meetingNumber"] as? String ?? "", "123",
                    "both meeting spellings are sent")
    }

    // ── transport framing: the everything-times-out class ────────────────────

    /// The core emits large snapshots, so a JSON object IS routinely split
    /// across stdout reads. Dropping a partial tail loses responses at random
    /// and every command looks like a timeout.
    private static func testBridgeReassemblesSplitLines() {
        var buffer: [UInt8] = []
        let whole = "{\"id\":\"a\",\"ok\":true}\n"
        let half = whole.index(whole.startIndex, offsetBy: 9)

        buffer.append(contentsOf: Array(whole[whole.startIndex..<half].utf8))
        let firstPass = MediaCoreBridge.drainCompleteLines(&buffer)
        expectEqual(firstPass.count, 0, "a partial line yields nothing yet")
        expect(!buffer.isEmpty, "the partial tail is retained for the next read")

        buffer.append(contentsOf: Array(whole[half...].utf8))
        let secondPass = MediaCoreBridge.drainCompleteLines(&buffer)
        expectEqual(secondPass.count, 1, "the completed line is delivered")
        expectEqual(secondPass.first?["id"] as? String ?? "", "a", "and parses correctly")
        expectEqual(buffer.count, 0, "buffer drains once consumed")
    }

    /// Several objects can arrive in one read; all must be delivered in order.
    private static func testBridgeDeliversMultipleLinesFromOneRead() {
        var buffer = Array("{\"id\":\"1\"}\n{\"id\":\"2\"}\n{\"id\":\"3\"}\n".utf8)
        let objects = MediaCoreBridge.drainCompleteLines(&buffer)
        expectEqual(objects.count, 3, "all three objects delivered")
        expectEqual(objects.map { $0["id"] as? String ?? "" }, ["1", "2", "3"], "in order")
    }

    /// One malformed event must not poison the stream — the session has to
    /// survive a bad line, not go silent.
    private static func testBridgeSkipsMalformedLinesWithoutStalling() {
        var buffer = Array("{oops\n{\"id\":\"good\"}\n".utf8)
        let objects = MediaCoreBridge.drainCompleteLines(&buffer)
        expectEqual(objects.count, 1, "the malformed line is skipped")
        expectEqual(objects.first?["id"] as? String ?? "", "good", "the good line still arrives")
    }


    // ── coalescing: the RPC-flood class ──────────────────────────────────────

    /// A drag emits a change per frame. Sending one request per delta held the
    /// core lock ~100% of wall time and every shell command timed out — joins,
    /// scene syncs, assigns. Debouncing is not a nicety here, so assert it
    /// rather than trusting that the pattern was copied correctly (it has now
    /// been written three times: scenes, colour grade, lower thirds).
    @MainActor
    private static func testColorGradePushesAreCoalesced() {
        let model = AppModel()
        let before = model.colorGradePushCount
        for step in 0..<20 {                      // a fast drag
            model.gradeSaturation = Double(step) * 0.1
            model.applyColorGrade()
        }
        expectEqual(model.colorGradePushCount, before,
                    "nothing is sent while the gesture is still moving")
        // Spin the main run loop past the debounce window so the surviving task
        // can fire (Task.sleep needs the loop to turn).
        RunLoop.current.run(until: Date().addingTimeInterval(0.4))
        expectEqual(model.colorGradePushCount, before + 1,
                    "20 rapid changes collapse into exactly ONE push")
    }


    // ── OAuth: the signs-you-out-of-OBS class ────────────────────────────────

    /// Zoom ROTATES refresh tokens on use. A refresh response that omits
    /// `refresh_token` means "no rotation, keep what you have" — writing an
    /// empty string over the stored one leaves nothing to present on the next
    /// refresh and signs the account out. The owner's OBS plugin authenticates
    /// the SAME Zoom account, so a bad write here logs them out of a tool they
    /// use in production.
    private static func testRefreshWithoutRotationKeepsTheStoredToken() {
        let merged = ZoomOAuth.mergeTokenResponse(
            ["access_token": "new-access", "expires_in": NSNumber(value: 3600)],
            existingRefreshToken: "STORED-REFRESH", now: 1_000_000)
        expectEqual(merged.accessToken, "new-access", "the new access token is taken")
        expectEqual(merged.refreshToken, "STORED-REFRESH",
                    "a response with no rotation must PRESERVE the stored refresh token")
    }

    /// When Zoom DOES rotate, the new token must replace the old one — keeping
    /// a stale refresh token is the same failure from the other direction.
    private static func testRefreshWithRotationTakesTheNewToken() {
        let merged = ZoomOAuth.mergeTokenResponse(
            ["access_token": "a", "refresh_token": "ROTATED", "expires_in": NSNumber(value: 3600)],
            existingRefreshToken: "STORED-REFRESH", now: 1_000_000)
        expectEqual(merged.refreshToken, "ROTATED", "a rotated token replaces the stored one")
    }

    /// An empty response must not fabricate a token, and must still not destroy
    /// the stored one.
    private static func testEmptyResponseDoesNotDestroyTheStoredToken() {
        let merged = ZoomOAuth.mergeTokenResponse([:], existingRefreshToken: "STORED",
                                                  now: 1_000_000)
        expectEqual(merged.refreshToken, "STORED", "an empty response preserves the refresh token")
        expectEqual(merged.accessToken, "", "but does not invent an access token")
    }

    /// Expiry carries 60s of slack: a token that expires mid-join is a failed
    /// show, and the default must be conservative when the broker omits it.
    private static func testExpiryLeavesSlackBeforeTheRealDeadline() {
        let merged = ZoomOAuth.mergeTokenResponse(
            ["access_token": "a", "expires_in": NSNumber(value: 3600)],
            existingRefreshToken: "r", now: 1_000_000)
        expectEqual(merged.expiresAt, 1_000_000 + 3600 - 60, "expiry is shortened by 60s")
        let noExpiry = ZoomOAuth.mergeTokenResponse(["access_token": "a"],
                                                    existingRefreshToken: "r", now: 1_000_000)
        expectEqual(noExpiry.expiresAt, 1_000_000 + 3600 - 60,
                    "a missing expires_in falls back to one hour, still with slack")
    }

    /// HARD RULE: this app owns its own Keychain item. Reading or writing the
    /// OBS plugin's item makes a second reader of a rotating token and signs the
    /// plugin out.
    private static func testKeychainServiceIsNeverTheObsPluginItem() {
        expectEqual(ZoomOAuth.service, "us.iamfatness.corevideopro.zoom-oauth",
                    "this app uses its OWN Keychain service")
        expect(!ZoomOAuth.service.contains("OBS"),
               "the OBS plugin's Keychain item must never be touched")
    }

    /// The broker pins the app return URI; changing the scheme 400s every sign-in.
    private static func testOAuthReturnUriMatchesTheBrokerAllowlist() {
        expectEqual(ZoomOAuth.appReturnUri, "corevideo://oauth/callback",
                    "the broker allowlists exactly this return uri")
    }


    // ── encoder policy: the legal-exposure class ─────────────────────────────

    /// The shell must never offer a codec this build cannot legally or
    /// technically encode. HEVC sits under multiple patent pools that charge
    /// encoder royalties, and the GPL software encoders (x264/x265) cannot ship
    /// in a proprietary product at all — so the product ships HARDWARE encoders
    /// only (NVENC on Windows, VideoToolbox on Apple), where licensing rides
    /// with the OS and silicon vendor. macOS therefore encodes H.264 and nothing
    /// else (native/src/modules/EncoderPolicy.h: "HEVC encode is not shipped on
    /// any platform"; AV1 is the non-Apple branch).
    ///
    /// A picker offering HEVC is not merely a dead control here — it invites the
    /// operator to select something the product deliberately does not license.
    @MainActor
    private static func testShellOffersOnlyEncodableCodecs() {
        let offered = Set(AppModel.supportedStreamCodecs.map(\.id))
        expect(!offered.contains("h265"),
               "HEVC must not be offered — it is royalty-encumbered and not shipped")
        expect(!offered.contains("av1"),
               "AV1 is the non-Apple branch of EncoderPolicy; macOS cannot encode it")
        expectEqual(offered, ["h264"], "macOS ships H.264 via VideoToolbox only")
        expect(offered.contains(AppModel().streamCodec),
               "the default codec must itself be one the build can encode")
    }


    // ── telemetry: the silently-degraded-recording class ─────────────────────

    /// The core has always published totalDroppedFrames; nothing in the shell
    /// read it, so a recording could be losing frames with no sign anywhere in
    /// the operator's view. The percentage is of ATTEMPTED frames so it stays
    /// meaningful whether a show ran four minutes or four hours.
    @MainActor
    private static func testDroppedFrameReadout() {
        let model = AppModel()
        expectEqual(model.droppedFramesLabel, "0", "no frames attempted reads as a plain zero")

        model.recordingFramesWritten = 1000
        model.recordingDroppedFrames = 0
        expectEqual(model.droppedFramesLabel, "0 (0.0%)", "a clean recording reads 0 (0.0%)")

        model.recordingDroppedFrames = 10          // 10 of 1010 attempted
        expectEqual(model.droppedFramesLabel, "10 (1.0%)",
                    "the percentage is of ATTEMPTED frames, not written ones")

        model.recordingFramesWritten = 0
        model.recordingDroppedFrames = 5
        expectEqual(model.droppedFramesLabel, "5 (100.0%)",
                    "every frame dropped must not divide by zero")
    }


    // ── chroma key: the wrong-colour-keys-the-wrong-thing class ──────────────

    /// The key node is sent ONLY when enabled. The core treats a chromaKey
    /// object without an explicit enabled:false as "key this", so shipping
    /// settings for a switched-off key would punch holes in a live program.
    @MainActor
    private static func testChromaKeyNodeOnlySentWhenEnabled() {
        let model = AppModel()
        var slot = ShowInputSlot(id: 1)
        slot.kind = "capture"
        slot.sourceId = "cam-1"

        slot.keyEnabled = false
        expect(model.chromaKeyNode(for: slot) == nil,
               "a disabled key must send NO node at all")

        slot.keyEnabled = true
        guard let node = model.chromaKeyNode(for: slot) else {
            expect(false, "an enabled key must produce a node")
            return
        }
        expectEqual(node["enabled"] as? Bool ?? false, true, "node is explicitly enabled")
        expectEqual(node["similarity"] as? Double ?? 0, 0.4, "default similarity")
    }

    /// A hex that fails to parse must fall back to GREEN, never to black —
    /// keying on black would key the shadows out of every source.
    @MainActor
    private static func testKeyColourParsing() {
        let green = AppModel.rgbComponents("#00ff00")
        expectEqual(green.g, 1.0, "green channel full")
        expectEqual(green.r, 0.0, "red channel empty")

        let blue = AppModel.rgbComponents("0000ff")
        expectEqual(blue.b, 1.0, "a hex without # still parses")

        let broken = AppModel.rgbComponents("nonsense")
        expectEqual(broken.g, 1.0, "an unparseable colour falls back to GREEN")
        expectEqual(broken.r, 0.0, "and never to black, which would key shadows")
    }

    // ── ZoomSourceBudget ─────────────────────────────────────────────────────

    private static func testBudgetProgramOutranksTheWall() {
        // p-pgm holds a fixed Program route but sits LAST in the wall; order must
        // come from the tier, not the slot index. Also exercises keep-first dedupe:
        // p-pgm and p-cue appear in the wall tier too and must appear ONCE, at
        // their higher tier. Invariant: cameraOn covers every pid used.
        let entries = ZoomSourceBudget.videoEntries(
            programRouted: ["p-pgm"],
            previewRouted: ["p-cue"],
            multiviewAssigned: ["p-wall1", "p-wall2", "p-pgm", "p-cue"],
            isoArmed: [],
            cameraOn: ["p-pgm", "p-cue", "p-wall1", "p-wall2"])
        expectEqual(entries.map(\.participantId),
                    ["p-pgm", "p-cue", "p-wall1", "p-wall2"],
                    "budget order is program, preview, then the wall in slot order — once each")
        expectEqual(entries[0].purpose, "program", "a fixed program route takes the program purpose")
        expectEqual(entries[1].purpose, "preview", "a fixed preview route takes the preview purpose")
        expectEqual(entries[2].purpose, "multiview", "a wall-only source takes the multiview purpose")
    }

    private static func testBudgetCameraOffSpendsNothing() {
        // 11 camera-on wall sources w1..w11 (w10/w11 exist to make the cap
        // reachable) plus a camera-OFF program route ahead of all of them.
        // Windows rule (ZoomSourceSetPolicy): camera-off never spends VIDEO
        // budget. The cap is 10 — Windows DefaultMaxVideoSubscriptions, the
        // fixture-derived constant here.
        let wall = (1...11).map { "w\($0)" }
        let entries = ZoomSourceBudget.videoEntries(
            programRouted: ["p-off"],
            previewRouted: [],
            multiviewAssigned: wall,
            isoArmed: [],
            cameraOn: Set(wall))
        expectEqual(entries.count, 10, "the video budget is exactly 10 subscriptions")
        expect(!entries.contains { $0.participantId == "p-off" },
               "a camera-off source never spends video budget")
        expect(entries.contains { $0.participantId == "w10" },
               "the slot a camera-off source would have taken goes to the next candidate")
        expect(!entries.contains { $0.participantId == "w11" },
               "the 11th camera-on candidate is over budget")
        expectEqual(ZoomSourceBudget.maxVideoSubscriptions, 10,
                    "CONFIRMATORY: documents the owner ruling; the count assertion above is the guard")
    }

    private static func testBudgetIsoTierAndDeterminism() {
        // An iso-armed-only pid enters LAST; an iso-armed pid already listed by a
        // higher tier is not repeated. Two identical calls agree — CONFIRMATORY
        // for a pure function; the real nondeterminism guard is budget/wire-shape
        // plus the pushSpine call site replacing the old Set-derived input.
        let first = ZoomSourceBudget.videoEntries(
            programRouted: ["a"], previewRouted: [], multiviewAssigned: ["b"],
            isoArmed: ["i1", "a"], cameraOn: ["a", "b", "i1"])
        expectEqual(first.map(\.participantId), ["a", "b", "i1"],
                    "iso-armed sources enter after the wall, already-listed pids once only")
        expectEqual(first[2].purpose, "iso", "an iso-only source takes the iso purpose")
        let second = ZoomSourceBudget.videoEntries(
            programRouted: ["a"], previewRouted: [], multiviewAssigned: ["b"],
            isoArmed: ["i1", "a"], cameraOn: ["a", "b", "i1"])
        expect(first == second, "CONFIRMATORY: deterministic for identical inputs")
    }

    private static func testRoutedZoomPidsReadTheRealRouteShape() {
        // Drive the REAL SceneRoute serialization (rule 10: exercise the structure,
        // not a hand-copied dictionary): a fixed zoom route, an unbound
        // active-speaker layer and a capture route — only the zoom pid survives.
        let zoom = SceneRoute(routeId: "r1", mode: "fixed", participantId: "p9",
                              captureDeviceId: nil, rect: (0, 0, 1, 1), zIndex: 0)
        let speaker = SceneRoute(routeId: "r2", mode: "active-speaker", participantId: nil,
                                 captureDeviceId: nil, rect: (0, 0, 1, 1), zIndex: 1)
        let capture = SceneRoute(routeId: "r3", mode: "capture-input", participantId: nil,
                                 captureDeviceId: "cam-1", rect: (0, 0, 1, 1), zIndex: 2)
        let pids = ZoomSourceBudget.routedZoomPids([zoom.json, speaker.json, capture.json])
        expectEqual(pids, ["p9"], "only routes carrying a participantId contribute budget pids")
    }

    @MainActor
    private static func testSpineSubscriptionWireShape() {
        let entries = [ZoomSourceBudget.Entry(participantId: "p1", purpose: "program"),
                       ZoomSourceBudget.Entry(participantId: "p2", purpose: "multiview")]
        let payloads = AppModel.spineSubscriptionPayloads(entries)
        expectEqual(payloads.count, 2, "one subscription per budget entry")
        expectEqual(payloads[0]["participantId"] as? String, "p1", "payload order is budget order")
        expectEqual(payloads[0]["priority"] as? Int, 0, "priority is the budget index")
        // participant-video is the tiered kind: the core grants 1080P to the first
        // 8 camera-on entries IN THIS ORDER (ZoomSubscriptionResolutionPolicy.h),
        // which is the entire point of the budget. Identity is purpose-free for
        // this kind, so the real purpose rides the wire without churn.
        expectEqual(payloads[0]["kind"] as? String, "participant-video",
                    "the tiered kind — 1080P eligible, budget-order granted")
        expectEqual(payloads[0]["purpose"] as? String, "program", "the entry's real purpose")
        expectEqual(payloads[1]["purpose"] as? String, "multiview", "the entry's real purpose")
        expectEqual(payloads[1]["priority"] as? Int, 1, "priority is the budget index")
    }

    // ── dropout policy (per-source, #535 slice 4a reaching Mac) ─────────────

    @MainActor
    private static func testSourcePolicyCommandsAreZoomOnlyAndOrdered() {
        // Core contract (MediaCore::setSourcePolicy, #535 slice 4a): only
        // "zoom:<pid>" may carry dropoutPolicy — a non-zoom id pushes a sticky
        // scene warning EVERY sync; values are exactly "hold"/"black"; an explicit
        // hold must be SENT (PRESENT-OR-KEEP: omission keeps the stored black).
        var zoomB = ShowInputSlot(id: 1); zoomB.kind = "zoom"; zoomB.sourceId = "20"
        zoomB.dropoutPolicy = "black"
        var zoomA = ShowInputSlot(id: 2); zoomA.kind = "zoom"; zoomA.sourceId = "11"
        zoomA.dropoutPolicy = "hold"
        var capture = ShowInputSlot(id: 3); capture.kind = "capture"; capture.sourceId = "cam"
        capture.dropoutPolicy = "black"   // unreachable via UI; the filter is the guard
        var unset = ShowInputSlot(id: 4); unset.kind = "zoom"; unset.sourceId = "30"
        _ = unset   // dropoutPolicy stays "" — must emit nothing
        let commands = AppModel.sourcePolicyCommands(slots: [zoomB, zoomA, capture, unset])
        expectEqual(commands.count, 2, "capture and policy-less slots emit nothing")
        expectEqual(commands[0]["sourceId"] as? String, "zoom:11", "ordinal order by sourceId")
        expectEqual(commands[0]["dropoutPolicy"] as? String, "hold",
                    "an explicit hold is sent — it must overwrite an earlier black")
        expectEqual(commands[1]["sourceId"] as? String, "zoom:20", "ordinal order by sourceId")
        expectEqual(commands[1]["dropoutPolicy"] as? String, "black", "black rides verbatim")
        expectEqual(commands[0]["type"] as? String, "set-source-policy", "the core's command name")
    }

    @MainActor
    private static func testSourcePoliciesFollowTheSceneGraphInTheBatch() {
        // MediaCore: loadSceneGraph clears sceneValidationWarnings_, so a policy
        // placed BEFORE load-scene-graph has any refusal warning silently wiped.
        // applyCommands runs in the order given — policies must come last.
        var slot = ShowInputSlot(id: 1); slot.kind = "zoom"; slot.sourceId = "7"
        slot.dropoutPolicy = "black"
        let batch = AppModel.appendSourcePolicies(
            to: [["type": "load-scene-graph"], ["type": "set-preview-scene"]],
            slots: [slot])
        expectEqual(batch.count, 3, "policies are appended, never interleaved")
        expectEqual(batch[0]["type"] as? String, "load-scene-graph", "scene commands keep position")
        expectEqual(batch[2]["type"] as? String, "set-source-policy",
                    "the policy command comes after every scene command")
    }

    // ── #1196 / #1070: scene editing never changes what is on air ───────────

    @MainActor
    private static func sceneEditModel() -> AppModel {
        let model = AppModel()
        model.scenes = [
            SceneDef(id: "s1", name: "Intro", layout: "single",
                     layers: [SceneLayer(id: "s1-layer-0", slotId: 2, x: 0.1, y: 0.2,
                                         width: 0.5, height: 0.6, fitMode: "fit",
                                         opacity: 0.75)],
                     backgroundAssetId: "bg-asset"),
            SceneDef(id: "s2", name: "Panel", layout: "two-up"),
            SceneDef(id: "s3", name: "Closing", layout: "pip"),
        ]
        model.programSceneId = "s2"
        model.previewSceneId = "s1"
        return model
    }

    @MainActor
    private static func testSceneRenameKeepsEveryOtherField() {
        let model = sceneEditModel()
        let before = model.scenes[0]
        let syncs = model.sceneSyncRequestCount
        expect(model.renameScene("s1", to: "  Opening  ") == nil, "rename accepted")
        let after = model.scenes[0]
        expectEqual(after.name, "Opening", "name trimmed + applied")
        expectEqual(after.backgroundAssetId, "bg-asset", "rename keeps the background")
        expectEqual(after.layers, before.layers, "rename keeps layers byte-equal")
        expectEqual(after.layout, before.layout, "rename keeps layout")
        expectEqual(after.id, before.id, "rename keeps id")
        expectEqual(model.sceneSyncRequestCount, syncs, "rename sends no scene command")
        expectEqual(model.programSceneId, "s2", "rename leaves Program alone")
    }

    @MainActor
    private static func testSceneRenameValidationIsTheWindowsPolicy() {
        let model = sceneEditModel()
        expectEqual(model.renameScene("s1", to: "   ") ?? "", "Enter a scene name.",
                    "blank refused with the Windows text")
        expectEqual(model.renameScene("s1", to: "  PANEL ") ?? "",
                    "A scene with that name already exists. Choose a different name.",
                    "case-insensitive duplicate refused with the Windows text")
        expectEqual(model.scenes[0].name, "Intro", "refused rename changes nothing")
        expect(model.renameScene("s1", to: "  INTRO  ") == nil, "own name (any case) allowed")
        expect(model.renameScene("s1", to: "主題 – Montréal") == nil, "unicode allowed")
    }

    @MainActor
    private static func testSceneDuplicateCopiesEveryField() {
        let model = sceneEditModel()
        let source = model.scenes[0]
        model.duplicateScene("s1")
        guard let copy = model.scenes.last, copy.id != "s1" else {
            expect(false, "duplicate appended a scene"); return
        }
        expectEqual(copy.backgroundAssetId, source.backgroundAssetId, "duplicate keeps background")
        expectEqual(copy.layout, source.layout, "duplicate keeps layout")
        expectEqual(copy.layers.count, source.layers.count, "duplicate keeps layer count")
        let l = copy.layers[0], o = source.layers[0]
        expect(l.slotId == o.slotId && l.x == o.x && l.y == o.y && l.width == o.width
               && l.height == o.height && l.fitMode == o.fitMode && l.opacity == o.opacity,
               "duplicate layer geometry/fit/opacity/slot equal")
        expect(copy.name != source.name, "duplicate has its own name")
        expectEqual(model.programSceneId, "s2", "duplicate leaves Program alone")
    }

    @MainActor
    private static func testSceneDeleteRefusesOnAirAndLast() {
        let model = sceneEditModel()
        let syncs = model.sceneSyncRequestCount
        expectEqual(model.deleteScene("s2") ?? "", "The on-air scene cannot be removed",
                    "Program delete refused with the Windows text")
        expect(model.scenes.contains { $0.id == "s2" }, "Program scene still exists")
        expectEqual(model.programSceneId, "s2", "Program did not switch")
        expectEqual(model.sceneSyncRequestCount, syncs, "refused delete sends no scene command")
        expectEqual(model.sceneEditMessage, "The on-air scene cannot be removed",
                    "refusal is surfaced to the UI")
        // Program == Preview: still the on-air scene.
        model.previewSceneId = "s2"
        expect(model.deleteScene("s2") != nil, "Program delete refused even when also Preview")
        // Last scene.
        let solo = AppModel()
        solo.scenes = [SceneDef(id: "only", name: "Only", layout: "single")]
        solo.programSceneId = "other"
        expectEqual(solo.deleteScene("only") ?? "", "The last scene cannot be removed",
                    "last scene refused with the Windows text")
        // A non-air scene goes; Program is untouched.
        let ok = sceneEditModel()
        expect(ok.deleteScene("s3") == nil, "off-air scene deletes")
        expectEqual(ok.scenes.map(\.id), ["s1", "s2"], "only the target removed")
        expectEqual(ok.programSceneId, "s2", "Program untouched by a delete")
        // Deleting the Preview scene empties Preview, never moves Program.
        expect(ok.deleteScene("s1") == nil, "preview scene deletes")
        expectEqual(ok.previewSceneId, "", "Preview empties")
        expectEqual(ok.programSceneId, "s2", "Program still untouched")
    }

    @MainActor
    private static func testHarnessNeverTouchesRealPrefs() {
        expect(ShellPrefs.path != ShellPrefs.realPath, "harness prefs path is not the real path")
        expect(ShellPrefs.path.hasPrefix(NSTemporaryDirectory()), "harness prefs live under the temp dir")
        let real = FileManager.default.attributesOfItemOrNil(ShellPrefs.realPath)
        let model = sceneEditModel()
        model.duplicateScene("s1")
        _ = model.deleteScene("s3")
        model.newScene(layout: "single")
        model.saveScenes()
        expect(FileManager.default.fileExists(atPath: ShellPrefs.path), "saves went to the harness path")
        let after = FileManager.default.attributesOfItemOrNil(ShellPrefs.realPath)
        expect(real == after, "real prefs file untouched by scene saves")
    }

    @MainActor
    private static func testSceneEditorNeverTargetsProgram() {
        let model = sceneEditModel()
        expectEqual(model.sceneEditTargetId ?? "<nil>", "s1", "editor targets Preview")
        model.previewSceneId = ""
        expect(model.sceneEditTargetId == nil, "empty Preview -> no edit target (not Program)")
        // The Program scene graph is identical before and after an editor-less state.
        let programBefore = model.buildRoutes(for: "s2", isProgram: true).count
        _ = model.deleteScene("s3")
        expectEqual(model.buildRoutes(for: "s2", isProgram: true).count, programBefore,
                    "Program routes unchanged")
    }


    // ── #870: meters say aging / stale when the audio observation ages ───────

    @MainActor
    private static func testAudioObservationThresholdsAreWindows() {
        // AudioObservationPolicy.cs: <=250 fresh, <=1000 aging, else stale.
        let obs = AudioObservation(sequence: 3, status: "fresh", ageMs: 0)
        let t0 = Date(timeIntervalSince1970: 1_000)
        func state(_ elapsedMs: Double) -> String {
            AudioObservationPolicy.state(obs, receivedAt: t0, now: t0.addingTimeInterval(elapsedMs / 1000))
        }
        expectEqual(state(250), "fresh", "250 ms is fresh")
        expectEqual(state(251), "aging", "251 ms is aging")
        expectEqual(state(1000), "aging", "1000 ms is aging")
        expectEqual(state(1001), "stale", "1001 ms is stale")
        // Owner age is carried, not reset by receipt.
        let owned = AudioObservation(sequence: 3, status: "fresh", ageMs: 50)
        expectEqual(AudioObservationPolicy.state(owned, receivedAt: t0,
                    now: t0.addingTimeInterval(0.19)), "fresh", "50+190 fresh")
        expectEqual(AudioObservationPolicy.state(owned, receivedAt: t0,
                    now: t0.addingTimeInterval(0.21)), "aging", "50+210 aging")
        expectEqual(AudioObservationPolicy.state(
            AudioObservation(sequence: 4, status: "stale", ageMs: 2000), receivedAt: t0, now: t0),
                    "stale", "published age past 1 s is stale at receipt")
        expectEqual(AudioObservationPolicy.state(
            AudioObservation(sequence: 0, status: "unobserved", ageMs: 0), receivedAt: t0, now: t0),
                    "unobserved", "sequence 0 is unobserved")
        expectEqual(AudioObservationPolicy.warning(
            AudioObservation(sequence: 4, status: "stale", ageMs: 2000), receivedAt: t0, now: t0) ?? "",
                    "meters stale \u{2014} last values held", "stale wording is Windows")
        expectEqual(AudioObservationPolicy.warning(
            AudioObservation(sequence: 1, status: "aging", ageMs: 500), receivedAt: t0, now: t0) ?? "",
                    "meters aging", "aging wording is Windows")
        expectEqual(AudioObservationPolicy.warning(
            AudioObservation(sequence: 0, status: "unobserved", ageMs: 0), receivedAt: t0, now: t0) ?? "",
                    "awaiting measured audio", "unobserved wording is Windows")
        expect(AudioObservationPolicy.warning(obs, receivedAt: t0, now: t0) == nil, "fresh: no warning")
    }

    @MainActor
    private static func testMeterWarningFollowsSnapshotAgeAndRecovers() {
        let model = AppModel()
        let t0 = Date(timeIntervalSince1970: 5_000)
        func snap(_ seq: Int, _ ageMs: Double, level: Int = 60) -> [String: Any] {
            ["audioObservation": ["sequence": seq, "ageMs": ageMs, "status": "fresh"],
             "audioMixSession": ["masterLevel": level]]
        }
        expect(model.meterWarning(now: t0) == nil, "legacy/no envelope: no warning")
        model.observeAudioObservation(snap(1, 20), now: t0)
        expect(model.meterWarning(now: t0) == nil, "fresh observation: levels shown clean")
        expect(model.meterWarning(now: t0.addingTimeInterval(0.5)) == "meters aging",
               "no new snapshot for 500 ms: aging")
        expect(model.meterWarning(now: t0.addingTimeInterval(2)) ==
               "meters stale \u{2014} last values held", "no new snapshot for 2 s: stale")
        model.observeAudioObservation(snap(2, 3000), now: t0.addingTimeInterval(2))
        expect(model.meterWarning(now: t0.addingTimeInterval(2)) ==
               "meters stale \u{2014} last values held", "core-published age past 1 s: stale")
        model.observeAudioObservation(snap(3, 10), now: t0.addingTimeInterval(2.1))
        expect(model.meterWarning(now: t0.addingTimeInterval(2.1)) == nil, "fresh again: recovers")
        model.observeAudioObservation(["audioMixSession": [:]], now: t0.addingTimeInterval(2.2))
        expect(model.meterWarning(now: t0.addingTimeInterval(9)) == nil, "envelope gone: no warning")
    }

    // ── #1188: output start/stop mid-take must not shrink the armed ISO set ──

    @MainActor
    private static func isoTakeModel() -> AppModel {
        let model = AppModel()
        func guest(_ id: String) -> RosterParticipant {
            RosterParticipant(id: id, name: id, hasVideo: true, muted: false,
                              talking: false, assigned: true)
        }
        model.roster = [guest("a"), guest("b"), guest("c")]
        for (slot, id) in [(1, "a"), (2, "b"), (3, "c")] {
            model.assignSlot(slot, kind: "zoom", sourceId: id, name: id)
        }
        model.toggleSlotIso(1)
        model.toggleSlotIso(2)
        return model
    }

    private static func isoIds(_ command: JSONObject) -> [String] {
        command["isoSourceIds"] as? [String] ?? ["<missing>"]
    }

    @MainActor
    private static func testIsoTakeSurvivesRosterLossAcrossStreamToggle() {
        let model = isoTakeModel()
        model.recordingDesired = true
        expectEqual(isoIds(model.startProgramOutput(recording: true, streaming: false)),
                    ["zoom:a", "zoom:b"], "take arms both ISOs")
        model.roster = []          // transient empty roster (#1183)
        expectEqual(isoIds(model.startProgramOutput(recording: true, streaming: true)),
                    ["zoom:a", "zoom:b"], "Stream ON mid-take keeps both ISOs")
        expectEqual(isoIds(model.startProgramOutput(recording: true, streaming: false)),
                    ["zoom:a", "zoom:b"], "Stream OFF mid-take keeps both ISOs")
        // Recovery must not admit a source that was never in the take.
        model.roster = [RosterParticipant(id: "c", name: "c", hasVideo: true, muted: false,
                                          talking: false, assigned: true)]
        expectEqual(isoIds(model.startProgramOutput(recording: true, streaming: true)),
                    ["zoom:a", "zoom:b"], "recovery does not change the take")
    }

    @MainActor
    private static func testIsoTakeOperatorDeselectIsAuthoritative() {
        let model = isoTakeModel()
        model.recordingDesired = true
        _ = model.startProgramOutput(recording: true, streaming: false)
        model.roster = []
        model.toggleSlotIso(1)     // operator deselects a
        expectEqual(isoIds(model.startProgramOutput(recording: true, streaming: true)),
                    ["zoom:b"], "operator deselect removes exactly that id")
        model.unassignSlot(2)      // operator unassigns b
        expectEqual(isoIds(model.startProgramOutput(recording: true, streaming: true)),
                    [], "unassign / master off empties the take")
    }

    @MainActor
    private static func testIsoNextTakeFiltersByPresenceAgain() {
        let model = isoTakeModel()
        model.recordingDesired = true
        _ = model.startProgramOutput(recording: true, streaming: false)
        model.roster = []
        model.recordingDesired = false     // take over
        expectEqual(isoIds(model.startProgramOutput(recording: false, streaming: true)),
                    [], "between takes the present-filter applies")
        model.roster = [RosterParticipant(id: "b", name: "b", hasVideo: true, muted: false,
                                          talking: false, assigned: true)]
        model.recordingDesired = true      // new take arms from presence
        expectEqual(isoIds(model.startProgramOutput(recording: true, streaming: false)),
                    ["zoom:b"], "new take arms only present sources")
    }

    @MainActor
    private static func testIsoDisplayReadDoesNotConsumeOperatorEdit() {
        let model = isoTakeModel()
        model.recordingDesired = true
        _ = model.startProgramOutput(recording: true, streaming: false)
        model.roster = [RosterParticipant(id: "c", name: "c", hasVideo: true, muted: false,
                                          talking: false, assigned: true)]
        model.toggleSlotIso(3)     // operator selects c (edit pending)
        let first = model.peekIsoSourceIds()
        expectEqual(first, ["zoom:a", "zoom:b", "zoom:c"], "peek shows the pending edit")
        expectEqual(model.peekIsoSourceIds(), first, "peek is repeatable")
        // If peek had resolved (mutated), c would now be locked into the take.
        model.roster = []
        expectEqual(isoIds(model.startProgramOutput(recording: true, streaming: false)),
                    ["zoom:a", "zoom:b"],
                    "display reads must not change what the next command sends")
    }

    private static func testRecordingIsoSelectionPort() {
        var sel = RecordingIsoSelection()
        let abc: Set<String> = ["zoom:a", "zoom:b", "zoom:c"]
        func r(_ eligible: [String], _ assigned: Set<String>? = nil) -> [String] {
            sel.resolve(recordingRequested: true, enabled: true, selected: abc,
                        eligible: eligible, assigned: assigned ?? abc)
        }
        expectEqual(r(["zoom:a", "zoom:b"]), ["zoom:a", "zoom:b"], "arm filters by presence")
        expectEqual(r([]), ["zoom:a", "zoom:b"], "passive loss keeps the take")
        expectEqual(r(["zoom:c", "zoom:b"]), ["zoom:a", "zoom:b"], "recovery does not admit c")
        expectEqual(r(["zoom:c", "zoom:b"], abc.union(["zoom:unselected"])),
                    ["zoom:a", "zoom:b"], "unrelated assignment is not an ISO edit")
        sel.noteOperatorEdit()
        expectEqual(r(["zoom:c", "zoom:b"]), ["zoom:a", "zoom:b", "zoom:c"],
                    "an operator edit re-derives from presence + take")
        // Explicit authority (ExplicitDeselectUnassignAndMasterOffRemainAuthoritative).
        var auth = RecordingIsoSelection()
        let ab: Set<String> = ["zoom:a", "zoom:b"]
        func a(_ selected: Set<String>, enabled: Bool = true, eligible: [String] = [],
               assigned: Set<String>) -> [String] {
            auth.resolve(recordingRequested: true, enabled: enabled, selected: selected,
                         eligible: eligible, assigned: assigned)
        }
        _ = a(ab, eligible: Array(ab), assigned: ab)
        expectEqual(a(["zoom:b"], assigned: ab), ["zoom:b"],
                    "smaller selected set mid-take removes the id")
        expectEqual(a(ab, assigned: []), [], "empty assigned set empties the take")
        _ = a(ab, eligible: Array(ab), assigned: ab)
        expectEqual(a(ab, enabled: false, eligible: Array(ab), assigned: ab), [],
                    "master off while recording returns empty")
        expectEqual(a(ab, assigned: ab), [], "master-off reset: no stale take, empty roster")
        var nine = RecordingIsoSelection()
        let ids = (1...9).map { "zoom:\($0)" }
        expectEqual(nine.resolve(recordingRequested: true, enabled: true, selected: Set(ids),
                                 eligible: ids, assigned: Set(ids)), Array(ids.prefix(8)),
                    "eight-writer cap")
        nine.noteOperatorEdit()
        expectEqual(nine.resolve(recordingRequested: true, enabled: true, selected: Set(ids),
                                 eligible: [ids[8]], assigned: Set(ids)), Array(ids.prefix(8)),
                    "retained sources keep their slots under the cap")
    }

    private static func testIsoPreflightWarnsLoudly() {
        // The Windows T3.7 defect mirrored: 7 ISO streams sat at framesWritten 0,
        // no file, recording.warning null. The counts are the fixture constants;
        // the copy must carry the number and the fix.
        let seven = IsoCapturePreflight.warning(zoomIsoCount: 7, captureIntended: false,
                                                rawMediaActive: false)
        expect(seven != nil, "ISOs armed with capture off must warn")
        expect(seven?.contains("7 Zoom ISO sources") == true, "the warning names the count")
        expect(seven?.contains("Turn Capture on") == true, "the warning names the fix")
        let one = IsoCapturePreflight.warning(zoomIsoCount: 1, captureIntended: false,
                                              rawMediaActive: false)
        expect(one?.contains("1 Zoom ISO source ") == true, "singular copy for one source")
        expectEqual(IsoCapturePreflight.warning(zoomIsoCount: 0, captureIntended: false,
                                                rawMediaActive: false), nil,
                    "no ISO sources, no warning")
        expectEqual(IsoCapturePreflight.warning(zoomIsoCount: 3, captureIntended: true,
                                                rawMediaActive: true), nil,
                    "capture on and observed active is healthy")
        let intentOnObservedOff = IsoCapturePreflight.warning(zoomIsoCount: 3, captureIntended: true,
                                                              rawMediaActive: false)
        expect(intentOnObservedOff != nil,
               "intent ON but observed OFF still warns — the observed state is the truth")
        expect(intentOnObservedOff?.contains("re-arm Capture") == true,
               "intent-ON/observed-OFF copy is distinct — Capture IS on, so the fix is re-arming it, not turning it on")
    }

    private static func testIsoPreflightNeverWarnsOnAnUnobservedState() {
        // Windows round-2 ruling: capture-off must be a FACT. Intent ON with no
        // snapshot ever reported (nil) is unobserved — warning here would fire on
        // every pre-join recording start.
        expectEqual(IsoCapturePreflight.warning(zoomIsoCount: 4, captureIntended: true,
                                                rawMediaActive: nil), nil,
                    "an unobserved capture state is not a warning")
        expect(IsoCapturePreflight.warning(zoomIsoCount: 4, captureIntended: false,
                                           rawMediaActive: nil) != nil,
               "intent OFF is itself observed — the shell owns the Capture toggle")
    }

    // #764: the core stopped storing/applying logoText, lowerThirdStyle,
    // captionStyle and defaultOverlayBehavior; Windows stopped sending them
    // (MediaCoreCommandBuilderTests brand-kit test) and removed the Logo text
    // box, lower-third Style combo and Caption style combo.
    private static func testBrandKitCommandCarriesOnlyWhatTheCoreApplies() {
        let cmd = AppModel.brandKitCommand(OverlaysState())
        expectEqual(cmd["type"] as? String, "set-brand-kit", "brand kit command type")
        expectEqual(Set(cmd.keys),
                    Set(["type", "name", "brandColor", "accentColor", "backgroundColor"]),
                    "set-brand-kit carries exactly the fields the core applies")
        for dead in ["logoText", "lowerThirdStyle", "captionStyle", "defaultOverlayBehavior",
                     "logoAssetId", "logoAssetName", "logoAssetPath"] {
            expect(cmd[dead] == nil, "set-brand-kit no longer sends \(dead)")
        }
    }

    private static func testOldOverlayPrefsWithRemovedFieldsStillDecode() {
        let json = """
        {"buildInMs":700,"buildOutMs":300,"motionTiming":"custom","lowerThirdStyle":"solid",
         "brandName":"Acme","logoText":"ACME","brandColor":"#112233","accentColor":"#445566",
         "backgroundColor":"#000000","defaultOverlayBehavior":"restore-last",
         "captionStyle":"boxed","captionsEnabled":true}
        """
        let decoded = try? JSONDecoder().decode(OverlaysState.self, from: Data(json.utf8))
        expect(decoded != nil, "an overlays block still carrying the removed keys decodes")
        expectEqual(decoded?.brandName, "Acme", "kept fields survive the removed keys")
        expectEqual(decoded?.buildInMs, 700, "build timing survives")
        expectEqual(decoded?.captionsEnabled, true, "captions toggle survives")
        expectEqual(decoded?.brandColor, "#112233", "brand colour survives")
        // Whole prefs file, through the real decoder.
        let full = "{\"overlays\":" + json + "}"
        let prefs = try? JSONDecoder().decode(ShellPrefs.self, from: Data(full.utf8))
        expectEqual(prefs?.overlays?.brandName, "Acme", "ShellPrefs keeps the overlays block")
    }

    // Windows ApplyBrandOverlayDefaults (StudioViewModel.cs:1060), run from
    // SetBrandKit (:1024) only: manual no-op, all-off disables every graphic,
    // anything else leaves them alone.
    @MainActor
    private static func brandDefaultsModel(_ behavior: String, phase: String)
        -> (AppModel, () -> [JSONObject]) {
        let model = AppModel()
        model.overlays.defaultOverlayBehavior = behavior
        model.lowerThirdPhase = phase
        model.lowerThirdName = "Guest"
        model.lowerThirdSettleOverrideNs = 5_000_000  // 5ms, not the real build-out wait
        var seen: [JSONObject] = []
        model.overlayCommandObserver = { seen.append($0) }
        return (model, { seen })
    }

    /// Lets the main-actor settle Task run (the harness has no async context).
    @MainActor
    private static func spin(_ seconds: Double = 0.15) {
        RunLoop.current.run(until: Date().addingTimeInterval(seconds))
    }

    private static func hiddenAcks(_ cmds: [JSONObject]) -> Int {
        cmds.filter {
            ($0["overlayId"] as? String) == "key:lower-third"
                && ($0["keyPhase"] as? String) == "hidden"
        }.count
    }

    private static func buildOuts(_ cmds: [JSONObject]) -> Int {
        cmds.filter { ($0["keyPhase"] as? String) == "building-out" }.count
    }

    private static func lowerThirdHides(_ cmds: [JSONObject]) -> Bool {
        cmds.contains {
            ($0["overlayId"] as? String) == "key:lower-third"
                && ($0["keyPhase"] as? String) == "building-out"
        }
    }

    @MainActor
    private static func testBrandDefaultAllOffRetiresTheLowerThird() {
        for phase in ["on-air", "building-in"] {
            let (model, seen) = brandDefaultsModel("all-off", phase: phase)
            model.pushBrandKitNow()
            expect(seen().contains { ($0["type"] as? String) == "set-brand-kit" },
                   "brand kit still goes out (\(phase))")
            expect(lowerThirdHides(seen()),
                   "all-off on a brand-kit apply retires the \(phase) lower third")
            spin()
            expectEqual(hiddenAcks(seen()), 1,
                        "second step: the hidden ack is sent once when nothing intervenes (\(phase))")
        }
        let (hidden, seenHidden) = brandDefaultsModel("all-off", phase: "hidden")
        hidden.pushBrandKitNow()
        expect(!lowerThirdHides(seenHidden()), "all-off with nothing up sends no retire")
    }

    @MainActor
    private static func testBrandDefaultRetireIsIdempotentAndShowWins() {
        // A second brand-kit apply inside the window (snapshot still says on-air)
        // must not start a second build-out or a second ack.
        let (twice, seenTwice) = brandDefaultsModel("all-off", phase: "on-air")
        twice.pushBrandKitNow()
        twice.pushBrandKitNow()
        expectEqual(buildOuts(seenTwice()), 1, "repeated retire sends one building-out")
        spin()
        expectEqual(hiddenAcks(seenTwice()), 1, "repeated retire sends one hidden ack")
        // After the retire completes, a later apply on a re-shown key retires again.
        twice.lowerThirdPhase = "on-air"
        twice.pushBrandKitNow()
        expectEqual(buildOuts(seenTwice()), 2, "a retire after the first completed is allowed")
        spin()

        // Show inside the window: the pending hidden ack must not follow.
        let (shown, seenShown) = brandDefaultsModel("all-off", phase: "on-air")
        shown.pushBrandKitNow()
        shown.showLowerThird()
        spin()
        expectEqual(hiddenAcks(seenShown()), 0, "Show inside the retire window cancels the hidden ack")
        expect(seenShown().contains { ($0["keyPhase"] as? String) == "building-in" },
               "the show itself went out")
    }

    @MainActor
    private static func testBrandDefaultOptionsAreWindows() {
        expectEqual(AppModel.defaultOverlayBehaviors, ["all-off", "manual"],
                    "Default overlay options match Windows BrandDefaultOverlayBehaviorOptions")
        expectEqual(OverlaysState().defaultOverlayBehavior, "all-off",
                    "default matches Windows ProductionCatalog.BrandKit (all-off)")
    }

    @MainActor
    private static func testBrandDefaultManualAndUnknownAreNoOps() {
        for behavior in ["manual", "bogus"] {
            let (model, seen) = brandDefaultsModel(behavior, phase: "on-air")
            model.pushBrandKitNow()
            expect(!lowerThirdHides(seen()), "\(behavior) leaves an enabled overlay enabled")
            expectEqual(model.lowerThirdPhase, "on-air", "\(behavior) leaves the phase alone")
        }
    }

    @MainActor
    private static func testBrandDefaultsAreNotRerunBySyncs() {
        let (model, seen) = brandDefaultsModel("all-off", phase: "on-air")
        model.setCaptionsEnabled(true)
        model.applyMotionTiming(.gentle)
        model.showLowerThird()
        expect(!lowerThirdHides(seen()),
               "captions / motion / show never apply the brand default; only a brand-kit apply does")
    }

    // ── runner ───────────────────────────────────────────────────────────────

    // ── quit guard (T1.8 parity) ─────────────────────────────────────────────

    private static func runBlocking<T: Sendable>(_ body: @escaping @Sendable () async -> T) -> T {
        let box = QuitProbeResult<T>()
        let done = DispatchSemaphore(value: 0)
        Task.detached { box.value = await body(); done.signal() }
        done.wait()
        return box.value!
    }

    private static func testQuitDecisionAsksOnlyWhenOutputsAreLive() {
        func outputs(_ recording: String = "idle", desired: Bool = false,
                     streaming: Bool = false, stream: String = "") -> QuitOutputs {
            QuitOutputs(recordingStatus: recording, recordingDesired: desired,
                        streamingDesired: streaming, streamStatus: stream)
        }
        for idle in ["idle", "completed", "failed", "interrupted"] {
            var gate = QuitGate()
            expectEqual(gate.request(outputs(idle)), .quitNow, "\(idle) recording quits immediately")
        }
        var fresh = QuitGate()
        expectEqual(fresh.request(outputs()), .quitNow, "idle app quits immediately")
        for live in ["recording", "starting", "producing", "stopping", "finalizing", "warning", "unknown"] {
            var gate = QuitGate()
            expectEqual(gate.request(outputs(live)), .ask, "\(live) recording asks first")
        }
        var intent = QuitGate()
        expectEqual(intent.request(outputs(desired: true)), .ask, "a recording intent asks even before the core reports live")
        var streaming = QuitGate()
        expectEqual(streaming.request(outputs(streaming: true)), .ask, "streaming intent asks")
        var senderLive = QuitGate()
        expectEqual(senderLive.request(outputs(stream: "live")), .ask, "a live sender asks")
        var senderGone = QuitGate()
        expectEqual(senderGone.request(outputs(stream: "failed")), .quitNow, "a failed sender does not ask")
    }

    private static func testQuitGateAllowsOnePromptAtATime() {
        let live = QuitOutputs(recordingStatus: "recording", recordingDesired: true)
        var gate = QuitGate()
        expectEqual(gate.request(live), .ask, "first quit asks")
        expectEqual(gate.request(live), .ignore, "a second quit while the dialog is open is ignored")
        expect(gate.confirm(), "confirm from the dialog proceeds")
        expectEqual(gate.request(live), .ignore, "a quit while outputs finish is ignored, never a force-exit")
        var kept = QuitGate()
        _ = kept.request(live)
        kept.cancel()
        expectEqual(kept.request(live), .ask, "Keep running re-arms the next quit")
        var stray = QuitGate()
        expect(!stray.confirm(), "confirm without a dialog does nothing")
        expect(QuitGuardPolicy.recordingTerminal("completed") && QuitGuardPolicy.recordingTerminal("failed"),
               "completed and failed are terminal")
        expect(!QuitGuardPolicy.recordingTerminal("idle") && !QuitGuardPolicy.recordingTerminal("stopping")
               && !QuitGuardPolicy.recordingTerminal("finalizing"),
               "idle, stopping and finalizing are NOT terminal evidence")
        expectEqual(QuitGuardPolicy.finishTimeout, 15, "wait bound matches Windows OutputShutdownCoordinator (15 s)")
        expectEqual(QuitGuardPolicy.coreExitGrace, 2, "core exit grace matches Windows ShutdownBudget (2 s)")
    }

    private static func testQuitFinishStopsThenWaitsForTerminalEvidence() {
        let probe = QuitProbe()
        let outcome = runBlocking {
            await QuitFlow.finishOutputs(
                stop: { _ in probe.stop(landing: true) },
                isFinished: { _ in probe.check(finishedAfter: 3) },
                timeout: 5, poll: 0.01)
        }
        expectEqual(outcome, .finished, "finishes once terminal evidence arrives")
        expectEqual(probe.order.first ?? "", "stop", "the stop is sent before any evidence is read")
        expect(probe.order.filter { $0 == "check" }.count >= 3, "it kept waiting until the terminal state")
        expectEqual(probe.stops, 1, "a stop that landed is not re-sent")
    }

    private static func testQuitFinishTimesOutLoudlyAndResendsAStopThatDidNotLand() {
        let probe = QuitProbe()
        let started = Date()
        let outcome = runBlocking {
            await QuitFlow.finishOutputs(
                stop: { _ in probe.stop(landing: false) },
                isFinished: { _ in probe.check(finishedAfter: Int.max) },
                timeout: 0.35, poll: 0.01, resendEvery: 0.1)
        }
        expectEqual(outcome, .timedOut, "never-terminal outputs end in a bounded timeout")
        expect(Date().timeIntervalSince(started) < 3, "the wait is bounded")
        expect(probe.stops >= 2, "a stop that did not land is re-sent")
        expect(!probe.order.contains("check"), "no evidence counts before a stop has landed")
    }


    private static func testBridgeAppExitClosesStdinAndGivesTheCoreAGraceBeforeTerminating() {
        // /bin/cat exits on stdin EOF, like the core (it finalizes on EOF).
        let polite = MediaCoreBridge(corePath: "/bin/cat", environmentExtras: [:])
        polite.start()
        Thread.sleep(forTimeInterval: 0.4)
        let t0 = Date()
        let exitedByItself = runBlocking { await polite.stopForAppExit(grace: 3) }
        expect(exitedByItself, "a core that honours EOF exits by itself within the grace")
        expect(Date().timeIntervalSince(t0) < 2.5, "it does not burn the whole grace when the core exits")
        // A core that ignores stdin: the grace runs out, then it is terminated.
        let script = NSTemporaryDirectory() + "cv-quit-stubborn-\(UUID().uuidString).sh"
        try? "#!/bin/sh\nexec sleep 30\n".write(toFile: script, atomically: true, encoding: .utf8)
        try? FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: script)
        defer { try? FileManager.default.removeItem(atPath: script) }
        let stubborn = MediaCoreBridge(corePath: script, environmentExtras: [:])
        stubborn.start()
        Thread.sleep(forTimeInterval: 0.4)
        let t1 = Date()
        let graceful = runBlocking { await stubborn.stopForAppExit(grace: 0.4) }
        expect(!graceful, "a core that ignores EOF is reported as terminated after the grace")
        expect(Date().timeIntervalSince(t1) < 3, "the grace is bounded")
    }


    private static func testQuitDeadlineHoldsWhenAStopRequestHangs() {
        let started = Date()
        let outcome = runBlocking {
            await QuitFlow.finishOutputs(
                stop: { _ in try? await Task.sleep(nanoseconds: 30_000_000_000); return true },
                isFinished: { _ in true },
                timeout: 0.4, poll: 0.01)
        }
        expectEqual(outcome, .timedOut, "a hung stop cannot extend the wait")
        expect(Date().timeIntervalSince(started) < 3, "one deadline covers the whole flow, hung stop included")
    }

    /// Scripted core: records the command types it is sent, answers snapshots whose
    /// recording lifecycle advances stopping -> completed after a stop.
    private final class FakeQuitCore: @unchecked Sendable {
        private let lock = NSLock()
        private var _commands: [String] = [], _polls = 0, stopped = false, _hang = false
        var completeAfterPolls = 2
        var commands: [String] { lock.lock(); defer { lock.unlock() }; return _commands }
        var hang: Bool { get { lock.lock(); defer { lock.unlock() }; return _hang }
                         set { lock.lock(); _hang = newValue; lock.unlock() } }
        func handle(_ body: [String: Any]) async throws -> [String: Any] {
            if hang { try? await Task.sleep(nanoseconds: 30_000_000_000) }
            lock.lock(); defer { lock.unlock() }
            let cmds = (body["commands"] as? [[String: Any]] ?? []).compactMap { $0["type"] as? String }
            _commands += cmds
            if cmds.contains("stop-recording-session") { stopped = true; _polls = 0 }
            var state = "producing"
            if stopped {
                _polls += 1
                state = _polls > completeAfterPolls ? "completed" : "stopping"
            }
            let lifecycle: [String: Any] = ["sessionId": "s", "desiredActive": !stopped, "state": state,
                                            "health": "healthy", "finalized": state == "completed"]
            return ["ok": true, "snapshot": ["recording": ["lifecycle": lifecycle]] as [String: Any]]
        }
    }

    @MainActor
    private static func quitModel(core: FakeQuitCore, timeout: TimeInterval = 15)
        -> (AppModel, replies: () -> [Bool]) {
        let model = AppModel()
        let box = RepliesBox()
        model.quitReplyForTesting = { box.values.append($0) }
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.quitFinishTimeout = timeout
        model.quitCoreGrace = 0.1
        model.quitPollInterval = 0.01
        model.status = .connected(renderer: "test")
        return (model, { box.values })
    }

    @MainActor
    private static func spin(until condition: () -> Bool, limit: TimeInterval) {
        let end = Date().addingTimeInterval(limit)
        while !condition(), Date() < end { RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
    }

    @MainActor
    private static func testQuitFlowStopsInOrderAndFinishesAtTheTerminalSnapshot() {
        let core = FakeQuitCore()
        let (model, replies) = quitModel(core: core, timeout: 15)
        model.recordingStatus = "recording"
        model.recordingDesired = true
        model.streamingDesired = true
        expect(model.handleQuitRequest() == .terminateLater, "live outputs defer the quit")
        expect(model.quitPromptVisible, "the confirmation is shown")
        expect(model.handleQuitRequest() == .terminateCancel, "a second quit while asking is cancelled, never forced")
        let t0 = Date()
        model.confirmQuit()
        spin(until: { !replies().isEmpty }, limit: 5)
        expectEqual(replies(), [true], "exactly one reply, to terminate")
        expect(Date().timeIntervalSince(t0) < 3, "finishes when the terminal snapshot arrives, far before the 15 s bound")
        expectEqual(core.commands.filter { $0 != "" }, ["start-program-output", "stop-recording-session"],
                    "stream stop first, then the recording stop")
        expectEqual(model.recordingStatus, "completed", "the direct poll applied the terminal snapshot")
        expect(!model.recordingDesired, "recording intent stays off after the quit stop")
        expect(!model.quitFinishing, "the finishing state is cleared")
        expect(!model.warnings.contains { $0.contains("did not finish") }, "no false timeout warning")
    }

    @MainActor
    private static func testQuitCancelRepliesFalseOnceAndSendsNothing() {
        let core = FakeQuitCore()
        let (model, replies) = quitModel(core: core)
        model.recordingStatus = "recording"
        model.recordingDesired = true
        _ = model.handleQuitRequest()
        model.cancelQuit()
        model.cancelQuit()
        expectEqual(replies(), [false], "Keep running replies false exactly once")
        expect(core.commands.isEmpty, "nothing is stopped")
        expect(model.handleQuitRequest() == .terminateLater, "the next quit asks again")
        model.cancelQuit()
    }

    @MainActor
    private static func testQuitIdleQuitsImmediately() {
        let core = FakeQuitCore()
        let (model, replies) = quitModel(core: core)
        expect(model.handleQuitRequest() == .terminateNow, "idle quits at once")
        expect(replies().isEmpty && !model.quitPromptVisible, "no prompt, no reply to await")
    }

    @MainActor
    private static func testQuitWithADeadCoreFinishesImmediately() {
        let core = FakeQuitCore()
        let (model, replies) = quitModel(core: core, timeout: 15)
        model.recordingStatus = "interrupted"
        model.recordingDesired = true
        model.quitCoreGoneForTesting = true
        _ = model.handleQuitRequest()
        let t0 = Date()
        model.confirmQuit()
        spin(until: { !replies().isEmpty }, limit: 5)
        expectEqual(replies(), [true], "a dead core still lets the quit complete")
        expect(Date().timeIntervalSince(t0) < 2, "no 15 s wait on a core that cannot write anything")
        expect(core.commands.isEmpty, "no stop is sent to a dead core")
        expect(!model.warnings.contains { $0.contains("did not finish") }, "no false timeout warning")
    }

    @MainActor
    private static func testQuitDeadlineIsHonouredWhenTheCoreHangs() {
        let core = FakeQuitCore()
        core.hang = true
        let (model, replies) = quitModel(core: core, timeout: 0.5)
        model.recordingStatus = "recording"
        model.recordingDesired = true
        _ = model.handleQuitRequest()
        let t0 = Date()
        model.confirmQuit()
        spin(until: { !replies().isEmpty }, limit: 6)
        expectEqual(replies(), [true], "a hung core cannot hold the quit forever")
        expect(Date().timeIntervalSince(t0) < 3, "the bounded wait ends near its deadline")
        expect(model.warnings.contains { $0.contains("did not finish") }, "the timeout is loud")
    }

    @MainActor
    static func runAndExit() -> Never {
        failures = []
        checks = 0

        let cases: [(String, () -> Void)] = [
            ("monitor/revisioned-projection", testMonitorControlProjection),
            ("audio-route/revisioned-projection", testAudioRouteControlProjection),
            ("zoom-roster/recovery-projection", testZoomRosterProjection),
            ("recording/command-retry", testRecordingCommandRetriesAndSupersession),
            ("recording/retained-terminal-vs-new-start", testRecordingRetainedTerminalStateDoesNotCancelNewStart),
            ("bridge/generation-lifecycle", testBridgeGenerationRejectsStaleWork),
            ("wire/shared-lifecycle-contracts", testSharedLifecycleContracts),
            ("prefs/older-file", testPrefsSurviveAnOlderFile),
            ("prefs/round-trip", testPrefsRoundTripKeepsEveryField),
            ("prefs/garbage", testPrefsToleratesGarbage),
            ("roster/engine-shape", testRosterParsesTheEngineWireShape),
            ("roster/legacy-shape", testRosterParsesTheLegacyShape),
            ("roster/no-id", testRosterDropsEntriesWithoutAnId),
            ("roster/name-fallback", testRosterNameFallsBackToId),
            ("redaction/secrets", testRedactionRemovesSecretsAndKeepsDiagnostics),
            ("grade/neutral", testColorGradeNeutralDetection),
            ("wire/payload-wrapper", testPayloadWrappedCommandsCarryTheWrapper),
            ("wire/contract-vs-core", testPayloadContractMatchesTheCoreSource),
            ("wire/root-level", testRootLevelCommandsAreNotWrapped),
            ("wire/join-zak", testJoinOmitsAnAbsentZak),
            ("bridge/split-line", testBridgeReassemblesSplitLines),
            ("bridge/multi-line", testBridgeDeliversMultipleLinesFromOneRead),
            ("bridge/malformed", testBridgeSkipsMalformedLinesWithoutStalling),
            ("coalescing/color-grade", testColorGradePushesAreCoalesced),
            ("oauth/no-rotation-keeps-token", testRefreshWithoutRotationKeepsTheStoredToken),
            ("oauth/rotation-replaces", testRefreshWithRotationTakesTheNewToken),
            ("oauth/empty-response", testEmptyResponseDoesNotDestroyTheStoredToken),
            ("oauth/expiry-slack", testExpiryLeavesSlackBeforeTheRealDeadline),
            ("oauth/keychain-identity", testKeychainServiceIsNeverTheObsPluginItem),
            ("oauth/return-uri", testOAuthReturnUriMatchesTheBrokerAllowlist),
            ("encoder/no-unlicensed-codecs", testShellOffersOnlyEncodableCodecs),
            ("telemetry/dropped-frames", testDroppedFrameReadout),
            ("chromakey/enabled-only", testChromaKeyNodeOnlySentWhenEnabled),
            ("chromakey/colour-parsing", testKeyColourParsing),
            ("budget/order", testBudgetProgramOutranksTheWall),
            ("budget/camera-off", testBudgetCameraOffSpendsNothing),
            ("budget/iso-dedupe-determinism", testBudgetIsoTierAndDeterminism),
            ("budget/route-shape", testRoutedZoomPidsReadTheRealRouteShape),
            ("budget/wire-shape", testSpineSubscriptionWireShape),
            ("dropout/zoom-only-ordered", testSourcePolicyCommandsAreZoomOnlyAndOrdered),
            ("dropout/after-scene-graph", testSourcePoliciesFollowTheSceneGraphInTheBatch),
            ("iso/take-survives-roster-loss", testIsoTakeSurvivesRosterLossAcrossStreamToggle),
            ("iso/take-operator-deselect", testIsoTakeOperatorDeselectIsAuthoritative),
            ("iso/next-take-refilters", testIsoNextTakeFiltersByPresenceAgain),
            ("iso/display-read-pure", testIsoDisplayReadDoesNotConsumeOperatorEdit),
            ("iso/selection-port", testRecordingIsoSelectionPort),
            ("iso/preflight-loud", testIsoPreflightWarnsLoudly),
            ("iso/preflight-unobserved", testIsoPreflightNeverWarnsOnAnUnobservedState),
            ("overlays/brand-kit-payload", testBrandKitCommandCarriesOnlyWhatTheCoreApplies),
            ("overlays/old-prefs-decode", testOldOverlayPrefsWithRemovedFieldsStillDecode),
            ("overlays/brand-default-all-off", testBrandDefaultAllOffRetiresTheLowerThird),
            ("overlays/brand-default-noops", testBrandDefaultManualAndUnknownAreNoOps),
            ("overlays/brand-default-retire-race", testBrandDefaultRetireIsIdempotentAndShowWins),
            ("overlays/brand-default-options", testBrandDefaultOptionsAreWindows),
            ("overlays/brand-default-not-on-sync", testBrandDefaultsAreNotRerunBySyncs),
            ("scenes/rename-keeps-fields", testSceneRenameKeepsEveryOtherField),
            ("scenes/rename-policy", testSceneRenameValidationIsTheWindowsPolicy),
            ("scenes/duplicate-copies-all", testSceneDuplicateCopiesEveryField),
            ("scenes/delete-refusals", testSceneDeleteRefusesOnAirAndLast),
            ("scenes/harness-prefs-isolated", testHarnessNeverTouchesRealPrefs),
            ("scenes/editor-never-program", testSceneEditorNeverTargetsProgram),
            ("meters/observation-thresholds", testAudioObservationThresholdsAreWindows),
            ("meters/warning-recovers", testMeterWarningFollowsSnapshotAgeAndRecovers),
            ("quit/decision", testQuitDecisionAsksOnlyWhenOutputsAreLive),
            ("quit/one-prompt", testQuitGateAllowsOnePromptAtATime),
            ("quit/stop-then-wait", testQuitFinishStopsThenWaitsForTerminalEvidence),
            ("quit/bridge-app-exit", testBridgeAppExitClosesStdinAndGivesTheCoreAGraceBeforeTerminating),
            ("quit/deadline-hung-stop", testQuitDeadlineHoldsWhenAStopRequestHangs),
            ("quit/flow-order-terminal", testQuitFlowStopsInOrderAndFinishesAtTheTerminalSnapshot),
            ("quit/cancel", testQuitCancelRepliesFalseOnceAndSendsNothing),
            ("quit/idle", testQuitIdleQuitsImmediately),
            ("quit/dead-core", testQuitWithADeadCoreFinishesImmediately),
            ("quit/core-hang-deadline", testQuitDeadlineIsHonouredWhenTheCoreHangs),
            ("quit/timeout-resend", testQuitFinishTimesOutLoudlyAndResendsAStopThatDidNotLand),
        ]
        for (name, body) in cases {
            FileHandle.standardError.write("  running \(name)\n".data(using: .utf8)!)
            body()
        }

        if failures.isEmpty {
            print("\(checks) shell checks passed, 0 failed.")
            exit(0)
        }
        for failure in failures {
            print("FAIL: \(failure)")
        }
        print("\(checks) shell checks run, \(failures.count) failed.")
        exit(1)
    }
}

private extension FileManager {
    /// Existence + mtime + size of a file, nil when absent (isolation guard).
    func attributesOfItemOrNil(_ path: String) -> String? {
        guard let a = try? attributesOfItem(atPath: path) else { return nil }
        return "\(a[.modificationDate] as? Date ?? .distantPast)|\(a[.size] as? Int ?? -1)"
    }
}

private final class QuitProbe: @unchecked Sendable {
    private let lock = NSLock()
    private var _stops = 0, _checks = 0, _order: [String] = []
    var stops: Int { lock.lock(); defer { lock.unlock() }; return _stops }
    var order: [String] { lock.lock(); defer { lock.unlock() }; return _order }
    func stop(landing: Bool) -> Bool {
        lock.lock(); defer { lock.unlock() }
        _stops += 1; _order.append("stop"); return landing
    }
    func check(finishedAfter n: Int) -> Bool {
        lock.lock(); defer { lock.unlock() }
        _checks += 1; _order.append("check")
        return _checks >= n
    }
}

private final class QuitProbeResult<T>: @unchecked Sendable { var value: T? }

private final class RepliesBox { var values: [Bool] = [] }
