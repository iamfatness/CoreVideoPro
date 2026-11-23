// Graphics G4-G7 Tasks 2-3 — the names-list editor: parser/writer vectors
// (contracts/graphics-shell/names-editor.vectors.json, from
// GraphicsShellVectorTests.NamesEditorVectors) and the inspector's save rules.

import Foundation

extension ShellTests {
    static var graphicsNamesCases: [(String, () -> Void)] {
        [
            ("graphics-names/editor-vectors", testNamesEditorVectorsReplay),
            ("graphics-names/save-turns-guest-into-names-list", testSaveNamesListTurnsAGuestRowIntoATitleNamesList),
            ("graphics-names/clearing-returns-following-row-to-guest", testClearingTheListReturnsAFollowingRowToGuest),
            ("graphics-names/save-refused-when-template-changed", testSaveRefusedWhenTheTemplateChanged),
            ("graphics-names/no-names-message", testNoNamesMessage),
            ("graphics-names/cells-only-for-eligible-fields", testCellsOnlyForEligibleFields),
            ("graphics-names/re-save-keeps-on-air-ids", testNamesListReSaveKeepsOnAirInstanceIds),
        ]
    }

    static func testNamesEditorVectorsReplay() {
        var checked = 0
        for vector in loadGraphicsVectors("names-editor") {
            let id = vector["id"] as? String ?? "?"
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            switch vector["operation"] as? String {
            case "parse":
                let got = GraphicsNamesList.parse(input["text"] as? String ?? "", existing: input["existing"] as? [JSONObject] ?? [],
                                                  variableIds: input["variableIds"] as? [String])
                let want = expected["variants"] as? [JSONObject] ?? []
                expectEqual(got.count, want.count, "\(id): count")
                for (g, w) in zip(got, want) { expect(jsonEqual(g, w), "\(id): \(g) != \(w)") }
            case "text":
                expectEqual(GraphicsNamesList.text(input["variants"] as? [JSONObject] ?? []), expected["text"] as? String ?? "", "\(id)")
            case "newRowId":
                expectEqual(GraphicsNamesList.newRowId(input["name"] as? String ?? "", taken: input["taken"] as? [String] ?? []),
                            expected["id"] as? String ?? "", "\(id)")
            default: expect(false, "\(id): unknown operation")
            }
            checked += 1
        }
        expect(checked >= 28, "every names-editor vector replayed (\(checked))")
    }

    @MainActor
    static func testSaveNamesListTurnsAGuestRowIntoATitleNamesList() {
        let (model, _) = playoutModel(windowsShowRows())
        guard let guest = model.rows.first(where: { $0.id == "guest" }) else { expect(false, "guest row"); return }
        let result = model.saveNamesList(rowId: "guest", text: "Ada Lovelace, Mathematician\nBo Diddley,",
                                         templateId: guest.templateId, templateVersion: guest.templateVersion)
        expect(result == nil, "saved (\(result ?? "nil"))")
        guard let saved = model.rows.first(where: { $0.id == "guest" }) else { expect(false, "row kept"); return }
        expectEqual(saved.kind, GraphicsRow.title, "a list turns the row into a names list (kind title)")
        expectEqual(saved.variants.map { $0["name"] as? String ?? "" }, ["Ada Lovelace", "Bo Diddley"], "names from the text")
        expectEqual(saved.variants.map { $0["title"] as? String ?? "" }, ["Mathematician", ""], "titles from the text")
        expect(saved.followSources, "followSources is not touched")
        _ = awaitMain { await model.saveNow() }
        let disk = GraphicsDocumentStore(path: model.documentStore.path).load().document.row("guest")
        expectEqual(disk?.kind, GraphicsRow.title, "kind on disk")
        expect(disk?.followSources == true, "followSources unchanged on disk")
    }

    @MainActor
    static func testClearingTheListReturnsAFollowingRowToGuest() {
        var rows = windowsShowRows()
        guard let index = rows.firstIndex(where: { $0.id == "guest" }) else { expect(false, "guest row"); return }
        rows[index].kind = GraphicsRow.title
        rows[index].json["variants"] = [["id": "ada", "name": "Ada", "title": "", "cells": [:] as JSONObject]] as [JSONObject]
        let (model, _) = playoutModel(rows)
        let row = model.rows[index]
        expect(model.saveNamesList(rowId: "guest", text: "  \n", templateId: row.templateId, templateVersion: row.templateVersion) == nil, "blank text saves")
        expectEqual(model.rows[index].kind, GraphicsRow.guest, "a cleared list on a row following Sources is a Guest row again")
        expectEqual(model.rows[index].variants.count, 0, "no variants")
    }

    @MainActor
    static func testSaveRefusedWhenTheTemplateChanged() {
        let (model, _) = playoutModel(windowsShowRows())
        let before = model.document.playlist
        let guest = model.rows.first { $0.id == "guest" }!
        expectEqual(model.saveNamesList(rowId: "guest", text: "Ada", templateId: guest.templateId, templateVersion: guest.templateVersion + 1),
                    "", "a stale template version is refused silently")
        expectEqual(model.saveNamesList(rowId: "gone", text: "Ada", templateId: "", templateVersion: 0), "", "a gone row is refused silently")
        expect(model.document.playlist == before, "document unchanged")
    }

    @MainActor
    static func testNoNamesMessage() {
        let (model, _) = playoutModel(windowsShowRows())
        let guest = model.rows.first { $0.id == "guest" }!
        expectEqual(model.saveNamesList(rowId: "guest", text: "name,title\n", templateId: guest.templateId, templateVersion: guest.templateVersion),
                    "No names found. Use Name, Title and optional variable IDs in the header.", "Windows' message, verbatim")
    }

    @MainActor
    static func testCellsOnlyForEligibleFields() {
        let (model, _) = playoutModel(namesClockRows())
        let names = model.rows.first { $0.id == "panel-names" }!
        let result = model.saveNamesList(rowId: "panel-names", text: "name,title,text-size,time\nAda,Host,72,9:00",
                                         templateId: names.templateId, templateVersion: names.templateVersion)
        expect(result == nil, "saved")
        let cells = model.rows.first { $0.id == "panel-names" }?.variants.first?["cells"] as? JSONObject ?? [:]
        expectEqual(cells["text-size"] as? String, "72", "an eligible template field becomes a cell")
        expect(cells["time"] == nil, "time is handled by the template, never a cell")
    }

    /// Re-saving an edited list keeps the ids of unchanged names, so a line on air keeps its instance id.
    @MainActor
    static func testNamesListReSaveKeepsOnAirInstanceIds() {
        let (model, _) = playoutModel(namesClockRows())
        let names = model.rows.first { $0.id == "panel-names" }!
        let before = Dictionary(uniqueKeysWithValues: GraphicsPlayout.variantEntries(names).map { ($0.label, $0.instanceId) })
        let text = GraphicsNamesList.text(names.variants).replacingOccurrences(of: "Bo Diddley", with: "Bo Diddley Jr")
        expect(model.saveNamesList(rowId: "panel-names", text: "Dee, New\n" + text, templateId: names.templateId, templateVersion: names.templateVersion) == nil, "saved")
        let after = Dictionary(uniqueKeysWithValues: GraphicsPlayout.variantEntries(model.rows.first { $0.id == "panel-names" }!).map { ($0.label, $0.instanceId) })
        expectEqual(after["Ada Lovelace"], before["Ada Lovelace"], "an unchanged name keeps its instance id")
        expectEqual(after["Cy Twombly"], before["Cy Twombly"], "after a line is inserted above it too")
        expect(after["Bo Diddley Jr"] != nil && after["Bo Diddley Jr"] != before["Bo Diddley"], "an edited name is a new line")
    }
}
