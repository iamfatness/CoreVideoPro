// macOS audio parity, Phase 4 (docs/superpowers/plans/2026-11-22-mac-audio-phase4.md):
// A7 processing workspace, A8 system audio, A9 guest lip sync. Its own file so
// parallel Mac branches extending ShellTests.swift do not collide.

import Foundation

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var audioPhase4Cases: [(String, () -> Void)] {
        [
            // Task 1: response-curve maths, pinned to vectors from the Windows file.
            ("a7-curve-eq-windows-vectors", testCurveEqMatchesWindowsVectors),
            ("a7-curve-comp-windows-vectors", testCurveCompMatchesWindowsVectors),
            ("a7-curve-gate-windows-vectors", testCurveGateMatchesWindowsVectors),
            ("a7-curve-point-counts", testCurvePointCounts),
            // Task 2: ordered channel insert chain, Limiter, slot model.
            ("a7-chain-order-is-toggle-order", testChainOrderIsToggleOrder),
            ("a7-chain-limiter-processes-with-ceiling", testChainLimiterProcessesWithCeiling),
            ("a7-chain-duplicate-refused", testChainDuplicateRefused),
            ("a7-chain-normalize-is-windows", testChainNormalizeIsWindows),
            ("a7-chain-vst-gated-on-host", testChainVstGatedOnHost),
            ("a7-chain-slots-labels", testChainSlotsLabels),
            ("a7-chain-reasserted-on-new-core", testChainReassertedOnNewCore),
            ("a7-chain-prefs-compat", testChainPrefsCompat),
            // Task 3: processing targets; bus and master chains on the wire.
            ("a7-master-chain-on-every-default-send", testMasterChainOnEveryDefaultSend),
            ("a7-chain-survives-doubling-guard", testChainSurvivesDoublingGuard),
            ("a7-legacy-route-path-keeps-bus-chains", testLegacyRoutePathKeepsBusChains),
            ("a7-bus-chain-change-pushes-matrix", testBusChainChangePushesMatrix),
            ("a7-removed-bus-drops-chain-and-target", testRemovedBusDropsChainAndTarget),
            ("a7-targets-order-and-strings", testTargetsOrderAndStrings),
            ("a7-select-strip-selects-channel-target", testSelectStripSelectsChannelTarget),
            ("a7-bus-eq-refused-with-reason", testBusEqRefusedWithReason),
            ("a7-bus-chain-reasserted-on-new-core", testBusChainReassertedOnNewCore),
            ("a7-primary-bus-label", testPrimaryBusLabel),
            // Task 1-2 review fix-ups.
            ("a7-fix-blank-add-is-noop", testFixBlankAddIsNoop),
            ("a7-fix-host-handled-names", testFixHostHandledNames),
            ("a7-fix-bus-slot-rule", testFixBusSlotRule),
            ("a7-fix-vst-repick-is-noop", testFixVstRepickIsNoop),
            ("a7-fix-empty-prefs-clears-vst", testFixEmptyPrefsClearsVst),
            ("a7-fix-bus-vst-gated-on-host", testFixBusVstGatedOnHost),
            // Task 4: SHOW layout (fixed viewport, Windows workspace, strip bank).
            ("a7-layout-cell-widths", testLayoutCellWidths),
            ("a7-pan-gain-tooltip-strings", testPanGainTooltipStrings),
            ("a7-param-specs-are-windows", testParamSpecsAreWindows),
            ("a7-show-not-in-page-scroll", testShowNotInPageScroll),
            // Task 4 review fix-ups.
            ("a7-fix4-popout-size", testFix4PopoutSize),
            ("a7-fix4-vst-menu-state", testFix4VstMenuState),
            ("a7-fix4-rack-keeps-target", testFix4RackKeepsTarget),
            ("a7-fix4-seed-strips-named", testFix4SeedStripsNamed),
            // Task 5: SETUP layout (two columns, Inserts & plug-ins, VST browser folded in).
            ("a7-setup-add-buttons", testSetupAddButtons),
            ("a7-surface-pills", testSurfacePills),
            ("a7-setup-target-picker-drives-list", testSetupTargetPickerDrivesList),
            ("a7-setup-unrouted-bus-note", testSetupUnroutedBusNote),
            // Task 5 review fix-ups.
            ("a7-fix5-bus-to-bus-feed-is-unrouted", testFix5BusToBusFeedIsUnrouted),
            ("a7-fix5-readout-strings", testFix5ReadoutStrings),
            ("a7-fix5-rack-stage-columns", testFix5RackStageColumns),
            ("a7-fix5-setup-vst-menu-state", testFix5SetupVstMenuState),
            ("a7-fix5-effective-strip-shared", testFix5EffectiveStripShared),
            // Review fix-ups 7: VST scan state, pop-out window constants.
            ("a7-fix7-scan-reply-applied", testFix7ScanReplyApplied),
            ("a7-fix7-unscanned-is-not-host-missing", testFix7UnscannedIsNotHostMissing),
            ("a7-fix7-failed-scan-retries", testFix7FailedScanRetries),
            ("a7-fix7-popout-window-uses-layout", testFix7PopoutWindowUsesLayout),
        ]
    }

    // ── Task 1: DspCurveMath ────────────────────────────────────────────────
    //
    // Fixture provenance: every (x, y) below was printed with ToString("R") by
    // native-shell/CoreVideoPro.WinUI/Services/DspCurveMath.cs, compiled
    // UNMODIFIED with ~/.dotnet/dotnet (net9.0 console project that
    // <Compile Include>s the Windows file) on 2026-11-22. Windows has no test for
    // this file, so these vectors are the oracle. Invariants: EQ has 64 points,
    // comp/gate 48; x == i/(n-1); tolerance 1e-9.

    private static let curveTolerance = 1e-9

    /// Eight band gains with one band set (band is 0-based; -1 = flat).
    private static func curveBands(_ band: Int, _ gainDb: Double) -> [Double] {
        var bands = [Double](repeating: 0, count: 8)
        if band >= 0 { bands[band] = gainDb }
        return bands
    }

    private static func expectCurve(_ points: [DspPoint], _ expected: [(Int, Double, Double)],
                                    _ what: String,
                                    _ file: StaticString = #file, _ line: UInt = #line) {
        for (index, x, y) in expected {
            guard index < points.count else {
                expect(false, "\(what)[\(index)] exists (count \(points.count))", file, line)
                continue
            }
            let point = points[index]
            expect(abs(point.x - x) <= curveTolerance && abs(point.y - y) <= curveTolerance,
                   "\(what)[\(index)] expected (\(x), \(y)), got (\(point.x), \(point.y))",
                   file, line)
        }
    }

    private static func testCurveEqMatchesWindowsVectors() {
        // Mutation: high-pass skipped / Q changed -> red here.
        expectCurve(DspCurveMath.eqResponse(highpassHz: 90, bandGainsDb: curveBands(-1, 0)), [
            (10, 0.15873015873015872, 0.4483948696574838),
            (20, 0.31746031746031744, 0.6592303079543049),
            (63, 1, 0.6666666666543891),
        ], "eq(90, flat)")
        // Mutation: peaking Q 1.0 instead of 1.4 -> red at [32].
        expectCurve(DspCurveMath.eqResponse(highpassHz: 20, bandGainsDb: curveBands(4, -6)), [
            (0, 0, 0.5830082106640145),
            (32, 0.5079365079365079, 0.5953287920957033),
            (40, 0.6349206349206349, 0.6086025552198154),
        ], "eq(20, band5 -6)")
        let hp400Band8Plus12: [(Int, Double, Double)] = [
            (0, 0, 0),
            (40, 0.6349206349206349, 0.6741139415929801),
            (52, 0.8253968253968254, 0.8401268106052969),
        ]
        expectCurve(DspCurveMath.eqResponse(highpassHz: 400, bandGainsDb: curveBands(7, 12)),
                    hp400Band8Plus12, "eq(400, band8 +12)")
        // Mutation: no high-pass clamp 20..400 -> red here. Windows prints the
        // same values for 1000 Hz as for 400 Hz because of the clamp (plan
        // deviation: the plan's eq(400) row sits inside the clamp and cannot
        // catch this mutation).
        expectCurve(DspCurveMath.eqResponse(highpassHz: 1000, bandGainsDb: curveBands(7, 12)),
                    hp400Band8Plus12, "eq(1000, band8 +12) == eq(400, band8 +12)")
        // Mutation: no band clamp ±12 -> red here (same reasoning, +20 clamps to +12).
        expectCurve(DspCurveMath.eqResponse(highpassHz: 400, bandGainsDb: curveBands(7, 20)),
                    hp400Band8Plus12, "eq(400, band8 +20) == eq(400, band8 +12)")
        // Mutation: drop the |gain| < 0.05 skip rule -> red here. 0.04 dB is
        // skipped, so this equals a flat EQ at 20 Hz.
        expectCurve(DspCurveMath.eqResponse(highpassHz: 20, bandGainsDb: curveBands(0, 0.04)), [
            (10, 0.15873015873015872, 0.6651732863528543),
        ], "eq(20, band1 0.04)")
    }

    private static func testCurveCompMatchesWindowsVectors() {
        expectCurve(DspCurveMath.compressorTransfer(thresholdDb: -18, ratio: 4, kneeDb: 0), [
            (33, 0.7021276595744681, 0.700531914893617),
            (40, 0.851063829787234, 0.7377659574468086),
            (47, 1, 0.775),
        ], "comp(-18, 4, knee 0)")
        // Mutation: hard knee only -> red at [33].
        expectCurve(DspCurveMath.compressorTransfer(thresholdDb: -18, ratio: 4, kneeDb: 6), [
            (32, 0.6808510638297872, 0.6772818583069262),
            (33, 0.7021276595744681, 0.6919378112267994),
            (34, 0.723404255319149, 0.7031985626980535),
        ], "comp(-18, 4, knee 6)")
        // Mutation: no ratio floor at 1 -> red at [36] and [40]. Plan deviation:
        // the plan's [47] row alone stays green under that mutation, because
        // ratio 0.5 makes the output +18 dB there and the [-60..0] output clamp
        // maps it to 1 anyway. Above threshold but below 0 dB the floored ratio
        // is unity (y == x); the unfloored one is not.
        expectCurve(DspCurveMath.compressorTransfer(thresholdDb: -18, ratio: 0.5), [
            (36, 0.7659574468085106, 0.7659574468085106),
            (40, 0.851063829787234, 0.851063829787234),
            (47, 1, 1),
        ], "comp(-18, 0.5)")
    }

    private static func testCurveGateMatchesWindowsVectors() {
        // [24] (input -39.15 dB) is the first unity point above -40.
        expectCurve(DspCurveMath.gateTransfer(thresholdDb: -40, rangeDb: -20), [
            (20, 0.425531914893617, 0.17553191489361702),
            (23, 0.48936170212765956, 0.23936170212765956),
            (24, 0.5106382978723404, 0.5106382978723404),
        ], "gate(-40, -20)")
        expectCurve(DspCurveMath.gateTransfer(thresholdDb: -12, rangeDb: -6), [
            (30, 0.6382978723404256, 0.5632978723404255),
            (41, 0.8723404255319149, 0.8723404255319149),
        ], "gate(-12, -6)")
        // Mutation: `<=` instead of `<` at the threshold -> red at [47]. The
        // input grid is -80 + i*80/47, so -40 never lands on a point and the
        // plan's gate(-40)[24] row cannot see this mutation (plan deviation).
        // 0 dB is exactly input [47]: unity under `<`, range applied under `<=`.
        expectCurve(DspCurveMath.gateTransfer(thresholdDb: 0, rangeDb: -20), [
            (46, 0.9787234042553191, 0.7287234042553191),
            (47, 1, 1),
        ], "gate(0, -20)")
    }

    private static func testCurvePointCounts() {
        let eq = DspCurveMath.eqResponse(highpassHz: 90, bandGainsDb: curveBands(-1, 0))
        let comp = DspCurveMath.compressorTransfer(thresholdDb: -18, ratio: 4)
        let gate = DspCurveMath.gateTransfer(thresholdDb: -40)
        expectEqual(eq.count, 64, "EQ default point count")
        expectEqual(comp.count, 48, "comp default point count")
        expectEqual(gate.count, 48, "gate default point count")
        expectEqual(eq.last?.x, 1, "EQ last x")
        expectEqual(comp.last?.x, 1, "comp last x")
        expectEqual(gate.last?.x, 1, "gate last x")
        expectEqual(eq.first?.x, 0, "EQ first x")
        // Every point sits on the grid x == i/(n-1), first point included, for
        // all three curves (and a non-default count). Mutation: start comp's or
        // gate's grid at i+1, or divide by n instead of n-1 -> red here.
        let grids: [(String, [DspPoint])] = [
            ("eq", eq), ("comp", comp), ("gate", gate),
            ("comp(knee 6)", DspCurveMath.compressorTransfer(thresholdDb: -18, ratio: 4, kneeDb: 6)),
            ("gate(-40, -20)", DspCurveMath.gateTransfer(thresholdDb: -40, rangeDb: -20)),
            ("eq(n=9)", DspCurveMath.eqResponse(highpassHz: 90, bandGainsDb: curveBands(3, 6), pointCount: 9)),
        ]
        for (what, points) in grids {
            var offGrid: [Int] = []
            for (index, point) in points.enumerated()
            where abs(point.x - Double(index) / Double(points.count - 1)) > curveTolerance {
                offGrid.append(index)
            }
            expect(offGrid.isEmpty, "\(what): every x == i/(n-1); off-grid indices \(offGrid)")
            expectEqual(points.first?.x, 0, "\(what) first x")
        }
        expectEqual(DspCurveMath.eqBandHz, [63, 125, 250, 500, 1000, 2000, 4000, 8000],
                    "EQ band centres (kEqBandHz)")
    }

    // ── Task 2: ordered channel insert chain ────────────────────────────────

    /// A model with one Zoom guest strip "11" (plus the zoom-mix strip).
    @MainActor
    private static func chainModel() -> AppModel {
        let model = AppModel()
        model.zoomAudioSourceIds = ["11"]
        model.meetingAudioSubscribed = true
        model.refreshStrips()
        return model
    }

    private static func wireChannel(_ command: JSONObject?, _ id: String) -> JSONObject? {
        (command?["channels"] as? [JSONObject])?.first { $0["participantId"] as? String == id }
    }

    @MainActor
    private static func testChainOrderIsToggleOrder() {
        let model = chainModel()
        model.editInserts("11") { $0.toggle(ChannelInserts.eqName) }
        model.editInserts("11") { $0.toggle(ChannelInserts.gateName) }
        model.editInserts("11") { $0.toggle(ChannelInserts.compName) }
        // Mutation: today's fixed gate->EQ->comp `activeNames` order -> red.
        expectEqual(wireChannel(model.audioMixCommand(), "11")?["pluginInserts"] as? [String],
                    ["Built-in EQ", "Noise Gate", "Compressor"],
                    "the wire carries the chain in the order the operator toggled it (Windows PluginInserts)")
        let flags = model.channelInserts["11"] ?? ChannelInserts()
        expect(flags.gateEnabled && flags.eqEnabled && flags.compEnabled, "the get-only flags follow the chain")
        // Toggling again removes it (ToggleSelectedRackInsert); re-adding appends at the end.
        model.editInserts("11") { $0.toggle("noise gate") }
        expectEqual(model.channelInserts["11"]?.chain, ["Built-in EQ", "Compressor"],
                    "toggle removes case-insensitively")
        expect(model.channelInserts["11"]?.gateEnabled == false, "gate flag off once removed")
        model.editInserts("11") { $0.toggle(ChannelInserts.gateName) }
        expectEqual(wireChannel(model.audioMixCommand(), "11")?["pluginInserts"] as? [String],
                    ["Built-in EQ", "Compressor", "Noise Gate"], "re-added gate goes last")
        let settings = wireChannel(model.audioMixCommand(), "11")?["insertSettings"] as? JSONObject ?? [:]
        expectEqual(Set(settings.keys), Set(["Built-in EQ", "Compressor", "Noise Gate"]),
                    "settings for every built-in on the chain, keyed by its chain name")
    }

    @MainActor
    private static func testChainLimiterProcessesWithCeiling() {
        let model = chainModel()
        expectEqual(ChannelInserts.limiterName, "Limiter", "core matches \"limiter\" (AudioDsp.h)")
        expectEqual(ChannelInserts.builtInNames, ["Noise Gate", "Built-in EQ", "Compressor", "Limiter"],
                    "Windows BuiltInInsertNames (SVM:2250)")
        expectEqual(ChannelInserts().limiterCeilingDb, -1.0, "core default ceiling")
        model.editInserts("11") {
            $0.limiterCeilingDb = -3
            _ = $0.add("Limiter")
        }
        let channel = wireChannel(model.audioMixCommand(), "11")
        expectEqual(channel?["pluginInserts"] as? [String], ["Limiter"], "the Limiter name is on the wire")
        let limiter = (channel?["insertSettings"] as? JSONObject)?["Limiter"] as? JSONObject
        // Mutation: omit the limiter from settingsJson -> red.
        expectEqual(limiter?["ceilingDb"] as? Double, -3, "insertSettings[Limiter][ceilingDb]")
        model.editInserts("11") { $0.limiterCeilingDb = -20 }
        let clamped = ((wireChannel(model.audioMixCommand(), "11")?["insertSettings"] as? JSONObject)?["Limiter"]
                       as? JSONObject)?["ceilingDb"] as? Double
        expectEqual(clamped, -12, "ceiling clamped to the core's -12...-0.1")
        model.editInserts("11") { $0.limiterCeilingDb = 0 }
        let top = ((wireChannel(model.audioMixCommand(), "11")?["insertSettings"] as? JSONObject)?["Limiter"]
                   as? JSONObject)?["ceilingDb"] as? Double
        expectEqual(top, -0.1, "ceiling clamped at -0.1")
    }

    private static func testChainDuplicateRefused() {
        var inserts = ChannelInserts()
        expectEqual(inserts.add("eq"), .added("Built-in EQ"), "first add normalises")
        expectEqual(inserts.add("eq"), .duplicate("Built-in EQ"), "second add is refused (SVM AddSelectedAudioInsert)")
        expectEqual(inserts.add("built-in eq"), .duplicate("Built-in EQ"), "normalised spelling is a duplicate")
        expectEqual(inserts.chain.count, 1, "the chain holds one EQ")
        // Mutation: case-sensitive compare -> red here. ("built-in eq" normalises to
        // the canonical spelling, so only a name normalize() passes through can see it.)
        expectEqual(inserts.add("Noise Gate"), .added("Noise Gate"), "gate added")
        expectEqual(inserts.add("noise gate"), .duplicate("noise gate"), "duplicates compare case-insensitively")
        expectEqual(inserts.chain, ["Built-in EQ", "Noise Gate"], "no duplicate entered the chain")
        expect(inserts.remove("NOISE GATE"), "remove is case-insensitive")
        expect(!inserts.remove("Noise Gate"), "removing an absent name reports false")
        inserts.clear()
        expectEqual(inserts.chain, [], "clear empties the chain")
    }

    private static func testChainNormalizeIsWindows() {
        // SVM NormalizeAudioInsertName.
        let table: [(String, String)] = [
            ("eq", "Built-in EQ"), ("Built-In EQ", "Built-in EQ"), ("built-in eq", "Built-in EQ"),
            ("compressor", "Compressor"), ("COMPRESSOR", "Compressor"),
            ("vst", "VST3 Bridge Slot"), ("vst3", "VST3 Bridge Slot"), ("vst3 bridge", "VST3 Bridge Slot"),
            ("  Reverb ", "Reverb"), ("Noise Gate", "Noise Gate"), ("limiter", "limiter"),
        ]
        for (raw, expected) in table {
            expectEqual(InsertChainPolicy.normalize(raw), expected, "normalize(\"\(raw)\")")
        }
        // SVM IsBuiltInInsert / IsVstInsert.
        expect(InsertChainPolicy.isBuiltIn("Noise Gate") && InsertChainPolicy.isBuiltIn("limiter"), "built-ins")
        expect(!InsertChainPolicy.isBuiltIn("vst:Pro-EQ"), "a vst: name is never built-in")
        expect(!InsertChainPolicy.isBuiltIn("Reverb"), "an unknown name is not built-in")
        expect(InsertChainPolicy.isVst("VST:Pro-Q 3") && InsertChainPolicy.isVst("VST3 Bridge Slot"), "VST names")
        expect(!InsertChainPolicy.isVst("Compressor"), "a built-in is not a VST")
    }

    @MainActor
    private static func testChainVstGatedOnHost() {
        let model = chainModel()
        model.editInserts("11") { $0.chain = ["Compressor", "vst:Pro-Q 3"] }
        expect(!model.vstInsertsLive, "precondition: no live host")
        // Mutation: send the VST unconditionally -> red.
        expectEqual(model.pluginInsertNames(for: "11"), ["Compressor"], "a dead host gets no VST insert")
        expectEqual(wireChannel(model.audioMixCommand(), "11")?["pluginInserts"] as? [String], ["Compressor"],
                    "and neither does the wire")
        let slots = InsertChainPolicy.slots(chain: model.channelInserts["11"]?.chain ?? [], onBus: false,
                                            vstLive: model.vstInsertsLive,
                                            vstStatusLine: model.vstStatusLine, labelStyle: .channel)
        expectEqual(slots.map(\.statusLabel), ["LIVE", "BYPASS"], "the VST slot is shown as bypassed")
        expectEqual(slots.last?.tooltip, model.vstStatusLine, "with the host status line as the reason")
        expectEqual(slots.last?.isBuiltIn, false, "a VST is not built-in")
        model.vstHost.serveRunning = true
        expectEqual(model.pluginInsertNames(for: "11"), ["Compressor", "vst:Pro-Q 3"],
                    "a live host gets the VST in chain position")
    }

    private static func testChainSlotsLabels() {
        let chain = ["Built-in EQ", "Compressor"]
        let channel = InsertChainPolicy.slots(chain: chain, onBus: false, vstLive: false,
                                              vstStatusLine: "x", labelStyle: .channel)
        expectEqual(channel.map(\.slotLabel), ["1", "2"], "channel labels (SVM RefreshSelectedChannelInsertSlots)")
        expectEqual(channel.map(\.statusLabel), ["LIVE", "LIVE"], "built-ins process on a channel")
        expectEqual(channel.first?.tooltip, "Processing active.", "Windows built-in tooltip")
        expectEqual(channel.map(\.id), chain, "slot identity is the chain name")
        let target = InsertChainPolicy.slots(chain: chain, onBus: true, vstLive: false,
                                             vstStatusLine: "x", labelStyle: .target)
        expectEqual(target.map(\.slotLabel), ["01", "02"], "target labels (SVM RefreshSelectedAudioProcessingSlots)")
        // Mutation: LIVE for every built-in -> red here (OD-1).
        expectEqual(target.map(\.statusLabel), ["PASS", "LIVE"], "bus EQ passes through, bus compressor processes")
        expectEqual(target.first?.tooltip,
                    "Built-in EQ does not process on a bus; audio passes through unchanged.", "the PASS reason")
        let busGate = InsertChainPolicy.slots(chain: ["Noise Gate", "Limiter"], onBus: true, vstLive: false,
                                              vstStatusLine: "x", labelStyle: .target)
        expectEqual(busGate.map(\.statusLabel), ["PASS", "LIVE"], "bus gate passes, bus limiter processes")
        expectEqual(busGate.first?.tooltip,
                    "Built-in Noise Gate does not process on a bus; audio passes through unchanged.", "gate reason")
    }

    /// CONFIRMATORY (plan-authoring rule 8): onConnected() already re-sent the
    /// mix command before Task 2, so this case pins the new ordered chain on
    /// that existing path rather than driving a behaviour change. No mutation
    /// of Task 2's code is expected to turn it red except breaking the chain
    /// order on the wire, which a7-chain-order-is-toggle-order already owns.
    @MainActor
    private static func testChainReassertedOnNewCore() {
        let model = chainModel()
        model.editInserts("11") { $0.toggle(ChannelInserts.compName) }
        model.editInserts("11") { $0.toggle(ChannelInserts.limiterName) }
        model.editInserts("11") { $0.toggle(ChannelInserts.eqName) }
        model.markAudioConsoleDeliveredForTesting()
        var batches: [[JSONObject]] = []
        model.audioReassertObserver = { batches.append($0) }
        model.onConnected()
        model.stopTimersForTesting()
        let first = batches.first?.first
        expectEqual(first?["type"] as? String, "sync-participant-audio-mix", "the mix leads the reassert batch")
        expectEqual(wireChannel(first, "11")?["pluginInserts"] as? [String],
                    ["Compressor", "Limiter", "Built-in EQ"], "a respawned core gets the chain in order")
    }

    @MainActor
    private static func testChainPrefsCompat() {
        var prefs = ShellPrefs()
        prefs.vstChannelSelections = ["101": "vst:Test"]
        let model = AppModel()
        model.applyRestoredPrefs(prefs)
        expectEqual(model.channelInserts["101"]?.chain, ["vst:Test"], "a saved VST selection restores into the chain")
        expectEqual(model.currentPrefs().vstChannelSelections, ["101": "vst:Test"], "and saves back")
        model.editInserts("101") { _ = $0.add("Compressor") }
        expectEqual(model.channelInserts["101"]?.chain, ["vst:Test", "Compressor"], "built-ins append after it")
        expectEqual(model.currentPrefs().vstChannelSelections, ["101": "vst:Test"],
                    "only the VST is saved; built-in chains are not persisted (Windows)")
        model.setVstChannelSelection("101", "vst:Other")
        expectEqual(model.channelInserts["101"]?.chain, ["Compressor", "vst:Other"],
                    "picking another plug-in replaces the VST in the chain")
        expectEqual(model.currentPrefs().vstChannelSelections, ["101": "vst:Other"], "and is saved")
        model.setVstChannelSelection("101", nil)
        expectEqual(model.channelInserts["101"]?.chain, ["Compressor"], "None removes the VST")
        expect(model.currentPrefs().vstChannelSelections == nil, "nothing to save without a VST")
    }

    // ── Task 3: processing targets, bus and master chains ───────────────────
    //
    // Fixture invariant: Zoom guest strips "101" and "102" (per-guest ISO, no
    // meeting mix), so the default sends are exactly
    // AudioConsolePolicy.defaultSendBuses for each strip (2 x 5 = 10 sends, 2
    // of them to master). No operator edits unless a test states one.

    @MainActor
    private static func targetModel() -> AppModel {
        let model = AppModel()
        model.zoomAudioSourceIds = ["101", "102"]
        model.refreshStrips()
        return model
    }

    private static func sends(_ command: JSONObject?) -> [JSONObject] {
        command?["sends"] as? [JSONObject] ?? []
    }

    private static func chainOf(_ send: JSONObject) -> [String]? { send["busPluginInserts"] as? [String] }

    @MainActor
    private static func testMasterChainOnEveryDefaultSend() {
        let model = targetModel()
        expectEqual(model.strips.map(\.id), ["101", "102"], "fixture: two strips")
        model.busInserts = ["master": ["Compressor", "Limiter"]]
        let all = sends(model.routingBaseCommand())
        expectEqual(all.count, 2 * AudioConsolePolicy.defaultSendBuses.count, "fixture: only the default sends")
        let master = all.filter { $0["busId"] as? String == "master" }
        expectEqual(master.count, 2, "one default master send per strip")
        // Mutations: attach the chain only to operator edits, or before the
        // defaults are synthesised -> red here (the defaults lack it).
        for send in master {
            expectEqual(chainOf(send), ["Compressor", "Limiter"],
                        "default master send \(send["sourceId"] ?? "?") carries the master chain (SVM:9222-9230)")
        }
        for send in all where send["busId"] as? String != "master" {
            expectEqual(chainOf(send), [], "a \(send["busId"] ?? "?") send carries that bus's (empty) chain")
        }
        // An operator edit on a default master send keeps the chain too.
        let edited = AudioConsolePolicy.baseRoutingSends(
            zoomAudioIds: ["101", "102"], captureAudioIds: [], mediaRouted: false,
            operatorEdits: [AudioRouteKey(source: "101", bus: "master"): .init(enabled: true, gainDb: -6)],
            busInserts: ["master": ["Compressor"]])
        let editedSend = edited.first { $0["sourceId"] as? String == "101" && $0["busId"] as? String == "master" }
        expectEqual(editedSend?["gainDb"] as? Double, -6, "precondition: the edit applied")
        expectEqual(editedSend.flatMap(chainOf), ["Compressor"], "an edited master send carries the chain")
    }

    private static func testChainSurvivesDoublingGuard() {
        // ProgramMix: zoom-mix gets the defaults; the master chain rides them.
        let programMix = AudioConsolePolicy.baseRoutingSends(
            zoomAudioIds: ["101", "102"], captureAudioIds: [], mediaRouted: false, operatorEdits: [:],
            zoomAudioMode: .programMix, meetingAudio: true, busInserts: ["master": ["Compressor", "Limiter"]])
        let mixToMaster = programMix.first {
            $0["sourceId"] as? String == AudioConsolePolicy.meetingMixId && $0["busId"] as? String == "master"
        }
        expect(mixToMaster != nil, "precondition: zoom-mix -> master is a ProgramMix default")
        expectEqual(mixToMaster.flatMap(chainOf), ["Compressor", "Limiter"], "the zoom-mix -> master send carries the chain")
        expect(!programMix.contains { $0["sourceId"] as? String == "101" }, "precondition: stems get no defaults")
        // PerGuestIso: the guard drops zoom-mix on the default buses; an operator's
        // zoom-mix -> aux-1 survives it and carries the aux-1 chain, and nothing
        // the guard removed comes back with a chain attached.
        let iso = AudioConsolePolicy.baseRoutingSends(
            zoomAudioIds: ["101"], captureAudioIds: [], mediaRouted: false,
            operatorEdits: [AudioRouteKey(source: AudioConsolePolicy.meetingMixId, bus: "aux-1"): .init(enabled: true, gainDb: 0),
                            AudioRouteKey(source: AudioConsolePolicy.meetingMixId, bus: "master"): .init(enabled: true, gainDb: 0)],
            zoomAudioMode: .perGuestIso, meetingAudio: true,
            busInserts: ["master": ["Compressor"], "aux-1": ["Limiter"]])
        let mixSends = iso.filter { $0["sourceId"] as? String == AudioConsolePolicy.meetingMixId }
        expectEqual(mixSends.map { $0["busId"] as? String ?? "" }, ["aux-1"], "the guard removed zoom-mix -> master")
        expectEqual(mixSends.first.flatMap(chainOf), ["Limiter"], "the surviving send carries its bus chain")
    }

    @MainActor
    private static func testLegacyRoutePathKeepsBusChains() {
        let model = targetModel()
        model.busInserts = ["master": ["Compressor", "Limiter"]]
        // The core's echo hydrated the grid (applyRoutingSnapshot): both strips on
        // master and pgm-l. A core with no authority epoch takes the legacy tail.
        model.audioSends = ["101": ["master": 0, "pgm-l": 0], "102": ["master": -3]]
        var requests: [JSONObject] = []
        model.audioRoutingRequestForTesting = { body in
            requests.append(body)
            return ["snapshot": ["audioRoutingMatrix": ["sends": [JSONObject]()]] as JSONObject]
        }
        model.syncAudioRouting()
        func matrix() -> JSONObject? {
            requests.lazy.compactMap { ($0["commands"] as? [JSONObject])?.first }
                .first { $0["type"] as? String == "sync-audio-routing-matrix" }
        }
        spin(until: { matrix() != nil }, limit: 3)
        let legacy = sends(matrix())
        expect(!legacy.isEmpty, "the legacy tail sent a matrix")
        let master = legacy.filter { $0["busId"] as? String == "master" }
        expectEqual(master.count, 2, "both master sends")
        // Mutation: leave `[]` in the pushAudioRouting tail -> red.
        for send in master {
            expectEqual(chainOf(send), ["Compressor", "Limiter"], "legacy master send carries the master chain")
        }
        let pgm = legacy.first { $0["busId"] as? String == "pgm-l" }
        expectEqual(pgm.flatMap(chainOf), [], "a bus without a chain sends []")
    }

    @MainActor
    private static func testBusChainChangePushesMatrix() {
        let model = targetModel()
        var sent: [JSONObject] = []
        model.routingBaseSendObserver = { sent.append($0) }
        // Stated operator edit (plan deviation): aux-1 is not a default bus, so a
        // chain on it reaches the wire only through a send to it. Route 101 there
        // and let that push settle before the chain change.
        model.selectOrRouteSend(source: "101", bus: "aux-1")
        spin(until: { !sent.isEmpty }, limit: 2)
        spin(until: { false }, limit: 0.3)
        let before = sent.count
        expect(before >= 1, "precondition: the route pushed")
        model.selectProcessingTarget("bus:aux-1")
        model.addProcessingInsert("Limiter")
        spin(until: { sent.count > before }, limit: 2)
        spin(until: { false }, limit: 0.3)
        // Mutation: strip busPluginInserts from the memo copy -> red (memo equal, nothing sent).
        expectEqual(sent.count, before + 1, "a bus-chain-only change pushes exactly one new matrix")
        let aux = sends((sent.last?["commands"] as? [JSONObject])?.first)
            .first { $0["sourceId"] as? String == "101" && $0["busId"] as? String == "aux-1" }
        expectEqual(aux.flatMap(chainOf), ["Limiter"], "with the aux-1 chain on its send")
    }

    @MainActor
    private static func testRemovedBusDropsChainAndTarget() {
        let model = targetModel()
        let bus = model.addCustomBus()
        expectEqual(bus.id, "bus-01", "fixture: first custom bus")
        model.selectOrRouteSend(source: "101", bus: "bus-01")
        model.selectProcessingTarget("bus:bus-01")
        expectEqual(model.selectedProcessingTargetId, "bus:bus-01", "precondition: the bus is the target")
        model.addProcessingInsert("compressor")
        expectEqual(model.busInserts["bus-01"], ["Compressor"], "precondition: the chain is on the bus")
        expectEqual(sends(model.routingBaseCommand()).first { $0["busId"] as? String == "bus-01" }.flatMap(chainOf),
                    ["Compressor"], "precondition: the chain rides the bus-01 send")
        expect(model.removeBus("bus-01"), "the bus is removed")
        // Mutations: keep the chain / keep the target -> red.
        expectEqual(model.selectedProcessingTargetId, "bus:master", "the target falls back to bus:master")
        expect(model.busInserts["bus-01"] == nil, "the bus chain is dropped")
        expect(!sends(model.routingBaseCommand()).contains { $0["busId"] as? String == "bus-01" },
               "no send in the next matrix names bus-01")
        expect(!model.processingTargets.contains { $0.id == "bus:bus-01" }, "and it is no longer a target")
    }

    @MainActor
    private static func testTargetsOrderAndStrings() {
        let model = targetModel()
        // Bus order is AudioBusPolicy.fixedBuses (Windows BusHeaders), master
        // hoisted first (SVM:11544). Plan deviation: the plan's elided list reads
        // "iso-1,…,aux-2"; the real order puts mon and stream between iso-8 and aux-1.
        let expected = ["channel:101", "channel:102", "bus:master", "bus:pgm-l", "bus:pgm-r"]
            + (1...8).map { "bus:iso-\($0)" } + ["bus:mon", "bus:stream", "bus:aux-1", "bus:aux-2"]
        // Mutation: master after the other buses -> red.
        expectEqual(model.processingTargets.map(\.id), expected, "Windows RefreshAudioProcessingTargets order")
        func target(_ id: String) -> AudioProcessingTarget? { model.processingTargets.first { $0.id == id } }
        expectEqual(target("bus:aux-1")?.kind, "Aux bus", "aux kind (SVM:11614-11637)")
        expectEqual(target("bus:aux-1")?.detail, "Aux send processing.", "aux detail (SVM:11639-11649)")
        expectEqual(target("bus:master")?.kind, "Master bus", "master kind")
        expectEqual(target("bus:master")?.detail, "Final program mix processing before output.", "master detail")
        expectEqual(target("bus:master")?.label, "MASTER", "a bus target's label is its header label")
        expectEqual(target("bus:iso-3")?.kind, "ISO bus", "iso kind")
        expectEqual(target("bus:iso-3")?.detail, "Isolated recording bus processing.", "iso detail")
        expectEqual(target("bus:mon")?.detail, "Control-room monitor bus processing.", "mon detail")
        expectEqual(target("bus:stream")?.detail, "Streaming output bus processing.", "stream detail")
        expectEqual(target("bus:pgm-l")?.detail, "Left program bus processing.", "pgm-l detail")
        expectEqual(target("bus:pgm-r")?.kind, "Bus", "pgm-r kind")
        expectEqual(target("channel:101")?.kind, "Channel", "channel kind")
        expect(model.audioSends.isEmpty, "precondition: nothing routed yet")
        expectEqual(target("channel:101")?.detail, "Channel - MASTER",
                    "with no routed bus the detail falls back to MASTER (SVM:11660)")
        // A real routed state: the primary bus is the first ROUTED bus in header
        // order (SVM:11651-11661). Mutation: always return "MASTER", or take the
        // first routed bus in dictionary order instead of header order -> red.
        model.audioSends = ["101": ["aux-1": 0], "102": ["aux-1": 0, "iso-3": -6]]
        expectEqual(target("channel:101")?.detail, "Channel - AUX 1", "channel routed only to aux-1")
        expectEqual(target("channel:102")?.detail, "Channel - ISO 3",
                    "iso-3 precedes aux-1 in header order, so it is the primary bus")
        model.audioSends = [:]
        expectEqual(target("bus:master")?.insertLabel, "No inserts", "an empty chain")
        model.busInserts["master"] = ["Compressor", "Limiter"]
        expectEqual(target("bus:master")?.insertLabel, "Compressor + Limiter", "the chain joined with \" + \"")
        model.editInserts("101") { _ = $0.add("eq") }
        expectEqual(target("channel:101")?.insertLabel, "Built-in EQ", "a channel's own chain")
        model.addCustomBus()
        expectEqual(target("bus:bus-01")?.kind, "Custom bus", "custom kind")
        expectEqual(target("bus:bus-01")?.detail, "Custom bus processing.", "custom detail")
        expectEqual(model.processingTargets.last?.id, "bus:bus-01", "a custom bus follows the fixed buses")
        expectEqual(AudioProcessingTargetPolicy.defaultTargetId, "bus:master", "the default target (SVM:508)")
        expectEqual(model.selectedProcessingTargetId, "bus:master", "a fresh model selects master")
    }

    @MainActor
    private static func testSelectStripSelectsChannelTarget() {
        let model = targetModel()
        model.roster = [RosterParticipant(id: "102", name: "Ben", hasVideo: false, muted: false,
                                          talking: false, assigned: false)]
        model.selectAudioStrip("102")
        expectEqual(model.selectedAudioStripId, "102", "the strip is selected")
        expectEqual(model.selectedProcessingTargetId, "channel:102", "and so is its channel target (SVM:4688-4693)")
        model.addProcessingInsert("compressor")
        expectEqual(model.channelInserts["102"]?.chain, ["Compressor"], "the insert lands on strip 102's chain")
        expect(model.channelInserts["101"]?.chain.isEmpty ?? true, "and not on 101's")
        expectEqual(model.takeStatus, "Compressor added to Ben", "SVM AddAudioProcessingInsert status")
        model.addProcessingInsert("Compressor")
        expectEqual(model.takeStatus, "Compressor is already on Ben", "duplicate status")
        expectEqual(model.channelInserts["102"]?.chain, ["Compressor"], "no duplicate")
        model.removeProcessingInsert("compressor")
        expectEqual(model.takeStatus, "Removed compressor from Ben", "SVM RemoveAudioProcessingInsert status")
        model.addProcessingInsert("Limiter")
        model.clearProcessingInserts()
        expectEqual(model.takeStatus, "Insert chain cleared for Ben", "SVM ClearAudioProcessingInserts status")
        expectEqual(model.channelInserts["102"]?.chain, [], "cleared")
        // A strip that leaves the console while selected falls back to master.
        model.zoomAudioSourceIds = ["101"]
        model.refreshStrips()
        expectEqual(model.selectedProcessingTargetId, "bus:master", "a vanished strip's target falls back to bus:master")
        expect(model.selectedAudioStripId == nil, "and its selection clears")
    }

    @MainActor
    private static func testBusEqRefusedWithReason() {
        let model = targetModel()
        expectEqual(model.selectedProcessingTargetId, "bus:master", "precondition: master is the target")
        model.addProcessingInsert("Compressor")
        expectEqual(model.takeStatus, "Compressor added to MASTER", "a compressor processes on a bus")
        // Mutation: allow EQ on a bus target -> red.
        model.addProcessingInsert("eq")
        expectEqual(model.busInserts["master"], ["Compressor"], "the bus chain is unchanged")
        expectEqual(model.takeStatus, "Built-in EQ does not process on a bus yet; add a compressor or limiter.",
                    "the OD-1 reason")
        expectEqual(AudioProcessingTargetPolicy.busRefusal("eq"),
                    "Built-in EQ does not process on a bus yet; add a compressor or limiter.",
                    "the same sentence for the SETUP button (Task 5)")
        expect(AudioProcessingTargetPolicy.busRefusal("Limiter") == nil, "a limiter is allowed")
        expect(AudioProcessingTargetPolicy.busRefusal("vst:Pro-L 2") == nil, "a VST is allowed on a bus")
    }

    @MainActor
    private static func testBusChainReassertedOnNewCore() {
        let model = targetModel()
        model.addProcessingInsert("Compressor")
        model.addProcessingInsert("Limiter")
        model.markAudioConsoleDeliveredForTesting()
        var batches: [[JSONObject]] = []
        model.audioReassertObserver = { batches.append($0) }
        model.onConnected()
        model.stopTimersForTesting()
        let routing = batches.first?.first { $0["type"] as? String == "sync-audio-routing-matrix" }
        let master = sends(routing).filter { $0["busId"] as? String == "master" }
        expectEqual(master.count, 2, "the respawn batch has both master sends")
        for send in master {
            expectEqual(chainOf(send), ["Compressor", "Limiter"], "a respawned core gets the master chain")
        }
        // The core's echo (CoreStateCapture busProcessing[]) reads back as a map.
        let echo: JSONObject = ["busProcessing": [
            ["busId": "master", "pluginInserts": ["Compressor", "Limiter"]],
            ["busId": "aux-1", "pluginInserts": [String]()],
        ]]
        expectEqual(AppModel.busChainFromSnapshot(echo), ["master": ["Compressor", "Limiter"]],
                    "busChainFromSnapshot reads busProcessing[] (empty chains dropped)")
    }

    @MainActor
    private static func testPrimaryBusLabel() {
        let model = targetModel()
        // The core's echo hydrated the grid with the default sends.
        for id in ["101", "102"] {
            model.audioSends[id] = Dictionary(uniqueKeysWithValues: AudioConsolePolicy.defaultSendBuses.map { ($0, 0.0) })
        }
        expectEqual(model.primaryBusLabel("101"), "MASTER", "routed to master")
        model.removeSend(source: "101", bus: "master")
        expectEqual(model.primaryBusLabel("101"), "PGM L", "the first routed bus in header order")
        for bus in ["pgm-l", "pgm-r", "mon", "stream"] { model.removeSend(source: "101", bus: bus) }
        model.selectOrRouteSend(source: "101", bus: "aux-1")
        expectEqual(model.primaryBusLabel("101"), "AUX 1", "master removed by an operator edit, aux-1 routed")
        expectEqual(model.processingTargets.first { $0.id == "channel:101" }?.detail, "Channel - AUX 1",
                    "the channel target's detail follows")
        model.removeSend(source: "101", bus: "aux-1")
        expectEqual(model.primaryBusLabel("101"), "MASTER", "nothing routed falls back to MASTER (SVM:11660)")
    }

    // ── Task 1-2 review fix-ups ─────────────────────────────────────────────

    @MainActor
    private static func testFixBlankAddIsNoop() {
        var inserts = ChannelInserts()
        // Mutation: drop the blank guard in ChannelInserts.add -> red (it appends "").
        expectEqual(inserts.add("   "), .rejectedBlank, "a whitespace name is rejected (SVM:5522)")
        expectEqual(inserts.add(""), .rejectedBlank, "an empty name is rejected")
        expectEqual(inserts.add("\n\t"), .rejectedBlank, "a newline/tab name is rejected")
        expectEqual(inserts.chain, [], "nothing entered the chain")
        // The SETUP path says nothing for a blank name, on a channel or a bus.
        // Coverage note: this half pins the observable behaviour only. It
        // never reaches the ChannelInserts.add guard above, because
        // addProcessingInsert returns early on a blank raw name; the mutation
        // named above is caught by the first half alone.
        let model = targetModel()
        model.selectProcessingTarget(AudioProcessingTargetPolicy.channelId("101"))
        expectEqual(model.selectedProcessingTargetId, "channel:101", "precondition: channel target")
        model.takeStatus = "before"
        model.addProcessingInsert("   ")
        expectEqual(model.takeStatus, "before", "no \"added\" status for a blank name on a channel")
        expectEqual(model.channelInserts["101"]?.chain ?? [], [], "the channel chain is unchanged")
        model.selectProcessingTarget(AudioProcessingTargetPolicy.defaultTargetId)
        model.addProcessingInsert("  ")
        expectEqual(model.takeStatus, "before", "no status for a blank name on a bus")
        expect(model.busInserts["master"] == nil, "the bus chain is unchanged")
    }

    @MainActor
    private static func testFixHostHandledNames() {
        // The core's rule (PluginHostScan.h:72): contains "vst" or "host".
        for name in ["Host Test", "vst pro", "VST:Pro-Q 3", "VST3 Bridge Slot", "My VST Compressor"] {
            expect(InsertChainPolicy.isHostHandled(name), "\(name) is host-handled")
        }
        expect(!InsertChainPolicy.isHostHandled("Compressor") && !InsertChainPolicy.isHostHandled("Noise Gate"),
               "built-ins are not host-handled")
        expect(!InsertChainPolicy.isVst("Host Test") && !InsertChainPolicy.isVst("vst pro"),
               "precondition: Windows isVst is narrower (the gap this fix closes)")
        expect(InsertChainPolicy.coreKind("My VST Compressor") == nil,
               "a host-handled name never reaches the built-in matcher")

        let model = chainModel()
        model.editInserts("11") { $0.chain = ["Compressor", "Host Test", "vst pro"] }
        expect(!model.vstInsertsLive, "precondition: no live host")
        // Mutation: filter the wire with isVst instead of isHostHandled -> red.
        expectEqual(model.pluginInsertNames(for: "11"), ["Compressor"], "a dead host gets no plug-in name")
        expectEqual(wireChannel(model.audioMixCommand(), "11")?["pluginInserts"] as? [String], ["Compressor"],
                    "and neither does the wire")
        // Mutation: slots test isVst instead of isHostHandled -> red (Host Test
        // would read PASS, vst pro PASS).
        for onBus in [false, true] {
            let dead = InsertChainPolicy.slots(chain: ["Compressor", "Host Test", "vst pro"], onBus: onBus,
                                               vstLive: false, vstStatusLine: "host down", labelStyle: .channel)
            expectEqual(dead.map(\.statusLabel), ["LIVE", "BYPASS", "BYPASS"],
                        "host-handled names read BYPASS without a live host (onBus \(onBus))")
            expectEqual(dead.last?.tooltip, "host down", "with the host status line (onBus \(onBus))")
        }
        // "Voice Host" would be an EQ to the channel matcher, but the host hook takes it first.
        let voiceHost = InsertChainPolicy.slots(chain: ["Voice Host"], onBus: false, vstLive: false,
                                                vstStatusLine: "x", labelStyle: .channel)
        expectEqual(voiceHost.map(\.statusLabel), ["BYPASS"], "a host-handled name is never LIVE without a host")
        let live = InsertChainPolicy.slots(chain: ["Host Test", "vst pro"], onBus: false, vstLive: true,
                                           vstStatusLine: "x", labelStyle: .target)
        expectEqual(live.map(\.statusLabel), ["START", "START"], "with a live host they read START")
        expectEqual(live.first?.tooltip, "Plug-in starting; audio continues unchanged.",
                    "the Mac START text (no \"Click to open its controls.\")")
        model.vstHost.serveRunning = true
        expectEqual(model.pluginInsertNames(for: "11"), ["Compressor", "Host Test", "vst pro"],
                    "a live host gets them in chain position")
    }

    @MainActor
    private static func testFixBusSlotRule() {
        let chain = ["Voice Compressor", "Noise Limiter", "Noise Gate"]
        // Mutation: bus slots use the channel matcher (coreKind) -> red: "Voice
        // Compressor" is an EQ there and would read PASS.
        let bus = InsertChainPolicy.slots(chain: chain, onBus: true, vstLive: false,
                                          vstStatusLine: "x", labelStyle: .target)
        expectEqual(bus.map(\.statusLabel), ["LIVE", "LIVE", "PASS"],
                    "bus: limiter then compressor process, the gate passes (AudioDsp.h applyBusInsertChain)")
        expectEqual(bus.first?.tooltip, "Processing active.", "a processing bus insert's tooltip")
        expectEqual(bus.last?.tooltip,
                    "Built-in Noise Gate does not process on a bus; audio passes through unchanged.", "the PASS reason")
        let channel = InsertChainPolicy.slots(chain: chain, onBus: false, vstLive: false,
                                              vstStatusLine: "x", labelStyle: .channel)
        expectEqual(channel.map(\.statusLabel), ["LIVE", "LIVE", "LIVE"], "all three process on a channel")
        expect(InsertChainPolicy.busKind("Voice Compressor") == .compressor, "Voice Compressor compresses on a bus")
        expect(InsertChainPolicy.busKind("Compressor Limiter") == .limiter, "limiter is checked first")
        expect(InsertChainPolicy.busKind("Built-in EQ") == nil, "EQ passes on a bus")
        // OD-1 still refuses EQ on a bus, and no longer refuses a compressor the
        // bus WOULD run. Mutation: drop the busKind guard in busRefusal -> red.
        expect(AudioProcessingTargetPolicy.busRefusal("Voice Compressor") == nil,
               "a bus may take Voice Compressor")
        expectEqual(AudioProcessingTargetPolicy.busRefusal("eq"),
                    "Built-in EQ does not process on a bus yet; add a compressor or limiter.", "EQ still refused")
        expectEqual(AudioProcessingTargetPolicy.busRefusal("Noise Gate"),
                    "Noise Gate does not process on a bus yet; add a compressor or limiter.", "gate still refused")
        let model = targetModel()
        model.addProcessingInsert("Voice Compressor")
        expectEqual(model.busInserts["master"], ["Voice Compressor"], "added to the master chain")
        expectEqual(model.takeStatus, "Voice Compressor added to MASTER", "with the Windows status")
        model.addProcessingInsert("eq")
        expectEqual(model.busInserts["master"], ["Voice Compressor"], "EQ refused on the bus (OD-1)")
    }

    @MainActor
    private static func testFixVstRepickIsNoop() {
        let model = chainModel()
        model.editInserts("11") { $0.chain = ["vst:Pro-Q 3", "Compressor"] }
        // Mutation: drop the same-plug-in early return -> red (the VST moves last).
        model.setVstChannelSelection("11", "vst:Pro-Q 3")
        expectEqual(model.channelInserts["11"]?.chain, ["vst:Pro-Q 3", "Compressor"],
                    "re-picking the current plug-in keeps its chain position")
        model.setVstChannelSelection("11", "VST:pro-q 3")
        expectEqual(model.channelInserts["11"]?.chain, ["vst:Pro-Q 3", "Compressor"],
                    "the comparison is case-insensitive, like every chain compare")
        model.setVstChannelSelection("11", "vst:Other")
        expectEqual(model.channelInserts["11"]?.chain, ["Compressor", "vst:Other"],
                    "a different plug-in still replaces it at the end")
    }

    @MainActor
    private static func testFixEmptyPrefsClearsVst() {
        // Pre-A7 restore was `vstChannelSelection = prefs.vstChannelSelections ?? [:]`:
        // the saved map REPLACED the current one.
        let model = AppModel()
        model.channelInserts["101"] = ChannelInserts(chain: ["Compressor", "vst:Old"])
        model.channelInserts["102"] = ChannelInserts(chain: ["vst:Keep", "Limiter"])
        var prefs = ShellPrefs()
        prefs.vstChannelSelections = ["102": "vst:Saved", "103": "vst:New"]
        // Mutation: restore the append-only loop -> red ("vst:Old" survives on 101).
        model.applyRestoredPrefs(prefs)
        expectEqual(model.channelInserts["101"]?.chain, ["Compressor"],
                    "a strip with no saved selection loses its VST; built-ins stay")
        expectEqual(model.channelInserts["102"]?.chain, ["Limiter", "vst:Saved"],
                    "a saved selection replaces the current VST")
        expectEqual(model.channelInserts["103"]?.chain, ["vst:New"], "a new strip gets its saved selection")
        let empty = AppModel()
        empty.channelInserts["101"] = ChannelInserts(chain: ["vst:Old", "Compressor"])
        empty.applyRestoredPrefs(ShellPrefs())
        expectEqual(empty.channelInserts["101"]?.chain, ["Compressor"], "empty prefs clear the VST selection")
        expect(empty.currentPrefs().vstChannelSelections == nil, "and nothing is saved back")
    }

    @MainActor
    private static func testFixBusVstGatedOnHost() {
        let model = targetModel()
        // "Host Test" is host-handled (contains "host") but NOT Windows isVst,
        // so swapping the wire filter back to isVst keeps it and turns red.
        model.busInserts = ["master": ["Compressor", "vst:Test", "Host Test"]]
        expect(!model.vstInsertsLive, "precondition: no live host")
        // Base matrix (routingBaseCommand -> baseRoutingSends).
        // Mutation: pass busInserts instead of wireBusInserts there -> red.
        let base = sends(model.routingBaseCommand()).filter { $0["busId"] as? String == "master" }
        expectEqual(base.count, 2, "fixture: two default master sends")
        for send in base {
            expectEqual(chainOf(send), ["Compressor"], "base master send: the plug-in is dropped without a host")
        }
        // Legacy pushAudioRouting tail (no authority epoch).
        // Mutation: read busInserts instead of wireBusInserts there -> red.
        model.audioSends = ["101": ["master": 0], "102": ["master": -3]]
        var requests: [JSONObject] = []
        model.audioRoutingRequestForTesting = { body in
            requests.append(body)
            return ["snapshot": ["audioRoutingMatrix": ["sends": [JSONObject]()]] as JSONObject]
        }
        model.syncAudioRouting()
        func matrix() -> JSONObject? {
            requests.lazy.compactMap { ($0["commands"] as? [JSONObject])?.first }
                .first { $0["type"] as? String == "sync-audio-routing-matrix" }
        }
        spin(until: { matrix() != nil }, limit: 3)
        let legacy = sends(matrix()).filter { $0["busId"] as? String == "master" }
        expectEqual(legacy.count, 2, "legacy tail: both master sends")
        for send in legacy {
            expectEqual(chainOf(send), ["Compressor"], "legacy master send: the plug-in is dropped without a host")
        }
        // Wire-only: the operator's chain is kept, and its slot says why.
        expectEqual(model.busInserts["master"], ["Compressor", "vst:Test", "Host Test"], "busInserts keeps the operator's chain")
        let slots = InsertChainPolicy.slots(chain: model.busInserts["master"] ?? [], onBus: true,
                                            vstLive: model.vstInsertsLive, vstStatusLine: model.vstStatusLine,
                                            labelStyle: .target)
        expectEqual(slots.map(\.statusLabel), ["LIVE", "BYPASS", "BYPASS"], "the bus plug-in slots read BYPASS")
        expectEqual(slots.last?.tooltip, model.vstStatusLine, "with the host status line")
        // A live host gets the full chain in the base matrix.
        model.vstHost.serveRunning = true
        for send in sends(model.routingBaseCommand()) where send["busId"] as? String == "master" {
            expectEqual(chainOf(send), ["Compressor", "vst:Test", "Host Test"], "a live host gets the bus plug-ins")
        }
    }

    // ── Task 4: SHOW layout ─────────────────────────────────────────────────

    /// XAML:988-994: columns 190 | * | * | 1.4*, ColumnSpacing 10. A 1000 pt
    /// workspace leaves 1000 - 190 - 3*10 = 780 for the cells: 780 / 3.4 each
    /// star. Mutation: equal thirds (eq / gate == 1, red).
    static func testLayoutCellWidths() {
        let widths = AudioLayout.cellWidths(total: 1000)
        expectEqual(AudioLayout.rackWidth, 190, "rack column 190 (XAML:990)")
        expectEqual(AudioLayout.columnSpacing, 10, "column spacing 10 (XAML:988)")
        expect(abs(widths.gate - widths.comp) < 0.01, "gate == comp (\(widths.gate), \(widths.comp))")
        expect(abs(widths.eq / widths.gate - 1.4) < 0.01, "eq / gate == 1.4 (got \(widths.eq / widths.gate))")
        let sum: CGFloat = widths.gate + widths.comp + widths.eq
        expect(abs(sum - 780) < 0.01, "gate + comp + eq == 1000 - 190 - 30 (got \(sum))")
        // Too narrow to hold the rack: no negative widths.
        let tiny = AudioLayout.cellWidths(total: 100)
        expect(tiny.gate >= 0 && tiny.comp >= 0 && tiny.eq >= 0, "no negative cell widths")
        expectEqual(AudioLayout.workspaceHeight, 255, "workspace height 255 (XAML:986)")
        expectEqual(AudioLayout.stripWidth, 120, "strip width 120 (XAML:1326)")
        expectEqual(AudioLayout.railWidth, 150, "rail width 150 (XAML:1435)")
    }

    /// SVM:11451-11456 (GainLabel, PanLabel) and SVM:11464 (MuteStateLabel).
    static func testPanGainTooltipStrings() {
        expectEqual(ConsoleStripLabels.pan(0.004), "C", "pan 0.004 -> C")
        expectEqual(ConsoleStripLabels.pan(0), "C", "pan 0 -> C")
        expectEqual(ConsoleStripLabels.pan(-0.25), "L 0.25", "pan -0.25")
        expectEqual(ConsoleStripLabels.pan(0.5), "R 0.50", "pan 0.5")
        expectEqual(ConsoleStripLabels.pan(-1), "L 1.00", "pan -1")
        expectEqual(ConsoleStripLabels.gain(3), "+3.0 dB", "gain +3")
        expectEqual(ConsoleStripLabels.gain(-2.5), "-2.5 dB", "gain -2.5")
        expectEqual(ConsoleStripLabels.gain(0), "0.0 dB", "gain 0 has no sign")
        expectEqual(ConsoleStripLabels.meterTooltip(muted: true, sourceMuted: false), "Muted in mix", "muted")
        expectEqual(ConsoleStripLabels.meterTooltip(muted: true, sourceMuted: true), "Muted in mix",
                    "the console mute wins over the Zoom mute")
        expectEqual(ConsoleStripLabels.meterTooltip(muted: false, sourceMuted: true), "Source muted", "Zoom-muted")
        expectEqual(ConsoleStripLabels.meterTooltip(muted: false, sourceMuted: false), "Live", "live")
    }

    /// W/Views/AudioPage.xaml.cs:137-166 (InsertParamSpecs), less the EQ
    /// presence rows (dead keys since C6b). Mutation: port the presence rows
    /// (red on the EQ count).
    static func testParamSpecsAreWindows() {
        typealias S = InsertParamSpec
        expectEqual(InsertChainPolicy.paramSpecs("Noise Gate"), [
            S(label: "Threshold (dBFS)", key: "thresholdDb", min: -80, max: -12, step: 1, defaultValue: -48),
            S(label: "Release (ms)", key: "releaseMs", min: 20, max: 500, step: 5, defaultValue: 120),
        ], "gate specs")
        let highpass = [S(label: "High-pass (Hz)", key: "highpassHz", min: 20, max: 400, step: 5, defaultValue: 90)]
        expectEqual(InsertChainPolicy.paramSpecs("High-pass"), highpass, "high-pass specs")
        expectEqual(InsertChainPolicy.paramSpecs("Built-in EQ"), highpass, "EQ: the high-pass row only")
        expectEqual(InsertChainPolicy.paramSpecs("Built-in EQ").count, 1, "EQ has exactly one spec")
        expectEqual(InsertChainPolicy.paramSpecs("Compressor"), [
            S(label: "Threshold (dBFS)", key: "thresholdDb", min: -40, max: -6, step: 1, defaultValue: -18),
            S(label: "Ratio", key: "ratio", min: 1, max: 20, step: 0.5, defaultValue: 4),
        ], "compressor specs")
        expectEqual(InsertChainPolicy.paramSpecs("Limiter"), [
            S(label: "Ceiling (dBFS)", key: "ceilingDb", min: -12, max: -0.1, step: 0.1, defaultValue: -1),
        ], "limiter specs")
        expectEqual(InsertChainPolicy.paramSpecs("vst:Some Plugin"), [], "a plug-in has no built-in specs")
        expectEqual(InsertChainPolicy.paramSpecs("Mystery"), [], "an unknown name has none")
        // The slot popover writes the same field the cell reads.
        var inserts = ChannelInserts()
        inserts.setParam("Compressor", key: "ratio", 8)
        expectEqual(inserts.compRatio, 8, "popover ratio edits the COMP cell's ratio")
        expectEqual(inserts.param("Compressor", key: "ratio"), 8, "and reads it back")
        inserts.setParam("Limiter", key: "ceilingDb", -3)
        expectEqual(inserts.limiterCeilingDb, -3, "popover ceiling edits limiterCeilingDb")
        inserts.setParam("Built-in EQ", key: "highpassHz", 150)
        expectEqual(inserts.eqHighpassHz, 150, "popover high-pass edits the EQ cell's low cut")
    }

    /// OD-3, XAML:31-36: SHOW is a fixed viewport, so the Audio tab is not in
    /// the page ScrollView and its width is uncapped. Mutation: revert the
    /// TabPage change (red).
    static func testShowNotInPageScroll() {
        expectEqual(TabPage.wrapsInScrollView(.audio), false, "the Audio tab brings its own scrolling")
        expectEqual(TabPage.wrapsInScrollView(.settings), true, "a form tab keeps the page scroll")
        expectEqual(TabPage.wrapsInScrollView(.sources), true, "Sources keeps the page scroll")
        expect(TabPageLayout.maxWidth(for: .audio) == .infinity, "the Audio tab width is uncapped")
    }

    // ── Task 5: SETUP layout ────────────────────────────────────────────────

    /// XAML:762-786: the 2x2 grid Add EQ / Add compressor / Add VST3 plug-in /
    /// Clear chain. OD-1 dims Add EQ on a bus target with the refusal sentence.
    /// Mutation: enable Add EQ on buses (red).
    static func testSetupAddButtons() {
        let status = "Plug-in host not running"
        let titles = ["Add EQ", "Add compressor", "Add VST3 plug-in", "Clear chain"]
        let bus = AudioSetupPolicy.addButtons(target: "bus:master", vstLive: false, vstStatusLine: status,
                                              chain: ["Compressor"])
        expectEqual(bus.map(\.title), titles, "the four buttons, in XAML order")
        expectEqual(bus[0].enabled, false, "Add EQ is dimmed on a bus (OD-1)")
        expectEqual(bus[0].reason, "Built-in EQ does not process on a bus yet; add a compressor or limiter.",
                    "with the OD-1 sentence (AudioProcessingTargetPolicy.busRefusal)")
        expectEqual(bus[1].enabled, true, "Add compressor is enabled on a bus")
        expect(bus[1].reason == nil, "an enabled button gives no reason")
        expectEqual(bus[2].enabled, false, "Add VST3 is dimmed while the host is not live")
        expectEqual(bus[2].reason, status, "with the host status line")
        expectEqual(bus[3].enabled, true, "Clear chain is enabled on a non-empty chain")
        let channel = AudioSetupPolicy.addButtons(target: "channel:101", vstLive: false, vstStatusLine: status,
                                                  chain: ["Built-in EQ"])
        expectEqual(channel.map(\.enabled), [true, true, false, true], "a channel: all enabled except Add VST3")
        expectEqual(channel[2].reason, status, "Add VST3 says why")
        let live = AudioSetupPolicy.addButtons(target: "channel:101", vstLive: true, vstStatusLine: status,
                                               chain: [])
        expectEqual(live[2].enabled, true, "a live host enables Add VST3")
        expectEqual(live[3].enabled, false, "Clear chain is dimmed on an empty chain")
        expectEqual(live[3].reason, "The insert chain is already empty.", "and says why")
        let emptyBus = AudioSetupPolicy.addButtons(target: "bus:aux-1", vstLive: true, vstStatusLine: status, chain: [])
        expectEqual(emptyBus[3].enabled, false, "Clear chain is dimmed on an empty bus chain")
    }

    /// OD-2, XAML:62-73: three pills; the Mac-only PLUG-INS pill is gone and
    /// its env value opens SETUP with the VST browser disclosure open.
    /// Mutation: keep PLUG-INS (red).
    static func testSurfacePills() {
        expectEqual(AudioPane.surfaces, ["SHOW", "ROUTING", "SETUP"], "the Windows pills")
        let plugins = AudioPane.resolveSurface("PLUG-INS")
        expectEqual(plugins.surface, "SETUP", "PLUG-INS resolves to SETUP")
        expectEqual(plugins.vstBrowserOpen, true, "with the VST browser open")
        let setup = AudioPane.resolveSurface("SETUP")
        expectEqual(setup.surface, "SETUP", "SETUP stays SETUP")
        expectEqual(setup.vstBrowserOpen, false, "with the browser collapsed (XAML:647 IsExpanded=False)")
        expectEqual(AudioPane.resolveSurface("ROUTING").surface, "ROUTING", "ROUTING")
        expectEqual(AudioPane.resolveSurface(nil).surface, "SHOW", "no env opens SHOW")
        expectEqual(AudioPane.resolveSurface("bogus").surface, "SHOW", "an unknown value opens SHOW")
    }

    /// XAML:636-643, 714-760: the target picker drives the SIGNAL FLOW list.
    @MainActor
    static func testSetupTargetPickerDrivesList() {
        let model = targetModel()
        model.selectProcessingTarget("bus:aux-1")
        expectEqual(model.selectedProcessingTargetId, "bus:aux-1", "precondition: aux-1 is a target")
        model.addProcessingInsert("Compressor")
        let slots = model.selectedTargetSlots
        expectEqual(slots.map(\.slotLabel), ["01"], "target-style slot label")
        expectEqual(slots.map(\.name), ["Compressor"], "the bus chain")
        expectEqual(slots.map(\.statusLabel), ["LIVE"], "a bus compressor processes")
        expectEqual(model.selectedProcessingTarget?.insertLabel, "Compressor", "the insert label")
        expectEqual(model.selectedProcessingTarget?.label, "AUX 1", "the target label")
        // Switching the target switches the list.
        model.selectProcessingTarget("channel:101")
        expectEqual(model.selectedTargetSlots.map(\.name), [], "channel 101 has its own (empty) chain")
        expectEqual(model.selectedProcessingTarget?.insertLabel, "No inserts", "SVM:11611")
        // Windows binds the readouts to SelectedAudioMix (SVM:2195-2233), the
        // SHOW-selected strip, whatever the target: a bus target picked in the
        // model still reads strip 101. Mutation: dash on a bus target (red).
        model.editStrip("101") { $0.manualGainDb = 3 }
        model.selectAudioStrip("101")
        model.selectProcessingTarget("bus:aux-1")
        expect(model.selectedProcessingTarget.map { AudioProcessingTargetPolicy.parse($0.id).bus != nil } == true,
               "precondition: the model's selected target is a bus")
        expectEqual(model.setupReadouts.gain, "+3 dB", "the readouts follow the selected strip on a bus target")
        expectEqual(model.setupReadouts.manual, "+3 dB", "manual gain too")
        // No explicit selection: the effective strip is the first one (101), as
        // SHOW shows it (a7-fix5-effective-strip-shared covers the empty case).
        model.selectedAudioStripId = nil
        expectEqual(model.setupReadouts.gain, "+3 dB", "no explicit selection reads the first strip")
    }

    /// Honest reporting (Task 3 review): the core reads a bus chain only from
    /// sends TO that bus (MediaCore.cpp:7195-7212), so a chain on a bus nothing
    /// is routed to never runs. Mutation: always hide the note (red).
    @MainActor
    static func testSetupUnroutedBusNote() {
        let note = "Nothing is routed to this bus yet, so its inserts do not run."
        expectEqual(AudioSetupPolicy.unroutedBusNote, note, "the sentence")
        let model = targetModel()
        model.selectProcessingTarget("bus:aux-1")
        expectEqual(model.selectedTargetUnroutedNote, note, "aux-1 has no sends by default")
        model.selectProcessingTarget("bus:master")
        expect(model.selectedTargetUnroutedNote == nil, "master carries the default sends")
        model.selectProcessingTarget("channel:101")
        expect(model.selectedTargetUnroutedNote == nil, "a channel target never shows it")
        // An operator route to aux-1 (the grid's audioSends) clears it.
        model.selectProcessingTarget("bus:aux-1")
        model.selectOrRouteSend(source: "101", bus: "aux-1")
        expect(model.selectedTargetUnroutedNote == nil, "a routed source makes the chain run")
        // A custom bus that only SENDS to master is still unrouted: its chain is
        // read from source sends to it, and with no source its PCM is empty.
        let custom = model.addCustomBus()
        model.setBusSend(custom.id, target: "master", enabled: true)
        model.selectProcessingTarget("bus:\(custom.id)")
        expectEqual(model.selectedTargetUnroutedNote, note, "a bus-to-bus output does not feed its chain")
        model.selectOrRouteSend(source: "102", bus: custom.id)
        expect(model.selectedTargetUnroutedNote == nil, "a source routed to the custom bus clears it")
        // Pure form.
        expectEqual(AudioSetupPolicy.unroutedNote(target: "bus:aux-2", routedBusIds: ["master"]), note, "pure: unrouted")
        expect(AudioSetupPolicy.unroutedNote(target: "bus:aux-2", routedBusIds: ["aux-2"]) == nil, "pure: routed")
    }

    // ── Task 4 review fix-ups ───────────────────────────────────────────────

    /// W/Views/AudioMixerWindow.xaml.cs:17-18 (1080 x 700). The minimum height
    /// is the smallest that renders SHOW usably (strip faders, tick scale and
    /// master meters, checked in renders at 900 x 640).
    /// Mutations: default back to no size / 900 x 520, or the minimum back to 520 (red).
    static func testFix4PopoutSize() {
        expectEqual(AudioLayout.popoutDefault.width, 1080, "Windows WindowWidth")
        expectEqual(AudioLayout.popoutDefault.height, 700, "Windows WindowHeight")
        expectEqual(AudioLayout.popoutMin.width, 900, "the minimum width the Task 4 render held at")
        expectEqual(AudioLayout.popoutMin.height, 640, "the minimum height SHOW stays usable at")
        expect(AudioLayout.popoutMin.width <= AudioLayout.popoutDefault.width
               && AudioLayout.popoutMin.height <= AudioLayout.popoutDefault.height,
               "the default size satisfies the minimum")
    }

    /// Honest reporting: before any scan has completed the add menu must not
    /// claim "No VST3 plugins found" (the core's status starts at "absent").
    /// Mutation: state() returns .noneFound for an un-scanned "absent" (red).
    @MainActor
    static func testFix4VstMenuState() {
        typealias P = VstAddMenuPolicy
        expectEqual(P.state(status: "absent", pluginCount: 0, scanCompleted: false), .notScanned, "never scanned")
        expectEqual(P.emptyLine(.notScanned), "Plug-in scan has not run yet — reopen this menu in a moment",
                    "the not-scanned line")
        expectEqual(P.state(status: "scanning", pluginCount: 0, scanCompleted: false), .scanning, "scanning")
        expectEqual(P.state(status: "probing", pluginCount: 0, scanCompleted: true), .scanning, "probing")
        expectEqual(P.emptyLine(.scanning), "Scanning for plugins — reopen this menu in a moment", "xaml.cs copy")
        expectEqual(P.state(status: "ready", pluginCount: 0, scanCompleted: true), .noneFound, "scanned, none")
        expectEqual(P.emptyLine(.noneFound), "No VST3 plugins found on this machine", "xaml.cs copy")
        expectEqual(P.state(status: "probing", pluginCount: 2, scanCompleted: false), .plugins, "a list wins")
        expect(P.emptyLine(.plugins) == nil, "a list has no empty line")
        // Completion.
        expect(!P.scanCompleted(status: "absent", pluginCount: 0, requestReturned: false), "absent before a scan")
        expect(P.scanCompleted(status: "absent", pluginCount: 0, requestReturned: true), "absent after the scan: host not found")
        expect(!P.scanCompleted(status: "scanning", pluginCount: 0, requestReturned: true), "still scanning")
        expect(P.scanCompleted(status: "ready", pluginCount: 0, requestReturned: false), "a core-kicked scan finished")
        expect(P.scanCompleted(status: "error", pluginCount: 0, requestReturned: false), "a failed scan finished")
        // A fresh model (no bridge, no scan) reads "not scanned", never "none found".
        let model = AppModel()
        expect(!model.vstScanCompleted, "a fresh model has not scanned")
        expectEqual(P.state(status: model.vstHost.status, pluginCount: model.vstPlugins.count,
                            scanCompleted: model.vstScanCompleted), .notScanned, "fresh model state")
        model.applyPluginHostSnapshot(["status": "ready", "plugins": [] as [JSONObject]])
        expect(model.vstScanCompleted, "a ready snapshot completes the scan")
    }

    /// The SHOW rack adds/removes on the channel it shows and leaves SETUP's
    /// processing target alone (Windows' rack never changes the target).
    /// Mutation: rackAdd selects the strip first (the old targetThisStrip) (red).
    @MainActor
    static func testFix4RackKeepsTarget() {
        let model = targetModel()
        model.selectProcessingTarget(AudioProcessingTargetPolicy.defaultTargetId)
        expectEqual(model.selectedProcessingTargetId, "bus:master", "precondition: bus:master is the target")
        ProcessingWorkspace.rackAdd(model, stripId: "101", "Limiter")
        expectEqual(model.selectedProcessingTargetId, "bus:master", "the target stays bus:master")
        expectEqual(model.channelInserts["101"]?.chain, ["Limiter"], "strip 101's chain gets the Limiter")
        expect(model.busInserts["master"] == nil, "the master chain is untouched")
        expectEqual(model.takeStatus, "Limiter added to \(model.audioSourceLabel("101"))", "the Windows status")
        ProcessingWorkspace.rackAdd(model, stripId: "101", "Limiter")
        expectEqual(model.takeStatus, "Limiter is already on \(model.audioSourceLabel("101"))", "duplicate status")
        ProcessingWorkspace.rackRemove(model, stripId: "101", "Limiter")
        expectEqual(model.selectedProcessingTargetId, "bus:master", "remove keeps the target too")
        expectEqual(model.channelInserts["101"]?.chain ?? [], [], "removed from 101")
        expectEqual(model.takeStatus, "Removed Limiter from \(model.audioSourceLabel("101"))", "remove status")
        // The target path still works the same way through the channel branch.
        model.selectAudioStrip("102")
        model.addProcessingInsert("Compressor")
        expectEqual(model.channelInserts["102"]?.chain, ["Compressor"], "addProcessingInsert on a channel target")
    }

    /// Harness: every seeded Audio strip reads a display name, not a raw id.
    /// Mutation: seed the extras as unslotted capture ids (red).
    @MainActor
    static func testFix4SeedStripsNamed() {
        let model = AppModel()
        TabSnapshotHarness.seed(model)
        expect(model.strips.count >= 10, "the seeded bank (\(model.strips.count) strips)")
        for strip in model.strips {
            expect(model.audioSourceLabel(strip.id) != strip.id, "\(strip.id) has a display name")
        }
    }

    // ── Task 5 review fix-ups ───────────────────────────────────────────────

    /// The core reads a bus chain only from SOURCE sends to that bus
    /// (MediaCore.cpp:7195-7212): a bus fed only by another bus's output, or
    /// master with no sources, carries a chain that never runs.
    /// Mutations: count bus-to-bus destinations as routed (red on stream);
    /// hard-code master as routed (red on master).
    @MainActor
    static func testFix5BusToBusFeedIsUnrouted() {
        let note = AudioSetupPolicy.unroutedBusNote
        let model = AppModel()
        expect(model.strips.isEmpty, "fixture: no sources")
        model.selectProcessingTarget("bus:master")
        expectEqual(model.selectedProcessingTargetId, "bus:master", "precondition: master is a target")
        expectEqual(model.selectedTargetUnroutedNote, note, "master with no sources does not run its chain")
        let custom = model.addCustomBus()
        model.setBusSend(custom.id, target: "stream", enabled: true)
        model.setBusSend(custom.id, target: "master", enabled: true)
        model.selectProcessingTarget("bus:stream")
        expectEqual(model.selectedProcessingTargetId, "bus:stream", "precondition: stream is a target")
        expectEqual(model.selectedTargetUnroutedNote, note, "stream fed only by a bus-to-bus send stays unrouted")
        model.selectProcessingTarget("bus:master")
        expectEqual(model.selectedTargetUnroutedNote, note, "so does master")
        // Sources make the default sends, and master/stream run.
        model.zoomAudioSourceIds = ["101"]
        model.refreshStrips()
        expect(model.selectedTargetUnroutedNote == nil, "a source's default send makes master run")
    }

    /// SVM:2200-2230: the readout and Mute strings.
    static func testFix5ReadoutStrings() {
        expectEqual(AudioSetupPolicy.muteLabel(muted: false), "Mute in mix", "SVM:2230")
        expectEqual(AudioSetupPolicy.muteLabel(muted: true), "Unmute in mix", "SVM:2230")
        func strip(_ gain: Double, level: Int = 42) -> AudioStrip {
            AudioStrip(id: "101", outputLevel: level, manualGainDb: gain, muted: false, solo: false, pan: 0, status: "live")
        }
        let unity = AudioSetupPolicy.readouts(strip: strip(0))
        expectEqual(unity.gain, "0 dB", "unity gain has no sign")
        expectEqual(unity.manual, "Auto", "manual 0 reads Auto (SVM:2220)")
        expectEqual(unity.output, "42", "output level is the core's 0..100")
        let up = AudioSetupPolicy.readouts(strip: strip(3.4))
        expectEqual(up.gain, "+3 dB", "positive gain is signed, {gain:0}")
        expectEqual(up.manual, "+3 dB", "manual likewise")
        let down = AudioSetupPolicy.readouts(strip: strip(-6))
        expectEqual(down.gain, "-6 dB", "negative gain")
        expectEqual(down.manual, "-6 dB", "negative manual")
        let none = AudioSetupPolicy.readouts(strip: nil)
        expectEqual([none.gain, none.manual, none.output], ["—", "—", "—"], "no strip reads dashes")
    }

    /// The rack reflows: the seven stages fit one row in the capped column
    /// (820 - 2 x 16 = 788) and fall to fewer columns in the minimum-width
    /// pop-out's right column, derived from the layout constants: window 900,
    /// AudioPane padding 2 x 10, SETUP trailing 8, left column 340, spacing 12
    /// -> right column 520; minus the section padding 2 x 16 -> 488 for the
    /// stage grid. Mutation: a fixed 7 columns (red at the pop-out width).
    static func testFix5RackStageColumns() {
        let L = AudioLayout.self
        let capped = L.setupRightMaxWidth - 2 * L.setupSectionPadding
        expectEqual(capped, 788, "the capped column's grid width")
        expectEqual(MasteringRackLayout.stageColumns(width: capped), 7, "one row in the capped column")
        let rightColumn = L.popoutMin.width - 2 * L.panePadding - L.setupTrailingPadding
            - L.setupLeftWidth - L.setupColumnSpacing
        expectEqual(rightColumn, 520, "the pop-out minimum's SETUP right column")
        let popoutGrid = min(rightColumn, L.setupRightMaxWidth) - 2 * L.setupSectionPadding
        expectEqual(popoutGrid, 488, "the pop-out minimum's stage grid width")
        expectEqual(MasteringRackLayout.stageColumns(width: popoutGrid), 4, "four columns at the pop-out minimum")
        expectEqual(MasteringRackLayout.stageColumns(width: 100), 1, "never zero columns")
        expectEqual(MasteringRackLayout.stageColumns(width: 5000), 7, "never more columns than stages")
    }

    /// SETUP's add-VST menu reads the same scan state as SHOW's (honest
    /// reporting): before any scan it never claims "No VST3 plugins found".
    /// Mutation: setupVstMenuEmptyLine is the old literal path
    /// (vstPlugins.isEmpty ? noneFoundLine : nil) (red).
    @MainActor
    static func testFix5SetupVstMenuState() {
        typealias P = VstAddMenuPolicy
        let model = AppModel()
        expectEqual(model.setupVstMenuEmptyLine, P.notScannedLine, "never scanned: not 'none found'")
        model.applyPluginHostSnapshot(["status": "scanning", "plugins": [] as [JSONObject]])
        expectEqual(model.setupVstMenuEmptyLine, P.scanningLine, "scanning")
        model.applyPluginHostSnapshot(["status": "ready", "plugins": [] as [JSONObject]])
        expectEqual(model.setupVstMenuEmptyLine, P.noneFoundLine, "a completed scan found nothing")
        // SHOW and SETUP agree in every state.
        let show = P.emptyLine(P.state(status: model.vstHost.status, pluginCount: model.vstPlugins.count,
                                       scanCompleted: model.vstScanCompleted))
        expectEqual(model.setupVstMenuEmptyLine, show, "SETUP says what SHOW says")
    }

    /// SHOW and SETUP resolve the same effective strip: the explicit selection
    /// while it exists, else the first strip, else nil (Windows auto-selects
    /// the first channel, SVM:8856-8859). The fallback never moves the target.
    /// Mutation: setupSelectedStrip reads the raw selectedAudioStripId (red).
    @MainActor
    static func testFix5EffectiveStripShared() {
        let model = targetModel()
        // Fixture gains that tell the two strips apart in the readouts.
        model.editStrip("101") { $0.manualGainDb = 4 }
        model.editStrip("102") { $0.manualGainDb = -2 }
        expect(model.selectedAudioStripId == nil, "precondition: nothing explicitly selected")
        expectEqual(AudioPane.selectedStrip(model)?.id, "101", "SHOW falls back to the first strip")
        expectEqual(model.setupSelectedStrip?.id, "101", "SETUP shows the same strip")
        expectEqual(model.setupReadouts.gain, "+4 dB", "SETUP readouts show the fallback strip 101's gain")
        expectEqual(model.selectedProcessingTargetId, "bus:master", "the fallback leaves the target on bus:master")
        model.selectAudioStrip("102")
        expectEqual(AudioPane.selectedStrip(model)?.id, "102", "SHOW: explicit 102")
        expectEqual(model.setupSelectedStrip?.id, "102", "SETUP: explicit 102")
        expectEqual(model.setupReadouts.gain, "-2 dB", "SETUP readouts show strip 102's gain")
        model.zoomAudioSourceIds = ["101"]
        model.refreshStrips()
        expectEqual(AudioPane.selectedStrip(model)?.id, "101", "SHOW: 102 gone, back to 101")
        expectEqual(model.setupSelectedStrip?.id, "101", "SETUP: 102 gone, back to 101")
        model.zoomAudioSourceIds = []
        model.refreshStrips()
        // refreshStrips may keep non-Zoom strips; clear them for the empty case.
        model.strips = []
        expect(AudioPane.selectedStrip(model) == nil, "SHOW: no strips, nil")
        expect(model.setupSelectedStrip == nil, "SETUP: no strips, nil")
        let r = model.setupReadouts
        expectEqual([r.gain, r.manual, r.output], ["—", "—", "—"], "no strips reads dashes")
    }

    // ── Review fix-ups 7 ────────────────────────────────────────────────────

    private static func pluginHostReply(_ status: String) -> JSONObject {
        ["snapshot": ["pluginHost": ["status": status, "plugins": [] as [JSONObject]] as JSONObject] as JSONObject]
    }

    /// The scan request's reply snapshot (taken after the core set "scanning",
    /// MediaCore.cpp startPluginHostScan) is applied before the scan counts as
    /// returned: with the model's state still the stale pre-scan "absent", the
    /// menus say "scanning", never "none found"; a later absent + zero plug-ins
    /// snapshot after completion says "none found".
    /// Mutation: discard the reply (`_ = try await ...`, returned = true) (red).
    @MainActor
    static func testFix7ScanReplyApplied() {
        typealias P = VstAddMenuPolicy
        let model = AppModel()
        expectEqual(model.vstHost.status, "absent", "precondition: the stale pre-scan status")
        var sent: [JSONObject] = []
        model.vstScanRequestForTesting = { body, _ in
            await MainActor.run { sent.append(body) }
            return pluginHostReply("scanning")
        }
        model.kickVstScanIfNeeded()
        spin(until: { model.vstScanRequestReturned }, limit: 2)
        expectEqual(sent.map { $0["type"] as? String ?? "" }, ["scan-vst-plugins"], "one scan request")
        expect(model.vstScanRequestReturned, "the request returned")
        expectEqual(model.vstHost.status, "scanning", "the reply's pluginHost state is applied")
        expect(!model.vstScanCompleted, "a scan the core still runs has not completed")
        expectEqual(model.vstAddMenuState, .scanning, "SHOW's menu: scanning")
        expectEqual(model.setupVstMenuEmptyLine, P.scanningLine, "SETUP's menu: scanning, not 'none found'")
        model.applyPluginHostSnapshot(["status": "absent", "plugins": [] as [JSONObject]])
        expect(model.vstScanCompleted, "absent after the scan: completed")
        expectEqual(model.setupVstMenuEmptyLine, P.noneFoundLine, "a completed scan found nothing")
        expectEqual(model.vstCapability, .missing, "and the host is really missing")
    }

    /// Before any scan, the core's default "absent" is not "host missing":
    /// SETUP's Add VST3 hint and the VST3 insert control's note say the scan
    /// has not run; after a completed scan that leaves "absent" they say the
    /// host was not found.
    /// Mutation: vstCapability maps "absent" to .missing regardless of the scan (red).
    @MainActor
    static func testFix7UnscannedIsNotHostMissing() {
        let model = AppModel()
        expectEqual(model.vstCapability, .notScanned, "a fresh model has not scanned")
        expectEqual(model.vstStatusLine, "Plug-in scan has not run yet.", "the unscanned status line")
        let hint = AudioSetupPolicy.addButtons(target: "channel:101", vstLive: model.vstInsertsLive,
                                               vstStatusLine: model.vstStatusLine, chain: [])
            .first { $0.title == "Add VST3 plug-in" }
        expectEqual(hint?.reason, "Plug-in scan has not run yet.", "SETUP's Add VST3 hint")
        expectEqual(model.vstUnavailableNote, "Plug-in scan has not run yet.", "the insert control's note")
        // A completed scan (the request returned, its reply then a later poll) leaving "absent".
        model.vstScanRequestForTesting = { _, _ in pluginHostReply("scanning") }
        model.scanVstPlugins()
        spin(until: { model.vstScanRequestReturned }, limit: 2)
        model.applyPluginHostSnapshot(["status": "absent", "plugins": [] as [JSONObject]])
        expectEqual(model.vstCapability, .missing, "after a scan: missing")
        expect(model.vstStatusLine.hasPrefix("Plug-in host not found — build corevideo-plugin-host"),
               "the host-missing status line: \(model.vstStatusLine)")
        expectEqual(model.vstUnavailableNote, "Plug-in host not found.", "the insert control's host-missing note")
    }

    /// A failed scan request clears the kicked flag so the next kick retries,
    /// and the menus and status line say the request failed instead of "has
    /// not run yet — reopen in a moment" forever.
    /// Mutation: the catch leaves scanKicked true / sets no failure (red).
    @MainActor
    static func testFix7FailedScanRetries() {
        typealias P = VstAddMenuPolicy
        struct ScanDown: Error {}
        let model = AppModel()
        var attempts = 0
        model.vstScanRequestForTesting = { _, _ in
            await MainActor.run { attempts += 1 }
            throw ScanDown()
        }
        model.kickVstScanIfNeeded()
        spin(until: { model.vstScanRequestFailed }, limit: 2)
        expectEqual(attempts, 1, "one request")
        expect(model.vstScanRequestFailed, "the failure is recorded")
        expect(!model.vstScratch.scanKicked, "the kicked flag is cleared")
        expectEqual(model.vstAddMenuState, .scanFailed, "SHOW's menu: the request failed")
        expectEqual(model.setupVstMenuEmptyLine, "Plug-in scan request failed — click to retry", "SETUP's menu line")
        expectEqual(model.vstStatusLine, "Plug-in scan request failed — rescan to retry.", "the status line")
        expect(model.setupVstMenuEmptyLine != P.notScannedLine, "not stuck on 'has not run yet'")
        // Reopening retries; a good reply clears the failure.
        model.vstScanRequestForTesting = { _, _ in
            await MainActor.run { attempts += 1 }
            return pluginHostReply("scanning")
        }
        model.kickVstScanIfNeeded()
        spin(until: { model.vstScanRequestReturned }, limit: 2)
        expectEqual(attempts, 2, "the next kick sent a second request")
        expect(!model.vstScanRequestFailed, "a new request clears the failure")
        expectEqual(model.setupVstMenuEmptyLine, P.scanningLine, "and the menu follows the reply")
    }

    /// The Audio Mixer pop-out window takes its sizes from AudioLayout and no
    /// literal: ShellApp's Window block names popoutMin and popoutDefault and
    /// carries no number, so a literal that could drift from the constants is red.
    /// Mutation: write `minWidth: 900` (or any literal size) in ShellApp (red).
    static func testFix7PopoutWindowUsesLayout() {
        let file = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("ShellApp.swift")
        guard let source = try? String(contentsOf: file, encoding: .utf8),
              let start = source.range(of: "Window(\"Audio Mixer\""),
              let end = source.range(of: ".defaultSize(", range: start.upperBound..<source.endIndex),
              let close = source.range(of: ")", range: end.upperBound..<source.endIndex) else {
            expect(false, "ShellApp.swift's Audio Mixer Window block must be readable from the harness"); return
        }
        // Comment lines (the Windows citation carries "1080 x 700") are not code.
        let code = source[start.lowerBound..<close.upperBound].split(separator: "\n")
            .filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//") }.joined(separator: "\n")
        for name in ["minWidth: AudioLayout.popoutMin.width", "minHeight: AudioLayout.popoutMin.height",
                     "width: AudioLayout.popoutDefault.width", "height: AudioLayout.popoutDefault.height"] {
            expect(code.contains(name), "the pop-out window reads \(name)")
        }
        expect(code.range(of: "[0-9]", options: .regularExpression) == nil,
               "no numeric literal in the pop-out window's sizes")
    }
}
