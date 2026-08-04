// MediaBin — the mac port of MediaBinService/MediaBinClassifier. Assets live
// under ~/Library/Application Support/CoreVideoPro/media/<kind>/, classified
// by extension exactly like Windows (video→stinger, image→lower-third,
// audio→audio-bed, slate by folder/name), and asset ids are byte-compatible:
// "media-" + sha256(relativePath with "/" separators).hex[..12].

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

    var isStillImage: Bool {
        let ext = (filePath as NSString).pathExtension.lowercased()
        return ["png", "jpg", "jpeg", "bmp", "gif", "tif", "tiff"].contains(ext)
    }
}

enum MediaBin {
    static let kinds = ["stinger", "lower-third", "audio-bed", "slate"]
    static let videoExtensions = ["mp4", "mov", "webm"]
    static let imageExtensions = ["png", "jpg", "jpeg", "gif"]
    static let audioExtensions = ["wav", "mp3", "aac", "m4a"]

    static var root: String {
        NSHomeDirectory() + "/Library/Application Support/CoreVideoPro/media"
    }

    static func classify(fileName: String, relativePath: String) -> String {
        let lowerPath = relativePath.lowercased()
        let lowerName = fileName.lowercased()
        if lowerPath.contains("slates/") || lowerName.hasPrefix("slate") {
            return "slate"
        }
        let ext = (fileName as NSString).pathExtension.lowercased()
        if videoExtensions.contains(ext) { return "stinger" }
        if imageExtensions.contains(ext) { return "lower-third" }
        if audioExtensions.contains(ext) { return "audio-bed" }
        return "lower-third"
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

    static func importFiles(_ urls: [URL]) -> [MediaAsset] {
        let fm = FileManager.default
        var imported: [MediaAsset] = []
        for url in urls {
            let name = url.lastPathComponent
            let kind = classify(fileName: name, relativePath: name)
            let kindDir = root + "/" + kind
            try? fm.createDirectory(atPath: kindDir, withIntermediateDirectories: true)
            var target = kindDir + "/" + name
            var suffix = 1
            while fm.fileExists(atPath: target) {
                let base = (name as NSString).deletingPathExtension
                let ext = (name as NSString).pathExtension
                target = kindDir + "/" + base + "-\(suffix)" + (ext.isEmpty ? "" : "." + ext)
                suffix += 1
            }
            do {
                try fm.copyItem(at: url, to: URL(fileURLWithPath: target))
            } catch {
                continue
            }
            if let asset = assetFrom(path: target) { imported.append(asset) }
        }
        return imported
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
        let kind = classify(fileName: name, relativePath: relative)
        let size = naturalSize(of: path)
        return MediaAsset(
            id: assetId(relativePath: relative),
            name: (name as NSString).deletingPathExtension,
            kind: kind, relativePath: relative, filePath: path,
            naturalWidth: size.0, naturalHeight: size.1)
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
