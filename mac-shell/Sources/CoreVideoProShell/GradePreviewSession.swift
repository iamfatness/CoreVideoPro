// One editor's leased native preview demand (grading Phase 2, Task 8; spec §3.6, §5.5). A port of
// the Windows GradePreviewCoordinator (native-shell/CoreVideoPro.WinUI/Services/GradePreviewCoordinator.cs):
//  - a 100 ms tick; a NEW draft revision sends a full `set-grade-preview` (the advanced document
//    registered first), the SAME revision renews with `renewOnly: true` every 500 ms (the core's
//    lease is 3 s, GradePreviewController.h:21, so a crashed editor cannot leave demand alive);
//  - one send in flight; a failed send clears the sent revision so the next tick resends IN FULL;
//  - close sends `enabled: false` after any in-flight send;
//  - a new core generation resets the sent revision (the lease died with the old core).
// No pixels and no UI work here; observations go to GradeEditorModel.observePreview.

import Foundation

struct GradePreviewDraft: Equatable {
    var revision: Int64
    var grade: SourceColorGrade
    var scopesEnabled: Bool
    var scopesOriginal: Bool
    var histogramMode: Int
    var waveformMode: Int
    var compareOriginal: Bool
    var scopeView: Int
    var roi: GradeScopeRoi
}

@MainActor
final class GradePreviewSession {
    /// Harness switch: tests tick by hand with an explicit clock.
    static var autoTick = true
    static let tickInterval: TimeInterval = 0.1
    static let renewInterval: TimeInterval = 0.5

    let instanceId: String
    let sourceId: String
    var draft: GradePreviewDraft
    private(set) var isSending = false
    /// Called with the send error's text (the editor shows "Native preview unavailable: …").
    var onFailure: ((String) -> Void)?

    private let send: (JSONObject) async throws -> JSONObject
    private let register: (AdvancedGradeDocument) async throws -> String
    private var lastRevision: Int64 = -1
    private var lastSent = Date.distantPast
    private var timer: Timer?
    private var closed = false

    init(instanceId: String, sourceId: String, draft: GradePreviewDraft,
         send: @escaping (JSONObject) async throws -> JSONObject,
         register: @escaping (AdvancedGradeDocument) async throws -> String) {
        self.instanceId = instanceId
        self.sourceId = sourceId
        self.draft = draft
        self.send = send
        self.register = register
        if Self.autoTick {
            timer = Timer.scheduledTimer(withTimeInterval: Self.tickInterval, repeats: true) { [weak self] _ in
                Task { @MainActor in self?.tick(now: Date()) }
            }
        }
    }

    /// GradePreviewCoordinator.Tick (:41-55).
    func tick(now: Date) {
        guard !closed, !isSending else { return }
        let renew = lastRevision == draft.revision
        if renew && now.timeIntervalSince(lastSent) < Self.renewInterval { return }
        lastRevision = draft.revision
        lastSent = now
        let snapshot = draft
        isSending = true
        Task { @MainActor in
            await self.transmit(snapshot, enabled: true, renew: renew)
            self.isSending = false
        }
    }

    /// A new core generation: the old lease is gone, so the next tick sends in full (:96-102).
    func coreReset() { lastRevision = -1 }

    /// GradePreviewCoordinator.Dispose (:103-115): stop ticking, then end the demand.
    func close() {
        guard !closed else { return }
        closed = true
        timer?.invalidate(); timer = nil
        let snapshot = draft
        Task { @MainActor in
            while self.isSending { try? await Task.sleep(nanoseconds: 5_000_000) }
            await self.transmit(snapshot, enabled: false, renew: false)
        }
    }

    private func transmit(_ draft: GradePreviewDraft, enabled: Bool, renew: Bool) async {
        do {
            if renew {
                _ = try await send(["type": "set-grade-preview", "instanceId": instanceId, "sourceId": sourceId,
                                    "revision": NSNumber(value: draft.revision), "renewOnly": true])
            } else {
                _ = try await send(try body(draft, enabled: enabled))
            }
        } catch {
            if lastRevision == draft.revision { lastRevision = -1 }
            if enabled && !closed {
                onFailure?((error as? LocalizedError)?.errorDescription ?? "\(error)")
            }
        }
    }

    /// MediaCoreSupervisor.SetGradePreviewAsync (:486-502): root-level fields, the advanced document
    /// by reference (registered first, only when enabled).
    private func body(_ draft: GradePreviewDraft, enabled: Bool) async throws -> JSONObject {
        var grade = draft.grade.wireGrade(documentRef: nil)
        if enabled, let document = draft.grade.advanced {
            grade["advanced"] = ["documentRef": try await register(document)]
        }
        return ["type": "set-grade-preview", "instanceId": instanceId, "sourceId": sourceId,
                "revision": NSNumber(value: draft.revision), "enabled": enabled,
                "scopesEnabled": draft.scopesEnabled, "scopesOriginal": draft.scopesOriginal,
                "histogramMode": draft.histogramMode, "waveformMode": draft.waveformMode,
                "scopeView": draft.scopeView, "compareOriginal": draft.compareOriginal,
                "scopeRoi": draft.roi.wire, "grade": grade]
    }
}
