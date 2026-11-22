// macOS media clip playback, Phase 1 (plan 2026-11-16-mac-media-phase1). Kept in
// its own file, like ShellTestsTier0.swift and ShellTestsVcamUpdate.swift, so
// parallel branches extending ShellTests.swift do not collide line-for-line.

import AVFoundation
import Combine
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
            ("media-tab/summary-counts-by-kind", testMediaSummaryCountsByKind),
            ("media-tab/durations-probe-real-files", testMediaDurationsProbeRealFiles),
            ("media-wire/media-sources-row-shape", testMediaSourcesDecodeTheCoreRowShape),
            ("media-wire/media-sources-apply-only-when-changed", testMediaSourcesApplyOnlyWhenChanged),
            ("media-wire/clip-capability-from-profile", testClipCapabilityComesFromTheProfile),
            ("media-wire/route-wire-drops-retired-fields", testMediaRouteWireDropsRetiredFieldsAndSendsLoop),
            ("media-wire/logo-bug-route-drops-retired-fields", testLogoBugRouteWireDropsRetiredFields),
            ("media-cue/adds-full-canvas-layer-refuses-duplicate", testCueInPreviewAddsAFullCanvasMediaLayerAndRefusesADuplicate),
            ("media-cue/refuses-clip-without-capability", testCueRefusesAClipWhenTheBuildCannotPlayClipsButCuesStills),
            ("media-cue/refuses-solo-preview", testCueRefusesASoloPreview),
            ("media-cue/selected-summary-windows-wording", testSelectedSummaryIsWindowsWording),
            ("media-play/resolve-tap", testResolveTap),
            ("media-play/transport-state-and-on-air", testResolveTransportStateAndOnAir),
            ("media-play/bin-playback-projection", testMediaBinPlaybackProjection),
            ("media-play/transport-gesture-ledger", testMediaTransportGestureLedger),
            ("media-play/one-shot-gestures-retry-only-latest", testPlayMediaAssetSendsOneShotGesturesAndRetriesOnlyTheLatest),
            ("media-play/busy-gesture-gives-up-visibly", testAGestureThatStaysBusyGivesUpVisibly),
            ("media-play/off-program-tap-is-local-audition", testAnOffProgramTapIsALocalAuditionAndSendsNothing),
            ("media-play/armed-retry-dropped-by-newer-tap", testAnArmedRetryIsDroppedWhenANewerTapLands),
            ("media-play/remote-rejection-sends-once", testARemoteRejectionSendsOnceAndSaysSo),
            ("media-play/toggle-without-selection", testToggleWithoutASelectionSaysSo),
            ("media-take/promotion-pure-rules", testTakePromotionPureRules),
            ("media-take/rollback-restores-selection-a-live-clip-took", testTakeRollbackRestoresTheSelectionAClipThatWentLiveTookOver),
            ("media-take/rollback-keeps-paused-program-clip-paused", testTakeRollbackKeepsAPausedProgramClipPausedWithAnHonestToggle),
            ("media-take/rollback-rolling-clip-back-on-pause-program", testTakeRollbackPutsARollingClipThatLeftProgramBackOnPauseProgram),
            ("media-take/refused-rollback-leaves-selection", testTakeRefusedRollbackLeavesTheSelectionAlone),
            ("media-take/rollback-operator-pick-left-program", testTakeRollbackGivesAnOperatorPickThatLeftProgramOffProgramWording),
            ("media-take/success-promotes-clip-never-still", testASuccessfulTakePromotesTheClipThatWentLiveButNeverAStill),
            ("media-take/rollback-reads-on-air-row-not-saved-flag", testTakeRollbackReadsTheOnAirRowNotTheSavedFlag),
            ("media-review/cue-refuses-audio-only", testCueRefusesAnAudioOnlyAsset),
            ("media-review/refused-gesture-status-shown-once", testARefusedGestureIsShownOnceNotPinned),
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

    // ── Task 9: the Media tab, durations and the local preview (S3) ─────────

    private static func testMediaSummaryCountsByKind() {
        func a(_ f: String) -> MediaAsset {
            MediaAsset(id: f, name: f, kind: "x", relativePath: f, filePath: "/b/" + f,
                       naturalWidth: 0, naturalHeight: 0)
        }
        expectEqual(MediaBin.summary([a("a.mp4"), a("b.mov"), a("c.png"), a("d.wav")]),
                    "4 assets · 3 clips · 1 still · 1 audio",
                    "audio counts as a clip for playback AND as audio")
        expectEqual(MediaBin.summary([]), "Bin is empty — import stills, stingers or audio beds.", "empty")
    }

    /// 1-channel 44.1 kHz silent AAC, `seconds` long, written with AVAssetWriter.
    static func writeSilentM4a(path: String, seconds: Double) -> Bool {
        let url = URL(fileURLWithPath: path)
        try? FileManager.default.removeItem(at: url)
        guard let writer = try? AVAssetWriter(outputURL: url, fileType: .m4a) else { return false }
        let rate = 44_100.0
        let input = AVAssetWriterInput(mediaType: .audio, outputSettings: [
            AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: rate, AVNumberOfChannelsKey: 1,
            AVEncoderBitRateKey: 64_000])
        guard writer.canAdd(input) else { return false }
        writer.add(input)
        guard writer.startWriting() else { return false }
        writer.startSession(atSourceTime: .zero)
        var format = AudioStreamBasicDescription(
            mSampleRate: rate, mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, mBytesPerPacket: 4,
            mFramesPerPacket: 1, mBytesPerFrame: 4, mChannelsPerFrame: 1, mBitsPerChannel: 32, mReserved: 0)
        var description: CMAudioFormatDescription?
        guard CMAudioFormatDescriptionCreate(allocator: nil, asbd: &format, layoutSize: 0, layout: nil,
                                             magicCookieSize: 0, magicCookie: nil, extensions: nil,
                                             formatDescriptionOut: &description) == noErr,
              let description else { return false }
        let total = Int(seconds * rate)
        var written = 0
        let deadline = Date().addingTimeInterval(20)
        while written < total, Date() < deadline {
            guard input.isReadyForMoreMediaData else { Thread.sleep(forTimeInterval: 0.001); continue }
            let count = min(4_410, total - written)
            var block: CMBlockBuffer?
            guard CMBlockBufferCreateWithMemoryBlock(
                    allocator: nil, memoryBlock: nil, blockLength: count * 4, blockAllocator: nil,
                    customBlockSource: nil, offsetToData: 0, dataLength: count * 4,
                    flags: kCMBlockBufferAssureMemoryNowFlag, blockBufferOut: &block) == kCMBlockBufferNoErr,
                  let block, CMBlockBufferFillDataBytes(with: 0, blockBuffer: block, offsetIntoDestination: 0,
                                                        dataLength: count * 4) == kCMBlockBufferNoErr
            else { return false }
            var sample: CMSampleBuffer?
            guard CMAudioSampleBufferCreateReadyWithPacketDescriptions(
                    allocator: nil, dataBuffer: block, formatDescription: description, sampleCount: count,
                    presentationTimeStamp: CMTime(value: CMTimeValue(written), timescale: CMTimeScale(rate)),
                    packetDescriptions: nil, sampleBufferOut: &sample) == noErr,
                  let sample, input.append(sample) else { return false }
            written += count
        }
        input.markAsFinished()
        writer.endSession(atSourceTime: CMTime(value: CMTimeValue(total), timescale: CMTimeScale(rate)))
        let done = DispatchSemaphore(value: 0)
        writer.finishWriting { done.signal() }
        _ = done.wait(timeout: .now() + 20)
        return written == total && writer.status == .completed
    }

    @MainActor
    private static func testMediaDurationsProbeRealFiles() {
        guard MediaBin.root != MediaBin.realRoot, MediaBin.root.hasPrefix(NSTemporaryDirectory()) else {
            expect(false, "REFUSED to run: the media bin is not isolated under the harness")
            return
        }
        // Fixture: a 1.0 s silent AAC .m4a and a PNG, both real files in the harness bin.
        let fm = FileManager.default
        let audioDir = MediaBin.root + "/audio-bed", stillDir = MediaBin.root + "/lower-third"
        try? fm.createDirectory(atPath: audioDir, withIntermediateDirectories: true)
        try? fm.createDirectory(atPath: stillDir, withIntermediateDirectories: true)
        expect(writeSilentM4a(path: audioDir + "/second.m4a", seconds: 1.0), "fixture: the .m4a was written")
        fm.createFile(atPath: stillDir + "/card.png", contents: Data([0x89, 0x50, 0x4E, 0x47]))
        let assets = MediaBin.loadAssets()
        let clip = assets.first { $0.name == "second" }
        let still = assets.first { $0.name == "card" }
        expect(clip != nil && still != nil, "fixture: both assets are in the bin")
        var probedOnMain: [Bool] = []
        MediaBin.probeThreadObserverForTesting = { probedOnMain.append($0) }
        defer { MediaBin.probeThreadObserverForTesting = nil }
        let box = ProbeBox()
        Task.detached { box.set(await MediaBin.probeDurations(assets)) }
        spin(until: { box.value != nil }, limit: 10)
        let durations = box.value ?? [:]
        let measured = durations[clip?.id ?? ""] ?? -1
        expect(abs(measured - 1000) <= 25, "the .m4a probes to 1000 ms +/- 25 (AAC priming), got \(measured)")
        expect(durations[still?.id ?? ""] == nil, "a still has no duration entry")
        expectEqual(durations.count, 1, "exactly the one playable asset was probed")
        expectEqual(probedOnMain, [false], "the probe ran once, off the main thread")
        // The model writes the probe back: one publish, on the main actor.
        let model = AppModel()
        model.refreshMediaBin()
        spin(until: { model.mediaAssets.first { $0.name == "second" }?.durationMs != nil }, limit: 10)
        expectEqual(model.mediaAssets.first { $0.name == "second" }?.durationLabel ?? "", "1s",
                    "refreshMediaBin fills durationMs from the probe")
        try? fm.removeItem(atPath: MediaBin.root)
    }

    // ── Task 10: mediaSources[], the capability flag, the wire cleanup (S1) ─

    /// ShellTests.runTake is file-private; the same thing for the media cases.
    @MainActor
    @discardableResult
    static func mediaRunTake(_ model: AppModel, wait: Bool = true) -> TakeBox {
        let box = TakeBox()
        Task { @MainActor in box.result = await model.take() }
        if wait { spin(until: { box.result != nil }, limit: 3) }
        return box
    }

    private static func testMediaSourcesDecodeTheCoreRowShape() {
        // Fixture: two rows exactly as CoreStateCapture.cpp:1199-1217 writes them (numbers are doubles).
        let snapshot: JSONObject = ["mediaSources": [
            ["sourceId": "media:clip", "mediaAssetId": "clip", "state": "live", "loop": false,
             "onProgram": true, "onPreview": false, "positionMs": 1250.0, "durationMs": 3000.0,
             "decodedVideoFrames": 75.0, "lastDecodedAgeMs": 4.0, "presentedVideoFrames": 74.0,
             "lastPresentedAgeMs": 9.0, "videoQueued": 1.0, "decoderRestarts": 0.0],
            ["sourceId": "background:bg", "mediaAssetId": "bg", "state": "live", "loop": true,
             "onProgram": true, "onPreview": true, "positionMs": 400.0, "durationMs": -1.0],
            ["mediaAssetId": "orphan", "state": "live"],                       // no sourceId: dropped
        ]]
        let rows = MediaSourceRow.parse(snapshot) ?? []
        expectEqual(rows.count, 2, "row without sourceId dropped")
        expectEqual(rows[0].state, "live", "state"); expectEqual(rows[0].positionMs, 1250, "position")
        expectEqual(rows[0].decodedVideoFrames, 75, "#701 trace read")
        expectEqual(rows[1].durationMs, -1, "unknown duration stays -1, never 0")
        expect(rows[1].decodedVideoFrames == nil, "absent trace field is nil (older core)")
        expect(MediaSourceRow.parse([:]) == nil, "absent node -> nil")
        expectEqual(MediaSourceRow.parse(["mediaSources": [JSONObject]()])?.count ?? -1, 0, "present-empty -> []")
    }

    @MainActor
    private static func testMediaSourcesApplyOnlyWhenChanged() {
        let model = AppModel()
        let snap: JSONObject = ["mediaSources": [["sourceId": "media:c", "mediaAssetId": "c", "state": "live",
                                                  "onProgram": true]]]
        var publishes = 0
        let sink = model.$mediaSources.dropFirst().sink { _ in publishes += 1 }
        model.applySnapshotForTesting(snap); model.applySnapshotForTesting(snap); model.applySnapshotForTesting(snap)
        expectEqual(publishes, 1, "identical snapshots publish once")
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:c", "mediaAssetId": "c",
                                                         "state": "paused", "onProgram": true]]])
        expectEqual(publishes, 2, "a state change publishes")
        model.applySnapshotForTesting(["programBlackout": false])
        expectEqual(model.mediaSources.first?.state ?? "", "paused", "a snapshot without the node keeps the rows")
        sink.cancel()
    }

    @MainActor
    private static func testClipCapabilityComesFromTheProfile() {
        let model = AppModel()
        expect(!model.mediaClipPlaybackAvailable, "unknown -> unavailable")
        model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "omitted"]]]])
        expect(!model.mediaClipPlaybackAvailable, "omitted -> unavailable")
        model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]])
        expect(model.mediaClipPlaybackAvailable, "available")
    }

    @MainActor
    private static func testMediaRouteWireDropsRetiredFieldsAndSendsLoop() {
        let core = FakeTakeCore()
        let model = takeModel(core)                         // Program s2, Preview s1
        model.mediaAssets = [MediaAsset(id: "clip", name: "Clip", kind: "stinger",
            relativePath: "stinger/clip.mp4", filePath: "/b/stinger/clip.mp4", naturalWidth: 0, naturalHeight: 0),
            MediaAsset(id: "bg-asset", name: "BG", kind: "stinger", relativePath: "stinger/bg.mp4",
            filePath: "/b/stinger/bg.mp4", naturalWidth: 0, naturalHeight: 0)]
        model.scenes[0].layers.append(SceneLayer(id: "s1-layer-1", slotId: nil, mediaAssetId: "clip"))
        model.selectedAssetId = "clip"
        _ = mediaRunTake(model)                                  // s1 becomes Program
        let cmds = core.bodies.first?["commands"] as? [JSONObject] ?? []
        let load = cmds.first { $0["type"] as? String == "load-scene-graph" } ?? [:]
        let media = (load["routes"] as? [JSONObject] ?? []).first { $0["mediaAssetId"] as? String == "clip" } ?? [:]
        expect(media["mediaPlaybackKey"] == nil, "retired mediaPlaybackKey not sent")
        expect(media["mediaAssetPlaying"] == nil, "retired mediaAssetPlaying not sent")
        expectEqual(media["mediaAssetLoop"] as? Bool, false, "mediaAssetLoop sent, false for a stinger")
        let bg = load["background"] as? JSONObject ?? [:]
        expect(bg["playing"] == nil, "background carries no playing flag")
        expectEqual(bg["mediaAssetPath"] as? String ?? "", "/b/stinger/bg.mp4", "background path")
        let sel = cmds.first { $0["type"] as? String == "set-media-playback" } ?? [:]
        expectEqual(sel["mediaAssetId"] as? String ?? "", "clip", "selection rides the batch")
        expectEqual(sel["mediaAssetPath"] as? String ?? "", "/b/stinger/clip.mp4", "with its path")
        expect(sel["payload"] == nil, "root-level command")
    }

    /// ADDED (the plan's mutation "keep mediaAssetPlaying on the logo-bug route only" stayed GREEN
    /// against testMediaRouteWireDropsRetiredFieldsAndSendsLoop, which has no bug route). The logo
    /// bug is emitted at TWO sites: after the edited canvas layers, and after the layout preset.
    @MainActor
    private static func testLogoBugRouteWireDropsRetiredFields() {
        let model = sceneEditModel()          // s1 has edited layers; s2 "two-up" has none (preset path)
        model.logoBug = MediaAsset(id: "bug", name: "Bug", kind: "lower-third", relativePath: "lower-third/bug.png",
                                   filePath: "/b/lower-third/bug.png", naturalWidth: 400, naturalHeight: 200)
        for sceneId in ["s1", "s2"] {
            let routes = model.buildRoutes(for: sceneId, isProgram: true)
            let bug = routes.first { $0["mediaAssetId"] as? String == "bug" }
            expect(bug != nil, "\(sceneId): the logo bug rides the scene as a route")
            expect(bug?["mediaPlaybackKey"] == nil, "\(sceneId): bug route sends no mediaPlaybackKey")
            expect(bug?["mediaAssetPlaying"] == nil, "\(sceneId): bug route sends no mediaAssetPlaying")
            expectEqual(bug?["mediaAssetLoop"] as? Bool, false, "\(sceneId): bug route sends mediaAssetLoop false")
        }
    }

    // ── Task 11: Cue in Preview, the selected-asset card, playout status (S1) ─

    @MainActor
    static func cueModel() -> AppModel {
        let model = sceneEditModel()                        // Preview s1 "Intro" (1 slot layer), Program s2
        model.mediaAssets = [
            MediaAsset(id: "clip", name: "Sting", kind: "stinger", relativePath: "stinger/s.mp4",
                       filePath: "/b/stinger/s.mp4", naturalWidth: 0, naturalHeight: 0),
            MediaAsset(id: "logo", name: "Logo", kind: "lower-third", relativePath: "lower-third/l.png",
                       filePath: "/b/lower-third/l.png", naturalWidth: 0, naturalHeight: 0),
            MediaAsset(id: "bg-asset", name: "BG", kind: "lower-third", relativePath: "lower-third/bg.png",
                       filePath: "/b/lower-third/bg.png", naturalWidth: 0, naturalHeight: 0)]
        return model
    }

    @MainActor
    private static func testCueInPreviewAddsAFullCanvasMediaLayerAndRefusesADuplicate() {
        let model = cueModel()
        model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]])
        expectEqual(model.cueSelectedInPreview(), "Select a media asset before adding it to Preview", "no selection")
        model.selectedAssetId = "clip"
        expectEqual(model.cueSelectedInPreview(),
                    "Sting cued in Preview; Take to start playback on Program", "clip wording")
        let layer = model.scenes[0].layers.last!
        expectEqual(layer.mediaAssetId, "clip", "media layer"); expectEqual(layer.id, "s1-layer-1", "first free index")
        expectEqual([layer.x, layer.y, layer.width, layer.height], [0, 0, 1, 1], "full canvas")
        expectEqual(layer.fitMode, "fill", "Windows default fit")
        expectEqual(model.scenes[0].layers.count, 2, "appended on top of the existing layer")
        expectEqual(model.cueSelectedInPreview(), "Sting is already cued in Preview", "duplicate refused")
        expectEqual(model.scenes[0].layers.count, 2, "no second layer")
        model.selectedAssetId = "logo"
        expectEqual(model.cueSelectedInPreview(), "Logo cued in Preview; Take to show it on Program", "still wording")
    }

    @MainActor
    private static func testCueRefusesAClipWhenTheBuildCannotPlayClipsButCuesStills() {
        let model = cueModel()                              // no capability snapshot -> unavailable
        model.selectedAssetId = "clip"
        expectEqual(model.cueSelectedInPreview(), "Clip playback is unavailable in this build", "clip refused")
        expectEqual(model.scenes[0].layers.count, 1, "nothing added")
        model.selectedAssetId = "logo"
        expect(model.cueSelectedInPreview().hasPrefix("Logo cued in Preview"), "stills still cue")
    }

    @MainActor
    private static func testCueRefusesASoloPreview() {
        let model = cueModel()
        model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]])
        model.selectedAssetId = "logo"
        model.previewSceneId = "mv-solo-a"                  // a solo scene id (AppModel.isSoloScene)
        expectEqual(model.cueSelectedInPreview(), "Cue in Preview needs an operator scene in Preview.", "solo refused")
    }

    private static func testSelectedSummaryIsWindowsWording() {
        typealias P = MediaRoutePlayback
        expectEqual(P.selectedSummary(name: nil, isOnProgram: false, isCuedInPreview: false, playing: false),
                    "No media asset selected", "none")
        expectEqual(P.selectedSummary(name: "Sting", isOnProgram: true, isCuedInPreview: false, playing: true),
                    "Playing Sting on Program", "live")
        expectEqual(P.selectedSummary(name: "Sting", isOnProgram: true, isCuedInPreview: true, playing: false),
                    "Sting paused on Program", "Program wins over Preview")
        expectEqual(P.selectedSummary(name: "Sting", isOnProgram: false, isCuedInPreview: true, playing: false),
                    "Sting cued in Preview", "cued")
        expectEqual(P.selectedSummary(name: "Sting", isOnProgram: false, isCuedInPreview: false, playing: false),
                    "Sting ready to cue", "idle")
    }

    // ── Task 12: Play/Pause - tap rules, projection, gesture ledger (S2) ────

    static func row(_ source: String, _ asset: String, _ state: String,
                            onProgram: Bool = true, onPreview: Bool = false) -> MediaSourceRow {
        MediaSourceRow(sourceId: source, mediaAssetId: asset, state: state, loop: source.hasPrefix("background:"),
                       onProgram: onProgram, onPreview: onPreview, positionMs: 0, durationMs: -1,
                       decodedVideoFrames: nil, lastDecodedAgeMs: nil, presentedVideoFrames: nil,
                       lastPresentedAgeMs: nil, videoQueued: nil, decoderRestarts: nil)
    }

    // MediaRoutePlaybackServiceTests (Windows) — names kept.
    private static func testResolveTap() {
        typealias P = MediaRoutePlayback
        expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: "live"), .pause, "ResolveTap_ALiveProgramClipPauses")
        expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: "paused"), .resume, "ResolveTap_APausedOrEndedProgramClipResumes/paused")
        expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: "ended"), .resume, "…/ended")
        expectEqual(P.resolveTap(isOnProgram: false, isLooping: false, state: "live"), .select, "ResolveTap_AClipNotOnProgramIsJustSelected")
        expectEqual(P.resolveTap(isOnProgram: true, isLooping: true, state: "live"), .select, "ResolveTap_ALoopOnProgramIsJustSelectedNeverPaused")
        expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: nil), .select, "ResolveTap_AnUnknownOrMissingStateIsJustSelected/nil")
        expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: "unavailable"), .select, "…/unavailable")
    }
    private static func testResolveTransportStateAndOnAir() {
        typealias P = MediaRoutePlayback
        expectEqual(P.resolveTransportState("clip", rows: [row("media:clip", "clip", "paused")]), "paused",
                    "ResolveTransportState_ReturnsTheRowsStateForTheAsset")
        expect(P.resolveTransportState("clip", rows: []) == nil, "ResolveTransportState_ReturnsNullWhenTheCoreHasNoRow")
        expect(P.resolveTransportState(nil, rows: [row("media:clip", "clip", "live")]) == nil, "…/nil asset")
        let both = [row("background:clip", "clip", "live"), row("media:clip", "clip", "paused")]
        expectEqual(P.resolveTransportState("clip", rows: both), "paused",
                    "ResolveTransportState_TheRouteRowWinsWhenAnAssetIsBothARouteAndABackground")
        expectEqual(P.resolveTransportState("bg", rows: [row("background:bg", "bg", "live")]), "live",
                    "ResolveTransportState_ABackgroundRowStillAnswersWhenTheAssetHasNoRoute")
        expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: P.resolveTransportState("clip", rows: both)),
                    .resume, "ResolveTap_ALiveBackgroundRowCannotMakeAPausedClipLookPausable")
        expect(P.isPlayingOnAir("clip", rows: [row("media:clip", "clip", "live")]), "IsPlayingOnAir_ReadsTheCoreRowLive")
        for s in ["paused", "cued", "ended"] {
            expect(!P.isPlayingOnAir("clip", rows: [row("media:clip", "clip", s)]), "IsPlayingOnAir_ReadsTheCoreRowPausedAsNotPlaying/\(s)")
        }
        expect(!P.isPlayingOnAir("clip", rows: []), "IsPlayingOnAir_ReadsTheCoreRowAbsentAsNotPlaying")
        expect(!P.isPlayingOnAir("clip", rows: [row("media:other", "other", "live")]), "…/another asset")
        expect(P.isPlayingOnAir("bg", rows: [row("background:bg", "bg", "live")]), "IsPlayingOnAir_ALoopingBackgroundRowAnswersForItsAsset")
        expectEqual(P.actionLabel(isOnProgram: true, playing: true), "Pause Program", "label")
        expectEqual(P.actionLabel(isOnProgram: true, playing: false), "Resume Program", "label")
        expectEqual(P.actionLabel(isOnProgram: false, playing: true), "Pause audition", "label")
        expectEqual(P.actionLabel(isOnProgram: false, playing: false), "Audition", "label")
    }

    // MediaBinPlaybackProjectionTests (Windows) — names kept, bodies 1:1.
    private static func testMediaBinPlaybackProjection() {
        typealias J = MediaBinPlaybackProjection
        var r = J.resolve(rows: [row("media:clip", "clip", "cued", onProgram: false, onPreview: true)],
                          lastLiveSignature: "", lastSelectedTransportState: nil,
                          selectedId: "clip", selectedName: "Clip", selectedPlaying: true)
        expect(!r.rewriteSelection && r.selectedPlaying && r.selectedStatus == nil, "ACuedPreviewRowNeverClearsALocalAudition")
        r = J.resolve(rows: [], lastLiveSignature: "", lastSelectedTransportState: nil,
                      selectedId: "clip", selectedName: "Clip", selectedPlaying: true)
        expect(!r.rewriteSelection && r.selectedPlaying, "NoRowAtAllNeverClearsALocalAudition")
        r = J.resolve(rows: [row("media:clip", "clip", "live")], lastLiveSignature: "", lastSelectedTransportState: nil,
                      selectedId: "clip", selectedName: "Clip", selectedPlaying: false)
        expect(r.rewriteSelection && r.selectedPlaying, "AProgramRowOwnsTheSelectedFlag/live")
        expectEqual(r.selectedStatus ?? "", "Playing Clip on Program", "…/live status")
        r = J.resolve(rows: [row("media:clip", "clip", "paused")], lastLiveSignature: "", lastSelectedTransportState: nil,
                      selectedId: "clip", selectedName: "Clip", selectedPlaying: true)
        expect(r.rewriteSelection && !r.selectedPlaying, "AProgramRowOwnsTheSelectedFlag/paused")
        expectEqual(r.selectedStatus ?? "", "Clip paused on Program", "…/paused status")
        r = J.resolve(rows: [row("media:clip", "clip", "ended")], lastLiveSignature: "", lastSelectedTransportState: "paused",
                      selectedId: "clip", selectedName: "Clip", selectedPlaying: false)
        expect(r.rewriteSelection && !r.selectedPlaying, "TheStatusIsRewrittenWhenTheSTATEMovesEvenThoughTheBooleanDoesNot")
        expectEqual(r.selectedStatus ?? "", "Clip ended on Program", "…/status")
        expectEqual(r.selectedTransportState ?? "", "ended", "…/state")
        r = J.resolve(rows: [row("media:clip", "clip", "live")], lastLiveSignature: "clip", lastSelectedTransportState: "live",
                      selectedId: "clip", selectedName: "Clip", selectedPlaying: true)
        expect(!r.rewriteSelection && !r.rewriteBinScalars, "ASettledSelectionWritesNothing")
        let rows = [row("media:clip", "clip", "live")]
        let first = J.resolve(rows: rows, lastLiveSignature: "", lastSelectedTransportState: nil,
                              selectedId: nil, selectedName: nil, selectedPlaying: false)
        expect(first.rewriteBinScalars, "TheBinScalarsAreRewrittenOnlyWhenTheLiveSetMoves/first")
        expectEqual(first.liveSignature, "clip", "…/signature")
        let second = J.resolve(rows: rows, lastLiveSignature: first.liveSignature, lastSelectedTransportState: nil,
                               selectedId: nil, selectedName: nil, selectedPlaying: false)
        expect(!second.rewriteBinScalars, "…/second")
        let other = [row("media:other", "other", "live")]
        expect(J.binRowIsPlaying("other", supportsPlayback: true, rows: other, selectedId: nil, selectedPlaying: false),
               "ABinRowsIndicatorIsTheSameExpressionAsTheOperatorRebuild/on air")
        expect(J.binRowIsPlaying("clip", supportsPlayback: true, rows: other, selectedId: "clip", selectedPlaying: true),
               "…/local audition")
        expect(!J.binRowIsPlaying("other", supportsPlayback: false, rows: other, selectedId: nil, selectedPlaying: false),
               "…/still")
        r = J.resolve(rows: [row("background:clip", "clip", "live"), row("media:clip", "clip", "paused")],
                      lastLiveSignature: "", lastSelectedTransportState: nil,
                      selectedId: "clip", selectedName: "Clip", selectedPlaying: true)
        expect(r.rewriteSelection && !r.selectedPlaying, "ALiveBackgroundRowDoesNotSpeakForAClipThatIsAlsoARoute")
        expectEqual(r.selectedStatus ?? "", "Clip paused on Program", "…/status")
    }

    // MediaTransportGestureLedgerTests (Windows) — names kept.
    private static func testMediaTransportGestureLedger() {
        var l = MediaTransportGestureLedger()
        let pause = l.claim("clip")
        expect(l.shouldSend("clip", pause), "ASupersededRetryIsRefusedBeforeItSends/a")
        expectEqual(l.onSkipped("clip", pause, attempt: 0), .retry, "…/b")
        let resume = l.claim("clip")
        expect(l.shouldSend("clip", resume), "…/c"); expect(l.release("clip", resume), "…/d")
        expect(!l.shouldSend("clip", pause), "…/stale pause refused")
        l = MediaTransportGestureLedger()
        let p2 = l.claim("clip"), r2 = l.claim("clip")
        expect(!l.shouldSend("clip", p2) && l.shouldSend("clip", r2), "ANewerTapSupersedesAnOlderOneWhileItIsStillInFlight")
        expectEqual(l.onSkipped("clip", p2, attempt: 0), .superseded, "…/superseded")
        expect(!l.release("clip", p2) && l.shouldSend("clip", r2) && l.release("clip", r2),
               "ASupersededGestureNeverReleasesTheNewerOnesSlot")
        l = MediaTransportGestureLedger()
        let intro = l.claim("intro"), outro = l.claim("outro")
        expect(l.shouldSend("intro", intro) && l.shouldSend("outro", outro), "TheSlotIsPerAssetSoTwoClipsDoNotSupersedeEachOther")
        l = MediaTransportGestureLedger(retryAttempts: 2)
        let t = l.claim("clip")
        expectEqual(l.onSkipped("clip", t, attempt: 0), .retry, "ASkippedGestureReArmsUntilTheAttemptBudgetRunsOut/0")
        expectEqual(l.onSkipped("clip", t, attempt: 1), .retry, "…/1")
        expectEqual(l.onSkipped("clip", t, attempt: 2), .giveUp, "…/2")
        expect(!l.shouldSend("clip", t), "…/released on give-up")
        l = MediaTransportGestureLedger()
        let done = l.claim("clip"); l.release("clip", done)
        expect(!l.shouldSend("clip", done), "ACompletedGestureCannotSendAgain")
        expect(!l.shouldSend("never-claimed", 1), "AnUnknownAssetNeverSends")
    }

    @MainActor
    private static func testPlayMediaAssetSendsOneShotGesturesAndRetriesOnlyTheLatest() {
        // Fixture: Program s2 carries a fixed media route for "clip" (append a SceneLayer with
        // mediaAssetId "clip" to s2 after ensureLayers); the core row says live.
        let model = cueModel()
        model.ensureLayers(for: "s2")
        model.scenes[1].layers.append(SceneLayer(id: "s2-layer-9", slotId: nil, mediaAssetId: "clip"))
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:clip", "mediaAssetId": "clip",
                                                         "state": "live", "onProgram": true]]])
        var sent: [JSONObject] = []
        var failNext = 1                                   // the FIRST send times out, then succeed
        model.mediaTransportSendForTesting = { body in
            sent.append(body)
            if failNext > 0 { failNext -= 1; throw BridgeError.timeout }
            return ["ok": true]
        }
        model.playMediaAsset("clip")                       // live on Program -> pause
        expect(!model.selectedMediaPlaying, "pause flips the flag at once")
        expectEqual(model.mediaStatus, "Sting paused on Program", "status")
        spin(until: { sent.count == 2 }, limit: 2)
        let gestures = sent.compactMap { ($0["commands"] as? [JSONObject])?.first }
        expectEqual(gestures.map { $0["type"] as? String ?? "" }, ["set-media-transport", "set-media-transport"],
                    "a skipped send re-arms the SAME gesture")
        expectEqual(gestures.map { $0["action"] as? String ?? "" }, ["pause", "pause"], "pause twice")
        // Supersession end to end: tap 1 times out and is re-armed; tap 2 lands first; tap 1 must not send.
        sent = []; failNext = 1
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:clip", "mediaAssetId": "clip",
                                                         "state": "paused", "onProgram": true]]])
        model.playMediaAsset("clip")                       // paused -> play (times out, retry armed)
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:clip", "mediaAssetId": "clip",
                                                         "state": "live", "onProgram": true]]])
        model.playMediaAsset("clip")                       // live -> pause (succeeds)
        spin(until: { false }, limit: 0.4)                 // let the 120 ms retry fire if it would
        let actions = sent.compactMap { (($0["commands"] as? [JSONObject])?.first?["action"]) as? String }
        expectEqual(actions, ["play", "pause"], "the stale play retry never reaches the core")
        expect(!sent.contains { body in (body["commands"] as? [JSONObject] ?? []).contains { $0["type"] as? String == "load-scene-graph" } },
               "the gesture is never folded into a scene sync")
    }

    @MainActor
    private static func testAGestureThatStaysBusyGivesUpVisibly() {
        let model = cueModel()
        model.ensureLayers(for: "s2")
        model.scenes[1].layers.append(SceneLayer(id: "s2-layer-9", slotId: nil, mediaAssetId: "clip"))
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:clip", "mediaAssetId": "clip",
                                                         "state": "live", "onProgram": true]]])
        var sends = 0
        model.mediaTransportSendForTesting = { _ in sends += 1; throw BridgeError.timeout }
        model.playMediaAsset("clip")
        spin(until: { model.mediaStatus.hasPrefix("Could not") }, limit: 2)
        expectEqual(sends, 6, "attempt 0 + 5 retries")
        expectEqual(model.mediaStatus, "Could not pause Sting - the media core stayed busy. Press it again.", "Windows wording")
    }

    @MainActor
    private static func testAnOffProgramTapIsALocalAuditionAndSendsNothing() {
        let model = cueModel()
        var sends = 0
        model.mediaTransportSendForTesting = { _ in sends += 1; return [:] }
        model.playMediaAsset("clip")
        expect(model.selectedMediaPlaying, "audition on"); expectEqual(model.mediaStatus, "Auditioning Sting", "status")
        model.playMediaAsset("clip")
        expect(!model.selectedMediaPlaying, "audition off"); expectEqual(model.mediaStatus, "Sting audition paused", "status")
        expectEqual(sends, 0, "no core gesture for an off-Program clip")
        expectEqual(model.mediaPlaybackButtonLabel, "Audition", "label")
    }

    /// ADDED. The plan's supersession leg taps twice with no run-loop turn between, so the first
    /// send has not even failed when the second tap claims the slot: the ledger's onSkipped already
    /// answers `superseded` and the 120 ms retry is never armed. That leaves "the retry asks
    /// shouldSend again before it sends" unpinned (the named mutation stayed GREEN). Here the first
    /// send fails and its retry IS armed before the newer tap lands.
    @MainActor
    private static func testAnArmedRetryIsDroppedWhenANewerTapLands() {
        let model = cueModel()
        model.ensureLayers(for: "s2")
        model.scenes[1].layers.append(SceneLayer(id: "s2-layer-9", slotId: nil, mediaAssetId: "clip"))
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:clip", "mediaAssetId": "clip",
                                                         "state": "paused", "onProgram": true]]])
        var sent: [JSONObject] = []
        var failNext = 1
        model.mediaTransportSendForTesting = { body in
            sent.append(body)
            if failNext > 0 { failNext -= 1; throw BridgeError.timeout }
            return ["ok": true]
        }
        // The 120 ms retry runs on a harness timer: a 30 ms wall-clock spin let a
        // slow CI runner fire the retry before the newer tap (2026-11-22).
        let timers = ShellTests.ManualTimers()
        model.timerSleepForTesting = { await timers.sleep($0) }
        model.playMediaAsset("clip")                       // paused -> play; the send times out
        spin(until: { sent.count == 1 }, limit: 2)
        spin(until: { timers.pending == 1 }, limit: 2)     // the failure is handled: the retry is armed
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:clip", "mediaAssetId": "clip",
                                                         "state": "live", "onProgram": true]]])
        model.playMediaAsset("clip")                       // live -> pause, inside the retry delay
        spin(until: { sent.count == 2 }, limit: 2)
        timers.fireAll()                                   // now the armed retry elapses
        spin(until: { false }, limit: 0.3)
        expectEqual(timers.pending, 0, "the dropped retry arms no further retry")
        let actions = sent.compactMap { (($0["commands"] as? [JSONObject])?.first?["action"]) as? String }
        expectEqual(actions, ["play", "pause"], "the armed play retry asks shouldSend again and does not send")
    }

    /// ADDED (the plan asks for it if "treat BridgeError.remote as skipped" stays green; pinned up front).
    @MainActor
    private static func testARemoteRejectionSendsOnceAndSaysSo() {
        let model = cueModel()
        model.ensureLayers(for: "s2")
        model.scenes[1].layers.append(SceneLayer(id: "s2-layer-9", slotId: nil, mediaAssetId: "clip"))
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:clip", "mediaAssetId": "clip",
                                                         "state": "live", "onProgram": true]]])
        var sends = 0
        model.mediaTransportSendForTesting = { _ in sends += 1; throw BridgeError.remote("clip is not on Program") }
        model.playMediaAsset("clip")
        spin(until: { model.mediaStatus.hasPrefix("Could not") }, limit: 2)
        spin(until: { false }, limit: 0.3)
        expectEqual(sends, 1, "a remote error sends once: it is a refusal, not backpressure")
        expectEqual(model.mediaStatus, "Could not pause Sting; the media core rejected it. clip is not on Program",
                    "Windows wording")
    }

    @MainActor
    private static func testToggleWithoutASelectionSaysSo() {
        let model = cueModel()
        model.toggleSelectedMediaPlayback()
        expectEqual(model.mediaStatus, "Select a media asset before playback", "Windows wording")
    }

    // ── Task 13: Take promotion and the media-selection rollback (S2) ───────

    @MainActor
    private static func mediaTakeModel(programClips: [String], previewClips: [String]) -> (AppModel, FakeTakeCore) {
        // Program "intro", Preview "interview"; each scene is ONE layer per clip id (slotless media layers).
        let core = FakeTakeCore()
        let model = AppModel()
        func scene(_ id: String, _ clips: [String]) -> SceneDef {
            SceneDef(id: id, name: id.capitalized, layout: "single",
                     layers: clips.enumerated().map { SceneLayer(id: "\(id)-layer-\($0.offset)", slotId: nil, mediaAssetId: $0.element) })
        }
        model.scenes = [scene("intro", programClips), scene("interview", previewClips)]
        model.mediaAssets = ["bed", "x", "y", "z"].map {
            MediaAsset(id: $0, name: $0.uppercased(), kind: "stinger", relativePath: "stinger/\($0).mp4",
                       filePath: "/b/stinger/\($0).mp4", naturalWidth: 0, naturalHeight: 0)
        }
        model.programSceneId = "intro"; model.previewSceneId = "interview"
        model.quitRequestForTesting = { body, _ in try await core.handle(body) }
        model.status = .connected(renderer: "test")
        return (model, core)
    }

    @MainActor
    private static func testTakeRollbackRestoresTheSelectionAClipThatWentLiveTookOver() {
        let (model, core) = mediaTakeModel(programClips: [], previewClips: ["y"])
        model.selectMediaAsset("bed")                      // "BED ready to cue or audition"
        let prior = model.captureMediaSelection()
        core.failure = "core answered with an unparseable snapshot"
        _ = mediaRunTake(model)
        expectEqual(model.captureMediaSelection(), prior, "Take_RollbackRestoresTheSelectionAClipThatWentLiveTookOver")
        expectEqual(model.mediaPlaybackButtonLabel, "Audition", "…/toggle")
    }

    @MainActor
    private static func testTakeRollbackKeepsAPausedProgramClipPausedWithAnHonestToggle() {
        let (model, core) = mediaTakeModel(programClips: ["x"], previewClips: ["x", "y"])
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:x", "mediaAssetId": "x",
                                                         "state": "paused", "onProgram": true]]])
        model.selectMediaAsset("x")
        expectEqual(model.mediaPlaybackButtonLabel, "Resume Program", "precondition")
        core.failure = "media core did not answer within 4000 ms"
        _ = mediaRunTake(model)
        expectEqual(model.selectedAssetId, "x", "Take_RollbackKeepsAPausedProgramClipPaused…/selection")
        expect(!model.selectedMediaPlaying, "…/still paused")
        expectEqual(model.mediaStatus, "X paused on Program", "…/status")
        expectEqual(model.mediaPlaybackButtonLabel, "Resume Program", "…/toggle")
    }

    @MainActor
    private static func testTakeRollbackPutsARollingClipThatLeftProgramBackOnPauseProgram() {
        let (model, core) = mediaTakeModel(programClips: ["x"], previewClips: [])
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:x", "mediaAssetId": "x",
                                                         "state": "live", "onProgram": true]]])
        model.selectMediaAsset("x")
        expect(model.selectedMediaPlaying, "precondition: selecting a live Program clip reads playing")
        core.failure = "native rejected scene"
        _ = mediaRunTake(model)
        expect(model.selectedMediaPlaying, "Take_RollbackPutsARollingClipThatLeftProgramBackOnPauseProgram")
        expectEqual(model.mediaStatus, "Playing X on Program", "…/status")
        expectEqual(model.mediaPlaybackButtonLabel, "Pause Program", "…/toggle")
    }

    @MainActor
    private static func testTakeRefusedRollbackLeavesTheSelectionAlone() {
        let (model, core) = mediaTakeModel(programClips: [], previewClips: ["y"])
        model.scenes.append(SceneDef(id: "outro", name: "Outro", layout: "single"))
        model.selectMediaAsset("bed")
        core.hold = true; core.failure = "native rejected scene"
        let box = mediaRunTake(model, wait: false)
        spin(until: { core.bodies.count == 1 }, limit: 2)
        // PLAN FIXTURE CORRECTED. The plan set previewSceneId = "intro" and renamed a scene. After the
        // Take's local swap Preview already IS "intro", and the rollback guard compares only the two bus
        // scene ids and the solo cues, so neither edit trips it: the rollback ran and the test could
        // only pass by NOT restoring the selection on a real rollback. The operator cues a third scene
        // instead, which is a newer edit the guard does see.
        model.previewSceneId = "outro"
        core.hold = false
        spin(until: { box.result != nil }, limit: 3)
        expectEqual(model.selectedAssetId, "y", "Take_RefusedRollbackLeavesTheSelectionAlone")
    }

    @MainActor
    private static func testTakeRollbackGivesAnOperatorPickThatLeftProgramOffProgramWording() {
        let (model, core) = mediaTakeModel(programClips: [], previewClips: ["y", "z"])
        model.selectMediaAsset("bed")
        core.hold = true; core.failure = "native rejected scene"
        let box = mediaRunTake(model, wait: false)
        spin(until: { core.bodies.count == 1 }, limit: 2)
        model.selectMediaAsset("z")                        // the operator's pick during the pending sync
        core.hold = false
        spin(until: { box.result != nil }, limit: 3)
        expectEqual(model.selectedAssetId, "z", "Take_RollbackGivesAnOperatorPickThatLeftProgramOffProgramWording/kept")
        expect(!model.selectedMediaPlaying, "…/not playing")
        expectEqual(model.mediaStatus, "Z left Program", "…/status")
        expectEqual(model.mediaPlaybackButtonLabel, "Audition", "…/toggle")
    }

    @MainActor
    private static func testASuccessfulTakePromotesTheClipThatWentLiveButNeverAStill() {
        let (model, _) = mediaTakeModel(programClips: [], previewClips: ["y"])
        model.mediaAssets.append(MediaAsset(id: "logo", name: "LOGO", kind: "lower-third",
            relativePath: "lower-third/logo.png", filePath: "/b/lower-third/logo.png", naturalWidth: 0, naturalHeight: 0))
        model.scenes[1].layers.insert(SceneLayer(id: "interview-layer-s", slotId: nil, mediaAssetId: "logo"), at: 0)
        model.selectMediaAsset("bed")
        _ = mediaRunTake(model)
        expectEqual(model.selectedAssetId, "y", "AStillGoingLiveIsNeverPromoted: the clip, not the logo")
        expect(model.selectedMediaPlaying, "promoted clip reads playing")
        expectEqual(model.mediaStatus, "Playing Y on Program", "status")
    }

    // Windows MediaRoutePlaybackServiceTests, ported by name; routes are the wire dictionaries.
    private static func testTakePromotionPureRules() {
        typealias P = MediaRoutePlayback
        func route(_ id: String) -> JSONObject { ["mode": "fixed", "mediaAssetId": id] }
        expectEqual(P.assetsEnteringProgram(previous: [], current: [route("clip")]), ["clip"],
                    "AssetsEnteringProgram_NamesAClipThatEntersProgram")
        expectEqual(P.assetsEnteringProgram(previous: [route("clip")], current: [route("clip")]), [],
                    "AssetsEnteringProgram_IgnoresAClipThatStaysOnProgram")
        expectEqual(P.assetsEnteringProgram(previous: [route("clip")], current: []), [],
                    "AssetsEnteringProgram_IgnoresAClipThatLeavesProgram")
        expectEqual(P.chooseAssetToPromote(["intro", "outro"], selected: "outro", supportsPlayback: nil), "outro",
                    "ChooseAssetToPromote_PrefersTheSelectedAssetWhenItWentLive")
        expectEqual(P.chooseAssetToPromote(["intro", "outro"], selected: "bumper", supportsPlayback: nil), "intro",
                    "ChooseAssetToPromote_OtherwiseTakesTheFirstWentLiveAsset/other selected")
        expectEqual(P.chooseAssetToPromote(["intro", "outro"], selected: nil, supportsPlayback: nil), "intro",
                    "ChooseAssetToPromote_OtherwiseTakesTheFirstWentLiveAsset/none selected")
        expect(P.chooseAssetToPromote([], selected: "intro", supportsPlayback: nil) == nil,
               "ChooseAssetToPromote_NothingWentLivePromotesNothing/selected")
        expect(P.chooseAssetToPromote([], selected: nil, supportsPlayback: nil) == nil,
               "ChooseAssetToPromote_NothingWentLivePromotesNothing/none")
        let stills: (String) -> Bool = { $0 != "logo" }
        expect(P.chooseAssetToPromote(["logo"], selected: "logo", supportsPlayback: stills) == nil,
               "AStillGoingLiveIsNeverPromoted/only a still")
        expectEqual(P.chooseAssetToPromote(["logo", "clip"], selected: "logo", supportsPlayback: stills), "clip",
                    "AStillGoingLiveIsNeverPromoted/the clip, not the selected still")
        expect(P.selectedAssetLeftProgram("clip", previous: [route("clip")], current: []),
               "SelectedAssetLeftProgram_TrueWhenTheSelectedClipWasOnProgramAndIsNotNow")
        expect(!P.selectedAssetLeftProgram("clip", previous: [route("clip")], current: [route("clip")]),
               "SelectedAssetLeftProgram_FalseWhenTheSelectedClipStaysOnProgram")
        expect(!P.selectedAssetLeftProgram("clip", previous: [], current: []),
               "SelectedAssetLeftProgram_FalseWhenTheSelectedAssetWasNeverOnProgram")
        expect(!P.selectedAssetLeftProgram("clip", previous: [route("clip"), route("other")], current: [route("clip")]),
               "SelectedAssetLeftProgram_FalseWhenADifferentClipLeftProgram")
        expect(!P.selectedAssetLeftProgram(nil, previous: [route("clip")], current: []),
               "SelectedAssetLeftProgram_FalseWhenNoAssetIsSelected")
    }

    /// ADDED. The plan's mutation "restore the saved playing flag instead of the on-air row" stayed
    /// GREEN: in every plan fixture the saved flag and the core's row agree (rule 2: both operands
    /// came from the same state). Here they differ: X was LIVE when the selection was captured, and
    /// the core reports it PAUSED while the Take is pending (another control surface paused it).
    @MainActor
    private static func testTakeRollbackReadsTheOnAirRowNotTheSavedFlag() {
        let (model, core) = mediaTakeModel(programClips: ["x"], previewClips: ["x", "y"])
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:x", "mediaAssetId": "x",
                                                         "state": "live", "onProgram": true]]])
        model.selectMediaAsset("x")
        expect(model.captureMediaSelection().playing, "precondition: the saved flag says playing")
        core.hold = true; core.failure = "native rejected scene"
        let box = mediaRunTake(model, wait: false)
        spin(until: { core.bodies.count == 1 }, limit: 2)
        expectEqual(model.selectedAssetId, "y", "precondition: the Take promoted the clip that went live")
        model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:x", "mediaAssetId": "x",
                                                         "state": "paused", "onProgram": true]]])
        core.hold = false
        spin(until: { box.result != nil }, limit: 3)
        expectEqual(model.selectedAssetId, "x", "the pre-Take selection is back")
        expect(!model.selectedMediaPlaying, "the flag is the core's row (paused), not the saved 'playing'")
        expectEqual(model.mediaStatus, "X paused on Program", "status from the on-air row")
        expectEqual(model.mediaPlaybackButtonLabel, "Resume Program", "an honest toggle")
    }

    // ── Final-review fixes ──────────────────────────────────────────────────

    /// An audio bed "supports playback", so Cue used to add it as a full-canvas layer. The Mac
    /// decoder has no picture for it (audio-only assets on air are slice N3): the layer would sit
    /// on Program with no frame and a warning on every poll.
    @MainActor
    private static func testCueRefusesAnAudioOnlyAsset() {
        let model = cueModel()
        model.mediaAssets.append(MediaAsset(id: "bed", name: "Bed", kind: "audio-bed", relativePath: "audio-bed/bed.wav",
                                            filePath: "/b/audio-bed/bed.wav", naturalWidth: 0, naturalHeight: 0))
        model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]])
        model.selectedAssetId = "bed"
        expectEqual(model.cueSelectedInPreview(),
                    "Bed is audio only; audio-only assets cannot be cued on Mac yet.", "refused, in words")
        expectEqual(model.scenes[0].layers.count, 1, "nothing added to Preview")
        expect(!model.canCueSelectedMedia, "the Cue button is disabled")
    }

    /// The core keeps a refused gesture's reason in programFrame.warnings until the next scene load,
    /// so every snapshot carried it and every snapshot rewrote the status line with it.
    @MainActor
    private static func testARefusedGestureIsShownOnceNotPinned() {
        let model = cueModel()
        model.selectedAssetId = "clip"
        let refusal = "media:clip is not on Program; pause/play applies to the Program clip only."
        let snapshot: JSONObject = ["programFrame": ["warnings": [refusal]]]
        model.applySnapshotForTesting(snapshot)
        expectEqual(model.mediaStatus, refusal, "the refusal is shown when it first appears")
        model.selectMediaAsset("clip")
        expectEqual(model.mediaStatus, "Sting ready to cue or audition", "a later status replaces it")
        model.applySnapshotForTesting(snapshot)
        model.applySnapshotForTesting(snapshot)
        expectEqual(model.mediaStatus, "Sting ready to cue or audition", "the same standing warning is not re-shown")
        model.applySnapshotForTesting(["programFrame": ["warnings": [String]()]])
        model.applySnapshotForTesting(snapshot)
        expectEqual(model.mediaStatus, refusal, "a refusal that went away and came back is new again")
    }
}

private final class ProbeBox: @unchecked Sendable {
    private let lock = NSLock()
    private var stored: [String: Int]?
    var value: [String: Int]? { lock.lock(); defer { lock.unlock() }; return stored }
    func set(_ value: [String: Int]) { lock.lock(); stored = value; lock.unlock() }
}
