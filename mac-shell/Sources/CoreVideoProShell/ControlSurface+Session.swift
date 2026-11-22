// ControlSurface+Session.swift — session actions: the Take transition, the
// capture engine, Zoom join/leave, capture online/offline and the RTMP
// destination. Each calls the entry point the Mac UI uses; where the UI is
// fire-and-forget and Windows awaits an outcome, the model has an awaitable twin.
// Plan: docs/superpowers/plans/2026-11-16-mac-remote-control-phase1.md (Tasks 9-11).

import Foundation

extension AppModelControlSurface {
    /// Windows: always Success; the model normalizes like NormalizeTakeTransitionMode.
    @MainActor
    static func setTransition(_ model: AppModel, mode: String) -> ControlInvokeResult {
        model.setTakeTransition(mode)
        return .success
    }

    @MainActor
    private static func hasCore(_ model: AppModel) -> Bool {
        model.bridge != nil || model.quitRequestForTesting != nil
    }

    /// Windows RunTransportToggle.
    @MainActor
    static func toggleEngine(_ model: AppModel) -> ControlInvokeResult {
        guard hasCore(model) else {
            return .fail("Cannot toggle capture engine right now (the control is unavailable in the current state).")
        }
        model.setCapture(enabled: !model.captureEnabled)
        return .success
    }

    /// Windows RunTransportSet: an identical request is an idempotent success.
    @MainActor
    static func setEngine(_ model: AppModel, on: Bool) -> ControlInvokeResult {
        if model.captureEnabled == on { return .success }
        guard hasCore(model) else {
            return .fail("Cannot \(on ? "start" : "stop") capture engine right now (the control is unavailable in the current state).")
        }
        model.setCapture(enabled: on)
        return .success
    }

    /// Windows StudioControlSurface.cs:374-386 (texts verbatim).
    @MainActor
    static func setCaptureOnline(_ model: AppModel, sourceId: String, online: Bool) async -> ControlInvokeResult {
        guard case .capture(let deviceId)? = ControlSourceId.parse(sourceId) else {
            return .fail("Choose a capture source ID.")
        }
        return await model.setCaptureDeviceOnline(deviceId, online: online)
    }
}
