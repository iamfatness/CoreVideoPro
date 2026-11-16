// MediaBin — the mac port of MediaBinService/MediaBinClassifier. Assets live
// under ~/Library/Application Support/CoreVideoPro/media/<kind>/, classified
// by extension exactly like Windows (video→stinger, image→lower-third,
// audio→audio-bed, slate by folder/name), and asset ids are byte-compatible:
// "media-" + sha256(relativePath with "/" separators).hex[..12].

import AVFoundation
import CryptoKit
import Foundation
import ImageIO

struct MediaAsset: Identifiable, Equatable {
    let id: String
    let name: String
    let kind: String
    let relativePath: String
    let filePath: String
    let naturalWidth: Int
    let naturalHeight: Int
    /// Probed after the scan (MediaBin.probeDurations); nil = unknown or a still.
    /// Defaulted so every existing memberwise call site still compiles.
    var durationMs: Int? = nil

    var isStillImage: Bool {
        let ext = (filePath as NSString).pathExtension.lowercased()
        return ["png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff"].contains(ext)
    }

    /// Windows MediaAsset.SupportsPlayback (ProductionModels.cs): anything that is not a still.
    var supportsPlayback: Bool { !isStillImage }

    /// Windows MediaAsset.DurationLabel: `Math.Round(ms / 100.0) / 10.0` formatted "0.#" + "s".
    /// .NET's Math.Round is banker's rounding, so 1250 ms is "1.2s" and 1350 ms is "1.4s".
    var durationLabel: String {
        guard let ms = durationMs else { return "" }
        let tenths = Int((Double(ms) / 100.0).rounded(.toNearestOrEven))
        return tenths % 10 == 0 ? "\(tenths / 10)s" : "\(tenths / 10).\(tenths % 10)s"
    }
}

enum MediaBin {
    static let kinds = ["stinger", "lower-third", "audio-bed", "slate"]
    static let videoExtensions = ["mp4", "mov", "webm"]
    static let imageExtensions = ["png", "jpg", "jpeg", "gif"]
    static let audioExtensions = ["wav", "mp3", "aac", "m4a"]

    /// What the bin accepts, in the Windows picker's order (MediaBinService.cs).
    static let supportedExtensions = ["mp4", "mov", "webm", "png", "jpg", "jpeg", "gif",
                                      "wav", "mp3", "aac", "m4a"]

    /// The operator's real bin. Never touched by a harness run.
    static let realRoot = NSHomeDirectory() + "/Library/Application Support/CoreVideoPro/media"

    /// Harness-isolated, the ShellPrefs rule: under COREVIDEO_SHELL_TESTS /
    /// COREVIDEO_SHELL_SELFCHECK the bin is a per-process temp directory, so no
    /// test can list, import into, or move files out of the operator's bin.
    static let root: String = {
        guard ShellHarness.isActive else { return realRoot }
        return NSTemporaryDirectory() + "corevideo-shell-harness-\(getpid())/media"
    }()

    /// Windows MediaBinClassifier.ClassifyFile: slate (folder or name prefix)
    /// first, then by extension; nil for an extension the bin does not accept.
    static func classify(fileName: String, relativePath: String) -> String? {
        let lowerPath = relativePath.lowercased().replacingOccurrences(of: "\\", with: "/")
        let lowerName = fileName.lowercased()
        let ext = (fileName as NSString).pathExtension.lowercased()
        guard supportedExtensions.contains(ext) else { return nil }
        if lowerPath.contains("slates/") || lowerName.hasPrefix("slate") {
            return "slate"
        }
        if videoExtensions.contains(ext) { return "stinger" }
        if imageExtensions.contains(ext) { return "lower-third" }
        if audioExtensions.contains(ext) { return "audio-bed" }
        return nil
    }

    /// Windows MediaBinClassifier.KindLabel.
    static func kindLabel(_ kind: String) -> String {
        switch kind {
        case "stinger": return "Stinger"
        case "lower-third": return "Lower-third"
        case "audio-bed": return "Audio bed"
        case "slate": return "Slate"
        default: return kind
        }
    }

    /// Windows MediaBinClassifier.GroupAssets: kinds in `kinds` order, empty groups omitted.
    static func groups(_ assets: [MediaAsset]) -> [(kind: String, label: String, assets: [MediaAsset])] {
        kinds.compactMap { kind in
            let members = assets.filter { $0.kind == kind }
            return members.isEmpty ? nil : (kind: kind, label: kindLabel(kind), assets: members)
        }
    }

    /// `<folder>/<name>`, or `<folder>/<base>-<n>.<ext>` for the first free n.
    private static func uniqueDestination(folder: String, name: String) -> String {
        let fm = FileManager.default
        var target = folder + "/" + name
        var suffix = 1
        while fm.fileExists(atPath: target) {
            let base = (name as NSString).deletingPathExtension
            let ext = (name as NSString).pathExtension
            target = folder + "/" + base + "-\(suffix)" + (ext.isEmpty ? "" : "." + ext)
            suffix += 1
        }
        return target
    }

    /// Windows MediaBinService.DeleteAsset: NON-DESTRUCTIVE. The file moves into
    /// <root>/.trash under a unique name, so an accidental remove is recoverable.
    static func moveToTrash(_ asset: MediaAsset) -> Bool {
        let fm = FileManager.default
        guard !asset.filePath.isEmpty, fm.fileExists(atPath: asset.filePath) else { return false }
        let trash = root + "/.trash"
        do {
            try fm.createDirectory(atPath: trash, withIntermediateDirectories: true)
            let destination = uniqueDestination(folder: trash, name: (asset.filePath as NSString).lastPathComponent)
            try fm.moveItem(atPath: asset.filePath, toPath: destination)
            return true
        } catch {
            return false
        }
    }

    static func assetId(relativePath: String) -> String {
        let normalized = relativePath.replacingOccurrences(of: "\\", with: "/")
        let digest = SHA256.hash(data: Data(normalized.utf8))
        let hex = digest.map { String(format: "%02x", $0) }.joined()
        return "media-" + String(hex.prefix(12))
    }

    static func naturalSize(of path: String) -> (Int, Int) {
        guard let source = CGImageSourceCreateWithURL(
                URL(fileURLWithPath: path) as CFURL, nil),
              let props = CGImageSourceCopyPropertiesAtIndex(source, 0, nil)
                as? [CFString: Any],
              let width = props[kCGImagePropertyPixelWidth] as? Int,
              let height = props[kCGImagePropertyPixelHeight] as? Int
        else { return (0, 0) }
        return (width, height)
    }

    struct ImportResult: Equatable {
        let imported: [MediaAsset]
        /// File names the bin refused (an extension it does not accept) or could not copy.
        let refused: [String]
    }

    static func importFiles(_ urls: [URL]) -> ImportResult {
        let fm = FileManager.default
        var imported: [MediaAsset] = []
        var refused: [String] = []
        for url in urls {
            let name = url.lastPathComponent
            guard let kind = classify(fileName: name, relativePath: name) else {
                refused.append(name)
                continue
            }
            let kindDir = root + "/" + kind
            try? fm.createDirectory(atPath: kindDir, withIntermediateDirectories: true)
            let target = uniqueDestination(folder: kindDir, name: name)
            do {
                try fm.copyItem(at: url, to: URL(fileURLWithPath: target))
            } catch {
                refused.append(name)
                continue
            }
            if let asset = assetFrom(path: target) { imported.append(asset) } else { refused.append(name) }
        }
        return ImportResult(imported: imported, refused: refused)
    }

    static func loadAssets() -> [MediaAsset] {
        let fm = FileManager.default
        var assets: [MediaAsset] = []
        for kind in kinds {
            let dir = root + "/" + kind
            guard let names = try? fm.contentsOfDirectory(atPath: dir) else { continue }
            for name in names.sorted() where !name.hasPrefix(".") {
                if let asset = assetFrom(path: dir + "/" + name) {
                    assets.append(asset)
                }
            }
        }
        return assets
    }

    private static func assetFrom(path: String) -> MediaAsset? {
        guard path.hasPrefix(root + "/") else { return nil }
        let relative = String(path.dropFirst(root.count + 1))
        let name = (path as NSString).lastPathComponent
        // A file the classifier refuses is not an asset (Windows: unknown -> null).
        guard let kind = classify(fileName: name, relativePath: relative) else { return nil }
        let size = naturalSize(of: path)
        return MediaAsset(
            id: assetId(relativePath: relative),
            name: (name as NSString).deletingPathExtension,
            kind: kind, relativePath: relative, filePath: path,
            naturalWidth: size.0, naturalHeight: size.1)
    }

    /// Video containers AVFoundation plays on this Mac. `.webm` is in the bin's
    /// video list (Windows plays it through FFmpeg) but is NOT playable here
    /// until FFmpeg playback exists (plan default D1): the core refuses it loudly.
    static let avFoundationVideoExtensions = ["mp4", "mov"]

    /// Assets a scene background may use on this Mac: stills and AVFoundation-playable video.
    /// `.webm` is excluded until FFmpeg playback exists (default D1). Audio is never a background.
    /// Decided by what the FILE is, not by its bin kind: a slate or a stinger is a
    /// background candidate when it is a picture or a playable clip. Bin order is kept.
    static func backgroundOptions(_ assets: [MediaAsset]) -> [MediaAsset] {
        assets.filter { asset in
            if asset.isStillImage { return true }
            let ext = (asset.filePath as NSString).pathExtension.lowercased()
            return avFoundationVideoExtensions.contains(ext)
        }
    }

    /// Test hook: told whether a probe started on the main thread. Never set in production.
    nonisolated(unsafe) static var probeThreadObserverForTesting: ((Bool) -> Void)?

    private static func startedOnMainThread() -> Bool { Thread.isMainThread }

    /// Durations in milliseconds by asset id, for every playable asset AVFoundation
    /// can read. Stills and files it cannot open are simply absent. Runs OFF the
    /// main actor: a bin on a slow volume must never stall the window.
    /// (Windows' scan leaves DurationMs null; the Mac fills it, plan default D9.)
    static func probeDurations(_ assets: [MediaAsset]) async -> [String: Int] {
        probeThreadObserverForTesting?(startedOnMainThread())
        var durations: [String: Int] = [:]
        for asset in assets where asset.supportsPlayback {
            let media = AVURLAsset(url: URL(fileURLWithPath: asset.filePath))
            guard let duration = try? await media.load(.duration), duration.isNumeric else { continue }
            let seconds = CMTimeGetSeconds(duration)
            if seconds > 0 { durations[asset.id] = Int((seconds * 1000).rounded()) }
        }
        return durations
    }

    /// "<n> assets · <k> clips · <s> stills · <a> audio". A clip is anything that
    /// plays (Windows SupportsPlayback), so an audio bed counts as a clip AND as audio.
    static func summary(_ assets: [MediaAsset]) -> String {
        guard !assets.isEmpty else { return "Bin is empty — import stills, stingers or audio beds." }
        func count(_ n: Int, _ singular: String) -> String { "\(n) \(singular)\(n == 1 ? "" : "s")" }
        let clips = assets.filter(\.supportsPlayback).count
        let stills = assets.count - clips
        let audio = assets.filter {
            audioExtensions.contains(($0.filePath as NSString).pathExtension.lowercased())
        }.count
        return [count(assets.count, "asset"), count(clips, "clip"), count(stills, "still"), "\(audio) audio"]
            .joined(separator: " · ")
    }

    // OverlayLayerService.ComputeOverlayRect: 15% canvas width, aspect-locked
    // height on a 16:9 canvas, snapped in a 5% safe area, 0.08 floor.
    static func bugRect(naturalWidth: Int, naturalHeight: Int)
        -> (x: Double, y: Double, w: Double, h: Double) {
        let aspect = naturalWidth > 0 && naturalHeight > 0
            ? Double(naturalWidth) / Double(naturalHeight) : 16.0 / 9.0
        let w = max(0.08, 0.15)
        let h = max(0.08, min(0.9, w * (16.0 / 9.0) / aspect))
        return (x: 1.0 - 0.05 - w, y: 0.05, w: w, h: h)
    }
}
