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

import CameraExtensionKit
import Combine
import AppKit
import SwiftUI
import SystemExtensions
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

    static func expect(_ condition: Bool, _ what: String,
                               _ file: StaticString = #file, _ line: UInt = #line) {
        checks += 1
        if !condition {
            failures.append("\(what)  (\(URL(fileURLWithPath: "\(file)").lastPathComponent):\(line))")
        }
    }

    static func expectEqual<T: Equatable>(_ actual: T, _ expected: T, _ what: String,
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

    // ── camera extension activation policy (roadmap M9.2) ────────────────────
    private static let extDomain = "OSSystemExtensionErrorDomain"

    private static func testCameraExtensionErrorMapping() {
        func map(_ code: Int, _ canceledByUs: Bool = false) -> CameraExtensionState {
            CameraExtensionPolicy.state(forActivationError: extDomain, code: code,
                                        canceledByUs: canceledByUs)
        }
        expectEqual(map(3), .notInApplications, "code 3 = unsupported parent bundle location")
        let unsigned = CameraExtensionState.unavailable("This copy of CoreVideo Pro isn't signed to install the camera.")
        expectEqual(map(2), unsigned, "code 2 = missing entitlement")
        expectEqual(map(8), unsigned, "code 8 = invalid code signature")
        expectEqual(map(10), .unavailable("macOS policy blocked the camera extension."), "code 10 = system policy")
        expectEqual(map(11, true), .active, "code 11 we canceled (same version) = already active")
        expectEqual(map(11, false), .failed("Couldn't install the virtual camera (code 11)."),
                    "code 11 not canceled by us is a real failure")
        expectEqual(map(13), .needsApproval, "code 13 = authorization required")
        expectEqual(map(9), .failed("Couldn't install the virtual camera (code 9)."), "anything else fails with its code")
        expectEqual(CameraExtensionPolicy.state(forActivationError: "other", code: 3, canceledByUs: false),
                    .failed("Couldn't install the virtual camera (code 3)."), "a foreign domain is never mapped")
    }

    private static func testCameraExtensionPreflight() {
        let msg = "This build can't install the virtual camera (development build without a provisioning profile)."
        expectEqual(CameraExtensionPolicy.preflight(bundlePath: "/Users/x/Downloads/CoreVideo Pro.app", hasEmbeddedProfile: true),
                    .notInApplications, "Downloads is not /Applications")
        expectEqual(CameraExtensionPolicy.preflight(bundlePath: "/ApplicationsX/CoreVideo Pro.app", hasEmbeddedProfile: true),
                    .notInApplications, "/ApplicationsX is not /Applications/")
        expectEqual(CameraExtensionPolicy.preflight(bundlePath: "/Users/x/Applications/CoreVideo Pro.app", hasEmbeddedProfile: true),
                    .notInApplications, "a nested Applications folder is not /Applications/")
        expectEqual(CameraExtensionPolicy.preflight(bundlePath: "/Applications/CoreVideo Pro.app", hasEmbeddedProfile: false),
                    .unavailable(msg), "no provisioning profile")
        expect(CameraExtensionPolicy.preflight(bundlePath: "/Applications/CoreVideo Pro.app", hasEmbeddedProfile: true) == nil,
               "installed with a profile passes preflight")
    }

    private static func testCameraExtensionReplacement() {
        expectEqual(CameraExtensionPolicy.replacement(existingVersion: "100", newVersion: "100"), .keep, "same version keeps")
        expectEqual(CameraExtensionPolicy.replacement(existingVersion: "100", newVersion: "101"), .replace, "upgrade replaces")
        expectEqual(CameraExtensionPolicy.replacement(existingVersion: "101", newVersion: "100"), .replace,
                    "downgrade replaces (older install)")
    }

    private static func testCameraExtensionProperties() {
        typealias P = (enabled: Bool, awaitingApproval: Bool, uninstalling: Bool)
        let f = CameraExtensionPolicy.state(forProperties:)
        expectEqual(f([]), .notInstalled, "no entries = not installed")
        expectEqual(f([P(true, false, false)]), .active, "enabled = active")
        expectEqual(f([P(true, false, true), P(false, true, false)]), .needsApproval,
                    "enabled but uninstalling does not count; awaiting approval wins next")
        expectEqual(f([P(false, false, false), P(true, false, true)]), .notInstalled, "nothing usable")
        expectEqual(f([P(false, true, false), P(true, false, false)]), .active, "any enabled entry wins")
    }

    private static func testCameraExtensionSettingsLink() {
        let security = "x-apple.systempreferences:com.apple.preference.security?Security"
        let login = "x-apple.systempreferences:com.apple.LoginItems-Settings.extension"
        for v in [13, 14] { expectEqual(CameraExtensionPolicy.settingsURL(osMajor: v).absoluteString, security, "macOS \(v)") }
        for v in [15, 26] { expectEqual(CameraExtensionPolicy.settingsURL(osMajor: v).absoluteString, login, "macOS \(v)") }
        expectEqual(CameraExtensionPolicy.settingsInstruction(osMajor: 15),
                    "System Settings \u{203A} General \u{203A} Login Items & Extensions \u{203A} Camera Extensions", "15+ text")
        expectEqual(CameraExtensionPolicy.settingsInstruction(osMajor: 14),
                    "System Settings \u{203A} Privacy & Security, then click Allow next to the CoreVideo Pro message", "13-14 text")
    }

    private static func testCameraExtensionFakeRecordsCalls() {
        let fake = FakeCameraExtensionActivator(script: [.activating, .needsApproval, .active])
        var seen: [CameraExtensionState] = []
        fake.activate { seen.append($0) }
        expectEqual(seen, [.activating, .needsApproval, .active], "scripted states are delivered in order")
        fake.deactivate { seen.append($0) }
        fake.query { seen.append($0) }
        expectEqual(fake.calls, ["activate", "deactivate", "query"], "calls are recorded")
    }

    private static func testCameraExtensionReducerSequences() {
        typealias R = CameraExtensionRequestReducer
        func run(_ kind: R.Kind, _ events: [R.Event], replace: (String, String)? = nil) -> [CameraExtensionState] {
            var r = R(kind: kind)
            if let (e, n) = replace { _ = r.replacement(existingVersion: e, newVersion: n) }
            return events.compactMap { r.reduce($0) }
        }
        expectEqual(run(.activate, [.needsUserApproval, .finished(rebootRequired: false)]),
                    [.needsApproval, .active], "activate -> approval -> completed")
        expectEqual(run(.activate, [.finished(rebootRequired: true)]), [.needsReboot], "reboot result")
        expectEqual(run(.activate, [.finished(rebootRequired: false)], replace: ("100", "101")),
                    [.active], "activate -> replace -> completed")
        expectEqual(run(.activate, [.failed(domain: extDomain, code: 11)], replace: ("100", "100")),
                    [.active], "equal version -> cancel -> requestCanceled(11) -> active")
        expectEqual(run(.activate, [.failed(domain: extDomain, code: 11)]),
                    [.failed("Couldn't install the virtual camera (code 11).")], "11 without our cancel fails")
        expectEqual(run(.query, [.foundProperties([(true, false, false)]), .finished(rebootRequired: false)]),
                    [.active], "a query's didFinish never overwrites the properties state")
        expectEqual(run(.query, [.finished(rebootRequired: false)]), [], "a bare query finish emits nothing")
        expectEqual(run(.deactivate, [.finished(rebootRequired: false)]), [.notInstalled], "deactivate completes")
        expectEqual(run(.activate, [.finished(rebootRequired: false), .failed(domain: extDomain, code: 13)]),
                    [.active], "events after a request finished are ignored")
    }

    private static func testCameraExtensionActivatorKeepsRequestsApart() {
        var submitted: [OSSystemExtensionRequest] = []
        let a = SystemCameraExtensionActivator(bundlePath: "/Applications/CoreVideo Pro.app",
                                               hasProfile: true, submit: { submitted.append($0) })
        var act: [CameraExtensionState] = []
        var qry: [CameraExtensionState] = []
        a.activate { act.append($0) }
        a.query { qry.append($0) }
        expectEqual(submitted.count, 2, "both requests submitted")
        guard submitted.count == 2 else { return }
        let (r1, r2) = (submitted[0], submitted[1])
        a.requestNeedsUserApproval(r1)
        a.request(r2, foundProperties: [])
        a.request(r1, didFinishWithResult: .completed)
        a.request(r2, didFinishWithResult: .completed)  // late finish of the finished query
        a.request(r1, didFailWithError: NSError(domain: extDomain, code: 13))  // finished request
        expectEqual(act, [.activating, .needsApproval, .active], "activation keeps its own states")
        expectEqual(qry, [.notInstalled], "query keeps its own state, never .active")
        let stranger = OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: "x", queue: .main)
        a.requestNeedsUserApproval(stranger)
        expectEqual(act.count, 3, "an unknown request is ignored")
    }

    // ── virtual camera shell side (roadmap M9.2 Task 6) ─────────────────────

    private static func vcamCore(_ status: String, warning: String = "",
                                 width: Int = 1920, height: Int = 1080,
                                 framerState: String = "off", framerWarning: String = "") -> VirtualCameraReadModel {
        var m = VirtualCameraReadModel()
        m.enabled = true; m.status = status; m.warning = warning
        m.width = width; m.height = height
        m.framerState = framerState; m.framerWarning = framerWarning
        return m
    }

    private static func testVirtualCameraLabelPrecedence() {
        typealias P = VirtualCameraPresentation
        let live = vcamCore("live")
        // One row per precedence line.
        expectEqual(P.label(intent: false, ext: .active, core: nil), "Virtual camera off", "1 intent off")
        expectEqual(P.label(intent: true, ext: .activating, core: nil),
                    "Installing the virtual camera\u{2026}", "2 activating")
        expectEqual(P.label(intent: true, ext: .needsApproval, core: nil),
                    "Allow CoreVideo Pro Camera in System Settings to finish setup.", "3 needs approval")
        expectEqual(P.label(intent: true, ext: .notInApplications, core: nil),
                    "Move CoreVideo Pro to the Applications folder to use the virtual camera.", "4 not in /Applications")
        expectEqual(P.label(intent: true, ext: .needsReboot, core: nil),
                    "Restart your Mac to finish updating the virtual camera.", "5 needs reboot")
        expectEqual(P.label(intent: true, ext: .unavailable("U reason"), core: nil), "U reason", "6 unavailable")
        expectEqual(P.label(intent: true, ext: .failed("F reason"), core: nil), "F reason", "6 failed")
        expectEqual(P.label(intent: true, ext: .active, core: vcamCore("live", width: 1280, height: 720)),
                    "Virtual camera live (1280x720)", "7 core live")
        expectEqual(P.label(intent: true, ext: .active,
                            core: vcamCore("failed", warning: "CAMERA_EXTENSION_NOT_FOUND: no device")),
                    "Connecting to CoreVideo Pro Camera\u{2026}", "8 not found")
        expectEqual(P.label(intent: true, ext: .active,
                            core: vcamCore("failed", warning: "CAMERA_EXTENSION_LOST: sink vanished")),
                    "Connecting to CoreVideo Pro Camera\u{2026}", "8 lost")
        expectEqual(P.label(intent: true, ext: .active,
                            core: vcamCore("failed", warning: "SINK_START_FAILED: queue refused: busy")),
                    "SINK_START_FAILED: queue refused: busy", "9 other warning is shown whole (Windows)")
        expectEqual(P.label(intent: true, ext: .active,
                            core: vcamCore("failed", warning: "CAMERA_TAP_PREP_FAILED: the GPU camera tap could not be prepared; restart CoreVideo Pro.")),
                    "CAMERA_TAP_PREP_FAILED: the GPU camera tap could not be prepared; restart CoreVideo Pro.",
                    "9 a failed GPU tap preparation is shown whole, never as connecting")
        expectEqual(P.label(intent: true, ext: .active,
                            core: vcamCore("failed", warning: "CAMERA_PRODUCER_REFUSED: CoreVideo Pro Camera refused this copy of CoreVideo Pro. Install the signed app in the Applications folder (CMIODeviceStartStream -4).")),
                    "CoreVideo Pro Camera refused this copy of CoreVideo Pro. Reinstall CoreVideo Pro in the Applications folder.",
                    "9 a refused producer says what fixes it, never 'connecting' or a bare -4")
        expectEqual(P.label(intent: true, ext: .active, core: vcamCore("failed", warning: "plain words")),
                    "plain words", "9 warning without a code prefix is shown whole")
        expectEqual(P.label(intent: true, ext: .active, core: vcamCore("failed")),
                    "Virtual camera failed", "9 empty warning: the Windows core-failure wording")
        expectEqual(P.label(intent: true, ext: .active, core: vcamCore("starting")), "Virtual camera on", "10 starting")
        expectEqual(P.label(intent: true, ext: .unknown, core: nil), "Virtual camera on", "10 no snapshot yet")
        // Conflict rows.
        expectEqual(P.label(intent: false, ext: .active, core: live), "Virtual camera off",
                    "intent off beats a live core")
        expectEqual(P.label(intent: true, ext: .needsApproval, core: live),
                    "Allow CoreVideo Pro Camera in System Settings to finish setup.",
                    "needsApproval beats a live core")
        // Settings button only while approval is pending.
        expect(P.showsSettingsButton(.needsApproval), "approval shows the settings button")
        for s: CameraExtensionState in [.unknown, .notInstalled, .activating, .active, .needsReboot,
                                        .notInApplications, .unavailable("x"), .failed("y")] {
            expect(!P.showsSettingsButton(s), "no settings button for \(s)")
        }
        // Framer label ports VirtualCameraFramerStatusLabel.
        expectEqual(P.framerLabel(framerEnabled: false, intent: true, core: live), "Framer off", "framer off")
        expectEqual(P.framerLabel(framerEnabled: true, intent: false, core: live),
                    "Ready when webcam output is enabled", "framer armed, camera off")
        expectEqual(P.framerLabel(framerEnabled: true, intent: true,
                                  core: vcamCore("live", framerState: "unavailable", framerWarning: "No Metal device")),
                    "No Metal device", "framer unavailable shows its warning")
        expectEqual(P.framerLabel(framerEnabled: true, intent: true, core: vcamCore("live", framerState: "active")),
                    "OH Framer active on webcam output", "framer active")
        expectEqual(P.framerLabel(framerEnabled: true, intent: true, core: nil),
                    "Waiting for webcam frame", "framer waiting")
    }

    private static func testVirtualCameraReadModelParsesTheCoreShape() {
        // Exactly the CoreStateCapture::virtualCameraState keys the brief names.
        let node: JSONObject = [
            "enabled": true, "status": "live", "deviceName": "CoreVideo Pro Camera",
            "resolution": ["width": 1920, "height": 1080] as JSONObject, "fps": 60,
            "framesPublished": 42.0, "framerState": "active", "framerWarning": "fw",
            "warning": "w",
        ]
        guard let m = VirtualCameraReadModel.parse(["virtualCamera": node]) else {
            expect(false, "a snapshot with the node parses"); return
        }
        expect(m.enabled, "enabled read")
        expectEqual(m.status, "live", "status read")
        expectEqual(m.width, 1920, "width from resolution.width")
        expectEqual(m.height, 1080, "height from resolution.height")
        expectEqual(m.framesPublished, 42, "framesPublished read (a JSON double)")
        expectEqual(m.framerState, "active", "framerState read")
        expectEqual(m.framerWarning, "fw", "framerWarning read")
        expectEqual(m.warning, "w", "warning read")
        expect(VirtualCameraReadModel.parse(["recording": [:] as JSONObject]) == nil,
               "a snapshot without the node is nil, not a default 'off'")
    }

    private static func testSyncVirtualCameraWire() {
        let wire = CoreCommands.syncVirtualCamera(
            PersistedVirtualCamera(enabled: true, mirror: true, framerEnabled: true, deviceName: "Studio Cam"))
        expectEqual(Set(wire.keys), ["type", "on", "mirror", "framerEnabled", "deviceName"], "exact keys")
        expectEqual(wire["type"] as? String, "sync-virtual-camera", "type")
        expectEqual(wire["on"] as? Bool, true, "on")
        expectEqual(wire["mirror"] as? Bool, true, "mirror")
        expectEqual(wire["framerEnabled"] as? Bool, true, "framerEnabled")
        expectEqual(wire["deviceName"] as? String, "Studio Cam", "deviceName")
        expect(wire["payload"] == nil, "root-level, not payload-wrapped")
        let off = CoreCommands.syncVirtualCamera(PersistedVirtualCamera())
        expectEqual(off["on"] as? Bool, false, "off is sent explicitly (the core defaults on to TRUE)")
        // C11: the core ignores an empty name, so a blank field sends the default.
        expectEqual(off["deviceName"] as? String, CameraConstants.defaultDeviceName, "blank name -> default")
        let spaced = CoreCommands.syncVirtualCamera(PersistedVirtualCamera(enabled: true, deviceName: "   "))
        expectEqual(spaced["deviceName"] as? String, CameraConstants.defaultDeviceName, "whitespace -> default")
        // Cross-check the key names against MediaCore::syncVirtualCamera.
        guard let fromCore = CoreCommands.syncVirtualCameraKeysFromCoreSource() else {
            FileHandle.standardError.write(
                "  SKIPPED sync-virtual-camera core cross-check (core source not alongside binary)\n"
                    .data(using: .utf8)!)
            return
        }
        expectEqual(Set(wire.keys).subtracting(["type"]), fromCore,
                    "the shell's sync-virtual-camera keys are exactly what MediaCore::syncVirtualCamera reads")
    }

    private static func testVirtualCameraPrefsRoundTripAndOlderFile() {
        let older = #"{"joinMeetingId":"555","streamUrl":"rtmp://x/live","cameraExtensionOptedIn":true}"#
        guard let decoded = try? JSONDecoder().decode(ShellPrefs.self, from: Data(older.utf8)) else {
            expect(false, "an older prefs file without virtualCamera decodes"); return
        }
        expect(decoded.virtualCamera == nil, "absent key decodes to nil")
        expectEqual(decoded.joinMeetingId, "555", "other fields are kept")
        expectEqual(decoded.cameraExtensionOptedIn, true, "opt-in kept")
        var p = ShellPrefs()
        p.virtualCamera = PersistedVirtualCamera(enabled: true, mirror: true, framerEnabled: true, deviceName: "Cam B")
        let back = try? JSONDecoder().decode(ShellPrefs.self, from: JSONEncoder().encode(p))
        expectEqual(back?.virtualCamera, p.virtualCamera, "all four fields survive a round trip")
        expect(ShellPrefs.roundTripSelfCheck() == nil, "the self-check still round-trips every field")
    }

    @MainActor
    private static func testVirtualCameraResentOnNewCoreGeneration() {
        let model = AppModel()
        model.cameraExtension = FakeCameraExtensionActivator(script: [.active])
        var sent: [JSONObject] = []
        model.coreCommandCaptureForTesting = { sent.append($0) }
        model.setVirtualCamera(enabled: true)
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        func vcamCommands() -> [JSONObject] {
            sent.flatMap { ($0["commands"] as? [JSONObject]) ?? [] }
                .filter { $0["type"] as? String == "sync-virtual-camera" }
        }
        expectEqual(vcamCommands().count, 1, "the toggle sends one sync-virtual-camera")
        sent.removeAll()
        // Two core generations through the real bridge-status path.
        model.handleBridgeStatus(.connected(renderer: "test-gen-1"))
        model.handleBridgeStatus(.exited(code: 9))
        model.handleBridgeStatus(.connected(renderer: "test-gen-2"))
        let resent = vcamCommands()
        expectEqual(resent.count, 2, "each new core generation is re-sent the virtual camera state")
        expect(resent.allSatisfy { $0["on"] as? Bool == true }, "the resend carries the operator's intent")
        model.stopTimersForTesting()
    }

    /// Activation wiring: first enable opts in and activates; launch activates
    /// when opted in and only queries otherwise; a silent query times out.
    @MainActor
    private static func testVirtualCameraActivationWiring() {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(script: [.activating, .needsApproval])
        model.cameraExtension = fake
        model.coreCommandCaptureForTesting = { _ in }
        expect(model.cameraExtensionOptedIn == nil, "never asked by default")
        model.setVirtualCamera(enabled: true)
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(fake.calls, ["activate"], "enabling with the extension inactive activates it")
        expectEqual(model.cameraExtensionOptedIn, true, "the first enable opts in")
        expectEqual(model.cameraExtensionState, .needsApproval, "activation states reach the model")
        expectEqual(model.virtualCameraLabel, "Allow CoreVideo Pro Camera in System Settings to finish setup.",
                    "the label follows the activation state")
        model.setVirtualCameraMirror(true)
        model.setVirtualCameraFramer(true)
        model.setVirtualCameraDeviceName("  Booth  ")
        expect(model.virtualCamera.mirror && model.virtualCamera.framerEnabled, "setters update intent")

        let optedIn = AppModel()
        let fake2 = FakeCameraExtensionActivator(script: [.active])
        optedIn.cameraExtension = fake2
        optedIn.cameraExtensionOptedIn = true
        optedIn.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(fake2.calls, ["installs", "activate"],
                    "launch after an opt-in with nothing installed asks, then activates")

        let fresh = AppModel()
        let fake3 = FakeCameraExtensionActivator()
        fresh.cameraExtension = fake3
        fresh.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(fake3.calls, ["query"], "launch without an opt-in only queries")
        expectEqual(fresh.cameraExtensionState, .notInstalled, "query result reaches the model")

        final class SilentActivator: CameraExtensionActivating {
            func query(_ done: @escaping (CameraExtensionState) -> Void) {}
            func activate(_ update: @escaping (CameraExtensionState) -> Void) {}
            func deactivate(_ update: @escaping (CameraExtensionState) -> Void) {}
        }
        let silent = AppModel()
        silent.cameraExtension = SilentActivator()
        silent.cameraExtensionQueryTimeout = 0.1
        silent.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.4))
        expectEqual(silent.cameraExtensionState, .notInstalled, "a query that never answers times out to notInstalled")
    }

    /// Settings "Remove virtual camera": one deactivate, the opt-in cleared (so a
    /// relaunch does not re-install it), intent off, and the core told once.
    @MainActor
    private static func testRemoveVirtualCameraCallsDeactivateAndClearsOptIn() {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(script: [.active])
        model.cameraExtension = fake
        var sent: [JSONObject] = []
        model.coreCommandCaptureForTesting = { sent.append($0) }
        model.setVirtualCamera(enabled: true)
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(model.cameraExtensionOptedIn, true, "precondition: opted in")
        sent.removeAll()
        model.removeVirtualCamera()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(fake.calls.filter { $0 == "deactivate" }.count, 1, "exactly one deactivate call")
        expectEqual(model.cameraExtensionOptedIn, false, "the opt-in is cleared")
        expectEqual(model.virtualCamera.enabled, false, "intent is off")
        expectEqual(model.cameraExtensionState, .notInstalled, "the deactivation result reaches the model")
        let syncs = sent.flatMap { ($0["commands"] as? [JSONObject]) ?? [] }
            .filter { $0["type"] as? String == "sync-virtual-camera" }
        expectEqual(syncs.count, 1, "one sync-virtual-camera is sent")
        expectEqual(syncs.first?["on"] as? Bool, false, "and it says off")
        model.stopTimersForTesting()
    }

    /// A cancelled password or a failed deactivation leaves the extension
    /// installed: the opt-in must survive and the Settings row must say so.
    @MainActor
    private static func testRemoveVirtualCameraFailureKeepsOptInAndReports() {
        final class FailingDeactivator: CameraExtensionActivating {
            let result: CameraExtensionState
            init(_ r: CameraExtensionState) { result = r }
            func query(_ done: @escaping (CameraExtensionState) -> Void) { done(.active) }
            func activate(_ update: @escaping (CameraExtensionState) -> Void) { update(.active) }
            func deactivate(_ update: @escaping (CameraExtensionState) -> Void) { update(result) }
        }
        for (name, result) in [("cancelled", CameraExtensionState.failed("Couldn't install the virtual camera (code 4).")),
                               ("unavailable", CameraExtensionState.unavailable("macOS policy blocked the camera extension."))] {
            let model = AppModel()
            model.cameraExtension = FailingDeactivator(result)
            model.coreCommandCaptureForTesting = { _ in }
            model.setVirtualCamera(enabled: true)
            RunLoop.current.run(until: Date().addingTimeInterval(0.05))
            model.removeVirtualCamera()
            RunLoop.current.run(until: Date().addingTimeInterval(0.05))
            expectEqual(model.cameraExtensionOptedIn, true, "\(name): the opt-in is kept when removal fails")
            expectEqual(model.virtualCamera.enabled, false, "\(name): intent is still off")
            expectEqual(model.virtualCameraRemovalStatus, "Couldn't remove the virtual camera. It is still installed.",
                        "\(name): the Settings row reports the failure")
            model.stopTimersForTesting()
        }
        let ok = AppModel()
        ok.cameraExtension = FakeCameraExtensionActivator(script: [.active])
        ok.coreCommandCaptureForTesting = { _ in }
        ok.setVirtualCamera(enabled: true)
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        ok.removeVirtualCamera()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expect(ok.virtualCameraRemovalStatus == nil, "success leaves no removal status")
        ok.stopTimersForTesting()
    }

    /// The flyout toggles draw their own track: on and off must be told apart,
    /// and the off track must be visible against the flyout's panel.
    private static func testStudioToggleTrackIsVisibleInBothStates() {
        func rgba(_ c: Color) -> (r: Double, g: Double, b: Double, a: Double) {
            let n = NSColor(c).usingColorSpace(.sRGB) ?? .black
            return (Double(n.redComponent), Double(n.greenComponent), Double(n.blueComponent), Double(n.alphaComponent))
        }
        func luminance(_ c: Color) -> Double {
            let v = rgba(c)
            func lin(_ x: Double) -> Double { x <= 0.03928 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4) }
            return 0.2126 * lin(v.r) + 0.7152 * lin(v.g) + 0.0722 * lin(v.b)
        }
        func contrast(_ a: Color, _ b: Color) -> Double {
            let (x, y) = (luminance(a), luminance(b))
            return (max(x, y) + 0.05) / (min(x, y) + 0.05)
        }
        let on = StudioToggleStyle.trackColor(isOn: true)
        let off = StudioToggleStyle.trackColor(isOn: false)
        expect(on == Studio.accent, "on track is the accent")
        expectEqual(rgba(off).a, 1, "off track is opaque (a translucent white vanishes on the panel)")
        expect(contrast(off, Studio.panel) >= 2.5, "off track stands out from Studio.panel")
        expect(contrast(on, off) >= 1.5, "on and off tracks are distinguishable")
        expect(contrast(StudioToggleStyle.knobColor(isOn: false), off) >= 2.5, "off knob visible on its track")
        expect(contrast(StudioToggleStyle.knobColor(isOn: true), on) >= 2.5, "on knob visible on its track")
    }

    @MainActor
    private static func testVirtualCameraUnchangedNameSendsNothing() {
        let model = AppModel()
        model.cameraExtension = FakeCameraExtensionActivator(script: [.active])
        var sent = 0
        model.coreCommandCaptureForTesting = { _ in sent += 1 }
        model.setVirtualCameraDeviceName("Booth")
        expectEqual(sent, 1, "a new name is sent")
        model.setVirtualCameraDeviceName("  Booth  ")
        expectEqual(sent, 1, "the same name (after sanitizing) sends nothing")
        model.setVirtualCameraDeviceName("")
        expectEqual(sent, 2, "clearing the name is a change (back to the default)")
        model.setVirtualCameraDeviceName(CameraConstants.defaultDeviceName)
        expectEqual(sent, 2, "typing the default over a blank name sends nothing (same wire value)")
    }

    private static func testVirtualCameraProofNeedsTheSnapshotVariable() {
        expect(!AppModel.vcamProofRequested(["COREVIDEO_SHELL_VCAM": "live"]), "VCAM alone is inert")
        expect(!AppModel.vcamProofRequested(["COREVIDEO_SHELL_SNAPSHOT": "/tmp/x.png"]), "SNAPSHOT alone stages nothing")
        expect(AppModel.vcamProofRequested(["COREVIDEO_SHELL_VCAM": "live", "COREVIDEO_SHELL_SNAPSHOT": "/tmp/x.png"]),
               "both variables stage the proof")
    }

    private static func testCameraExtensionPrefsField() {
        var p = ShellPrefs()
        expect(p.cameraExtensionOptedIn == nil, "absent by default")
        p.cameraExtensionOptedIn = true
        let data = try! JSONEncoder().encode(p)
        let back = try! JSONDecoder().decode(ShellPrefs.self, from: data)
        expectEqual(back.cameraExtensionOptedIn, true, "opt-in survives a round trip")
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

    /// The corevideo:// FALLBACK (older brokers, manual links) keeps its exact
    /// allowlisted spelling; the normal path is the loopback listener.
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

    /// Live defect 2026-11-16: an in-show webcam drew on Program and Preview but
    /// its multiview tile stayed empty. The shell sent the scene-route MODE
    /// ("capture-input") as the multiview KIND; the core only takes "capture"
    /// (resolveMultiviewFeed), so the tile got no participantId and the
    /// compositor never looked up the frame.
    @MainActor
    private static func testMultiviewSourceWireShape() {
        var webcam = ShowInputSlot(id: 1)
        webcam.kind = "capture"; webcam.sourceId = "cam-1"; webcam.name = "Desk cam"; webcam.inShow = true
        var guest = ShowInputSlot(id: 2)
        guest.kind = "zoom"; guest.sourceId = "p7"; guest.name = "Guest"; guest.inShow = true
        var benched = ShowInputSlot(id: 3)
        benched.kind = "capture"; benched.sourceId = "cam-2"
        var gone = ShowInputSlot(id: 4)
        gone.kind = "capture"; gone.sourceId = "cam-3"; gone.inShow = true; gone.offline = true
        let payloads = AppModel.multiviewSourcePayloads([webcam, guest, benched, gone, ShowInputSlot(id: 5)])
        expectEqual(payloads.count, 2, "only in-show slots that still have a source get a tile")
        expectEqual(payloads[0]["kind"] as? String, "capture",
                    "the core's multiview kind for a capture device (MultiviewFeed.h), not the route mode")
        expectEqual(payloads[0]["captureDeviceId"] as? String, "cam-1", "the bare device id")
        expectEqual(payloads[0]["sourceId"] as? String, "capture:cam-1", "the frame key")
        expectEqual(payloads[0]["label"] as? String, "Desk cam", "the operator's name")
        expectEqual(payloads[1]["kind"] as? String, "participant-video", "a Zoom guest")
        expectEqual(payloads[1]["participantId"] as? String, "p7", "the raw participant id")
        expectEqual(payloads[1]["sourceId"] as? String, "zoom:p7", "the frame key")
    }

    @MainActor
    private static func testSpineSubscriptionWireShape() {
        let entries = [ZoomSourceBudget.Entry(participantId: "p1", purpose: "program"),
                       ZoomSourceBudget.Entry(participantId: "p2", purpose: "multiview")]
        let payloads = AppModel.spineSubscriptionPayloads(
            entries, audioParticipantIds: [], meetingHasParticipants: false)
        expectEqual(payloads.count, 2, "one subscription per budget entry")
        expectEqual(payloads[0]["participantId"] as? String, "p1", "payload order is budget order")
        // Windows ZoomMediaSpinePayloadBuilder: video priorities are 10+i (0 is the
        // meeting mix, 40+i the per-participant audio).
        expectEqual(payloads[0]["priority"] as? Int, 10, "video priority is 10 + budget index")
        // participant-video is the tiered kind: the core grants 1080P to the first
        // 8 camera-on entries IN THIS ORDER (ZoomSubscriptionResolutionPolicy.h),
        // which is the entire point of the budget. Identity is purpose-free for
        // this kind, so the real purpose rides the wire without churn.
        expectEqual(payloads[0]["kind"] as? String, "participant-video",
                    "the tiered kind — 1080P eligible, budget-order granted")
        expectEqual(payloads[0]["purpose"] as? String, "program", "the entry's real purpose")
        expectEqual(payloads[1]["purpose"] as? String, "multiview", "the entry's real purpose")
        expectEqual(payloads[1]["priority"] as? Int, 11, "video priority is 10 + budget index")
    }

    /// The live defect (2026-11-15): a real 9-guest meeting, raw media on, and
    /// audioMixSession.participants == 0 — the Mac spine subscribed video only.
    @MainActor
    private static func testSpineSubscribesTheMeetingMixAndSourceAudio() {
        let video = [ZoomSourceBudget.Entry(participantId: "101", purpose: "program")]
        let payloads = AppModel.spineSubscriptionPayloads(
            video, audioParticipantIds: ["101", "202"], meetingHasParticipants: true)
        let mix = payloads.filter { $0["kind"] as? String == "meeting-audio" }
        expectEqual(mix.count, 1, "exactly one meeting-audio subscription while in a meeting")
        expectEqual(mix.first?["participantId"] as? String, "0",
                    "the meeting mix rides the synthetic MeetingMixParticipantId, never a guest's id (#465)")
        expectEqual(mix.first?["purpose"] as? String, "program", "Windows: meeting-audio purpose program")
        expectEqual(mix.first?["priority"] as? Int, 0, "Windows: meeting-audio priority 0")
        let audio = payloads.filter { $0["kind"] as? String == "participant-audio" }
        expectEqual(audio.map { $0["participantId"] as? String ?? "" }, ["101", "202"],
                    "one participant-audio per audio source, in source order")
        expectEqual(audio.map { $0["purpose"] as? String ?? "" }, ["mix", "mix"],
                    "participant-audio purpose is the constant \"mix\" (it is part of the engine identity)")
        expectEqual(audio.map { $0["priority"] as? Int ?? -1 }, [40, 41],
                    "Windows: participant-audio priority 40 + index")
        let idle = AppModel.spineSubscriptionPayloads(
            [], audioParticipantIds: [], meetingHasParticipants: false)
        expect(idle.isEmpty, "no meeting participants: no meeting-audio subscription")
    }

    private static func testAudioSourceSetKeepsCameraOffAndRefusesNonSources() {
        // p-off: a camera-off wall guest. p-nobody: in the room, routed nowhere.
        // p-gone: still in a slot but left the meeting.
        let ids = ZoomSourceBudget.audioParticipantIds(
            programRouted: ["p-pgm"], previewRouted: ["p-pvw", "p-pgm"],
            multiviewAssigned: ["p-off", "p-gone"], isoArmed: ["p-iso"],
            present: ["p-pgm", "p-pvw", "p-off", "p-iso", "p-nobody"])
        expectEqual(ids, ["p-pgm", "p-pvw", "p-off", "p-iso"],
                    "Program -> Preview -> wall -> ISO order, deduped keep-first")
        expect(ids.contains("p-off"), "a camera-OFF source keeps its audio (owner rule #478)")
        expect(!ids.contains("p-nobody"), "a non-source is never subscribed for audio (sources only)")
        expect(!ids.contains("p-gone"), "a participant who left is not a source")
        let wall = (0..<14).map { "w\($0)" }
        expectEqual(ZoomSourceBudget.audioParticipantIds(
            programRouted: [], previewRouted: [], multiviewAssigned: wall, isoArmed: [],
            present: Set(wall)).count, 14, "audio is uncapped (only video is capped at 10)")
        let video = ZoomSourceBudget.videoEntries(
            programRouted: [], previewRouted: [], multiviewAssigned: ["p-off"], isoArmed: [],
            cameraOn: [])
        expect(video.isEmpty, "the same camera-off source spends no VIDEO budget")
    }

    /// The REAL call site (pushSpine's subscription array): slots + roster in, the
    /// audio set out — a camera-off wall guest is subscribed for audio, a guest in
    /// no slot is not, and the meeting mix rides along.
    @MainActor
    private static func testPushSpineSubscribesWallAudioFromTheModel() {
        let model = AppModel()
        model.roster = [
            RosterParticipant(id: "11", name: "On", hasVideo: true, muted: false,
                              talking: false, assigned: true),
            RosterParticipant(id: "22", name: "Off", hasVideo: false, muted: false,
                              talking: false, assigned: true),
            RosterParticipant(id: "33", name: "Audience", hasVideo: true, muted: false,
                              talking: false, assigned: false),
        ]
        for (index, pid) in ["11", "22"].enumerated() {
            model.slots[index].kind = "zoom"
            model.slots[index].sourceId = pid
            model.slots[index].inShow = true
        }
        let subs = model.resolveSpineSubscriptions()
        func ids(_ kind: String) -> [String] {
            subs.filter { $0["kind"] as? String == kind }.compactMap { $0["participantId"] as? String }
        }
        expectEqual(ids("meeting-audio"), ["0"], "in a meeting: the Zoom program mix is subscribed")
        expectEqual(ids("participant-audio"), ["11", "22"],
                    "every wall source gets audio, the camera-off one included")
        expect(!ids("participant-audio").contains("33"), "a non-source gets no audio")
        expectEqual(ids("participant-video"), ["11"], "only the camera-on source spends video")
        expectEqual(model.zoomAudioSourceIds, ["11", "22"],
                    "the console's Zoom source set is the one just subscribed")
        expect(model.meetingAudioSubscribed, "the console knows the meeting mix is subscribed")
    }

    // ── audio console: the FADER LAW + #481 on Mac ──────────────────────────

    /// A guest who starts sending audio AFTER the console's first channel sync is
    /// not in the core's echo (it echoes only synced channels) — the strip must
    /// come from the shell's own source set, or the FADER LAW drops the guest.
    private static func testStripsComeFromTheSourceSetNotOnlyTheEcho() {
        let expected = AudioConsolePolicy.expectedSourceIds(
            meetingAudio: true, zoomAudioIds: ["11", "22"], captureAudioIds: ["capture:cam"],
            mediaStrip: true)
        expectEqual(expected, ["zoom-mix", "11", "22", "capture:cam", "media"],
                    "zoom-mix, the Zoom audio sources, paired mics, and the media strip")
        // The core echoes only what was synced before guest 22 arrived.
        let echo: [JSONObject] = [
            ["participantId": "zoom-mix", "outputLevel": 40, "status": "live"],
            ["participantId": "11", "outputLevel": 30, "status": "live"],
        ]
        let strips = AudioConsolePolicy.mergeStrips(echo: echo, prior: [], expected: expected)
        expectEqual(strips.map(\.id), expected, "every expected source has a strip, echoed or not")
        expectEqual(strips.first { $0.id == "22" }?.status, "waiting-for-pcm",
                    "a source with no PCM yet gets a waiting strip (Windows)")
        expectEqual(strips.first { $0.id == "zoom-mix" }?.outputLevel, 40, "levels come from the echo")
        expect(strips.contains { $0.id == "zoom-mix" }, "the Zoom program mix always has a fader")
        expect(strips.contains { $0.id == "media" }, "a media route on a bus: the media strip exists")
        expectEqual(AudioConsolePolicy.expectedSourceIds(
            meetingAudio: false, zoomAudioIds: [], captureAudioIds: [], mediaStrip: false), [],
            "no meeting and no media route: no zoom-mix and no media strip (Windows)")
    }

    private static func testStripSetIsExactlyTheModelledSources() {
        let echo: [JSONObject] = [
            ["participantId": "99"],               // a Zoom guest who is not a source
            ["participantId": "media:clip-1"],     // a clip: governed by "media"
            ["participantId": "capture:gone"],     // an unpaired mic the core still echoes
            ["participantId": "zoom-mix"],         // the last meeting's mix
        ]
        let ids = AudioConsolePolicy.mergeStrips(
            echo: echo, prior: [], expected: ["11", "media"]).map(\.id)
        expectEqual(ids, ["11", "media"], "the strip set is exactly the modelled sources")
    }

    /// Review M1: a strip whose source is gone must leave, even though the core
    /// keeps echoing the channel it was last sent.
    @MainActor
    private static func testUnpairedMicStripLeavesTheConsole() {
        let model = AppModel()
        model.slots[0].kind = "capture"; model.slots[0].sourceId = "cam"
        model.slots[0].audioDeviceId = "mic"; model.slots[0].audioDeviceName = "Mic"
        model.refreshStrips()
        expect(model.strips.contains { $0.id == "capture:cam" }, "a paired mic has a strip")
        model.markAudioConsoleDeliveredForTesting()
        model.slots[0].audioDeviceId = ""      // unpaired
        model.applySnapshotForTesting(["audioMixSession": ["participants": [
            ["participantId": "capture:cam", "outputLevel": 0]]]])   // core still echoes it
        expect(!model.strips.contains { $0.id == "capture:cam" }, "the unpaired mic's strip is gone")
        expect(model.scheduledStripIds != nil, "...and the smaller set is synced to the core")
    }

    /// #481: the A1's mute is only set by the A1.
    private static func testA1MuteIsNeverAdoptedFromTheEcho() {
        // Before any channel sync the echo's `muted` is the voice-activity flag:
        // every quiet guest reads muted. A new strip must start UNMUTED.
        let first = AudioConsolePolicy.mergeStrips(
            echo: [["participantId": "11", "muted": true, "manualGainDb": -6.0, "pan": 0.5]],
            prior: [], expected: ["11"])
        expectEqual(first.first?.muted, false, "a new channel starts unmuted (#481)")
        expectEqual(first.first?.manualGainDb, 0, "a new channel starts at unity")
        // The A1 mutes and trims; then the core restarts and echoes "unmuted".
        var held = first.isEmpty ? [AudioStrip(id: "11", outputLevel: 0, manualGainDb: 0,
                                               muted: false, solo: false, pan: 0, status: "")] : first
        held[0].muted = true
        held[0].manualGainDb = 4
        let afterRestart = AudioConsolePolicy.mergeStrips(
            echo: [["participantId": "11", "muted": false, "outputLevel": 0]],
            prior: held, expected: ["11"])
        expectEqual(afterRestart.first?.muted, true, "a core echo never un-mutes the A1's channel")
        expectEqual(afterRestart.first?.manualGainDb, 4, "a core echo never resets the A1's fader")
    }

    @MainActor
    private static func testConsoleSyncCarriesEveryStripAndTheA1State() {
        let model = AppModel()
        model.zoomAudioSourceIds = ["11", "22"]
        model.meetingAudioSubscribed = true
        model.refreshStrips()
        expectEqual(model.scheduledStripIds, Set(["zoom-mix", "11", "22"]),
                    "a changed strip SET schedules a full channel sync (late guests get a fader)")
        model.editStrip("22") { $0.muted = true }
        let channels = model.audioMixCommand()["channels"] as? [JSONObject] ?? []
        expectEqual(channels.compactMap { $0["participantId"] as? String },
                    ["zoom-mix", "11", "22"],
                    "sync-participant-audio-mix carries a strip for every source (FADER LAW)")
        expectEqual(channels.first { $0["participantId"] as? String == "22" }?["muted"] as? Bool, true,
                    "the A1's mute rides the wire")
        expectEqual(model.audioSourceLabel("zoom-mix"), "Zoom program mix", "Windows strip label")
    }

    // ── default routing: a fresh Mac console routed nothing ─────────────────

    private static func testDefaultRoutingIsTheWindowsPerGuestTopology() {
        let sends = AudioConsolePolicy.baseRoutingSends(
            zoomAudioIds: ["11"], captureAudioIds: ["capture:cam"], mediaRouted: true,
            operatorEdits: [:])
        func buses(_ source: String) -> [String] {
            sends.filter { $0["sourceId"] as? String == source }.compactMap { $0["busId"] as? String }.sorted()
        }
        let program = ["master", "mon", "pgm-l", "pgm-r", "stream"]
        expectEqual(buses("11"), program, "a Zoom source's stem reaches program, stream and MON")
        expectEqual(buses("capture:cam"), program, "a paired capture mic gets the same defaults")
        expectEqual(buses("media"), program, "the media strip is routed by default (T1.6 alias)")
        expectEqual(buses("zoom-mix"), [], "zoom-mix is NOT summed with the stems (doubled voices)")
        expect(sends.allSatisfy { ($0["gainDb"] as? Double) == 0 }, "defaults are unity")
        let edited = AudioConsolePolicy.baseRoutingSends(
            zoomAudioIds: ["11"], captureAudioIds: [], mediaRouted: false,
            operatorEdits: [
                AudioRouteKey(source: "11", bus: "stream"): .init(enabled: false, gainDb: 0),
                AudioRouteKey(source: "zoom-mix", bus: "iso-1"): .init(enabled: true, gainDb: -6),
            ])
        expect(!edited.contains { $0["sourceId"] as? String == "11" && $0["busId"] as? String == "stream" },
               "an operator's removal wins over the default")
        expect(edited.contains { $0["sourceId"] as? String == "zoom-mix" && $0["busId"] as? String == "iso-1"
                && $0["gainDb"] as? Double == -6 }, "an operator's own crosspoint rides the base")
    }

    @MainActor
    private static func testModelRoutesEverySourceToTheMonitorBus() {
        let model = AppModel()
        model.roster = [RosterParticipant(id: "22", name: "Off", hasVideo: false, muted: false,
                                          talking: false, assigned: true)]
        model.slots[0].kind = "zoom"; model.slots[0].sourceId = "22"; model.slots[0].inShow = true
        _ = model.resolveSpineSubscriptions()
        let sends = model.routingBaseCommand()?["sends"] as? [JSONObject] ?? []
        expect(sends.contains { $0["sourceId"] as? String == "22" && $0["busId"] as? String == "mon" },
               "the camera-off wall guest is on MON: the Monitor toggle produces sound")
        expectEqual(model.routingBaseCommand()?["type"] as? String, "sync-audio-routing-matrix",
                    "the base matrix uses the core's full-state routing command")
    }

    /// The one-shot rule: a respawned core starts with no channels, mastering or
    /// routing. The next generation must get the whole console again — and the
    /// A1's mute must survive the fresh core's "unmuted" echo.
    @MainActor
    private static func testCoreRestartReassertsTheWholeAudioConsole() {
        let model = AppModel()
        model.zoomAudioSourceIds = ["11"]
        model.meetingAudioSubscribed = true
        model.refreshStrips()
        model.editStrip("11") { $0.muted = true }
        model.selectOrRouteSend(source: "zoom-mix", bus: "iso-1")
        expect(model.operatorRouteEdits[AudioRouteKey(source: "zoom-mix", bus: "iso-1")]?.enabled == true,
               "an operator crosspoint is kept as intent")
        model.markAudioConsoleDeliveredForTesting()
        expect(model.scheduledStripIds == nil && model.lastRoutingBaseSent != nil,
               "precondition: everything delivered to the old core")
        var batches: [[JSONObject]] = []
        model.audioReassertObserver = { batches.append($0) }
        model.onConnected()  // the respawned core's handshake
        expectEqual(batches.count, 1, "a new core generation gets the console in ONE request")
        let batch = batches.first ?? []
        expectEqual(batch.map { $0["type"] as? String ?? "" },
                    ["sync-participant-audio-mix", "sync-audio-routing-matrix", "sync-capture-audio-sources"],
                    "mix FIRST, then routing, then the mics: a muted source is never heard at unity (M6)")
        let channels = batch.first?["channels"] as? [JSONObject] ?? []
        expectEqual(Set(channels.compactMap { $0["participantId"] as? String }),
                    Set(["zoom-mix", "11"]), "the full channel list again")
        expectEqual(channels.first { $0["participantId"] as? String == "11" }?["muted"] as? Bool, true,
                    "the A1's mute is in the very first command the new core applies")
        let resent = (batch.count > 1 ? batch[1]["sends"] as? [JSONObject] : nil) ?? []
        expect(resent.contains { $0["sourceId"] as? String == "zoom-mix" && $0["busId"] as? String == "iso-1" },
               "the base routing (with the operator's crosspoint) is re-sent to the new core")
        // The fresh core echoes its defaults: unmuted, no gain.
        model.applySnapshotForTesting(["audioMixSession": ["participants": [
            ["participantId": "11", "muted": false, "outputLevel": 0]]]])
        expectEqual(model.strips.first { $0.id == "11" }?.muted, true,
                    "the restarted core's echo does not un-mute the A1's channel")
        let base = model.routingBaseCommand()?["sends"] as? [JSONObject] ?? []
        expect(base.contains { $0["sourceId"] as? String == "zoom-mix" && $0["busId"] as? String == "iso-1" },
               "the operator's crosspoint rides the re-sent base matrix")
    }

    /// #481 pre-mute meter: a muted guest who is talking shows, dimmed.
    private static func testMutedTalkingGuestShowsADimmedPreMuteMeter() {
        expectEqual(AudioConsolePolicy.meterLevel(dbfs: -60), 0, "Windows scale: -60 dBFS is 0")
        expectEqual(AudioConsolePolicy.meterLevel(dbfs: -24), 60, "Windows scale: -24 dBFS is 60")
        expectEqual(AudioConsolePolicy.meterLevel(dbfs: 0), 100, "Windows scale: 0 dBFS is 100")
        var prior = AudioStrip(id: "11", outputLevel: 0, manualGainDb: 0, muted: true,
                               solo: false, pan: 0, status: "")
        prior.inputLevel = 0
        let merged = AudioConsolePolicy.mergeStrips(
            echo: [["participantId": "11", "muted": true, "outputLevel": 0,
                    "inputPeakDbfs": -12.0, "inputRmsDbfs": -18.0]],
            prior: [prior], expected: ["11"])
        let strip = merged.first ?? prior
        expectEqual(strip.inputLevel, 80, "the core's pre-mute input peak reaches the strip")
        let reading = AudioConsolePolicy.meterReading(strip)
        expectEqual(reading.level, 80, "a muted, talking guest is not shown as silence")
        expect(reading.dimmed, "...and the meter is dimmed: nothing reaches a bus")
        var live = strip
        live.muted = false
        live.outputLevel = 55
        expect(AudioConsolePolicy.meterReading(live) == (55, false), "unmuted: the output level, undimmed")
        let native = AudioConsolePolicy.mergeStrips(
            echo: [["participantId": "11", "inputLevel": 42]], prior: [], expected: ["11"])
        expectEqual(native.first?.inputLevel, 42, "before any sync the native mixer's inputLevel is used")
    }

    /// Review I1: a cue or Take reorders the BUDGET (Program first) but must not
    /// move the A1's faders, and an order-only change is not a new strip set.
    @MainActor
    private static func testStripOrderIsRosterOrderAndStableAcrossACue() {
        let model = AppModel()
        model.roster = ["11", "22"].map {
            RosterParticipant(id: $0, name: "g\($0)", hasVideo: true, muted: false,
                              talking: false, assigned: true)
        }
        model.meetingAudioSubscribed = true
        model.zoomAudioSourceIds = ["22", "11"]   // 22 is on Program
        model.refreshStrips()
        expectEqual(model.strips.map(\.id), ["zoom-mix", "11", "22"],
                    "Zoom strips follow the roster, not the budget")
        model.markAudioConsoleDeliveredForTesting()
        model.zoomAudioSourceIds = ["11", "22"]   // a Take swaps Program and Preview
        model.refreshStrips()
        expectEqual(model.strips.map(\.id), ["zoom-mix", "11", "22"],
                    "a cue or Take does not move the A1's faders")
        expect(model.scheduledStripIds == nil, "an order-only change sends no channel sync")
    }

    /// Review I2: a muted guest who leaves the source set for one push (off the
    /// wall and back) comes back MUTED, not live at unity.
    @MainActor
    private static func testMutedGuestStaysMutedAcrossASourceSetExit() {
        let model = AppModel()
        model.zoomAudioSourceIds = ["11"]
        model.refreshStrips()
        model.editStrip("11") { $0.muted = true; $0.manualGainDb = -8 }
        model.zoomAudioSourceIds = []          // moved off the wall
        model.refreshStrips()
        expect(!model.strips.contains { $0.id == "11" }, "precondition: the strip is gone")
        model.zoomAudioSourceIds = ["11"]      // ...and back
        model.refreshStrips()
        let back = model.strips.first { $0.id == "11" }
        expectEqual(back?.muted, true, "the A1's mute survives a source-set exit")
        expectEqual(back?.manualGainDb, -8, "the A1's fader survives a source-set exit")
    }

    /// Review M4 + I2: the memory is per MEETING. Zoom reuses user ids, so a new
    /// meeting's guest 11 must not inherit the last meeting's mute or routing.
    @MainActor
    private static func testMeetingEndForgetsPerGuestAudioState() {
        let model = AppModel()
        model.zoomAudioSourceIds = ["11"]
        model.meetingAudioSubscribed = true
        model.refreshStrips()
        model.editStrip("11") { $0.muted = true }
        model.selectOrRouteSend(source: "11", bus: "iso-1")
        model.selectOrRouteSend(source: "zoom-mix", bus: "iso-2")
        model.applyZoomForTesting(["meetingState": "in-meeting"])
        expectEqual(model.rememberedStripSettings["11"]?.muted, true, "in a meeting: remembered")
        model.applyZoomForTesting(["meetingState": ""])
        expectEqual(model.rememberedStripSettings["11"]?.muted, true, "an UNKNOWN state never clears")
        model.applyZoomForTesting(["meetingState": "idle"])
        expect(model.rememberedStripSettings["11"] == nil, "meeting end forgets the guest's strip settings")
        expect(model.operatorRouteEdits[AudioRouteKey(source: "11", bus: "iso-1")] == nil,
               "meeting end forgets the guest's route edits")
        expect(model.operatorRouteEdits[AudioRouteKey(source: "zoom-mix", bus: "iso-2")] != nil,
               "non-guest route edits are kept")
        expect(!model.strips.contains { $0.id == "11" || $0.id == "zoom-mix" },
               "the meeting's strips leave the console at meeting end")
        model.zoomAudioSourceIds = ["11"]      // next meeting, Zoom reuses id 11
        model.refreshStrips()
        expectEqual(model.strips.first { $0.id == "11" }?.muted, false,
                    "a reused id starts unmuted in the next meeting")
    }

    /// Review I3: a send-gain drag must not put one full routing matrix per
    /// slider tick into the core's 128-entry mailbox.
    @MainActor
    private static func testSendGainDragCoalescesTheRoutingPush() {
        let model = AppModel()
        model.zoomAudioSourceIds = ["11"]
        var sent: [JSONObject] = []
        model.routingBaseSendObserver = { sent.append($0) }
        // The 100 ms debounce runs on a harness timer: on a slow CI runner a
        // "20 ms" spin overshot 100 ms and each such gap legitimately flushed
        // (7 sends, 2026-11-22). The timer elapses only when the harness says so.
        let timers = ManualTimers()
        model.timerSleepForTesting = { await timers.sleep($0) }
        model.selectOrRouteSend(source: "11", bus: "iso-1")
        for tick in 0..<40 {   // a drag: the run loop turns between ticks, no timer elapses
            model.setSendGain(source: "11", bus: "iso-1", gainDb: -Double(tick) / 4)
            spin(until: { timers.pending > tick }, limit: 2)
        }
        expect(sent.isEmpty, "nothing is sent while the debounce has not elapsed")
        // Fire every parked timer until the push lands: the last tick's task may
        // park after a fire, and every earlier one is cancelled. Identical later
        // pushes are deduplicated, so the count stays exact; a debounce that does
        // not cancel (or does not wait) still sends one matrix per tick.
        spin(until: { timers.fireAll(); return !sent.isEmpty }, limit: 2)
        spin(until: { timers.fireAll(); return false }, limit: 0.1)
        expectEqual(sent.count, 1, "a 40-tick drag sends ONE routing matrix (latest wins)")
        let gain = ((sent.last?["commands"] as? [JSONObject])?.first?["sends"] as? [JSONObject])?
            .first { $0["sourceId"] as? String == "11" && $0["busId"] as? String == "iso-1" }?["gainDb"] as? Double
        expectEqual(gain, -39.0 / 4, "the one push carries the LAST slider value")
        expectEqual(sent.last?["replaceableFullState"] as? Bool, true,
                    "opted in to core mailbox coalescing")
        expectEqual(sent.last?["coalescingKey"] as? String, "audio-routing-base",
                    "coalescing needs a non-empty key (CommandMailbox::push)")
    }

    /// Review M3: Windows routes "media" only when PROGRAM has a fixed media
    /// route, and shows its strip when Program or Preview does.
    @MainActor
    private static func testMediaRoutingFollowsTheProgramMediaRoute() {
        let model = AppModel()
        model.programSceneId = model.scenes[0].id
        model.previewSceneId = model.scenes[1].id
        func mediaSends() -> [JSONObject] {
            (model.routingBaseCommand()?["sends"] as? [JSONObject] ?? [])
                .filter { $0["sourceId"] as? String == "media" }
        }
        expect(mediaSends().isEmpty, "no media route on Program: media is not routed")
        expect(!model.strips.contains { $0.id == "media" }, "no media route on a bus: no media strip")
        model.logoBug = MediaAsset(id: "bug", name: "Bug", kind: "media-video", relativePath: "bug.mov", filePath: "/tmp/bug.mov",
                                   naturalWidth: 200, naturalHeight: 100)
        expect(model.sceneHasMediaRoute(model.programSceneId, isProgram: true),
               "precondition: the logo bug is a fixed media route on Program")
        expectEqual(mediaSends().count, 5, "a media route on Program: media gets the default sends")
        expect(model.strips.contains { $0.id == "media" }, "...and the media strip appears")
    }

    /// The roster-membership trigger: a camera-OFF guest joining or leaving
    /// changes the audio source set (and meeting-audio), so it must re-push the
    /// spine even though no camera changed.
    @MainActor
    private static func testRosterMembershipChangeRepushesTheSpine() {
        let model = AppModel()
        func roster(_ ids: [String]) -> JSONObject {
            ["meetingState": "in-meeting",
             "participants": ids.map { ["userId": $0, "displayName": "g\($0)", "videoOn": false] }]
        }
        model.applyZoomForTesting(roster(["11"]))
        let afterJoin = model.spineSyncRequests
        expect(afterJoin >= 1, "the first camera-off guest re-pushes the spine")
        model.applyZoomForTesting(roster(["11"]))
        expectEqual(model.spineSyncRequests, afterJoin, "an unchanged roster pushes nothing")
        model.applyZoomForTesting(roster(["11", "22"]))
        expectEqual(model.spineSyncRequests, afterJoin + 1, "a camera-off guest joining re-pushes")
        model.applyZoomForTesting(roster(["11"]))
        expectEqual(model.spineSyncRequests, afterJoin + 2, "a camera-off guest leaving re-pushes")
    }

    /// Round-2 review: only TERMINAL meeting states clear per-guest audio memory.
    @MainActor
    private static func testOnlyTerminalMeetingStatesForgetGuestAudio() {
        func primed() -> AppModel {
            let model = AppModel()
            model.zoomAudioSourceIds = ["11"]
            model.meetingAudioSubscribed = true
            model.refreshStrips()
            model.editStrip("11") { $0.muted = true; $0.manualGainDb = -5 }
            model.selectOrRouteSend(source: "11", bus: "iso-1")
            model.applyZoomForTesting(["meetingState": "in_meeting"])
            return model
        }
        for transient in ["joining", "leaving", "status-6", "status-12", "in-meeting", "reconnecting"] {
            let model = primed()
            model.applyZoomForTesting(["meetingState": transient])
            expectEqual(model.rememberedStripSettings["11"]?.muted, true,
                        "\(transient) mid-meeting keeps the guest's mute (breakout / waiting room / reconnect)")
            expectEqual(model.rememberedStripSettings["11"]?.manualGainDb, -5, "\(transient) keeps the fader")
            expect(model.operatorRouteEdits[AudioRouteKey(source: "11", bus: "iso-1")] != nil,
                   "\(transient) keeps the guest's route edits")
            expectEqual(model.strips.first { $0.id == "11" }?.muted, true, "\(transient) keeps the strip muted")
        }
        // Leave / meeting ended (engine "left" -> idle), the SDK's ended, and a
        // lost engine or failed meeting (error) all clear.
        for terminal in ["idle", "ended", "error", "left", "failed"] {
            let model = primed()
            model.applyZoomForTesting(["meetingState": terminal])
            expect(model.rememberedStripSettings["11"] == nil, "\(terminal) forgets the guest's strip settings")
            expect(model.operatorRouteEdits[AudioRouteKey(source: "11", bus: "iso-1")] == nil,
                   "\(terminal) forgets the guest's route edits")
        }
    }

    /// Round-2 review (I2 gap): a returning A1-muted guest's mute — and the base
    /// routing — reach the core in a request AHEAD of the spine that re-subscribes
    /// the guest's audio, never 150 ms after it.
    @MainActor
    private static func testReturningMutedGuestsMuteGoesAheadOfTheSpine() {
        let model = AppModel()
        model.roster = [RosterParticipant(id: "11", name: "G", hasVideo: true, muted: false,
                                          talking: false, assigned: true)]
        model.slots[0].kind = "zoom"; model.slots[0].sourceId = "11"; model.slots[0].inShow = true
        let first = model.spinePushRequests()
        expect(((first.first?["commands"] as? [JSONObject]) ?? []).contains {
            $0["type"] as? String == "sync-audio-routing-matrix" },
            "a new guest's routing goes ahead of the spine (no raw unmuted fallback window)")
        model.editStrip("11") { $0.muted = true }
        model.markAudioConsoleDeliveredForTesting()
        model.slots[0].inShow = false            // off the wall...
        _ = model.spinePushRequests()
        model.slots[0].inShow = true             // ...and back
        let requests = model.spinePushRequests()
        expectEqual(requests.map { $0["type"] as? String ?? "" },
                    ["media-core-sync", "zoom-media-spine-sync"],
                    "the console request goes AHEAD of the spine that re-subscribes the audio")
        let commands = requests.first?["commands"] as? [JSONObject] ?? []
        let mix = commands.first { $0["type"] as? String == "sync-participant-audio-mix" }
        let channel = (mix?["channels"] as? [JSONObject])?.first { $0["participantId"] as? String == "11" }
        expectEqual(channel?["muted"] as? Bool, true, "the returning guest's remembered mute is in it")
        let subs = (requests.last?["spinePayload"] as? JSONObject)?["subscriptions"] as? [JSONObject] ?? []
        expect(subs.contains { $0["kind"] as? String == "participant-audio" && $0["participantId"] as? String == "11" },
               "the spine after it re-subscribes the guest's audio")
        let again = model.spinePushRequests()
        expectEqual(again.count, 1, "an unchanged console adds nothing ahead of the next spine")
    }

    /// Cross-checks the audio wire strings against the CORE's source, the way the
    /// payload-wrapper contract test does: a rename in C++ fails here, not live.
    @MainActor
    private static func testAudioSubscriptionKindsMatchTheCoreSource() {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        guard let runtime = try? String(contentsOf: root.appendingPathComponent(
            "native/src/modules/ZoomEngineRuntime.cpp"), encoding: .utf8) else {
            FileHandle.standardError.write(
                "  SKIPPED audio-kind cross-check (core source not alongside the shell)\n"
                    .data(using: .utf8)!)
            return
        }
        guard let line = runtime.split(separator: "\n").first(where: {
            $0.contains("const bool isAudioSubscription =")
        }) else {
            expect(false, "the core no longer declares isAudioSubscription — re-derive the audio kinds")
            return
        }
        let quoted = line.split(separator: "\"").enumerated()
            .filter { $0.offset % 2 == 1 }.map { String($0.element) }
        let emitted = Set(AppModel.spineSubscriptionPayloads(
            [], audioParticipantIds: ["1"], meetingHasParticipants: true)
            .compactMap { $0["kind"] as? String })
        expectEqual(emitted, Set(quoted),
                    "the shell's audio kinds are exactly the core's audio subscription kinds")
        expect(runtime.contains("command.isolateAudio = kind == \"participant-audio\""),
               "the core isolates per-guest audio on kind participant-audio")
        expect(runtime.contains("std::string(\"zoom-mix\")"),
               "the core ingests the meeting mix as the \"zoom-mix\" source (the strip id)")
        expect(runtime.contains("kind + \"-\" + participantId + \"-\" + purpose"),
               "audio identity includes purpose — why participant-audio purpose must stay constant")
    }

    // ── dropout policy (per-source, #535 slice 4a reaching Mac) ─────────────

    // testSourcePolicyCommandsAreZoomOnlyAndOrdered moved to ShellTestsSources.swift as
    // sources-policy/commands-zoom-and-capture-with-names (sources plan Task 9: the core
    // now takes a dropout policy on capture:<id>, and every command carries displayName).

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
    static func sceneEditModel() -> AppModel {
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
        // Deleting the Preview scene moves Preview to Program (Windows
        // PreviewAfterRemoval), never moves Program.
        expect(ok.deleteScene("s1") == nil, "preview scene deletes")
        expectEqual(ok.previewSceneId, "s2", "Preview moves to Program")
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

    private static func testHarnessPredicateIsOneAndAbortsOnRealPrefs() {
        expect(ShellHarness.detect(in: ["COREVIDEO_SHELL_TESTS": "1"]), "TESTS is a harness run")
        expect(ShellHarness.detect(in: ["COREVIDEO_SHELL_SELFCHECK": "1"]),
               "SELFCHECK is a harness run (the shell log must not be written)")
        expect(ShellHarness.detect(in: ["COREVIDEO_SHELL_TAB_SNAPSHOT": "/tmp/x"]),
               "TAB_SNAPSHOT is a harness run (its seed must not write the real shell log)")
        expect(!ShellHarness.detect(in: [:]), "a normal launch is not")
        expect(ShellHarness.isActive, "this run is a harness run")
        expect(ShellHarness.prefsIsolationFailure(path: "/x/real.json", realPath: "/x/real.json") != nil,
               "prefs resolving to the real file is an abort")
        expect(ShellHarness.prefsIsolationFailure(path: ShellPrefs.path, realPath: ShellPrefs.realPath) == nil,
               "this run's prefs are isolated")
    }

    @MainActor
    private static func testSceneEditorNeverTargetsProgram() {
        let model = sceneEditModel()
        expectEqual(model.sceneEditTargetId ?? "<nil>", "s1", "editor targets Preview")
        model.previewSceneId = ""
        expect(model.sceneEditTargetId == nil, "empty Preview -> no edit target (not Program)")
        // Preview == Program (the on-air row clicked, or the one-scene launch
        // fallback): the editor must not target the live scene.
        model.previewSceneId = model.programSceneId
        expect(model.sceneEditTargetId == nil, "Preview == Program -> no edit target (on air)")
        model.previewSceneId = ""
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
        // Elapsed time past the shell's own poll period (the allowance) counts.
        let poll = AudioObservationPolicy.snapshotPollInterval
        func state(_ elapsedMs: Double) -> String {
            AudioObservationPolicy.state(obs, receivedAt: t0,
                                         now: t0.addingTimeInterval(poll + elapsedMs / 1000))
        }
        expectEqual(state(250), "fresh", "250 ms is fresh")
        expectEqual(state(251), "aging", "251 ms is aging")
        expectEqual(state(1000), "aging", "1000 ms is aging")
        expectEqual(state(1001), "stale", "1001 ms is stale")
        // Owner age is carried, not reset by receipt.
        let owned = AudioObservation(sequence: 3, status: "fresh", ageMs: 50)
        expectEqual(AudioObservationPolicy.state(owned, receivedAt: t0,
                    now: t0.addingTimeInterval(poll + 0.19)), "fresh", "50+190 fresh")
        expectEqual(AudioObservationPolicy.state(owned, receivedAt: t0,
                    now: t0.addingTimeInterval(poll + 0.21)), "aging", "50+210 aging")
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

    // Owner report 2026-11-15 ("the bottom is bouncing with some odd error about
    // audio"): the shell polls snapshots every 0.5 s, and the policy counted the
    // time since RECEIPT against the 250 ms fresh budget, so healthy audio read
    // "meters aging" for the back half of every poll — twice a second.
    @MainActor
    private static func testMeterWarningAllowsTheShellPollInterval() {
        let t0 = Date(timeIntervalSince1970: 7_000)
        let healthy = AudioObservation(sequence: 9, status: "fresh", ageMs: 20)
        expect(AudioObservationPolicy.warning(healthy, receivedAt: t0,
                                              now: t0.addingTimeInterval(0.4)) == nil,
               "fresh snapshot received 400 ms ago (inside one poll): no warning")
        expect(AudioObservationPolicy.warning(healthy, receivedAt: t0,
                                              now: t0.addingTimeInterval(0.5)) == nil,
               "exactly one poll period since receipt: no warning")
        expectEqual(AudioObservationPolicy.warning(healthy, receivedAt: t0,
                                                   now: t0.addingTimeInterval(2)) ?? "",
                    "meters stale \u{2014} last values held",
                    "received 2 s ago: a stalled core still goes stale")
        expectEqual(AudioObservationPolicy.snapshotPollInterval, 0.5,
                    "the allowance is the shell's own poll period")
        let model = AppModel()
        model.observeAudioObservation(
            ["audioObservation": ["sequence": 1, "ageMs": 20, "status": "fresh"]], now: t0)
        for step in stride(from: 0.0, through: 0.5, by: 0.05) {
            expect(model.meterWarning(now: t0.addingTimeInterval(step)) == nil,
                   "healthy audio between polls never warns (+\(step)s)")
        }
    }

    // ── Studio footer parity (owner report 2026-11-15) ──────────────────────

    @MainActor
    private static func testStudioFooterReadoutsAreWindows() {
        expectEqual(StudioFooterReadout.transitions.map(\.mode), ["cut", "fade", "dip", "wipe"],
                    "transition menu is Cut / Fade / Dip / Wipe (all built by the shared core)")
        for option in StudioFooterReadout.transitions {
            expectEqual(AppModel.normalizeTakeTransitionMode(option.mode), option.mode,
                        "\(option.label) survives the take-transition normalizer")
        }
        expectEqual(StudioFooterReadout.transitionDuration("cut"), "Instant", "cut has no duration")
        expectEqual(StudioFooterReadout.transitionDuration("dip"), "\u{00B7} 300 ms", "dip is 300 ms")
        var slots = (1...10).map { ShowInputSlot(id: $0) }
        expectEqual(StudioFooterReadout.setupSummary(slots: slots, guests: 0),
                    "0 inputs \u{00B7} 0 Zoom guests", "Windows SetupProgressSummary, empty")
        slots[0].kind = "zoom"; slots[0].inShow = true
        slots[1].kind = "capture"; slots[1].inShow = false
        expectEqual(StudioFooterReadout.setupSummary(slots: slots, guests: 1),
                    "1 input \u{00B7} 1 Zoom guest", "only in-show assigned slots count; singulars")
        expectEqual(StudioFooterReadout.lufsLabel(-120), "LUFS \u{2014}", "no loudness yet")
        expectEqual(StudioFooterReadout.lufsLabel(-16.44), "-16.4 LUFS", "measured loudness")
        expectEqual(StudioFooterReadout.masterStatusLine(warning: "meters aging", limiterActive: true),
                    "meters aging", "the meter warning takes the status line's place")
        expectEqual(StudioFooterReadout.masterStatusLine(warning: nil, limiterActive: true),
                    "Limiter reducing", "limiter state when healthy")
        let taps: [String: Any] = ["audioRoutingMatrix": ["busTaps": [
            ["busId": "pgm-l", "frames": 480, "peakDbfs": -6.0],
            ["busId": "pgm-r", "frames": 480, "peakDbfs": -30.0],
        ]]]
        let sides = StudioFooterReadout.masterLevels(snapshot: taps, fallback: 5)
        expectEqual(sides.left, 90, "L from the pgm-l tap (-6 dBFS on the -60..0 scale)")
        expectEqual(sides.right, 50, "R from the pgm-r tap")
        let none = StudioFooterReadout.masterLevels(snapshot: [:], fallback: 42)
        expect(none.left == 42 && none.right == 42, "no taps: mono master level on both sides")
        for text in [StudioFooterReadout.ndiUnavailable] {
            expect(text.contains("isn't available on Mac yet"), "unavailable output says so plainly")
            expect(!text.contains("Developer ID"), "the app IS Developer ID signed; never blame it")
        }
    }

    /// The bounce itself: status that comes and goes must never resize the footer.
    @MainActor
    private static func testStudioFooterHeightIgnoresStatus() {
        func height(_ configure: (AppModel) -> Void) -> CGFloat {
            let model = AppModel()
            configure(model)
            let host = NSHostingView(rootView: StudioFooter().environmentObject(model)
                .frame(width: 1600))
            host.frame = NSRect(x: 0, y: 0, width: 1600, height: 400)
            host.layoutSubtreeIfNeeded()
            return host.fittingSize.height
        }
        let t0 = Date()
        let calm = height { model in
            model.observeAudioObservation(
                ["audioObservation": ["sequence": 1, "ageMs": 20, "status": "fresh"]], now: t0)
        }
        expect(calm > 100, "footer measured (\(calm) pt)")
        let warned = height { model in
            model.observeAudioObservation(
                ["audioObservation": ["sequence": 1, "ageMs": 5000, "status": "stale"]], now: t0)
        }
        expectEqual(warned, calm, "a meter warning does not change the footer height")
        let busy = height { model in
            FooterSnapshotHarness.seedLive(model, now: t0)
            model.recordingWarning = "disk nearly full"
            model.takeStatus = String(repeating: "Take failed: a very long reason ", count: 6)
            model.lowerThirdPhase = "building-out"
        }
        expectEqual(busy, calm, "recording/stream/take/lower-third status does not change the height")
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
        expect(model.meterWarning(now: t0.addingTimeInterval(0.5)) == nil,
               "one shell poll period without a snapshot is expected, not aging")
        expect(model.meterWarning(now: t0.addingTimeInterval(1.0)) == "meters aging",
               "no new snapshot for 1 s (a missed poll): aging")
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
        seedProgramSource(model)   // Key in names a rendered Program source (Tier 0.6)
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

    /// CONTROLLER RULING (final review): Windows ApplyBrandOverlayDefaults
    /// disables Graphics rows only; the manual lower third is independent
    /// (StudioViewModel.cs:13851 ShouldEnableProgramLowerThird = manuallyEnabled
    /// || ...). The Mac has no Graphics rows yet, so all-off must leave an
    /// on-air manual lower third alone, on a brand-kit apply AND a brand edit.
    @MainActor
    private static func testBrandDefaultAllOffLeavesTheManualLowerThirdAlone() {
        for phase in ["on-air", "building-in"] {
            let (model, seen) = brandDefaultsModel("all-off", phase: phase)
            model.pushBrandKitNow()
            expect(seen().contains { ($0["type"] as? String) == "set-brand-kit" },
                   "brand kit still goes out (\(phase))")
            expect(!lowerThirdHides(seen()), "all-off does not retire the manual \(phase) lower third")
            spin()
            expectEqual(hiddenAcks(seen()), 0, "no hidden ack either (\(phase))")
            expectEqual(model.lowerThirdPhase, phase, "the phase is untouched (\(phase))")
        }
        let (edited, seenEdited) = brandDefaultsModel("all-off", phase: "on-air")
        edited.overlays.brandName = "Acme"
        edited.applyBrandKit()               // the coalesced brand-edit path
        spin(0.45)
        expect(seenEdited().contains { ($0["type"] as? String) == "set-brand-kit" }, "the edit pushed the kit")
        expect(!lowerThirdHides(seenEdited()) && hiddenAcks(seenEdited()) == 0,
               "a brand-kit edit with all-off never takes the manual lower third off air")
    }

    /// The manual Hide's two-step retire: idempotent while in flight, and a
    /// Show inside the window cancels the pending hidden ack.
    @MainActor
    private static func testLowerThirdRetireIsIdempotentAndShowWins() {
        let (twice, seenTwice) = brandDefaultsModel("manual", phase: "on-air")
        twice.hideLowerThird()
        twice.hideLowerThird()
        expectEqual(buildOuts(seenTwice()), 1, "repeated Hide sends one building-out")
        spin()
        expectEqual(hiddenAcks(seenTwice()), 1, "repeated Hide sends one hidden ack")
        twice.hideLowerThird()
        expectEqual(buildOuts(seenTwice()), 2, "a Hide after the first completed is allowed")
        spin()

        let (shown, seenShown) = brandDefaultsModel("manual", phase: "on-air")
        shown.hideLowerThird()
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


    /// File > New Window and Dock-reopen after closing the main window both mint a
    /// new main-window view whose .onAppear calls start() again. A second run
    /// re-restored prefs over live state, doubled the autosave timer and launched a
    /// second core, orphaning the first (possibly mid-recording). Everything start()
    /// sets up must happen exactly once per process.
    @MainActor
    private static func testStartRunsItsSetupExactlyOncePerProcess() {
        let model = AppModel()
        var runs = 0
        model.startBodyForTesting = { runs += 1 }
        model.start()
        model.start()
        model.start()
        expectEqual(runs, 1, "start() runs its setup (prefs restore, timers, bridge launch) once")
        expect(model.didStart, "the model records that it has started")
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

    /// Scripted core: records every command it is sent (types AND bodies) and
    /// answers snapshots from a small recording lifecycle: a start makes it
    /// `live`; a stop (or `markStopping`) walks it through `stopState` to
    /// `completed`. `liveAfterStopPolls` answers that many post-stop requests
    /// (the stop's own ack counts as one) with `live` still — a poll issued
    /// before the stop landed core-side.
    private final class FakeQuitCore: @unchecked Sendable {
        private let lock = NSLock()
        private var _commands: [String] = [], _sent: [[String: Any]] = []
        private var _polls = 0, stopped = false, started = false, _hang = false
        var completeAfterPolls = 2
        var liveAfterStopPolls = 0
        var stopState = "stopping"
        var commands: [String] { lock.lock(); defer { lock.unlock() }; return _commands }
        var sent: [[String: Any]] { lock.lock(); defer { lock.unlock() }; return _sent }
        var hang: Bool { get { lock.lock(); defer { lock.unlock() }; return _hang }
                         set { lock.lock(); _hang = newValue; lock.unlock() } }
        /// The core already saw a stop before the scenario begins.
        func markStopping() { lock.lock(); stopped = true; _polls = 0; lock.unlock() }
        func handle(_ body: [String: Any]) async throws -> [String: Any] {
            if hang { try? await Task.sleep(nanoseconds: 30_000_000_000) }
            return respond(body)
        }
        private func respond(_ body: [String: Any]) -> [String: Any] {
            lock.lock(); defer { lock.unlock() }
            let raw = body["commands"] as? [[String: Any]] ?? []
            let cmds = raw.compactMap { $0["type"] as? String }
            _commands += cmds
            _sent += raw
            if cmds.contains("start-recording-session") { started = true; stopped = false }
            if cmds.contains("stop-recording-session") { stopped = true; _polls = 0 }
            var state = started ? "live" : "producing"
            if stopped {
                _polls += 1
                if _polls <= liveAfterStopPolls {
                    state = "live"
                } else {
                    state = _polls > liveAfterStopPolls + completeAfterPolls ? "completed" : stopState
                }
            }
            return ["ok": true, "snapshot": Self.snapshot(state: state, desired: !stopped)]
        }
        static func snapshot(state: String, desired: Bool) -> [String: Any] {
            let lifecycle: [String: Any] = ["sessionId": "s", "desiredActive": desired, "state": state,
                                            "health": "healthy", "finalized": state == "completed"]
            return ["recording": ["lifecycle": lifecycle] as [String: Any]]
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
    /// Harness timer for AppModel.timerSleepForTesting: every sleep parks until
    /// fireAll(). A cancelled debounce task resumes and sees Task.isCancelled.
    final class ManualTimers {
        private var waiters: [CheckedContinuation<Void, Never>] = []
        /// Sleeps parked right now (not yet fired).
        var pending: Int { waiters.count }
        func sleep(_ nanoseconds: UInt64) async {
            await withCheckedContinuation { waiters.append($0) }
        }
        func fireAll() {
            let ready = waiters
            waiters = []
            ready.forEach { $0.resume() }
        }
    }

    static func spin(until condition: () -> Bool, limit: TimeInterval) {
        let end = Date().addingTimeInterval(limit)
        while !condition(), Date() < end { RunLoop.main.run(until: Date().addingTimeInterval(0.01)) }
    }

    /// Drives the operator's REAL Record start (intent through recordingCommands)
    /// and waits for its acknowledgement to be finished.
    @MainActor
    private static func startRecordingThroughTheRealPath(_ model: AppModel, _ core: FakeQuitCore) {
        model.toggleRecording()
        spin(until: { core.commands.contains("start-recording-session") }, limit: 3)
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))   // let finish() land
        model.applySnapshotForTesting(FakeQuitCore.snapshot(state: "live", desired: true))
    }

    @MainActor
    private static func testQuitFlowStopsInOrderAndFinishesAtTheTerminalSnapshot() {
        let core = FakeQuitCore()
        // The first poll after the stop still says `recording` (issued before
        // the stop landed core-side). A stop that bypassed recordingCommands
        // left the policy's intent ON, and that poll re-flipped it.
        core.liveAfterStopPolls = 2   // the stop's own ack + the first quit poll
        let (model, replies) = quitModel(core: core, timeout: 15)
        model.streamingDesired = true
        startRecordingThroughTheRealPath(model, core)
        expect(model.recordingDesired && model.recordingStatus == "recording",
               "intent primed through the real start path")
        var history: [Bool] = []
        let watch = model.$recordingDesired.dropFirst().sink { history.append($0) }
        defer { watch.cancel() }
        let before = core.commands.count
        expect(model.handleQuitRequest() == .terminateLater, "live outputs defer the quit")
        expect(model.quitPromptVisible, "the confirmation is shown")
        expect(model.handleQuitRequest() == .terminateCancel, "a second quit while asking is cancelled, never forced")
        let t0 = Date()
        model.confirmQuit()
        spin(until: { !replies().isEmpty }, limit: 5)
        expectEqual(replies(), [true], "exactly one reply, to terminate")
        expect(Date().timeIntervalSince(t0) < 3, "finishes when the terminal snapshot arrives, far before the 15 s bound")
        expectEqual(Array(core.commands.dropFirst(before)), ["start-program-output", "stop-recording-session"],
                    "stream stop first, then the recording stop")
        expectEqual(model.recordingStatus, "completed", "the direct poll applied the terminal snapshot")
        expect(!history.contains(true),
               "recording intent never re-arms after the quit stop (a post-stop `recording` poll included): \(history)")
        expect(!model.recordingDesired, "recording intent stays off after the quit stop")
        expect(!model.quitFinishing, "the finishing state is cleared")
        expect(!model.warnings.contains { $0.contains("did not finish") }, "no false timeout warning")
    }

    private static func recordingDestinations(_ sent: [[String: Any]]) -> [[String]] {
        sent.filter { ($0["type"] as? String) == "start-program-output" }
            .map { $0["destinations"] as? [String] ?? ["<missing>"] }
    }

    /// Final-review CRITICAL 1: Stop Rec, then Cmd-Q while the shell still reads
    /// `finalizing` with a stream up. The stream stop must not ask the core for
    /// a "recording" destination (the core no longer owns it and would open a
    /// NEW session folder that nothing ever stops).
    @MainActor
    private static func testQuitWhileFinalizingNeverReopensARecording() {
        let core = FakeQuitCore()
        core.markStopping()
        core.stopState = "finalizing"
        let (model, replies) = quitModel(core: core, timeout: 5)
        model.recordingStatus = "finalizing"
        model.recordingDesired = false
        model.streamingDesired = true
        expect(model.handleQuitRequest() == .terminateLater, "a live stream asks")
        model.confirmQuit()
        spin(until: { !replies().isEmpty }, limit: 6)
        expectEqual(replies(), [true], "the quit completes")
        expect(!recordingDestinations(core.sent).contains { $0.contains("recording") },
               "no start-program-output carries a recording destination: \(recordingDestinations(core.sent))")
        expectEqual(core.commands, ["stop-encoder-session"],
                    "the stream stop is the operator's stop; no second recording stop while finalizing")
    }

    /// Whole-branch quit: ISO take armed, roster emptied mid-take, stream up,
    /// operator stops the recording, then quits. Asserts EXACTLY what reached the
    /// wire, in order, and that no ISO re-arms from presence afterwards.
    @MainActor
    private static func testWholeBranchQuitAfterStoppingAnIsoTakeWithAStreamUp() {
        let core = FakeQuitCore()
        core.stopState = "finalizing"
        core.completeAfterPolls = 3
        let (model, replies) = quitModel(core: core, timeout: 5)
        func guest(_ id: String) -> RosterParticipant {
            RosterParticipant(id: id, name: id, hasVideo: true, muted: false, talking: false, assigned: true)
        }
        model.roster = [guest("a"), guest("b")]
        model.assignSlot(1, kind: "zoom", sourceId: "a", name: "a")
        model.assignSlot(2, kind: "zoom", sourceId: "b", name: "b")
        model.toggleSlotIso(1)
        model.toggleSlotIso(2)
        // Key first, as in the app: toggleStreaming validates before it sets the
        // intent, and the streamed destinations are frozen at that moment.
        model.streamKey = "quit-test-key"        // only a VALID destination is sent (D1)
        model.streamingDesired = true            // stream up
        startRecordingThroughTheRealPath(model, core)
        model.roster = []                        // transient roster loss mid-take (#1183)
        model.toggleRecording()                  // operator stops the recording
        spin(until: { core.commands.contains("stop-recording-session") }, limit: 3)
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        model.applySnapshotForTesting(FakeQuitCore.snapshot(state: "finalizing", desired: false))
        expectEqual(model.recordingStatus, "finalizing", "the shell still shows the file finishing")
        expect(!model.recordingDesired, "operator intent is off")
        model.roster = [guest("a"), guest("b")]  // presence returns before the quit
        expect(model.handleQuitRequest() == .terminateLater, "stream + finalizing asks")
        model.confirmQuit()
        spin(until: { !replies().isEmpty }, limit: 6)
        expectEqual(replies(), [true], "the quit completes")
        // set-source-policy rides every scene sync for each assigned source since sources
        // plan Task 9 (it carries the slate displayName); it is not part of the quit flow.
        expectEqual(core.commands.filter { $0 != "set-source-policy" },
                    ["start-program-output", "set-recording-targets", "start-recording-session",
                     "stop-recording-session", "stop-encoder-session"],
                    "exactly: the take's start trio, the operator's stop, the quit's stream stop")
        let starts = core.sent.filter { ($0["type"] as? String) == "start-program-output" }
        expectEqual(starts.count, 1, "only the take's own start-program-output was sent")
        expectEqual(starts.first?["destinations"] as? [String] ?? [], ["recording", "rtmp"],
                    "the take started recording alongside the live stream")
        expectEqual(starts.first?["isoSourceIds"] as? [String] ?? [], ["zoom:a", "zoom:b"], "the take armed both ISOs")
        expect(!model.warnings.contains { $0.contains("did not finish") }, "finished, no timeout")
    }

    /// Windows CanSetRecording refuses while _outputsClosing: once Stop-and-quit
    /// is running, the Record / Stream buttons change nothing.
    @MainActor
    private static func testOutputTogglesRefuseWhileQuitting() {
        let core = FakeQuitCore()
        core.hang = true
        let (model, replies) = quitModel(core: core, timeout: 0.5)
        model.streamingDesired = true            // a stream-only quit
        _ = model.handleQuitRequest()
        model.confirmQuit()
        expect(model.quitFinishing, "the quit flow is running")
        model.toggleRecording()
        expect(!model.recordingDesired, "Record is refused while quitting (no new take starts)")
        model.toggleStreaming()
        expect(model.streamingDesired, "Stream is refused while quitting (intent unchanged)")
        spin(until: { !replies().isEmpty }, limit: 6)
    }


    // ── Take is a real transition with rollback (audit §3/§4) ────────────────
    // Windows: TransportCoordinatorTests Take_DoubleInvocationWhileAwaitingSync-
    // DoesNotSwapBack / Take_FailureRestoresPriorProgramAndRetainsPreviewForRetry;
    // MediaCoreCommandBuilder.BuildSyncCommands puts begin-take-transition first.

    final class FakeTakeCore: @unchecked Sendable {
        private let lock = NSLock()
        private var _bodies: [[String: Any]] = []
        private var _hold = false, _failure: String?
        var bodies: [[String: Any]] { lock.lock(); defer { lock.unlock() }; return _bodies }
        var hold: Bool { get { lock.lock(); defer { lock.unlock() }; return _hold }
                         set { lock.lock(); _hold = newValue; lock.unlock() } }
        var failure: String? { get { lock.lock(); defer { lock.unlock() }; return _failure }
                               set { lock.lock(); _failure = newValue; lock.unlock() } }
        private func record(_ body: [String: Any]) { lock.lock(); _bodies.append(body); lock.unlock() }
        func handle(_ body: [String: Any]) async throws -> [String: Any] {
            record(body)
            while hold { try? await Task.sleep(nanoseconds: 5_000_000) }
            if let failure { throw BridgeError.remote(failure) }
            return ["ok": true]
        }
        func types(_ index: Int = 0) -> [String] {
            ((bodies[index]["commands"] as? [[String: Any]]) ?? []).compactMap { $0["type"] as? String }
        }
    }

    @MainActor
    static func takeModel(_ core: FakeTakeCore) -> AppModel {
        let model = sceneEditModel()   // Program s2, Preview s1
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.status = .connected(renderer: "test")
        return model
    }

    @MainActor
    static func runTake(_ model: AppModel, wait: Bool = true) -> TakeBox {
        let box = TakeBox()
        Task { @MainActor in box.result = await model.take() }
        if wait { spin(until: { box.result != nil }, limit: 3) }
        return box
    }

    final class TakeBox { var result: AppModel.TakeResult? }

    @MainActor
    private static func testTakeBatchOrderAndFields() {
        let core = FakeTakeCore()
        let model = takeModel(core)
        let box = runTake(model)
        expectEqual(box.result, .success, "Take succeeds")
        expectEqual(core.bodies.count, 1, "Take is ONE media-core-sync request")
        guard core.bodies.count == 1 else { return }
        let types = core.types()
        expectEqual(types.first ?? "", "begin-take-transition", "the transition leads the batch")
        let load = types.firstIndex(of: "load-scene-graph") ?? -1
        expect(load > 0, "load-scene-graph follows begin-take-transition")
        expect(types.contains("set-preview-scene"), "preview rides the same batch")
        let cmds = core.bodies[0]["commands"] as? [[String: Any]] ?? []
        let t = cmds[0]
        expectEqual(t["mode"] as? String ?? "", "fade", "default mode is fade")
        expectEqual(t["durationMs"] as? Int ?? -1, 300, "300 ms")
        expectEqual(t["direction"] as? String ?? "", "left-to-right", "direction")
        expectEqual(t["dipColor"] as? String ?? "", "#000000", "dip colour")
        expect(!(t["operationId"] as? String ?? "").isEmpty, "operation id present")
        expect(t["revision"] as? Int != nil, "revision present")
        expectEqual(cmds[load]["sceneId"] as? String ?? "", "s1", "the taken scene loads as Program")
        expectEqual(model.programSceneId, "s1", "Program is the taken scene")
        expectEqual(model.previewSceneId, "s2", "Preview is the previous Program")
        expectEqual(model.takeStatus, "Program updated", "Windows success status")
    }

    @MainActor
    private static func testTakeTransitionPickerAndNormalizer() {
        expectEqual(AppModel().takeTransitionMode, "fade", "default Fade")
        expectEqual(AppModel.normalizeTakeTransitionMode(nil), "fade", "nil -> fade")
        expectEqual(AppModel.normalizeTakeTransitionMode(" CUT "), "cut", "trim + case")
        expectEqual(AppModel.normalizeTakeTransitionMode("dip"), "dip", "dip kept")
        expectEqual(AppModel.normalizeTakeTransitionMode("wipe"), "wipe", "wipe kept")
        expectEqual(AppModel.normalizeTakeTransitionMode("zoom"), "fade", "unknown -> fade")
        let core = FakeTakeCore()
        let model = takeModel(core)
        model.setTakeTransition("cut")
        expectEqual(model.takeStatus, "Take transition set to Cut", "Windows picker status")
        _ = runTake(model)
        guard let body = core.bodies.first else { expect(false, "Cut Take sent"); return }
        let cmds = body["commands"] as? [[String: Any]] ?? []
        expectEqual(cmds.first?["mode"] as? String ?? "", "cut", "Cut picked -> mode cut")
        model.setTakeTransition("bogus")
        expectEqual(model.takeTransitionMode, "fade", "picker normalizes")
    }

    @MainActor
    private static func testTakeBypassesTheSceneDebounce() {
        let core = FakeTakeCore()
        let model = takeModel(core)
        model.syncScenes()                     // a debounced sync is pending
        _ = runTake(model, wait: false)
        spin(until: { !core.bodies.isEmpty }, limit: 0.08)
        expectEqual(core.bodies.count, 1, "Take reached the core inside the 120 ms debounce")
        spin(until: { false }, limit: 0.3)
        expectEqual(core.bodies.count, 1, "the pending debounced sync was cancelled, not sent after the Take")
    }

    @MainActor
    private static func testTakeFailureRestoresProgramAndPreview() {
        let core = FakeTakeCore()
        core.failure = "native rejected scene"
        let model = takeModel(core)
        let syncsBefore = model.sceneSyncRequestCount
        let box = runTake(model)
        let expected = "Take was not confirmed; previous local Program restored. "
            + "Verify live output before retrying. native rejected scene"
        expectEqual(box.result, .failed(expected), "failed with the Windows rollback text")
        expectEqual(model.takeStatus, expected, "status carries the same text")
        expectEqual(model.programSceneId, "s2", "Program restored")
        expectEqual(model.previewSceneId, "s1", "Preview retained for retry")
        expectEqual(model.sceneSyncRequestCount, syncsBefore + 2,
                    "Take counted once, and the rollback requested a reconcile sync")
        core.failure = nil
        let retry = runTake(model)
        expectEqual(retry.result, .success, "retry succeeds")
        expectEqual(model.programSceneId, "s1", "retry takes")
    }

    @MainActor
    private static func testTakeRollbackRefusedWhenNewerEditsLanded() {
        let core = FakeTakeCore()
        core.hold = true
        core.failure = "gone"
        let model = takeModel(core)
        let box = runTake(model, wait: false)
        spin(until: { !core.bodies.isEmpty }, limit: 2)
        model.previewSceneId = "s3"            // operator re-cues while the Take is pending
        core.hold = false
        spin(until: { box.result != nil }, limit: 3)
        expectEqual(box.result, .failed("Take was not confirmed; newer local edits preserved. "
                                        + "Verify live output. gone"), "Windows preserved-edits text")
        expectEqual(model.previewSceneId, "s3", "the newer cue is kept")
        expectEqual(model.programSceneId, "s1", "Program is not clobbered")
    }

    @MainActor
    private static func testSecondTakeWhileInFlightIsRefused() {
        let core = FakeTakeCore()
        core.hold = true
        let model = takeModel(core)
        let first = runTake(model, wait: false)
        spin(until: { !core.bodies.isEmpty }, limit: 2)
        expect(!model.canTake, "Take is disabled while one is in flight")
        let second = runTake(model)
        expectEqual(second.result, .failed("A Take is already in progress."), "refused")
        expectEqual(core.bodies.count, 1, "no second request")
        expectEqual(model.programSceneId, "s1", "no swap back")
        expectEqual(model.previewSceneId, "s2", "preview not swapped back")
        core.hold = false
        spin(until: { first.result != nil }, limit: 3)
        expectEqual(first.result, .success, "first completes")
        expect(model.canTake, "Take is available again once the first completed")
    }

    @MainActor
    private static func testTakeRefusalsAndOffline() {
        let core = FakeTakeCore()
        let model = takeModel(core)
        model.previewSceneId = "ghost"
        let box = runTake(model)
        expectEqual(box.result, .failed("Take unavailable - select an available Preview scene."),
                    "unavailable Preview refused")
        expect(core.bodies.isEmpty, "nothing sent")
        expectEqual(model.programSceneId, "s2", "Program untouched")
        // Offline: no bridge and no seam.
        let offline = sceneEditModel()
        let off = runTake(offline)
        expectEqual(off.result, .failed("Program selected locally; Take was not sent because the media core is offline."),
                    "offline is not reported as on-air success")
        expectEqual(offline.programSceneId, "s1", "Windows keeps the local selection offline")
    }

    @MainActor
    private static func testTakeIsNotBlockedByTheQuitGuard() {
        let core = FakeTakeCore()
        let model = takeModel(core)
        model.quitFinishing = true     // Windows CanTake has no quit term (only Record/Stream do)
        expect(model.canTake, "CanTake ignores the quit guard")
        expectEqual(runTake(model).result, .success, "Take still runs")
    }

    // ── multiview cue -> Take (owner report 2026-11-15: "choosing a scene in the
    // multiviewer doesn't allow you to take that from preview to program") ───
    // Windows StudioViewModel.PreviewMultiviewTile: the solo scene is a REAL scene
    // entry (EnsureMultiviewSoloScene), the id cued is the one NOT on Program, and
    // each solo scene owns its own route, so a cue never changes what is on air.

    @MainActor
    static func soloModel(_ core: FakeTakeCore) -> AppModel {
        let model = takeModel(core)    // Program s2, Preview s1
        for (index, pid) in ["11", "22", "33"].enumerated() {
            model.slots[index].kind = "zoom"
            model.slots[index].sourceId = pid
            model.slots[index].name = "Guest \(pid)"
        }
        return model
    }

    @MainActor
    static func soloParticipant(_ model: AppModel, _ sceneId: String) -> String {
        model.buildRoutes(for: sceneId).first?["participantId"] as? String ?? "<none>"
    }

    @MainActor
    private static func testMultiviewCueCanBeTaken() {
        let core = FakeTakeCore()
        let model = soloModel(core)
        model.previewMultiviewSlot(1)
        expectEqual(model.previewSceneId, AppModel.soloSceneA, "a cue arms a solo scene on Preview")
        expect(model.canTake, "a multiview cue can be Taken")
        expectEqual(runTake(model).result, .success, "the Take succeeds")
        expectEqual(model.programSceneId, AppModel.soloSceneA, "Program is the solo scene")
        expectEqual(soloParticipant(model, AppModel.soloSceneA), "11", "Program shows the cued source")
        expectEqual(model.previewSceneId, "s2", "the previous Program is on Preview")
    }

    @MainActor
    private static func testMultiviewCueNeverChangesProgram() {
        let core = FakeTakeCore()
        let model = soloModel(core)
        model.previewMultiviewSlot(1)
        _ = runTake(model)              // Program = solo A showing 11
        model.previewMultiviewSlot(2)
        expectEqual(model.previewSceneId, AppModel.soloSceneB,
                    "the cue lands on the solo id NOT on Program")
        expectEqual(soloParticipant(model, AppModel.soloSceneA), "11",
                    "the on-air solo route is untouched by a cue")
        expectEqual(soloParticipant(model, AppModel.soloSceneB), "22", "Preview shows the new cue")
        expect(model.canTake, "the second cue can be Taken")
        expectEqual(runTake(model).result, .success, "second Take succeeds")
        expectEqual(model.programSceneId, AppModel.soloSceneB, "Program is solo B")
        expectEqual(soloParticipant(model, AppModel.soloSceneB), "22", "Program shows 22")
        model.previewMultiviewSlot(3)
        expectEqual(model.previewSceneId, AppModel.soloSceneA, "back to solo A, off air")
        expectEqual(soloParticipant(model, AppModel.soloSceneB), "22", "on-air route still 22")
        expectEqual(soloParticipant(model, AppModel.soloSceneA), "33", "Preview shows 33")
    }

    @MainActor
    private static func testMultiviewRecueResyncsTheSameSoloScene() {
        let core = FakeTakeCore()
        let model = soloModel(core)
        model.previewMultiviewSlot(1)
        let syncs = model.sceneSyncRequestCount
        model.previewMultiviewSlot(2)
        expectEqual(model.previewSceneId, AppModel.soloSceneA,
                    "Program is not solo, so the same solo id is reused (Windows)")
        expectEqual(soloParticipant(model, AppModel.soloSceneA), "22", "its route follows the new cue")
        expect(model.sceneSyncRequestCount > syncs, "the re-cue still syncs the core")
        expect(model.canTake, "the re-cue can be Taken")
    }

    @MainActor
    private static func testTakeRollbackRefusedWhenASoloSceneWasRecuedMidTake() {
        // Windows StudioViewModel.Transport.cs: the rollback snapshots BOTH scenes'
        // routes and refuses if either changed while the Take was pending.
        let core = FakeTakeCore()
        let model = soloModel(core)
        model.previewMultiviewSlot(1)
        _ = runTake(model)              // Program = solo A (11), Preview = s2
        core.hold = true
        core.failure = "gone"
        let box = runTake(model, wait: false)
        spin(until: { core.bodies.count >= 2 }, limit: 2)
        expectEqual(model.programSceneId, "s2", "Take of s2 is pending")
        model.previewMultiviewSlot(2)   // Program is s2 locally -> re-cues solo A
        expectEqual(model.previewSceneId, AppModel.soloSceneA, "the mid-Take cue reuses solo A")
        core.hold = false
        spin(until: { box.result != nil }, limit: 3)
        expectEqual(box.result, .failed("Take was not confirmed; newer local edits preserved. "
                                        + "Verify live output. gone"),
                    "a re-pointed solo scene refuses the rollback")
        expectEqual(model.programSceneId, "s2",
                    "solo A (now showing a source never Taken) is not put back on Program")
        expectEqual(soloParticipant(model, AppModel.soloSceneA), "22", "the newer cue is kept")
    }

    @MainActor
    private static func testSoloSceneCapturesItsSourceWhenCued() {
        // Windows BuildSoloRoute copies the participant / capture device id into
        // the route at cue time; later slot edits never reach it.
        let core = FakeTakeCore()
        let model = soloModel(core)
        model.previewMultiviewSlot(1)
        _ = runTake(model)              // Program = solo A showing 11
        model.slots[0].kind = "unassigned"
        model.slots[0].sourceId = ""
        expectEqual(soloParticipant(model, AppModel.soloSceneA), "11",
                    "unassigning the slot does not black Program")
        model.slots[0].kind = "zoom"
        model.slots[0].sourceId = "44"
        model.slots[0].name = "Guest 44"
        expectEqual(soloParticipant(model, AppModel.soloSceneA), "11",
                    "reassigning the slot does not change Program")
        expectEqual(model.busLabel(for: AppModel.soloSceneA), "Guest 11", "PGM still names the Taken source")
    }

    @MainActor
    private static func testSoloScenesStayOutOfTheRailAndPrefs() {
        let core = FakeTakeCore()
        let model = soloModel(core)
        model.previewMultiviewSlot(1)
        _ = runTake(model)
        model.previewMultiviewSlot(2)
        let solos: Set<String> = [AppModel.soloSceneA, AppModel.soloSceneB]
        expect(model.railScenes.allSatisfy { !solos.contains($0.id) }, "rail hides solo scenes")
        expectEqual(model.railScenes.map(\.id), ["s1", "s2", "s3"], "rail is the operator's scenes")
        expectEqual(model.currentPrefs().scenes.map(\.id), ["s1", "s2", "s3"],
                    "solo scenes are never persisted")
        expectEqual(model.busLabel(for: AppModel.soloSceneA), "Guest 11", "PGM names the source")
        expectEqual(model.busLabel(for: AppModel.soloSceneB), "Guest 22", "PVW names the source")
    }

    // ── Fade to Black (audit §2) + keyboard Take/FTB (audit §5) ──────────────
    // Windows: StudioViewModel.ProgramBlackout.cs (SetProgramBlackoutAsync,
    // ApplyProgramBlackoutSnapshot); Views/StudioWorkspace.TransportHotkeys.cs.

    final class FakeFtbCore: @unchecked Sendable {
        private let lock = NSLock()
        private var _bodies: [[String: Any]] = []
        private var _hold = false
        var pollSnapshot: [String: Any] = ["programBlackout": false, "programBlackoutOpacity": 0.0]
        /// nil = echo the requested target; otherwise this snapshot comes back.
        var commandSnapshot: [String: Any]?
        var failure: String?
        private func record(_ body: [String: Any]) { lock.lock(); _bodies.append(body); lock.unlock() }
        var bodies: [[String: Any]] { lock.lock(); defer { lock.unlock() }; return _bodies }
        var hold: Bool { get { lock.lock(); defer { lock.unlock() }; return _hold }
                         set { lock.lock(); _hold = newValue; lock.unlock() } }
        func handle(_ body: [String: Any]) async throws -> [String: Any] {
            record(body)
            let commands = (body["commands"] as? [[String: Any]]) ?? []
            guard let ftb = commands.first(where: { $0["type"] as? String == "program-blackout" })
            else { return ["snapshot": pollSnapshot] }
            while hold { try? await Task.sleep(nanoseconds: 5_000_000) }
            if let failure { throw BridgeError.remote(failure) }
            if let commandSnapshot { return ["snapshot": commandSnapshot] }
            let target = ftb["enabled"] as? Bool ?? false
            return ["snapshot": ["programBlackout": target, "programBlackoutOpacity": target ? 1.0 : 0.0]]
        }
        var commands: [[String: Any]] {
            bodies.flatMap { ($0["commands"] as? [[String: Any]]) ?? [] }
                .filter { $0["type"] as? String == "program-blackout" }
        }
    }

    @MainActor
    static func ftbModel(_ core: FakeFtbCore) -> AppModel {
        let model = sceneEditModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.status = .connected(renderer: "test")
        return model
    }

    @MainActor
    private static func runFtb(_ model: AppModel, _ enabled: Bool? = nil, wait: Bool = true) -> FtbBox {
        let box = FtbBox()
        Task { @MainActor in box.result = await model.setProgramBlackout(enabled) }
        if wait { spin(until: { box.result != nil }, limit: 3) }
        return box
    }

    final class FtbBox { var result: AppModel.BlackoutResult? }

    @MainActor
    private static func testFtbSendsTheWireCommandAndAppliesTheEcho() {
        let core = FakeFtbCore()
        let model = ftbModel(core)
        let box = runFtb(model)
        expectEqual(box.result, .success, "toggle from clear succeeds")
        expectEqual(core.commands.count, 1, "one program-blackout command")
        let command = core.commands.first ?? [:]
        expectEqual(command["type"] as? String, "program-blackout", "wire type")
        expectEqual(command["enabled"] as? Bool, true, "toggle from clear targets black")
        expectEqual(command["durationMs"] as? Int, 300, "300 ms fade")
        expect(model.programBlackout, "the echoed snapshot is applied")
        expectEqual(model.programBlackoutLabel, "BLACK · Restore", "settled black label")
        expectEqual(model.takeStatus, "Fading Program to black — audio continues", "Windows status")
        // Toggle back: pressing again restores.
        core.pollSnapshot = ["programBlackout": true, "programBlackoutOpacity": 1.0]
        let back = runFtb(model)
        expectEqual(back.result, .success, "toggle from black succeeds")
        expectEqual(core.commands.last?["enabled"] as? Bool, false, "toggle from black targets clear")
        expectEqual(model.takeStatus, "Restoring Program picture", "Windows restore status")
        expect(!model.programBlackout, "restored")
        // Explicit set is idempotent-friendly (remote set action later).
        let set = runFtb(model, true)
        expectEqual(set.result, .success, "explicit set")
        expectEqual(core.commands.last?["enabled"] as? Bool, true, "explicit target wins over toggle")
    }

    @MainActor
    private static func testFtbAdmissionRefusals() {
        // Offline: no bridge and no seam.
        let offline = sceneEditModel()
        expectEqual(runFtb(offline).result, .failed("FTB unavailable: media core is offline."),
                    "offline refused")
        // Older core: snapshot without a boolean programBlackout.
        let core = FakeFtbCore()
        core.pollSnapshot = ["recording": [:]]
        let model = ftbModel(core)
        expectEqual(runFtb(model).result, .failed("This media core does not support Fade to Black."),
                    "no observation: rejected rather than implying success")
        expect(core.commands.isEmpty, "no command sent to an older core")
        core.pollSnapshot = ["programBlackout": "yes"]
        expectEqual(runFtb(model).result, .failed("This media core does not support Fade to Black."),
                    "non-bool observation rejected")
        expect(core.commands.isEmpty, "still nothing sent")
    }

    @MainActor
    private static func testFtbRequiresTheEchoAndReportsFailure() {
        let core = FakeFtbCore()
        core.commandSnapshot = ["programBlackout": false, "programBlackoutOpacity": 0.0]
        let model = ftbModel(core)
        expectEqual(runFtb(model).result, .failed("Media core did not acknowledge FTB."),
                    "an echo that disagrees with the target is not success")
        expect(!model.programBlackout, "state follows the snapshot, not the intent")
        core.commandSnapshot = [:]   // missing observation also fails
        expectEqual(runFtb(model).result, .failed("Media core did not acknowledge FTB."),
                    "missing echo fails")
        core.commandSnapshot = nil
        core.failure = "boom"
        let failed = runFtb(model)
        guard case .failed(let why)? = failed.result else { expect(false, "request failure surfaced"); return }
        expect(why.hasPrefix("FTB unavailable: "), "failure is worded like Windows: \(why)")
        expect(model.takeStatus.hasPrefix("FTB unavailable: "), "status shows it")
        core.failure = nil
        expectEqual(runFtb(model).result, .success, "in-flight flag cleared after a failure")
    }

    @MainActor
    private static func testFtbInFlightGuard() {
        let core = FakeFtbCore()
        core.hold = true
        let model = ftbModel(core)
        let first = runFtb(model, wait: false)
        spin(until: { !core.commands.isEmpty }, limit: 2)
        let second = runFtb(model)
        expectEqual(second.result, .failed("FTB command is already in progress."), "second refused")
        expectEqual(core.commands.count, 1, "no second command")
        core.hold = false
        spin(until: { first.result != nil }, limit: 3)
        expectEqual(first.result, .success, "first completes")
    }

    @MainActor
    private static func testFtbLabelsFromOpacity() {
        // Windows ApplyProgramBlackoutSnapshot, case for case.
        expectEqual(AppModel.programBlackoutLabel(blackout: true, opacity: 0.4), "Fading to black", "fading")
        expectEqual(AppModel.programBlackoutLabel(blackout: true, opacity: 0.998), "Fading to black", "<.999")
        expectEqual(AppModel.programBlackoutLabel(blackout: true, opacity: 0.999), "BLACK · Restore", "settled")
        expectEqual(AppModel.programBlackoutLabel(blackout: true, opacity: 1), "BLACK · Restore", "full")
        expectEqual(AppModel.programBlackoutLabel(blackout: false, opacity: 0.5), "Restoring picture", "restoring")
        expectEqual(AppModel.programBlackoutLabel(blackout: false, opacity: 0.002), "Restoring picture", ">.001")
        expectEqual(AppModel.programBlackoutLabel(blackout: false, opacity: 0.001), "Fade to Black", "clear")
        expectEqual(AppModel.programBlackoutLabel(blackout: false, opacity: 0), "Fade to Black", "clear 0")
        expectEqual(AppModel.programBlackoutLabel(blackout: nil, opacity: 0.9), "Fade to Black", "unobserved")
        let model = AppModel()
        expectEqual(model.programBlackoutLabel, "Fade to Black", "a new session starts clear")
        expect(!model.programBlackout, "no preference engages FTB")
        model.applySnapshotForTesting(["programBlackout": true, "programBlackoutOpacity": 0.5])
        expect(model.programBlackout, "snapshot sets black")
        expectEqual(model.programBlackoutLabel, "Fading to black", "label tracks the snapshot")
        // Another scene Take does not release the blackout (core owns it; shell just observes).
        model.applySnapshotForTesting(["programBlackout": true, "programBlackoutOpacity": 1.0])
        expectEqual(model.programBlackoutLabel, "BLACK · Restore", "settled")
        model.applySnapshotForTesting(["programBlackout": false, "programBlackoutOpacity": 0.5])
        expectEqual(model.programBlackoutLabel, "Restoring picture", "restoring")
        model.applySnapshotForTesting(["recording": [:]])   // no observation -> not black
        expect(!model.programBlackout, "a snapshot without the field reads as not black (Windows == true)")
    }

    @MainActor
    private static func testTakeWhileBlackDoesNotReleaseTheBlackout() {
        let core = FakeTakeCore()
        let model = takeModel(core)
        model.applySnapshotForTesting(["programBlackout": true, "programBlackoutOpacity": 1.0])
        expectEqual(runTake(model).result, .success, "Take while black succeeds")
        expect(!core.types().contains("program-blackout"), "Take never sends a blackout command")
        expect(model.programBlackout, "still black after Take")
    }

    private static func key(_ kind: TransportKeyEvent.Kind, _ mods: TransportKeyEvent.Modifiers = [],
                            studio: Bool = true, text: Bool = false, repeating: Bool = false) -> TransportKeyDecision {
        TransportKeys.decide(TransportKeyEvent(kind: kind, modifiers: mods, isRepeat: repeating,
                                               isStudioTab: studio, isTextEntryFocus: text))
    }

    private static func testTransportKeyRouting() {
        // Space (Windows OnTransportPreviewKeyDown).
        expectEqual(key(.space), .take, "Space takes")
        expectEqual(key(.space, repeating: true), .swallow, "held Space is consumed but fires once")
        expectEqual(key(.space, studio: false), .ignore, "Studio tab only")
        expectEqual(key(.space, text: true), .ignore, "text entry keeps its Space")
        for mod in [TransportKeyEvent.Modifiers.command, .shift, .option, .control] {
            expectEqual(key(.space, mod), .ignore, "any modifier on Space is ignored: \(mod)")
        }
        // Cmd+Return (Windows Ctrl+Enter accelerator).
        expectEqual(key(.returnKey, .command), .alternateTake, "Cmd+Return is the alternate Take")
        expectEqual(key(.returnKey), .ignore, "bare Return is nothing")
        expectEqual(key(.returnKey, [.command, .shift]), .ignore, "extra modifier is not the accelerator")
        expectEqual(key(.returnKey, .command, text: true), .ignore, "text entry ignored")
        expectEqual(key(.returnKey, .command, studio: false), .ignore, "Studio only")
        expectEqual(key(.returnKey, .command, repeating: true), .swallow, "held accelerator does not re-fire")
        // Cmd+Shift+B (Windows Ctrl+Shift+B).
        expectEqual(key(.b, [.command, .shift]), .toggleBlackout, "Cmd+Shift+B is FTB")
        expectEqual(key(.b, .command), .ignore, "Cmd+B alone is not FTB")
        expectEqual(key(.b, [.command, .shift, .option]), .ignore, "exact modifier set")
        expectEqual(key(.b, [.command, .shift], text: true), .ignore, "text entry ignored")
        expectEqual(key(.b, [.command, .shift], studio: false), .ignore, "Studio only")
        expectEqual(key(.b, [.command, .shift], repeating: true), .swallow, "no FTB flapping on key repeat")
        expectEqual(key(.other, .command), .ignore, "other keys untouched")
    }

    @MainActor
    private static func testTransportKeyTextEntryDetection() {
        let field = NSTextField(frame: .zero)
        let editor = NSTextView(frame: .zero)
        let button = NSButton(frame: .zero)
        let secure = NSSecureTextField(frame: .zero)
        expect(TransportKeys.isTextEntry(field), "NSTextField is text entry")
        expect(TransportKeys.isTextEntry(editor), "the field editor (NSTextView) is text entry")
        expect(TransportKeys.isTextEntry(secure), "secure field is text entry")
        let readOnly = NSTextView(frame: .zero)
        readOnly.isEditable = false
        expect(!TransportKeys.isTextEntry(readOnly), "read-only selectable text is not text entry")
        let label = NSTextField(labelWithString: "x")
        expect(!TransportKeys.isTextEntry(label), "a label (non-editable NSTextField) is not text entry")
        expect(!TransportKeys.isTextEntry(button), "a focused button is NOT text entry (Space still Takes)")
        expect(!TransportKeys.isTextEntry(nil), "no responder")
    }

    @MainActor
    private static func testTransportKeyHandlerCallsTheRealCommands() {
        let ftb = FakeFtbCore()
        let model = ftbModel(ftb)
        var takes = 0
        // Space with Take unavailable is still consumed and never fires a Take.
        model.previewSceneId = model.programSceneId
        expect(!model.canTake, "precondition: Take unavailable")
        let consumed = model.handleTransportKey(
            TransportKeyEvent(kind: .space, modifiers: [], isRepeat: false, isStudioTab: true, isTextEntryFocus: false),
            takeOverride: { takes += 1 })
        expect(consumed, "Space is consumed even when Take is unavailable")
        expectEqual(takes, 0, "no Take fired while unavailable")
        // Available: routes into take().
        let core = FakeTakeCore()
        let takeModel = takeModel(core)
        let spaceEvent = TransportKeyEvent(kind: .space, modifiers: [], isRepeat: false,
                                           isStudioTab: true, isTextEntryFocus: false)
        expect(takeModel.handleTransportKey(spaceEvent), "Space handled")
        spin(until: { !core.bodies.isEmpty }, limit: 3)
        expectEqual(core.types(), ["begin-take-transition", "load-scene-graph", "set-preview-scene"],
                    "keyboard Take goes through take() (transition leads the batch)")
        // Alternate Take when unavailable is NOT consumed (Windows CanExecute false -> unhandled).
        let unavailable = model.handleTransportKey(
            TransportKeyEvent(kind: .returnKey, modifiers: .command, isRepeat: false,
                              isStudioTab: true, isTextEntryFocus: false))
        expect(!unavailable, "Cmd+Return unavailable passes through")
        // FTB.
        let fired = model.handleTransportKey(
            TransportKeyEvent(kind: .b, modifiers: [.command, .shift], isRepeat: false,
                              isStudioTab: true, isTextEntryFocus: false))
        expect(fired, "Cmd+Shift+B consumed")
        spin(until: { !ftb.commands.isEmpty }, limit: 3)
        expectEqual(ftb.commands.first?["type"] as? String, "program-blackout", "keyboard FTB sends the wire command")
    }

    @MainActor
    private static func testTransportKeysPassThroughWhileTheQuitPromptIsUp() {
        for (visible, finishing) in [(true, false), (false, true), (true, true)] {
            let ftb = FakeFtbCore()
            let model = ftbModel(ftb)
            let core = FakeTakeCore()
            let tm = takeModel(core)
            for m in [model, tm] { m.quitPromptVisible = visible; m.quitFinishing = finishing }
            for (kind, mods) in [(TransportKeyEvent.Kind.space, TransportKeyEvent.Modifiers()),
                                 (.returnKey, [.command]), (.b, [.command, .shift])] {
                let event = TransportKeyEvent(kind: kind, modifiers: mods, isRepeat: false,
                                              isStudioTab: true, isTextEntryFocus: false)
                expect(!tm.handleTransportKey(event), "\(kind) passes through (prompt \(visible)/\(finishing))")
                expect(!model.handleTransportKey(event), "\(kind) not consumed on FTB model")
            }
            spin(until: { false }, limit: 0.2)
            expect(core.bodies.isEmpty, "no Take fired under the quit prompt")
            expect(ftb.commands.isEmpty, "no FTB fired under the quit prompt")
        }
    }

    // Windows 1.0 nav (StudioWorkspace.xaml): one "Produce" group, Studio, Zoom,
    // Sources, Scenes, Routing, Graphics, Audio, (Comms), Media, Health, (OHG
    // Show). Comms/OHG are not on Mac 1.0. Settings trails (no Windows
    // counterpart: Windows' Zoom button opens its connection page, whose
    // content is Mac's Zoom pane; Mac Settings is output/recording config).
    @MainActor
    private static func testNavigationMatchesWindowsOrderAndLabels() {
        expectEqual(StudioTab.navigation(commsEnabled: false).map(\.navLabel),
                    ["Studio", "Zoom", "Sources", "Scenes", "Routing", "Graphics",
                     "Audio", "Media", "Health", "Settings"],
                    "Mac nav order + labels follow Windows 1.0")
        expectEqual(StudioTab.navigation(commsEnabled: false).map(\.rawValue),
                    ["Studio", "Zoom", "Sources", "Scenes", "Routing", "Overlays",
                     "Audio", "Media", "Diagnose", "Settings"],
                    "tab identifiers are unchanged")
        expect(!StudioTab.navigation(commsEnabled: false).contains(.automation), "no Automation pill")
        expectEqual(StudioTab.navGroups, ["Produce"], "one group label")
    }

    @MainActor
    private static func testTabNamesResolveWithAliases() {
        expectEqual(StudioTab(name: "Overlays"), .overlays, "old name Overlays")
        expectEqual(StudioTab(name: "graphics"), .overlays, "Graphics alias")
        expectEqual(StudioTab(name: "Diagnose"), .diagnose, "old name Diagnose")
        expectEqual(StudioTab(name: "HEALTH"), .diagnose, "Health alias")
        expectEqual(StudioTab(name: "settings"), .settings, "Settings")
        expectEqual(StudioTab(name: "zoom"), .zoom, "case-insensitive")
        expect(StudioTab(name: "nope") == nil, "unknown name")
    }

    @MainActor
    private static func testTransportMonitorLifecycleFollowsTheStudioTab() {
        let model = AppModel()
        model.selectedTab = .zoom
        model.syncTransportHotkeys()
        expect(!model.transportHotkeys.isInstalled, "not installed off Studio")
        model.selectedTab = .studio
        model.syncTransportHotkeys()
        expect(model.transportHotkeys.isInstalled, "installed on Studio")
        model.syncTransportHotkeys()
        expect(model.transportHotkeys.isInstalled, "second sync keeps one monitor")
        model.selectedTab = .audio
        model.syncTransportHotkeys()
        expect(!model.transportHotkeys.isInstalled, "removed when leaving Studio")
        model.selectedTab = .studio
        model.syncTransportHotkeys()
        expect(model.transportHotkeys.isInstalled, "reinstalled on return")
        model.transportHotkeys.remove()
    }

    @MainActor
    private static func testFtbRefusalsSurfaceAsStatusForDirectCallers() {
        let offline = sceneEditModel()
        _ = runFtb(offline)
        expectEqual(offline.takeStatus, "FTB unavailable: media core is offline.", "offline status set by the shared path")
        let core = FakeFtbCore()
        core.hold = true
        let model = ftbModel(core)
        let first = runFtb(model, wait: false)
        spin(until: { !core.commands.isEmpty }, limit: 2)
        _ = runFtb(model)
        expectEqual(model.takeStatus, "FTB command is already in progress.", "in-flight status set by the shared path")
        core.hold = false
        spin(until: { first.result != nil }, limit: 3)
        let old = FakeFtbCore()
        old.pollSnapshot = [:]
        let om = ftbModel(old)
        _ = runFtb(om)
        expectEqual(om.takeStatus, "This media core does not support Fade to Black.", "unsupported status")
    }

    /// Final-review IMPORTANT 7: Cmd-Q with the main window closed must still
    /// show something the operator can answer, and a repeat Cmd-Q re-presents it.
    @MainActor
    private static func testQuitPromptIsAlwaysAnswerable() {
        let core = FakeQuitCore()
        let (model, _) = quitModel(core: core)
        var shown: [QuitPromptPlacement] = []
        model.quitPresentForTesting = { shown.append($0) }
        model.recordingStatus = "recording"
        model.recordingDesired = true
        model.mainWindowAvailableForTesting = false
        expect(model.handleQuitRequest() == .terminateLater, "live recording asks")
        expectEqual(shown, [.fallbackPanel], "no main window: the prompt goes in its own panel")
        expect(model.handleQuitRequest() == .terminateCancel, "a repeat quit is not a force-exit")
        expectEqual(shown, [.fallbackPanel, .fallbackPanel], "a repeat quit re-presents the pending prompt")
        model.cancelQuit()
        model.mainWindowAvailableForTesting = true
        shown = []
        _ = model.handleQuitRequest()
        expectEqual(shown, [.mainWindow], "with the main window available the prompt shows there")
        model.cancelQuit()
        // While outputs finish there is nothing to answer: a repeat quit does
        // not re-open a prompt (the finishing notice is already up).
        shown = []
        model.mainWindowAvailableForTesting = false
        core.hang = true
        model.quitFinishTimeout = 0.3
        _ = model.handleQuitRequest()
        model.confirmQuit()
        shown = []
        expect(model.handleQuitRequest() == .terminateCancel, "a quit while finishing is ignored")
        expectEqual(shown, [.fallbackPanel], "the finishing notice is brought forward, never a second prompt")
        spin(until: { !model.quitFinishing }, limit: 4)
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


    // ── footer health line + multiview placeholder (Task 3) ──────────────────
    // Ported from CoreVideoPro.MediaCore.Tests StalledFeedsReadoutTests /
    // StreamDegradationReadoutTests and WinUI.Tests MultiviewEmptyStatePolicyTests.

    private static func hSource(_ id: String, _ kind: String, _ health: String,
                                age: Double? = 2000, hasVideo: Bool = true) -> JSONObject {
        var row: JSONObject = ["sourceId": id, "kind": kind, "health": health, "hasVideo": hasVideo]
        row["sinceLastNewFrameMs"] = age.map { $0 as Any } ?? NSNull()
        return row
    }
    private static func hZoom(_ id: String, _ age: Double = 2000) -> JSONObject {
        hSource(id, "zoom", "stalled", age: age)
    }
    private static func hSnap(_ bp: JSONObject?, _ sources: [JSONObject], status: String = "live",
                              fps: Int = 60, lifecycle: String? = nil) -> JSONObject {
        var sender: JSONObject = ["senderId": "rtmp", "destination": "rtmp", "status": status]
        if let bp { sender["backpressure"] = bp }
        if let lifecycle { sender["lifecycle"] = ["state": lifecycle] }
        return ["outputProfile": ["fps": fps],
                "outputSenderSession": ["status": status, "senders": [sender]],
                "sources": sources]
    }
    private static let hLevel1Queue445: JSONObject =
        ["divisor": 2, "appliedDivisor": 1, "level": 1, "bufferedMs": 445]

    private static func testStalledFeedsReadout() {
        // The 2026-10-31 live-show shape: no claimed cause.
        let line = StudioHealthReadout.stalledFeeds(hSnap(hLevel1Queue445,
            [hZoom("1"), hZoom("2"), hZoom("3"), hSource("4", "zoom", "producing", age: 16)]))
        expectEqual(line, "Stream 445 ms behind and 3 Zoom feeds stalled", "live-show line")
        expect(!(line ?? "").localizedCaseInsensitiveContains("network")
               && !(line ?? "").localizedCaseInsensitiveContains("congest"), "never claims a cause")
        let rate: JSONObject = ["divisor": 3, "appliedDivisor": 3, "level": 2, "bufferedMs": 900]
        expectEqual(StudioHealthReadout.stalledFeeds(hSnap(rate, [hZoom("1"), hZoom("2")])),
                    "Stream at 1/3 rate and 2 Zoom feeds stalled", "applied divisor reads as a rate")
        let drained: JSONObject = ["divisor": 2, "appliedDivisor": 1, "level": 1, "bufferedMs": 0]
        expectEqual(StudioHealthReadout.stalledFeeds(hSnap(drained, [hZoom("1"), hZoom("2")])),
                    "Stream backpressure level 1 and 2 Zoom feeds stalled", "drained queue reports level")
        // Static screen captures never light the line.
        let captures = [hSource("capture:screen-1", "capture", "stalled", age: 120_000),
                        hSource("capture:screen-2", "capture", "stalled", age: 300_000)]
        expectEqual(StudioHealthReadout.stalledFeedCount(captures), 0, "captures are not feeds")
        expect(StudioHealthReadout.stalledFeeds(hSnap(hLevel1Queue445, captures)) == nil, "captures: no line")
        // On-air 1.5 s edges.
        for (age, counted) in [(200.0, false), (1499.0, false), (1500.0, true), (3000.0, true)] {
            let rows = [hZoom("1", age), hZoom("2", age)]
            expectEqual(StudioHealthReadout.stalledFeedCount(rows), counted ? 2 : 0, "age \(age) count")
            expectEqual(StudioHealthReadout.stalledFeeds(hSnap(hLevel1Queue445, rows)) != nil, counted,
                        "age \(age) line")
        }
        expectEqual(StudioHealthReadout.stalledFeedCount(
            [hSource("1", "zoom", "stalled", age: nil), hSource("2", "zoom", "stalled", age: nil)]), 0,
            "row without age never counted")
        expect(StudioHealthReadout.stalledFeeds(hSnap(hLevel1Queue445,
            [hZoom("1"), hSource("2", "zoom", "producing", age: 16)])) == nil, "one stalled feed is one guest's problem")
        let healthy: JSONObject = ["divisor": 1, "appliedDivisor": 1, "level": 0]
        expect(StudioHealthReadout.stalledFeeds(hSnap(healthy, [hZoom("1"), hZoom("2")])) == nil,
               "no pressure: nothing")
        expect(StudioHealthReadout.stalledFeeds(hSnap(nil, [hZoom("1"), hZoom("2")])) == nil,
               "no backpressure node: nothing")
        for status in ["starting", "failed", "idle"] {
            expect(StudioHealthReadout.stalledFeeds(
                hSnap(hLevel1Queue445, [hZoom("1"), hZoom("2")], status: status)) == nil,
                   "non-producing sender (\(status)) is not backpressure")
        }
        let mixed = [hSource("still:logo", "still", "stalled", age: 60_000),
                     hSource("media:clip", "media", "stalled", age: 60_000),
                     hSource("tiles:s", "composed", "stalled", age: 60_000),
                     hSource("zoom-slate", "zoom-slate", "stalled", age: 60_000),
                     hSource("9", "zoom", "stalled", age: 60_000, hasVideo: false), hZoom("1")]
        expectEqual(StudioHealthReadout.stalledFeedCount(mixed), 1, "only zoom video feeds count")
        expect(StudioHealthReadout.stalledFeeds(hSnap(hLevel1Queue445, mixed)) == nil, "mixed: one feed, no line")
        // Core wire shape (SourceVideoIngress.cpp / CoreStateCapture.cpp), through real JSON.
        let json = """
        {"sources":[{"sourceId":"12","kind":"zoom","hasVideo":true,"health":"stalled","sinceLastNewFrameMs":1730},
                    {"sourceId":"13","kind":"zoom","hasVideo":false,"health":"warming","sinceLastNewFrameMs":null},
                    {"sourceId":"14","kind":"zoom","hasVideo":true,"health":"stalled","sinceLastNewFrameMs":1900}],
         "outputProfile":{"fps":60},
         "outputSenderSession":{"status":"live","senders":[{"senderId":"rtmp","destination":"rtmp","status":"live",
           "lifecycle":{"state":"producing"},
           "backpressure":{"divisor":2,"appliedDivisor":1,"level":1,"bufferedMs":445,"queuedChunks":3}}]}}
        """
        let wire = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? JSONObject ?? [:]
        expectEqual(StudioHealthReadout.stalledFeeds(wire),
                    "Stream 445 ms behind and 2 Zoom feeds stalled", "wire shape binds")
        // Both readouts agree on what pressure is.
        let snap = hSnap(hLevel1Queue445, [hZoom("1"), hZoom("2")])
        expect(StudioHealthReadout.streamDegradation(snap) != nil && StudioHealthReadout.stalledFeeds(snap) != nil,
               "both readouts see the pressure")
        let recovered = hSnap(["divisor": 1, "appliedDivisor": 1, "level": 0], [hZoom("1"), hZoom("2")])
        expect(StudioHealthReadout.streamDegradation(recovered) == nil
               && StudioHealthReadout.stalledFeeds(recovered) == nil, "recovery clears both")
    }

    private static func testStreamDegradationReadout() {
        for (fps, divisor, expected) in [(60, 4, "15 fps feed"), (30, 4, "7.5 fps feed"), (25, 3, "8.33 fps feed")] {
            let line = StudioHealthReadout.streamDegradation(
                hSnap(["appliedDivisor": divisor, "level": 0], [], fps: fps))
            expect((line ?? "").contains(expected), "cadence \(fps)/\(divisor) -> \(expected), got \(line ?? "nil")")
        }
        expectEqual(StudioHealthReadout.streamDegradation(
            hSnap(["divisor": 4, "appliedDivisor": 1, "level": 3, "bufferedMs": 0], [])),
            "RTMP degraded · 60 fps feed", "requested divisor does not pretend the feed was reduced")
        for status in ["starting", "failed", "idle"] {
            expect(StudioHealthReadout.streamDegradation(
                hSnap(["appliedDivisor": 4, "level": 3], [], status: status)) == nil,
                   "\(status) does not advertise degradation")
        }
        expect(StudioHealthReadout.streamDegradation(hSnap(["appliedDivisor": 1, "level": 0], [])) == nil,
               "recovery clears")
        expect((StudioHealthReadout.streamDegradation(hSnap(["appliedDivisor": 4, "level": 3], [], fps: 0)) ?? "")
            .contains("feed rate unknown"), "fps 0 unknown")
        expect((StudioHealthReadout.streamDegradation(hSnap(["appliedDivisor": 0, "level": 3], [])) ?? "")
            .contains("feed rate unknown"), "divisor 0 unknown")
        expect(StudioHealthReadout.streamDegradation(
            hSnap(["appliedDivisor": 4, "level": 3], [], lifecycle: "interrupted")) == nil,
               "lifecycle other than producing is not degraded streaming")
    }

    private static func testMultiviewEmptyStatePolicy() {
        typealias P = MultiviewEmptyStatePolicy
        let slate = P.Surface(status: "Waiting for video frames")
        expect(P.showAssignmentGuidance(tileCount: 0, surface: nil), "empty, no surface: guidance")
        expect(P.showAssignmentGuidance(tileCount: 0, surface: slate), "empty generic slate: guidance")
        expect(!P.showAssignmentGuidance(tileCount: 2, surface: slate), "populated slate: no guidance")
        expect(!P.showAssignmentGuidance(tileCount: 0, surface: P.Surface(status: "Renderer failed")),
               "failure status stays visible")
        expect(!P.showAssignmentGuidance(tileCount: 0, surface: P.Surface(status: "Capture paused")),
               "unknown status stays visible")
        for generic in ["Waiting for video frames", "Preparing video", "Preparing Program"] {
            expect(P.showAssignmentGuidance(tileCount: 0, surface: P.Surface(status: generic)), "\(generic) is generic")
        }
        var picture = slate; picture.hasPreviewBitmap = true
        expect(!P.showAssignmentGuidance(tileCount: 0, surface: picture), "available picture never covered")
        var frames = slate; frames.hasFrames = true
        expect(!P.showAssignmentGuidance(tileCount: 0, surface: frames), "published frame keeps surface")
        var gpu = slate; gpu.awaitingGpuHandle = true
        expect(!P.showAssignmentGuidance(tileCount: 0, surface: gpu), "pending GPU handle keeps surface")
        var media = slate; media.mediaAssetPath = "/x.mp4"
        expect(!P.showAssignmentGuidance(tileCount: 0, surface: media), "media path keeps surface")
        // The Mac mapping: a published IOSurface is a picture.
        expect(P.showAssignmentGuidance(tileCount: 0, multiviewSurfaceId: 0), "no IOSurface: guidance")
        // An allocated IOSurface with no tiles holds no real picture yet (grid, nothing
        // assigned): the guidance stays. The core publishes no multiview frame signal.
        expect(P.showAssignmentGuidance(tileCount: 0, multiviewSurfaceId: 7),
               "surface present but 0 tiles: guidance still shown")
        expect(!P.showAssignmentGuidance(tileCount: 3, multiviewSurfaceId: 0), "tiles present: no guidance")
    }

    // ── CameraExtensionKit (roadmap M9.2) ────────────────────────────────────
    private static func testStandbySlateMatchesTheCoreSlate() {
        // Fixture invariant: 1080/12 = 90, top = (1080-90)/2 = 495, bottom = 585.
        let s = StandbySlate.nv12(width: 1920, height: 1080)!
        expectEqual(s.count, 3_110_400, "NV12 size")
        expectEqual(s[494 * 1920], 24, "row above bar is field")
        expectEqual(s[495 * 1920], 130, "first bar row")
        expectEqual(s[584 * 1920 + 1919], 130, "last bar row, last column")
        expectEqual(s[585 * 1920], 24, "row below bar is field")
        expect(s[(1920 * 1080)...].allSatisfy { $0 == 128 }, "neutral chroma")
        expect(StandbySlate.nv12(width: 1919, height: 1080) == nil, "odd width refused")
        expect(StandbySlate.nv12(width: 1920, height: 1081) == nil, "odd height refused")
        expect(StandbySlate.nv12(width: 0, height: 1080) == nil, "zero width refused")
        expect(StandbySlate.nv12(width: 1920, height: -2) == nil, "negative height refused")
    }
    private static func testFrameSelectorHoldAndStop() {
        var f = FrameSelector()
        expectEqual(f.choice(atNanoseconds: 10), .slate, "never delivered -> slate")
        f.producerDelivered(atNanoseconds: 1_000_000_000)
        expectEqual(f.choice(atNanoseconds: 1_500_000_000), .program, "exactly 500 ms -> still program")
        expectEqual(f.choice(atNanoseconds: 1_500_000_001), .slate, "500 ms + 1 ns -> slate")
        expectEqual(f.choice(atNanoseconds: 999_999_999), .slate, "now before delivery -> slate (fail toward law 2)")
        f.producerDelivered(atNanoseconds: 2_000_000_000); f.producerStopped()
        expectEqual(f.choice(atNanoseconds: 2_000_000_001), .slate, "stop -> slate immediately, no hold")
        f.producerDelivered(atNanoseconds: 3_000_000_000)
        expectEqual(f.choice(atNanoseconds: 3_000_000_001), .program, "delivery after stop -> program")
    }
    private static func testDeviceNamePolicy() {
        expectEqual(DeviceNamePolicy.sanitized("  Studio A \n"), "Studio A", "trimmed")
        expectEqual(DeviceNamePolicy.sanitized("   "), "CoreVideo Pro Camera", "blank -> default")
        expectEqual(DeviceNamePolicy.sanitized("A\u{0007}B"), "AB", "control chars dropped")
        expectEqual(DeviceNamePolicy.sanitized(String(repeating: "\u{00E9}", count: 80)).unicodeScalars.count, 64, "capped at 64 scalars")
    }
    private static func testSlateClassifier() {
        func classify(_ plane: [UInt8], _ w: Int, _ h: Int) -> Bool {
            plane.withUnsafeBufferPointer {
                SlateClassifier.isSlate(yPlane: $0, width: w, height: h, bytesPerRow: w)
            }
        }
        let w = 1280, h = 720
        let slate = StandbySlate.nv12(width: w, height: h)!
        expect(classify(Array(slate[0..<(w * h)]), w, h), "the real slate is a slate")
        expect(!classify([UInt8](repeating: 82, count: w * h), w, h), "BT.601 red (Y 82) is not a slate")
        expect(!classify([UInt8](repeating: 24, count: w * h), w, h), "flat dark, no bar, is not a slate")
        // Flat Y 130 everywhere: the middle row matches, rows h/4 and 3h/4 do not.
        expect(!classify([UInt8](repeating: 130, count: w * h), w, h), "flat 130, no dark field, is not a slate")
        // Padded rows: bytesPerRow > width must be honoured.
        let stride = w + 64
        var padded = [UInt8](repeating: 0, count: stride * h)
        for row in 0..<h { for col in 0..<w { padded[row * stride + col] = slate[row * w + col] } }
        let paddedResult = padded.withUnsafeBufferPointer {
            SlateClassifier.isSlate(yPlane: $0, width: w, height: h, bytesPerRow: stride)
        }
        expect(paddedResult, "padded stride is honoured")
    }

    @MainActor
    static func runAndExit() -> Never {
        // Plan D4: the harness never reads or writes the operator's graphics show.
        setenv("COREVIDEO_GRAPHICS_DOCUMENT_PATH",
               (NSTemporaryDirectory() as NSString).appendingPathComponent("cvp-shell-tests-graphics-\(getpid())/default.json"), 1)
        // Before ANY test: a run whose prefs resolve to the real file aborts.
        ShellHarness.requireIsolatedPrefsOrExit()
        failures = []
        checks = 0
        // No test runs a real core binary as a camera probe (cases that test
        // the probe name their own executable).
        FreshProcessCameraDeviceProbe.defaultExecutable = { nil }
        // No test touches a real secret: every KeychainStore user (stream key,
        // Zoom tokens, later feed credentials) gets an in-memory Keychain.
        KeychainStore.defaultBackend = MemoryKeychain()

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
            ("oauth/loopback-request-line", testOAuthLoopbackParsesTheRequestLine),
            ("oauth/loopback-page", testOAuthLoopbackPageSaysWhatHappened),
            ("oauth/loopback-lifecycle", testOAuthLoopbackLifecycleIsOneShot),
            ("oauth/loopback-broker-url", testOAuthBrokerUrlCarriesTheLoopbackReturnUri),
            ("oauth/loopback-end-to-end", testOAuthLoopbackListenerEndToEnd),
            ("zoom/join-status-late-truth", testZoomJoinStatusPrefersZoomsReasonOverATimeout),
            ("zoom/join-status-rejoin", testZoomJoinStatusIgnoresTheOldFailureDuringARejoin),
            ("oauth/state-before-error", testOAuthCallbackChecksStateBeforeError),
            ("encoder/no-unlicensed-codecs", testShellOffersOnlyEncodableCodecs),
            ("telemetry/dropped-frames", testDroppedFrameReadout),
            ("chromakey/enabled-only", testChromaKeyNodeOnlySentWhenEnabled),
            ("chromakey/colour-parsing", testKeyColourParsing),
            ("budget/order", testBudgetProgramOutranksTheWall),
            ("budget/camera-off", testBudgetCameraOffSpendsNothing),
            ("budget/iso-dedupe-determinism", testBudgetIsoTierAndDeterminism),
            ("budget/route-shape", testRoutedZoomPidsReadTheRealRouteShape),
            ("budget/wire-shape", testSpineSubscriptionWireShape),
            ("multiview/source-wire-shape", testMultiviewSourceWireShape),
            ("zoom-audio/spine-subscribes-audio", testSpineSubscribesTheMeetingMixAndSourceAudio),
            ("zoom-audio/source-set", testAudioSourceSetKeepsCameraOffAndRefusesNonSources),
            ("zoom-audio/kinds-vs-core", testAudioSubscriptionKindsMatchTheCoreSource),
            ("zoom-audio/push-spine-call-site", testPushSpineSubscribesWallAudioFromTheModel),
            ("audio-console/strips-from-source-set", testStripsComeFromTheSourceSetNotOnlyTheEcho),
            ("audio-console/exactly-modelled-sources", testStripSetIsExactlyTheModelledSources),
            ("audio-console/unpaired-mic-leaves", testUnpairedMicStripLeavesTheConsole),
            ("audio-console/a1-mute-481", testA1MuteIsNeverAdoptedFromTheEcho),
            ("audio-console/sync-carries-every-strip", testConsoleSyncCarriesEveryStripAndTheA1State),
            ("audio-routing/default-topology", testDefaultRoutingIsTheWindowsPerGuestTopology),
            ("audio-routing/model-mon-bus", testModelRoutesEverySourceToTheMonitorBus),
            ("audio-console/core-restart-reasserts", testCoreRestartReassertsTheWholeAudioConsole),
            ("audio-console/pre-mute-meter", testMutedTalkingGuestShowsADimmedPreMuteMeter),
            ("audio-console/roster-order-stable", testStripOrderIsRosterOrderAndStableAcrossACue),
            ("audio-console/mute-survives-source-exit", testMutedGuestStaysMutedAcrossASourceSetExit),
            ("audio-console/meeting-end-forgets", testMeetingEndForgetsPerGuestAudioState),
            ("audio-routing/slider-coalesced", testSendGainDragCoalescesTheRoutingPush),
            ("audio-routing/media-follows-program", testMediaRoutingFollowsTheProgramMediaRoute),
            ("zoom-audio/roster-change-repushes-spine", testRosterMembershipChangeRepushesTheSpine),
            ("audio-console/only-terminal-states-forget", testOnlyTerminalMeetingStatesForgetGuestAudio),
            ("audio-console/returning-mute-ahead-of-spine", testReturningMutedGuestsMuteGoesAheadOfTheSpine),
            ("dropout/after-scene-graph", testSourcePoliciesFollowTheSceneGraphInTheBatch),
            ("iso/take-survives-roster-loss", testIsoTakeSurvivesRosterLossAcrossStreamToggle),
            ("iso/take-operator-deselect", testIsoTakeOperatorDeselectIsAuthoritative),
            ("iso/next-take-refilters", testIsoNextTakeFiltersByPresenceAgain),
            ("iso/display-read-pure", testIsoDisplayReadDoesNotConsumeOperatorEdit),
            ("iso/selection-port", testRecordingIsoSelectionPort),
            ("iso/preflight-loud", testIsoPreflightWarnsLoudly),
            ("iso/preflight-unobserved", testIsoPreflightNeverWarnsOnAnUnobservedState),
            ("vcam-ext/error-mapping", testCameraExtensionErrorMapping),
            ("vcam-ext/preflight", testCameraExtensionPreflight),
            ("vcam-ext/replacement", testCameraExtensionReplacement),
            ("vcam-ext/properties", testCameraExtensionProperties),
            ("vcam-ext/settings-link", testCameraExtensionSettingsLink),
            ("vcam-ext/fake", testCameraExtensionFakeRecordsCalls),
            ("vcam-ext/prefs-field", testCameraExtensionPrefsField),
            ("vcam-ext/reducer", testCameraExtensionReducerSequences),
            ("vcam-ext/activator-isolation", testCameraExtensionActivatorKeepsRequestsApart),
            ("vcam/label-precedence", testVirtualCameraLabelPrecedence),
            ("vcam/read-model-core-shape", testVirtualCameraReadModelParsesTheCoreShape),
            ("vcam/wire-vs-core", testSyncVirtualCameraWire),
            ("vcam/prefs-round-trip-older-file", testVirtualCameraPrefsRoundTripAndOlderFile),
            ("vcam/resent-each-core-generation", testVirtualCameraResentOnNewCoreGeneration),
            ("vcam/activation-wiring", testVirtualCameraActivationWiring),
            ("vcam/remove-deactivates-and-clears-opt-in", testRemoveVirtualCameraCallsDeactivateAndClearsOptIn),
            ("vcam/remove-failure-keeps-opt-in-and-reports", testRemoveVirtualCameraFailureKeepsOptInAndReports),
            ("vcam/toggle-track-visible", testStudioToggleTrackIsVisibleInBothStates),
            ("vcam/unchanged-name-sends-nothing", testVirtualCameraUnchangedNameSendsNothing),
            ("vcam/proof-needs-snapshot", testVirtualCameraProofNeedsTheSnapshotVariable),
            ("overlays/brand-kit-payload", testBrandKitCommandCarriesOnlyWhatTheCoreApplies),
            ("overlays/old-prefs-decode", testOldOverlayPrefsWithRemovedFieldsStillDecode),
            ("overlays/brand-default-all-off-leaves-manual", testBrandDefaultAllOffLeavesTheManualLowerThirdAlone),
            ("overlays/brand-default-noops", testBrandDefaultManualAndUnknownAreNoOps),
            ("overlays/lower-third-retire-race", testLowerThirdRetireIsIdempotentAndShowWins),
            ("overlays/brand-default-options", testBrandDefaultOptionsAreWindows),
            ("overlays/brand-default-not-on-sync", testBrandDefaultsAreNotRerunBySyncs),
            ("scenes/rename-keeps-fields", testSceneRenameKeepsEveryOtherField),
            ("scenes/rename-policy", testSceneRenameValidationIsTheWindowsPolicy),
            ("scenes/duplicate-copies-all", testSceneDuplicateCopiesEveryField),
            ("scenes/delete-refusals", testSceneDeleteRefusesOnAirAndLast),
            ("scenes/harness-prefs-isolated", testHarnessNeverTouchesRealPrefs),
            ("harness/one-predicate-and-abort", testHarnessPredicateIsOneAndAbortsOnRealPrefs),
            ("scenes/editor-never-program", testSceneEditorNeverTargetsProgram),
            ("meters/observation-thresholds", testAudioObservationThresholdsAreWindows),
            ("meters/warning-recovers", testMeterWarningFollowsSnapshotAgeAndRecovers),
            ("meters/poll-interval-allowance", testMeterWarningAllowsTheShellPollInterval),
            ("footer/readouts-windows", testStudioFooterReadoutsAreWindows),
            ("footer/height-ignores-status", testStudioFooterHeightIgnoresStatus),
            ("quit/decision", testQuitDecisionAsksOnlyWhenOutputsAreLive),
            ("quit/one-prompt", testQuitGateAllowsOnePromptAtATime),
            ("quit/stop-then-wait", testQuitFinishStopsThenWaitsForTerminalEvidence),
            ("quit/bridge-app-exit", testBridgeAppExitClosesStdinAndGivesTheCoreAGraceBeforeTerminating),
            ("quit/deadline-hung-stop", testQuitDeadlineHoldsWhenAStopRequestHangs),
            ("quit/flow-order-terminal", testQuitFlowStopsInOrderAndFinishesAtTheTerminalSnapshot),
            ("quit/finalizing-never-reopens-recording", testQuitWhileFinalizingNeverReopensARecording),
            ("quit/whole-branch-iso-stream-stop-quit", testWholeBranchQuitAfterStoppingAnIsoTakeWithAStreamUp),
            ("quit/toggles-refused-while-quitting", testOutputTogglesRefuseWhileQuitting),
            ("quit/prompt-always-answerable", testQuitPromptIsAlwaysAnswerable),
            ("take/batch-order-fields", testTakeBatchOrderAndFields),
            ("take/picker-normalizer", testTakeTransitionPickerAndNormalizer),
            ("take/bypasses-debounce", testTakeBypassesTheSceneDebounce),
            ("take/failure-rollback", testTakeFailureRestoresProgramAndPreview),
            ("take/rollback-refused-newer-edits", testTakeRollbackRefusedWhenNewerEditsLanded),
            ("take/in-flight-refused", testSecondTakeWhileInFlightIsRefused),
            ("take/refusals-offline", testTakeRefusalsAndOffline),
            ("take/multiview-cue-can-be-taken", testMultiviewCueCanBeTaken),
            ("take/multiview-cue-never-changes-program", testMultiviewCueNeverChangesProgram),
            ("take/multiview-recue-resyncs", testMultiviewRecueResyncsTheSameSoloScene),
            ("take/solo-scenes-out-of-rail-and-prefs", testSoloScenesStayOutOfTheRailAndPrefs),
            ("take/rollback-refused-on-mid-take-solo-recue", testTakeRollbackRefusedWhenASoloSceneWasRecuedMidTake),
            ("take/solo-scene-captures-source-at-cue", testSoloSceneCapturesItsSourceWhenCued),
            ("take/not-blocked-by-quit", testTakeIsNotBlockedByTheQuitGuard),
            ("ftb/wire-and-echo", testFtbSendsTheWireCommandAndAppliesTheEcho),
            ("ftb/admission-refusals", testFtbAdmissionRefusals),
            ("ftb/echo-required", testFtbRequiresTheEchoAndReportsFailure),
            ("ftb/in-flight-guard", testFtbInFlightGuard),
            ("ftb/labels-from-opacity", testFtbLabelsFromOpacity),
            ("ftb/take-while-black", testTakeWhileBlackDoesNotReleaseTheBlackout),
            ("keys/routing-decision", testTransportKeyRouting),
            ("keys/text-entry-detection", testTransportKeyTextEntryDetection),
            ("keys/handler-real-commands", testTransportKeyHandlerCallsTheRealCommands),
            ("keys/quit-prompt-passthrough", testTransportKeysPassThroughWhileTheQuitPromptIsUp),
            ("nav/windows-order", testNavigationMatchesWindowsOrderAndLabels),
            ("nav/tab-aliases", testTabNamesResolveWithAliases),
            ("keys/monitor-lifecycle", testTransportMonitorLifecycleFollowsTheStudioTab),
            ("ftb/refusal-status", testFtbRefusalsSurfaceAsStatusForDirectCallers),
            ("health/stalled-feeds", testStalledFeedsReadout),
            ("health/stream-degradation", testStreamDegradationReadout),
            ("multiview/empty-state-policy", testMultiviewEmptyStatePolicy),
            ("quit/cancel", testQuitCancelRepliesFalseOnceAndSendsNothing),
            ("quit/idle", testQuitIdleQuitsImmediately),
            ("quit/dead-core", testQuitWithADeadCoreFinishesImmediately),
            ("quit/core-hang-deadline", testQuitDeadlineIsHonouredWhenTheCoreHangs),
            ("quit/timeout-resend", testQuitFinishTimesOutLoudlyAndResendsAStopThatDidNotLand),
            ("control/closure", testControlClosure),
            ("control/dispatch-drive", testControlEverySupportedIdIsDispatched),
            ("control/unavailable-422", testControlUnavailableActionsAnswer422),
            ("control/manifest-shared-file", testControlManifestIsTheSharedFileFiltered),
            ("control/fixtures", testControlSharedFixtures),
            ("control/hardening-timeouts", testControlHardeningTimeouts),
            ("control/hardening-connection-cap", testControlHardeningConnectionCap),
            ("control/hardening-no-preflight", testControlHardeningNoPreflight),
            ("control/invoke-rejections", testControlInvokeRejectsLikeWindows),
            ("control/invoke-bind-and-replies", testControlInvokeBindsAndShapesReplies),
            ("control/snapshot-envelope", testControlSnapshotEnvelopeIsWindows),
            ("control/snapshot-redaction", testControlSnapshotRedactionIsWindows),
            ("control/auth-and-bind-policy", testControlAuthAndBindPolicyAreWindows),
            ("control/http-parser-bounds", testControlHttpParserBounds),
            ("control/surface-model-paths", testControlSurfaceRoutesThroughTheModel),
            ("control/surface-virtualcam", testControlSurfaceVirtualCameraActions),
            ("control/surface-ftb-snapshot", testControlSurfaceFtbAndSnapshot),
            ("control/server-end-to-end", testControlServerEndToEnd),
            ("control/server-auth-wire", testControlServerAuthOverTheWire),
            ("control/server-real-model", testControlServerDrivesTheRealModel),
            ("control/state-stream-throttle", testControlStateStreamIsAThrottleNotADebounce),
            ("control/state-stream-event-tracking", testControlStateStreamDeliversDuringEventTracking),
            ("lifecycle/start-is-idempotent", testStartRunsItsSetupExactlyOncePerProcess),
            ("control/ws-origin-rule", testControlWebSocketOriginRule),
            ("control/ws-slow-client-dropped", testControlSlowWebSocketClientIsDropped),
            ("camera-kit/standby-slate", testStandbySlateMatchesTheCoreSlate),
            ("camera-kit/frame-selector", testFrameSelectorHoldAndStop),
            ("camera-kit/device-name", testDeviceNamePolicy),
            ("camera-kit/slate-classifier", testSlateClassifier),
            ("camera-kit/producer-attestation", testProducerAttestation),
        ] + tier0Cases + vcamUpdateCases + mediaCases + sourcesCases + controlParityCases + controlOscServerCases + gradeCases + graphicsFontsCases + graphicsCases + graphicsGatewayCases + graphicsOperateCases + graphicsDocumentCases + graphicsPlayoutCases + graphicsPlaylistCases + graphicsHotkeyCases + graphicsTakeCases + graphicsAuthorityCases + graphicsLiveCases + graphicsReviewCases + graphicsDesignResyncCases + graphicsLayoutParityCases + graphicsNamesCases + graphicsClockCases + audioPhase2Cases + controlPhase2Cases + quickWinCases + streamingCases + gradePhase2Cases + sourcesPhase2Cases + commsCases + commsModelCases + commsWiringCases + commsModelReviewCases + commsPageCases + commsKeysCases + controlRC7Cases + leftoverCases + socialCases + socialPlayoutCases + socialSheetCases + socialLiveCases + tilesCases + tilesLiveCases
        // COREVIDEO_SHELL_TESTS_FILTER=<prefix> runs only the matching cases (mutation runs,
        // iteration); COREVIDEO_SHELL_TEST_FILTER is accepted too. Unset, every case runs.
        let env = ProcessInfo.processInfo.environment
        let filter = env["COREVIDEO_SHELL_TESTS_FILTER"] ?? env["COREVIDEO_SHELL_TEST_FILTER"] ?? ""
        for (name, body) in cases where filter.isEmpty || name.hasPrefix(filter) {
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
