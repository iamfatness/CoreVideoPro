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
            // Task 7: buses (ports of WT/RoutingMatrixViewModelTests bus cases).
            ("audio-buses/fixed-list-and-custom", testAudioBusListAndCustomBusesAreWindows),
            ("audio-buses/sends-and-listen", testAudioBusSendsAndListenAreWindows),
            ("audio-buses/wire-full-state", testAudioBusStateRidesTheBaseMatrix),
            ("audio-buses/bus-only-change-pushes", testABusOnlyChangeStillPushesTheMatrix),
            ("audio-buses/remove-routed-bus", testRemovingARoutedCustomBusUnroutesIt),
            ("control/route-set-custom-bus", testControlRouteSetAcceptsLiveBuses),
            // Review fixes.
            ("audio-meters/respawn-resets-master-meters", testARespawnedCoreNeverShowsTheDeadCoresMeters),
            ("audio-buses/bus-state-with-nothing-routed", testBusStateReachesTheCoreWithNothingRouted),
            ("audio-mastering/quit-flushes-the-rack", testQuitFlushesARackEditMadeJustBefore),
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

    // ── Task 7 ───────────────────────────────────────────────────────────────

    static func testAudioBusListAndCustomBusesAreWindows() {
        expectEqual(AudioBusPolicy.fixedBuses.map(\.id),
                    ["master", "pgm-l", "pgm-r", "iso-1", "iso-2", "iso-3", "iso-4", "iso-5", "iso-6", "iso-7",
                     "iso-8", "mon", "stream", "aux-1", "aux-2"], "the 15 fixed buses in Windows order")
        expectEqual(AudioBusPolicy.fixedBuses.map(\.label).prefix(4).map { $0 }, ["MASTER", "PGM L", "PGM R", "ISO 1"], "labels")
        expectEqual(AudioBusPolicy.fixedBuses.filter(\.isRemovable).map(\.id), ["aux-1", "aux-2"], "only aux is removable")
        var buses = AudioBusPolicy.fixedBuses
        var counter = 1
        var added = AudioBusPolicy.addCustom(buses, counter: counter)
        buses = added.buses; counter = added.counter
        expectEqual(added.added.id, "bus-01", "first custom bus"); expectEqual(added.added.label, "BUS 1", "label")
        buses = AudioBusPolicy.remove("bus-01", from: buses)!
        added = AudioBusPolicy.addCustom(buses, counter: counter)
        expectEqual(added.added.id, "bus-02", "a removed number is never reused")
        expect(AudioBusPolicy.remove("master", from: buses) == nil, "fixed buses cannot be removed")
        expect(AudioBusPolicy.rename("iso-1", to: "X", in: buses) == nil, "fixed buses cannot be renamed")
        expect(AudioBusPolicy.rename("aux-1", to: "   ", in: buses) == nil, "blank name refused")
        expectEqual(AudioBusPolicy.rename("aux-1", to: " Talkback ", in: buses)?.first { $0.id == "aux-1" }?.label,
                    "Talkback", "aux renamed, trimmed")
    }

    /// WT BusOutputs_BuildBusSendsReflectsTheCardToggles + BusListen_IsExclusive...
    static func testAudioBusSendsAndListenAreWindows() {
        var buses = AudioBusPolicy.fixedBuses
        buses = AudioBusPolicy.setSend("aux-1", target: "master", enabled: true, in: buses)!
        buses = AudioBusPolicy.setSend("aux-1", target: "mon", enabled: true, in: buses)!
        buses = AudioBusPolicy.setOutputGain("aux-1", gainDb: -6, in: buses)!
        let sends = AudioBusPolicy.busSends(buses)
        expectEqual(sends.count, 2, "two sends")
        expectEqual(sends.map { [$0["fromBusId"] as? String ?? "", $0["toBusId"] as? String ?? ""] },
                    [["aux-1", "master"], ["aux-1", "mon"]], "aux-1 -> master, mon")
        expectEqual(sends.map { $0["gainDb"] as? Double ?? 0 }, [-6, -6], "shared output gain")
        expect(AudioBusPolicy.setSend("master", target: "mon", enabled: true, in: buses) == nil, "fixed bus send refused")
        expect(AudioBusPolicy.setSend("aux-1", target: "iso-1", enabled: true, in: buses) == nil, "iso target refused")
        expectEqual(AudioBusPolicy.setOutputGain("aux-2", gainDb: 40, in: buses)?.first { $0.id == "aux-2" }?.outputGainDb,
                    10, "gain clamped to +10")
        buses = AudioBusPolicy.setSend("aux-1", target: "master", enabled: false, in: buses)!
        expectEqual(AudioBusPolicy.busSends(buses).count, 1, "untoggling master leaves one send")
        var listen: String? = nil
        listen = AudioBusPolicy.listen(current: listen, bus: "aux-1", on: true); expectEqual(listen, "aux-1", "aux-1")
        listen = AudioBusPolicy.listen(current: listen, bus: "aux-2", on: true); expectEqual(listen, "aux-2", "exclusive")
        listen = AudioBusPolicy.listen(current: listen, bus: "aux-1", on: false); expectEqual(listen, "aux-2", "off on another bus keeps it")
        listen = AudioBusPolicy.listen(current: listen, bus: "aux-2", on: false); expect(listen == nil, "off clears")
        expectEqual(AudioBusPolicy.listen(current: nil, bus: "iso-3", on: true), "iso-3", "a fixed bus can be listened")
    }

    @MainActor
    static func testAudioBusStateRidesTheBaseMatrix() {
        let model = AppModel()
        model.zoomAudioSourceIds = ["11"]
        expectEqual(model.routingBaseCommand()?["monitorBusId"] as? String, "", "no LISTEN is \"\"")
        expectEqual((model.routingBaseCommand()?["busSends"] as? [JSONObject])?.count, 0, "no bus sends")
        model.setBusSend("aux-2", target: "stream", enabled: true)
        model.setListening("aux-2", on: true)
        let command = model.routingBaseCommand() ?? [:]
        expectEqual(command["monitorBusId"] as? String, "aux-2", "LISTEN rides the base matrix")
        let sends = command["busSends"] as? [JSONObject] ?? []
        expectEqual(sends.first?["fromBusId"] as? String, "aux-2", "bus send rides the base matrix")
        expectEqual(sends.first?["toBusId"] as? String, "stream", "to stream")
        // A respawned core gets them back: the reassert sends the same base matrix.
        var reasserted: [JSONObject] = []
        model.audioReassertObserver = { reasserted = $0 }
        model.reassertAudioConsole()
        let matrix = reasserted.first { $0["type"] as? String == "sync-audio-routing-matrix" }
        expectEqual(matrix?["monitorBusId"] as? String, "aux-2", "LISTEN survives a core respawn (Windows, D3)")
    }

    /// The memo regression: a change to ONLY the bus state must still push.
    @MainActor
    static func testABusOnlyChangeStillPushesTheMatrix() {
        let model = AppModel()
        model.zoomAudioSourceIds = ["11"]
        var sent: [JSONObject] = []
        model.routingBaseSendObserver = { sent.append($0) }
        model.pushRoutingBase()
        spin(until: { sent.count >= 1 }, limit: 2)
        expectEqual(sent.count, 1, "first push")
        model.setListening("mon", on: true)
        spin(until: { sent.count >= 2 }, limit: 2)
        expectEqual(sent.count, 2, "a LISTEN-only change pushes")
        expectEqual((sent.last?["commands"] as? [JSONObject])?.first?["monitorBusId"] as? String, "mon", "with the bus")
        model.setBusSend("aux-1", target: "master", enabled: true)
        spin(until: { sent.count >= 3 }, limit: 2)
        expectEqual(sent.count, 3, "a bus-send-only change pushes")
        model.pushRoutingBase(); spin(until: { false }, limit: 0.3)
        expectEqual(sent.count, 3, "an unchanged matrix still sends nothing")
    }

    @MainActor
    static func testRemovingARoutedCustomBusUnroutesIt() {
        let model = AppModel()
        model.zoomAudioSourceIds = ["11"]
        let bus = model.addCustomBus()
        model.selectOrRouteSend(source: "11", bus: bus.id)
        model.setBusSend(bus.id, target: "master", enabled: true)
        model.setListening(bus.id, on: true)
        func sendsTo(_ id: String) -> Int {
            (model.routingBaseCommand()?["sends"] as? [JSONObject] ?? []).filter { $0["busId"] as? String == id }.count
        }
        expectEqual(sendsTo(bus.id), 1, "routed to the custom bus")
        expectEqual(model.routedCount(bus: bus.id), 1, "routed count")
        expect(model.removeBus(bus.id), "removed")
        expectEqual(sendsTo(bus.id), 0, "its crosspoints are unrouted")
        expectEqual((model.routingBaseCommand()?["busSends"] as? [JSONObject])?.count, 0, "its bus sends are gone")
        expectEqual(model.routingBaseCommand()?["monitorBusId"] as? String, "", "LISTEN on it is cleared")
        expect(!model.routingBusIds.contains(bus.id), "the column is gone")
        expect(!model.removeBus("master"), "a fixed bus is never removed")
    }

    @MainActor
    static func testControlRouteSetAcceptsLiveBuses() {
        let model = AppModel()
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        let refused = invoke(surface, "audio.route.set", ["11", "bus-01", true, 0.0, "epoch", 0, "op-1"])
        expect(refused?.ok == false && (refused?.error ?? "").hasPrefix("busId must be one of"), "bus-01 refused before it exists")
        expect((refused?.error ?? "").contains("iso-8") && (refused?.error ?? "").contains("aux-2"), "the list names all 15")
        model.addCustomBus()
        let after = invoke(surface, "audio.route.set", ["11", "bus-01", true, 0.0, "epoch", 0, "op-1"])
        expect(!(after?.error ?? "").hasPrefix("busId must be one of"), "bus-01 passes the bus check once added")
    }

    // ── Review fix 2: a respawned core never shows the dead core's numbers ───

    @MainActor
    static func testARespawnedCoreNeverShowsTheDeadCoresMeters() {
        let model = AppModel()
        model.applySnapshotForTesting(["audioMixSession": [
            "masterLevel": 60, "loudnessLufs": -14.0, "mixedFrameCount": 900, "limiterActive": true,
            "masteringRideDb": 2.0,
            "masterMeter": ["momentaryLufs": -13.0, "shortTermLufs": -14.0, "integratedLufs": -15.5, "truePeakDbfs": -0.8],
        ] as JSONObject])
        expectEqual(model.integratedLufs, -15.5, "precondition: the old core's integrated")
        model.onConnected()   // the respawned core's handshake
        expectEqual([model.integratedLufs, model.momentaryLufs, model.shortTermLufs, model.loudnessLufs, model.truePeakDbfs],
                    [-120, -120, -120, -120, -120], "every master loudness value is back to no-evidence")
        expectEqual(model.mixedFrameCount, 0, "no mixed frame yet")
        expectEqual([model.masterLevel, model.masterLevelLeft, model.masterLevelRight], [0, 0, 0], "levels zeroed")
        expect(!model.limiterActive, "limiter activity cleared")
        expectEqual(MasterMeterReadout.integratedLabel(model.integratedLufs), "—", "the guide meter reads a dash")
        expectEqual(MasterMeterReadout.railLufsLabel(loudnessLufs: model.loudnessLufs, mixedFrames: model.mixedFrameCount),
                    "LUFS —", "the rail reads a dash")
        // A new core's snapshot that has not published masterMeter yet keeps the dash.
        model.applySnapshotForTesting(["audioMixSession": ["masterLevel": 0] as JSONObject])
        expectEqual(model.integratedLufs, -120, "a missing masterMeter is no evidence, not the old value")
    }

    // ── Review fix 3: bus state reaches the core with nothing routed ─────────

    /// The core applies busSends and monitorBusId from a matrix with an empty
    /// `sends` when it holds no sends (MediaCore.cpp:3245-3393; Windows sends the
    /// matrix on every production sync). The Mac never sends an EMPTY matrix, but
    /// one that carries bus state is not empty.
    @MainActor
    static func testBusStateReachesTheCoreWithNothingRouted() {
        let model = AppModel()   // no sources, no edits
        expect(model.routingBaseCommand() == nil, "nothing routed and default bus state: still no matrix")
        model.setListening("iso-2", on: true)
        let listen = model.routingBaseCommand()
        expectEqual(listen?["monitorBusId"] as? String, "iso-2", "LISTEN with nothing routed is sent")
        expectEqual((listen?["sends"] as? [JSONObject])?.count, 0, "with an empty sends list")
        model.setListening("iso-2", on: false)
        model.setBusSend("aux-1", target: "mon", enabled: true)
        expectEqual((model.routingBaseCommand()?["busSends"] as? [JSONObject])?.count, 1, "a bus send with nothing routed is sent")
        var reasserted: [JSONObject] = []
        model.audioReassertObserver = { reasserted = $0 }
        model.reassertAudioConsole()
        expect(reasserted.contains { $0["type"] as? String == "sync-audio-routing-matrix" },
               "a respawned core gets the bus state with nothing routed")
        model.setBusSend("aux-1", target: "mon", enabled: false)
        // Back to default: one matrix carrying the cleared state must go out, so the
        // core does not keep LISTEN or a bus send the operator turned off.
        expectEqual((model.routingBaseCommand()?["busSends"] as? [JSONObject])?.count, 0,
                    "turning the last bus state off still sends the cleared state once")
    }

    // ── Review fix 7: a rack edit just before quit is saved ──────────────────

    /// editMastering is per slider tick, so it does not write the file itself
    /// (the 5 s autosave does). Quitting inside those 5 s used to lose the edit.
    @MainActor
    static func testQuitFlushesARackEditMadeJustBefore() {
        let model = AppModel()
        model.applyRestoredPrefs(ShellPrefs())
        model.editMastering { $0.glueAmount = 0.77; $0.limiterEnabled = false }
        expect(model.handleQuitRequest() == .terminateNow, "an idle quit quits")
        let saved = ShellPrefs.load()
        expectEqual(saved.masteringCurrent?.glueAmount, 0.77, "the rack edit made just before quit is on disk")
        expectEqual(saved.masteringCurrent?.limiterEnabled, false, "with the limiter state")
    }
}
