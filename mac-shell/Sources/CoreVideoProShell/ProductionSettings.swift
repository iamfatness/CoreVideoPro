// ProductionSettings — the Settings-tab production values that must survive a
// relaunch and be re-applied to every core generation (Tier 0 item 0.7).
//
// Windows keeps these in production-output-preferences.json
// (StudioViewModel.cs:12064-12074 save, :12291 restore) and replays them into
// every sync. The Mac had them as session-only @Published defaults, so a 4K or
// 30 fps show came back as 1080p60, and nothing re-sent the canvas to a
// restarted core (docs/reference/gotchas.md: a one-shot command must be
// re-applied on every core generation).

import Foundation

enum ProductionSettingsPolicy {
    /// The canvas sizes the Settings picker offers (SettingsPane "Canvas size").
    static let canvasSizes: [(width: Int, height: Int)] = [
        (3840, 2160), (2560, 1440), (1920, 1080), (1280, 720),
    ]
    static let canvasFpsChoices = [60, 30, 24]
    static let streamBitrateRange: ClosedRange<Double> = 1...20
    static let recordFormats = ["mp4", "mov"]
    static let multiviewLayoutModes = ["pgmPvwTop", "pgmPvwLarge", "pgmPvwSide", "grid"]
    static let multiviewTileRange: ClosedRange<Int> = 1...16

    /// "1080p60" from the real canvas, never a literal.
    static func canvasLabel(height: Int, fps: Int) -> String { "\(height)p\(fps)" }

    /// The stream flyout's recipe line. The stream profile itself is still the
    /// fixed 1080p30 H.264 recipe (separate stream render sizes are Tier 2.8);
    /// only the bitrate is the operator's, and the line says what is sent.
    static func streamRecipeLabel(bitrateMbps: Double) -> String {
        let mbps = bitrateMbps.rounded() == bitrateMbps
            ? String(format: "%.0f", bitrateMbps) : String(format: "%.1f", bitrateMbps)
        return "1080p30 H.264 (VideoToolbox via ffmpeg) \u{00B7} \(mbps) Mbps CBR \u{00B7} AAC 160 kbps"
    }
}

extension AppModel {
    /// Applies the saved production values. Anything absent (an older prefs
    /// file) or outside what the Settings controls can produce keeps the
    /// current value — a hand-edited or corrupt file never puts an unpickable
    /// canvas on the wire.
    func restoreProductionSettings(_ prefs: ShellPrefs) {
        if let width = prefs.canvasWidth, let height = prefs.canvasHeight,
           ProductionSettingsPolicy.canvasSizes.contains(where: {
               $0.width == width && $0.height == height
           }) {
            canvasWidth = width
            canvasHeight = height
        }
        if let fps = prefs.canvasFps, ProductionSettingsPolicy.canvasFpsChoices.contains(fps) {
            canvasFps = fps
        }
        if let bitrate = prefs.streamBitrateMbps, bitrate.isFinite {
            let range = ProductionSettingsPolicy.streamBitrateRange
            streamBitrateMbps = min(max(bitrate, range.lowerBound), range.upperBound)
        }
        if let format = prefs.recordFormat, ProductionSettingsPolicy.recordFormats.contains(format) {
            recordFormat = format
        }
        if let prefix = prefs.recordPrefix { recordPrefix = prefix }
        if let mode = prefs.multiviewLayoutMode,
           ProductionSettingsPolicy.multiviewLayoutModes.contains(mode) {
            multiviewLayoutMode = mode
        }
        if let tiles = prefs.multiviewTileCount {
            let range = ProductionSettingsPolicy.multiviewTileRange
            multiviewTileCount = min(max(tiles, range.lowerBound), range.upperBound)
        }
    }

    /// Writes the production values into a prefs snapshot (currentPrefs()).
    func storeProductionSettings(into prefs: inout ShellPrefs) {
        prefs.canvasWidth = canvasWidth
        prefs.canvasHeight = canvasHeight
        prefs.canvasFps = canvasFps
        prefs.streamBitrateMbps = streamBitrateMbps
        prefs.recordFormat = recordFormat
        prefs.recordPrefix = recordPrefix
        prefs.multiviewLayoutMode = multiviewLayoutMode
        prefs.multiviewTileCount = multiviewTileCount
    }

    var canvasLabel: String {
        ProductionSettingsPolicy.canvasLabel(height: canvasHeight, fps: canvasFps)
    }

    func outputProfileCommand() -> JSONObject {
        [
            "type": "set-output-profile",
            "width": canvasWidth, "height": canvasHeight, "fps": canvasFps,
            "targetBitrateMbps": streamBitrateMbps, "codec": streamCodec,
            "resolution": "\(canvasWidth)x\(canvasHeight)",
        ]
    }

    func multiviewConfigCommand() -> JSONObject {
        CoreCommands.configureMultiviewer(layoutMode: multiviewLayoutMode,
                                          tileCount: multiviewTileCount)
    }

    /// What a fresh core generation must be told about production settings.
    /// The core starts at its built-in 1920x1080@60 every launch.
    func productionSettingsCommands() -> [JSONObject] {
        [outputProfileCommand(), multiviewConfigCommand()]
    }
}
