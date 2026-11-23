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
}
