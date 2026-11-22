// Meters (A4): the Windows meter scale and the master readouts, pure.
// Every number here is a measurement the core published (masterMeter,
// audioMixSession.loudnessLufs, per-channel gainReductionDb). No evidence reads
// a dash or zero, never an invented level (docs/reference/honest-reporting.md;
// the 2026-08-09 fabricated-meter lesson).

import Foundation

/// Windows W/Models/AudioMeterScale.cs: a calibrated -60..0 dBFS -> 0..100 scale.
enum AudioMeterScale {
    static let minimumDbfs = -60.0
    static let maximumDbfs = 0.0
    static let majorTicksDbfs: [Double] = [0, -6, -12, -24, -36, -48, -60]

    /// Rounded (Math.Round), not truncated; muted, non-finite or at/below -60 is 0.
    static func toLevel(_ dbfs: Double, muted: Bool = false) -> Int {
        guard !muted, dbfs.isFinite, dbfs > minimumDbfs else { return 0 }
        let normalized = (dbfs - minimumDbfs) / (maximumDbfs - minimumDbfs)
        return max(0, min(100, Int((normalized * 100).rounded(.toNearestOrEven))))
    }
}

enum MasterMeterReadout {
    /// Windows SVM:1313-1323 and the -119 floor rule (the core's floor is -120).
    static func integratedLabel(_ lufs: Double) -> String {
        lufs > -119 ? String(format: "%.1f LUFS", lufs) : "—"
    }

    static func truePeakLabel(_ dbtp: Double) -> String {
        dbtp > -119 ? String(format: "%.1f dBTP", dbtp) : "—"
    }

    static func targetLabel(_ lufs: Double) -> String { String(format: "target %.0f LUFS", lufs) }
    static func ceilingLabel(_ dbfs: Double) -> String { String(format: "ceiling %.1f dBTP", dbfs) }

    /// Windows MasteringGuideMeter: fill fraction, 0 when value <= min (not primed).
    static func guideFraction(value: Double, min lo: Double, max hi: Double) -> Double {
        guard value.isFinite, value > lo, hi > lo else { return 0 }
        return Swift.min(1, (value - lo) / (hi - lo))
    }

    /// Windows TransportViewModel MasterLufsLabel: the core's loudnessLufs
    /// (short-term, else momentary), "LUFS —" before any mixed frame or at -59
    /// and below.
    static func railLufsLabel(loudnessLufs: Double, mixedFrames: Int) -> String {
        guard mixedFrames > 0, loudnessLufs.isFinite, loudnessLufs > -59 else { return "LUFS —" }
        return String(format: "%.1f LUFS", loudnessLufs)
    }

    /// Windows SVM:2401-2407: 0..24 dB -> 0..100, "GR −x.x dB" above 0.05 dB.
    static func gainReduction(_ db: Double) -> (level: Int, label: String) {
        let value = db.isFinite ? db : 0
        let level = Int(max(0, min(100, value / 24.0 * 100.0)))
        return (level, value > 0.05 ? String(format: "GR −%.1f dB", value) : "GR —")
    }
}
