// The per-source basic grade editor (grading plan 2026-11-16, Task 6). One model per open
// editor window, at most three (the Windows rule). Every send goes through SourceGradeStore,
// which keeps one apply in flight per source; nothing here talks to the core directly.

import Foundation

@MainActor
final class GradeEditorModel: ObservableObject, Identifiable {
    let sourceId: String
    let sourceName: String
    let binding: String                         // the identity the draft was authored for
    nonisolated var id: String { sourceId }
    var headerTitle: String { "Color grade - \(sourceName)" }      // ColorGradeEditorViewModel.cs:26

    @Published var lut: String
    @Published var exposure: Int
    @Published var contrast: Int
    @Published var saturation: Int
    @Published var temperature: Int
    /// "Apply edits live". Turning it back on sends the current grade once (VM.cs:57).
    @Published var liveEditing: Bool {
        didSet { if liveEditing && !oldValue { Task { _ = await send() } } }
    }
    @Published private(set) var status: String
    @Published private(set) var isApplying = false

    static let initialStatus = "Basic controls retain their established scale."
    /// D3 (spec §5.4): there is no private preview on Metal yet.
    static let previewNotice = "No private preview on this Mac build yet. Applied edits go to air."
    /// D6 overridden: Reset sends `clear`, so the source follows the Program Look again.
    static let resetNotice = "Reset returns this source to the Program Look."
    /// Windows help lines, verbatim (ColorGradeEditorWindow.xaml).
    static let help = [
        "Exposure": "0 is neutral. Expand grade for exposure in stops.",
        "Contrast": "0 is neutral; higher values increase contrast.",
        "Saturation": "0 is neutral; higher values increase color.",
        "Temperature": "Cool ← → warm; 0 is neutral.",
    ]

    var currentGrade: SourceColorGrade {
        SourceColorGrade(lut: lut, exposure: exposure, contrast: contrast, saturation: saturation,
                         temperature: temperature)
    }

    /// Set by AppModel: "Apply and Done" closes the editor through it.
    var onClose: (() -> Void)?
    private let store: SourceGradeStore

    init(sourceId: String, sourceName: String, binding: String,
         seed: SourceColorGrade, draftNotice: String?, store: SourceGradeStore) {
        self.sourceId = sourceId
        self.sourceName = sourceName
        self.binding = binding
        self.store = store
        lut = seed.lut
        exposure = seed.exposure
        contrast = seed.contrast
        saturation = seed.saturation
        temperature = seed.temperature
        // A saved Zoom grade is a DRAFT until Apply Live confirms the binding (Grading.cs:87-90).
        liveEditing = draftNotice == nil
        status = draftNotice ?? Self.initialStatus
    }

    /// After any control change. Live: the store sends it (one in flight per source).
    func edited() {
        guard liveEditing else { return }
        Task { _ = await send() }
    }

    func applyLive() async { _ = await send() }

    /// True only when the core acknowledged it; then the editor closes.
    func applyAndDone() async -> Bool {
        let applied = await send()
        if applied { onClose?() }
        return applied
    }

    /// Look "none", every axis 0; sent as a clear when live (D6 override).
    func reset() {
        lut = "none"; exposure = 0; contrast = 0; saturation = 0; temperature = 0
        edited()
    }

    @discardableResult
    private func send() async -> Bool {
        isApplying = true
        let before = status
        status = "Applying grade…"                                         // Apply.cs:46
        let outcome = await store.apply(currentGrade, sourceId: sourceId, authoredFor: binding)
        isApplying = false
        switch outcome {
        case .applied(let revision):
            status = "Live grade acknowledged · revision \(revision)"      // Apply.cs:52
            return true
        case .refused(let text), .stale(let text):
            status = text
        case .superseded:
            if status == "Applying grade…" { status = before }
        }
        return false
    }
}

enum GradeEditorOpenResult { case opened(GradeEditorModel); case refused(String) }

extension AppModel {
    static let gradeNoSourceText = "Select a source before editing its color grade"          // Grading.cs:68
    static let gradeEditorLimitText = "Close one of the three grade editors before opening another"  // :74

    /// The slot's grade target: the raw Zoom pid or "capture:<id>"; nil when unassigned.
    func gradeSourceId(forSlot slotId: Int) -> String? {
        sourceGradeContext(slotId: slotId)?.gradeSourceId
    }

    /// The slot name, else the roster or device name, else the id.
    func gradeSourceName(for sourceId: String) -> String {
        if let slot = slots.first(where: { sourceGradeContext(slotId: $0.id)?.gradeSourceId == sourceId }),
           !slot.name.isEmpty { return slot.name }
        if let participant = roster.first(where: { $0.id == sourceId }) { return participant.name }
        if sourceId.hasPrefix("capture:"),
           let device = captureDevices.first(where: { "capture:\($0.id)" == sourceId }) { return device.name }
        return sourceId
    }

    func openGradeEditor(sourceId: String?) -> GradeEditorOpenResult {
        guard let sourceId, !sourceId.isEmpty else { return .refused(Self.gradeNoSourceText) }
        if let open = gradeEditors[sourceId] { return .opened(open) }
        guard gradeEditors.count < 3 else { return .refused(Self.gradeEditorLimitText) }
        let seed = sourceGrades.seed(for: sourceId)
        let editor = GradeEditorModel(
            sourceId: sourceId, sourceName: gradeSourceName(for: sourceId),
            binding: gradeIdentity(for: sourceId)?.binding ?? "unavailable",
            seed: seed.grade, draftNotice: seed.draftNotice, store: sourceGrades)
        editor.onClose = { [weak self] in self?.closeGradeEditor(sourceId: sourceId) }
        gradeEditors[sourceId] = editor
        return .opened(editor)
    }

    /// "Close / discard draft": sends nothing.
    func closeGradeEditor(sourceId: String) {
        gradeEditors[sourceId] = nil
    }
}
