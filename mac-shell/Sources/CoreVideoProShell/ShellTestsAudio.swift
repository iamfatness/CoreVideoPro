// macOS audio parity, Phase 2 (docs/superpowers/plans/2026-11-21-mac-control-audio-phase2.md):
// A3 mastering presets + A/B, A4 meters, A6 buses. Its own file so parallel Mac
// branches extending ShellTests.swift do not collide. ContractParity scans this
// file: no `"type": "<name>"` literal for a request the core does not handle.

import Foundation

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var audioPhase2Cases: [(String, () -> Void)] {
        [
            // Task 2: the 21-field rack, catalog, library (ports of the Windows tests).
            ("audio-mastering/catalog-values", testMasteringCatalogValuesAreWindows),
            ("audio-mastering/reset-stage", testMasteringResetStageIsWindows),
            ("audio-mastering/library-save-rename-delete", testMasteringLibraryIsWindows),
            ("audio-mastering/library-clamp-and-load", testMasteringLibraryClampAndLoad),
            ("audio-mastering/wire-all-fields-and-limiter", testMasteringWireCarriesEveryFieldAndTheLimiter),
        ]
    }

    // ── Task 2 ───────────────────────────────────────────────────────────────

    /// WT/MasteringPresetCatalogTests (Create_*, Neutral_*) plus a full table: every
    /// value is transcribed from MasteringPresetCatalog.cs:35-45, never computed.
    static func testMasteringCatalogValuesAreWindows() {
        // id: (target LUFS, glue, ceiling, maxRide, input, hp, lp, low, presence, high, width)
        let table: [String: [Double]] = [
            "streaming": [-14, 0.35, -1.0, 6.0, 0.0, 30.0, 0.0, 0.5, 1.0, 0.5, 1.05],
            "podcast": [-16, 0.45, -1.0, 8.0, 0.0, 70.0, 0.0, 0.0, 1.5, 0.5, 1.0],
            "broadcast": [-23, 0.30, -1.0, 4.0, 0.0, 30.0, 0.0, 0.0, 0.5, 0.0, 1.0],
            "neutral": [-14, 0.0, -1.3, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0],
        ]
        for (id, row) in table {
            guard let p = MasteringPresetCatalog.create("  \(id.uppercased()) ") else {
                expect(false, "\(id) exists (trimmed, any case)"); continue
            }
            let got = [p.targetLufs, p.glueAmount, p.ceilingDbfs, p.maxRideDb, p.inputGainDb, p.highPassHz,
                       p.lowPassHz, p.lowShelfDb, p.presenceDb, p.highShelfDb, p.stereoWidth]
            expectEqual(got, row, "\(id) values are Windows'")
            expect(p.enabled && p.limiterEnabled, "\(id): enabled and limiter armed")
            expectEqual([p.glueRatio, p.glueAttackMs, p.glueReleaseMs, p.glueMakeupDb,
                         p.glueBandLowDb, p.glueBandMidDb, p.glueBandHighDb], [2, 30, 250, 0, 0, 0, 0],
                        "\(id): glue character defaults")
            expect(!p.glueMultiband, "\(id): multiband off")
        }
        expect(MasteringPresetCatalog.create("loud") == nil, "unknown preset is nil")
    }

    /// WT/MasteringPresetCatalogTests.ResetStage_*.
    static func testMasteringResetStageIsWindows() {
        var start = MasteringPresetCatalog.create("podcast")!
        start.inputGainDb = 2; start.maxRideDb = 8; start.stereoWidth = 1.4
        start.glueMultiband = true; start.ceilingDbfs = -0.6; start.limiterEnabled = false
        start.lowPassHz = 12000
        func reset(_ stage: String) -> MasteringParams { MasteringPresetCatalog.resetStage(start, stage: stage)! }
        let tone = reset("tone")
        expectEqual([tone.lowShelfDb, tone.presenceDb, tone.highShelfDb], [0, 0, 0], "tone zeroed")
        expectEqual([tone.targetLufs, tone.glueAmount, tone.highPassHz], [-16, 0.45, 70], "tone leaves the rest")
        expectEqual(reset("input").inputGainDb, 0, "input")
        expectEqual([reset("filter").highPassHz, reset("filter").lowPassHz], [0, 0], "filter clears HP and LP")
        expectEqual([reset("ride").maxRideDb, reset("ride").stereoWidth], [0, 1.4], "ride leaves width")
        expectEqual([reset("width").stereoWidth, reset("width").maxRideDb], [1.0, 8], "width leaves ride")
        expect(!reset("glue").glueMultiband && reset("glue").glueAmount == 0, "glue resets character")
        expect(reset("glue") == reset("dynamics"), "dynamics is glue")
        expectEqual([reset("loudness").maxRideDb, reset("loudness").stereoWidth], [0, 1.0], "loudness = ride + width")
        expect(reset("loudness") == reset("image"), "image is loudness")
        expectEqual(reset("ceiling").ceilingDbfs, -1.3, "ceiling to -1.3")
        expect(reset("ceiling").limiterEnabled, "ceiling re-arms the limiter")
        expect(reset("ceiling") == reset("limiter"), "limiter is ceiling")
        for stage in ["input", "filter", "tone", "ride", "glue", "width", "ceiling"] {
            expectEqual(reset(stage).enabled, start.enabled, "\(stage) never touches enabled")
            expectEqual(reset(stage).targetLufs, start.targetLufs, "\(stage) never touches target")
        }
        expect(MasteringPresetCatalog.resetStage(start, stage: "eq") == nil, "unknown stage is nil")
    }

    /// WT/MasteringPresetLibraryTests (Save_*, Rename_*, Delete_*).
    static func testMasteringLibraryIsWindows() {
        var settings = MasteringPresetCatalog.neutral
        settings.glueAmount = 0.4
        guard let first = MasteringPresetLibrary.save([], name: "  Sunday Show ", settings: settings) else {
            expect(false, "save succeeds"); return
        }
        expectEqual(first.saved.name, "Sunday Show", "trimmed")
        expect(first.saved.id.hasPrefix("user-") && first.saved.id.count == 37, "user-<32 hex>")
        expectEqual(first.saved.settings.glueAmount, 0.4, "settings stored")

        var a = settings; a.glueAmount = 0.2
        var b = settings; b.glueAmount = 0.9
        let one = MasteringPresetLibrary.save([], name: "Show", settings: a)!
        let two = MasteringPresetLibrary.save(one.presets, name: "show", settings: b)!
        expectEqual(two.presets.count, 1, "same name (any case) overwrites")
        expectEqual(two.saved.id, one.saved.id, "keeps the id")
        expectEqual(two.saved.name, "show", "takes the new casing")
        expectEqual(two.saved.settings.glueAmount, 0.9, "new settings")

        for bad in [nil, "", "   ", "Streaming", "neutral", "  Podcast  ",
                    String(repeating: "x", count: 49)] as [String?] {
            expect(MasteringPresetLibrary.save([], name: bad, settings: settings) == nil,
                   "rejects \(String(describing: bad))")
        }
        expect(MasteringPresetLibrary.save([], name: String(repeating: "x", count: 48), settings: settings) != nil,
               "48 characters is allowed")

        var full: [MasteringUserPreset] = []
        for index in 0..<32 { full = MasteringPresetLibrary.save(full, name: "Preset \(index)", settings: settings)!.presets }
        expect(MasteringPresetLibrary.save(full, name: "One more", settings: settings) == nil, "full library refuses a new name")
        expect(MasteringPresetLibrary.save(full, name: "Preset 0", settings: b) != nil, "overwrite still works when full")

        let lib = MasteringPresetLibrary.save(MasteringPresetLibrary.save([], name: "One", settings: a)!.presets,
                                              name: "Two", settings: b)!.presets
        let renamed = MasteringPresetLibrary.rename(lib, id: lib[0].id, to: "Uno")!
        expectEqual(renamed.renamed.id, lib[0].id, "rename keeps id")
        expectEqual(renamed.renamed.settings.glueAmount, 0.2, "rename keeps settings")
        expectEqual(renamed.presets.map(\.name), ["Uno", "Two"], "rename keeps position")
        expect(MasteringPresetLibrary.rename(lib, id: "user-missing", to: "X") == nil, "unknown id")
        expect(MasteringPresetLibrary.rename(lib, id: lib[0].id, to: "Broadcast") == nil, "built-in name")
        expect(MasteringPresetLibrary.rename(lib, id: lib[0].id, to: "two") == nil, "another preset's name")
        expect(MasteringPresetLibrary.rename(lib, id: lib[0].id, to: "ONE") != nil, "own name, new case")

        let deleted = MasteringPresetLibrary.delete(lib, id: lib[0].id)!
        expectEqual(deleted.presets.map(\.name), ["Two"], "delete removes only the target")
        expect(MasteringPresetLibrary.delete(lib, id: "user-missing") == nil, "unknown id")
    }

    /// WT/MasteringPresetLibraryTests.FromPersisted_* and FromPersistedPresets_*.
    static func testMasteringLibraryClampAndLoad() {
        var p = MasteringParams()
        p.targetLufs = 9; p.ceilingDbfs = 3.0; p.glueRatio = 100; p.stereoWidth = 5
        p.glueAttackMs = 0; p.maxRideDb = 99; p.highPassHz = 900; p.glueBandLowDb = -40
        let c = MasteringPresetLibrary.clamped(p)
        expectEqual([c.targetLufs, c.ceilingDbfs, c.glueRatio, c.stereoWidth, c.glueAttackMs, c.maxRideDb,
                     c.highPassHz, c.glueBandLowDb], [-9, -0.5, 10, 2, 1, 12, 300, -6],
                    "rack limits (target: the Mac's -24...-9 slider)")
        let settings = MasteringPresetCatalog.neutral
        let input = [
            MasteringUserPreset(id: "user-1", name: "Keep Me", settings: p),
            MasteringUserPreset(id: " ", name: "Blank Id", settings: settings),
            MasteringUserPreset(id: "user-2", name: "  ", settings: settings),
            MasteringUserPreset(id: "user-3", name: "Streaming", settings: settings),
            MasteringUserPreset(id: "user-4", name: "keep me", settings: settings),
        ]
        let loaded = MasteringPresetLibrary.loaded(input)
        expectEqual(loaded.map(\.id), ["user-1"], "only Keep Me survives")
        expectEqual(loaded.first?.settings.ceilingDbfs, -0.5, "each loaded preset is clamped")
    }

    /// The mix command carries the whole rack: 20 mastering keys and the
    /// top-level limiter that follows the params (never hard-coded on).
    @MainActor
    static func testMasteringWireCarriesEveryFieldAndTheLimiter() {
        let model = AppModel()
        model.editMastering { p in
            p.lowPassHz = 16000; p.glueRatio = 3; p.glueAttackMs = 20; p.glueReleaseMs = 300
            p.glueMakeupDb = 1; p.glueMultiband = true; p.glueBandLowDb = -2; p.glueBandMidDb = 0.5
            p.glueBandHighDb = 1; p.limiterEnabled = false
        }
        let command = model.audioMixCommand()
        let mastering = command["mastering"] as? JSONObject ?? [:]
        let expected: [String: Double] = ["lowPassHz": 16000, "glueRatio": 3, "glueAttackMs": 20,
                                          "glueReleaseMs": 300, "glueMakeupDb": 1, "glueBandLowDb": -2,
                                          "glueBandMidDb": 0.5, "glueBandHighDb": 1]
        for (key, value) in expected {
            expectEqual((mastering[key] as? NSNumber)?.doubleValue, value, "mastering.\(key)")
        }
        expectEqual(mastering["glueMultiband"] as? Bool, true, "mastering.glueMultiband")
        expectEqual(mastering.count, 20, "exactly the 20 Windows keys")
        expect(mastering["limiterEnabled"] == nil, "limiterEnabled is never inside mastering")
        expectEqual(command["limiterEnabled"] as? Bool, false, "top-level limiter follows the rack")
    }
}
