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
}
