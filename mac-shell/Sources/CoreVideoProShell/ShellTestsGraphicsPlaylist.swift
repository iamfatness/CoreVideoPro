// Graphics G2 Task 8 — Operate playlist, row inspector, Guest rows, default rows.

import Foundation

extension ShellTests {
    static var graphicsPlaylistCases: [(String, () -> Void)] {
        [
            ("graphics-playlist/row-state-table", testGraphicsRowStateTable),
            ("graphics-playlist/default-rows-equal-windows", testGraphicsDefaultRowsEqualWindows),
            ("graphics-playlist/add-default-rows-keeps-existing", testGraphicsAddDefaultRowsKeepsExisting),
            ("graphics-playlist/guest-rows-expand-with-empty-title", testGraphicsGuestRowsExpand),
            ("graphics-playlist/hotkey-one-row-per-key", testGraphicsHotkeyOneRowPerKey),
            ("graphics-playlist/typed-text-reaches-show-despite-failed-save", testGraphicsTypedTextReachesShow),
            ("graphics-playlist/edits-persist", testGraphicsPlaylistEditsPersist),
            ("graphics-playlist/app-slots-map-to-canonical-ids", testGraphicsAppSlotsMapToCanonicalIds),
        ]
    }

    static func testGraphicsRowStateTable() {
        func instance(_ apply: (inout GraphicsInstance) -> Void) -> GraphicsInstance { var i = GraphicsInstance(); i.instanceId = "x"; apply(&i); return i }
        let cases: [(String, GraphicsInstance?, GraphicsRowState)] = [
            ("no instance", nil, .idle),
            ("created, hidden", instance { $0.revision = 2 }, .idle),
            ("cued", instance { $0.revision = 2; $0.cueRevision = 2 }, .cued),
            ("cue preparing", instance { $0.cueRevision = 2; $0.cuePending = true }, .preparing("Preparing…")),
            ("cue failed", instance { $0.cueRevision = 2; $0.cuePreparation = "failed"; $0.cueWarning = "font missing" }, .failed("font missing")),
            ("instance warning", instance { $0.warning = "template gone" }, .failed("template gone")),
            ("on program", instance { $0.programRevision = 2; $0.visibility = "on-air" }, .onProgram),
            ("on program AND a failed cue still offers Hide",
             instance { $0.programRevision = 2; $0.visibility = "on-air"; $0.cueRevision = 3; $0.cuePreparation = "failed" }, .onProgram),
            ("building out is not on Program", instance { $0.programRevision = 2; $0.visibility = "building-out" }, .idle),
        ]
        for (what, i, want) in cases { expectEqual(GraphicsRowStates.project(i), want, "row state: \(what)") }
        let row = GraphicsRow(json: ["id": "t", "name": "T", "templateId": "centered-title", "templateVersion": 1])
        let entry = GraphicsPlayout.entry(for: row)
        let onAir = GraphicsRowLine(row: row, entry: entry, state: .onProgram, label: "T")
        expect(onAir.canHide && !onAir.canShow && !onAir.canCue, "on Program: Hide only")
        let idle = GraphicsRowLine(row: row, entry: entry, state: .idle, label: "T")
        expect(idle.canCue && idle.canShow && !idle.canHide && !idle.canClearCue, "idle: Cue, Show")
        let cued = GraphicsRowLine(row: row, entry: entry, state: .cued, label: "T")
        expect(cued.canShow && cued.canClearCue && !cued.canHide, "cued: Show, Clear cue")
        let preparing = GraphicsRowLine(row: row, entry: entry, state: .preparing("x"), label: "T")
        expect(!preparing.canShow && !preparing.canCue, "preparing: disabled")
        let placeholder = GraphicsRowLine(row: GraphicsRow(json: ["id": "s", "name": "S"]), entry: entry, state: .idle, label: "S")
        expect(!placeholder.canShow, "a row with no graphic cannot Show")
    }

    static func testGraphicsDefaultRowsEqualWindows() {
        let catalog = GraphicsState(node: ["schemaVersion": 1, "coreEpoch": "e", "templates": realCatalogTemplates])
        let mac = GraphicsDefaultRows.interview(catalog) ?? []
        let windows = interviewRows()
        expectEqual(mac.count, 5, "five rows")
        for (m, w) in zip(mac, windows) {
            expect(jsonEqual(m.json, w.json), "default row \(w.id) equals Windows' DefaultPlaylistFor, key for key")
        }
        expect(GraphicsDefaultRows.interview(GraphicsState(node: ["schemaVersion": 1, "coreEpoch": "e"])) == nil,
               "no catalog: unavailable, never invented")
    }

    @MainActor
    static func testGraphicsAddDefaultRowsKeepsExisting() {
        let (model, _) = playoutModel([])
        model.document.playlist = [GraphicsRow(json: ["id": "title", "name": "Mine", "hotkey": "F3"])]
        model.addDefaultRows()
        expectEqual(model.rows.map(\.id), ["title", "guest", "logo", "live-bug", "sponsor"], "existing rows kept, the rest appended")
        expectEqual(model.rows.first?.name, "Mine", "an existing row is never overwritten")
        expectEqual(model.rows.first { $0.id == "logo" }?.hotkey, "", "a default never steals a key the show already uses")
        expectEqual(model.rows.first { $0.id == "guest" }?.hotkey, "F1", "free keys are kept")
    }

    @MainActor
    static func testGraphicsGuestRowsExpand() {
        let (model, _) = playoutModel(interviewRows())
        model.document.playlist = interviewRows()
        model.rosterSlots = {
            [GraphicsRosterSlot(slotNumber: 2, sourceId: "capture:cam", derivedName: "Desk cam"),
             GraphicsRosterSlot(slotNumber: 1, sourceId: "zoom:100", derivedName: "Ada"),
             GraphicsRosterSlot(slotNumber: 3, sourceId: nil, derivedName: "")]
        }
        let guests = model.lines().filter { $0.row.id == "guest" }
        expectEqual(guests.map(\.entry.sourceId), ["zoom:100", "capture:cam"], "one line per bound source, in slot order")
        expectEqual(guests.map(\.entry.label), ["Ada", "Desk cam"], "named by the roster or device name")
        expectEqual(guests.map(\.entry.title), ["", ""], "with an EMPTY title (owner decision until the Sources rebuild)")
        expectEqual(guests.map(\.label), ["Guest name · Ada", "Guest name · Desk cam"], "line labels")
        expectEqual(model.guestEntry(for: "capture:cam")?.instanceId, GraphicsPlayoutIds.instanceId(rowId: "guest", sourceId: "capture:cam"),
                    "the Program subject finds its Guest entry")
        model.rosterSlots = { [] }
        expect(model.lines().filter { $0.row.id == "guest" }.isEmpty, "no bound slots: no guest lines")
    }

    @MainActor
    static func testGraphicsHotkeyOneRowPerKey() {
        let (model, _) = playoutModel([])
        model.document.playlist = interviewRows()
        model.setHotkey("logo", "F1")
        expectEqual(model.rows.first { $0.id == "logo" }?.hotkey, "F1", "the key moves to the edited row")
        expectEqual(model.rows.first { $0.id == "guest" }?.hotkey, "", "and leaves the row that had it")
        expectEqual(model.rows.filter { $0.hotkey == "F1" }.count, 1, "one row per key")
        model.setHotkey("logo", "")
        expectEqual(model.rows.filter { $0.hotkey == "F1" }.count, 0, "clearing frees the key")
    }

    /// Spec 5.4: text typed is committed before Show, and a failed save does not
    /// block it. The store is read-only (a newer schema on disk), so every save fails.
    @MainActor
    static func testGraphicsTypedTextReachesShow() {
        let rows = interviewRows()
        let (model, core) = playoutModel(rows)
        let path = model.documentStore.path
        graphicsWrite(#"{"schemaVersion":9}"#, path)
        model.adopt(model.documentStore.load())
        expect(model.documentReadOnly, "the store is read-only")
        model.document.playlist = rows          // what the operator sees
        model.setField("title", "heading", "Tonight's show")
        let entry = GraphicsPlayout.entry(for: model.rows.first { $0.id == "title" }!)
        let result = awaitMain(5) { await model.playout.show(entry) }
        spin(until: { model.documentStatus.hasPrefix("Graphics playlist not saved") }, limit: 2)
        expectEqual(result?.accepted, true, "Show still plays")
        let create = core.commands.first { $0["type"] as? String == "graphics-instance-create" }?["graphics"] as? JSONObject
        expectEqual((create?["fields"] as? JSONObject)?["heading"] as? String, "Tonight's show", "the typed text is in the create body")
        expect(model.documentStatus.hasPrefix("Graphics playlist not saved"), "and the failed save is reported")
    }

    @MainActor
    static func testGraphicsPlaylistEditsPersist() {
        let (model, _) = playoutModel([])
        model.adopt(model.documentStore.load())
        model.document.playlist = interviewRows()
        model.setName("title", "Late show")
        model.setHidesAfter("title", 15000)
        spin(until: { (graphicsReadJSON(model.documentStore.path)?["playlist"] as? [JSONObject])?.count == 5 }, limit: 3)
        let saved = GraphicsDocumentStore(path: model.documentStore.path).load().document
        expectEqual(saved.row("title")?.name, "Late show", "renamed row saved")
        expectEqual(saved.row("title")?.playOutAfterMs, 15000, "hides-after saved")
        model.setName("title", "")
        expectEqual(model.rows.first { $0.id == "title" }?.name, "Late show", "an invalid edit (empty name) is refused")
        expectEqual(model.documentStatus, GraphicsModel.nameRequiredText, "and says exactly why (review round)")
    }

    @MainActor
    static func testGraphicsAppSlotsMapToCanonicalIds() {
        let app = AppModel()
        app.slots[0].kind = "zoom"; app.slots[0].sourceId = "16778240"; app.slots[0].name = "Ada"
        app.slots[1].kind = "capture"; app.slots[1].sourceId = "cam-1"; app.slots[1].sourceName = "FaceTime HD"
        let slots = app.graphics.rosterSlots()
        expectEqual(slots.first?.sourceId, "zoom:16778240", "a Zoom slot is zoom:<pid>")
        expectEqual(slots.first?.derivedName, "Ada", "named by the slot name")
        expectEqual(slots[1].sourceId, "capture:cam-1", "a capture slot is capture:<id>")
        expectEqual(slots[1].derivedName, "FaceTime HD", "falls back to the source's own name")
        expect(slots[2].sourceId == nil, "an unassigned slot has no source")
        expect(app.graphics.documentStore.path.contains("cvp-shell-tests-graphics"), "the harness never touches the operator's show (D4)")
    }
}
