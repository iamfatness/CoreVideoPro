// macOS media clip playback, Phase 1 (plan 2026-11-16-mac-media-phase1). Kept in
// its own file, like ShellTestsTier0.swift and ShellTestsVcamUpdate.swift, so
// parallel branches extending ShellTests.swift do not collide line-for-line.

import Foundation

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var mediaCases: [(String, () -> Void)] {
        [
            ("media/background-picker-options", testBackgroundPickerListsStillsAndPlayableVideo),
            ("media-bin/classifier-matches-windows", testMediaClassifierMatchesWindows),
            ("media-bin/groups-order-kinds", testMediaGroupsOrderKindsAndOmitEmpty),
            ("media-bin/duration-label-rounding", testMediaDurationLabelUsesWindowsRounding),
            ("media-bin/root-isolated-under-harness", testMediaRootIsIsolatedUnderTheHarness),
            ("media-bin/remove-refused-in-use-else-trash", testMediaRemoveIsRefusedWhileInUseAndTrashesOtherwise),
            ("media-bin/import-status-windows-wording", testMediaImportStatusIsWindowsWording),
        ]
    }

    @MainActor
    private static func testBackgroundPickerListsStillsAndPlayableVideo() {
        func asset(_ id: String, _ file: String, _ kind: String) -> MediaAsset {
            MediaAsset(id: id, name: id, kind: kind, relativePath: kind + "/" + file,
                       filePath: "/bin/" + kind + "/" + file, naturalWidth: 0, naturalHeight: 0)
        }
        // Fixture: one of every class the bin can hold, in bin order.
        let bin = [asset("sting", "open.mp4", "stinger"), asset("mov", "loop.mov", "stinger"),
                   asset("web", "x.webm", "stinger"), asset("logo", "logo.png", "lower-third"),
                   asset("jpg", "card.jpg", "lower-third"), asset("bed", "bed.wav", "audio-bed"),
                   asset("slate", "slate-break.mp4", "slate")]
        let ids = MediaBin.backgroundOptions(bin).map(\.id)
        expectEqual(ids, ["sting", "mov", "logo", "jpg", "slate"],
                    "stills + mp4/mov in bin order; no webm (D1), no audio")
    }

    // ── Task 8: media bin parity (S3) ───────────────────────────────────────

    private static func testMediaClassifierMatchesWindows() {
        // MediaBinServiceTests.ClassifyFile_MapsSupportedExtensions, case for case.
        let cases: [(String, String)] = [("intro.mp4", "stinger"), ("clip.MOV", "stinger"),
            ("transition.webm", "stinger"), ("overlay.png", "lower-third"), ("title.JPG", "lower-third"),
            ("bed.wav", "audio-bed"), ("music.mp3", "audio-bed"), ("voice.m4a", "audio-bed")]
        for (file, kind) in cases {
            expectEqual(MediaBin.classify(fileName: file, relativePath: file), kind, file)
        }
        expectEqual(MediaBin.classify(fileName: "show-open.png", relativePath: "slates/show-open.png"),
                    "slate", "slates folder")
        expectEqual(MediaBin.classify(fileName: "slate-break.mp4", relativePath: "assets"), "slate", "slate prefix")
        expect(MediaBin.classify(fileName: "readme.txt", relativePath: "readme.txt") == nil,
               "unknown extension is refused (was lower-third)")
        expectEqual(MediaBin.assetId(relativePath: "stingers\\brand.mp4"),
                    MediaBin.assetId(relativePath: "stingers/brand.mp4"), "id separator-stable")
    }

    private static func testMediaGroupsOrderKindsAndOmitEmpty() {
        // MediaBinServiceTests.GroupAssets_OrdersKindsAndOmitsEmptyGroups.
        func a(_ id: String, _ kind: String) -> MediaAsset {
            MediaAsset(id: id, name: id, kind: kind, relativePath: kind + "/" + id,
                       filePath: "/b/" + id, naturalWidth: 0, naturalHeight: 0)
        }
        let groups = MediaBin.groups([a("bed", "audio-bed"), a("st", "stinger"), a("ov", "lower-third")])
        expectEqual(groups.map(\.kind), ["stinger", "lower-third", "audio-bed"], "Windows kind order")
        expectEqual(groups.first?.label ?? "", "Stinger", "label")
        expectEqual(groups.last?.label ?? "", "Audio bed", "label")
        expect(!groups.contains { $0.kind == "slate" }, "empty group omitted")
    }

    private static func testMediaDurationLabelUsesWindowsRounding() {
        func d(_ ms: Int?) -> String {
            var asset = MediaAsset(id: "x", name: "x", kind: "stinger", relativePath: "s/x.mp4",
                                   filePath: "/b/s/x.mp4", naturalWidth: 0, naturalHeight: 0)
            asset.durationMs = ms
            return asset.durationLabel
        }
        expectEqual(d(3000), "3s", "whole seconds drop the decimal")
        expectEqual(d(1250), "1.2s", "banker's rounding, like .NET Math.Round")
        expectEqual(d(1350), "1.4s", "banker's rounding the other way")
        expectEqual(d(nil), "", "unknown duration is blank")
    }

    @MainActor
    private static func testMediaRootIsIsolatedUnderTheHarness() {
        expect(MediaBin.root != MediaBin.realRoot, "the harness never touches the operator's bin")
        expect(MediaBin.root.hasPrefix(NSTemporaryDirectory()), "harness bin is under the temp dir")
    }

    @MainActor
    private static func testMediaRemoveIsRefusedWhileInUseAndTrashesOtherwise() {
        // SAFETY (added to the plan's body): this test creates files in the bin and deletes the bin
        // root when it is done. If isolation ever regressed, that would be the operator's real bin.
        guard MediaBin.root != MediaBin.realRoot, MediaBin.root.hasPrefix(NSTemporaryDirectory()) else {
            expect(false, "REFUSED to run: the media bin is not isolated under the harness")
            return
        }
        // Fixture: real files in the harness bin, so moveToTrash does real I/O.
        let fm = FileManager.default
        let dir = MediaBin.root + "/stinger"
        try? fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
        for name in ["used.mp4", "free.mp4"] { fm.createFile(atPath: dir + "/" + name, contents: Data([1])) }
        let model = sceneEditModel()                       // s1 "Intro" (preview), s2 "Panel" (program)
        model.refreshMediaBin()
        let used = model.mediaAssets.first { $0.name == "used" }!
        let free = model.mediaAssets.first { $0.name == "free" }!
        model.scenes[1].backgroundAssetId = used.id        // "Panel" background
        model.scenes[0].layers.append(SceneLayer(id: "s1-layer-9", slotId: nil, mediaAssetId: used.id))
        model.removeMediaAsset(used.id)
        expectEqual(model.mediaStatus,
                    "'used' is in use (scene Intro, scene Panel background). Unassign it there before removing.",
                    "refused, every reference named")
        expect(fm.fileExists(atPath: used.filePath), "a refused remove leaves the file")
        model.selectMediaAsset(free.id)
        model.removeMediaAsset(free.id)
        expectEqual(model.mediaStatus, "Removed 'free' (recoverable from the media .trash folder).", "removed")
        expect(!fm.fileExists(atPath: free.filePath), "moved out of the bin")
        expect(fm.fileExists(atPath: MediaBin.root + "/.trash/free.mp4"), "into .trash")
        expectEqual(model.selectedAssetId, "", "selection cleared")
        expect(!model.mediaAssets.contains { $0.id == free.id }, "bin refreshed")
        fm.createFile(atPath: dir + "/free.mp4", contents: Data([2]))     // same name again
        model.refreshMediaBin()
        model.removeMediaAsset(model.mediaAssets.first { $0.name == "free" }!.id)
        expect(fm.fileExists(atPath: MediaBin.root + "/.trash/free-1.mp4"), "unique name in .trash")
        try? fm.removeItem(atPath: MediaBin.root)
    }

    @MainActor
    private static func testMediaImportStatusIsWindowsWording() {
        // SAFETY (added to the plan's body): this test creates files in the bin and deletes the bin
        // root when it is done. If isolation ever regressed, that would be the operator's real bin.
        guard MediaBin.root != MediaBin.realRoot, MediaBin.root.hasPrefix(NSTemporaryDirectory()) else {
            expect(false, "REFUSED to run: the media bin is not isolated under the harness")
            return
        }
        let src = NSTemporaryDirectory() + "cvp-import-\(getpid())"
        try? FileManager.default.createDirectory(atPath: src, withIntermediateDirectories: true)
        for name in ["a.mp4", "b.png", "notes.txt"] {
            FileManager.default.createFile(atPath: src + "/" + name, contents: Data([1]))
        }
        let model = AppModel()
        model.importMedia(urls: [URL(fileURLWithPath: src + "/a.mp4")])
        expectEqual(model.mediaStatus, "a imported and selected", "one file")
        expectEqual(model.mediaAssets.first { $0.id == model.selectedAssetId }?.name ?? "", "a", "selected")
        model.importMedia(urls: [URL(fileURLWithPath: src + "/b.png"), URL(fileURLWithPath: src + "/notes.txt")])
        expectEqual(model.mediaStatus, "b imported and selected", "txt refused, png imported")
        model.importMedia(urls: [URL(fileURLWithPath: src + "/notes.txt")])
        expectEqual(model.mediaStatus, "No supported media files imported", "nothing usable")
        model.importMedia(urls: [])
        expectEqual(model.mediaStatus, "Media import canceled", "cancel")
        try? FileManager.default.removeItem(atPath: src)
        try? FileManager.default.removeItem(atPath: MediaBin.root)
    }
}
