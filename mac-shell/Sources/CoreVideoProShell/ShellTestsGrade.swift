// macOS per-source colour grading, Phase 1 (docs/superpowers/plans/2026-11-16-mac-grading-phase1.md).
// Kept in its own file so the parallel Mac branches that also extend ShellTests.swift do not
// collide line-for-line. ContractParity scans this file: no `"type": "<name>"` dictionary
// literal may appear here for a request the core does not handle.

import Foundation

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var gradeCases: [(String, () -> Void)] {
        [
            // Task 4: model, identity, saved-grade file.
            ("grade/model-validation-and-summary", testSourceGradeValidationAndSummaryAreWindows),
            ("grade/model-wire-shape", testSourceGradeWireShape),
            ("grade/identity-keys-and-bindings", testGradeIdentityKeysAndBindings),
            ("grade/roster-carries-persistent-identity", testRosterCarriesPersistentIdentity),
            ("grade/file-tolerant-capped-ordered", testSourceGradeFileIsTolerantCappedAndOrdered),
            ("grade/file-harness-isolation", testSourceGradeFileIsIsolatedUnderTheHarness),
            // Task 5: SourceGradeStore, the only sender.
            ("grade/store-queries-once", testGradeApplyQueriesOnceThenUsesTheCoresEpochAndRevision),
            ("grade/store-no-frames", testGradeApplyWithNoFramesSaysSoAndSendsNothing),
            ("grade/store-capture-epoch-zero", testCaptureEpochZeroIsAppliedNotTreatedAsAbsent),
            ("grade/store-one-in-flight", testOneApplyInFlightAndTheNewestPendingWins),
            ("grade/store-conflict-retry-once", testAConflictIsRequeriedAndRetriedExactlyOnce),
            ("grade/store-wrong-person", testTheWrongPersonIsRefusedBeforeAnythingIsSent),
            ("grade/store-generation-fence", testACoreRespawnDuringAPendingApplyCannotRecordItsAcknowledgement),
            ("grade/store-respawn-reassert", testCoreRespawnReassertsSessionGradesOnceFramesReturn),
            ("grade/store-epoch-same-person", testAnEpochChangeReassertsForTheSamePersonOnly),
            ("grade/store-meeting-end", testMeetingEndDropsZoomSessionGradesButNotCapture),
            ("grade/store-follows-person", testGradeFollowsThePersonToANewParticipantId),
            ("grade/store-persist-debounce-and-clear", testAcknowledgedGradesPersistByStableKeyAfterTheDebounce),
            ("grade/store-relaunch-capture-vs-zoom", testRelaunchReappliesCaptureButZoomOnlyAsDraft),
            ("grade/model-onconnected-reasserts", testOnConnectedAndSnapshotsReassertThroughTheModel),
            ("grade/model-meeting-end", testMeetingEndThroughTheModelForgetsZoomGrades),
        ]
    }

    // ── Task 4 ───────────────────────────────────────────────────────────────

    static func testSourceGradeValidationAndSummaryAreWindows() {
        var g = SourceColorGrade(lut: "warm-film", exposure: 2, contrast: 0, saturation: -3, temperature: 7)
        expectEqual(g.summary, "Look warm-film · exposure +2 · contrast 0 · saturation -3",
                    "Windows ColorGrade.Summary (ProductionModels.cs:838-841)")
        expect((try? g.validated(range: SourceColorGrade.editorRange)) != nil, "±50 is valid in the editor")
        g.exposure = 51
        expect((try? g.validated(range: SourceColorGrade.editorRange)) == nil, "51 is outside the editor's −50…+50")
        expect((try? g.validated(range: SourceColorGrade.wireRange)) != nil, "51 is inside the core's ±100")
        g.exposure = 101
        do { _ = try g.validated(range: SourceColorGrade.wireRange); expect(false, "101 must be refused") }
        catch let e as GradeValidationError { expectEqual(e.message, "Unsupported Basic grade.", "Windows paste message") }
        catch { expect(false, "unexpected error type") }
        g.exposure = 0; g.lut = "teal-orange"
        expect((try? g.validated(range: SourceColorGrade.wireRange)) == nil, "an unknown look is refused (core: unsupported-look)")
        expectEqual(SourceColorGrade.looks, ["none", "neutral", "warm-film", "cool-broadcast", "punch"],
                    "Windows LutOptions, same order (ColorGradeEditorViewModel.cs:12-13)")
        expect(SourceColorGrade().isNeutral, "the default grade is neutral")
        expect(!SourceColorGrade(lut: "neutral").isNeutral,
               "the 'neutral' look adds contrast (ColorGradeParams.h:21), so it is not neutral")
    }

    static func testSourceGradeWireShape() {
        let g = SourceColorGrade(lut: "punch", exposure: -4, contrast: 1, saturation: 2, temperature: 3)
        let wire = g.wireGrade(documentRef: nil)
        expectEqual(wire["lut"] as? String, "punch", "look at the grade root")
        expectEqual(wire["exposure"] as? Int, -4, "exposure")
        expectEqual(wire["temperature"] as? Int, 3, "temperature")
        expect(wire["advanced"] == nil, "no advanced key without a document")
        let ref = String(repeating: "a", count: 64)
        let withDocument = g.wireGrade(documentRef: ref)
        expectEqual((withDocument["advanced"] as? JSONObject)?["documentRef"] as? String, ref,
                    "a registered document travels by reference (JsonRpcServer.cpp:278-290)")
    }

    static func testGradeIdentityKeysAndBindings() {
        let cam = GradeSourceIdentity.capture(deviceId: "cam-1")
        expectEqual(cam.sourceId, "capture:cam-1", "capture frame key (AvfCaptureDeviceAdapter.mm:226-227)")
        expectEqual(cam.stableKey, "capture:cam-1", "Windows GradePersistenceKey for capture")
        expectEqual(cam.binding, "capture:cam-1", "a capture binding is the hardware id")
        let guest = GradeSourceIdentity.zoom(participantId: "16778240", persistentId: "P-abc", sourceGeneration: 3)
        expectEqual(guest.sourceId, "16778240", "a Zoom grade targets the RAW participant id, never zoom:<pid>")
        expectEqual(guest.stableKey, "zoom-person:P-abc", "Windows GradePersistenceKey (StudioViewModel.Grading.cs:16-17)")
        expectEqual(guest.binding, "person:P-abc", "a Zoom binding is the person")
        let reused = GradeSourceIdentity.zoom(participantId: "16778240", persistentId: "P-xyz", sourceGeneration: 4)
        expect(reused.binding != guest.binding, "a reused id with another person never shares a binding")
        let anonymous = GradeSourceIdentity.zoom(participantId: "16778240", persistentId: "", sourceGeneration: 3)
        expect(anonymous.stableKey == nil, "no persistentId → never persisted (Windows returns null)")
        expectEqual(anonymous.binding, "gen:16778240:3", "no persistentId → id plus source generation")
        let regenerated = GradeSourceIdentity.zoom(participantId: "16778240", persistentId: "", sourceGeneration: 4)
        expect(regenerated.binding != anonymous.binding, "a new source generation breaks an anonymous binding")
    }

    /// Fixture corrected (see the grading Phase 1 report): base 38d01ffd already parses both
    /// fields (Sources Phase 1) as `persistentId: String?` (nil when absent or empty) and
    /// `sourceGeneration: Int64`. The plan's "absent → empty string" predates that.
    static func testRosterCarriesPersistentIdentity() {
        let parsed = RosterParticipant.parse([
            ["userId": "16778240", "displayName": "Guest", "persistentId": "P-abc", "sourceGeneration": 3],
            ["userId": "16778241", "displayName": "Legacy"],
        ])
        expectEqual(parsed.first?.persistentId, "P-abc", "engine shape (ZoomEngineRuntime.cpp:1443)")
        expectEqual(parsed.first?.sourceGeneration, 3, "engine shape (ZoomEngineRuntime.cpp:1444)")
        expect(parsed.last?.persistentId == nil, "absent → nil")
        expectEqual(parsed.last?.sourceGeneration, 0, "absent → 0")
    }

    static func testSourceGradeFileIsTolerantCappedAndOrdered() {
        // Fixture: 66 valid capture keys c00…c65 listed oldest-first in `order`, one malformed entry,
        // one foreign key. Invariant: every valid key appears in `order` exactly once.
        var grades: [String: Any] = [:]
        var order: [String] = []
        for i in 0..<66 {
            let key = String(format: "capture:c%02d", i)
            grades[key] = ["lut": "none", "exposure": i % 50, "contrast": 0, "saturation": 0, "temperature": 0]
            order.append(key)
        }
        grades["capture:bad"] = ["lut": 5]
        order.insert("capture:bad", at: 0)
        grades["media:x"] = ["lut": "none", "exposure": 0, "contrast": 0, "saturation": 0, "temperature": 0]
        let dir = NSTemporaryDirectory() + "grade-file-\(UUID().uuidString)"
        let path = dir + "/source-grades.json"
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let data = try! JSONSerialization.data(withJSONObject: ["version": 1, "grades": grades, "order": order])
        try! data.write(to: URL(fileURLWithPath: path))

        var (file, notices) = SourceGradeFile.load(from: path)
        expectEqual(file.grades.count, 64, "Take(64) (StudioViewModel.Grading.cs:24)")
        expectEqual(file.order.first, "capture:c00", "oldest valid key first")
        expectEqual(file.order.last, "capture:c63", "the first 64 valid keys in order are kept")
        expect(file.grades["media:x"] == nil, "a foreign key is dropped")
        expectEqual(notices.filter { $0.hasPrefix("Saved grade unavailable: ") }.count, 1,
                    "the malformed entry is named once (Grading.cs:27)")
        expectEqual(file.grades["capture:c07"]?.exposure, 7, "values survive")

        let notice = file.remember(SourceColorGrade(exposure: 9), key: "capture:new")
        expectEqual(notice, " · oldest saved source grade retired (64-source limit)", "Grading.cs:151")
        expect(file.grades["capture:c00"] == nil, "the oldest is retired")
        expectEqual(file.order.last, "capture:new", "the newest is last")
        expect(file.remember(SourceColorGrade(exposure: 1), key: "capture:c10") == nil, "re-saving a key retires nothing")
        expectEqual(file.order.last, "capture:c10", "re-saving moves the key to the end")
        expectEqual(file.grades.count, 64, "still at capacity")

        try! file.save(to: path)
        let (reloaded, _) = SourceGradeFile.load(from: path)
        expectEqual(reloaded, file, "round trip is exact")
    }

    static func testSourceGradeFileIsIsolatedUnderTheHarness() {
        expect(SourceGradeFile.path != SourceGradeFile.realPath,
               "a harness run can never read or replace the operator's saved grades")
        expect(SourceGradeFile.realPath.hasSuffix("/Library/Application Support/CoreVideoPro/source-grades.json"),
               "spec §5.9 location")
    }

    // ── Task 5 ───────────────────────────────────────────────────────────────

    /// Mirrors native SourceGradeState (apply refuses identity before revision,
    /// SourceGradeState.h) and register-grade-document (JsonRpcServer.cpp).
    /// Fixture invariants: epochs[id] != nil ⇔ the core saw a frame for id in the LAST tick
    /// (observe replaces the map every tick); revisions start at 0 in every core process;
    /// respawn() is a new process: no frames, no grades, no documents.
    /// Honors the D6 `clear: true` contract (plan rule 9): a cleared source is no longer graded
    /// and its revision keeps counting.
    final class FakeGradeCore: @unchecked Sendable {
        private let lock = NSLock()
        private var _epochs: [String: UInt64] = [:]
        private var _revisions: [String: Int64] = [:]
        private var _graded: Set<String> = []
        private var _documents: Set<String> = []
        private var _bodies: [JSONObject] = []
        private var _hold = false
        private var _refusals: [String] = []
        private func locked<T>(_ body: () -> T) -> T { lock.lock(); defer { lock.unlock() }; return body() }
        func setEpoch(_ id: String, _ epoch: UInt64?) { locked { _epochs[id] = epoch } }
        func setRevision(_ id: String, _ revision: Int64) { locked { _revisions[id] = revision } }
        var hold: Bool { get { locked { _hold } } set { locked { _hold = newValue } } }
        func refuseNext(_ reasons: String...) { locked { _refusals.append(contentsOf: reasons) } }
        func respawn() { locked { _epochs = [:]; _revisions = [:]; _graded = []; _documents = [] } }
        var bodies: [JSONObject] { locked { _bodies } }
        var documents: Set<String> { locked { _documents } }
        func isGraded(_ id: String) -> Bool { locked { _graded.contains(id) } }
        func requests(_ type: String) -> [JSONObject] { bodies.filter { $0["type"] as? String == type } }
        static func int(_ value: Any?) -> Int64? { (value as? NSNumber)?.int64Value }

        func handle(_ body: JSONObject) async throws -> JSONObject {
            locked { _bodies.append(body) }
            let id = body["sourceId"] as? String ?? ""
            switch body["type"] as? String {
            case "query-source-grade":
                return locked { ["ok": true, "present": _epochs[id] != nil,
                                 "sourceEpoch": NSNumber(value: _epochs[id] ?? 0),
                                 "revision": NSNumber(value: _revisions[id] ?? 0)] }
            case "set-source-grade":
                while hold { try? await Task.sleep(nanoseconds: 5_000_000) }
                return locked { () -> JSONObject in
                    if !_refusals.isEmpty { return ["ok": true, "accepted": false, "reason": _refusals.removeFirst()] }
                    guard let epoch = Self.int(body["sourceEpoch"]), let expected = Self.int(body["expectedRevision"]) else {
                        return ["ok": true, "accepted": false, "reason": "invalid-source-grade"]
                    }
                    guard let live = _epochs[id], live == UInt64(epoch) else {
                        return ["ok": true, "accepted": false, "reason": "source-identity-changed"]
                    }
                    let current = _revisions[id] ?? 0
                    guard expected == current else {
                        return ["ok": true, "accepted": false, "reason": "grade-revision-conflict", "revision": NSNumber(value: current)]
                    }
                    if let advanced = (body["grade"] as? JSONObject)?["advanced"] as? JSONObject,
                       let ref = advanced["documentRef"] as? String, !_documents.contains(ref) {
                        return ["ok": true, "accepted": false, "reason": "invalid-advanced-grade"]
                    }
                    _revisions[id] = current + 1
                    if body["clear"] as? Bool == true { _graded.remove(id) } else { _graded.insert(id) }
                    return ["ok": true, "accepted": true, "revision": NSNumber(value: current + 1), "sourceEpoch": NSNumber(value: epoch)]
                }
            default:
                // register-grade-document returns with Task 9 (GradeDocumentRef).
                return ["ok": true]
            }
        }
    }

    final class IdentityBox { var ids: [String: GradeSourceIdentity]; init(_ ids: [String: GradeSourceIdentity]) { self.ids = ids } }

    @MainActor static func tempGradeFile() -> String {
        NSTemporaryDirectory() + "grade-store-\(UUID().uuidString)/source-grades.json"
    }
    @MainActor static func gradeStore(_ core: FakeGradeCore, _ box: IdentityBox, file: String? = nil) -> SourceGradeStore {
        let store = SourceGradeStore(send: { try await core.handle($0) }, filePath: file ?? tempGradeFile())
        store.identityProvider = { box.ids[$0] }
        return store
    }
    /// Every grade test that builds an AppModel uses this, so no case can see another's saves.
    @MainActor static func gradeModel(file: String? = nil) -> AppModel {
        AppModel.gradeFilePathForTesting = file ?? tempGradeFile()
        defer { AppModel.gradeFilePathForTesting = nil }
        return AppModel()
    }
    @MainActor static func awaitApply(_ store: SourceGradeStore, _ grade: SourceColorGrade,
                                      _ sourceId: String, binding: String) -> GradeApplyOutcome? {
        var outcome: GradeApplyOutcome?
        Task { outcome = await store.apply(grade, sourceId: sourceId, authoredFor: binding) }
        spin(until: { outcome != nil }, limit: 3)
        return outcome
    }
    /// A sourceGrades row. `active` is DERIVED with the core's rule so no fixture can contradict it.
    static func gradeRow(_ id: String, revision: Int64, applied: UInt64, live: UInt64, present: Bool = true) -> JSONObject {
        ["sourceId": id, "revision": NSNumber(value: revision), "appliedEpoch": NSNumber(value: applied),
         "liveEpoch": NSNumber(value: present ? live : 0), "livePresent": present, "active": present && applied == live]
    }

    @MainActor static func testGradeApplyQueriesOnceThenUsesTheCoresEpochAndRevision() {
        let core = FakeGradeCore(); core.setEpoch("16778240", 7)          // fixture: the engine's epoch is 7
        let guest = GradeSourceIdentity.zoom(participantId: "16778240", persistentId: "P-a", sourceGeneration: 1)
        let store = gradeStore(core, IdentityBox(["16778240": guest]))
        expectEqual(awaitApply(store, SourceColorGrade(exposure: 3), "16778240", binding: guest.binding),
                    .applied(revision: 1), "first apply acknowledged at revision 1")
        expectEqual(core.requests("query-source-grade").count, 1, "the epoch was unknown, so the store asked once")
        let first = core.requests("set-source-grade").first ?? [:]
        expectEqual(FakeGradeCore.int(first["sourceEpoch"]), 7, "the core's epoch, not 0")
        expectEqual(FakeGradeCore.int(first["expectedRevision"]), 0, "a fresh core starts at revision 0")
        expect(first["payload"] == nil, "root-level request (JsonRpcServer.cpp set-source-grade)")
        expect(first["clear"] == nil, "a non-neutral grade is never a clear")
        expectEqual(awaitApply(store, SourceColorGrade(exposure: 4), "16778240", binding: guest.binding),
                    .applied(revision: 2), "second apply")
        expectEqual(core.requests("query-source-grade").count, 1, "a known epoch is not re-queried")
        expectEqual(core.requests("set-source-grade").count, 2, "no conflict round-trip: the stored revision was right")
    }

    @MainActor static func testGradeApplyWithNoFramesSaysSoAndSendsNothing() {
        let core = FakeGradeCore()
        let cam = GradeSourceIdentity.capture(deviceId: "cam")
        let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
        expectEqual(awaitApply(store, SourceColorGrade(exposure: 3), "capture:cam", binding: cam.binding),
                    .refused("Source has no video yet; grade not applied."), "spec §5.10")
        expectEqual(core.requests("set-source-grade").count, 0, "nothing sent for an absent source")
    }

    @MainActor static func testCaptureEpochZeroIsAppliedNotTreatedAsAbsent() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let cam = GradeSourceIdentity.capture(deviceId: "cam")
        let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
        expectEqual(awaitApply(store, SourceColorGrade(exposure: 3), "capture:cam", binding: cam.binding),
                    .applied(revision: 1), "Mac capture frames carry epoch 0 and are present")
        expectEqual(FakeGradeCore.int(core.requests("set-source-grade").first?["sourceEpoch"]), 0, "sent with epoch 0")
    }

    @MainActor static func testOneApplyInFlightAndTheNewestPendingWins() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let cam = GradeSourceIdentity.capture(deviceId: "cam")
        let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
        core.hold = true
        var outcomes: [Int: GradeApplyOutcome] = [:]
        for exposure in 1...3 {
            Task { outcomes[exposure] = await store.apply(SourceColorGrade(exposure: exposure), sourceId: "capture:cam", authoredFor: cam.binding) }
            RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        }
        spin(until: { core.requests("set-source-grade").count == 1 }, limit: 2)
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        expectEqual(core.requests("set-source-grade").count, 1, "one apply in flight per source")
        core.hold = false
        spin(until: { outcomes.count == 3 }, limit: 3)
        let sets = core.requests("set-source-grade")
        expectEqual(sets.count, 2, "exactly one follow-up apply")
        expectEqual(FakeGradeCore.int((sets.last?["grade"] as? JSONObject)?["exposure"]), 3, "the follow-up carries the newest edit")
        expectEqual(outcomes[2], .superseded, "the replaced edit says so")
        expectEqual(outcomes[3], .applied(revision: 2), "the newest edit is acknowledged")
    }

    @MainActor static func testAConflictIsRequeriedAndRetriedExactlyOnce() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let cam = GradeSourceIdentity.capture(deviceId: "cam")
        let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
        _ = awaitApply(store, SourceColorGrade(exposure: 1), "capture:cam", binding: cam.binding)
        core.setRevision("capture:cam", 5)                   // the store's revision 1 is now stale
        expectEqual(awaitApply(store, SourceColorGrade(exposure: 2), "capture:cam", binding: cam.binding),
                    .applied(revision: 6), "re-queried and retried on the core's revision (D7)")
        expectEqual(core.requests("set-source-grade").map { FakeGradeCore.int($0["expectedRevision"]) }, [0, 1, 5],
                    "stale 1 refused, then 5")
        core.refuseNext("grade-revision-conflict", "grade-revision-conflict")
        expectEqual(awaitApply(store, SourceColorGrade(exposure: 3), "capture:cam", binding: cam.binding),
                    .refused("Grade was not applied: grade-revision-conflict. Previous live grade retained."),
                    "a second refusal is shown verbatim (Apply.cs:50)")
        expectEqual(core.requests("set-source-grade").count, 5, "exactly one retry, never a loop")
        core.refuseNext("grade-capacity")
        expectEqual(awaitApply(store, SourceColorGrade(exposure: 4), "capture:cam", binding: cam.binding),
                    .refused("Grade was not applied: grade-capacity. Previous live grade retained."), "not retryable")
        expectEqual(core.requests("set-source-grade").count, 6, "no retry for a non-identity refusal")
    }

    @MainActor static func testTheWrongPersonIsRefusedBeforeAnythingIsSent() {
        let core = FakeGradeCore(); core.setEpoch("16778240", 7)
        let box = IdentityBox(["16778240": .zoom(participantId: "16778240", persistentId: "P-a", sourceGeneration: 1)])
        let store = gradeStore(core, box)
        _ = awaitApply(store, SourceColorGrade(exposure: 1), "16778240", binding: "person:P-a")
        box.ids["16778240"] = .zoom(participantId: "16778240", persistentId: "P-b", sourceGeneration: 2)
        expectEqual(awaitApply(store, SourceColorGrade(exposure: 2), "16778240", binding: "person:P-a"),
                    .refused("Source identity changed. Close and reopen its grade editor before applying."), "Apply.cs:26")
        expectEqual(core.requests("set-source-grade").count, 1, "nothing sent for the wrong person")
        box.ids["16778240"] = .zoom(participantId: "16778240", persistentId: "P-a", sourceGeneration: 2)
        core.setEpoch("16778240", 8)                          // same person, new stream
        expectEqual(awaitApply(store, SourceColorGrade(exposure: 3), "16778240", binding: "person:P-a"),
                    .applied(revision: 2), "identity-changed is re-queried and retried for the same person")
        expectEqual(core.requests("set-source-grade").map { FakeGradeCore.int($0["sourceEpoch"]) }, [7, 7, 8],
                    "stale epoch refused, then the live one")
    }

    @MainActor static func testACoreRespawnDuringAPendingApplyCannotRecordItsAcknowledgement() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let cam = GradeSourceIdentity.capture(deviceId: "cam")
        let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
        core.hold = true
        var outcome: GradeApplyOutcome?
        Task { outcome = await store.apply(SourceColorGrade(exposure: 9), sourceId: "capture:cam", authoredFor: cam.binding) }
        spin(until: { core.requests("set-source-grade").count == 1 }, limit: 2)
        store.coreGenerationChanged()
        core.hold = false
        spin(until: { outcome != nil }, limit: 2)
        expectEqual(outcome, .stale("Source changed during apply. Grade acknowledgement is no longer current; review the draft again."),
                    "Windows Apply.cs:49; port of AdvancedGradeEditorTests.cs:63")
        expect(!store.isGraded("capture:cam"), "an old generation's acknowledgement is never the session grade")
        store.observe(sourceGrades: [], sources: [cam], now: Date(timeIntervalSince1970: 3_000))
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        expectEqual(core.requests("set-source-grade").count, 1, "and it is not re-asserted on the new core")
    }

    @MainActor static func testCoreRespawnReassertsSessionGradesOnceFramesReturn() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let cam = GradeSourceIdentity.capture(deviceId: "cam")
        let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
        _ = awaitApply(store, SourceColorGrade(lut: "warm-film", exposure: 5), "capture:cam", binding: cam.binding)
        core.respawn()
        store.coreGenerationChanged()
        let t0 = Date(timeIntervalSince1970: 1_000)
        store.observe(sourceGrades: [], sources: [cam], now: t0)
        spin(until: { core.requests("query-source-grade").count >= 2 }, limit: 2)
        expectEqual(core.requests("set-source-grade").count, 1, "no frames yet (present:false): nothing sent")
        store.observe(sourceGrades: [], sources: [cam], now: t0.addingTimeInterval(0.2))
        RunLoop.current.run(until: Date().addingTimeInterval(0.15))
        expectEqual(core.requests("query-source-grade").count, 2, "re-queries are paced at 500 ms")
        core.setEpoch("capture:cam", 0)
        store.observe(sourceGrades: [], sources: [cam], now: t0.addingTimeInterval(0.6))
        spin(until: { core.requests("set-source-grade").count >= 2 }, limit: 2)
        expectEqual(core.requests("set-source-grade").count, 2, "one re-assert (the first set also had revision 0)")
        expect(core.isGraded("capture:cam"), "the respawned core grades the source again")
        let reassert = core.requests("set-source-grade").last ?? [:]
        expectEqual(FakeGradeCore.int(reassert["expectedRevision"]), 0, "the new core's revision, not the old 1")
        expectEqual((reassert["grade"] as? JSONObject)?["lut"] as? String, "warm-film", "the session grade itself")
        expectEqual(FakeGradeCore.int((reassert["grade"] as? JSONObject)?["exposure"]), 5, "every axis")
    }

    @MainActor static func testAnEpochChangeReassertsForTheSamePersonOnly() {
        let core = FakeGradeCore(); core.setEpoch("16778240", 7)
        let box = IdentityBox(["16778240": .zoom(participantId: "16778240", persistentId: "P-a", sourceGeneration: 1)])
        let store = gradeStore(core, box)
        _ = awaitApply(store, SourceColorGrade(exposure: 6), "16778240", binding: "person:P-a")
        let t0 = Date(timeIntervalSince1970: 2_000)
        store.observe(sourceGrades: [gradeRow("16778240", revision: 1, applied: 7, live: 7)], sources: Array(box.ids.values), now: t0)
        RunLoop.current.run(until: Date().addingTimeInterval(0.15))
        expectEqual(core.requests("set-source-grade").count, 1, "an active grade is never re-sent")
        core.setEpoch("16778240", 8)                          // join-ramp size change, same person
        store.observe(sourceGrades: [gradeRow("16778240", revision: 1, applied: 7, live: 8)], sources: Array(box.ids.values),
                      now: t0.addingTimeInterval(1))
        spin(until: { core.requests("set-source-grade").count >= 2 }, limit: 2)
        let reassert = core.requests("set-source-grade").last ?? [:]
        expectEqual(FakeGradeCore.int(reassert["sourceEpoch"]), 8, "re-asserted on the live epoch (D4)")
        expectEqual(FakeGradeCore.int(reassert["expectedRevision"]), 1, "against the core's revision")
        box.ids["16778240"] = .zoom(participantId: "16778240", persistentId: "P-b", sourceGeneration: 2)
        core.setEpoch("16778240", 9)
        store.observe(sourceGrades: [gradeRow("16778240", revision: 2, applied: 8, live: 9)], sources: Array(box.ids.values),
                      now: t0.addingTimeInterval(2))
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        expectEqual(core.requests("set-source-grade").count, 2, "P-a's grade never lands on P-b")
        expectEqual(store.summary(for: "16778240")?.identityChanged, true, "the store says the binding broke")
    }

    @MainActor static func testMeetingEndDropsZoomSessionGradesButNotCapture() {
        let core = FakeGradeCore(); core.setEpoch("101", 3); core.setEpoch("capture:cam", 0)
        let guest = GradeSourceIdentity.zoom(participantId: "101", persistentId: "P-a", sourceGeneration: 1)
        let cam = GradeSourceIdentity.capture(deviceId: "cam")
        let store = gradeStore(core, IdentityBox(["101": guest, "capture:cam": cam]))
        _ = awaitApply(store, SourceColorGrade(exposure: 2), "101", binding: guest.binding)
        _ = awaitApply(store, SourceColorGrade(exposure: 4), "capture:cam", binding: cam.binding)
        store.meetingEnded()
        expect(!store.isGraded("101"), "Zoom reuses ids per meeting: the session grade goes")
        expect(store.isGraded("capture:cam"), "a capture grade is not a meeting's")
        core.respawn(); core.setEpoch("101", 3); core.setEpoch("capture:cam", 0)
        store.coreGenerationChanged()
        store.observe(sourceGrades: [], sources: [guest, cam], now: Date(timeIntervalSince1970: 4_000))
        spin(until: { core.requests("set-source-grade").count >= 3 }, limit: 2)
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        let resent = core.requests("set-source-grade").dropFirst(2).map { $0["sourceId"] as? String }
        expectEqual(resent, ["capture:cam"], "only the capture grade is re-asserted")
    }

    @MainActor static func testGradeFollowsThePersonToANewParticipantId() {      // D5
        let core = FakeGradeCore(); core.setEpoch("101", 3)
        let box = IdentityBox(["101": .zoom(participantId: "101", persistentId: "P-a", sourceGeneration: 1)])
        let store = gradeStore(core, box)
        _ = awaitApply(store, SourceColorGrade(exposure: 7), "101", binding: "person:P-a")
        box.ids = ["205": .zoom(participantId: "205", persistentId: "P-a", sourceGeneration: 2)]
        core.setEpoch("101", nil); core.setEpoch("205", 4)
        store.observe(sourceGrades: [gradeRow("101", revision: 1, applied: 3, live: 0, present: false)],
                      sources: Array(box.ids.values), now: Date(timeIntervalSince1970: 5_000))
        spin(until: { core.requests("set-source-grade").count >= 2 }, limit: 2)
        let moved = core.requests("set-source-grade").last ?? [:]
        expectEqual(moved["sourceId"] as? String, "205", "the grade followed P-a to the new id")
        expectEqual(FakeGradeCore.int((moved["grade"] as? JSONObject)?["exposure"]), 7, "the same grade")
        expect(store.isGraded("205") && !store.isGraded("101"), "the session grade moved, not copied")
    }

    /// D6 overridden (clear implemented): Reset sends `clear: true`, the source has no session
    /// grade afterwards (it takes the Program look again), and the saved key is forgotten. The
    /// plan's draft asserted "stays the session grade"; that was the D6 default being replaced.
    @MainActor static func testAcknowledgedGradesPersistByStableKeyAfterTheDebounce() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0); core.setEpoch("777", 2)
        let cam = GradeSourceIdentity.capture(deviceId: "cam")
        let anonymous = GradeSourceIdentity.zoom(participantId: "777", persistentId: "", sourceGeneration: 1)
        let file = tempGradeFile()
        let store = gradeStore(core, IdentityBox(["capture:cam": cam, "777": anonymous]), file: file)
        _ = awaitApply(store, SourceColorGrade(exposure: 5), "capture:cam", binding: cam.binding)
        _ = awaitApply(store, SourceColorGrade(exposure: 8), "777", binding: anonymous.binding)
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        expect(!FileManager.default.fileExists(atPath: file), "not written before the 750 ms debounce")
        spin(until: { FileManager.default.fileExists(atPath: file) }, limit: 2)
        let (saved, _) = SourceGradeFile.load(from: file)
        expectEqual(saved.grades["capture:cam"]?.exposure, 5, "saved under the stable key")
        expectEqual(saved.grades.count, 1, "a Zoom source without a persistentId is never persisted")
        // Reset (neutral) is a clear: the core forgets the grade, the store forgets both.
        expectEqual(awaitApply(store, SourceColorGrade(), "capture:cam", binding: cam.binding), .applied(revision: 2),
                    "a clear is acknowledged as a new revision")
        expectEqual(core.requests("set-source-grade").last?["clear"] as? Bool, true, "neutral is sent as clear:true")
        expect(!core.isGraded("capture:cam"), "the core no longer grades the source")
        store.flush()
        expect(SourceGradeFile.load(from: file).file.grades["capture:cam"] == nil, "a neutral grade is not saved")
        expect(!store.isGraded("capture:cam"), "the source returns to the Program look, so it has no session grade")
        core.respawn(); core.setEpoch("capture:cam", 0)
        store.coreGenerationChanged()
        store.observe(sourceGrades: [], sources: [cam], now: Date(timeIntervalSince1970: 9_000))
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        expectEqual(core.requests("set-source-grade").count, 3, "nothing to re-assert after a clear")
    }

    @MainActor static func testRelaunchReappliesCaptureButZoomOnlyAsDraft() {   // D2
        let file = tempGradeFile()
        var prior = SourceGradeFile()
        _ = prior.remember(SourceColorGrade(exposure: 4), key: "capture:cam")
        _ = prior.remember(SourceColorGrade(exposure: 8), key: "zoom-person:P-a")
        try! prior.save(to: file)
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0); core.setEpoch("101", 3)
        let cam = GradeSourceIdentity.capture(deviceId: "cam")
        let guest = GradeSourceIdentity.zoom(participantId: "101", persistentId: "P-a", sourceGeneration: 1)
        let store = gradeStore(core, IdentityBox(["capture:cam": cam, "101": guest]), file: file)
        store.coreGenerationChanged()                         // the launch handshake
        store.observe(sourceGrades: [], sources: [cam, guest], now: Date(timeIntervalSince1970: 6_000))
        spin(until: { core.requests("set-source-grade").count >= 1 }, limit: 2)
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        let sets = core.requests("set-source-grade")
        expectEqual(sets.map { $0["sourceId"] as? String }, ["capture:cam"], "only the capture grade returns by itself")
        expectEqual(FakeGradeCore.int((sets.first?["grade"] as? JSONObject)?["exposure"]), 4, "the saved values")
        let seed = store.seed(for: "101")
        expectEqual(seed.grade.exposure, 8, "the Zoom grade seeds the editor")
        expectEqual(seed.draftNotice, "Saved grade loaded as a draft. Apply Live confirms this source binding.",
                    "StudioViewModel.Grading.cs:89")
    }

    /// Drives the REAL AppModel hooks (rule 10): onConnected and applySnapshot.
    @MainActor static func testOnConnectedAndSnapshotsReassertThroughTheModel() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        let devices: JSONObject = ["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]]
        model.applySnapshotForTesting(devices)
        let cam = GradeSourceIdentity.capture(deviceId: "cam")
        expectEqual(model.gradeIdentity(for: "capture:cam"), cam, "capture identity from the device list")
        _ = awaitApply(model.sourceGrades, SourceColorGrade(exposure: 7), "capture:cam", binding: cam.binding)
        core.respawn()
        var clock = Date(timeIntervalSince1970: 7_000)
        model.sourceGrades.now = { clock }
        model.onConnected()
        core.setEpoch("capture:cam", 0)
        model.applySnapshotForTesting(devices.merging(["sourceGrades": []]) { $1 })
        clock = clock.addingTimeInterval(0.6)
        model.applySnapshotForTesting(devices.merging(["sourceGrades": []]) { $1 })
        spin(until: { core.requests("set-source-grade").count >= 2 }, limit: 3)
        // Fixture corrected: the first apply ALSO carried expectedRevision 0 and exposure 7, so
        // without a count the two assertions below passed with no re-assert at all (green under
        // the plan's own mutation 10, deleting coreGenerationChanged from onConnected).
        expectEqual(core.requests("set-source-grade").count, 2, "exactly one re-assert reached the respawned core")
        expect(core.isGraded("capture:cam"), "the respawned core grades the source again")
        let reassert = core.requests("set-source-grade").last ?? [:]
        expectEqual(FakeGradeCore.int(reassert["expectedRevision"]), 0, "re-asserted on the respawned core")
        expectEqual(FakeGradeCore.int((reassert["grade"] as? JSONObject)?["exposure"]), 7, "the session grade")
    }

    @MainActor static func testMeetingEndThroughTheModelForgetsZoomGrades() {
        let core = FakeGradeCore(); core.setEpoch("101", 3)
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applyZoomForTesting(["meetingState": "in-meeting",
                                   "participants": [["userId": "101", "displayName": "G", "videoOn": true,
                                                     "persistentId": "P-a", "sourceGeneration": 1]]])
        let guest = model.gradeIdentity(for: "101")
        expectEqual(guest, .zoom(participantId: "101", persistentId: "P-a", sourceGeneration: 1), "Zoom identity from the roster")
        _ = awaitApply(model.sourceGrades, SourceColorGrade(exposure: 2), "101", binding: guest?.binding ?? "")
        model.applyZoomForTesting(["meetingState": ""])
        expect(model.sourceGrades.isGraded("101"), "an UNKNOWN meeting state never clears (CLAUDE.md #478 rule)")
        model.applyZoomForTesting(["meetingState": "idle"])
        expect(!model.sourceGrades.isGraded("101"), "meeting end forgets the guest's session grade")
    }
}
