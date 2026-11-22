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
            // Task 6: the editor, its entry point (Sources inspector, Picture tab), Program Look.
            ("grade/editor-open-rules", testGradeEditorOpenRulesAreWindows),
            ("grade/editor-targets-frame-key", testGradeTargetsTheFrameKeyOfTheSlotsSource),
            ("grade/editor-live-vs-draft", testEditorLiveEditsApplyAndDraftEditsDoNot),
            ("grade/editor-apply-and-done", testApplyAndDoneClosesOnlyOnAcceptanceAndDiscardSendsNothing),
            ("grade/editor-saved-zoom-draft", testASavedZoomGradeOpensAsADraft),
            ("grade/editor-reset-is-clear", testResetReturnsTheSourceToTheProgramLook),
            ("grade/editor-edit-after-reset-cancels-the-clear", testAnEditAfterResetCancelsTheClear),
            ("grade/inspector-line", testTheInspectorLineSaysWhatTheSourceFollows),
            // Task 9: the advanced document, register-before-apply, presets.
            ("grade/advanced-portable-verdicts", testAdvancedDocumentsHaveThePortableVerdicts),
            ("grade/advanced-swift-encoding", testTheSwiftEncodingIsWhatTheCoreAccepts),
            ("grade/presets-cross-platform", testPresetsMoveBetweenPlatforms),
            ("grade/advanced-register-every-apply", testEveryApplyRegistersItsDocumentAndARespawnReuploads),
            ("grade/advanced-explicit-nulls", testExplicitNullsAreRefused),
            // Task 10: advanced panel, undo/redo, copy/paste.
            ("grade/history-gesture-and-depth", testGradeHistoryIsOneStepPerGestureAndHundredDeep),
            ("grade/editor-expand-draft-undo-preset", testExpandingIsADraftAndUndoRestoresAWholePreset),
            ("grade/editor-paste-no-leak", testAPasteWithLiveEditingLeaksNoIntermediateValues),
            ("grade/editor-eight-and-invalid", testTheEightAdjustmentLimitAndAnInvalidEditAreRefusedWithoutChange),
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
            case "register-grade-document":
                let ref = body["documentRef"] as? String ?? ""
                if locked({ _documents.contains(ref) }) { return ["ok": true, "present": true] }
                guard let text = body["documentJson"] as? String else { return ["ok": true, "present": false] }
                guard GradeDocumentRef.sha256Hex(text) == ref else { throw BridgeError.remote("Invalid grade document content.") }
                locked { _ = _documents.insert(ref) }
                return ["ok": true, "present": true]
            default:
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
        GradePreviewSession.autoTick = false      // Phase 2: tests tick preview sessions by hand
        return AppModel()
    }
    @MainActor static func awaitApply(_ store: SourceGradeStore, _ grade: SourceColorGrade,
                                      _ sourceId: String, binding: String, clear: Bool = false) -> GradeApplyOutcome? {
        var outcome: GradeApplyOutcome?
        Task { outcome = await store.apply(grade, sourceId: sourceId, authoredFor: binding, clear: clear) }
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
        // Owner ruling 2026-11-16 (Windows parity): a NEUTRAL grade is a real grade that
        // overrides the Program look. Only an explicit clear (the editor's Reset) forgets it.
        expectEqual(awaitApply(store, SourceColorGrade(), "capture:cam", binding: cam.binding), .applied(revision: 2),
                    "a neutral grade is applied")
        expect(core.requests("set-source-grade").last?["clear"] == nil, "a neutral grade is never sent as a clear")
        expect(core.isGraded("capture:cam"), "the core keeps grading the source (neutral, ignoring the Program look)")
        expect(store.isGraded("capture:cam"), "a neutral grade is the source's own grade")
        store.flush()
        expectEqual(SourceGradeFile.load(from: file).file.grades["capture:cam"]?.exposure, 0, "a neutral grade is saved")
        // The explicit clear: the core forgets the grade, the store forgets both.
        expectEqual(awaitApply(store, SourceColorGrade(), "capture:cam", binding: cam.binding, clear: true),
                    .applied(revision: 3), "a clear is acknowledged as a new revision")
        expectEqual(core.requests("set-source-grade").last?["clear"] as? Bool, true, "an explicit clear is sent as clear:true")
        expect(!core.isGraded("capture:cam"), "the core no longer grades the source")
        store.flush()
        expect(SourceGradeFile.load(from: file).file.grades["capture:cam"] == nil, "a neutral grade is not saved")
        expect(!store.isGraded("capture:cam"), "the source returns to the Program look, so it has no session grade")
        core.respawn(); core.setEpoch("capture:cam", 0)
        store.coreGenerationChanged()
        store.observe(sourceGrades: [], sources: [cam], now: Date(timeIntervalSince1970: 9_000))
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        expectEqual(core.requests("set-source-grade").count, 4, "nothing to re-assert after a clear")
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

    // ── Task 6 ───────────────────────────────────────────────────────────────

    @MainActor static func testGradeEditorOpenRulesAreWindows() {
        let model = gradeModel()
        if case .refused(let text) = model.openGradeEditor(sourceId: nil) {
            expectEqual(text, "Select a source before editing its color grade", "Grading.cs:68")
        } else { expect(false, "a nil source must refuse") }
        for id in ["capture:a", "capture:b", "capture:c"] {
            if case .refused = model.openGradeEditor(sourceId: id) { expect(false, "\(id) should open") }
        }
        if case .opened(let again) = model.openGradeEditor(sourceId: "capture:b") {
            expect(again === model.gradeEditors["capture:b"], "re-opening returns the existing editor")
        } else { expect(false, "re-opening an open source must succeed") }
        if case .refused(let text) = model.openGradeEditor(sourceId: "capture:d") {
            expectEqual(text, "Close one of the three grade editors before opening another", "Grading.cs:74")
        } else { expect(false, "a fourth editor must refuse") }
        model.closeGradeEditor(sourceId: "capture:a")
        if case .refused = model.openGradeEditor(sourceId: "capture:d") { expect(false, "closing one frees a place") }
    }

    @MainActor static func testGradeTargetsTheFrameKeyOfTheSlotsSource() {
        let model = gradeModel()
        model.assignSlot(1, kind: "zoom", sourceId: "16778240", name: "Guest")
        model.assignSlot(2, kind: "capture", sourceId: "cam-1", name: "Cam")
        expectEqual(model.gradeSourceId(forSlot: 1), "16778240", "raw pid: the key Zoom frames carry (fact 4)")
        expectEqual(model.gradeSourceId(forSlot: 2), "capture:cam-1", "capture frame key")
        expectEqual(model.gradeSourceId(forSlot: 3), nil, "an unassigned slot has no grade target")
        expect(AppModel.gradeEditorAvailable, "the Sources inspector's Grade section is live (G1 entry point)")
    }

    @MainActor static func testEditorLiveEditsApplyAndDraftEditsDoNot() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
        expect(editor.liveEditing, "Apply edits live is on by default")
        expectEqual(editor.status, "Basic controls retain their established scale.", "Windows initial status")
        expectEqual(editor.headerTitle, "Color grade - Cam", "Windows header")
        editor.exposure = 4; editor.edited()
        spin(until: { core.requests("set-source-grade").count == 1 }, limit: 2)
        expectEqual(FakeGradeCore.int((core.requests("set-source-grade").first?["grade"] as? JSONObject)?["exposure"]), 4,
                    "a live edit goes to air")
        editor.liveEditing = false
        editor.exposure = 9; editor.edited()
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        expectEqual(core.requests("set-source-grade").count, 1,
                    "a draft edit never reaches set-source-grade (ColorGradeEditorViewModelTests.cs:68)")
        Task { await editor.applyLive() }
        spin(until: { core.requests("set-source-grade").count == 2 }, limit: 2)
        expectEqual(FakeGradeCore.int((core.requests("set-source-grade").last?["grade"] as? JSONObject)?["exposure"]), 9,
                    "Apply Live sends the draft")
        spin(until: { editor.status.hasPrefix("Live grade acknowledged") }, limit: 2)
        expectEqual(editor.status, "Live grade acknowledged · revision 2", "Apply.cs:52")
    }

    @MainActor static func testApplyAndDoneClosesOnlyOnAcceptanceAndDiscardSendsNothing() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
        editor.liveEditing = false
        editor.exposure = 6
        core.refuseNext("grade-capacity")
        var done: Bool?
        Task { done = await editor.applyAndDone() }
        spin(until: { done != nil }, limit: 2)
        expectEqual(done, false, "a refused apply does not close")
        expect(model.gradeEditors["capture:cam"] != nil, "the editor stays open")
        expectEqual(editor.status, "Grade was not applied: grade-capacity. Previous live grade retained.", "Apply.cs:50")
        done = nil
        Task { done = await editor.applyAndDone() }
        spin(until: { done != nil }, limit: 2)
        expectEqual(done, true, "an acknowledged apply closes")
        expect(model.gradeEditors["capture:cam"] == nil, "closed")
        guard case .opened(let reopened) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "reopens"); return }
        expectEqual(reopened.exposure, 6, "the acknowledged session grade seeds the next editor")
        reopened.liveEditing = false
        reopened.exposure = 30
        let sent = core.requests("set-source-grade").count
        model.closeGradeEditor(sourceId: "capture:cam")      // "Close / discard draft"
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        expectEqual(core.requests("set-source-grade").count, sent, "discard sends nothing")
    }

    @MainActor static func testASavedZoomGradeOpensAsADraft() {
        let file = tempGradeFile()
        var saved = SourceGradeFile(); _ = saved.remember(SourceColorGrade(exposure: 8), key: "zoom-person:P-a")
        try! saved.save(to: file)
        let model = gradeModel(file: file)
        model.applyZoomForTesting(["meetingState": "in-meeting",
                                   "participants": [["userId": "101", "displayName": "G", "videoOn": true,
                                                     "persistentId": "P-a", "sourceGeneration": 1]]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "101") else { expect(false, "opens"); return }
        expect(!editor.liveEditing, "a saved Zoom grade is a draft (D2)")
        expectEqual(editor.exposure, 8, "seeded from the saved grade")
        expectEqual(editor.status, "Saved grade loaded as a draft. Apply Live confirms this source binding.", "Grading.cs:89")
    }

    /// D6 overridden: the editor's Reset (live) sends a clear, and the source has no grade of its own.
    @MainActor static func testResetReturnsTheSourceToTheProgramLook() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
        editor.lut = "punch"; editor.edited()
        spin(until: { model.sourceGrades.isGraded("capture:cam") }, limit: 2)
        // Owner ruling 2026-11-16 (Windows parity): editing back to neutral by hand is a real
        // grade, so the source keeps ignoring the Program look. Only Reset clears.
        editor.lut = "none"; editor.edited()
        spin(until: { core.requests("set-source-grade").count == 2 }, limit: 2)
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        expect(core.requests("set-source-grade").last?["clear"] == nil, "a hand-made neutral grade is not a clear")
        expect(model.sourceGrades.isGraded("capture:cam"), "the source keeps its own (neutral) grade")
        editor.reset()
        spin(until: { core.requests("set-source-grade").count == 3 }, limit: 2)
        RunLoop.current.run(until: Date().addingTimeInterval(0.1))
        expectEqual(core.requests("set-source-grade").last?["clear"] as? Bool, true, "Reset is sent as clear:true")
        expect(!model.sourceGrades.isGraded("capture:cam"), "the source follows the Program Look again")
        expectEqual(GradeEditorModel.resetNotice, "Reset returns this source to the Program Look.", "the D6 copy is gone")
    }

    /// Owner ruling 2026-11-16: only Reset clears. With live editing off, an edit made after
    /// Reset and before Apply is a real grade, so Apply must not send a clear.
    @MainActor static func testAnEditAfterResetCancelsTheClear() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
        editor.liveEditing = false
        editor.reset()
        editor.lut = "punch"; editor.edited()
        var applied: Bool?
        Task { applied = await editor.applyAndDone() }
        spin(until: { applied != nil }, limit: 3)
        expectEqual(applied, true, "Apply is acknowledged")
        expect(core.requests("set-source-grade").last?["clear"] == nil, "the edit after Reset cancelled the clear")
        expect(model.sourceGrades.isGraded("capture:cam"), "the source keeps the edited grade")
    }

    static func testTheInspectorLineSaysWhatTheSourceFollows() {
        expectEqual(GradeInspectorSection.line(nil), "No grade of its own: follows the Program Look.", "ungraded")
        let graded = SourceGradeSummary(line: "Look punch · exposure 0 · contrast 0 · saturation 0", indicator: nil,
                                        active: true, identityChanged: false)
        expectEqual(GradeInspectorSection.line(graded), "Look punch · exposure 0 · contrast 0 · saturation 0", "graded and live")
        let waiting = SourceGradeSummary(line: "Look punch · exposure 0 · contrast 0 · saturation 0", indicator: nil,
                                         active: false, identityChanged: false)
        expect(GradeInspectorSection.line(waiting).hasSuffix("waiting for this source's video"), "inactive says so")
        let moved = SourceGradeSummary(line: "x", indicator: nil, active: false, identityChanged: true)
        expectEqual(GradeInspectorSection.line(moved), "Source identity changed. Reopen its grade editor.", "binding broke")
    }
    // ── Task 9 ───────────────────────────────────────────────────────────────

    static func loadGradeFixtures() -> [String: Any]? {
        let root = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
        guard let data = try? Data(contentsOf: root.appendingPathComponent("contracts/grade.fixtures.json")) else { return nil }
        return try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    }

    static func testAdvancedDocumentsHaveThePortableVerdicts() {
        guard let fixtures = loadGradeFixtures(), let documents = fixtures["documents"] as? [[String: Any]] else {
            expect(false, "contracts/grade.fixtures.json must be readable from the harness"); return
        }
        var checked = 0
        for entry in documents {
            let name = entry["name"] as? String ?? "?"
            // Phase 2 Task 1 (G3) retired the D8 skip: needsCubeParser rows are checked too.
            let data = try! JSONSerialization.data(withJSONObject: entry["json"]!, options: [.fragmentsAllowed])
            let valid: Bool
            do { try JSONDecoder().decode(AdvancedGradeDocument.self, from: data).validate(); valid = true } catch { valid = false }
            expectEqual(valid, (entry["portable"] as? String) == "valid", "portable verdict: \(name)")
            checked += 1
        }
        expectEqual(checked, 25, "every document fixture is checked, cube-parser rows included (G3)")
    }

    static func testTheSwiftEncodingIsWhatTheCoreAccepts() {
        guard let fixtures = loadGradeFixtures(), let encoded = fixtures["macEncoded"] as? [[String: Any]],
              let text = encoded.first?["text"] as? String else { expect(false, "macEncoded fixture"); return }
        var op = GradeOperation(); op.id = "black"; op.gain = 0
        let document = AdvancedGradeDocument(operations: [op])
        let mine = try! document.canonicalJSON()
        let a = try! JSONSerialization.jsonObject(with: Data(mine.utf8)) as! NSDictionary
        let b = try! JSONSerialization.jsonObject(with: Data(text.utf8)) as! NSDictionary
        expect(a.isEqual(b), "Swift's encoding equals the fixture the native test feeds readAdvancedGrade")
        for key in ["version", "colorSpace", "bypass", "intensity", "operations"] {
            expect(a[key] != nil, "the core requires \(key) (AdvancedGrade.h:33-42)")
        }
        let curves = ((a["operations"] as? [NSDictionary])?.first?["curves"] as? [[Any]])
        expectEqual(curves?.count, 4, "four curve channels are always written (AdvancedGrade.h:51-52)")
    }

    static func testPresetsMoveBetweenPlatforms() {
        guard let fixtures = loadGradeFixtures(), let presets = fixtures["presets"] as? [[String: Any]] else {
            expect(false, "presets fixture"); return
        }
        for entry in presets {
            let name = entry["name"] as? String ?? "?"
            let expectValues = entry["expect"] as? [String: Any] ?? [:]
            guard case .grade(let grade)? = try? GradePreset.decode(entry["text"] as? String ?? "") else {
                expect(false, "\(name) must paste on the Mac"); continue
            }
            expectEqual(grade.lut, expectValues["lut"] as? String, "\(name) lut")
            expectEqual(grade.exposure, expectValues["exposure"] as? Int, "\(name) exposure")
            expectEqual(grade.saturation, expectValues["saturation"] as? Int ?? 0, "\(name) saturation")
            expectEqual(grade.advanced?.operations.count ?? 0, expectValues["operations"] as? Int, "\(name) operations")
            if entry["origin"] as? String == "mac" {
                let mine = try! GradePreset.encode(grade)
                let a = try! JSONSerialization.jsonObject(with: Data(mine.utf8)) as! NSDictionary
                let b = try! JSONSerialization.jsonObject(with: Data((entry["text"] as! String).utf8)) as! NSDictionary
                expect(a.isEqual(b), "the Mac preset fixture is exactly what the Mac writes")
            }
        }
        let tooBig = String(repeating: " ", count: GradePreset.maxBytes + 1)
        do { _ = try GradePreset.decode(tooBig); expect(false, "over the cap must refuse") }
        catch let e as GradeValidationError { expectEqual(e.message, "Grade document is too large.", "VM.Advanced.cs:160") }
        catch { expect(false, "unexpected error type") }
        do { _ = try GradePreset.decode("{\"presetVersion\":2,\"grade\":{\"lut\":\"none\"}}"); expect(false, "version 2 must refuse") }
        catch let e as GradeValidationError { expectEqual(e.message, "Unsupported grade preset.", "VM.Advanced.cs:163") }
        catch { expect(false, "unexpected error type") }
    }

    @MainActor static func testEveryApplyRegistersItsDocumentAndARespawnReuploads() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let cam = GradeSourceIdentity.capture(deviceId: "cam")
        let store = gradeStore(core, IdentityBox(["capture:cam": cam]))
        var op = GradeOperation(); op.id = "p1"; op.saturation = 0
        let grade = SourceColorGrade(lut: "none", advanced: AdvancedGradeDocument(operations: [op]))
        expectEqual(awaitApply(store, grade, "capture:cam", binding: cam.binding), .applied(revision: 1), "applied")
        let registers = core.requests("register-grade-document")
        expectEqual(registers.count, 2, "query, then upload")
        expect(registers.first?["documentJson"] == nil, "the first request only asks")
        let text = registers.dropFirst().first?["documentJson"] as? String ?? ""
        expectEqual(registers.dropFirst().first?["documentRef"] as? String, GradeDocumentRef.sha256Hex(text), "ref = SHA-256 of the exact text sent")
        let ref = ((core.requests("set-source-grade").last?["grade"] as? JSONObject)?["advanced"] as? JSONObject)?["documentRef"] as? String
        expectEqual(ref, GradeDocumentRef.sha256Hex(text), "the set carries the registered reference")
        expect(core.requests("set-source-grade").last?["clear"] == nil, "a grade with a document is never a clear")
        expectEqual(store.summary(for: "capture:cam")?.indicator, "Advanced adjustments active", "the inspector indicator")
        _ = awaitApply(store, grade, "capture:cam", binding: cam.binding)
        expectEqual(core.requests("register-grade-document").count, 3, "a cached document is asked about, not re-uploaded")
        core.respawn(); store.coreGenerationChanged(); core.setEpoch("capture:cam", 0)
        store.observe(sourceGrades: [], sources: [cam], now: Date(timeIntervalSince1970: 8_000))
        spin(until: { core.requests("set-source-grade").count >= 3 }, limit: 2)
        expectEqual(core.requests("register-grade-document").count, 5, "the respawned core gets the document again")
        expectEqual(core.requests("set-source-grade").last?["sourceId"] as? String, "capture:cam", "and the grade")
        expect(core.documents.contains(GradeDocumentRef.sha256Hex(text)), "registered on the new core")
    }

    static func testExplicitNullsAreRefused() {
        for text in ["{\"version\":2,\"operations\":null}", "{\"version\":2,\"operations\":[null]}"] {
            let decoded = try? JSONDecoder().decode(AdvancedGradeDocument.self, from: Data(text.utf8))
            expect(decoded == nil || (try? decoded!.validate()) == nil,
                   "\(text) is refused (AdvancedGradeEditorTests.cs UnsupportedAndMalformedDocuments…)")
        }
    }

    // ── Task 10 ──────────────────────────────────────────────────────────────

    static func testGradeHistoryIsOneStepPerGestureAndHundredDeep() {
        var history = GradeHistory()
        var current = SourceColorGrade()
        history.beginGesture()
        for value in 1...20 { history.remember(current); current.exposure = value }   // one slider drag
        history.endGesture()
        expectEqual(history.undo(from: current)?.exposure, 0, "a whole drag is one undo step")
        var history2 = GradeHistory()
        var grade = SourceColorGrade()
        for value in 1...150 { history2.remember(grade); grade.exposure = value % 50 }
        var undone = 0
        while let previous = history2.undo(from: grade) { grade = previous; undone += 1 }
        expectEqual(undone, GradeHistory.depth, "history keeps exactly 100 entries")
    }

    @MainActor static func testExpandingIsADraftAndUndoRestoresAWholePreset() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
        // Fixture corrected: the plan's draft turned live editing off itself before expanding,
        // so "expanding makes edits a draft" stayed green with expansion leaving it on (its own
        // mutation 4). The basic edit now goes live first; expanding must turn live editing off.
        editor.lut = "warm-film"; editor.exposure = 2; editor.edited()
        spin(until: { core.requests("set-source-grade").count == 1 }, limit: 2)
        expect(editor.liveEditing, "precondition: live before expanding")
        editor.setAdvancedExpanded(true)
        expect(!editor.liveEditing, "expanding makes edits a draft")
        expectEqual(editor.status, "Draft — Apply Live sends this grade to the show.", "VM.Advanced.cs:75")
        editor.addPrimaries()
        editor.editSelected { $0.saturation = 0 }
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        expectEqual(core.requests("set-source-grade").count, 1, "nothing more reaches air from a draft")
        expectEqual(editor.indicator, "Advanced adjustments active", "C# AdvancedIndicator")
        // Paste a preset as ONE undo step (port of AdvancedGradeEditorTests.cs:28).
        let preset = try! GradePreset.encode(SourceColorGrade(lut: "punch", exposure: 15))
        expect(editor.paste(preset), "paste accepted")
        expectEqual(editor.exposure, 15, "pasted values")
        expect(editor.document == nil, "a preset without a document clears the draft's document")
        editor.undo()
        expectEqual(editor.exposure, 2, "undo restores the basic values")
        expectEqual(editor.lut, "warm-film", "and the look")
        expectEqual(editor.document?.operations.count, 1, "and the document, in one step")
        editor.redo()
        expectEqual(editor.exposure, 15, "redo")
    }

    @MainActor static func testAPasteWithLiveEditingLeaksNoIntermediateValues() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
        expect(editor.liveEditing, "precondition: live")
        let preset = try! GradePreset.encode(SourceColorGrade(lut: "cool-broadcast", exposure: -7, contrast: 3, saturation: 4, temperature: -2))
        expect(editor.paste(preset), "paste accepted")
        spin(until: { core.requests("set-source-grade").count >= 1 }, limit: 2)
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        let sets = core.requests("set-source-grade")
        expectEqual(sets.count, 1, "exactly one apply for a paste (VM.Advanced.cs:169-172)")
        let sent = sets.first?["grade"] as? JSONObject ?? [:]
        expectEqual(sent["lut"] as? String, "cool-broadcast", "with the final look")
        expectEqual(FakeGradeCore.int(sent["exposure"]), -7, "and the final exposure")
        expectEqual(FakeGradeCore.int(sent["temperature"]), -2, "and the final temperature")
    }

    @MainActor static func testTheEightAdjustmentLimitAndAnInvalidEditAreRefusedWithoutChange() {
        let model = gradeModel()
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:x") else { expect(false, "opens"); return }
        editor.setAdvancedExpanded(true)
        for _ in 0..<8 { editor.addPrimaries() }
        editor.addPrimaries()
        expectEqual(editor.document?.operations.count, 8, "at most eight")
        expectEqual(editor.status, "A grade can contain up to eight adjustments.", "VM.Advanced.cs:119")
        editor.duplicateSelected()
        expectEqual(editor.document?.operations.count, 8, "duplicate respects the limit")
        let before = editor.document
        editor.editSelected { $0.gamma = 0.05 }
        expectEqual(editor.document, before, "an out-of-range edit leaves the draft unchanged")
        expectEqual(editor.status, "Grade parameter is outside its supported range.", "C# Validate message")
        let ids = editor.document?.operations.map(\.id) ?? []
        guard ids.count > 1 else { expect(false, "eight adjustments exist"); return }
        editor.selectedAdjustmentId = ids[1]
        editor.moveSelected(-1)
        expectEqual(editor.document?.operations.first?.id, ids[1], "Up moves the selection one place")
    }
}
