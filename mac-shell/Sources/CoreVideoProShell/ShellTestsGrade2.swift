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
}
