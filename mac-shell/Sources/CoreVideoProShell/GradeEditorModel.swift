// The per-source basic grade editor (grading plan 2026-11-16, Task 6). One model per open
// editor window, at most three (the Windows rule). Every send goes through SourceGradeStore,
// which keeps one apply in flight per source; nothing here talks to the core directly.

import CoreGraphics
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
    // G2 (Task 10): the advanced document, its panel and undo/redo.
    @Published private(set) var advancedExpanded = false      // "Expand / collapse grade"
    @Published private(set) var document: AdvancedGradeDocument?
    @Published var selectedAdjustmentId: String?
    @Published private(set) var history = GradeHistory()
    /// The grade after the last recorded edit: what an undo step stores.
    private var recorded: SourceColorGrade
    /// Set only by Reset: the next send asks the core to forget this source's grade (follow the
    /// Program Look). Any other edit cancels it, so a neutral grade made by hand stays a real grade.
    private var clearRequested = false

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
                         temperature: temperature, advanced: document)
    }

    static let draftStatus = "Draft — Apply Live sends this grade to the show."   // VM.Advanced.cs:75
    static let adjustmentLimitText = "A grade can contain up to eight adjustments." // :119
    /// GradeAdjustmentControl.cs:46, with Command in place of Ctrl (spec §5.4).
    static let curveHelp = "Click to add; drag to shape. Home/End selects an endpoint; ⌘+arrows selects a point. "
        + "Arrows adjust by 0.1%; Shift by 1%. Delete removes an interior point. Endpoints move vertically. Linear interpolation."
    static let curveChannels = ["Master", "Red", "Green", "Blue"]
    static let maxCubeFileBytes = 2_000_000                       // ColorGradeEditorWindow.Advanced.cs:97

    // G3 (Phase 2 Task 2): the curve channel being edited and its selected point.
    @Published var curveChannel = 0 { didSet { if curveChannel != oldValue { curveSelection = -1 } } }
    @Published private(set) var curveSelection = -1

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
        document = seed.advanced
        selectedAdjustmentId = seed.advanced?.operations.first?.id
        recorded = seed
        // A saved Zoom grade is a DRAFT until Apply Live confirms the binding (Grading.cs:87-90).
        liveEditing = draftNotice == nil
        status = draftNotice ?? Self.initialStatus
    }

    /// After any control change: one undo step (per gesture), then, when live, the store sends
    /// it (one in flight per source).
    func edited() { edited(clearing: false) }

    private func edited(clearing: Bool) {
        clearRequested = clearing
        rememberEdit()
        guard liveEditing else { return }
        Task { _ = await send() }
    }

    private func rememberEdit() {
        let now = currentGrade
        guard now != recorded else { return }
        history.remember(recorded)
        recorded = now
    }

    /// A slider drag is ONE undo step (VM.Advanced.cs:16-23).
    func beginGesture() { history.beginGesture() }
    func endGesture() { history.endGesture() }

    func applyLive() async { _ = await send() }

    /// True only when the core acknowledged it; then the editor closes.
    func applyAndDone() async -> Bool {
        let applied = await send()
        if applied { onClose?() }
        return applied
    }

    /// Look "none", every axis 0 and no document, sent as a clear: the source follows the
    /// Program Look again (D6 override). A neutral grade built by hand is not a clear.
    func reset() {
        lut = "none"; exposure = 0; contrast = 0; saturation = 0; temperature = 0
        document = nil; selectedAdjustmentId = nil
        edited(clearing: true)
    }

    // ── G2: the advanced panel ───────────────────────────────────────────────

    var indicator: String { document?.indicator ?? "No advanced adjustments" }
    var selectedAdjustment: GradeOperation? {
        document?.operations.first { $0.id == selectedAdjustmentId }
    }

    /// Expanding makes edits a draft (VM.Advanced.cs:73-77).
    func setAdvancedExpanded(_ on: Bool) {
        advancedExpanded = on
        if on { liveEditing = false; status = Self.draftStatus }
    }

    func addPrimaries() { addAdjustment(kind: "primaries") }
    /// "+ Curves" (VM.Advanced.cs:115-121).
    func addCurves() { addAdjustment(kind: "curves") }

    private func addAdjustment(kind: String) {
        let operations = document?.operations ?? []
        guard operations.count < 8 else { status = Self.adjustmentLimitText; return }
        var added = GradeOperation(); added.kind = kind
        if editDocument(withOperations: operations + [added]) { selectedAdjustmentId = added.id; curveSelection = -1 }
    }

    // ── G3: curves ───────────────────────────────────────────────────────────

    /// The selected curves adjustment's channel as an editing value (nil when none is selected).
    var curveEditing: GradeCurveEditing? {
        guard let op = selectedAdjustment, op.kind == "curves", op.curves.indices.contains(curveChannel) else { return nil }
        return GradeCurveEditing(points: op.curves[curveChannel], selectedIndex: curveSelection)
    }

    /// VM.Advanced.cs:107-112 SetCurve: one validated edit (a refusal keeps the draft and says why).
    func setCurve(channel: Int, points: [GradeCurvePoint]) {
        guard (0..<4).contains(channel), selectedAdjustment?.kind == "curves" else { return }
        editSelected { $0.curves[channel] = points }
    }

    private func applyCurve(_ change: (inout GradeCurveEditing) -> Bool) {
        guard var editing = curveEditing else { return }
        let changed = change(&editing)
        curveSelection = editing.selectedIndex
        if changed { setCurve(channel: curveChannel, points: editing.points) }
    }
    func curvePress(at point: CGPoint, size: CGSize) { applyCurve { $0.press(at: point, size: size) } }
    func curveDrag(to point: CGPoint, size: CGSize) { applyCurve { $0.drag(to: point, size: size) } }
    func curveKey(_ key: CurveKey, shift: Bool, command: Bool) { applyCurve { $0.key(key, shift: shift, command: command) } }
    func curveRemoveSelected() { applyCurve { $0.removeSelected() } }
    /// "Reset channel".
    func curveResetChannel() { applyCurve { editing in editing.reset(); return true } }
    /// The "Point input" / "Point output" numbers (GradeAdjustmentControl.cs:50-55).
    func curveSetSelectedPoint(x: Double, y: Double) { applyCurve { $0.setSelected(x: x, y: y) } }

    /// "Import LUT" (VM.Advanced.cs:181-190): parse first, then the eight-adjustment limit, then add a
    /// named cube adjustment carrying the exact text and its SHA-256, selected. Refusals keep the draft.
    func importCube(text: String, name: String) {
        do { _ = try CubeLutParser.parse(text) }
        catch let error as GradeValidationError { status = error.message; return }
        catch { status = error.localizedDescription; return }
        let operations = document?.operations ?? []
        guard operations.count < 8 else { status = Self.adjustmentLimitText; return }
        var op = GradeOperation()
        op.kind = "cube"; op.name = name; op.cubeText = text; op.cubeSha256 = GradeDocumentRef.sha256Hex(text)
        if editDocument(withOperations: operations + [op]) { selectedAdjustmentId = op.id }
    }

    func moveSelected(_ direction: Int) {
        guard var operations = document?.operations,
              let index = operations.firstIndex(where: { $0.id == selectedAdjustmentId }) else { return }
        let target = index + direction
        guard operations.indices.contains(target) else { return }
        operations.swapAt(index, target)
        editDocument(withOperations: operations)
    }

    func duplicateSelected() {
        guard let operations = document?.operations, let selected = selectedAdjustment else { return }
        guard operations.count < 8 else { status = Self.adjustmentLimitText; return }
        var copy = selected; copy.id = GradeOperation.newId()
        var next = operations
        next.insert(copy, at: (operations.firstIndex { $0.id == selected.id } ?? operations.count - 1) + 1)
        if editDocument(withOperations: next) { selectedAdjustmentId = copy.id }
    }

    func removeSelected() {
        guard let operations = document?.operations, let selected = selectedAdjustmentId else { return }
        let remaining = operations.filter { $0.id != selected }
        if editDocument(withOperations: remaining) { selectedAdjustmentId = remaining.first?.id }
    }

    /// Validates the edited operation first; a refusal leaves the draft unchanged (VM.Advanced.cs:80-85).
    func editSelected(_ change: (inout GradeOperation) -> Void) {
        guard let operations = document?.operations,
              let index = operations.firstIndex(where: { $0.id == selectedAdjustmentId }) else { return }
        var next = operations
        change(&next[index])
        editDocument(withOperations: next)
    }

    func setBypass(_ on: Bool) {
        var next = document ?? AdvancedGradeDocument(); next.bypass = on
        editDocument(next)
    }

    func setIntensity(_ value: Double) {
        var next = document ?? AdvancedGradeDocument(); next.intensity = value
        editDocument(next)
    }

    /// An empty document, one history entry.
    func resetAdvanced() { editDocument(AdvancedGradeDocument()) }

    @discardableResult
    private func editDocument(withOperations operations: [GradeOperation]) -> Bool {
        var next = document ?? AdvancedGradeDocument()
        next.operations = operations
        return editDocument(next)
    }

    @discardableResult
    private func editDocument(_ next: AdvancedGradeDocument) -> Bool {
        do { try next.validate() } catch let error as GradeValidationError { status = error.message; return false }
        catch { status = error.localizedDescription; return false }
        document = next
        if !(next.operations.contains { $0.id == selectedAdjustmentId }) { selectedAdjustmentId = next.operations.first?.id }
        edited()
        return true
    }

    func undo() {
        guard let previous = history.undo(from: currentGrade) else { return }
        restore(previous)
    }

    func redo() {
        guard let next = history.redo(from: currentGrade) else { return }
        restore(next)
    }

    /// Puts a whole grade back without recording it; sends it once when live.
    private func restore(_ grade: SourceColorGrade) {
        lut = grade.lut; exposure = grade.exposure; contrast = grade.contrast
        saturation = grade.saturation; temperature = grade.temperature
        document = grade.advanced
        if !(grade.advanced?.operations.contains { $0.id == selectedAdjustmentId } ?? false) {
            selectedAdjustmentId = grade.advanced?.operations.first?.id
        }
        recorded = currentGrade
        clearRequested = false
        if liveEditing { Task { _ = await send() } }
    }

    /// "Copy grade": the same .cvgrade text a preset file holds.
    func copyGrade() -> String { (try? GradePreset.encode(currentGrade)) ?? "" }

    /// "Paste grade" / "Load preset": one undo step. A preset replaces the whole grade and, if
    /// live editing was on, is applied ONCE with the final values (live is suspended for the
    /// restore, VM.Advanced.cs:169-172). A bare document replaces only the document.
    func paste(_ text: String) -> Bool {
        let pasted: GradePreset.Pasted
        do { pasted = try GradePreset.decode(text) }
        catch let error as GradeValidationError { status = error.message; return false }
        catch { status = error.localizedDescription; return false }
        switch pasted {
        case .grade(let grade):
            let live = liveEditing
            liveEditing = false
            history.remember(currentGrade)
            restore(grade)
            liveEditing = live      // turning live back on sends the final grade once
        case .document(let pastedDocument):
            history.remember(currentGrade)
            var grade = currentGrade; grade.advanced = pastedDocument
            restore(grade)
        }
        return true
    }

    @discardableResult
    private func send() async -> Bool {
        isApplying = true
        let before = status
        status = "Applying grade…"                                         // Apply.cs:46
        let clear = clearRequested
        clearRequested = false
        let outcome = await store.apply(currentGrade, sourceId: sourceId, authoredFor: binding, clear: clear)
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
