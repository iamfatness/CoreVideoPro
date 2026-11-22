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
}
