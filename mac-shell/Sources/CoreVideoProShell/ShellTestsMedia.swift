// macOS media clip playback, Phase 1 (plan 2026-11-16-mac-media-phase1). Kept in
// its own file, like ShellTestsTier0.swift and ShellTestsVcamUpdate.swift, so
// parallel branches extending ShellTests.swift do not collide line-for-line.

import AVFoundation
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
}

private final class ProbeBox: @unchecked Sendable {
    private let lock = NSLock()
    private var stored: [String: Int]?
    var value: [String: Int]? { lock.lock(); defer { lock.unlock() }; return stored }
    func set(_ value: [String: Int]) { lock.lock(); stored = value; lock.unlock() }
}
