// macOS per-source colour grading, Phase 2 (docs/superpowers/plans/2026-11-21-mac-grading-phase2.md).
// Its own file so the parallel Mac branches extending ShellTests.swift / ShellTestsGrade.swift do
// not collide. ContractParity scans this file: no `"type": "<name>"` dictionary literal may appear
// here for a request the core does not handle.

import Foundation

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var gradePhase2Cases: [(String, () -> Void)] {
        [
            // Task 1 (G3): one .cube verdict on three platforms.
            ("grade2/cube-shared-verdicts", testCubeFixturesHaveTheSharedVerdicts),
            ("grade2/cube-csharp-messages", testCubeParserUsesTheWindowsMessages),
            ("grade2/cube-document-validation", testADocumentWithAMalformedCubeIsRefused),
            // Task 2 (G3): curves editor and LUT import.
            ("grade2/curve-press-adds-and-selects", testCurvePressAddsSelectsAndRefusesCrowding),
            ("grade2/curve-drag-clamps", testCurveDragKeepsEndpointsAndOrder),
            ("grade2/curve-keys", testCurveKeysSelectNudgeAndDelete),
            ("grade2/editor-curves-validated-one-undo", testEditorCurveEditsAreValidatedAndOneUndoPerGesture),
            ("grade2/editor-import-cube", testImportCubeAddsASelectedLutOrSaysWhy),
            // Task 4 (G4): Sources table row, toolbar and inspector header.
            ("grade2/g4-three-routes-one-editor", testRowToolbarAndInspectorOpenTheSameEditor),
            ("grade2/g4-refusals", testGradeEntryRefusalsUseTheWindowsWords),
            ("grade2/g4-graded-chip", testTheGradedChipFollowsTheStore),
            // Task 8 (G5+G6): preview session, Compare, status lines, scopes.
            ("grade2/preview-session-send-renew-resend-close", testPreviewSessionSendsRenewsResendsAndCloses),
            ("grade2/preview-draft-and-compare-never-apply", testDraftAndCompareSendPreviewsNeverSetSourceGrade),
            ("grade2/preview-observation-rules", testPreviewObservationsAreFilteredAndWordedLikeWindows),
            ("grade2/preview-scopes-only-expanded", testScopesAreDemandedOnlyExpandedAndJudgedLikeWindows),
            ("grade2/preview-epoch-hint", testAPreviewEpochSavesTheFirstQuery),
            ("grade2/preview-supported-notice", testTheNoPreviewNoticeFollowsTheCore),
            ("grade2/preview-core-reset", testACoreRespawnReconnectsThePreview),
        ]
    }

    // ── Task 1 ───────────────────────────────────────────────────────────────

    static func testCubeFixturesHaveTheSharedVerdicts() {
        guard let fixtures = loadGradeFixtures(), let cubes = fixtures["cubes"] as? [[String: Any]] else {
            expect(false, "contracts/grade.fixtures.json must carry the shared `cubes` rows"); return
        }
        var checked = 0, valid = 0
        for entry in cubes {
            let name = entry["name"] as? String ?? "?"
            let text = entry["text"] as? String ?? ""
            let parsed = (try? CubeLutParser.parse(text)) != nil
            expectEqual(parsed, entry["valid"] as? Bool ?? !parsed, "cube verdict: \(name)")
            checked += 1; if parsed { valid += 1 }
        }
        // Fixture invariant (plan rule 1): 26 rows, 7 valid, the same count the core and C# assert.
        expectEqual(checked, 26, "every shared cube row was checked")
        expectEqual(valid, 7, "exactly the valid rows parse")
        let swap = (cubes.first { $0["name"] as? String == "channel-swap-2" }?["text"] as? String) ?? ""
        if let lut = try? CubeLutParser.parse(swap) {
            expectEqual(lut.size, 2, "LUT_3D_SIZE 2")
            expectEqual(lut.samples.count, 8, "size³ samples")
            expectEqual(lut.samples[1], [0, 0, 1], "red-first ordering: input (1,0,0) maps to blue")
            expectEqual(lut.domainMin, [0, 0, 0], "default domain min")
            expectEqual(lut.domainMax, [1, 1, 1], "default domain max")
        } else { expect(false, "channel-swap-2 parses") }
    }

    static func testCubeParserUsesTheWindowsMessages() {
        func message(_ text: String) -> String? {
            do { _ = try CubeLutParser.parse(text); return nil }
            catch let error as GradeValidationError { return error.message }
            catch { return "\(error)" }
        }
        // CubeLutParser.cs, verbatim.
        expectEqual(message("LUT_3D_SIZE 34\n"),
                    "Supported 3D LUT dimensions are 2 through 33; duplicate size declarations are invalid.", "size")
        expectEqual(message("LUT_3D_SIZE 2\n0 0 0\n"), "LUT sample count or domain is invalid.", "short")
        expectEqual(message("LUT_3D_SIZE 2\n17 0 0\n"), "LUT outputs must be within -16 to 16.", "range")
        expectEqual(message("LUT_3D_SIZE 2\nnan 0 0\n"), "LUT contains a non-finite or invalid number.", "nan")
        expectEqual(message("LUT_3D_SIZE 2\nDOMAIN_MIN 0 0\n"), "Invalid LUT domain.", "domain tokens")
        expectEqual(message("0 0 0\n"), "Unsupported or malformed .cube content.", "no size")
        expectEqual(message(String(repeating: "#", count: 2_000_001)), "LUT files are limited to 2 MB.", "2 MB cap")
        // C# NumberStyles.Float never reads hex; Swift's Double(String) would.
        expectEqual(message("LUT_3D_SIZE 2\n0x1p-1 0 0\n"), "LUT contains a non-finite or invalid number.", "hex")
    }

    static func testADocumentWithAMalformedCubeIsRefused() {
        let text = "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n"
        var op = GradeOperation(); op.kind = "cube"; op.cubeText = text; op.cubeSha256 = GradeDocumentRef.sha256Hex(text)
        let document = AdvancedGradeDocument(operations: [op])
        do { try document.validate(); expect(false, "a malformed cube must be refused by validate()") }
        catch let error as GradeValidationError {
            expectEqual(error.message, "LUT sample count or domain is invalid.", "the parser runs before the hash (AdvancedGradeDocument.cs:86-90)")
        } catch { expect(false, "unexpected error type") }
    }

    // ── Task 2 ───────────────────────────────────────────────────────────────
    // Fixture invariants: a 400x200 view (y grows DOWN, as in GradeCurveControl.cs), so curve point
    // (x, y) sits at view (400x, 200(1-y)); the 13 px hit radius is 0.0325 of the width.

    static func curvePoint(_ x: Double, _ y: Double) -> GradeCurvePoint { GradeCurvePoint(x: x, y: y) }

    static func testCurvePressAddsSelectsAndRefusesCrowding() {
        let size = CGSize(width: 400, height: 200)
        var curve = GradeCurveEditing()
        expectEqual(curve.points, [curvePoint(0, 0), curvePoint(1, 1)], "a new curve is the identity line")
        var changed = curve.press(at: CGPoint(x: 200, y: 50), size: size)
        expect(changed, "a click on empty space adds a point")
        expectEqual(curve.points.count, 3, "three points")
        expectEqual(curve.points[1], curvePoint(0.5, 0.75), "the point lands where clicked (y flipped)")
        expectEqual(curve.selectedIndex, 1, "and is selected")
        changed = curve.press(at: CGPoint(x: 205, y: 55), size: size)
        expect(!changed && curve.selectedIndex == 1, "a click within 13 px selects the existing point instead")
        changed = curve.press(at: CGPoint(x: 0.6, y: 190), size: size)
        expect(!changed && curve.selectedIndex == 0, "a click on an endpoint selects it")
        curve = GradeCurveEditing(points: [curvePoint(0, 0), curvePoint(0.5, 0.5), curvePoint(1, 1)])
        changed = curve.press(at: CGPoint(x: 200.4 + 0.0019 * 400 + 14, y: 0), size: size)
        expect(changed, "a point 14 px away in x is far enough")
        var crowded = GradeCurveEditing(points: [curvePoint(0, 0), curvePoint(0.5, 0.5), curvePoint(1, 1)])
        crowded.selectedIndex = -1
        // x within 0.002 of an existing point but outside the hit radius vertically: refused.
        changed = crowded.press(at: CGPoint(x: 200.4, y: 0), size: size)
        expect(!changed, "a point within 0.002 in x of another is not added (GradeCurveControl.cs:91)")
        var full = GradeCurveEditing(points: (0..<16).map { curvePoint(Double($0) / 15, Double($0) / 15) })
        changed = full.press(at: CGPoint(x: 13, y: 20), size: size)
        expect(!changed && full.points.count == 16, "16 points is the cap")
        var edge = GradeCurveEditing()
        _ = edge.press(at: CGPoint(x: 399.9, y: 100), size: CGSize(width: 400, height: 200))
        expect(edge.points.allSatisfy { $0.x == 0 || $0.x == 1 || $0.x <= 0.999 }, "an added point's x is clamped to 0.999")
    }

    static func testCurveDragKeepsEndpointsAndOrder() {
        let size = CGSize(width: 400, height: 200)
        var curve = GradeCurveEditing(points: [curvePoint(0, 0), curvePoint(0.3, 0.3), curvePoint(0.6, 0.6), curvePoint(1, 1)])
        curve.selectedIndex = 0
        _ = curve.drag(to: CGPoint(x: 120, y: 100), size: size)
        expectEqual(curve.points[0], curvePoint(0, 0.5), "an endpoint moves vertically only")
        curve.selectedIndex = 3
        _ = curve.drag(to: CGPoint(x: 10, y: 0), size: size)
        expectEqual(curve.points[3], curvePoint(1, 1), "the last point keeps x = 1")
        curve.selectedIndex = 1
        _ = curve.drag(to: CGPoint(x: 380, y: 260), size: size)
        let p = curve.points[1]
        expect(p.x < 0.6 && p.x > 0.599, "an interior point stops just short of its right neighbour, got \(p.x)")
        expectEqual(p.y, 0, "y is clamped to 0…1")
        expect(Float(p.x) < Float(0.6), "the order still holds as Float, which is what every validator compares")
        var op = GradeOperation(); op.kind = "curves"; op.curves[0] = curve.points
        expect((try? AdvancedGradeDocument(operations: [op]).validate()) != nil, "a dragged curve is always a valid document")
    }

    static func testCurveKeysSelectNudgeAndDelete() {
        var curve = GradeCurveEditing(points: [curvePoint(0, 0), curvePoint(0.5, 0.5), curvePoint(1, 1)])
        expect(!curve.key(.right, shift: false, command: false), "an arrow with nothing selected only selects")
        expectEqual(curve.selectedIndex, 0, "…the first point")
        _ = curve.key(.end, shift: false, command: false)
        expectEqual(curve.selectedIndex, 2, "End selects the last endpoint")
        _ = curve.key(.left, shift: false, command: true)
        expectEqual(curve.selectedIndex, 1, "Command+Left steps the selection (Ctrl on Windows)")
        expect(curve.key(.up, shift: false, command: false), "Up nudges")
        expect(abs(curve.points[1].y - 0.501) < 1e-9, "by 0.1%")
        _ = curve.key(.right, shift: true, command: false)
        expect(abs(curve.points[1].x - 0.51) < 1e-9, "Shift nudges by 1%")
        _ = curve.key(.home, shift: false, command: false)
        expect(!curve.key(.delete, shift: false, command: false) && curve.points.count == 3, "an endpoint cannot be deleted")
        curve.selectedIndex = 1
        expect(curve.key(.delete, shift: false, command: false), "Delete removes an interior point")
        expectEqual(curve.points, [curvePoint(0, 0), curvePoint(1, 1)], "back to the line")
        expectEqual(curve.selectedIndex, -1, "nothing selected after a delete")
        curve = GradeCurveEditing(points: [curvePoint(0, 0.2), curvePoint(0.5, 0.9), curvePoint(1, 0.4)])
        curve.reset()
        expectEqual(curve.points, [curvePoint(0, 0), curvePoint(1, 1)], "Reset channel restores the line")
    }

    @MainActor static func testEditorCurveEditsAreValidatedAndOneUndoPerGesture() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
        editor.setAdvancedExpanded(true)
        editor.addCurves()
        expectEqual(editor.selectedAdjustment?.kind, "curves", "+ Curves adds and selects a curves adjustment")
        editor.curveChannel = 1                                    // Red
        let size = CGSize(width: 400, height: 200)
        editor.beginGesture()
        editor.curvePress(at: CGPoint(x: 200, y: 100), size: size)
        editor.curveDrag(to: CGPoint(x: 200, y: 60), size: size)
        editor.curveDrag(to: CGPoint(x: 200, y: 40), size: size)
        editor.endGesture()
        let red = editor.selectedAdjustment?.curves[1] ?? []
        expectEqual(red.count, 3, "the red curve gained a point")
        expect(abs((red.count == 3 ? red[1].y : 0) - 0.8) < 1e-9, "dragged to y 0.8")
        expectEqual(editor.selectedAdjustment?.curves[0], GradeOperation.identityCurves[0], "master untouched")
        editor.undo()
        expectEqual(editor.selectedAdjustment?.curves[1], GradeOperation.identityCurves[1],
                    "one undo removes the whole add-and-drag gesture")
        editor.setCurve(channel: 2, points: [curvePoint(0, 0), curvePoint(0.5, 0.5), curvePoint(0.5, 0.6), curvePoint(1, 1)])
        expectEqual(editor.selectedAdjustment?.curves[2], GradeOperation.identityCurves[2], "an unordered curve is refused")
        expectEqual(editor.status, "Curve inputs must be strictly ordered.", "with the validator's message")
        expect(!GradeEditorModel.curveHelp.contains("Ctrl"), "the Mac help says Command, not Ctrl")
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        expectEqual(core.requests("set-source-grade").count, 0, "advanced edits are a draft")
    }

    @MainActor static func testImportCubeAddsASelectedLutOrSaysWhy() {
        let model = gradeModel()
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
        editor.setAdvancedExpanded(true)
        let text = "LUT_3D_SIZE 2\n0 0 1\n0 0 1\n0 1 1\n0 1 1\n1 0 0\n1 0 0\n1 1 0\n1 1 0\n"
        editor.importCube(text: text, name: "Swap")
        let op = editor.selectedAdjustment
        expectEqual(op?.kind, "cube", "the import is a cube adjustment")
        expectEqual(op?.name, "Swap", "named after the file")
        expectEqual(op?.cubeText, text, "carrying the exact text")
        expectEqual(op?.cubeSha256, GradeDocumentRef.sha256Hex(text), "and its SHA-256")
        let before = editor.document
        editor.importCube(text: "LUT_3D_SIZE 2\n0 0 0\n", name: "Bad")
        expectEqual(editor.document, before, "a bad LUT leaves the draft unchanged")
        expectEqual(editor.status, "LUT sample count or domain is invalid.", "and says why (the parser's words)")
        for _ in 0..<7 { editor.addPrimaries() }
        expectEqual(editor.document?.operations.count, 8, "eight adjustments")
        editor.importCube(text: text, name: "Ninth")
        expectEqual(editor.document?.operations.count, 8, "a ninth is refused")
        expectEqual(editor.status, "A grade can contain up to eight adjustments.", "VM.Advanced.cs:185")
    }

    // ── Task 4 ───────────────────────────────────────────────────────────────

    @MainActor static func testRowToolbarAndInspectorOpenTheSameEditor() {
        let model = gradeModel()
        model.assignSlot(1, kind: "zoom", sourceId: "16778240", name: "Guest")
        model.assignSlot(2, kind: "capture", sourceId: "cam-1", name: "Cam")
        var opened: [String] = []
        // Row "Grade" on input 1, toolbar "Grade selected…" with input 1 selected, inspector header.
        expectEqual(model.openGradeWorkspace(slotId: 1) { opened.append($0) }, nil, "row Grade opens")
        model.sourcesWorkspace.selectedSlotId = 1
        expectEqual(model.openGradeWorkspace(slotId: model.sourcesWorkspace.selectedSlotId) { opened.append($0) }, nil,
                    "toolbar Grade selected… opens")
        expectEqual(opened, ["16778240", "16778240"], "both open the raw Zoom pid's window (the frame key), never zoom:<pid>")
        expectEqual(model.gradeEditors.count, 1, "the same editor, not a second one")
        expectEqual(model.openGradeWorkspace(slotId: 2) { opened.append($0) }, nil, "a capture input")
        expectEqual(opened.last, "capture:cam-1", "capture frame key")
        expectEqual(model.gradeNotice, "", "a successful open clears the notice")
        expect(SourcesWorkspaceState.gradeSelectedEnabled(selected: 1, slots: model.slots), "toolbar enabled on an assigned input")
        expect(!SourcesWorkspaceState.gradeSelectedEnabled(selected: 3, slots: model.slots), "disabled on an unassigned input")
        expect(!SourcesWorkspaceState.gradeSelectedEnabled(selected: nil, slots: model.slots), "disabled with no selection")
        expectEqual(SourceInspectorGradeCopy.pictureTitle, "Source grading", "SourcesWorkspaceControl.Inspector.cs:142")
        expectEqual(SourceInspectorGradeCopy.openButton, "Open grade workspace…", "Inspector.cs:144")
    }

    @MainActor static func testGradeEntryRefusalsUseTheWindowsWords() {
        let model = gradeModel()
        var opened: [String] = []
        expectEqual(model.openGradeWorkspace(slotId: nil) { opened.append($0) }, AppModel.gradeNoSourceText, "no selection")
        expectEqual(model.openGradeWorkspace(slotId: 4) { opened.append($0) }, AppModel.gradeNoSourceText, "an unassigned input")
        expectEqual(model.gradeNotice, AppModel.gradeNoSourceText, "the refusal is shown")
        for (slot, id) in [(1, "a"), (2, "b"), (3, "c"), (4, "d")] { model.assignSlot(slot, kind: "capture", sourceId: id, name: id) }
        for slot in 1...3 { _ = model.openGradeWorkspace(slotId: slot) { opened.append($0) } }
        expectEqual(model.openGradeWorkspace(slotId: 4) { opened.append($0) }, AppModel.gradeEditorLimitText, "a fourth editor")
        expectEqual(opened.count, 3, "a refusal opens no window")
    }

    @MainActor static func testTheGradedChipFollowsTheStore() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]]])
        model.assignSlot(1, kind: "capture", sourceId: "cam", name: "Cam")
        expect(!model.slotIsGraded(1), "no chip before a grade")
        _ = awaitApply(model.sourceGrades, SourceColorGrade(exposure: 5), "capture:cam", binding: "capture:cam")
        expect(model.slotIsGraded(1), "GRADED once the core acknowledged a grade")
        expect(!model.slotIsGraded(2), "an unassigned input never shows it")
    }

    // ── Task 8 ───────────────────────────────────────────────────────────────
    // Fixture invariants: a FakePreviewCore answers set-grade-preview {ok, accepted} and records
    // bodies; `failFull` makes the next FULL send throw (a renew still succeeds). Sessions are ticked
    // by hand with an explicit clock (the harness disables the 100 ms timer).

    final class FakePreviewCore: @unchecked Sendable {
        private let lock = NSLock()
        private var _bodies: [JSONObject] = []
        private var _failFull = false
        var failFull: Bool { get { lock.lock(); defer { lock.unlock() }; return _failFull }
                             set { lock.lock(); _failFull = newValue; lock.unlock() } }
        var bodies: [JSONObject] { lock.lock(); defer { lock.unlock() }; return _bodies }
        var previews: [JSONObject] { bodies.filter { $0["type"] as? String == "set-grade-preview" } }
        private func record(_ body: JSONObject) -> Bool {
            lock.lock(); defer { lock.unlock() }
            _bodies.append(body)
            let fail = _failFull && body["type"] as? String == "set-grade-preview" && body["renewOnly"] == nil
            if fail { _failFull = false }
            return fail
        }
        func handle(_ body: JSONObject) async throws -> JSONObject {
            let fail = record(body)
            if fail { throw BridgeError.remote("Grading preview preparation or event capacity is busy") }
            if body["type"] as? String == "register-grade-document" { return ["ok": true, "present": true] }
            return ["ok": true, "accepted": true]
        }
    }

    static func gradePreviewEvent(instance: String, revision: Int64, status: String = "ready", epoch: UInt64 = 7,
                                  surface: UInt32 = 501, scopes: JSONObject? = nil, source: String = "capture:cam",
                                  ageMs: Double = 0, reason: String = "") -> JSONObject {
        var event: JSONObject = ["type": "grade-preview", "instanceId": instance, "sourceId": source,
                                 "revision": NSNumber(value: revision), "sourceEpoch": NSNumber(value: epoch),
                                 "status": status, "reason": reason, "sourceAgeMs": ageMs,
                                 "texture": ["iosurfaceId": NSNumber(value: surface), "width": 1280, "height": 720,
                                             "sharedHandleHex": "", "format": "B8G8R8A8_UNORM", "frameNumber": 3]]
        if let scopes { event["scopes"] = scopes }
        return event
    }

    @MainActor static func previewEditor(_ core: FakePreviewCore) -> (AppModel, GradeEditorModel)? {
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]],
                                       "gradePreview": ["supported": true]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { return nil }
        return (model, editor)
    }

    @MainActor static func tickPreview(_ editor: GradeEditorModel, at now: Date, settle: Int = 0) {
        editor.previewSession?.tick(now: now)
        spin(until: { !(editor.previewSession?.isSending ?? false) }, limit: 2)
        _ = settle
    }

    @MainActor static func testPreviewSessionSendsRenewsResendsAndCloses() {
        let core = FakePreviewCore()
        guard let (model, editor) = previewEditor(core) else { expect(false, "opens"); return }
        let t0 = Date()
        tickPreview(editor, at: t0)
        expectEqual(core.previews.count, 1, "the first tick sends the draft")
        let first = core.previews[0]
        expectEqual(first["instanceId"] as? String, editor.previewInstanceId, "this editor's instance")
        expectEqual(first["sourceId"] as? String, "capture:cam", "the frame key")
        expectEqual(first["enabled"] as? Bool, true, "enabled")
        expect(first["renewOnly"] == nil && first["grade"] is JSONObject, "a full send carries the grade")
        let revision = FakeGradeCore.int(first["revision"])
        tickPreview(editor, at: t0.addingTimeInterval(0.2))
        expectEqual(core.previews.count, 1, "the same revision is not resent within 500 ms")
        tickPreview(editor, at: t0.addingTimeInterval(0.6))
        expectEqual(core.previews.count, 2, "renewed after 500 ms (the 3 s lease)")
        expectEqual(core.previews[1]["renewOnly"] as? Bool, true, "a renewal is renewOnly")
        expectEqual(FakeGradeCore.int(core.previews[1]["revision"]), revision, "for the same revision")
        expect(core.previews[1]["grade"] == nil, "a renewal carries no grade (MediaCoreSupervisor.cs:478-485)")
        editor.exposure = 9; editor.edited()
        core.failFull = true
        tickPreview(editor, at: t0.addingTimeInterval(0.7))
        expectEqual(core.previews.count, 3, "a new revision sends at once")
        expect((FakeGradeCore.int(core.previews.last?["revision"]) ?? 0) > (revision ?? 0), "with a higher revision")
        tickPreview(editor, at: t0.addingTimeInterval(0.8))
        expectEqual(core.previews.count, 4, "a failed send is resent IN FULL on the next tick, not renewed")
        expect(core.previews.count == 4 && core.previews[3]["renewOnly"] == nil, "in full")
        model.closeGradeEditor(sourceId: "capture:cam")
        spin(until: { core.previews.last?["enabled"] as? Bool == false }, limit: 2)
        expectEqual(core.previews.last?["enabled"] as? Bool, false, "closing the editor ends the demand (enabled:false)")
    }

    @MainActor static func testDraftAndCompareSendPreviewsNeverSetSourceGrade() {
        let core = FakePreviewCore()
        // Fixture: keep the AppModel alive; the editor's preview sends go through it ([weak self]).
        guard let (model, editor) = previewEditor(core) else { expect(false, "opens"); return }
        defer { withExtendedLifetime(model) {} }
        editor.liveEditing = false
        spin(until: { true }, limit: 0.2)
        let applies = core.bodies.filter { $0["type"] as? String == "set-source-grade" }.count
        let t0 = Date()
        editor.exposure = 12; editor.edited()
        tickPreview(editor, at: t0)
        editor.compareOriginal = true
        tickPreview(editor, at: t0.addingTimeInterval(0.05))
        let last = core.previews.last ?? [:]
        expectEqual(last["compareOriginal"] as? Bool, true, "Compare original rides the preview demand")
        expectEqual(FakeGradeCore.int((last["grade"] as? JSONObject)?["exposure"]), 12, "with the draft grade")
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        expectEqual(core.bodies.filter { $0["type"] as? String == "set-source-grade" }.count, applies,
                    "a draft edit and Compare never reach set-source-grade (ColorGradeEditorViewModelTests.cs:68)")
    }

    @MainActor static func testPreviewObservationsAreFilteredAndWordedLikeWindows() {
        let core = FakePreviewCore()
        guard let (model, editor) = previewEditor(core) else { expect(false, "opens"); return }
        let revision = editor.previewRevision
        model.handleEventForTesting(gradePreviewEvent(instance: "someone-else", revision: revision))
        expectEqual(editor.previewSurfaceId, 0, "another editor's observation is ignored")
        model.handleEventForTesting(gradePreviewEvent(instance: editor.previewInstanceId, revision: revision - 1))
        expectEqual(editor.previewSurfaceId, 0, "an older revision is ignored")
        model.handleEventForTesting(gradePreviewEvent(instance: editor.previewInstanceId, revision: revision, surface: 777))
        expectEqual(editor.previewSurfaceId, 777, "the ready observation's IOSurface is shown")
        expectEqual(editor.previewStatus, "Native graded source", "VM.cs:103")
        model.handleEventForTesting(gradePreviewEvent(instance: editor.previewInstanceId, revision: revision, status: "stale",
                                                      surface: 777, ageMs: 2300, reason: "source-not-advancing"))
        expectEqual(editor.previewStatus, "Source is not advancing — last native image held · source unchanged 2.3s",
                    "VM.cs:105,110")
        model.handleEventForTesting(gradePreviewEvent(instance: editor.previewInstanceId, revision: revision, status: "preparing",
                                                      surface: 0))
        expectEqual(editor.previewStatus, "Preparing native grading preview", "VM.cs:107")
        model.handleEventForTesting(gradePreviewEvent(instance: editor.previewInstanceId, revision: revision, status: "unavailable",
                                                      surface: 0, reason: "source-unavailable"))
        expectEqual(editor.previewStatus, "Native preview unavailable: source-unavailable", "VM.cs:108")
        expectEqual(editor.previewSurfaceId, 0, "no picture without a ready/held/stale surface")
        editor.compareOriginal = true
        model.handleEventForTesting(gradePreviewEvent(instance: editor.previewInstanceId, revision: editor.previewRevision, surface: 778))
        expectEqual(editor.previewStatus, "Original source — monitor comparison only", "VM.cs:103 (compare)")
    }

    static func scopesNode(revision: Int64, epoch: UInt64 = 7, original: Bool = false, view: Int = 0,
                           surface: UInt32 = 900, width: Int = 1536, roiEnabled: Bool = false) -> JSONObject {
        ["status": "ready", "revision": NSNumber(value: revision), "sourceEpoch": NSNumber(value: epoch), "original": original,
         "view": view, "sampleWidth": 256, "sampleHeight": 144, "sampleCount": 36864, "sourceAgeMs": 0,
         "roiPixelX": 0, "roiPixelY": 0, "roiPixelWidth": 1280, "roiPixelHeight": 720,
         "roi": ["enabled": roiEnabled, "x": 0, "y": 0, "width": 1, "height": 1, "revision": 0, "shape": "rectangle"],
         "texture": ["iosurfaceId": NSNumber(value: surface), "width": width, "height": 512, "sharedHandleHex": ""]]
    }

    @MainActor static func testScopesAreDemandedOnlyExpandedAndJudgedLikeWindows() {
        let core = FakePreviewCore()
        guard let (model, editor) = previewEditor(core) else { expect(false, "opens"); return }
        editor.scopesEnabled = true
        let t0 = Date()
        tickPreview(editor, at: t0)
        expectEqual(core.previews.last?["scopesEnabled"] as? Bool, false, "scopes are not demanded while collapsed (CaptureDraft)")
        editor.setAdvancedExpanded(true)
        tickPreview(editor, at: t0.addingTimeInterval(0.05))
        expectEqual(core.previews.last?["scopesEnabled"] as? Bool, true, "expanded + Scopes on demands them")
        editor.histogramMode = 0; editor.waveformMode = 2; editor.scopeView = 3; editor.scopesOriginal = true
        tickPreview(editor, at: t0.addingTimeInterval(0.1))
        let last = core.previews.last ?? [:]
        expectEqual(FakeGradeCore.int(last["histogramMode"]), 0, "histogram Luma")
        expectEqual(FakeGradeCore.int(last["waveformMode"]), 2, "waveform RGB parade")
        expectEqual(FakeGradeCore.int(last["scopeView"]), 3, "Expand scope: Vectorscope")
        expectEqual(last["scopesOriginal"] as? Bool, true, "Analyze original")
        let r = editor.previewRevision
        model.handleEventForTesting(gradePreviewEvent(instance: editor.previewInstanceId, revision: r,
                                                      scopes: scopesNode(revision: r, original: true, view: 3)))
        expectEqual(editor.scopeSurfaceId, 900, "a matching measurement is shown")
        expectEqual(editor.scopeStatus, "Original · Full frame · ready · unchanged 0.0s · sample 256×144 (36,864 pixels) · "
                    + "Rec.709 SDR assumed · waveform 0–100% · vectorscope 75% targets", "VM.Advanced.cs:56")
        model.handleEventForTesting(gradePreviewEvent(instance: editor.previewInstanceId, revision: r,
                                                      scopes: scopesNode(revision: r, original: false, view: 3)))
        expectEqual(editor.scopeSurfaceId, 0, "a measurement of the graded picture is refused while Analyze original is on")
        expectEqual(editor.scopeStatus, "Preparing native scopes; no measurement available.", "VM.Advanced.cs:53")
        model.handleEventForTesting(gradePreviewEvent(instance: editor.previewInstanceId, revision: r, epoch: 8,
                                                      scopes: scopesNode(revision: r, epoch: 7, original: true, view: 3)))
        expectEqual(editor.scopeSurfaceId, 0, "a measurement from another epoch is refused")
        editor.setAdvancedExpanded(false)
        expectEqual(editor.scopeStatus, "Scopes hidden.", "collapsing hides the scopes")
        expectEqual(GradeEditorModel.scopeCaptions, ["HISTOGRAM · 0–255", "WAVEFORM · 0–100%", "VECTORSCOPE · 75%"], "CGE:31-33")
    }

    @MainActor static func testAPreviewEpochSavesTheFirstQuery() {
        let core = FakeGradeCore(); core.setEpoch("capture:cam", 0)
        let model = gradeModel()
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]],
                                       "gradePreview": ["supported": true]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
        model.handleEventForTesting(gradePreviewEvent(instance: editor.previewInstanceId, revision: editor.previewRevision, epoch: 0))
        editor.exposure = 3; editor.edited()
        spin(until: { core.requests("set-source-grade").count == 1 }, limit: 2)
        expectEqual(core.requests("query-source-grade").count, 0, "the preview's epoch was used; no query (P7)")
        expectEqual(core.requests("set-source-grade").count, 1, "and the apply was accepted with it")
    }

    @MainActor static func testTheNoPreviewNoticeFollowsTheCore() {
        let model = gradeModel()
        model.applySnapshotForTesting(["captureDevices": [["id": "cam", "name": "Cam", "connectionState": "connected"]],
                                       "gradePreview": ["supported": false]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "capture:cam") else { expect(false, "opens"); return }
        expect(!model.gradePreviewSupported, "a core without the worker")
        expectEqual(editor.previewStatus, GradeEditorModel.previewNotice, "the D3 notice stays for a core without previews")
        model.applySnapshotForTesting(["gradePreview": ["supported": true]])
        expect(model.gradePreviewSupported, "a Metal core with the worker")
        expectEqual(editor.previewStatus, "Preparing native grading preview", "the notice gives way to the preview status")
    }

    @MainActor static func testACoreRespawnReconnectsThePreview() {
        let core = FakePreviewCore()
        guard let (model, editor) = previewEditor(core) else { expect(false, "opens"); return }
        let t0 = Date()
        tickPreview(editor, at: t0)
        model.handleEventForTesting(gradePreviewEvent(instance: editor.previewInstanceId, revision: editor.previewRevision, surface: 41))
        model.onConnected()
        expectEqual(editor.previewStatus, "Native preview reconnecting.", "GradePreviewCoordinator.cs:100")
        expectEqual(editor.previewSurfaceId, 0, "the dead core's surface is dropped")
        let before = core.previews.count
        tickPreview(editor, at: t0.addingTimeInterval(0.1))
        expectEqual(core.previews.count, before + 1, "the new core gets a full demand at once")
        expect(core.previews.last?["renewOnly"] == nil, "in full (the lease died with the old core)")
    }
}
