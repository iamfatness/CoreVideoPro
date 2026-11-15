// ShellPrefs — the mac analogue of production-output-preferences.json:
// operator state that should survive a relaunch. Saved as JSON under
// Application Support; restored before the bridge starts. Writes are
// change-detected on a slow timer rather than hooked into every setter —
// snapshot-rate published fields (meters) never touch disk.

import Foundation

/// The in-binary harnesses (COREVIDEO_SHELL_TESTS / COREVIDEO_SHELL_SELFCHECK).
/// ONE predicate, read once: prefs, the shell log and anything else that must
/// never touch the operator's real files ask this, never the environment.
enum ShellHarness {
    static func detect(in environment: [String: String]) -> Bool {
        environment["COREVIDEO_SHELL_TESTS"] != nil
            || environment["COREVIDEO_SHELL_SELFCHECK"] != nil
    }

    static let isActive: Bool = detect(in: ProcessInfo.processInfo.environment)

    /// Why a harness run must not proceed, or nil when prefs are isolated.
    static func prefsIsolationFailure(path: String, realPath: String) -> String? {
        path == realPath
            ? "shell harness ABORTED: prefs resolve to the operator's real file (\(realPath)); "
                + "refusing to run any test that could read or replace it."
            : nil
    }

    /// First thing every harness entry does, before any test or save runs.
    static func requireIsolatedPrefsOrExit() {
        if let failure = prefsIsolationFailure(path: ShellPrefs.path, realPath: ShellPrefs.realPath) {
            FileHandle.standardError.write(Data((failure + "\n").utf8))
            print(failure)
            exit(3)
        }
    }
}

// A saved scene: identity + layout preset + the edited canvas layers.
struct PersistedScene: Codable, Equatable {
    var id = ""
    var name = ""
    var layout = "single"
    var backgroundAssetId = ""
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
    // Add overlay (Tier 0.14): a layer bound to a media-bin still. Optional so
    // the synthesized decoder keeps older files (absent = a slot layer).
    var mediaAssetId: String?
}

/// One show-input slot assignment (Windows ShowInputSlotRecord,
/// ShowInputRosterStore.cs): operator INTENT — kind + source + name + in-show —
/// restored even when the source is not present yet (it reads offline until the
/// roster or device list produces it).
struct PersistedSlot: Codable, Equatable {
    var slotId = 0
    var kind = "unassigned"
    var sourceId = ""
    var name = ""
    var inShow = false
}

/// A persisted capture->microphone pairing. Reattached by `sourceKey`
/// ("capture:<id>") — the SOURCE it belongs to — so it can never land on
/// whatever else ends up in that slot. `slotId` is the legacy key, used only
/// for a record written before `sourceKey` existed.
struct CapturePairing: Codable, Equatable {
    var slotId = 0
    var audioDeviceId = ""
    var audioDeviceName = ""
    var sourceKey: String?
}

/// Chroma key settings for a slot. Only ENABLED keys are persisted — a saved
/// key that is off is indistinguishable from no key, and storing it would risk
/// restoring a key the operator turned off.
struct PersistedChromaKey: Codable, Equatable {
    var slotId = 0
    var colorHex = "#00ff00"
    var similarity = 0.4
    var smoothness = 0.1
    var spill = 0.2
    var sourceKey: String?   // see CapturePairing
}

/// Per-source dropout policy (core contract #535 slice 4a, `set-source-policy`).
/// Keyed by SLOT like `CapturePairing`/`PersistedChromaKey` — zoom pids churn
/// across meetings; the slot is the stable operator concept.
struct PersistedSlotPolicy: Codable, Equatable {
    var slotId = 0
    var dropoutPolicy = ""
    var sourceKey: String?   // see CapturePairing
}

struct ShellPrefs: Codable, Equatable {
    var version = 1
    var joinMeetingId = ""
    var displayName = "CoreVideo Pro (mac)"
    var monitorEnabled = false
    var monitorVolume = 0.7
    var isoRecordingEnabled = false
    var isoSelectedSourceIds: [String] = []
    var autoTakeEnabled = false
    var autoConfidenceThreshold = 70.0
    var autoHoldSeconds = 4.0
    // Automation-tab policy (AutomationExtras.swift). OPTIONAL on purpose: a
    // non-optional key throws keyNotFound on every prefs file written before it
    // existed, and load()'s `try?` would reset the whole file to defaults.
    var automation: AutomationExtrasState?
    var overlays: OverlaysState?
    var capturePairings: [CapturePairing]?
    var chromaKeys: [PersistedChromaKey]?
    var slotDropoutPolicies: [PersistedSlotPolicy]?
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
    // Production settings (the Windows ProductionSettingsWindow values that
    // production-output-preferences.json keeps: canvas size/fps, stream bitrate,
    // recording format/prefix, multiview layout + tile count). OPTIONAL: nil
    // means "never saved", and the model keeps its built-in default. A 4K or
    // 30 fps show used to come back as 1080p60 on every launch.
    var canvasWidth: Int?
    var canvasHeight: Int?
    var canvasFps: Int?
    var streamBitrateMbps: Double?
    var recordFormat: String?
    var recordPrefix: String?
    var multiviewLayoutMode: String?
    var multiviewTileCount: Int?
    // Show-input slot assignments (Tier 0.8). nil = a file written before this
    // existed: slots start empty, exactly as before.
    var showInputSlots: [PersistedSlot]?
    // Recording folder (Tier 0.11). nil/"" = the default ~/Movies/CoreVideoPro.
    var recordingFolder: String?
    // Monitor output device (Tier 0.12): CoreAudio UID + its name (kept so a
    // disconnected device can still be named). nil = system default output.
    var monitorDeviceId: String?
    var monitorDeviceName: String?

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
        autoTakeEnabled = false // Retired scene automation cannot be restored from preferences.
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
        vstChannelSelections = ((try? c.decodeIfPresent([String: String].self,
                                                        forKey: .vstChannelSelections)) ?? nil)
        overlays = ((try? c.decodeIfPresent(OverlaysState.self, forKey: .overlays)) ?? nil)
        capturePairings = ((try? c.decodeIfPresent([CapturePairing].self,
                                                   forKey: .capturePairings)) ?? nil)
        chromaKeys = ((try? c.decodeIfPresent([PersistedChromaKey].self,
                                              forKey: .chromaKeys)) ?? nil)
        slotDropoutPolicies = ((try? c.decodeIfPresent([PersistedSlotPolicy].self,
                                                       forKey: .slotDropoutPolicies)) ?? nil)
        canvasWidth = ((try? c.decodeIfPresent(Int.self, forKey: .canvasWidth)) ?? nil)
        canvasHeight = ((try? c.decodeIfPresent(Int.self, forKey: .canvasHeight)) ?? nil)
        canvasFps = ((try? c.decodeIfPresent(Int.self, forKey: .canvasFps)) ?? nil)
        streamBitrateMbps = ((try? c.decodeIfPresent(Double.self, forKey: .streamBitrateMbps)) ?? nil)
        recordFormat = ((try? c.decodeIfPresent(String.self, forKey: .recordFormat)) ?? nil)
        recordPrefix = ((try? c.decodeIfPresent(String.self, forKey: .recordPrefix)) ?? nil)
        multiviewLayoutMode = ((try? c.decodeIfPresent(String.self,
                                                       forKey: .multiviewLayoutMode)) ?? nil)
        multiviewTileCount = ((try? c.decodeIfPresent(Int.self, forKey: .multiviewTileCount)) ?? nil)
        showInputSlots = ((try? c.decodeIfPresent([PersistedSlot].self,
                                                  forKey: .showInputSlots)) ?? nil)
        recordingFolder = ((try? c.decodeIfPresent(String.self, forKey: .recordingFolder)) ?? nil)
        monitorDeviceId = ((try? c.decodeIfPresent(String.self, forKey: .monitorDeviceId)) ?? nil)
        monitorDeviceName = ((try? c.decodeIfPresent(String.self, forKey: .monitorDeviceName)) ?? nil)
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
        p.overlays = OverlaysState()
        p.capturePairings = [CapturePairing(slotId: 1, audioDeviceId: "d", audioDeviceName: "Mic",
                                            sourceKey: "capture:cam")]
        p.chromaKeys = [PersistedChromaKey(slotId: 1, sourceKey: "capture:cam")]
        p.slotDropoutPolicies = [PersistedSlotPolicy(slotId: 2, dropoutPolicy: "black",
                                                     sourceKey: "zoom:42")]
        p.showInputSlots = [PersistedSlot(slotId: 1, kind: "capture", sourceId: "cam",
                                          name: "Cam", inShow: true)]
        p.recordingFolder = "/Volumes/Show/Recordings"
        p.monitorDeviceId = "AppleUSBAudioEngine:Interface:1"
        p.monitorDeviceName = "Headphones Interface"
        p.vstChannelSelections = ["ch1": "vst:Test"]
        p.canvasWidth = 3840
        p.canvasHeight = 2160
        p.canvasFps = 30
        p.streamBitrateMbps = 12.5
        p.recordFormat = "mov"
        p.recordPrefix = "gala"
        p.multiviewLayoutMode = "grid"
        p.multiviewTileCount = 6
        guard let data = try? JSONEncoder().encode(p) else { return "encode failed" }
        guard let back = try? JSONDecoder().decode(ShellPrefs.self, from: data) else {
            return "decode failed"
        }
        return back == p ? nil
            : "a field survives encode but NOT decode — it is missing from init(from:)"
    }

    /// The operator's real preferences file. Never touched by a test run.
    static let realPath = NSHomeDirectory()
        + "/Library/Application Support/CoreVideoPro/shell-preferences.json"

    /// Structural isolation: under COREVIDEO_SHELL_TESTS or COREVIDEO_SHELL_SELFCHECK
    /// every load/save goes to a per-process temp file, so no harness run (CI or
    /// local) can read or replace the operator's saved scenes and settings.
    static let path: String = {
        guard ShellHarness.isActive else { return realPath }
        let dir = NSTemporaryDirectory() + "corevideo-shell-harness-\(getpid())"
        return dir + "/shell-preferences.json"
    }()

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
