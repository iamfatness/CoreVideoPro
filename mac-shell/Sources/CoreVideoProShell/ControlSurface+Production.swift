// ControlSurface+Production.swift — RC3: the legacy lower third, captions,
// multiview, mastering and the VST scan, each through the entry point the Mac
// UI uses; then (Tasks 14-15) the revisioned monitor and audio routes.
// Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Tasks 13-15).

import Foundation

extension AppModelControlSurface {
    /// Windows' legacy lower third (StudioControlSurface.cs:482-490): accepted
    /// intent, never "the pixels landed" — on-air is the core's `visible`.
    @MainActor
    static func setLowerThird(_ model: AppModel, on: Bool?) -> ControlInvokeResult {
        let target = on ?? !model.lowerThirdVisible
        guard target else {
            model.hideLowerThird()   // idempotent while a retire is in flight
            return .success
        }
        model.showLowerThird()
        return model.lowerThirdCommandStatus == LowerThirdKeyReadout.needsSource
            ? .fail(LowerThirdKeyReadout.needsSource) : .success
    }

    static let automaticCaptionsUnavailable =
        "Automatic captions are unavailable: CoreVideo does not transcribe speech. "
        + "Use explicit caption cues from a control client or the show engine."

    /// Windows MagicSceneCoordinator.cs:71-72: true is refused, false is a no-op.
    @MainActor
    static func setAutoCaptions(_ on: Bool) -> ControlInvokeResult {
        on ? .fail(automaticCaptionsUnavailable) : .success
    }

    /// Windows StudioViewModel.cs:3007-3011 normalization.
    static func normalizeMultiviewLayout(_ mode: String) -> String {
        ["grid", "pgmPvwTop", "pgmPvwLarge", "pgmPvwSide"].contains(mode) ? mode : "pgmPvwTop"
    }

    @MainActor
    static func setMultiviewLayout(_ model: AppModel, mode: String) -> ControlInvokeResult {
        model.configureMultiviewer(mode: normalizeMultiviewLayout(mode))
        return .success
    }

    /// The Settings stepper's two calls, clamped to the Mac's own range (D11).
    @MainActor
    static func setMultiviewTileCount(_ model: AppModel, count: Int) -> ControlInvokeResult {
        let range = ProductionSettingsPolicy.multiviewTileRange
        model.multiviewTileCount = min(max(count, range.lowerBound), range.upperBound)
        model.applyMultiviewConfig()
        return .success
    }

    /// Windows' three mastering targets (StudioViewModel.cs:1291), by index.
    static let masteringTargets: [Double] = [-14, -16, -23]

    @MainActor
    static func setMastering(_ model: AppModel, on: Bool) -> ControlInvokeResult {
        model.editMastering { $0.enabled = on }
        return .success
    }

    @MainActor
    static func setMasteringTarget(_ model: AppModel, index: Int) -> ControlInvokeResult {
        let lufs = masteringTargets[min(max(index, 0), masteringTargets.count - 1)]
        model.editMastering { $0.targetLufs = lufs }
        return .success
    }

    /// D10: the Windows index when the continuous Mac target is exactly one of
    /// the three; -1 otherwise.
    static func masteringTargetIndex(_ lufs: Double) -> Int {
        masteringTargets.firstIndex(of: lufs) ?? -1
    }

    /// Windows always succeeds; the Mac would otherwise claim a scan that never runs.
    @MainActor
    static func scanVst(_ model: AppModel) -> ControlInvokeResult {
        guard model.bridge != nil || model.quitRequestForTesting != nil else {
            return .fail("VST3 scan is unavailable: no media core.")
        }
        model.scanVstPlugins()
        return .success
    }
}
