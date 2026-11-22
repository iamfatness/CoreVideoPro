// ControlSurface+Production.swift — RC3: the legacy lower third, captions,
// multiview, mastering and the VST scan, each through the entry point the Mac
// UI uses; then (Tasks 14-15) the revisioned monitor and audio routes.
// Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Tasks 13-15).

import Foundation

extension AppModelControlSurface {
    /// Windows' legacy lower third (StudioControlSurface.cs:482-490): accepted
    /// intent, never "the pixels landed" — on-air is the core's `visible`.
    /// Routed through the one lower-third entry (LowerThirdAuthority.swift), so a
    /// control client, the footer and the drawer can never disagree about who owns
    /// the lower third (#1166). Toggle follows whoever renders Program now.
    @MainActor
    static func setLowerThird(_ model: AppModel, on: Bool?) async -> ControlInvokeResult {
        let target = on ?? !model.lowerThirdIntentVisible
        let result = await model.applyLowerThirdIntent(visible: target)
        return result.accepted ? .success : .fail(result.reason)
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

    // ── revisioned remote edits (Tasks 14-15) ──

    static let revisionError = "expectedRevision must be a nonnegative safe integer."

    /// Windows: a finite, non-negative, integral number <= 2^53-1; absent = nil.
    static func safeRevision(_ value: Any?) -> Result<Int64?, ControlBindError> {
        guard let value else { return .success(nil) }
        // The router binds a Double; a direct (surface-level) caller may pass an integer.
        let number: Double? = (value as? Double) ?? (value as? Int).map(Double.init) ?? (value as? Int64).map(Double.init)
        guard let number, number.isFinite, number >= 0, number.rounded() == number,
              number <= 9_007_199_254_740_991 else { return .failure(ControlBindError(message: revisionError)) }
        return .success(Int64(number))
    }

    static func blankIsNil(_ value: Any?) -> String? {
        guard let text = value as? String, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return nil }
        return text
    }

    /// audio.monitor.set (enabled) / audio.monitor.volume (volume), args 1-3 = epoch, revision, operation id.
    @MainActor
    static func setMonitor(_ model: AppModel, enabled: Bool?, volume: Double?, _ args: [ControlArg]) async -> ControlInvokeResult {
        func arg(_ index: Int) -> Any? { index < args.count ? args[index].value : nil }
        let revision: Int64?
        switch safeRevision(arg(2)) {
        case .failure(let error): return .fail(error.message)
        case .success(let value): revision = value
        }
        switch await model.requestMonitorControl(enabled: enabled, volume: volume, expectedEpoch: blankIsNil(arg(1)),
                                                 expectedRevision: revision, operationId: blankIsNil(arg(3))) {
        case .applied: return .success
        case .conflict(let current):
            return .fail("Monitor control conflict at revision \(current); read state and retry with the current epoch and revision.")
        case .rejected: return .fail("Monitor control rejected; application is unconfirmed.")
        case .unconfirmed: return .fail("Monitor control unconfirmed; application is unconfirmed.")
        }
    }

    /// audio.route.set (Windows StudioControlSurface.cs:1345-1369).
    @MainActor
    static func setAudioRoute(_ model: AppModel, _ args: [ControlArg]) async -> ControlInvokeResult {
        func arg(_ index: Int) -> Any? { index < args.count ? args[index].value : nil }
        func text(_ index: Int) -> String { (arg(index) as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines) }
        let source = text(0), bus = text(1), epoch = text(4), operation = text(6)
        guard !source.isEmpty, !bus.isEmpty, !epoch.isEmpty, !operation.isEmpty else {
            return .fail("sourceId, busId, authorityEpoch and operationId are required.")
        }
        let gain = (arg(3) as? Double) ?? (arg(3) as? Int).map(Double.init) ?? .nan
        guard gain.isFinite, (-60...10).contains(gain) else { return .fail("gainDb must be between -60 and +10.") }
        guard case .success(let revision?) = safeRevision(arg(5)) else { return .fail(revisionError) }
        // A Mac text: the Mac's full-state matrix would drop an unknown bus silently.
        guard AppModel.routingBuses.contains(bus) else {
            return .fail("busId must be one of \(AppModel.routingBuses.joined(separator: ", ")).")
        }
        switch await model.requestAudioRouteControl(source: source, bus: bus, enabled: arg(2) as? Bool ?? false,
                                                    gainDb: gain, expectedEpoch: epoch, expectedRevision: revision,
                                                    operationId: operation) {
        case .applied: return .success
        case .conflict(let current): return .fail("Audio route conflict at revision \(current); read state and retry.")
        case .rejected: return .fail("Audio route rejected; application is unconfirmed.")
        case .unconfirmed: return .fail("Audio route unconfirmed; application is unconfirmed.")
        }
    }

    /// Windows ControlAudioRouteState: the core-applied sends, sorted (sourceId, busId).
    @MainActor
    static func audioRoutes(_ model: AppModel) -> [[String: Any]] {
        model.audioRouteControl.applied.filter { $0.value.enabled }.keys.sorted().map { key in
            ["sourceId": key.source, "busId": key.bus, "gainDb": model.audioRouteControl.applied[key]?.gainDb ?? 0]
        }
    }

    /// One row per strip, sorted by id. No peakDbfs/rmsDbfs: the Mac strip has no
    /// true-peak or loudness per source, and inventing one is the meter lie (CLAUDE.md).
    @MainActor
    static func audioSources(_ model: AppModel) -> [[String: Any]] {
        model.strips.sorted { $0.id < $1.id }.map { strip in
            let slotName = model.slots.first {
                ControlSourceId.canonical(kind: $0.kind, sourceId: $0.sourceId) == strip.id && !$0.name.isEmpty
            }?.name
            return ["sourceId": strip.id, "name": slotName ?? strip.id, "level": strip.outputLevel,
                    "muted": strip.muted, "status": strip.status]
        }
    }
}
