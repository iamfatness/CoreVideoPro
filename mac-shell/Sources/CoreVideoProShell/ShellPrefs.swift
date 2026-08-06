// ShellPrefs — the mac analogue of production-output-preferences.json:
// operator state that should survive a relaunch. Saved as JSON under
// Application Support; restored before the bridge starts. Writes are
// change-detected on a slow timer rather than hooked into every setter —
// snapshot-rate published fields (meters) never touch disk.

import Foundation

// A saved scene: identity + layout preset + the edited canvas layers.
struct PersistedScene: Codable, Equatable {
    var id = ""
    var name = ""
    var layout = "single"
    var layers: [PersistedLayer] = []
}

struct PersistedLayer: Codable, Equatable {
    var id = ""
    var slotId: Int?
    var x = 0.0
    var y = 0.0
    var width = 1.0
    var height = 1.0
    var fitMode = "fill"
    var opacity = 1.0
}

struct ShellPrefs: Codable, Equatable {
    var version = 1
    var joinMeetingId = ""
    var displayName = "CoreVideo Pro (mac)"
    var monitorEnabled = false
    var monitorVolume = 0.7
    var isoRecordingEnabled = false
    var isoSelectedSourceIds: [String] = []
    var autoTakeEnabled = true
    var autoConfidenceThreshold = 70.0
    var autoHoldSeconds = 4.0
    var lowerThirdName = ""
    var lowerThirdTitle = ""
    var lowerThirdPosition = "lower-left"
    var logoBugAssetId = ""
    var streamUrl = "rtmp://a.rtmp.youtube.com/live2"  // key lives in the Keychain
    var recentMeetings: [String] = []  // most-recent first, capped at 5
    var scenes: [PersistedScene] = []  // custom scenes survive relaunches
    var webinar = false
    // Program color grade [exposure, contrast, saturation, temperature]. Empty
    // means neutral; a short array is tolerated so an older prefs file loads.
    var colorGrade: [Double] = []
    // Per-channel VST3 insert selection ("vst:<name>" / "vst:<name>/<class>").
    // OPTIONAL on purpose: Swift's synthesized Codable decodes a non-optional
    // property with `decode` and THROWS on a missing key even when the property
    // has a default — a new non-optional field would make every existing
    // preferences file fail to decode, and load() silently falls back to
    // defaults, wiping the operator's saved show. Optional gets decodeIfPresent.
    var vstChannelSelections: [String: String]?

    static let path = NSHomeDirectory()
        + "/Library/Application Support/CoreVideoPro/shell-preferences.json"

    static func load() -> ShellPrefs {
        guard let data = FileManager.default.contents(atPath: path),
              let prefs = try? JSONDecoder().decode(ShellPrefs.self, from: data)
        else { return ShellPrefs() }
        return prefs
    }

    func save() {
        let dir = (Self.path as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(
            atPath: dir, withIntermediateDirectories: true)
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(self) else { return }
        try? data.write(to: URL(fileURLWithPath: Self.path), options: .atomic)
    }
}
