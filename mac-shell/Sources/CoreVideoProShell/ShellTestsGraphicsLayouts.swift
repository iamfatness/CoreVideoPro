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
        expectEqual(final?.rowSettings["leaderboard"]?.overrides.first?.key, "heading", "the override")
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
}
