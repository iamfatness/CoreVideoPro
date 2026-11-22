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

    var canCue: Bool { if case .preparing = state { return false }; return state != .onProgram && !row.templateId.isEmpty }
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
    static let hidesAfterChoices: [(ms: Int, label: String)] = [(0, "Off"), (8000, "8 s"), (15000, "15 s")]
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

    func setName(_ id: String, _ name: String) { editRow(id) { $0.name = name } }
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

    /// Template string fields the inspector types (the template's `fields`).
    func editableFields(_ row: GraphicsRow) -> [GraphicsTemplateField] {
        guard let template = state?.template(row.templateId, row.templateVersion) else {
            return GraphicsPlayout.curated(row.templateId) ? [GraphicsTemplateField(id: "name", type: "string"), GraphicsTemplateField(id: "title", type: "string")] : []
        }
        if template.runtime != "declarative" {
            return [GraphicsTemplateField(id: "name", type: "string"), GraphicsTemplateField(id: "title", type: "string")]
        }
        return template.fields.filter { $0.type == "string" || $0.type == "number" }
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
            let entries = row.kind == GraphicsRow.guest ? guestEntries(row) : [GraphicsPlayout.entry(for: row)]
            for entry in entries {
                lines.append(GraphicsRowLine(row: row, entry: entry, state: GraphicsRowStates.project(state?.instance(entry.instanceId)),
                                             label: row.kind == GraphicsRow.guest ? row.name + " · " + entry.label : row.name))
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
}
