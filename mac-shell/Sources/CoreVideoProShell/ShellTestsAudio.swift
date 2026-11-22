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
            // Task 3: A/B, presets on the model, persistence before the first mix, the limiter action.
            ("audio-mastering/restore-before-first-mix", testMasteringRestoredBeforeTheFirstMixCommand),
            ("audio-mastering/ab-compare", testMasteringCompareSlotsAreWindows),
            ("audio-mastering/preset-into-active-slot", testMasteringPresetLandsInTheActiveSlot),
            ("audio-mastering/prefs-active-slot-is-live", testMasteringPrefsSerialiseTheActiveSlotFromTheLiveRack),
            ("control/master-limiter", testControlMasterLimiterAction),
            // Task 5: meter scale, readouts and decode.
            ("audio-meters/scale-vectors", testAudioMeterScaleIsWindows),
            ("audio-meters/readout-labels", testMasterMeterReadoutLabels),
            ("audio-meters/decode", testMasterMeterDecodeFromTheSnapshot),
            ("audio-meters/zoom-muted-is-display-only", testZoomMutedIsDisplayOnly),
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

    // ── Task 3 ───────────────────────────────────────────────────────────────

    /// The regression A2 exists for: the rack "silently resets to bypass" on a
    /// relaunch. A restored rack must be what the FIRST mix command carries.
    @MainActor
    static func testMasteringRestoredBeforeTheFirstMixCommand() {
        var rack = MasteringParams()
        rack.enabled = true; rack.glueAmount = 0.62; rack.lowPassHz = 16000; rack.limiterEnabled = false
        var prefs = ShellPrefs()
        prefs.masteringCurrent = rack
        prefs.masteringCompareA = MasteringPresetCatalog.create("podcast")
        prefs.masteringCompareSlot = "b"
        prefs.masteringUserPresets = [MasteringUserPreset(id: "user-1", name: "Show", settings: rack),
                                      MasteringUserPreset(id: "user-2", name: "Neutral", settings: rack)]
        // Through the real file round trip, not just the struct.
        let data = (try? JSONEncoder().encode(prefs)) ?? Data()
        let decoded = (try? JSONDecoder().decode(ShellPrefs.self, from: data)) ?? ShellPrefs()
        let model = AppModel()
        model.applyRestoredPrefs(decoded)
        let command = model.audioMixCommand()
        let mastering = command["mastering"] as? JSONObject ?? [:]
        expectEqual(mastering["enabled"] as? Bool, true, "first command: rack in")
        expectEqual((mastering["glueAmount"] as? NSNumber)?.doubleValue, 0.62, "first command: saved glue")
        expectEqual((mastering["lowPassHz"] as? NSNumber)?.doubleValue, 16000, "first command: saved low-pass")
        expectEqual(command["limiterEnabled"] as? Bool, false, "first command: saved limiter")
        expectEqual(model.masteringCompareSlot, "B", "slot B restored (any case)")
        expectEqual(model.masteringCompareB, rack, "a missing B falls back to current")
        expectEqual(model.masteringCompareA.targetLufs, -16, "A is the saved podcast rack")
        expectEqual(model.masteringUserPresets.map(\.id), ["user-1"], "presets restored, built-in name dropped")

        let fresh = AppModel()
        fresh.applyRestoredPrefs(ShellPrefs())
        expectEqual(fresh.mastering, MasteringParams(), "no saved rack: defaults kept")
        expectEqual(fresh.masteringCompareSlot, "A", "no saved rack: slot A")
    }

    /// Windows SelectMasteringCompareSlot (SVM:3241-3268).
    @MainActor
    static func testMasteringCompareSlotsAreWindows() {
        let model = AppModel()
        model.editMastering { $0.glueAmount = 0.7 }
        model.selectMasteringCompareSlot("b")
        expectEqual(model.masteringCompareSlot, "B", "\"b\" selects B")
        expectEqual(model.mastering, MasteringPresetCatalog.neutral, "B recalls its own (neutral) rack")
        expectEqual(model.masteringCompareA.glueAmount, 0.7, "the slot left keeps the live rack")
        expectEqual(model.masteringStatus, "Master processor recalled comparison B.", "Windows status")
        model.editMastering { $0.glueAmount = 0.2 }
        model.selectMasteringCompareSlot("x")
        expectEqual(model.masteringCompareSlot, "A", "anything but B is A")
        expectEqual(model.mastering.glueAmount, 0.7, "switching back recalls A's tweak")
        expectEqual(model.masteringCompareB.glueAmount, 0.2, "B kept its tweak")
        model.masteringStatus = ""
        model.editMastering { $0.glueAmount = 0.9 }
        model.selectMasteringCompareSlot("A")
        expectEqual(model.masteringStatus, "", "same slot: nothing happens")
        expectEqual(model.mastering.glueAmount, 0.9, "same slot: the live rack is untouched")
    }

    @MainActor
    static func testMasteringPresetLandsInTheActiveSlot() {
        let model = AppModel()
        model.selectMasteringCompareSlot("B")
        expect(model.applyMasteringPreset("Podcast"), "podcast applies")
        expectEqual(model.masteringStatus, "Podcast mastering starting point applied to comparison B.", "Windows status")
        model.selectMasteringCompareSlot("A")
        model.selectMasteringCompareSlot("B")
        expectEqual(model.mastering, MasteringPresetCatalog.create("podcast")!, "B is podcast after a round trip")
        expect(!model.applyMasteringPreset("loud"), "an unknown preset changes nothing")
        guard let saved = model.saveMasteringUserPreset(name: "Gala") else { expect(false, "save"); return }
        model.selectMasteringCompareSlot("A")
        expect(model.applyMasteringUserPreset(id: saved.id), "user preset applies")
        expectEqual(model.masteringCompareA, MasteringPresetCatalog.create("podcast")!, "user preset lands in A")
        expect(model.resetMasteringStage("tone"), "reset tone")
        expectEqual(model.masteringCompareA.presenceDb, 0, "reset lands in the active slot too")
        expectEqual(model.masteringStatus, "Tone stage reset to neutral.", "Windows status")
        expect(model.saveMasteringUserPreset(name: "streaming") == nil, "built-in name refused")
        expectEqual(model.masteringStatus, "That name belongs to a built-in starting point — pick another.", "Windows text")
    }

    @MainActor
    static func testMasteringPrefsSerialiseTheActiveSlotFromTheLiveRack() {
        let model = AppModel()
        model.selectMasteringCompareSlot("B")
        model.editMastering { $0.glueAmount = 0.55 }
        let prefs = model.currentPrefs()
        expectEqual(prefs.masteringCurrent?.glueAmount, 0.55, "current is the live rack")
        expectEqual(prefs.masteringCompareB?.glueAmount, 0.55, "the active slot is serialised from the live rack")
        expectEqual(prefs.masteringCompareA, MasteringParams(), "the other slot as stored (the live rack when A was left)")
        expectEqual(prefs.masteringCompareSlot, "B", "slot")
    }

    /// Windows StudioControlSurface.cs:659-661 + ControlState.MasterLimiterOn.
    @MainActor
    static func testControlMasterLimiterAction() {
        let model = AppModel()
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        expectEqual(invoke(surface, "audio.masterLimiter.set", [false]), .success, "limiter off accepted")
        expect(!model.mastering.limiterEnabled, "the rack's limiter is off")
        expectEqual(model.audioMixCommand()["limiterEnabled"] as? Bool, false, "and reaches the wire")
        expectEqual(AppModelControlSurface.state(model).dictionary["masterLimiterOn"] as? Bool, false, "state: off")
        expectEqual(invoke(surface, "audio.masterLimiter.set", [true]), .success, "limiter on")
        expectEqual(AppModelControlSurface.state(model).dictionary["masterLimiterOn"] as? Bool, true, "state: on")
    }

    // ── Task 5 ───────────────────────────────────────────────────────────────

    /// WT/AudioMeterScaleTests vectors, plus the rounding row that separates
    /// Math.Round from truncation.
    static func testAudioMeterScaleIsWindows() {
        let vectors: [(Double, Int)] = [(-120, 0), (-60, 0), (-48, 20), (-36, 40), (-24, 60), (-12, 80),
                                        (-6, 90), (0, 100), (-0.3, 100), (-59.9, 0), (-30.3, 50), (6, 100)]
        for (dbfs, level) in vectors {
            expectEqual(AudioMeterScale.toLevel(dbfs), level, "\(dbfs) dBFS -> \(level)")
        }
        expectEqual(AudioMeterScale.toLevel(-3, muted: true), 0, "muted reads 0")
        expectEqual(AudioMeterScale.toLevel(.nan), 0, "NaN reads 0")
        expectEqual(AudioConsolePolicy.meterLevel(dbfs: -0.3), 100, "the strip scale is the Windows scale")
        expectEqual(AudioMeterScale.majorTicksDbfs, [0, -6, -12, -24, -36, -48, -60], "ticks")
    }

    static func testMasterMeterReadoutLabels() {
        expectEqual(MasterMeterReadout.integratedLabel(-16.44), "-16.4 LUFS", "integrated")
        expectEqual(MasterMeterReadout.integratedLabel(-119.0), "—", "-119 is no evidence")
        expectEqual(MasterMeterReadout.integratedLabel(-118.9), "-118.9 LUFS", "just above the floor")
        expectEqual(MasterMeterReadout.truePeakLabel(-1.04), "-1.0 dBTP", "true peak")
        expectEqual(MasterMeterReadout.truePeakLabel(-120), "—", "true peak floor")
        expectEqual(MasterMeterReadout.targetLabel(-14), "target -14 LUFS", "target")
        expectEqual(MasterMeterReadout.ceilingLabel(-1.3), "ceiling -1.3 dBTP", "ceiling")
        expectEqual(MasterMeterReadout.guideFraction(value: -18, min: -36, max: 0), 0.5, "guide fraction")
        expectEqual(MasterMeterReadout.guideFraction(value: -36, min: -36, max: 0), 0, "at min: not primed")
        expectEqual(MasterMeterReadout.guideFraction(value: 3, min: -24, max: 0), 1, "clamped at max")
        expectEqual(MasterMeterReadout.railLufsLabel(loudnessLufs: -15.04, mixedFrames: 10), "-15.0 LUFS", "rail")
        expectEqual(MasterMeterReadout.railLufsLabel(loudnessLufs: -15, mixedFrames: 0), "LUFS —", "no mixed frame")
        expectEqual(MasterMeterReadout.railLufsLabel(loudnessLufs: -59, mixedFrames: 10), "LUFS —", "-59 floor")
        let none = MasterMeterReadout.gainReduction(0.05)
        expectEqual(none.label, "GR —", "0.05 dB is no reduction"); expectEqual(none.level, 0, "level 0")
        let six = MasterMeterReadout.gainReduction(6)
        expectEqual(six.label, "GR −6.0 dB", "6 dB label"); expectEqual(six.level, 25, "6 of 24 dB")
        expectEqual(MasterMeterReadout.gainReduction(30).level, 100, "clamped at 24 dB")
    }

    @MainActor
    static func testMasterMeterDecodeFromTheSnapshot() {
        let model = AppModel()
        model.applySnapshotForTesting(["audioMixSession": [
            "masterLevel": 40, "loudnessLufs": -15.0, "mixedFrameCount": 480,
            "masterMeter": ["momentaryLufs": -15.0, "shortTermLufs": -120.0,
                            "integratedLufs": -16.4, "truePeakDbfs": -2.5, "windowMs": 400],
        ] as JSONObject])
        expectEqual(model.integratedLufs, -16.4, "integrated decoded")
        expectEqual(model.momentaryLufs, -15.0, "momentary decoded")
        expectEqual(model.shortTermLufs, -120.0, "short-term stays its own field")
        expectEqual(model.truePeakDbfs, -2.5, "true peak")
        expectEqual(MasterMeterReadout.railLufsLabel(loudnessLufs: model.loudnessLufs,
                                                     mixedFrames: model.mixedFrameCount),
                    "-15.0 LUFS", "rail reads the core's loudnessLufs (short-term absent -> momentary)")
    }

    /// #481 / D6: the Zoom mute is display and meter only.
    @MainActor
    static func testZoomMutedIsDisplayOnly() {
        let echo: [JSONObject] = [["participantId": "101", "outputLevel": 0, "inputPeakDbfs": -12.0,
                                   "gainReductionDb": 4.5, "status": "live"]]
        let strips = AudioConsolePolicy.mergeStrips(echo: echo, prior: [], expected: ["101", "102"],
                                                    zoomMuted: ["101"])
        expect(strips[0].sourceMuted, "roster-muted guest reads sourceMuted")
        expect(!strips[0].muted, "the A1's mute is untouched")
        expectEqual(strips[0].gainReductionDb, 4.5, "GR decoded per channel")
        let reading = AudioConsolePolicy.meterReading(strips[0])
        expectEqual(reading.level, 80, "Zoom-muted meter shows the pre-mute input (-12 dBFS)")
        expect(reading.dimmed, "dimmed")
        expect(!strips[1].sourceMuted, "a guest not in the muted set is not")
        // Unmuted in Zoom on the next merge: never carried from prior.
        let next = AudioConsolePolicy.mergeStrips(echo: echo, prior: strips, expected: ["101", "102"], zoomMuted: [])
        expect(!next[0].sourceMuted, "unmuted in Zoom clears at once (never latched from prior)")
        let model = AppModel()
        model.roster = [RosterParticipant(id: "101", name: "A", hasVideo: true, muted: true, talking: false, assigned: true),
                        RosterParticipant(id: "102", name: "B", hasVideo: true, muted: false, talking: false, assigned: true)]
        expectEqual(model.zoomMutedIds, ["101"], "the roster is the only source")
        model.strips = strips
        let channels = model.audioMixCommand()["channels"] as? [JSONObject] ?? []
        expectEqual(channels.first?["muted"] as? Bool, false, "the wire mute is the A1's, not Zoom's")
    }
}
