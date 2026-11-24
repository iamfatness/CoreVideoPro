// Graphics G4-G7 Task 8 — create a data design (install, store design + dataset, add the row;
// plan D4) and evict unused designs when the native library is full (plan D6, port of
// GraphicsDesignEviction + GraphicsTemplateInstall.RunAsync's single retry).

import Foundation

extension ShellTests {
    static var graphicsDataDesignCases: [(String, () -> Void)] {
        [
            ("graphics-data-design/create-installs-stores-and-adds-a-row", testCreateDataDesignInstallsStoresAndAddsARow),
            ("graphics-data-design/refuses-at-32-designs", testCreateRefusesAt32Designs),
            ("graphics-data-design/refused-install-leaves-the-document", testRefusedInstallLeavesTheDocumentUnchanged),
            ("graphics-data-design/failed-save-removes-the-template", testFailedSaveRemovesTheInstalledTemplate),
            ("graphics-data-design/create-evicts-unused-design-when-full", testCreateEvictsUnusedDesignWhenFull),
            ("graphics-data-design/playlist-pin-install-also-evicts", testPlaylistPinInstallAlsoEvicts),
            ("graphics-data-design/reinstalled-after-restart", testMacDataDesignIsReinstalledAfterRestart),
            ("graphics-data-design/name-rule", testCreateDataDesignNameRule),
            ("graphics-data-design/row-save-failure-is-reported", testRowSaveFailureIsReported),
        ]
    }

    /// The scores dataset and the binding the data-table vector "manifest-table-sorted-points-limit-5" was computed for.
    static func scoresDesignInput() -> (GraphicsLocalData, GraphicsDataBinding, JSONObject)? {
        guard let vector = loadGraphicsVectors("data-table").first(where: { $0["id"] as? String == "manifest-table-sorted-points-limit-5" }),
              let input = vector["input"] as? JSONObject, let data = (input["data"] as? JSONObject).flatMap(GraphicsLocalData.init(json:)),
              var binding = (input["binding"] as? JSONObject).flatMap(GraphicsDataBinding.init(json:)),
              let manifest = (vector["expected"] as? JSONObject)?["value"] as? JSONObject else { return nil }
        binding.sourceId = "data-0123456789abcdef0123456789abcdef"
        var expected = manifest
        var metadata = expected["metadata"] as? JSONObject ?? [:]
        metadata["dataBinding"] = binding.json
        expected["metadata"] = metadata
        return (data, binding, expected)
    }

    /// A model over a scripted core that installs (or, when full, records a failed install), lists and removes templates.
    @MainActor
    static func designModel(rows: [GraphicsRow] = [], templates: [JSONObject] = realCatalogTemplates, instances: [JSONObject] = [],
                            capacity: Int = 16) -> (GraphicsModel, ScriptedGraphicsCore) {
        let (model, core) = playoutModel(rows, instances: instances)
        core.node["templates"] = templates
        model.observe(snapshot: ["graphics": core.node])
        let base = ScriptedGraphicsCore.instancesReaction()
        core.react = { command, node in
            let g = command["graphics"] as? JSONObject ?? [:]
            switch command["type"] as? String {
            case "graphics-template-install" where (node["templates"] as? [JSONObject] ?? []).count >= capacity:
                var installs = node["templateInstalls"] as? [JSONObject] ?? []
                installs.append(["requestId": g["requestId"] as? String ?? "", "state": "failed", "reason": "template-library-capacity"])
                node["templateInstalls"] = installs
                return (true, "")
            case "graphics-template-remove":
                let id = g["templateId"] as? String, version = (g["templateVersion"] as? NSNumber)?.intValue
                node["templates"] = (node["templates"] as? [JSONObject] ?? []).filter {
                    !($0["templateId"] as? String == id && ($0["templateVersion"] as? NSNumber)?.intValue == version)
                }
                return (true, "")
            default: return base(command, &node)
            }
        }
        model.designHex6 = { "a1b2c3" }
        model.playout.now = { Date(timeIntervalSince1970: 1_795_867_200) }
        return (model, core)
    }

    static func designManifest(_ id: String, _ version: Int = 1) -> JSONObject {
        ["templateId": id, "templateVersion": version, "runtime": "declarative", "family": "title", "fields": [["id": "heading", "type": "string"]]]
    }

    @MainActor
    static func removedTemplateIds(_ core: ScriptedGraphicsCore) -> [String] {
        core.commands.filter { $0["type"] as? String == "graphics-template-remove" }.compactMap { ($0["graphics"] as? JSONObject)?["templateId"] as? String }
    }

    @MainActor
    static func testCreateDataDesignInstallsStoresAndAddsARow() {
        guard let (data, binding, expectedManifest) = scoresDesignInput() else { expect(false, "scores vector input"); return }
        let (model, core) = designModel()
        let outcome = awaitMain { await model.createDataDesign(name: "Scores", data: data, binding: binding) }
        expect(outcome?.ok == true, "created: \(outcome?.message ?? "nil")")
        expectEqual(Array(core.transcript.prefix(2)), ["graphics-template-install", "graphics-template-list"], "install, then list until listed")
        expectEqual(model.document.designs.count, 1, "one design stored")
        let design = model.document.designs.first ?? [:]
        expectEqual(design["templateId"] as? String, "design-scores-a1b2c3", "design id from the name and hex6")
        expectEqual(design["designId"] as? String, "design-scores-a1b2c3", "designId is the template id")
        expect(jsonEqual(design["definition"], expectedManifest), "definition equals the manifest Windows computes for the same input")
        expectEqual(model.document.dataSources.map(\.id), [binding.sourceId], "the dataset is stored with it")
        expectEqual(model.document.dataSources.first?.revision, 1, "at revision 1")
        let row = model.rows.first
        expectEqual(row?.templateId, "design-scores-a1b2c3", "a row is added for the design")
        expectEqual(row?.hotkey, "F1", "on the first free F-key")
        expectEqual(row?.kind, GraphicsRow.title, "a title row")
        expectEqual(row?.dataBinding.flatMap(GraphicsDataBinding.init(json:)), binding, "bound to the dataset")
        expectEqual(row?.fields["table-row-1"] as? String, "[\"1\",\"Cats\",\"1e3\"]", "fields are the manifest defaults")
        expectEqual(outcome?.rowId, row?.id, "the outcome names the row")
        expectEqual(outcome?.message, "Created Scores. \"Scores\" is in the playlist on F1. Show it from Operate when you want it on air.", "Windows' messages")
        _ = awaitMain { await model.saveNow() }
        let disk = GraphicsDocumentStore(path: model.documentStore.path).load()
        expect(!disk.readOnly && disk.document.designs.count == 1 && disk.document.dataSources.count == 1, "stored on disk: \(disk.status)")
    }

    @MainActor
    static func testCreateRefusesAt32Designs() {
        guard let (data, binding, _) = scoresDesignInput() else { expect(false, "input"); return }
        let (model, core) = designModel()
        model.document.designs = (0..<32).map { ["designId": "design-d\($0)-aaaaaa", "templateId": "design-d\($0)-aaaaaa"] }
        let outcome = awaitMain { await model.createDataDesign(name: "Scores", data: data, binding: binding) }
        expectEqual(outcome?.message, "A show holds at most 32 designs. Delete one you no longer use.", "Windows' TooMany text")
        expectEqual(core.commands.count, 0, "nothing sent")
    }

    @MainActor
    static func testRefusedInstallLeavesTheDocumentUnchanged() {
        guard let (data, binding, _) = scoresDesignInput() else { expect(false, "input"); return }
        let (model, core) = designModel(capacity: 0)
        let before = model.document.json as NSDictionary
        let outcome = awaitMain { await model.createDataDesign(name: "Scores", data: data, binding: binding) }
        expectEqual(outcome?.ok, false, "refused")
        expectEqual(outcome?.message, "The graphics library is full. Delete designs you no longer use, or remove graphics from the playlist.", "library full")
        expect((model.document.json as NSDictionary).isEqual(before), "no design, dataset or row written before the install is confirmed")
        expectEqual(removedTemplateIds(core), [], "nothing to evict")
    }

    @MainActor
    static func testFailedSaveRemovesTheInstalledTemplate() {
        guard let (data, binding, _) = scoresDesignInput() else { expect(false, "input"); return }
        let (model, core) = designModel()
        graphicsWrite("{\"schemaVersion\":9}", model.documentStore.path)   // a newer show: read-only
        model.adopt(model.documentStore.load())
        expect(model.documentReadOnly, "the show is read-only")
        let outcome = awaitMain { await model.createDataDesign(name: "Scores", data: data, binding: binding) }
        expectEqual(outcome?.ok, false, "refused")
        expectEqual(removedTemplateIds(core), ["design-scores-a1b2c3"], "the installed template is removed again")
        expectEqual(model.document.designs.count, 0, "no design kept in memory")
        expectEqual(model.document.dataSources.count, 0, "no dataset kept in memory")
        expectEqual(model.rows.count, 0, "no row")
    }

    /// 16 templates: the 9 curated + 7 designs, of which only design-a and design-b are used by nothing
    /// (a row, a saved cue, an instance's program version, cue version and program image use the rest).
    @MainActor
    static func fullLibraryModel(rows: [GraphicsRow]) -> (GraphicsModel, ScriptedGraphicsCore) {
        var live = ScriptedGraphicsCore.instance("playlist-other", template: "design-live-aaaaaa")
        live["templateVersion"] = 3; live["programTemplateVersion"] = 2
        live["cueTemplateVersion"] = 1; live["templateId"] = "design-live-aaaaaa"
        var imaged = ScriptedGraphicsCore.instance("playlist-imaged", template: "compact-solid")
        imaged["programImage"] = ["templateId": "design-img-aaaaaa", "templateVersion": 1, "assetId": "a1"]
        var cueCopy = ScriptedGraphicsCore.instance("playlist-cued", template: "design-cuecopy-aaaaaa")
        cueCopy["templateVersion"] = 2; cueCopy["cueTemplateVersion"] = 1
        let designs = [designManifest("design-a-aaaaaa"), designManifest("design-b-aaaaaa"), designManifest("design-row-aaaaaa"),
                       designManifest("design-live-aaaaaa", 2), designManifest("design-cuecopy-aaaaaa", 1), designManifest("design-img-aaaaaa"),
                       designManifest("design-cue-aaaaaa")]
        let (model, core) = designModel(rows: rows, templates: realCatalogTemplates + designs, instances: [live, imaged, cueCopy])
        model.document.json["cues"] = [["id": "c", "name": "C", "draft": ["templateId": "design-cue-aaaaaa", "templateVersion": 1]]]
        return (model, core)
    }

    static func rowPinnedTo(_ templateId: String, id: String = "pinned") -> GraphicsRow {
        GraphicsRow(json: ["id": id, "kind": "title", "name": id, "hotkey": "", "playOutAfterMs": 0, "templateId": templateId, "templateVersion": 1,
                           "fields": ["heading": "x"], "followSources": false, "showClock": false])
    }

    @MainActor
    static func testCreateEvictsUnusedDesignWhenFull() {
        guard let (data, binding, _) = scoresDesignInput() else { expect(false, "input"); return }
        let (model, core) = fullLibraryModel(rows: [rowPinnedTo("design-row-aaaaaa")])
        expectEqual((core.node["templates"] as? [JSONObject])?.count, 16, "fixture invariant: a full 16-definition library")
        let outcome = awaitMain { await model.createDataDesign(name: "Scores", data: data, binding: binding) }
        expect(outcome?.ok == true, "created after eviction: \(outcome?.message ?? "nil")")
        expectEqual(removedTemplateIds(core).sorted(), ["design-a-aaaaaa", "design-b-aaaaaa"], "exactly the two unused design versions are evicted")
        expectEqual(core.transcript.filter { $0 == "graphics-template-install" }.count, 2, "one retry after the eviction")
        expect(!removedTemplateIds(core).contains { !GraphicsDesignIds.isDesignTemplateId($0) }, "never a curated template")
    }

    @MainActor
    static func testPlaylistPinInstallAlsoEvicts() {
        let row = rowPinnedTo("design-new-bbbbbb", id: "fresh")
        let (model, core) = fullLibraryModel(rows: [rowPinnedTo("design-row-aaaaaa"), row])
        core.node["installHash"] = resyncHash
        model.document.json["templates"] = [resyncPin("design-new-bbbbbb", 1)]
        let result = awaitMain { await model.playout.show(GraphicsPlayout.entry(for: row)) }
        expectEqual(removedTemplateIds(core).sorted(), ["design-a-aaaaaa", "design-b-aaaaaa"], "the pin reinstall evicts the same two")
        expect(core.transcript.contains("graphics-instance-create:playlist-fresh"), "and the graphic is created after the retry (\(result?.reason ?? ""))")
    }

    @MainActor
    static func testMacDataDesignIsReinstalledAfterRestart() {
        guard let (data, binding, _) = scoresDesignInput() else { expect(false, "input"); return }
        let (model, core) = designModel()
        model.documentLoaded = true
        _ = awaitMain { await model.createDataDesign(name: "Scores", data: data, binding: binding) }
        let installsBefore = core.transcript.filter { $0 == "graphics-template-install" }.count
        model.coreGenerationChanged()
        core.node["coreEpoch"] = "epoch-2"
        core.node["templates"] = realCatalogTemplates
        model.observe(snapshot: ["graphics": core.node, "profile": availableProfile])
        _ = awaitMain(5) { await model.designResync?.value; return true }
        expectEqual(core.transcript.filter { $0 == "graphics-template-install" }.count, installsBefore + 1, "the new core gets the design from the show's designs section")
        expect(model.state?.template("design-scores-a1b2c3", 1) != nil, "installed on the new core")
    }

    @MainActor
    static func testCreateDataDesignNameRule() {
        guard let (data, binding, _) = scoresDesignInput() else { expect(false, "input"); return }
        let (model, core) = designModel()
        for name in ["   ", String(repeating: "n", count: 81)] {
            let outcome = awaitMain { await model.createDataDesign(name: name, data: data, binding: binding) }
            expectEqual(outcome?.message, "Use a design name of 1–80 characters.", "name rule for \(name.count) chars")
        }
        expectEqual(core.commands.count, 0, "nothing sent")
    }

    /// Windows awaits SavePlaylistAsync: a row that never reached disk is not reported as added (review I1).
    @MainActor
    static func testRowSaveFailureIsReported() {
        guard let (data, binding, _) = scoresDesignInput() else { expect(false, "input"); return }
        let (model, _) = designModel()
        final class Count: @unchecked Sendable { var n = 0 }
        let saves = Count()
        model.writeDocument = { store, document in
            saves.n += 1
            if saves.n == 2 { throw GraphicsDocumentError(message: "disk full") }
            try store.save(document)
        }
        let outcome = awaitMain { await model.createDataDesign(name: "Scores", data: data, binding: binding) }
        expectEqual(outcome?.ok, false, "not ok")
        expectEqual(outcome?.message, "Created Scores. Not added to the playlist: Graphics playlist not saved: disk full", "says the row was not saved")
        expectEqual(saves.n, 2, "design save, then the row save")
    }
}
