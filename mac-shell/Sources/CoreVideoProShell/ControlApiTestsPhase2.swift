// macOS control parity, Phase 2 (docs/superpowers/plans/2026-11-21-mac-control-audio-phase2.md):
// RC6 Volt rundown and RC5 Companion. ContractParity skips *Tests.swift files.

import Foundation

extension ShellTests {

    @MainActor
    static var controlPhase2Cases: [(String, () -> Void)] {
        [
            ("control/volt-classify", testVoltRundownClassify),
            ("control/volt-load", testVoltRundownLoad),
            ("control/volt-preview", testVoltRundownPreview),
            ("control/volt-take", testVoltRundownTake),
        ]
    }

    /// Stages `<bin>/Volt Rundown/<name>` in the harness-isolated bin.
    static func stageVoltFile(_ name: String) {
        let path = MediaBin.root + "/" + MediaBin.voltRundownFolder + "/" + name
        try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent,
                                                 withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: path, contents: Data("staged".utf8))
    }

    /// Windows MediaBinClassifier + MediaBinServiceTests.LoadGroups_SeparatesVoltRundownCuts...
    static func testVoltRundownClassify() {
        expectEqual(MediaBin.classify(fileName: "a.mp4", relativePath: "Volt Rundown/a.mp4"), "volt-rundown", "staged mp4")
        expectEqual(MediaBin.classify(fileName: "slate-a.mp4", relativePath: "Volt Rundown/slate-a.mp4"), "volt-rundown",
                    "the Volt check runs before the slate check")
        expectEqual(MediaBin.classify(fileName: "a.mov", relativePath: "Volt Rundown/a.mov"), "stinger", ".mov is not a Volt clip")
        expectEqual(MediaBin.kindLabel("volt-rundown"), "Volt Rundown", "label")
        stageVoltFile("package-a123.mp4")
        stageVoltFile(".volt-working/partial.mp4")
        let volt = MediaBin.loadAssets().filter { $0.kind == "volt-rundown" }
        expectEqual(volt.map(\.name), ["package-a123"], "the staged clip is listed; the working folder is not")
        expectEqual(volt.first?.id, MediaBin.assetId(relativePath: "Volt Rundown/package-a123.mp4"), "Windows asset id")
    }

    @MainActor
    static func testVoltRundownLoad() {
        let model = AppModel()
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        for bad in ["Pkg", "pkg_a", "", String(repeating: "a", count: 73), "../x"] {
            expectEqual(invoke(s, "media.rundown.load", [bad]), .fail("Invalid staged clip key."), "bad key \(bad)")
        }
        expectEqual(invoke(s, "media.rundown.load", ["never-staged"]),
                    .fail("The staged clip is missing from the local Volt Rundown bin."), "missing clip")
        stageVoltFile("show-open.mp4")
        let id = MediaBin.assetId(relativePath: "Volt Rundown/show-open.mp4")
        expectEqual(invoke(s, "media.rundown.load", ["show-open"]), .withData(#"{"sourceId":"media:\#(id)"}"#),
                    "load refreshes the bin and answers the source id")
        expect(String(repeating: "a", count: 72).count == 72 && AppModel.isVoltClipKey(String(repeating: "a", count: 72)),
               "72 characters is a valid key")
    }

    @MainActor
    static func voltModel() -> (AppModel, FakeTakeCore) {
        let core = FakeTakeCore()
        let model = takeModel(core)   // Program s2, Preview s1
        stageVoltFile("clip-1.mp4")
        model.refreshMediaBin()
        if let index = model.scenes.firstIndex(where: { $0.id == "s1" }) {
            model.scenes[index].layers = [
                SceneLayer(id: "L1", slotId: 1), SceneLayer(id: "L2", slotId: 2),
                SceneLayer(id: "L3", slotId: 3, opacity: 0),
            ]
        }
        return (model, core)
    }

    @MainActor
    static func testVoltRundownPreview() {
        let (model, _) = voltModel()
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        let clipId = MediaBin.assetId(relativePath: "Volt Rundown/clip-1.mp4")
        expectEqual(invoke(s, "media.rundown.preview", ["gone", "s1", "L2"]),
                    .fail("The staged clip is missing. Send it from Volt again."), "missing")
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "s2", "L2"]),
                    .fail("The clip scene must be off Program before cueing another clip."), "scene on Program")
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "nope", ""]),
                    .fail("The clip scene or its editable video route does not exist."), "unknown scene")
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "s1", ""]),
                    .fail("The clip scene or its editable video route does not exist."), "two eligible layers: ambiguous")
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "s1", "L3"]),
                    .fail("The clip scene or its editable video route does not exist."), "an invisible layer is not eligible")
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "s1", "L2"]),
                    .fail("Clip playback is unavailable in this build"), "Mac: no clip playback in the core")
        model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]] as JSONObject)
        expectEqual(invoke(s, "media.rundown.preview", ["clip-1", "s1", "L2"]), .appliedPreview("s1"), "cued")
        let layers = model.scenes.first { $0.id == "s1" }?.layers ?? []
        expectEqual(layers.first { $0.id == "L2" }?.mediaAssetId, clipId, "the named layer carries the clip")
        expect(layers.first { $0.id == "L2" }?.slotId == nil, "and no slot")
        expectEqual(layers.first { $0.id == "L1" }?.slotId, 1, "the other layers are untouched")
        expect(layers.first { $0.id == "L1" }?.mediaAssetId == nil, "no clip on L1")
        expectEqual(model.previewSceneId, "s1", "the scene is on Preview")
    }

    @MainActor
    static func testVoltRundownTake() {
        let (model, core) = voltModel()
        let s = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        let clipId = MediaBin.assetId(relativePath: "Volt Rundown/clip-1.mp4")
        model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]] as JSONObject)
        expectEqual(invoke(s, "media.rundown.take", ["clip-1", "s1", "L2"]),
                    .fail("The staged Volt clip must be cued on Preview before Take."), "not cued yet")
        _ = invoke(s, "media.rundown.preview", ["clip-1", "s1", "L2"])
        expectEqual(invoke(s, "media.rundown.take", ["clip-1", "s1", "L2"]),
                    .fail("Wait for the staged clip to decode on Preview before Take."), "no decoded frame yet")
        func row(onProgram: Bool, onPreview: Bool, frames: Int) -> JSONObject {
            ["mediaSources": [["sourceId": "media:\(clipId)", "mediaAssetId": clipId, "state": "cued", "loop": false,
                               "onProgram": onProgram, "onPreview": onPreview, "positionMs": 0, "durationMs": 1000,
                               "decodedVideoFrames": frames]]] as JSONObject
        }
        model.applySnapshotForTesting(row(onProgram: false, onPreview: true, frames: 0))
        expectEqual(invoke(s, "media.rundown.take", ["clip-1", "s1", "L2"]),
                    .fail("Wait for the staged clip to decode on Preview before Take."), "0 decoded frames")
        model.applySnapshotForTesting(row(onProgram: true, onPreview: true, frames: 5))
        expectEqual(invoke(s, "media.rundown.take", ["clip-1", "s1", "L2"]),
                    .fail("The staged clip is already on Program. Take it off Program before replaying it."), "already on Program")
        expect(core.bodies.isEmpty, "no refusal reached the core")
        model.applySnapshotForTesting(row(onProgram: false, onPreview: true, frames: 5))
        expectEqual(invoke(s, "media.rundown.take", ["clip-1", "s1", "L2"]), .success, "ready: the ordinary Take runs")
        expectEqual(core.bodies.isEmpty ? nil : core.types().first, "begin-take-transition", "the button's Take batch")
        expectEqual(model.programSceneId, "s1", "the clip scene is on Program")
    }
}
