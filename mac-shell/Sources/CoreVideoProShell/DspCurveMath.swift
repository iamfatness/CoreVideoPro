// Response-curve maths and view for the A7 processing workspace
// (docs/superpowers/plans/2026-11-22-mac-audio-phase4.md, Task 1).
//
// DspCurveMath is a line-for-line port of
// native-shell/CoreVideoPro.WinUI/Services/DspCurveMath.cs: the drawn responses
// are computed from the SAME filter formulas the core runs (RBJ audio-EQ-cookbook
// biquads mirroring native AudioDsp.h:1407-1435 eqHighpass/eqPeaking; the
// compressor/gate static transfer functions mirroring the gain computer at
// AudioDsp.h:1549-1567), so a
// graph shows the actual processing, not an illustration. Every function returns
// normalised [0..1] points ready to scale into pixels. Pinned to vectors printed
// by the Windows file itself (ShellTestsAudioPhase4.swift, a7-curve-*).
//
// DspResponseCurve ports W/Controls/DspResponseCurve.xaml(.cs): colours, stroke
// widths, the reference line and the y flip.

import Foundation
import SwiftUI

struct DspPoint: Equatable {
    let x: Double
    let y: Double
}

enum DspCurveMath {
    static let minFrequencyHz = 20.0
    static let maxFrequencyHz = 20000.0
    static let minDb = -24.0
    static let maxDb = 12.0

    /// The 8-band centres. MUST match kEqBandHz in native AudioDsp.h:1655
    /// (DspCurveMath.cs:67 `EqBandHz`).
    static let eqBandHz: [Double] = [63, 125, 250, 500, 1000, 2000, 4000, 8000]

    private struct Biquad {
        let b0, b1, b2, a1, a2: Double
    }

    /// Math.Clamp(value, min, max).
    private static func clamp(_ value: Double, _ lower: Double, _ upper: Double) -> Double {
        min(max(value, lower), upper)
    }

    private static func highpass(_ sampleRate: Double, _ frequencyHz: Double,
                                 q: Double = 0.7071) -> Biquad {
        let w0 = 2 * Double.pi * frequencyHz / sampleRate
        let cosW0 = cos(w0)
        let alpha = sin(w0) / (2 * q)
        let a0 = 1 + alpha
        return Biquad(
            b0: (1 + cosW0) / 2 / a0,
            b1: -(1 + cosW0) / a0,
            b2: (1 + cosW0) / 2 / a0,
            a1: -2 * cosW0 / a0,
            a2: (1 - alpha) / a0)
    }

    private static func peaking(_ sampleRate: Double, _ frequencyHz: Double, _ gainDb: Double,
                                q: Double = 1.0) -> Biquad {
        let amplitude = pow(10, gainDb / 40)
        let w0 = 2 * Double.pi * frequencyHz / sampleRate
        let cosW0 = cos(w0)
        let alpha = sin(w0) / (2 * q)
        let a0 = 1 + alpha / amplitude
        return Biquad(
            b0: (1 + alpha * amplitude) / a0,
            b1: -2 * cosW0 / a0,
            b2: (1 - alpha * amplitude) / a0,
            a1: -2 * cosW0 / a0,
            a2: (1 - alpha / amplitude) / a0)
    }

    /// |H(e^jw)| of a biquad at frequency f: the standard z-plane magnitude.
    private static func magnitudeAt(_ biquad: Biquad, _ frequencyHz: Double,
                                    _ sampleRate: Double) -> Double {
        let w = 2 * Double.pi * frequencyHz / sampleRate
        let cos1 = cos(w)
        let sin1 = sin(w)
        let cos2 = cos(2 * w)
        let sin2 = sin(2 * w)
        let numReal = biquad.b0 + biquad.b1 * cos1 + biquad.b2 * cos2
        let numImag = -(biquad.b1 * sin1 + biquad.b2 * sin2)
        let denReal = 1 + biquad.a1 * cos1 + biquad.a2 * cos2
        let denImag = -(biquad.a1 * sin1 + biquad.a2 * sin2)
        let num = (numReal * numReal + numImag * numImag).squareRoot()
        let den = (denReal * denReal + denImag * denImag).squareRoot()
        return den <= 1e-12 ? 0 : num / den
    }

    /// 8-band EQ frequency response: high-pass (clamped 20..400 Hz) plus up to
    /// eight peaking bands (gain clamped ±12 dB, Q 1.4) at the octave centres,
    /// mirroring the native chain, with flat bands (|gain| < 0.05 dB) skipped.
    /// x = log-frequency position [0..1]; y = gain mapped [0..1], 0 = minDb, 1 = maxDb.
    static func eqResponse(highpassHz: Double, bandGainsDb: [Double],
                           pointCount: Int = 64, sampleRate: Double = 48000) -> [DspPoint] {
        var cascade = [highpass(sampleRate, clamp(highpassHz, 20, 400))]
        for band in 0..<eqBandHz.count {
            let gainDb = band < bandGainsDb.count ? clamp(bandGainsDb[band], -12, 12) : 0
            if abs(gainDb) >= 0.05 {
                cascade.append(peaking(sampleRate, eqBandHz[band], gainDb, q: 1.4))
            }
        }

        var points: [DspPoint] = []
        points.reserveCapacity(pointCount)
        let logMin = log10(minFrequencyHz)
        let logMax = log10(maxFrequencyHz)
        for index in 0..<pointCount {
            let x = Double(index) / Double(pointCount - 1)
            let frequency = pow(10, logMin + x * (logMax - logMin))
            var magnitude = 1.0
            for stage in cascade {
                magnitude *= magnitudeAt(stage, frequency, sampleRate)
            }

            let db = 20 * log10(max(magnitude, 1e-6))
            let y = (clamp(db, minDb, maxDb) - minDb) / (maxDb - minDb)
            points.append(DspPoint(x: x, y: y))
        }

        return points
    }

    /// Compressor static transfer curve (input dB -> output dB over [-60..0]),
    /// normalised to [0..1] on both axes. Knee-aware: quadratic interpolation
    /// across the knee width, matching applyStereoCompressor's gain computer.
    static func compressorTransfer(thresholdDb: Double, ratio: Double, kneeDb: Double = 0,
                                   pointCount: Int = 48) -> [DspPoint] {
        var points: [DspPoint] = []
        points.reserveCapacity(pointCount)
        let clampedRatio = max(1, ratio)
        let slope = 1.0 - 1.0 / clampedRatio
        let knee = max(0, kneeDb)
        for index in 0..<pointCount {
            let x = Double(index) / Double(pointCount - 1)
            let inputDb = -60 + x * 60
            let over = inputDb - thresholdDb
            var grDb = 0.0
            if knee > 0 && over > -knee / 2 && over < knee / 2 {
                let k = over + knee / 2
                grDb = slope * k * k / (2 * knee)
            } else if over >= knee / 2 {
                grDb = slope * over
            }

            points.append(DspPoint(x: x, y: (clamp(inputDb - grDb, -60, 0) + 60) / 60))
        }

        return points
    }

    /// Gate static transfer curve: below the threshold the output drops by the
    /// RANGE (the core's floor-not-silence gate, floored at -80 dB); at or above
    /// it, unity. Input and output span [-80..0] dB.
    static func gateTransfer(thresholdDb: Double, rangeDb: Double = -80,
                             pointCount: Int = 48) -> [DspPoint] {
        var points: [DspPoint] = []
        points.reserveCapacity(pointCount)
        for index in 0..<pointCount {
            let x = Double(index) / Double(pointCount - 1)
            let inputDb = -80 + x * 80
            let outputDb = inputDb < thresholdDb ? max(-80, inputDb + rangeDb) : inputDb
            points.append(DspPoint(x: x, y: (clamp(outputDb, -80, 0) + 80) / 80))
        }

        return points
    }
}

/// Drawn processor response: EQ frequency response, compressor transfer or gate
/// transfer, computed by DspCurveMath from the SAME parameters the core chain
/// applies. Parameters follow DspResponseCurve.xaml.cs:117-152 (Redraw):
/// eq (p1 = highpassHz, bands = 8 gains), comp (p1 = thresholdDb, p2 = ratio,
/// p3 = kneeDb), gate (p1 = thresholdDb, p2 = rangeDb).
struct DspResponseCurve: View {
    enum Kind { case eq, comp, gate }

    let kind: Kind
    let p1: Double
    let p2: Double
    let p3: Double
    let bands: [Double]
    let isEngaged: Bool

    init(kind: Kind, p1: Double, p2: Double = 0, p3: Double = 0,
         bands: [Double] = [], isEngaged: Bool) {
        self.kind = kind
        self.p1 = p1
        self.p2 = p2
        self.p3 = p3
        self.bands = bands
        self.isEngaged = isEngaged
    }

    // DspResponseCurve.xaml:6-8: Grid Background="#060A0E" CornerRadius="3";
    // CurveLine StrokeThickness 1.5; ReferenceLine Stroke="#22303A" StrokeThickness 1.
    private static let background = Color(hex: 0x060A0E)
    private static let referenceStroke = Color(hex: 0x22303A)
    // DspResponseCurve.xaml.cs:64-74: engaged = #3FD2A8 at opacity 1 (the core is
    // processing); bypassed = #46545C at opacity 0.55 (a dim preview of what IN
    // would do). The graph must never look live when the insert is not on the chain.
    private static let engagedStroke = Color(hex: 0x3FD2A8)
    private static let bypassedStroke = Color(hex: 0x46545C)

    var points: [DspPoint] {
        switch kind {
        case .comp: DspCurveMath.compressorTransfer(thresholdDb: p1, ratio: p2, kneeDb: p3)
        case .gate: DspCurveMath.gateTransfer(thresholdDb: p1, rangeDb: p2)
        case .eq: DspCurveMath.eqResponse(highpassHz: p1, bandGainsDb: bands)
        }
    }

    var body: some View {
        let points = self.points
        let kind = self.kind
        let engaged = isEngaged
        Canvas { context, size in
            // DspResponseCurve.xaml.cs:121-124 (in Redraw, 117-152): skip a host under 8 px.
            guard size.width >= 8, size.height >= 8 else { return }
            var curve = Path()
            for (index, point) in points.enumerated() {
                let cg = CGPoint(x: point.x * size.width, y: (1 - point.y) * size.height)
                if index == 0 { curve.move(to: cg) } else { curve.addLine(to: cg) }
            }
            context.stroke(curve,
                           with: .color(engaged ? Self.engagedStroke
                                                : Self.bypassedStroke.opacity(0.55)),
                           lineWidth: 1.5)

            // Reference: unity line for transfer curves, 0 dB line for EQ. Drawn
            // after the curve, as the XAML declares ReferenceLine after CurveLine.
            var reference = Path()
            switch kind {
            case .comp, .gate:
                reference.move(to: CGPoint(x: 0, y: size.height))
                reference.addLine(to: CGPoint(x: size.width, y: 0))
            case .eq:
                let zeroDbY = (1 - (0 - DspCurveMath.minDb)
                               / (DspCurveMath.maxDb - DspCurveMath.minDb)) * size.height
                reference.move(to: CGPoint(x: 0, y: zeroDbY))
                reference.addLine(to: CGPoint(x: size.width, y: zeroDbY))
            }
            context.stroke(reference, with: .color(Self.referenceStroke), lineWidth: 1)
        }
        .background(Self.background)
        .clipShape(RoundedRectangle(cornerRadius: 3))
    }
}
