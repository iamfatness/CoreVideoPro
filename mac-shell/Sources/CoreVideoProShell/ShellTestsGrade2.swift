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
            // Task 9 (G7): region of interest.
            ("grade2/roi-model-and-measurement", testScopeRoiModelMatchesTheWindowsRecord),
            ("grade2/roi-geometry", testScopeRoiGeometryIsTheWindowsGeometry),
            ("grade2/roi-gestures", testRoiGesturesDrawMoveResizeAndCancel),
            ("grade2/roi-any-monitor-size", testRoiMapsOntoThePictureAtAnyMonitorSize),
            ("grade2/editor-two-columns", testTheEditorWindowHoldsBothColumns),
            ("grade2/roi-keys", testRoiArrowKeysMoveAndShiftResizes),
            ("grade2/roi-editor-revision-and-wire", testTheEditorBumpsTheRoiRevisionOnlyOnARealChange),
            ("grade2/roi-persistence", testScopeRegionsPersistPerPersonAndNeverUnderSomeoneElse),
            ("grade2/roi-tools-precision-and-round-circle", testRoiToolsPrecisionSlidersAndARoundCircle),
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

    // ── Task 9 ───────────────────────────────────────────────────────────────
    // Fixture invariants: a 1920x1080 source in a 1000x450 host is pillarboxed to (100,0,800,450)
    // (ScopeRoiGeometryTests.cs:20-25), so host x = 100 + 800·nx and host y = 450·ny.

    static func near(_ a: Double, _ b: Double, _ tol: Double = 1e-8) -> Bool { abs(a - b) <= tol }

    static func testScopeRoiModelMatchesTheWindowsRecord() {
        let roi = GradeScopeRoi(enabled: true, x: 0.123456789, y: 0.19999999999999998, width: 0.22499999999999998,
                                height: 0.4, revision: 7, shape: "circle")
        var wire = roi; wire.x = 0.123457; wire.y = 0.2; wire.width = 0.225
        expect(roi.matchesMeasurement(wire), "six-digit native formatting still matches (ScopeRoiGeometryTests.cs:9-11)")
        var stale = wire; stale.revision = 6
        expect(!roi.matchesMeasurement(stale), "a stale revision does not")
        var rect = wire; rect.shape = "rectangle"
        expect(!roi.matchesMeasurement(rect), "another shape does not")
        var wider = wire; wider.width = 0.226
        expect(!roi.matchesMeasurement(wider), "another size does not")
        let edge = GradeScopeRoi(enabled: true, x: 1279.0 / 1280, y: 0, width: 1.0 / 1280, height: 0.1, revision: 7)
        var reported = edge; reported.x = 0.999219
        expect(!reported.isValid && edge.matchesMeasurement(reported), "a rounded reply a hair past the edge still matches")
        expectEqual(GradeScopeRoi(enabled: true, x: 0.1, y: 0.2, width: 0.2, height: 0.3, revision: 4, shape: "circle").expectedSampleCount,
                    28960, "circle sample count (ScopeRoi.h sampleCount)")
        expectEqual(GradeScopeRoi().expectedSampleCount, 36864, "full frame 256x144")
        var ellipse = roi; ellipse.shape = "ellipse"
        expect(!ellipse.isValid, "only rectangle and circle")
        let px = GradeScopeRoi(enabled: true, x: 0.75, y: 0.5, width: 1.0 / 64, height: 1.0 / 64).pixels(width: 64, height: 64)
        expect(px?.x == 48 && px?.y == 32 && px?.width == 1 && px?.height == 1, "the core's pixel rule (ScopeRoi.h:30-35)")
    }

    static func testScopeRoiGeometryIsTheWindowsGeometry() {
        let fit = ScopeRoiGeometry.fitImage(host: CGSize(width: 800, height: 800), source: (1920, 1080))
        expectEqual(fit, CGRect(x: 0, y: 175, width: 800, height: 450), "letterbox")
        expectEqual(ScopeRoiGeometry.fitImage(host: CGSize(width: 1000, height: 450), source: (1920, 1080)),
                    CGRect(x: 100, y: 0, width: 800, height: 450), "pillarbox")
        expectEqual(ScopeRoiGeometry.fitImage(host: CGSize(width: 1000, height: 450), source: (0, 1080)), .zero, "no source")
        for (x, y, ex, ey) in [(0.2, 0.2, 0.6, 0.7), (0.6, 0.7, 0.2, 0.2), (0.2, 0.7, 0.6, 0.2), (0.6, 0.2, 0.2, 0.7)] {
            let r = ScopeRoiGeometry.draw(start: (x, y), end: (ex, ey), source: (1920, 1080), shape: "circle")
            expect(r.isValid && near(r.width * 1920, 540) && near(r.height * 1080, 540),
                   "a circle is round in source pixels drawn \(x),\(y)→\(ex),\(ey)")
            expect(r.x >= min(x, ex) && r.y >= min(y, ey) && r.x + r.width <= max(x, ex) + 1e-9 && r.y + r.height <= max(y, ey) + 1e-9,
                   "and stays inside the drag")
        }
        for shape in ["circle", "rectangle"] {
            let r = ScopeRoiGeometry.draw(start: (1, 1), end: (1, 1), source: (1920, 1080), shape: shape)
            expect(r.isValid && near(r.width * 1920, 1) && near(r.height * 1080, 1), "a click at the edge is a valid 1-pixel \(shape)")
            expect(ScopeRoiGeometry.draw(start: (-1, -1), end: (2, 2), source: (1920, 1080), shape: shape).isValid, "outside clamps")
        }
        let sized = ScopeRoiGeometry.circleSize(GradeScopeRoi(enabled: true, x: 0.8, y: 0.8, width: 0.1, height: 0.1, revision: 3, shape: "circle"),
                                                diameter: 1000, source: (1920, 1080))
        expect(sized.isValid && near(sized.width * 1920, 216) && near(sized.height * 1080, 216) && sized.revision == 3,
               "a precision resize fits the image and links the axes (ScopeRoiGeometryTests.cs:48-53)")
    }

    static func testRoiGesturesDrawMoveResizeAndCancel() {
        let host = CGSize(width: 1000, height: 450)
        func h(_ nx: Double, _ ny: Double) -> CGPoint { CGPoint(x: 100 + 800 * nx, y: 450 * ny) }
        var gesture = ScopeRoiGesture(source: (1920, 1080))
        gesture.drawingShape = "rectangle"
        expect(!gesture.begin(at: CGPoint(x: 50, y: 100), host: host), "a press in the pillarbox does nothing")
        expect(gesture.begin(at: h(0.2, 0.2), host: host), "a press on the picture starts drawing")
        expectEqual(gesture.update(to: h(0.2 + 1.0 / 800, 0.2), host: host), nil, "under 3 px is not a drag yet")
        let drawn = gesture.update(to: h(0.6, 0.5), host: host)
        expect(drawn != nil && near(drawn!.x, 0.2) && near(drawn!.width, 0.4) && near(drawn!.height, 0.3) && drawn!.enabled,
               "a rectangle from the drag")
        gesture.complete()
        expectEqual(gesture.drawingShape, nil, "completing returns to Select / move (ScopeRoiControl.cs CompleteDrawingGesture)")
        expect(gesture.begin(at: h(0.4, 0.35), host: host), "a press inside the region moves it")
        let moved = gesture.update(to: h(0.9, 0.35), host: host)
        expect(moved != nil && near(moved!.x, 0.6) && near(moved!.width, 0.4), "a move clamps to the right edge")
        gesture.complete()
        expect(gesture.begin(at: h(1.0, 0.5), host: host), "a press on the right-bottom corner resizes")
        let resized = gesture.update(to: h(0.8, 0.4), host: host)
        expect(resized != nil && near(resized!.x, 0.6) && near(resized!.width, 0.2) && near(resized!.height, 0.2), "corner resize")
        gesture.cancel()
        expect(near(gesture.roi.width, 0.4), "Escape restores the region from before the gesture")
        var circle = ScopeRoiGesture(source: (1920, 1080))
        circle.drawingShape = "circle"
        _ = circle.begin(at: h(0.1, 0.1), host: host)
        let c = circle.update(to: h(0.5, 0.9), host: host)!
        circle.complete()
        expect(near(c.width * 1920, c.height * 1080, 1e-6), "a drawn circle is round in pixels")
        _ = circle.begin(at: h(c.x + c.width, c.y + c.height / 2), host: host)       // right edge handle
        let r = circle.update(to: h(c.x + c.width * 0.75, c.y + c.height / 2), host: host)!
        expect(near(r.width * 1920, r.height * 1080, 1e-6), "resizing a circle keeps it round (ScopeRoiControl.cs:118-124)")
        expect(r.width < c.width, "and smaller")
    }

    /// The two-column editor gives the preview whatever height the monitor column leaves, so the
    /// overlay's host is no longer a fixed 270 pt. The same drag over the PICTURE must produce the
    /// same region at every monitor size — the old fixed frame, the default window, the minimum one,
    /// and a tall pillarboxed one — or the guide would drift off the subject as the window resizes.
    static func testRoiMapsOntoThePictureAtAnyMonitorSize() {
        let hosts = [CGSize(width: 900, height: 270),        // the old fixed-height monitor
                     CGSize(width: 864, height: 330),        // the default 1280×820 window
                     CGSize(width: 592, height: 180),        // the 1000×700 minimum window
                     CGSize(width: 600, height: 700)]        // a tall, narrow column: letterboxed
        var regions: [GradeScopeRoi] = []
        for host in hosts {
            let image = ScopeRoiGeometry.fitImage(host: host, source: (1920, 1080))
            expect(image.minX >= 0 && image.minY >= 0 && image.maxX <= host.width + 1e-9 && image.maxY <= host.height + 1e-9
                   && near(Double(image.width / image.height), 1920.0 / 1080.0, 1e-9),
                   "the fitted picture keeps 16:9 inside a \(host) monitor")
            func p(_ nx: Double, _ ny: Double) -> CGPoint {
                CGPoint(x: image.minX + image.width * nx, y: image.minY + image.height * ny)
            }
            var gesture = ScopeRoiGesture(source: (1920, 1080))
            gesture.drawingShape = "rectangle"
            expect(gesture.begin(at: p(0.25, 0.3), host: host), "a press on the picture starts drawing in a \(host) monitor")
            if let drawn = gesture.update(to: p(0.75, 0.8), host: host) { regions.append(drawn) }
            gesture.complete()
        }
        expectEqual(regions.count, hosts.count, "every monitor size produced a region")
        for (host, r) in zip(hosts, regions) {
            expect(near(r.x, 0.25) && near(r.y, 0.3) && near(r.width, 0.5) && near(r.height, 0.5),
                   "the same picture drag is the same region in a \(host) monitor (got \(r.x),\(r.y) \(r.width)×\(r.height))")
        }
    }

    /// The window's own sizes hold the two columns: the controls column at Windows' 360 pt, and at
    /// the minimum size the picture and the scope strip at their own minimums one above the other.
    static func testTheEditorWindowHoldsBothColumns() {
        expectEqual(SourceGradeEditorPane.controlsWidth, 360, "controls column width (ColorGradeEditorWindow.xaml:7)")
        let minimum = SourceGradeEditorPane.minimumSize, standard = SourceGradeEditorPane.defaultSize
        expect(standard.width >= minimum.width && standard.height >= minimum.height, "the default window is at least the minimum")
        expect(minimum.width - SourceGradeEditorPane.controlsWidth - 48 >= 560,
               "the monitor column keeps a usable width at the minimum window")
        expect(GradePreviewMonitor.minimumHeight + GradeScopeStrip.minimumImageHeight + 340 <= minimum.height,
               "picture + scopes + the column's fixed rows fit the minimum height without scrolling")
        expect(GradeScopeStrip.imageHeight == 300, "Windows' scope height when there is room (CGE:35)")
    }

    static func testRoiArrowKeysMoveAndShiftResizes() {
        var gesture = ScopeRoiGesture(source: (1920, 1080))
        gesture.roi = GradeScopeRoi(enabled: true, x: 0.5, y: 0.5, width: 0.1, height: 0.1)
        _ = gesture.key(dx: 1, dy: 0, shift: false)
        expect(near(gesture.roi.x, 0.5 + 1.0 / 1920), "Right moves one source pixel")
        _ = gesture.key(dx: 0, dy: 1, shift: true)
        expect(near(gesture.roi.height, 0.1 + 1.0 / 1080), "Shift+Down grows one source pixel")
        gesture.roi = GradeScopeRoi(enabled: true, x: 0.95, y: 0.5, width: 0.05, height: 0.1)
        _ = gesture.key(dx: 1, dy: 0, shift: false)
        expect(near(gesture.roi.x, 0.95), "a move never leaves the picture")
        gesture.roi = GradeScopeRoi(enabled: true, x: 0.2, y: 0.2, width: 100.0 / 1920, height: 100.0 / 1080, shape: "circle")
        _ = gesture.key(dx: 1, dy: 0, shift: true)
        expect(near(gesture.roi.width * 1920, 101, 1e-6) && near(gesture.roi.height * 1080, 101, 1e-6), "a circle grows in both axes")
        var off = ScopeRoiGesture(source: (1920, 1080))
        expect(!off.key(dx: 1, dy: 0, shift: false), "keys do nothing without a region")
    }

    @MainActor static func testTheEditorBumpsTheRoiRevisionOnlyOnARealChange() {
        let core = FakePreviewCore()
        guard let (model, editor) = previewEditor(core) else { expect(false, "opens"); return }
        defer { withExtendedLifetime(model) {} }
        expectEqual(editor.scopeRegionLabel, "Full frame", "no region")
        editor.scopeRoiEnabled = true
        expectEqual(editor.scopeRoi, GradeScopeRoi(enabled: true, x: 0.25, y: 0.25, width: 0.5, height: 0.5, revision: 1),
                    "turning the region on from full frame centres it (ColorGradeEditorViewModel.Roi.cs:32-33)")
        editor.setScopeRoi(GradeScopeRoi(enabled: true, x: 0.25, y: 0.25, width: 0.5, height: 0.5, revision: 99))
        expectEqual(editor.scopeRoi.revision, 1, "the same region is not a new measurement (only the revision differs)")
        editor.setScopeRoi(GradeScopeRoi(enabled: true, x: 0.3, y: 0.25, width: 0.5, height: 0.5))
        expectEqual(editor.scopeRoi.revision, 2, "a real change bumps the selection revision")
        expectEqual(editor.scopeStatus, "Updating scope region; waiting for matching native measurement.", "Roi.cs:43")
        editor.setScopeRoi(GradeScopeRoi(enabled: true, x: 0.8, y: 0, width: 0.5, height: 0.5))
        expectEqual(editor.scopeRoi.revision, 2, "an invalid region is refused")
        expectEqual(editor.scopeRegionLabel, "Rectangle ROI · 30.0%, 25.0% · 50.0% × 50.0% · selection 2", "Roi.cs:29")
        editor.setAdvancedExpanded(true); editor.scopesEnabled = true
        tickPreview(editor, at: Date())
        let wire = core.previews.last?["scopeRoi"] as? JSONObject
        expectEqual(wire?["enabled"] as? Bool, true, "the region rides the preview demand")
        expectEqual(FakeGradeCore.int(wire?["revision"]), 2, "with its selection revision")
        editor.clearScopeRoi()
        expect(!editor.scopeRoi.enabled && editor.scopeRoi.revision == 3, "Clear measures the full frame (a new revision)")
        editor.resetScopeRoi()
        expect(editor.scopeRoi.enabled && near(editor.scopeRoi.x, 0.25) && editor.scopeRoi.revision == 4, "Reset region to center")
    }

    @MainActor static func testScopeRegionsPersistPerPersonAndNeverUnderSomeoneElse() {
        let path = tempGradeFile()
        var file = SourceGradeFile()
        for i in 0..<70 { file.rememberScopeRegion(GradeScopeRoi(enabled: true, x: 0.1, y: 0.1, width: 0.2, height: 0.2, revision: 9),
                                                   key: "capture:cam-\(i)") }
        expectEqual(file.scopeRegions.count, 64, "at most 64 regions, oldest retired")
        expect(file.scopeRegions["capture:cam-0"] == nil && file.scopeRegions["capture:cam-69"] != nil, "oldest first")
        expectEqual(file.scopeRegions["capture:cam-69"]?.revision, 0, "stored with revision 0 (ScopePreferences.cs:21)")
        try? file.save(to: path)
        var raw = (try? JSONSerialization.jsonObject(with: Data(contentsOf: URL(fileURLWithPath: path)))) as? JSONObject ?? [:]
        var regions = raw["scopeRegions"] as? JSONObject ?? [:]
        regions["capture:bad"] = ["enabled": true, "x": 0.9, "y": 0, "width": 0.5, "height": 0.5, "revision": 0, "shape": "rectangle"]
        regions["mystery:key"] = ["enabled": true, "x": 0, "y": 0, "width": 1, "height": 1, "revision": 0, "shape": "rectangle"]
        raw["scopeRegions"] = regions
        try? JSONSerialization.data(withJSONObject: raw).write(to: URL(fileURLWithPath: path))
        let loaded = SourceGradeFile.load(from: path).file
        expectEqual(loaded.scopeRegions.count, 64, "an invalid region and a foreign key are dropped on load")
        expect(loaded.scopeRegions["capture:bad"] == nil, "invalid dropped")

        // Through the model: the region is seeded on open and saved under the key captured at open.
        let core = FakePreviewCore()
        let model = gradeModel(file: tempGradeFile())
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.applySnapshotForTesting(["gradePreview": ["supported": true]])
        model.applyZoomForTesting(["meetingState": "in-meeting",
                                   "participants": [["userId": "16778240", "displayName": "Guest", "videoOn": true,
                                                     "persistentId": "P-a", "sourceGeneration": 1]]])
        guard case .opened(let editor) = model.openGradeEditor(sourceId: "16778240") else { expect(false, "opens"); return }
        editor.setScopeRoi(GradeScopeRoi(enabled: true, x: 0.1, y: 0.2, width: 0.3, height: 0.3, shape: "circle"))
        expectEqual(model.sourceGrades.scopeRegion(forKey: "zoom-person:P-a")?.x, 0.1, "saved under the person's stable key")
        model.closeGradeEditor(sourceId: "16778240")
        guard case .opened(let again) = model.openGradeEditor(sourceId: "16778240") else { expect(false, "reopens"); return }
        expect(again.scopeRoi.enabled && near(again.scopeRoi.x, 0.1) && again.scopeRoi.shape == "circle",
               "reopening restores the region (ScopePreferences.cs:18)")
        model.applyZoomForTesting(["meetingState": "in-meeting",
                                   "participants": [["userId": "16778240", "displayName": "Someone else", "videoOn": true,
                                                     "persistentId": "P-b", "sourceGeneration": 1]]])
        again.setScopeRoi(GradeScopeRoi(enabled: true, x: 0.5, y: 0.5, width: 0.1, height: 0.1))
        expect(model.sourceGrades.scopeRegion(forKey: "zoom-person:P-b") == nil, "never written under the new person")
        expectEqual(model.sourceGrades.scopeRegion(forKey: "zoom-person:P-a")?.x, 0.1, "and the departed person's region is untouched")
    }

    @MainActor static func testRoiToolsPrecisionSlidersAndARoundCircle() {
        let core = FakePreviewCore()
        guard let (model, editor) = previewEditor(core) else { expect(false, "opens"); return }
        defer { withExtendedLifetime(model) {} }
        editor.scopeRoiGuideVisible = false
        editor.armRoi("circle")
        expect(editor.advancedExpanded && editor.scopesEnabled && editor.scopeRoiGuideVisible && editor.roiDrawingShape == "circle",
               "arming a tool opens the panel, turns scopes and the guide on (Roi.cs:41-45)")
        expectEqual(editor.roiToolHint, "Drag to draw a circle over the subject. Escape cancels.", "Roi.cs:22")
        editor.armRoi(nil)
        expectEqual(editor.roiToolHint, "Choose a shape, then drag over the subject to measure its scopes.", "Roi.cs:24")
        // A 1920x1080 preview texture arrives; a circle is then edited through the sliders.
        editor.observePreviewSize(width: 1920, height: 1080)
        editor.setScopeRoi(GradeScopeRoi(enabled: true, x: 0.1, y: 0.1, width: 216.0 / 1920, height: 216.0 / 1080, shape: "circle"))
        editor.setRoiNumber(2, 432.0 / 1920)
        expect(near(editor.scopeRoi.width * 1920, 432) && near(editor.scopeRoi.height * 1080, 432),
               "the width slider resizes a circle in both axes, round in pixels (Roi.cs:34-37)")
        editor.setRoiNumber(3, 108.0 / 1080)
        expect(near(editor.scopeRoi.width * 1920, 108) && near(editor.scopeRoi.height * 1080, 108), "and the height slider by height")
        let before = editor.scopeRoi.revision
        editor.setRoiNumber(0, 0.2)
        expect(near(editor.scopeRoi.x, 0.2) && editor.scopeRoi.revision == before + 1, "left moves it, one new selection")
        // The picture changes shape (a portrait source): the circle is re-made round (Roi.cs:9-13).
        editor.observePreviewSize(width: 1080, height: 1920)
        expect(near(editor.scopeRoi.width * 1080, editor.scopeRoi.height * 1920, 0.01), "a circle stays round when the picture's shape changes")
        let rect = GradeScopeRoi(enabled: true, x: 0.1, y: 0.1, width: 0.3, height: 0.2)
        editor.setScopeRoi(rect)
        let rev = editor.scopeRoi.revision
        editor.observePreviewSize(width: 1920, height: 1080)
        expectEqual(editor.scopeRoi.revision, rev, "a rectangle is left alone")
        editor.setRoiNumber(2, 0.5)
        expect(near(editor.scopeRoi.width, 0.5) && near(editor.scopeRoi.height, 0.2), "a rectangle's width slider moves width only")
    }
}
