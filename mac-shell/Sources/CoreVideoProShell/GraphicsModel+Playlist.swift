// GraphicsModel+Playlist — the Operate playlist (graphics G2, spec 5.4): the show
// document, the row lines (Guest rows expanded per roster source), the row state
// projection, and the inspector edits.
//
// Edits land in the in-memory document AT ONCE (so text typed is already
// committed when Cue / Show / Take reads it) and are saved off the main actor
// after a short debounce. A failed save never blocks a Show: playout reads the
// in-memory rows (spec 5.4).

import Foundation

/// What one playlist line shows (spec 5.4 row states).
enum GraphicsRowState: Equatable {
    case idle                    // Cue, Show
    case cued                    // Show, Clear cue
    case onProgram               // Hide
    case preparing(String)       // disabled, with the core's preparation text
    case failed(String)          // the core's warning; Cue / Show stay available
}

struct GraphicsRowLine: Identifiable, Equatable {
    var row: GraphicsRow
    var entry: GraphicsPlaylistEntry
    var state: GraphicsRowState
    /// A Guest row's line names its source; an empty Guest row says why it has no lines.
    var label: String
    var id: String { entry.instanceId }

    var canCue: Bool {
        if case .preparing = state { return false }
        return state != .onProgram && !row.templateId.isEmpty && !row.needsDataPlayback
    }
    var canShow: Bool { canCue }
    var canHide: Bool { state == .onProgram }
    var canClearCue: Bool { state == .cued }
}

/// GraphicsRowState from the core's instance (nil = not created yet). On Program
/// wins: a graphic on air always offers Hide, whatever its cue copy is doing.
enum GraphicsRowStates {
    static func project(_ instance: GraphicsInstance?) -> GraphicsRowState {
        guard let i = instance else { return .idle }
        if i.isOnProgram { return .onProgram }
        if i.cuePending || i.cuePreparation == "preparing" {
            return .preparing("Preparing…")
        }
        if i.cuePreparation == "failed" || !i.cueWarning.isEmpty || !i.warning.isEmpty {
            let warning = !i.cueWarning.isEmpty ? i.cueWarning : !i.warning.isEmpty ? i.warning : "Graphic preparation failed."
            return .failed(warning)
        }
        if i.isOnPreview { return .cued }
        return .idle
    }
}

extension GraphicsModel {
    static let fKeyHint = "macOS uses F1-F12 for system controls unless \"Use F1, F2, etc. keys as standard function keys\" is on (System Settings > Keyboard)."
    /// Windows PlayOutChoices (GraphicsPlayoutControl.cs:34).
    static let hidesAfterChoices: [(ms: Int, label: String)] = [(0, "Manual"), (8000, "8 s"), (15000, "15 s")]
    static let hotkeys = [""] + (1...12).map { "F\($0)" }

    // ── document ─────────────────────────────────────────────────────────────

    func loadDocument() {
        let store = documentStore
        documentQueue.async {
            let load = store.load()
            DispatchQueue.main.async { [weak self] in self?.adopt(load) }
        }
    }

    func adopt(_ load: GraphicsDocumentLoad) {
        document = load.document
        documentReadOnly = load.readOnly
        documentStatus = load.status
        documentLoaded = true
        reproject()
        resyncDesignsIfDue()   // a core that came up before the show finished loading
    }

    var rows: [GraphicsRow] { document.playlist }
    var hasGuestRow: Bool { rows.contains { $0.kind == GraphicsRow.guest } }
    var anyHotkey: Bool { rows.contains { !$0.hotkey.isEmpty } }

    /// Replace the playlist in memory now; save after `saveDelayNs` off the main actor.
    func setRows(_ next: [GraphicsRow]) {
        guard next.allSatisfy(\.isValid), Set(next.map(\.id)).count == next.count else {
            documentStatus = "The playlist has an invalid or duplicate row. Previous playlist retained."
            return
        }
        document.playlist = next
        reproject()
        scheduleSave()
    }

    func scheduleSave() {
        saveTask?.cancel()
        let delay = saveDelayNs
        saveTask = Task { @MainActor [weak self] in
            if delay > 0 { try? await Task.sleep(nanoseconds: delay) }
            guard let self, !Task.isCancelled else { return }
            await self.saveNow()
        }
    }

    /// Writes the current document; reports a failure on the status line.
    @discardableResult
    func saveNow() async -> Bool {
        let store = documentStore, snapshot = document
        let error: String? = await withCheckedContinuation { done in
            documentQueue.async {
                do { try store.save(snapshot); done.resume(returning: nil) }
                catch { done.resume(returning: GraphicsDocumentStore.message(error)) }
            }
        }
        if let error { documentStatus = "Graphics playlist not saved: " + error; return false }
        return true
    }

    /// D6: Windows' interview default rows, on an explicit button only. Rows the
    /// show already has (by id) are kept as they are.
    func addDefaultRows() {
        guard let state, let defaults = GraphicsDefaultRows.interview(state) else {
            documentStatus = GraphicsDefaultRows.unavailable
            return
        }
        let existing = Set(rows.map(\.id))
        let taken = Set(rows.map(\.hotkey).filter { !$0.isEmpty })
        let added = defaults.filter { !existing.contains($0.id) }.map { row -> GraphicsRow in
            var row = row
            if taken.contains(row.hotkey) { row.hotkey = "" }   // never steal a key the show already uses
            return row
        }
        setRows(rows + added)
    }

    // ── edits (row inspector) ────────────────────────────────────────────────

    func editRow(_ id: String, _ change: (inout GraphicsRow) -> Void) {
        var next = rows
        guard let index = next.firstIndex(where: { $0.id == id }) else { return }
        change(&next[index])
        setRows(next)
    }

    static let nameRequiredText = "A row needs a name of 1 to 128 characters. The previous name is kept."

    /// The Name field may pass through empty while typing: an invalid name is not
    /// stored and the message says exactly why (never "invalid or duplicate row").
    func setName(_ id: String, _ name: String) {
        guard (1...128).contains(name.utf16.count) else { documentStatus = Self.nameRequiredText; return }
        if documentStatus == Self.nameRequiredText { documentStatus = "" }
        editRow(id) { $0.name = name }
    }
    func setField(_ id: String, _ key: String, _ value: Any) { editRow(id) { var f = $0.fields; f[key] = value; $0.fields = f } }
    func setHidesAfter(_ id: String, _ ms: Int) { editRow(id) { $0.playOutAfterMs = ms } }
    func setFollowSources(_ id: String, _ on: Bool) { editRow(id) { $0.json["followSources"] = on } }

    /// One row per key: giving a row a key takes it from any other row (Windows
    /// GraphicsPlayoutActions / StudioControlSurface :528).
    func setHotkey(_ id: String, _ key: String) {
        setRows(rows.map { row in
            var row = row
            if row.id == id { row.hotkey = key } else if !key.isEmpty && row.hotkey == key { row.hotkey = "" }
            return row
        })
    }

    /// A typed template variable (Windows commits through WithText): clamped to the box's MaxLength
    /// (the field's own, at most 4096, else the core's 512), stored at once like every inspector edit.
    func setTypedText(_ id: String, _ field: GraphicsTypeableField, _ text: String) {
        let cap = field.maxLength.map { $0 > 0 ? min($0, 4096) : GraphicsRowFields.coreStringLimit } ?? GraphicsRowFields.coreStringLimit
        setField(id, field.id, GraphicsRowFields.clamp(text, cap))
    }

    /// The row's Title line (MaxLength 512).
    func setTitleLine(_ id: String, key: String, _ text: String) {
        setField(id, key, GraphicsRowFields.clamp(text, GraphicsRowFields.titleLineLimit))
    }

    // ── names list (GraphicsPlayoutControl.NamesList.cs:40-63) ──────────────

    static let noNamesText = "No names found. Use Name, Title and optional variable IDs in the header."

    /// The variable ids a names-list header may name: the row's eligible template fields (Windows passes
    /// TemplateFieldsOf(...).Where(GraphicsMappedValues.Eligible); a compatibility template has none).
    func namesListVariableIds(_ row: GraphicsRow) -> [String] {
        GraphicsRowFields.templateFields(row, state: state, pins: document.templates).filter(GraphicsRowFields.eligibleForMapping).map(\.id)
    }

    /// Saves a row's names list from the editor's text. nil = saved; "" = refused silently (the row is
    /// gone, or its template changed since the editor opened); else the message to show.
    func saveNamesList(rowId: String, text: String, templateId: String, templateVersion: Int) -> String? {
        guard let current = rows.first(where: { $0.id == rowId }), current.templateId == templateId,
              current.templateVersion == templateVersion else { return "" }
        let variants = GraphicsNamesList.parse(text, existing: current.variants, variableIds: namesListVariableIds(current))
        if variants.isEmpty && !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { return Self.noNamesText }
        var updated = current
        updated.json["variants"] = variants
        updated.kind = !variants.isEmpty ? GraphicsRow.title : current.followSources ? GraphicsRow.guest : current.kind
        let before = document.playlist
        setRows(rows.map { $0.id == rowId ? updated : $0 })
        return document.playlist == before && updated != current ? documentStatus : nil
    }

    // ── selection and inspector (GraphicsPlayoutControl.Select / RefreshInspector) ──

    nonisolated static func selectionKey(rowId: String, sourceId: String) -> String { sourceId.isEmpty ? rowId : rowId + "|" + sourceId }

    func select(_ key: String) { if selectedKey != key { selectedKey = key } }

    /// A card shows the accent border when it, or (for a Guest / names-list row) one of its lines, is selected.
    func isCardSelected(_ row: GraphicsRow) -> Bool {
        let multi = row.kind == GraphicsRow.guest || row.hasVariants
        return selectedKey == row.id || (multi && selectedKey.hasPrefix(row.id + "|"))
    }

    /// Windows Caption(row) (:181-184).
    nonisolated static func caption(_ row: GraphicsRow) -> String {
        switch row.kind {
        case GraphicsRow.guest: return row.followSources ? "Guest · names from Sources" : "Guest"
        case GraphicsRow.title: return row.hasVariants ? "Names list · \(row.variants.count) · hotkey steps through" : "Title"
        case GraphicsRow.logo: return "Logo"
        case GraphicsRow.liveBug: return "Live bug"
        case GraphicsRow.sponsor: return row.templateId.isEmpty && row.image == nil ? "Sponsor card · no image yet" : "Sponsor card"
        default: return row.kind
        }
    }

    /// The row and (for "rowId|sourceId") the line a selection key names.
    func selection(_ key: String) -> (row: GraphicsRow, entry: GraphicsPlaylistEntry?)? {
        let rowId = String(key.split(separator: "|", maxSplits: 1, omittingEmptySubsequences: false).first ?? "")
        guard let row = rows.first(where: { $0.id == rowId }) else { return nil }
        guard key.count > rowId.count else { return (row, nil) }
        let sourceId = String(key.dropFirst(rowId.count + 1))
        let entries = row.hasVariants ? GraphicsPlayout.variantEntries(row) : row.kind == GraphicsRow.guest ? guestEntries(row) : []
        return (row, entries.first { $0.sourceId == sourceId })
    }

    func typeableFields(_ row: GraphicsRow) -> [GraphicsTypeableField] {
        GraphicsRowFields.typeable(row, state: state, pins: document.templates,
                                   titleKey: row.kind == GraphicsRow.guest ? nil : GraphicsRowFields.titleFieldKey(row))
    }

    /// The inspector, top to bottom, in Windows' RefreshInspector order (:598-692). Empty = no row
    /// selected (the view shows the hint). Sources-column pickers are not on the Mac (owner decision 6).
    func inspectorSections(for key: String) -> [GraphicsInspectorSection] {
        guard let (row, entry) = selection(key) else { return [] }
        var out: [GraphicsInspectorSection] = [
            .header(title: entry?.label ?? row.name, caption: entry.map { "Guest · " + $0.sourceId } ?? Self.caption(row)),
            .name,
        ]
        if row.kind == GraphicsRow.guest && row.followSources {
            out.append(.titleLine(key: "title", readOnly: true))
        } else if let key = GraphicsRowFields.titleFieldKey(row) {
            out.append(.titleLine(key: key, readOnly: false))
        }
        out += typeableFields(row).map(GraphicsInspectorSection.typeable)
        if row.kind == GraphicsRow.guest { out.append(.fromSources) }
        if row.kind == GraphicsRow.guest || row.hasVariants { out.append(.namesList) }
        if row.dataBinding != nil { out.append(.data) }
        out += [.hidesAfter, .hotkey, .hotkeyHint, .openInDesign]
        if row.templateId.isEmpty && row.image == nil { out.append(.noGraphicWarning) }
        return out
    }

    // ── lines ────────────────────────────────────────────────────────────────

    /// Guest entries for every Guest row (one per bound roster source, owner
    /// decision: roster/device name, EMPTY title until the Sources rebuild).
    func guestEntries(_ row: GraphicsRow) -> [GraphicsPlaylistEntry] {
        GraphicsPlayout.guestEntries(row, rosterSlots()) { _, name in (name, "") }
    }

    func lines() -> [GraphicsRowLine] {
        var lines: [GraphicsRowLine] = []
        for row in rows {
            let entries = row.hasVariants ? GraphicsPlayout.variantEntries(row)
                : row.kind == GraphicsRow.guest ? guestEntries(row) : [GraphicsPlayout.entry(for: row)]
            for entry in entries {
                let lineState = row.needsDataPlayback ? .failed(GraphicsPlayout.unsupportedDataText(row.name))
                    : GraphicsRowStates.project(state?.instance(entry.instanceId))
                lines.append(GraphicsRowLine(row: row, entry: entry, state: lineState,
                                             label: row.kind == GraphicsRow.guest || row.hasVariants ? row.name + " · " + entry.label : row.name))
            }
        }
        return lines
    }

    /// The Guest entry for a canonical source id (the Program subject).
    func guestEntry(for sourceId: String?) -> GraphicsPlaylistEntry? {
        guard let sourceId, let guest = rows.first(where: { $0.kind == GraphicsRow.guest }) else { return nil }
        return guestEntries(guest).first { $0.sourceId == sourceId }
    }

    // ── row actions ──────────────────────────────────────────────────────────

    func cue(_ entry: GraphicsPlaylistEntry) { perform { await $0.cue(entry) } }
    func show(_ entry: GraphicsPlaylistEntry) { perform { await $0.show(entry) } }
    func hide(entry: GraphicsPlaylistEntry) { hide(entry.instanceId) }

    // ── countdown transport (GraphicsPlayoutControl.cs:454-465) ──────────────

    /// Start / Pause / Resume / Reset appear only on rows whose clock is a countdown.
    nonisolated static func showsClockTransport(_ row: GraphicsRow) -> Bool { row.clock?["mode"] as? String == "countdown" }

    /// The row's own instance (a clock row is a single-line row).
    func clockInstance(_ row: GraphicsRow) -> GraphicsInstance? {
        state?.instance(GraphicsPlayoutIds.instanceId(rowId: row.id, sourceId: ""))
    }

    @discardableResult
    func clock(rowId: String, action: String) -> Task<GraphicsOperationResult, Never> {
        guard rows.contains(where: { $0.id == rowId }) else {
            // Windows StudioControlSurface (:498) refuses an unknown row before playout runs.
            let refused = GraphicsOperationResult.refused("That playlist row does not exist.")
            report(refused)
            return Task { refused }
        }
        return perform { await $0.clock(GraphicsPlaylistEntry(rowId: rowId, sourceId: "", name: "", title: ""), action: action) }
    }
}

/// The countdown row's readout (GraphicsPlayoutControl.cs:458): the core's clock text and state when
/// the instance has a clock, else the row's duration and "not cued". C#'s m\:ss TimeSpan format is kept
/// byte for byte, including its quirk: the minutes component is 0-59, so hours are dropped (3600 s reads
/// "0:00"; plan D3, owner decision 7: noted for a Windows fix, not diverged).
enum GraphicsClockReadout {
    static func text(row: GraphicsRow, instance: GraphicsInstance?) -> String {
        if let clock = instance?.clock { return "\(clock.text) · \(clock.state)" }
        return minutesSeconds((row.clock?["durationSeconds"] as? NSNumber)?.intValue ?? 300) + " · not cued"
    }

    /// TimeSpan.FromSeconds(s).ToString(@"m\:ss").
    static func minutesSeconds(_ totalSeconds: Int) -> String {
        let minutes = (totalSeconds / 60) % 60, seconds = totalSeconds % 60
        return "\(minutes):" + (seconds < 10 ? "0" : "") + "\(seconds)"
    }
}
