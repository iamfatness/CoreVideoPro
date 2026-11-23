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
}
