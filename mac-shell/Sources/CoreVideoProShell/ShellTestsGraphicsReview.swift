// Graphics G1-G3 review round: names lists, clock rows, data rows, the Name
// field, and lower-third on-air truth. Vectors: contracts/graphics-shell/
// names-clock.vectors.json + documents/windows-names-clock.json (exported from
// the real Windows store by GraphicsShellVectorTests.NamesListAndClockVectors).

import Foundation

extension ShellTests {
    static var graphicsReviewCases: [(String, () -> Void)] {
        [
            ("graphics-review/names-clock-vectors", testGraphicsNamesClockVectors),
            ("graphics-review/windows-names-clock-document-loads", testGraphicsNamesClockDocumentLoads),
            ("graphics-review/names-list-lines", testGraphicsNamesListLines),
            ("graphics-review/names-list-show-hides-siblings", testGraphicsNamesListShowHidesSiblings),
            ("graphics-review/names-list-hotkey-steps", testGraphicsNamesListHotkeySteps),
            ("graphics-review/clock-row-configures-clock", testGraphicsClockRowConfiguresClock),
            ("graphics-review/data-rows", testGraphicsDataRows),
            ("graphics-review/name-field-empty-is-precise", testGraphicsNameFieldEmptyIsPrecise),
            ("graphics-review/lower-third-on-air-counts-managed", testGraphicsLowerThirdOnAirCountsManaged),
        ]
    }

    static func namesClockRows() -> [GraphicsRow] {
        let url = repoRoot.appendingPathComponent("contracts/graphics-shell/documents/windows-names-clock.json")
        let json = (try? Data(contentsOf: url)).flatMap { try? JSONSerialization.jsonObject(with: $0) as? JSONObject } ?? [:]
        return (json["playlist"] as? [JSONObject] ?? []).map(GraphicsRow.init(json:))
    }

    static var realCatalog: GraphicsState { GraphicsState(node: ["schemaVersion": 1, "coreEpoch": "e", "templates": realCatalogTemplates]) }

    static func testGraphicsNamesClockVectors() {
        let rows = namesClockRows()
        guard let names = rows.first(where: { $0.id == "panel-names" }), let clock = rows.first(where: { $0.id == "countdown" }),
              let template = realCatalog.template(names.templateId, names.templateVersion)
        else { expect(false, "names/clock fixture rows and catalog template must load"); return }
        var checked = 0
        for row in loadGraphicsVectors("names-clock") {
            let id = row["id"] as? String ?? "?"
            let input = row["input"] as? JSONObject ?? [:], expected = row["expected"] as? JSONObject ?? [:]
            switch row["operation"] as? String {
            case "variantEntries":
                let got = GraphicsPlayout.variantEntries(names)
                let want = expected["entries"] as? [JSONObject] ?? []
                expectEqual(got.map(\.sourceId), want.map { $0["sourceId"] as? String ?? "" }, "\(id): sources")
                expectEqual(got.map(\.label), want.map { $0["name"] as? String ?? "" }, "\(id): names")
                expectEqual(got.map(\.title), want.map { $0["title"] as? String ?? "" }, "\(id): titles")
                expectEqual(got.map(\.instanceId), want.map { $0["instanceId"] as? String ?? "" }, "\(id): instance ids")
            case "nextVariant":
                let onAir = input["onProgram"] as? [String] ?? []
                expectEqual(GraphicsPlayout.nextVariant(names, onProgramSourceIds: onAir)?.sourceId, expected["sourceId"] as? String, "\(id)")
            case "cueFields":
                let entry = GraphicsPlaylistEntry(rowId: names.id, sourceId: input["sourceId"] as? String ?? "",
                                                  name: input["name"] as? String ?? "", title: input["title"] as? String ?? "")
                let fields = GraphicsPlayout.cueFields(names, entry, declarative: true, descriptors: template.fields)
                expect(jsonEqual(fields, expected["fields"]), "\(id): \(fields)")
            case "clockConfiguration":
                expect(jsonEqual(GraphicsPlayout.clockWire(clock.clock ?? [:]), expected["configuration"]), "\(id): clock wire")
                expectEqual(GraphicsRow.validClock(clock.clock ?? [:]), expected["valid"] as? Bool ?? false, "\(id): valid")
            default: expect(false, "\(id): unknown operation")
            }
            checked += 1
        }
        expect(checked >= 10, "every names/clock vector replayed (\(checked))")
    }

    static func testGraphicsNamesClockDocumentLoads() {
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("default.json")
        try? FileManager.default.copyItem(at: repoRoot.appendingPathComponent("contracts/graphics-shell/documents/windows-names-clock.json"),
                                          to: URL(fileURLWithPath: path))
        let load = GraphicsDocumentStore(path: path).load()
        expect(!load.readOnly, "the Windows names/clock show loads writable")
        expectEqual(load.document.playlist.map(\.id), ["panel-names", "countdown"], "both rows")
        expectEqual(load.document.row("panel-names")?.variants.count, 3, "three names")
    }

    @MainActor
    static func testGraphicsNamesListLines() {
        let (model, _) = playoutModel(namesClockRows())
        let lines = model.lines().filter { $0.row.id == "panel-names" }
        expectEqual(lines.map(\.entry.sourceId), ["variant:v-ada", "variant:v-bo", "variant:v-cy"], "one line per name")
        expectEqual(lines.map(\.label), ["Panel names · Ada Lovelace", "Panel names · Bo Diddley", "Panel names · Cy Twombly"], "labels")
    }

    @MainActor
    static func testGraphicsNamesListShowHidesSiblings() {
        let rows = namesClockRows()
        let (model, core) = playoutModel(rows)
        let names = rows[0]
        let entries = GraphicsPlayout.variantEntries(names)
        let ada = awaitMain(5) { await model.playout.show(entries[0]) }
        expectEqual(ada?.accepted, true, "first name shown")
        let create = core.commands.first { $0["type"] as? String == "graphics-instance-create" }?["graphics"] as? JSONObject
        expectEqual((create?["fields"] as? JSONObject)?["name"] as? String, "Ada Lovelace", "the NAME, not the row default")
        expectEqual((create?["fields"] as? JSONObject)?["text-size"] as? Double, 72, "the line's cell, typed")
        let before = core.commands.count
        let bo = awaitMain(5) { await model.playout.show(entries[1]) }
        expectEqual(bo?.accepted, true, "second name shown")
        expectEqual(Array(core.transcript.dropFirst(before)),
                    ["graphics-instance-create:\(entries[1].instanceId)", "graphics-instance-cue:\(entries[1].instanceId)",
                     "graphics-instance-trigger:hide:\(entries[0].instanceId)", "graphics-instance-trigger:show:\(entries[1].instanceId)"],
                    "showing a line takes the row's other line off Program first (GraphicsPlayoutService.PlayInAsync)")
        expectEqual(model.strip.onProgram.map(\.instanceId), [entries[1].instanceId], "one name on air")
        expectEqual(model.strip.onProgram.first?.label, "Panel names · Bo Diddley", "chip names the line")
    }

    @MainActor
    static func testGraphicsNamesListHotkeySteps() {
        let rows = namesClockRows()
        let (model, _) = playoutModel(rows)
        expectEqual(model.resolveEntry(rowId: "panel-names", subjectSourceId: nil)?.sourceId, "variant:v-ada", "nothing on air: the first name")
        _ = awaitMain(5) { await model.playout.show(GraphicsPlayout.variantEntries(rows[0])[0]) }
        expectEqual(model.resolveEntry(rowId: "panel-names", subjectSourceId: nil)?.sourceId, "variant:v-bo", "the bare hotkey steps to the next name")
        expectEqual(GraphicsHotkeys.route(function: 6, modifiers: [], textFocus: false, rows: rows), .playIn(rowId: "panel-names", guest: false),
                    "F6 routes to the names row")
    }

    @MainActor
    static func testGraphicsClockRowConfiguresClock() {
        let rows = namesClockRows()
        let (model, core) = playoutModel(rows)
        let clock = rows[1]
        let entry = GraphicsPlayout.entry(for: clock)
        let shown = awaitMain(5) { await model.playout.show(entry) }
        expectEqual(shown?.accepted, true, "clock row shown")
        expectEqual(core.transcript, ["graphics-instance-create:playlist-countdown", "graphics-clock-control:configure:playlist-countdown",
                                      "graphics-instance-cue:playlist-countdown", "graphics-instance-trigger:show:playlist-countdown"],
                    "the row's clock is configured after create (GraphicsPlayoutService.ConfigureClock)")
        let control = core.commands.first { $0["type"] as? String == "graphics-clock-control" }?["graphics"] as? JSONObject ?? [:]
        expect(jsonEqual(control["configuration"], GraphicsPlayout.clockWire(clock.clock ?? [:])), "with the row's configuration")
        expectEqual((control["expectedClockRevision"] as? NSNumber)?.intValue, 1, "clock revision >= 1")
        _ = awaitMain(5) { await model.playout.hide(entry.instanceId) }
        let before = core.commands.count
        _ = awaitMain(5) { await model.playout.show(entry) }
        expect(!core.transcript.dropFirst(before).contains { $0.hasPrefix("graphics-clock-control") },
               "an instance already carrying that clock is not reconfigured")
    }

    @MainActor
    static func testGraphicsDataRows() {
        var stat = GraphicsRow(json: ["id": "scores", "kind": "title", "name": "Scores", "templateId": "centered-title", "templateVersion": 1,
                                      "fields": ["heading": "Home 3 - 1 Away"]])
        stat.json["dataBinding"] = ["sourceId": "scores", "columns": ["heading"], "limit": 1, "revision": 4] as JSONObject
        var crawl = GraphicsRow(json: ["id": "ticker", "kind": "title", "name": "Ticker", "templateId": "centered-title", "templateVersion": 1])
        crawl.json["dataBinding"] = ["sourceId": "news", "columns": ["headline"], "limit": 20, "revision": 1,
                                     "crawl": ["speed": 120, "gap": 48, "direction": "left", "mode": "loop"]] as JSONObject
        var remote = GraphicsRow(json: ["id": "feed", "kind": "title", "name": "Feed", "templateId": "centered-title", "templateVersion": 1])
        remote.json["dataBinding"] = ["sourceId": "sheet", "columns": ["heading"], "limit": 1, "revision": 2, "remote": true] as JSONObject
        let (model, core) = playoutModel([stat, crawl, remote])
        let played = awaitMain(5) { await model.playout.show(GraphicsPlayout.entry(for: stat)) }
        expectEqual(played?.accepted, true, "a local data row plays its last published values, as Windows")
        let create = core.commands.first?["graphics"] as? JSONObject
        expectEqual((create?["fields"] as? JSONObject)?["heading"] as? String, "Home 3 - 1 Away", "the published values")
        let before = core.commands.count
        let crawlShow = awaitMain(5) { await model.playout.show(GraphicsPlayout.entry(for: crawl)) }
        expect(crawlShow?.reason != GraphicsPlayout.unsupportedDataText("Ticker"), "a local crawl is no longer refused (G4-G7 Task 11)")
        expectEqual(crawlShow?.accepted, true, "and it plays (\(crawlShow?.reason ?? ""))")
        let afterCrawl = core.commands.count
        let refusedRemote = awaitMain(5) { await model.playout.show(GraphicsPlayout.entry(for: remote)) }
        expectEqual(refusedRemote?.reason, GraphicsPlayout.unsupportedDataText("Feed"), "a live feed row is refused")
        expect(afterCrawl > before, "the crawl was sent")
        expectEqual(core.commands.count, afterCrawl, "nothing sent for the remote feed row")
        let lines = model.lines()
        expect(lines.first { $0.row.id == "feed" }?.canShow == false, "the remote feed line cannot Show (until G9, plan D7)")
        expectEqual(lines.first { $0.row.id == "feed" }?.state, .failed(GraphicsPlayout.unsupportedDataText("Feed")), "and says why")
        expectEqual(lines.first { $0.row.id == "scores" }?.state, .onProgram, "the local data line played and offers Hide")
    }

    @MainActor
    static func testGraphicsNameFieldEmptyIsPrecise() {
        let (model, _) = playoutModel([])
        model.document.playlist = interviewRows()
        model.setName("title", "")
        expectEqual(model.rows.first { $0.id == "title" }?.name, "Show title", "an empty name is not stored")
        expectEqual(model.documentStatus, GraphicsModel.nameRequiredText, "and the message says exactly why")
        model.setName("title", String(repeating: "n", count: 129))
        expectEqual(model.documentStatus, GraphicsModel.nameRequiredText, "too long, same precise message")
        model.setName("title", "Late show")
        expectEqual(model.rows.first { $0.id == "title" }?.name, "Late show", "a valid name commits")
        expectEqual(model.documentStatus, "", "and clears the message")
    }

    @MainActor
    static func testGraphicsLowerThirdOnAirCountsManaged() {
        var managed = ScriptedGraphicsCore.instance("lt-managed", program: 3, visibility: "on-air", template: "compact-solid")
        managed["scope"] = "global"
        let (model, _) = playoutModel(interviewRows(), instances: [managed])
        model.observe(snapshot: ["graphics": ["schemaVersion": 1, "coreEpoch": "epoch-1", "instances": [managed],
                                              "templates": realCatalogTemplates, "familyAuthorities": ["lower-third": "instances"]]])
        expect(model.lowerThirdOnAir, "any lower-third-family instance on Program counts (IsLowerThirdOnAir), not only Guest rows")
        var title = ScriptedGraphicsCore.instance("playlist-title", program: 3, visibility: "on-air", template: "centered-title")
        title["scope"] = "global"
        model.observe(snapshot: ["graphics": ["schemaVersion": 1, "coreEpoch": "epoch-1", "instances": [title], "templates": realCatalogTemplates]])
        expect(!model.lowerThirdOnAir, "a Title row is not a lower third")
    }
}
