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
    // Automation-tab policy (AutomationExtras.swift). OPTIONAL on purpose: a
    // non-optional key throws keyNotFound on every prefs file written before it
    // existed, and load()'s `try?` would reset the whole file to defaults.
    var automation: AutomationExtrasState?
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

    // EVERY FIELD IS OPTIONAL ON READ. Swift's synthesized Decodable does NOT
    // honour a property's default value — it calls decode() and THROWS
    // keyNotFound when the key is absent. load() swallows that with `try?`, so
    // adding one non-optional field silently reset the operator's ENTIRE
    // preferences file: scenes, meeting id, stream URL, everything. Shipping
    // `colorGrade` did exactly that to any prefs file written before it.
    //
    // Decoding field-by-field with decodeIfPresent makes the struct permanently
    // additive-safe: an older file keeps its values and the new key takes its
    // default. Do not replace this with the synthesized initializer.
    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        func v<T: Decodable>(_ key: CodingKeys, _ fallback: T) -> T {
            ((try? c.decodeIfPresent(T.self, forKey: key)) ?? nil) ?? fallback
        }
        version = v(.version, 1)
        joinMeetingId = v(.joinMeetingId, "")
        displayName = v(.displayName, "CoreVideo Pro (mac)")
        monitorEnabled = v(.monitorEnabled, false)
        monitorVolume = v(.monitorVolume, 0.7)
        isoRecordingEnabled = v(.isoRecordingEnabled, false)
        isoSelectedSourceIds = v(.isoSelectedSourceIds, [])
        autoTakeEnabled = v(.autoTakeEnabled, true)
        autoConfidenceThreshold = v(.autoConfidenceThreshold, 70.0)
        autoHoldSeconds = v(.autoHoldSeconds, 4.0)
        lowerThirdName = v(.lowerThirdName, "")
        lowerThirdTitle = v(.lowerThirdTitle, "")
        lowerThirdPosition = v(.lowerThirdPosition, "lower-left")
        logoBugAssetId = v(.logoBugAssetId, "")
        streamUrl = v(.streamUrl, "rtmp://a.rtmp.youtube.com/live2")
        recentMeetings = v(.recentMeetings, [])
        scenes = v(.scenes, [])
        webinar = v(.webinar, false)
        colorGrade = v(.colorGrade, [])
        automation = ((try? c.decodeIfPresent(AutomationExtrasState.self,
                                              forKey: .automation)) ?? nil)
    }

    // GUARD for the hazard this initializer exists to fix. A hand-written
    // decoder trades one silent failure for another: add a field to the struct,
    // forget a line here, and it encodes fine but NEVER restores from disk.
    // ShellPrefs is Equatable, so a full round-trip catches exactly that — any
    // field missing from init(from:) comes back as its default and the compare
    // fails. Runs in the headless self-check; keep it wired to a populated
    // instance, not a default one (a default survives a dropped field).
    static func roundTripSelfCheck() -> String? {
        var p = ShellPrefs()
        p.version = 7
        p.joinMeetingId = "8675309"
        p.displayName = "round-trip"
        p.monitorEnabled = true
        p.monitorVolume = 0.42
        p.isoRecordingEnabled = true
        p.isoSelectedSourceIds = ["zoom:101"]
        p.autoTakeEnabled = false
        p.autoConfidenceThreshold = 55.5
        p.autoHoldSeconds = 9.0
        p.lowerThirdName = "Name"
        p.lowerThirdTitle = "Title"
        p.lowerThirdPosition = "lower-right"
        p.logoBugAssetId = "asset-1"
        p.streamUrl = "rtmp://example/live"
        p.recentMeetings = ["1", "2"]
        p.scenes = [PersistedScene(id: "s", name: "S", layout: "duo", layers: [])]
        p.webinar = true
        p.colorGrade = [1, 2, 3, 4]
        p.automation = AutomationExtrasState()
        guard let data = try? JSONEncoder().encode(p) else { return "encode failed" }
        guard let back = try? JSONDecoder().decode(ShellPrefs.self, from: data) else {
            return "decode failed"
        }
        return back == p ? nil
            : "a field survives encode but NOT decode — it is missing from init(from:)"
    }

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
