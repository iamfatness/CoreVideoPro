// Graphics G4-G7 Slice D — Shot Layouts: model, validation, persistence and defaults (Task 13).
// Vectors: contracts/graphics-shell/layouts.vectors.json + documents/windows-layouts.json
// (GraphicsShellVectorTests.LayoutVectors, the real Windows serializer and store).

import Foundation

extension ShellTests {
    static var graphicsLayoutCases: [(String, () -> Void)] {
        [
            ("graphics-layouts/vectors", testLayoutVectorsReplay),
            ("graphics-layouts/set-moves-the-hotkey", testSetLayoutMovesTheHotkey),
            ("graphics-layouts/default-rows-install-presets-only-when-none", testDefaultRowsInstallPresetLayoutsOnlyWhenNone),
            ("graphics-layouts/windows-layouts-round-trip", testWindowsLayoutsRoundTrip),
            ("graphics-layouts/missing-scene-refuses-set", testMissingSceneRefusesSet),
            ("graphics-layouts/resolver-vectors", testLayoutResolverVectorsReplay),
            ("graphics-layouts/cue-transcripts", testLayoutTranscriptsReplay),
            ("graphics-layouts/preview-edit-during-preparation", testPreviewEditDuringPreparationRefuses),
            ("graphics-layouts/epoch-change-drops-prepared", testEpochChangeDropsThePreparedLayout),
            ("graphics-layouts/cue-waits-for-core-preview", testCueLayoutWaitsForTheCoreToShowThePreviewScene),
            ("graphics-layouts/header-buttons-per-state", testHeaderButtonsPerState),
            ("graphics-layouts/row-editor-options-and-read", testRowEditorOptionsAndRead),
            ("graphics-layouts/shift-f2-cues-that-layout", testShiftF2CuesTheLayoutWithThatHotkey),
            ("graphics-layouts/missing-scene-refuses-at-cue", testMissingSceneLayoutRefusesAtCue),
        ]
    }

    @MainActor
    static func layoutModel(drop: [String] = [], scenes: [String] = ["fullscreen", "two-up", "pip"]) -> GraphicsModel {
        let model = graphicsModel(ScriptedGraphicsCore())
        var document = GraphicsDocument(json: documentJSON("windows-layouts.json"))
        document.shotLayouts = document.shotLayouts.filter { !drop.contains($0.id) }
        model.document = document
        model.documentLoaded = true
        model.sceneIds = { scenes }
        return model
    }

    @MainActor
    static func testLayoutVectorsReplay() {
        var checked = 0
        for vector in loadGraphicsVectors("layouts") {
            let id = vector["id"] as? String ?? "?"
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            switch vector["operation"] as? String {
            case "layoutValid":
                expectEqual(GraphicsShotLayout(json: input["layout"] as? JSONObject ?? [:]).isValid, expected["valid"] as? Bool, "\(id)")
            case "slug":
                expectEqual(GraphicsLayouts.slug(input["name"] as? String ?? "", taken: input["taken"] as? [String] ?? []), expected["slug"] as? String, "\(id)")
            case "setLayout":
                let model = layoutModel(drop: input["dropLayouts"] as? [String] ?? [], scenes: input["scenes"] as? [String] ?? [])
                let before = model.document.shotLayouts.map { [$0.id, $0.hotkey] }
                let error = model.setLayout(GraphicsShotLayout(json: input["layout"] as? JSONObject ?? [:]))
                expectEqual(error == nil, expected["ok"] as? Bool, "\(id): ok (\(error ?? "nil"))")
                expectEqual(error, expected["error"] as? String, "\(id): message")
                if let want = expected["layouts"] as? [JSONObject] {
                    expectEqual(model.document.shotLayouts.map { [$0.id, $0.hotkey] }, want.map { [$0["id"] as? String ?? "", $0["hotkey"] as? String ?? ""] }, "\(id): layouts after")
                } else {
                    expectEqual(model.document.shotLayouts.map { [$0.id, $0.hotkey] }, before, "\(id): unchanged")
                }
            case "summary":
                let document = GraphicsDocument(json: documentJSON("windows-layouts.json"))
                let scenes = (input["scenes"] as? [JSONObject] ?? []).map { (id: $0["id"] as? String ?? "", name: $0["name"] as? String ?? "") }
                expectEqual(GraphicsLayouts.summary(GraphicsShotLayout(json: input["layout"] as? JSONObject ?? [:]), rows: document.playlist, scenes: scenes),
                            expected["summary"] as? String, "\(id)")
            default: expect(false, "\(id): unknown operation"); continue
            }
            checked += 1
        }
        expect(checked >= 44, "every layout vector replayed (\(checked))")
    }

    @MainActor
    static func testSetLayoutMovesTheHotkey() {
        let model = layoutModel(drop: ["ghost-row"])
        let error = model.setLayout(GraphicsShotLayout(json: ["id": "fresh", "name": "Fresh", "sceneId": "pip", "rowIds": ["logo"], "hotkey": "Shift+F1"]))
        expect(error == nil, "saved (\(error ?? ""))")
        expectEqual(model.document.shotLayouts.first { $0.id == "open" }?.hotkey, "", "the key moves from Open")
        expectEqual(model.document.shotLayouts.first { $0.id == "fresh" }?.hotkey, "Shift+F1", "to the new layout")
        expectEqual(model.removeLayout(id: "nope"), "That shot layout does not exist.", "remove of an unknown id")
        expect(model.removeLayout(id: "fresh") == nil, "remove")
        expect(!model.document.shotLayouts.contains { $0.id == "fresh" }, "gone")
    }

    @MainActor
    static func testDefaultRowsInstallPresetLayoutsOnlyWhenNone() {
        let (model, _) = playoutModel([])
        model.addDefaultRows()
        expectEqual(model.document.shotLayouts.map(\.id), ["open", "two-up", "panel", "clean"], "an empty show gets the four presets (Windows ids, owner decision 2)")
        expectEqual(model.document.shotLayouts.first?.sceneId, "intro", "Windows' scene id is kept")
        let custom = layoutModel(drop: ["ghost-row"])
        custom.document.shotLayouts = [GraphicsShotLayout(json: ["id": "mine", "name": "Mine", "sceneId": "", "rowIds": [] as [String]])]
        custom.document.playlist = []
        custom.observe(snapshot: ["graphics": ["schemaVersion": 1, "coreEpoch": "e", "templates": realCatalogTemplates]])
        custom.addDefaultRows()
        expectEqual(custom.document.shotLayouts.map(\.id), ["mine"], "a show with layouts keeps its own")
    }

    static func testWindowsLayoutsRoundTrip() {
        let dir = graphicsTempDir()
        defer { try? FileManager.default.removeItem(atPath: dir) }
        let path = (dir as NSString).appendingPathComponent("default.json")
        try? FileManager.default.copyItem(at: repoRoot.appendingPathComponent("contracts/graphics-shell/documents/windows-layouts.json"), to: URL(fileURLWithPath: path))
        let store = GraphicsDocumentStore(path: path)
        let load = store.load()
        expect(!load.readOnly, "windows-layouts loads writable: \(load.status)")
        expectEqual(load.document.shotLayouts.map(\.id), ["open", "two-up", "panel", "clean", "final-scores", "bad-override", "ghost-row"], "every layout")
        let final = load.document.shotLayouts.first { $0.id == "final-scores" }
        expectEqual(final?.rowSettings["leaderboard"]?.dataPin?.revision, 2, "the data pin")
        expectEqual(final?.rowSettings["title"]?.overrides.first?.key, "heading", "the override")
        do { try store.save(load.document) } catch { expect(false, "re-save: \(error)") }
        let saved = (try? Data(contentsOf: URL(fileURLWithPath: path))).flatMap { try? JSONSerialization.jsonObject(with: $0) }
        expect(jsonEqual(saved, documentJSON("windows-layouts.json")), "re-saved document equals the Windows original")
    }

    @MainActor
    static func testMissingSceneRefusesSet() {
        let model = layoutModel(drop: ["ghost-row"])
        expectEqual(model.setLayout(GraphicsShotLayout(json: ["id": "open", "name": "Open", "sceneId": "intro", "rowIds": ["title"], "hotkey": "Shift+F1"])),
                    "Scene 'intro' is not in this show.", "Windows' message for a scene the Mac show lacks")
    }

    /// windows-layouts.json with a vector's patch applied (a layout's rowSettings, extra template pins).
    static func patchedLayoutsDocument(_ patch: JSONObject?, layoutId: String) -> GraphicsDocument {
        var document = GraphicsDocument(json: documentJSON("windows-layouts.json"))
        if let settings = patch?["layoutRowSettings"] as? JSONObject {
            document.shotLayouts = document.shotLayouts.map { var l = $0; if l.id == layoutId { l.json["rowSettings"] = settings }; return l }
        }
        if let pins = patch?["pins"] as? [JSONObject] { document.json["templates"] = pins }
        return document
    }

    static func entry(_ o: JSONObject?) -> GraphicsPlaylistEntry {
        GraphicsPlaylistEntry(rowId: o?["rowId"] as? String ?? "", sourceId: o?["sourceId"] as? String ?? "", name: o?["name"] as? String ?? "",
                              title: o?["title"] as? String ?? "", slotNumber: (o?["slotNumber"] as? NSNumber)?.intValue)
    }

    static func testLayoutResolverVectorsReplay() {
        var checked = 0
        for vector in loadGraphicsVectors("layout-transcripts") where vector["operation"] as? String == "resolve" {
            let id = vector["id"] as? String ?? "?"
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            let layoutId = input["layoutId"] as? String ?? ""
            let document = patchedLayoutsDocument(input["patch"] as? JSONObject, layoutId: layoutId)
            let state = GraphicsState(node: input["node"] as? JSONObject ?? [:])
            guard let layout = document.shotLayouts.first(where: { $0.id == layoutId }) else { expect(false, "\(id): layout"); continue }
            do {
                let resolved = try GraphicsLayouts.resolve(document: document, state: state, layout: layout, entry: entry(input["entry"] as? JSONObject))
                expect(expected["error"] == nil, "\(id): expected the error \(expected["error"] ?? "")")
                expect(jsonEqual(resolved.row.fields, expected["fields"]), "\(id): fields \(resolved.row.fields)")
                expectEqual(resolved.identity?.sourceId, (expected["identity"] as? JSONObject)?["sourceId"] as? String, "\(id): identity")
                expectEqual(resolved.dataRevision, (expected["dataRevision"] as? NSNumber)?.int64Value, "\(id): data revision")
                expectEqual(resolved.dataReceivedAt, (expected["dataReceivedAt"] as? String).flatMap(GraphicsDataSourceRecord.parseISO8601), "\(id): received at")
            } catch {
                expectEqual((error as? GraphicsDataError)?.message, expected["error"] as? String, "\(id): error")
            }
            checked += 1
        }
        expect(checked >= 10, "every resolver vector replayed (\(checked))")
    }

    /// A model over windows-layouts.json whose core replays a recorded cue transcript.
    @MainActor
    static func layoutTranscriptModel(_ vector: JSONObject) -> (GraphicsModel, ScriptedGraphicsCore) {
        let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
        let core = ScriptedGraphicsCore(replaying: expected["steps"] as? [JSONObject] ?? [], initialNode: input["initialNode"] as? JSONObject ?? [:])
        let model = graphicsModel(core)
        model.document = GraphicsDocument(json: documentJSON("windows-layouts.json"))
        model.documentLoaded = true
        var clock = Date(timeIntervalSince1970: 1_795_867_200)
        model.playout.now = { clock }
        model.playout.sleep = { [weak core, weak model] ns in
            clock = clock.addingTimeInterval(Double(ns) / 1e9)
            core?.advanceReplayPoll()
            if let core { model?.observe(snapshot: ["graphics": core.node]) }
        }
        return (model, core)
    }

    @MainActor
    static func testLayoutTranscriptsReplay() {
        var checked = 0
        for vector in loadGraphicsVectors("layout-transcripts") where vector["operation"] as? String == "cueLayout" {
            let id = vector["id"] as? String ?? "?"
            let input = vector["input"] as? JSONObject ?? [:], expected = vector["expected"] as? JSONObject ?? [:]
            let (model, core) = layoutTranscriptModel(vector)
            let script = input["script"] as? String ?? ""
            let subject = entry(input["programSubject"] as? JSONObject)
            let result = awaitMain(10) {
                await model.playout.cueLayout(input["layoutId"] as? String ?? "", sceneSelect: { sceneId in
                    core.pseudo("scene-select", ["sceneId": sceneId])
                    return script != "scene-missing"
                }, programSubject: subject, sceneGuests: [], previewStamp: { "stamp" })
            }
            var cancelled: GraphicsOperationResult?
            if input["cancelAfter"] as? Bool == true { cancelled = awaitMain(10) { await model.playout.cancelLayout() } }
            expectEqual(core.replayFaults, [], "\(id): commands as recorded")
            expectEqual(core.commands.count, (expected["steps"] as? [Any])?.count ?? -1, "\(id): command count")
            expectEqual(result?.reason, expected["reason"] as? String, "\(id): reason")
            expectEqual(result?.accepted, expected["accepted"] as? Bool, "\(id): accepted")
            if let cancelReason = expected["cancelReason"] as? String { expectEqual(cancelled?.reason, cancelReason, "\(id): cancel") }
            if input["cancelAfter"] as? Bool != true {
                let prepared = model.playout.preparedLayout
                let want = expected["prepared"] as? JSONObject
                expectEqual(prepared?.layout.id, want?["layoutId"] as? String, "\(id): prepared layout")
                expectEqual(prepared?.cues.map { "\($0.instanceId)@\($0.cueRevision)" },
                            (want?["cues"] as? [JSONObject])?.map { "\($0["instanceId"] as? String ?? "")@\(($0["cueRevision"] as? NSNumber)?.int64Value ?? 0)" },
                            "\(id): prepared cues")
            }
            checked += 1
        }
        expect(checked >= 8, "every layout cue transcript replayed (\(checked))")
    }

    /// A Preview edit between the scene select and the last cue refuses the layout and releases the owned cues.
    @MainActor
    static func testPreviewEditDuringPreparationRefuses() {
        let core = ScriptedGraphicsCore(epoch: "epoch-1")
        core.node["templates"] = realCatalogTemplates + (GraphicsDocument(json: documentJSON("windows-data.json")).designs.compactMap { d -> JSONObject? in
            guard var definition = d["definition"] as? JSONObject else { return nil }
            definition["runtime"] = "declarative"; definition["manifestHash"] = String(repeating: "f", count: 64); return definition
        })
        core.react = ScriptedGraphicsCore.ownedCueReaction()
        let model = graphicsModel(core)
        model.document = GraphicsDocument(json: documentJSON("windows-layouts.json"))
        model.playout.sleep = { [weak core, weak model] _ in if let core { model?.observe(snapshot: ["graphics": core.node]) } }
        var stamp = "before"
        let result = awaitMain(10) {
            await model.playout.cueLayout("final-scores", sceneSelect: { _ in true }, programSubject: nil, sceneGuests: [], previewStamp: {
                defer { stamp = "after" }   // the second read (after the last cue) sees an operator's Preview edit
                return stamp
            })
        }
        expectEqual(result?.reason, "Final scores: Preview changed during preparation. Cue the layout again.", "Windows' refusal")
        let uncues = core.commands.filter { ($0["graphics"] as? JSONObject)?["action"] as? String == "uncue" }
        expectEqual(uncues.count, 2, "both owned cues released")
        expect(uncues.allSatisfy { (($0["graphics"] as? JSONObject)?["cueOwner"] as? String)?.hasPrefix("layout-") == true }, "with the attempt as cueOwner")
        expect(model.playout.preparedLayout == nil, "nothing prepared")
    }

    @MainActor
    static func testEpochChangeDropsThePreparedLayout() {
        guard let vector = loadGraphicsVectors("layout-transcripts").first(where: { $0["id"] as? String == "cue-guarded-final-scores" }) else { expect(false, "vector"); return }
        let (model, core) = layoutTranscriptModel(vector)
        _ = awaitMain(10) { await model.playout.cueLayout("final-scores", sceneSelect: { _ in true }, programSubject: nil, sceneGuests: [], previewStamp: { "stamp" }) }
        expect(model.playout.preparedLayout != nil, "prepared")
        expect(model.layoutProgress != nil, "progress shown")
        model.coreGenerationChanged()
        expect(model.playout.preparedLayout == nil, "a new core drops the prepared layout (restart rule)")
        expect(model.layoutProgress == nil, "and its progress")
        _ = core
    }

    /// GraphicsModel.cueLayout selects the scene to Preview, then waits until the CORE's snapshot shows it
    /// (5 s, injected sleep) before cueing any graphic.
    @MainActor
    static func testCueLayoutWaitsForTheCoreToShowThePreviewScene() {
        guard let vector = loadGraphicsVectors("layout-transcripts").first(where: { $0["id"] as? String == "cue-legacy-open" }) else { expect(false, "vector"); return }
        let (model, core) = layoutTranscriptModel(vector)
        var corePreview = "fullscreen", polls = 0
        model.selectPreviewScene = { sceneId in core.pseudo("scene-select", ["sceneId": sceneId]); return true }
        model.corePreviewSceneId = {
            polls += 1
            if polls > 2 { corePreview = "intro" }
            return corePreview
        }
        var cuesBeforeSwitch = -1
        let original = model.playout.sleep
        model.playout.sleep = { ns in
            if polls <= 2 { cuesBeforeSwitch = core.commands.filter { $0["type"] as? String == "graphics-instance-cue" }.count }
            await original(ns)
        }
        let result = awaitMain(10) { await model.cueLayout("open").value }
        expectEqual(result?.reason, "Open cued: scene intro and 2 graphics on Preview; Take carries both.", "cued (\(result?.reason ?? ""))")
        expect(polls >= 3, "it polled the core's Preview (\(polls))")
        expectEqual(cuesBeforeSwitch, 0, "no cue went out while the core still showed the old Preview scene")
    }

    // ── Task 16: the band, the editor, Shift+F1-F11 ──────────────────────────

    /// GraphicsPlayoutControl.RefreshLayouts' header (:228-251), every state.
    static func testHeaderButtonsPerState() {
        let document = GraphicsDocument(json: documentJSON("windows-layouts.json"))
        let legacy = document.shotLayouts.first { $0.id == "open" }!, guarded = document.shotLayouts.first { $0.id == "final-scores" }!
        let prepared = GraphicsPreparedLayout(layout: guarded, coreEpoch: "e", attemptId: "layout-x", cues: [], previewStamp: nil)
        func titles(_ selected: GraphicsShotLayout?, _ prepared: GraphicsPreparedLayout?, pending: Bool, progress: Bool, busy: Bool = false) -> [String] {
            GraphicsLayouts.headerButtons(selected: selected, prepared: prepared, takePending: pending, hasProgress: progress, busy: busy)
                .map { "\($0.title)\($0.enabled ? "" : " (off)")" }
        }
        expectEqual(titles(nil, nil, pending: false, progress: false), ["Edit layouts"], "nothing selected")
        expectEqual(titles(legacy, nil, pending: false, progress: false), ["Cue and Take", "Edit layouts"], "a selected legacy layout")
        expectEqual(titles(guarded, nil, pending: false, progress: false), ["Edit layouts"], "an unprepared guarded layout offers no Take")
        expectEqual(titles(guarded, nil, pending: false, progress: true), ["Cancel layout", "Edit layouts"], "preparing: Cancel only")
        expectEqual(titles(legacy, prepared, pending: false, progress: true), ["Take", "Cancel layout", "Edit layouts"], "a prepared guarded layout wins the selection")
        expectEqual(titles(nil, prepared, pending: true, progress: true), ["Check Take", "Cancel layout (off)", "Edit layouts"], "a pending guarded Take")
        expectEqual(titles(legacy, nil, pending: false, progress: false, busy: true), ["Cue and Take (off)", "Edit layouts (off)"], "busy")
    }

    /// AddLayoutRowEditor (GraphicsPlayoutControl.Layouts.cs:18-129) as a pure model.
    static func testRowEditorOptionsAndRead() {
        let document = GraphicsDocument(json: documentJSON("windows-layouts.json"))
        guard var row = document.row("leaderboard"), let source = document.dataSources.first(where: { $0.id == "scores" }) else { expect(false, "fixture"); return }
        // The row publishes revision 1 while the loaded source is at 2.
        var binding = row.dataBinding ?? [:]; binding["revision"] = 1; row.json["dataBinding"] = binding
        expectEqual(source.revision, 2, "fixture source at revision 2")
        var editor = GraphicsLayoutRowEditor(row: row, fields: [], dataSources: document.dataSources, saved: nil, included: true)
        expectEqual(editor.revisionOptions.map(\.label), ["Use published data (revision 1)", "Pin revision 1", "Pin revision 2"], "three options")
        expectEqual(editor.selectedRevision, 0, "published by default")
        func read(_ e: GraphicsLayoutRowEditor) -> (ok: Bool, settings: GraphicsLayoutRowSettings?) {
            do { return (true, try e.read()) } catch { return (false, nil) }
        }
        expect(read(editor) == (true, nil), "published data and no overrides read as no settings")
        editor.selectedRevision = 2
        let pinned = read(editor).settings
        expectEqual(pinned?.dataPin?.sourceId, "scores", "pin source")
        expectEqual(pinned?.dataPin?.revision, 2, "Pin revision 2")
        editor.included = false
        expect(read(editor) == (true, nil), "an unticked row has no settings")
        // A saved pin that is no longer offered stays selectable as unavailable.
        let stale = GraphicsLayoutRowSettings(json: ["dataPin": ["sourceId": "scores", "revision": 9], "overrides": [:] as JSONObject])
        let staleEditor = GraphicsLayoutRowEditor(row: row, fields: [], dataSources: document.dataSources, saved: stale, included: true)
        expectEqual(staleEditor.revisionOptions.last?.label, "Revision 9 unavailable", "unavailable revision")
        expectEqual(staleEditor.selectedRevision, 3, "and selected")

        // Overrides: an invalid saved override reads back verbatim unless replaced.
        let size = GraphicsTemplateField(id: "text-size", type: "number", minimum: 8, maximum: 200)
        let heading = GraphicsTemplateField(id: "heading", type: "string")
        guard let title = document.row("title") else { expect(false, "title row"); return }
        let saved = GraphicsLayoutRowSettings(json: ["overrides": ["text-size": "huge", "gone": "x"]])
        var overrides = GraphicsLayoutRowEditor(row: title, fields: [size, heading], dataSources: [], saved: saved, included: true)
        expectEqual(overrides.fieldEditors.map(\.title), ["Replace invalid Text size", "Override Heading"], "editor titles")
        expectEqual(overrides.fieldEditors.map(\.enabled), [false, false], "nothing enabled")
        expectEqual(overrides.unavailable.map(\.key), ["gone"], "the unavailable override is listed")
        let kept = read(overrides).settings
        expectEqual(kept?.json["overrides"] as? NSDictionary, ["text-size": "huge", "gone": "x"] as NSDictionary, "invalid and unavailable overrides read back verbatim")
        overrides.fieldEditors[0].enabled = true
        overrides.fieldEditors[0].value = 72
        overrides.unavailable[0].remove = true
        let replaced = read(overrides).settings
        expectEqual(replaced?.json["overrides"] as? NSDictionary, ["text-size": 72] as NSDictionary, "replaced, and the unavailable one removed")
        overrides.fieldEditors[0].value = 999
        do { _ = try overrides.read(); expect(false, "an out-of-range value is refused") }
        catch { expectEqual((error as? GraphicsDataError)?.message, "Show title: check the value for text-size.", "Windows text") }
    }

    /// Shift+F2 routes to the layout carrying that hotkey and cues it: the scene select comes first.
    @MainActor
    static func testShiftF2CuesTheLayoutWithThatHotkey() {
        let model = layoutModel(drop: ["ghost-row"])
        // The Guest row cues the Program subject (a routed guest), so the layout resolves before its scene select.
        model.rosterSlots = { [GraphicsRosterSlot(slotNumber: 1, sourceId: "zoom:100", derivedName: "Ada")] }
        model.programSubjectSourceId = { "zoom:100" }
        var selected: [String] = []
        model.selectPreviewScene = { selected.append($0); return false }
        guard let route = GraphicsHotkeys.route(function: 2, modifiers: [.shift], textFocus: false, rows: model.rows, layoutHotkeys: model.layoutHotkeys) else {
            expect(false, "Shift+F2 routes"); return
        }
        expectEqual(route, .layoutCue(layoutId: "two-up"), "to the Two-up preset")
        expect(model.perform(route, subjectSourceId: nil), "the route is performed")
        expectEqual(model.selectedLayoutId, "two-up", "and selects the layout")
        spin(until: { !selected.isEmpty }, limit: 2)
        expectEqual(selected, ["interview"], "the cue starts with the layout's scene select")
    }

    /// Review Focus 4: a preset keeps Windows' scene id (owner decision 2); on Mac scenes the cue is refused
    /// with Windows' text and nothing reaches the core.
    @MainActor
    static func testMissingSceneLayoutRefusesAtCue() {
        let (app, graphicsCore, takeCore) = takeAndGraphicsApp([])
        app.graphics.document = GraphicsDocument(json: documentJSON("windows-layouts.json"))
        let result = awaitMain(5) { await app.graphics.cueLayout("open").value }
        expectEqual(result?.reason, "Open: scene 'intro' is not in this show, so the layout was not cued. Edit the layout to pick a scene.", "Windows refusal")
        expectEqual(graphicsCore.commands.count, 0, "no graphics command")
        expectEqual(takeCore.bodies.count, 0, "no scene sync")
        expectEqual(app.previewSceneId, "s1", "Preview unchanged")
    }
}
