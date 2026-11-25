// AppModel — the shell's single observable state root. Runs the 10Hz
// media-core-sync tick + 2Hz zoom-snapshot poll, caches the snapshot slices
// the panes render, surfaces warnings loudly, and owns the command builders.
// Mirrors (in miniature) the WinUI TransportCoordinator/ShowInputsCoordinator
// responsibilities; their characterization tests are the porting spec.

import CameraExtensionKit
import Foundation
import SwiftUI

enum StudioTab: String, CaseIterable {
    case studio = "Studio"
    case zoom = "Zoom"
    case sources = "Sources"
    case scenes = "Scenes"
    case routing = "Routing"
    case overlays = "Overlays"
    case audio = "Audio"
    case comms = "Comms"
    case media = "Media"
    case automation = "Automation"
    case settings = "Settings"
    case diagnose = "Diagnose"

    // Top navigation, matching Windows 1.0 (StudioWorkspace.xaml): one "Produce"
    // group — Studio, Zoom, Sources, Scenes, Routing, Graphics, Audio, Comms,
    // Media, Health. Comms sits between Audio and Media, as on Windows, but
    // only once CommsGate is on (owner decision 2: the tab ships after the live
    // co-host test passes). OHG Show is not on Mac. Settings trails: Windows
    // reaches its Zoom connection page through "Zoom", which is Mac's Zoom
    // pane; Mac Settings (output/recording/stream) has no Windows counterpart.
    static let navGroups = ["Produce"]
    static var navigation: [StudioTab] { navigation(commsEnabled: CommsGate.enabled) }
    static func navigation(commsEnabled: Bool) -> [StudioTab] {
        [.studio, .zoom, .sources, .scenes, .routing, .overlays, .audio]
            + (commsEnabled ? [.comms] : [])
            + [.media, .diagnose, .settings]
    }

    /// Operator-facing label. rawValue stays the stable identifier (control
    /// state, COREVIDEO_SHELL_TAB); Windows calls these Graphics and Health.
    var navLabel: String {
        switch self {
        case .overlays: return "Graphics"
        case .diagnose: return "Health"
        default: return rawValue
        }
    }

    /// Resolves a tab by identifier or current label, case-insensitively.
    init?(name: String) {
        let n = name.lowercased()
        guard let tab = StudioTab.allCases.first(where: {
            $0.rawValue.lowercased() == n || $0.navLabel.lowercased() == n
        }), tab != .comms || CommsGate.enabled else { return nil }
        self = tab
    }
}

// A scene route mirrors ONLY what the core parses (loadSceneGraph): modes are
// fixed | active-speaker | screen-share | capture-input; a nil rect lets the
// core auto-grid; sourceId is DERIVED core-side (zoom:/capture:/media:).
struct SceneRoute {
    var routeId: String
    var mode: String
    var audioRole = "mix"
    var participantId: String?
    var captureDeviceId: String?
    var rect: (x: Double, y: Double, w: Double, h: Double)?
    var zIndex: Int
    // Chroma key node, attached from the SOURCE's settings when the route is
    // built. The core keys per ROUTE (the same camera can be keyed in one scene
    // and not another), but an operator reasons per SOURCE — "that camera is on
    // a green screen" — so the setting lives on the slot and is applied to every
    // route bound to it.
    var chromaKey: JSONObject?
    // A media slot's identity (mediaAssetId/Name/Kind/Path/Loop), merged as-is.
    var media: JSONObject?

    var json: JSONObject {
        var object: JSONObject = [
            "routeId": routeId, "mode": mode, "audioRole": audioRole,
            "fitMode": "fill", "opacity": 1.0, "zIndex": zIndex,
        ]
        if let chromaKey { object["chromaKey"] = chromaKey }
        if let participantId { object["participantId"] = participantId }
        if let captureDeviceId { object["captureDeviceId"] = captureDeviceId }
        if let media { object.merge(media) { _, new in new } }
        if let rect {
            object["rect"] = ["x": rect.x, "y": rect.y, "width": rect.w, "height": rect.h]
        }
        return object
    }
}

// Built-in layouts with rects lifted from SceneCanvasLayoutService (single /
// two-up grid with 0.02 gap / pip full-frame + 0.28 inset). `layers` is the
// operator's edited canvas: empty = derive from the layout preset + slots
// (the previous behavior), non-empty = the canvas editor owns this scene.
struct SceneDef: Identifiable {
    let id: String
    var name: String
    let layout: String
    var layers: [SceneLayer] = []
    // SuperSource background: a media asset composited full-canvas BEHIND every
    // route (the core renders it as a "media-background" layer at order -100).
    // Per-scene, exactly like Windows — the picker on the Studio rail describes
    // the selected scene, not a global setting.
    var backgroundAssetId = ""
    /// CoreVideo Tiles settings: non-nil makes this a Tiles scene (Windows
    /// `Scene.DynamicGallery`). A value type, so a copied scene never aliases it.
    var tiles: TilesSettings? = nil
}

// One canvas layer: a slot binding plus its normalized rect/fit/opacity —
// the SceneCanvasLayerViewModel fields the core actually parses.
struct SceneLayer: Identifiable, Equatable {
    let id: String
    var slotId: Int?          // nil = follow the active speaker
    var x = 0.0
    var y = 0.0
    var width = 1.0
    var height = 1.0
    var fitMode = "fill"      // fit | fill | stretch
    var opacity = 1.0         // [0, 1]
    // Add overlay (Tier 0.14): a still from the media bin instead of a slot.
    var mediaAssetId: String? = nil
}

struct CaptureDeviceRow: Identifiable, Equatable {
    let id: String
    let name: String
    let kind: String
    let vendor: String
    var connectionState: String
    var signalPresent: Bool
    var warning: String
}

struct AudioStrip: Identifiable, Equatable {
    let id: String  // participantId == routing source id (zoom-mix, capture:<id>, …)
    var outputLevel: Int
    var manualGainDb: Double
    var muted: Bool
    var solo: Bool
    var pan: Double
    var status: String
    /// PRE-MUTE input level, 0..100 (#481): measured before mute/fader, so a muted
    /// guest who is talking still shows on a dimmed meter instead of silence.
    var inputLevel = 0
    /// Muted IN ZOOM (the roster's flag), display only: never the A1's mute (#481).
    var sourceMuted = false
    /// The channel compressor's measured gain reduction (core gainReductionDb).
    var gainReductionDb = 0.0
}

// Show-input slot (inputs 1–10): the operator's stable mapping from "what's
// plugged in" to production slots — the ShowInputSlot model from the Windows
// shell (docs/routing-ux-spec.md tab 1). Slots are the AUTHORITY for show
// membership; assignedIds/multiview/scenes derive from them.
struct ShowInputSlot: Identifiable, Equatable {
    var id: Int                 // 1...10 (var only so a drag-swap can renumber)
    var kind = "unassigned"     // unassigned | zoom | capture | media (sourceId = bin asset id)
    var sourceId = ""           // participant id or capture device id
    var name = ""               // editable display name
    // The source's OWN name (participant / device). A hand-typed `name` lasts
    // one session and one source: this is what is saved, and what the slot
    // shows again after a relaunch or a reassignment (#1213, SlotNamePolicy).
    var sourceName = ""
    var inShow = false
    var iso = false
    var offline = false         // source vanished from the roster/devices
    // A capture card's video and its audio are SEPARATE devices; this is the
    // operator's pairing. Empty means "no audio" — which the core reads as a
    // deliberately video-only source, not a mistake.
    var audioDeviceId = ""
    var audioDeviceName = ""
    // Green/blue screen keying for this source. Defaults match the core's
    // (green, 0.4 similarity, 0.1 smoothness, 0.2 spill) so an operator who
    // just switches it on gets a sane key rather than a blank frame.
    var keyEnabled = false
    var keyColorHex = "#00ff00"
    var keySimilarity = 0.4
    var keySmoothness = 0.1
    var keySpill = 0.2
    // Per-source dropout policy (core contract #535 slice 4a,
    // `set-source-policy`). "" = unset — the core's own default ("hold")
    // applies. "hold"/"black" are operator-chosen and ALWAYS sent (an explicit
    // "hold" must overwrite an earlier stored "black" — PRESENT-OR-KEEP, E9).
    // Zoom AND capture: the core takes a policy on "zoom:<pid>" and
    // "capture:<id>" (MediaCore::setSourcePolicy) and refuses any other id.
    // Only capture policies are saved (Zoom slots are meeting-scoped, plan D2).
    var dropoutPolicy = ""
}

// Per-channel built-in inserts. Names and keys are the CORE's contract
// (AudioDsp.h applyChannelInserts): inserts match by lowercase SUBSTRING —
// "gate"/"noise", "eq"/"voice", "compressor", "limiter" — and each reads its
// own keys, clamped core-side to the ranges mirrored below. An unknown key is
// silently ignored, so these names/keys must stay exact.
struct ChannelInserts: Equatable {
    var gateEnabled = false
    var gateThresholdDb = -48.0   // [-80, -12]
    var gateRangeDb = -60.0       // [-80, -6]
    var gateAttackMs = 5.0        // [0.5, 50]
    var gateHoldMs = 50.0         // [0, 500]
    var gateReleaseMs = 120.0     // [20, 500]

    var compEnabled = false
    var compThresholdDb = -18.0   // [-40, -6]
    var compRatio = 4.0           // [1, 20]
    var compAttackMs = 10.0       // [0.1, 100]
    var compReleaseMs = 120.0     // [10, 1000]
    var compKneeDb = 6.0          // [0, 24]
    var compMakeupDb = 0.0        // [0, 24]

    var eqEnabled = false
    var eqHighpassHz = 90.0       // [20, 400]
    var eqBandsDb = [Double](repeating: 0, count: 8)  // band1Db…band8Db, [-12, 12]

    // The Windows shell's built-in insert names (StudioViewModel
    // BuiltInInsertNames) — they satisfy the core's substring matcher.
    static let gateName = "Noise Gate"
    static let eqName = "Built-in EQ"
    static let compName = "Compressor"

    var activeNames: [String] {
        var names: [String] = []
        if gateEnabled { names.append(Self.gateName) }
        if eqEnabled { names.append(Self.eqName) }
        if compEnabled { names.append(Self.compName) }
        return names
    }

    var settingsJson: JSONObject {
        var settings: JSONObject = [:]
        if gateEnabled {
            settings[Self.gateName] = [
                "thresholdDb": gateThresholdDb, "rangeDb": gateRangeDb,
                "attackMs": gateAttackMs, "holdMs": gateHoldMs,
                "releaseMs": gateReleaseMs,
            ]
        }
        if eqEnabled {
            var eq: JSONObject = ["highpassHz": eqHighpassHz]
            for (index, gain) in eqBandsDb.enumerated() {
                eq["band\(index + 1)Db"] = gain
            }
            settings[Self.eqName] = eq
        }
        if compEnabled {
            settings[Self.compName] = [
                "thresholdDb": compThresholdDb, "ratio": compRatio,
                "attackMs": compAttackMs, "releaseMs": compReleaseMs,
                "kneeDb": compKneeDb, "makeupDb": compMakeupDb,
            ]
        }
        return settings
    }
}

// Master-bus mastering chain (MediaCore mastering parse / AudioMastering.h).
// Every field is optional core-side and absent keys keep struct defaults that
// are bit-identical to the pre-B1 fixed values — so an untouched rack is a
// true no-op, and `enabled` alone gates the whole chain.
//
// The 21 Windows fields (MasteringSettings, MasteringPresetCatalog.cs:10-31). The
// ten added for A3 (low-pass .. band trims, limiter) carry the Windows/core
// defaults (AudioMastering.h:29-58), so sending them explicitly is a no-op. The
// target stays a continuous LUFS value on the Mac (Phase 1 D10); presets set
// exactly -14/-16/-23. Codable for ShellPrefs: decoded key-by-key with defaults
// so an older file never throws (the ShellPrefs rule).
struct MasteringParams: Equatable, Codable {
    var enabled = false
    var targetLufs = -16.0
    var ceilingDbfs = -1.0
    var glueAmount = 0.3
    var maxRideDb = 6.0
    var inputGainDb = 0.0
    var highPassHz = 0.0
    var lowShelfDb = 0.0
    var presenceDb = 0.0
    var highShelfDb = 0.0
    var stereoWidth = 1.0
    var lowPassHz = 0.0
    // Top-level `limiterEnabled` of sync-participant-audio-mix (MediaCore.cpp:2943),
    // never a key of the `mastering` object. Windows MasterLimiterEnabled.
    var limiterEnabled = true
    var glueRatio = 2.0
    var glueAttackMs = 30.0
    var glueReleaseMs = 250.0
    var glueMakeupDb = 0.0
    var glueMultiband = false
    var glueBandLowDb = 0.0
    var glueBandMidDb = 0.0
    var glueBandHighDb = 0.0

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        func v<T: Decodable>(_ key: CodingKeys, _ fallback: T) -> T {
            ((try? c.decodeIfPresent(T.self, forKey: key)) ?? nil) ?? fallback
        }
        let d = MasteringParams()
        enabled = v(.enabled, d.enabled); targetLufs = v(.targetLufs, d.targetLufs)
        ceilingDbfs = v(.ceilingDbfs, d.ceilingDbfs); glueAmount = v(.glueAmount, d.glueAmount)
        maxRideDb = v(.maxRideDb, d.maxRideDb); inputGainDb = v(.inputGainDb, d.inputGainDb)
        highPassHz = v(.highPassHz, d.highPassHz); lowShelfDb = v(.lowShelfDb, d.lowShelfDb)
        presenceDb = v(.presenceDb, d.presenceDb); highShelfDb = v(.highShelfDb, d.highShelfDb)
        stereoWidth = v(.stereoWidth, d.stereoWidth); lowPassHz = v(.lowPassHz, d.lowPassHz)
        limiterEnabled = v(.limiterEnabled, d.limiterEnabled); glueRatio = v(.glueRatio, d.glueRatio)
        glueAttackMs = v(.glueAttackMs, d.glueAttackMs); glueReleaseMs = v(.glueReleaseMs, d.glueReleaseMs)
        glueMakeupDb = v(.glueMakeupDb, d.glueMakeupDb); glueMultiband = v(.glueMultiband, d.glueMultiband)
        glueBandLowDb = v(.glueBandLowDb, d.glueBandLowDb); glueBandMidDb = v(.glueBandMidDb, d.glueBandMidDb)
        glueBandHighDb = v(.glueBandHighDb, d.glueBandHighDb)
    }

    /// The `mastering` object, the 20 keys Windows sends (MediaCoreCommandBuilder.cs:519-545).
    var json: JSONObject {
        [
            "enabled": enabled, "targetLufs": targetLufs,
            "ceilingDbfs": ceilingDbfs, "glueAmount": glueAmount,
            "maxRideDb": maxRideDb, "inputGainDb": inputGainDb,
            "highPassHz": highPassHz, "lowPassHz": lowPassHz, "lowShelfDb": lowShelfDb,
            "presenceDb": presenceDb, "highShelfDb": highShelfDb,
            "stereoWidth": stereoWidth,
            "glueRatio": glueRatio, "glueAttackMs": glueAttackMs, "glueReleaseMs": glueReleaseMs,
            "glueMakeupDb": glueMakeupDb, "glueMultiband": glueMultiband,
            "glueBandLowDb": glueBandLowDb, "glueBandMidDb": glueBandMidDb, "glueBandHighDb": glueBandHighDb,
        ]
    }
}

// One multiview tile's chrome, from multiviewSharedTexture.tiles. The core
// composites PIXELS into the texture; labels and tally borders are the
// SHELL's overlay (the WinUI ShowMultiviewHost pattern) — without them a PGM
// or PVW cell with no source is an invisible black rectangle.
struct MultiviewTile: Identifiable, Equatable {
    let id: String
    var label: String
    var role: String      // pgm | pvw | source
    var tally: String     // the core's words: pgm | pvw | none (normalize with MultiviewOverlayFormatting)
    var activeSpeaker: Bool
    var x: Double
    var y: Double
    var w: Double
    var h: Double
}

struct RosterParticipant: Identifiable, Equatable {
    let id: String
    let name: String
    var hasVideo: Bool
    var muted: Bool
    var talking: Bool
    var assigned: Bool
    // The engine's screen-share flag (ZoomEngineRuntime.cpp:817) — the same
    // field MediaCore::deriveDirectorSignals reads. Defaulted so the memberwise
    // init stays source-compatible.
    var sharingScreen = false
    // The CoreVideo participant itself (the engine includes itself in the
    // roster, main-macos.mm:507). Never auto-assigned (plan D6).
    var isMe = false
    // The engine's per-participant source generation: a reused Zoom id with a
    // different generation is a different source (ZoomEngineRuntime.cpp).
    var sourceGeneration: Int64 = 0
    // The engine's persistent id for this participant (ZoomEngineRuntime.cpp:1443),
    // for the grading plan's per-person grades. nil when not reported.
    var persistentId: String? = nil

    // PURE, so it can be tested without a running core. This mapping is the one
    // that shipped an empty roster in a live meeting: the shell read
    // id/name/hasVideo while BOTH engine paths emit
    // userId/displayName/videoOn/muted/talking
    // (ZoomEngineRuntime::rawCaptureSnapshotLocked, the same shape the WinUI
    // shell consumes). Both spellings are accepted and both are covered by
    // tests, because getting this wrong looks like "nobody is in the meeting".
    static func parse(_ entries: [JSONObject],
                      assignedIds: Set<String> = []) -> [RosterParticipant] {
        entries.compactMap { entry in
            guard let idValue = entry["userId"] ?? entry["id"] else { return nil }
            let id = "\(idValue)"
            return RosterParticipant(
                id: id,
                name: entry["displayName"] as? String ?? (entry["name"] as? String ?? id),
                hasVideo: entry["videoOn"] as? Bool ?? (entry["hasVideo"] as? Bool ?? false),
                muted: entry["muted"] as? Bool ?? (entry["isMuted"] as? Bool ?? false),
                talking: entry["talking"] as? Bool ?? (entry["isTalking"] as? Bool ?? false),
                assigned: assignedIds.contains(id),
                sharingScreen: entry["sharingScreen"] as? Bool ?? false,
                isMe: entry["isMe"] as? Bool ?? false,
                sourceGeneration: (entry["sourceGeneration"] as? NSNumber)?.int64Value ?? 0,
                persistentId: (entry["persistentId"] as? String).flatMap { $0.isEmpty ? nil : $0 })
        }
    }
}

@MainActor
final class AppModel: ObservableObject {
    @Published var status: BridgeStatus = .launching
    @Published var statusDetail = ""
    /// Re-entrancy guard for `updateTiles` (current and highest depth seen).
    var tilesEditDepth = 0
    var tilesEditMaxDepth = 0
    var tilesSaveTask: Task<Void, Never>?
    /// The Members tab's selected member id: "" when nothing is selected, never nil
    /// (Review Focus 3: a Take with an empty selection must not trap).
    @Published var tilesSelectedMemberId = ""
    /// The Tiles inspector's selected tab (Windows Pivot: Layout | Style | Members).
    @Published var tilesInspectorTab: TilesInspectorTab = .layout
    /// Sticky Tiles audio (Windows TilesAudioSourceLatch), observed once per spine build.
    let tilesAudioLatch = TilesAudioSourceLatch()
    /// True once a Zoom snapshot with a non-empty meetingState arrived on this core generation.
    var tilesMeetingStateObserved = false
    @Published var meetingState = "idle" {
        didSet {
            latchZoomCameraCeilingIfJoined()   // Z1: a join reached the meeting
            feedCommsMeeting()
        }
    }
    /// Z1 (ZoomCameraQuality.swift): the operator's ceilings, what the in-flight
    /// join sent, and what the current meeting applied.
    @Published var zoomCameraMaxResolution = ZoomCameraResolutionPreference.defaultChoice
    @Published var zoomCameraMaxFps = ZoomCameraFrameRatePreference.defaultFps
    @Published var zoomCameraApplied = ZoomCameraCeiling()
    var zoomCameraJoinSent: ZoomCameraCeiling?
    @Published var rawMediaActive = false
    @Published var roster: [RosterParticipant] = []
    /// The Zoom audio source set last placed on the spine (participant-audio ids,
    /// in budget order) and whether meeting-audio was subscribed with it. The audio
    /// console builds its strips from these — the FADER LAW needs a strip for
    /// every source the core is sent, not only for the ones it has echoed.
    @Published var zoomAudioSourceIds: [String] = []
    /// A1: the Zoom -> program topology (Windows StudioViewModel._zoomAudioMode,
    /// default perGuestIso). Changed only through setZoomAudioMode.
    @Published private(set) var zoomAudioMode: ZoomAudioMode = .perGuestIso
    @Published var meetingAudioSubscribed = false
    @Published var assignedIds: Set<String> = []
    @Published var recordingStatus = "idle"
    @Published var recordingDesired = false {
        // Take over: the next take arms from presence again (Windows
        // ResetRecordingIsoSelection on RecordingRequested -> false).
        didSet { if !recordingDesired { recordingIso.reset() } }
    }
    private var recordingIso = RecordingIsoSelection()
    private var recordingCommands = RecordingCommandPolicy()
    @Published var recordingStartedAt: Date?
    @Published var recordingArtifactPath = ""
    @Published var recordingWarning = ""
    @Published var masterLevel = 0
    // Footer MASTER L/R (StudioFooterReadout.masterLevels): measured bus-tap
    // peaks per side, else the mono masterLevel on both.
    @Published var masterLevelLeft = 0
    @Published var masterLevelRight = 0
    @Published var recordingDroppedFrames = 0
    @Published var recordingFramesWritten = 0

    /// Windows' "0 (0.0%)" readout. Percentage of ATTEMPTED frames, so it stays
    /// meaningful whether a show ran four minutes or four hours.
    var droppedFramesLabel: String {
        let attempted = recordingFramesWritten + recordingDroppedFrames
        guard attempted > 0 else { return "0" }
        let percent = Double(recordingDroppedFrames) / Double(attempted) * 100
        return String(format: "%d (%.1f%%)", recordingDroppedFrames, percent)
    }
    @Published var strips: [AudioStrip] = []
    @Published var channelInserts: [String: ChannelInserts] = [:]
    // VST3 plug-in inserts — all logic lives in VstInserts.swift (stored
    // properties cannot be declared in an extension, so only the state is here).
    @Published var vstHost = VstHostState()
    @Published var vstPlugins: [VstPlugin] = []
    @Published var vstChannelSelection: [String: String] = [:]  // stripId -> "vst:<name>"
    @Published var vstParamLocal: [UInt32: Double] = [:]        // held during a drag only
    let vstScratch = VstScratch()  // debounce task + quiet-window bookkeeping
    // #870: the core's audioObservation envelope + when the shell received it.
    @Published var audioObservation: AudioObservation?
    var audioObservationReceivedAt = Date()
    @Published var shortTermLufs = -120.0
    // A4: the rest of the core's masterMeter, and the rail's loudnessLufs.
    @Published var integratedLufs = -120.0
    @Published var momentaryLufs = -120.0
    @Published var loudnessLufs = -120.0
    @Published var mixedFrameCount = 0
    @Published var truePeakDbfs = -120.0
    @Published var limiterActive = false
    @Published var masteringRideDb = 0.0
    @Published var monitorStatus = ""
    @Published var monitorFeedbackRisk = false
    @Published var monitorEnabled = false
    @Published var monitorVolume = 0.7
    @Published var monitorControlNotice = ""
    private(set) var monitorControl = MonitorControlProjection()
    // Monitor output device (Tier 0.12, MonitorOutputDevice.swift). The
    // choice is a CoreAudio UID ("" = system default output), persisted; the
    // list is nil until CoreAudio has been enumerated.
    @Published var monitorDeviceChoice = ""
    @Published var monitorDeviceChoiceName = ""
    @Published var monitorOutputDevices: [MonitorOutputDevice]?
    @Published var monitorDeviceWarning = ""
    private var monitorDevicesWatched = false
    private var monitorControlSending = false
    @Published var audioRouteControlNotice = ""
    private(set) var audioRouteControl = AudioRouteControlProjection()
    @Published var rosterRecoveryNotice = ""
    private var rosterProjection = ZoomRosterProjection()
    @Published var warnings: [String] = []
    @Published var programSurfaceId: UInt32 = 0
    @Published var programFrameNumber: Int64 = 0
    @Published var previewSurfaceId: UInt32 = 0
    @Published var multiviewSurfaceId: UInt32 = 0
    @Published var multiviewTiles: [MultiviewTile] = []
    @Published var captureDevices: [CaptureDeviceRow] = []
    @Published var clockText = ""
    @Published var selectedTab: StudioTab = .studio {
        // S6: the core renders the source sheet only while the Sources tab holds the lease.
        didSet {
            if selectedTab != oldValue { updateSourcePreviewDemand() }
            // Navigating away from Comms: every held key goes up (panel-scoped
            // keys; Windows StudioViewModel.Comms.cs:57-60).
            if oldValue == .comms, selectedTab != .comms { releaseCommsKeys() }
        }
    }
    @Published var joinMeetingId = ""
    @Published var joinPasscode = ""
    @Published var displayName = "CoreVideo Pro (mac)"
    /// The Mac's starter catalog (plan D-C3, the twin of Windows ProductionCatalog.Scenes):
    /// the built-in scenes, in catalog order, with their preset (empty) layers.
    static let starterScenes: [SceneDef] = [
        SceneDef(id: "fullscreen", name: "Fullscreen", layout: "single"),
        SceneDef(id: "two-up", name: "Side by side", layout: "two-up"),
        SceneDef(id: "pip", name: "Picture in picture", layout: "pip"),
    ]
    static var starterSceneIds: [String] { starterScenes.map(\.id) }
    @Published var scenes: [SceneDef] = AppModel.starterScenes
    /// Starter ids the operator deleted (Windows RemovedCatalogSceneIds), persisted and
    /// cleared by restoreStarterScenes(). BOOKKEEPING ONLY on the Mac: Windows re-seeds its
    /// catalog every launch and needs the set to drop removed scenes again
    /// (SceneCatalogPolicy.RemovalsToApply); the Mac persists every rail scene, so a deleted
    /// starter stays deleted without it, and Restore keys on presence, as Windows does.
    // Nothing on the Mac reads this set yet (owner decision 2026-11-23: keep it Windows-shaped).
    @Published var removedStarterSceneIds: [String] = []
    @Published var programSceneId = "" { didSet { mediaAudioSceneChanged(oldValue != programSceneId) } }
    @Published var previewSceneId = "" { didSet { mediaAudioSceneChanged(oldValue != previewSceneId) } }
    @Published var mediaAssets: [MediaAsset] = []
    @Published var selectedAssetId = ""
    /// The Media tab's one status line (Windows CommandStatus for media).
    @Published var mediaStatus = ""
    /// The core's media transports, last parsed. Unchanged when a snapshot has no node.
    @Published private(set) var mediaSources: [MediaSourceRow] = []
    /// profile.capabilityStates["media-clip-playback"].state == "available". False until a
    /// snapshot says so: an unknown state disables clip Cue/Play honestly.
    @Published private(set) var mediaClipPlaybackAvailable = false
    @Published private(set) var mediaPlayback: MediaPlaybackReadModel?
    /// The selection's play flag: the core's row when the clip is on Program, the
    /// local audition otherwise.
    @Published private(set) var selectedMediaPlaying = false
    /// The live set, the only bin-wide play-state scalar (rows read it).
    @Published private(set) var mediaLiveSignature = ""
    private var lastSelectedTransportState: String?
    private var lastMediaRefusalSeen: String?
    private let mediaTransportGestures = MediaTransportGestureLedger()
    /// Test seam for the one-shot transport gesture; nil = the real core request.
    var mediaTransportSendForTesting: ((JSONObject) async throws -> JSONObject)?
    static let mediaTransportRetryDelayNs: UInt64 = 120_000_000
    /// Test seam for the shell's debounce/retry timers (the routing-push debounce
    /// and the media transport retry): the harness decides when a timer elapses
    /// instead of racing wall-clock spins on a slow CI runner. nil = Task.sleep.
    var timerSleepForTesting: ((UInt64) async -> Void)?
    func timerSleep(nanoseconds: UInt64) async {
        if let sleep = timerSleepForTesting {
            await sleep(nanoseconds)
        } else {
            try? await Task.sleep(nanoseconds: nanoseconds)
        }
    }
    private var mediaDurationProbeGeneration = 0
    @Published var logoBug: MediaAsset? {  // rides every scene as a top-right route
        didSet { mediaAudioSceneChanged(oldValue?.id != logoBug?.id) }
    }
    @Published var lowerThirdName = ""
    @Published var lowerThirdTitle = ""
    @Published var lowerThirdPosition = "lower-left"
    @Published var lowerThirdPhase = "hidden"  // truthful phase from overlayState
    /// The core's overlayState `visible` for key:lower-third: true only when the
    /// keyed source is on rendered Program and the key is actually drawn.
    @Published var lowerThirdVisible = false
    /// The source the manual key names (a Program source, never "mac:manual").
    @Published var lowerThirdKeyedSourceId = ""
    @Published var lowerThirdKeyedName = ""
    /// The last Key in / Key out outcome ("Lower third needs a program source").
    @Published var lowerThirdCommandStatus = ""
    @Published var overlaysOnAir = 0
    // Director recommendation (core-computed, shell-actuated — the core NEVER
    // switches scenes itself; recommend-auto-production is a documented no-op).
    @Published var autoRuleId = ""
    @Published var autoSceneId = ""
    @Published var autoConfidence = 0
    @Published var autoRationale = ""
    static let sceneAutomationAvailable = false
    @Published private(set) var autoDirectEnabled = false
    @Published var autoTakeEnabled = false
    @Published var autoConfidenceThreshold = 70.0
    @Published var autoHoldSeconds = 4.0
    @Published var autoStatus = "Manual mode"
    // AUTOMATION PARITY (behaviour lives in AutomationExtras.swift). Stored here
    // only because Swift extensions cannot add stored properties.
    @Published var autoExtras = AutomationExtrasState()
    // Which source the AUTOMATIC lower third currently names; "" = automation
    // does not own key:lower-third (the manual lower third may).
    @Published var autoLowerThirdSourceId = ""
    @Published var isoRecordingEnabled = false {
        // ISO-armed guests are sources: arming or disarming changes the spine.
        didSet { if oldValue != isoRecordingEnabled { syncSpine() } }
    }
    /// THE ISO selection (Windows ShowInputsCoordinator.IsoSelectedSourceIds):
    /// scheme-qualified source ids. The Sources-tab ISO toggles, the Routing
    /// matrix and the footer all write THIS; the slot `iso` flags and the
    /// matrix columns are projections of it, re-derived on every change.
    @Published var isoSelectedSourceIds: Set<String> = [] {
        didSet { projectIsoSelection() }
    }
    @Published var streamUrl = "rtmp://a.rtmp.youtube.com/live2"
    @Published var streamKey = ""
    @Published var streamingDesired = false {
        didSet {
            guard streamingDesired != oldValue else { return }
            // Review finding 4: the destinations being streamed are frozen at
            // Start. Without this an SRT/HLS edit mid-stream reached the core on
            // the next full-state start-program-output (a recording toggle) and
            // silently restarted the destination with half-typed settings.
            if streamingDesired {
                liveStreamDestinations = (streamConfig, streamSecrets)
            } else {
                liveStreamDestinations = nil
            }
        }
    }
    /// The config + secrets captured when streaming started (nil when off).
    private(set) var liveStreamDestinations: (config: StreamDestinationsConfig, secrets: StreamDestinationSecrets)?
    /// True while destination settings must not change (the editor is locked).
    var streamDestinationsLocked: Bool { streamingDesired || streamingObservedLive }
    @Published var streamStatus = ""       // sender status from outputSenderSession
    @Published var streamDetail = ""       // lastResultCode / warning
    /// The destination list (mac streaming Phase 2, D1). rtmpUrl is NOT read
    /// from here: `streamUrl` stays the one RTMP URL (remote configure,
    /// Diagnostics and the flyout all write it); see `streamConfig`.
    @Published var streamDestinations = StreamDestinationsConfig()
    /// SRT passphrase: Keychain only (StreamKeychain.srtPassphraseAccount).
    @Published var srtPassphrase = ""
    /// Per-destination status rows, projected from the core's lifecycle +
    /// supervisor on every snapshot (DestinationStatusProjection).
    @Published var destinationRows: [String: DestinationStatus] = [:]
    /// What the sender resolved as its ffmpeg ("" until a sender reports one).
    @Published var streamRuntimeDetail = ""
    /// Whether the core's stream ffmpeg runs under corevideo-child-guard (nil =
    /// not reported yet). false means a crashed core can leave ffmpeg running.
    @Published var streamChildGuard: Bool?
    var warnedUnguardedChild = false

    // The LAST snapshots as the core sent them. applySnapshot/applyZoom cherry-
    // pick the slices the panes bind to; the Health surface and the support
    // bundle need the WHOLE thing (health/encoderSession/outputSenderSession/
    // recording.proof/zoom.readiness were all on the wire and thrown away).
    // Never render these raw — Diagnostics.swift redacts before anything is
    // written to disk.
    @Published var lastSnapshot: JSONObject = [:]
    @Published var lastZoomSnapshot: JSONObject = [:]
    /// Lock-protected copy of `lastSnapshot` + its receipt time for the control
    /// API's GET /snapshot (ControlSurface.swift): served from any thread with no
    /// main-actor hop and no core round-trip (Windows reads the reference the
    /// bridge already published). Redacted at the boundary, never here.
    nonisolated let controlSnapshotBox = ControlSnapshotBox()
    let controlApi = ControlApiHost()

    // Readable (not settable) outside this file so cross-file extensions
    // (VstInserts.swift) can issue their own RPCs; the lifecycle stays owned here.
    private(set) var bridge: MediaCoreBridge?
    // Quit guard (T1.8 parity, QuitGuard.swift). The gate allows one prompt at a
    // time; `snapshotSequence` lets the finish wait accept only snapshots that
    // arrived AFTER the stop landed (a stale pre-stop `completed` never counts).
    private var quitGate = QuitGate()
    private(set) var snapshotSequence = 0
    @Published var quitPromptVisible = false
    @Published var quitFinishing = false
    /// Test seam: replaces the real stop/finish/shutdown so the flow can be driven
    /// without a core. Production leaves it nil.
    var quitReplyForTesting: ((Bool) -> Void)?
    /// Test seams (production leaves them at their defaults): the request path,
    /// the "core is gone" answer when that path is replaced, and the timings.
    var quitRequestForTesting: ((JSONObject, TimeInterval) async throws -> JSONObject)?
    var quitCoreGoneForTesting = false
    /// Remote control (ControlSurface+Session.swift): how long an awaitable
    /// control call waits for meetingState / a device state to settle after the
    /// core answers. Tests shorten it.
    var controlSettleSeconds: TimeInterval = 5
    /// The Settings "Remote control" row (ControlApiHost refreshes it, at most once a second).
    @Published var remoteControlStatus = RemoteControlStatus.idle
    /// Test seam for the capture connect/disconnect path (production: nil = bridge).
    var captureRequestForTesting: ((JSONObject) async throws -> JSONObject)?
    /// Where a remotely configured stream key is stored (the Keychain, never
    /// prefs); false = not saved. Tests replace it so the harness never writes
    /// the operator's Keychain.
    var streamKeySaver: (String) -> Bool = { StreamKeychain.save($0); return true }
    /// Where Start streaming stores the secrets it is about to use: (value,
    /// Keychain account). Tests replace it so the harness never writes the
    /// operator's Keychain.
    var streamSecretSaver: (String, String) -> Void = { StreamKeychain.save($0, account: $1) }
    /// Where a remotely configured SRT destination is saved BEFORE it is applied:
    /// the prefs (with the new config) and the Keychain passphrase; false = not
    /// saved. Prefs are nil until the operator's file has been restored. Tests
    /// replace it so the harness never writes the operator's Keychain.
    var srtDestinationSaver: (ShellPrefs?, String) -> Bool = { prefs, passphrase in
        StreamKeychain.save(passphrase, account: StreamKeychain.srtPassphraseAccount)
        prefs?.save()
        return true
    }
    /// Per-source grades (grading plan Task 5): the only sender of set-source-grade.
    /// Harness-only: the saved-grade file the NEXT AppModel() uses (nil → SourceGradeFile.path),
    /// so one harness case's debounced save can never land in another case.
    static var gradeFilePathForTesting: String?
    /// Open per-source grade editors, at most three, keyed by grade source id (Task 6).
    @Published var gradeEditors: [String: GradeEditorModel] = [:]
    /// The last refusal from opening a grade editor, shown under the inspector's Grade section.
    @Published var gradeNotice = ""
    /// The core's `gradePreview.supported` (Metal grade-preview worker present; Phase 2 G5).
    @Published var gradePreviewSupported = false
    private let gradeFilePath = AppModel.gradeFilePathForTesting ?? SourceGradeFile.path
    /// The ONE graphics sub-model (graphics G1, spec 5.1): decode, gateway,
    /// playout and the On Air strip. No graphics behaviour lives on AppModel.
    private(set) lazy var graphics: GraphicsModel = {
        let model = GraphicsModel(coreRunning: { [weak self] in
            if case .connected = self?.status { return true }
            return false
        }, send: { [weak self] body in
            guard let self else { throw BridgeError.notRunning }
            return try await self.gradeRequest(body)
        })
        model.gateway.elapsedMs = { [weak self] in self?.elapsedMs() ?? 0 }
        // Guest rows expand over the Show Input slots, as Windows' ShowInputEditors.
        model.rosterSlots = { [weak self] in
            (self?.slots ?? []).map {
                GraphicsRosterSlot(slotNumber: $0.id, sourceId: ControlSourceId.canonical(kind: $0.kind, sourceId: $0.sourceId),
                                   derivedName: $0.name.isEmpty ? $0.sourceName : $0.name)
            }
        }
        model.loadDocument()
        return model
    }()
    private(set) lazy var sourceGrades: SourceGradeStore = {
        let store = SourceGradeStore(send: { [weak self] body in
            guard let self else { throw BridgeError.notRunning }
            return try await self.gradeRequest(body)
        }, filePath: gradeFilePath)
        store.identityProvider = { [weak self] in self?.gradeIdentity(for: $0) }
        return store
    }()
    /// Test seams for where the quit prompt is presented (production: nil).
    var quitPresentForTesting: ((QuitPromptPlacement) -> Void)?
    var mainWindowAvailableForTesting: Bool?
    var quitFinishTimeout = QuitGuardPolicy.finishTimeout
    var quitCoreGrace = QuitGuardPolicy.coreExitGrace
    var quitPollInterval: TimeInterval = 0.1
    private var syncTimer: Timer?
    private var zoomTimer: Timer?
    private var startedAt = Date()
    private var programOutputStarted = false
    private var joinInFlight = false

    private(set) var lastSavedPrefs = ShellPrefs()
    /// True once restorePrefs() ran; persistPrefsNow() refuses before that.
    private var prefsRestored = false

    // ── virtual camera (roadmap M9.2; VirtualCamera.swift) ──────────────────
    /// Installs/queries the camera system extension. Held strongly here:
    /// OSSystemExtensionRequest.delegate is weak, and the activator owns the
    /// in-flight requests. Tests swap in FakeCameraExtensionActivator.
    var cameraExtension: CameraExtensionActivating = SystemCameraExtensionActivator()
    @Published private(set) var cameraExtensionState: CameraExtensionState = .unknown
    /// Operator intent (persisted). Change it only through the setters below.
    @Published var virtualCamera = PersistedVirtualCamera()
    /// The core's last `virtualCamera` node (nil = never observed).
    @Published var virtualCameraCore: VirtualCameraReadModel? {
        didSet { observeVirtualCameraLoss() }
    }
    /// Mid-session: the core reporting the device lost/not found for longer
    /// than this while the extension reads .active re-runs the registration
    /// check (the old copy may have exited after we called the swap done).
    var cameraLostRecheckAfter: TimeInterval = 10
    private var virtualCameraLostSince: Date?
    private var virtualCameraLostRechecked = false
    /// nil = never asked; true after the first enable (persisted).
    var cameraExtensionOptedIn: Bool?
    /// A properties query that ends with didFinish and no foundProperties never
    /// calls back; the shell must not wait on it forever.
    var cameraExtensionQueryTimeout: TimeInterval = 5
    private var cameraExtensionQueryToken: UUID?
    private var cameraExtensionInstallsToken: UUID?
    /// A NEW process's CoreMediaIO view of our camera device (registered? in
    /// use?), never this process's own: see FreshProcessCameraDeviceProbe.
    /// Tests swap in FakeCameraDeviceProbe. A read blocks until that process
    /// answers, so it is never made here: `cameraDeviceReader` runs every read
    /// off the main actor, one at a time (readCameraDevice).
    var cameraDeviceProbe: CameraDeviceProbing {
        get { cameraDeviceReader.probe }
        set { cameraDeviceReader.probe = newValue }
    }
    let cameraDeviceReader = CameraDeviceReader(probe: FreshProcessCameraDeviceProbe())
    /// One `.updateReady` retry check at a time: a tick that finds the last
    /// tick's read still out does nothing.
    private var cameraUpdateRetryChecking = false
    /// While `.updateReady`, how often the shell re-checks whether the camera
    /// is free and installs the update on its own.
    var cameraUpdateRetryInterval: TimeInterval = 5
    /// Update camera: how long to wait for the camera to stop after pausing our
    /// own output before giving up (another app still has it).
    var cameraUpdateReleaseWait: TimeInterval = 5
    var cameraUpdatePollInterval: TimeInterval = 0.5
    private var cameraUpdateTimer: Timer?
    /// After a completed activation: how long to wait for our device to appear
    /// in the HAL before asking macOS whether only a restart finishes it.
    var cameraRegistrationTimeout: TimeInterval = 10
    var cameraRegistrationPollInterval: TimeInterval = 0.5
    private var cameraRegistrationToken: UUID?

    /// True while a camera-extension check is still out: an installs query, a
    /// device read, an activation, the registration check, or an operator's
    /// Update camera. Tests wait on this instead of sleeping a fixed time.
    var cameraExtensionCheckPending: Bool {
        cameraExtensionInstallsToken != nil || cameraRegistrationToken != nil || cameraDeviceReader.isReading
            || cameraExtensionState == .activating || cameraUpdateInProgress
    }
    /// The last HAL reading of our device (diagnostics; nil = never read).
    @Published private(set) var cameraDeviceStatus: CameraDeviceStatus?
    /// The last activation outcome, with the time (diagnostics).
    @Published private(set) var cameraExtensionLastResult = ""
    /// True only while an operator's Update camera has paused our own output
    /// (the wire says off; the persisted intent is untouched).
    @Published private(set) var virtualCameraPausedForUpdate = false
    /// An operator's Update camera is running (from the click until the
    /// replacement finishes or the wait gives up). A second click and the 5 s
    /// retry do nothing meanwhile, and the status reads "Updating camera…".
    @Published private(set) var cameraUpdateInProgress = false
    /// Another app (not our output) was seen holding the camera while an
    /// update waited: the notice says "quit apps", else "click Update camera".
    @Published private(set) var cameraUpdateOthersHolding = false
    /// Opted-in launch with the output on: hold the first
    /// sync-virtual-camera {on:true} until the launch decision (and any swap
    /// it starts) is done, so our own sink stream never blocks the update and
    /// the device reads only OTHER apps (review I3). Bounded by
    /// `cameraLaunchHoldLimit`.
    @Published private(set) var virtualCameraHeldForLaunch = false
    var cameraLaunchHoldLimit: TimeInterval = 20
    /// The last installed-copies answer (diagnostics; nil = never answered).
    @Published private(set) var cameraExtensionInstalls: [CameraExtensionInstall]?
    /// TEST-ONLY command capture. Set only by ShellTests: no env var, control
    /// command or wire field reaches it. When set, commands this seam covers are
    /// handed to it instead of the bridge.
    var coreCommandCaptureForTesting: ((JSONObject) -> Void)?

    func currentPrefs() -> ShellPrefs {
        var prefs = ShellPrefs()
        prefs.joinMeetingId = joinMeetingId
        prefs.displayName = displayName
        prefs.monitorEnabled = monitorEnabled
        prefs.monitorVolume = monitorVolume
        prefs.monitorDeviceId = monitorDeviceChoice.isEmpty ? nil : monitorDeviceChoice
        prefs.monitorDeviceName = monitorDeviceChoice.isEmpty ? nil : monitorDeviceChoiceName
        prefs.isoRecordingEnabled = isoRecordingEnabled
        prefs.isoSelectedSourceIds = isoSelectedSourceIds.sorted()
        prefs.autoTakeEnabled = autoTakeEnabled
        prefs.autoConfidenceThreshold = autoConfidenceThreshold
        prefs.autoHoldSeconds = autoHoldSeconds
        prefs.automation = autoExtras
        prefs.lowerThirdName = lowerThirdName
        prefs.lowerThirdTitle = lowerThirdTitle
        prefs.lowerThirdPosition = lowerThirdPosition
        prefs.logoBugAssetId = logoBug?.id ?? ""
        prefs.streamUrl = streamUrl
        prefs.streamDestinations = streamDestinations
        prefs.removedStarterSceneIds = removedStarterSceneIds.isEmpty ? nil : removedStarterSceneIds
        prefs.recentMeetings = recentMeetings
        prefs.webinar = webinar
        prefs.colorGrade = [gradeExposure, gradeContrast, gradeSaturation, gradeTemperature]
        prefs.overlays = overlays
        prefs.capturePairings = slots
            .filter { !$0.audioDeviceId.isEmpty }
            .map { CapturePairing(slotId: $0.id, audioDeviceId: $0.audioDeviceId,
                                  audioDeviceName: $0.audioDeviceName,
                                  sourceKey: $0.sourceKey) }
        prefs.chromaKeys = slots.filter(\.keyEnabled).map {
            PersistedChromaKey(slotId: $0.id, colorHex: $0.keyColorHex,
                               similarity: $0.keySimilarity,
                               smoothness: $0.keySmoothness, spill: $0.keySpill,
                               sourceKey: $0.sourceKey)
        }
        prefs.slotDropoutPolicies = slots
            .filter { $0.kind == "capture" && !$0.dropoutPolicy.isEmpty }
            .map { PersistedSlotPolicy(slotId: $0.id, dropoutPolicy: $0.dropoutPolicy,
                                       sourceKey: $0.sourceKey) }
        // Capture slots only (plan D2): Zoom reuses user ids across meetings, so a
        // restored Zoom slot could attach a different person's name and settings.
        // Media too: a bin asset id is stable across launches.
        prefs.showInputSlots = slots.filter { ($0.kind == "capture" || $0.kind == "media") && $0.sourceKey != nil }.map {
            PersistedSlot(slotId: $0.id, kind: $0.kind, sourceId: $0.sourceId,
                          name: SlotNamePolicy.persisted($0), inShow: $0.inShow)
        }
        prefs.vstChannelSelections = vstChannelSelection.isEmpty ? nil : vstChannelSelection
        prefs.autoAssignInputs = autoAssignInputsEnabled
        prefs.zoomAudioMode = zoomAudioMode.persisted
        prefs.zoomCameraMaxResolution = zoomCameraMaxResolution
        prefs.zoomCameraMaxFps = zoomCameraMaxFps
        prefs.commsPrivate = comms.privatePreference ? true : nil
        prefs.localAudioEnabled = localAudio.enabled
        prefs.localAudioDeviceId = localAudio.deviceId.isEmpty ? nil : localAudio.deviceId
        prefs.localAudioDeviceName = localAudio.deviceName.isEmpty ? nil : localAudio.deviceName
        prefs.sourcesWorkspace = sourcesWorkspace.columns
        prefs.cameraExtensionOptedIn = cameraExtensionOptedIn
        prefs.virtualCamera = virtualCamera == PersistedVirtualCamera() ? nil : virtualCamera
        storeProductionSettings(into: &prefs)
        storeMastering(into: &prefs)
        prefs.recordingFolder = recordingFolderChoice.isEmpty ? nil : recordingFolderChoice
        prefs.scenes = railScenes.map { scene in
            PersistedScene(id: scene.id, name: scene.name, layout: scene.layout,
                           backgroundAssetId: scene.backgroundAssetId,
                           layers: scene.layers.map { layer in
                               PersistedLayer(id: layer.id, slotId: layer.slotId,
                                              x: layer.x, y: layer.y,
                                              width: layer.width, height: layer.height,
                                              fitMode: layer.fitMode,
                                              opacity: layer.opacity,
                                              mediaAssetId: layer.mediaAssetId)
                           },
                           tiles: scene.tiles?.persisted)
        }
        return prefs
    }

    /// `ScenePersistenceService.SceneFromPersisted`: explicit settings go through
    /// the restore clamps; an older Tiles scene that kept the layout discriminator
    /// but lost its settings gets the defaults (docs/archive/qa/tiles-incident-2026-09-07.md).
    static func restoredTiles(_ saved: PersistedScene) -> TilesSettings? {
        if let tiles = saved.tiles { return TilesSettings(persisted: tiles) }
        return saved.layout == "dynamic-gallery" ? TilesSettings() : nil
    }

    private func restorePrefs() {
        applyRestoredPrefs(ShellPrefs.load())
        streamKey = StreamKeychain.load()
        srtPassphrase = StreamKeychain.load(account: StreamKeychain.srtPassphraseAccount)
    }

    /// Everything a relaunch restores from the prefs file, minus the file read
    /// and the Keychain (so the harness can drive the real restore path).
    func applyRestoredPrefs(_ prefs: ShellPrefs) {
        // One restore per session (ShowInputsCoordinator.cs:146-153): a second load
        // would overwrite live operator changes with stale disk state.
        guard !prefsRestored else {
            ShellLog.write("roster: restore REFUSED — already restored this session; "
                + "a re-restore would overwrite live operator changes with disk state")
            return
        }
        overlays = (prefs.overlays ?? OverlaysState()).migratingLegacyTiming()
        restoreSlotAssignments(prefs.showInputSlots ?? [])
        // No reapply: no roster exists at launch.
        assignmentLaw.autoAssignEnabled = prefs.autoAssignInputs ?? true
        autoAssignInputsEnabled = assignmentLaw.autoAssignEnabled
        zoomAudioMode = ZoomAudioMode.parse(prefs.zoomAudioMode)
        zoomCameraMaxResolution = ZoomCameraResolutionPreference.normalize(prefs.zoomCameraMaxResolution)
        zoomCameraMaxFps = ZoomCameraFrameRatePreference.normalize(
            prefs.zoomCameraMaxFps ?? ZoomCameraFrameRatePreference.defaultFps)
        comms.restorePrivatePreference(prefs.commsPrivate ?? false)
        localAudio = LocalAudioSetting(enabled: prefs.localAudioEnabled ?? false,
                                       deviceId: prefs.localAudioDeviceId ?? "",
                                       deviceName: prefs.localAudioDeviceName ?? "")
        sourcesWorkspace.columns = prefs.sourcesWorkspace ?? SourcesColumnPrefs()
        for key in prefs.chromaKeys ?? [] {
            if let index = restoredSlotIndex(sourceKey: key.sourceKey, legacySlotId: key.slotId) {
                slots[index].keyEnabled = true
                slots[index].keyColorHex = key.colorHex
                slots[index].keySimilarity = key.similarity
                slots[index].keySmoothness = key.smoothness
                slots[index].keySpill = key.spill
            }
        }
        for pairing in prefs.capturePairings ?? [] {
            if let index = restoredSlotIndex(sourceKey: pairing.sourceKey,
                                             legacySlotId: pairing.slotId) {
                slots[index].audioDeviceId = pairing.audioDeviceId
                slots[index].audioDeviceName = pairing.audioDeviceName
            }
        }
        for policy in prefs.slotDropoutPolicies ?? [] {
            if let index = restoredSlotIndex(sourceKey: policy.sourceKey, legacySlotId: policy.slotId),
               slots[index].kind == "capture" {
                slots[index].dropoutPolicy = policy.dropoutPolicy
            }
        }
        if prefs.colorGrade.count == 4 {
            gradeExposure = prefs.colorGrade[0]
            gradeContrast = prefs.colorGrade[1]
            gradeSaturation = prefs.colorGrade[2]
            gradeTemperature = prefs.colorGrade[3]
        }
        vstChannelSelection = prefs.vstChannelSelections ?? [:]
        restoreMastering(from: prefs)
        joinMeetingId = prefs.joinMeetingId
        displayName = prefs.displayName
        monitorEnabled = prefs.monitorEnabled
        monitorVolume = prefs.monitorVolume
        monitorDeviceChoice = prefs.monitorDeviceId ?? ""
        monitorDeviceChoiceName = prefs.monitorDeviceName ?? ""
        isoRecordingEnabled = prefs.isoRecordingEnabled
        isoSelectedSourceIds = Set(prefs.isoSelectedSourceIds)
        pruneStaleZoomIsoIntent()   // last meeting's unslotted guests
        autoTakeEnabled = false // Ignore legacy saved auto-take intent in the first release.
        autoConfidenceThreshold = prefs.autoConfidenceThreshold
        autoHoldSeconds = prefs.autoHoldSeconds
        // Optional key: a prefs file written before this shipped decodes to nil
        // and keeps the Windows defaults (a non-optional key would throw
        // keyNotFound and reset the operator's ENTIRE prefs file).
        autoExtras = prefs.automation ?? AutomationExtrasState()
        lowerThirdName = prefs.lowerThirdName
        lowerThirdTitle = prefs.lowerThirdTitle
        lowerThirdPosition = LowerThirdPositions.migrate(prefs.lowerThirdPosition)
        if !prefs.logoBugAssetId.isEmpty {
            logoBug = mediaAssets.first { $0.id == prefs.logoBugAssetId }
        }
        streamUrl = prefs.streamUrl
        if let saved = prefs.streamDestinations { streamDestinations = saved }
        removedStarterSceneIds = (prefs.removedStarterSceneIds ?? []).filter { Self.starterSceneIds.contains($0) }
        recentMeetings = prefs.recentMeetings
        webinar = prefs.webinar
        if !prefs.scenes.isEmpty {
            scenes = prefs.scenes.filter { !Self.isSoloScene($0.id) }.map { saved in
                SceneDef(id: saved.id, name: saved.name, layout: saved.layout,
                         layers: saved.layers.map { layer in
                             SceneLayer(id: layer.id, slotId: layer.slotId,
                                        x: layer.x, y: layer.y,
                                        width: layer.width, height: layer.height,
                                        fitMode: layer.fitMode, opacity: layer.opacity,
                                        mediaAssetId: layer.mediaAssetId)
                         },
                         backgroundAssetId: saved.backgroundAssetId,
                         tiles: Self.restoredTiles(saved))
            }
        }
        cameraExtensionOptedIn = prefs.cameraExtensionOptedIn
        virtualCamera = prefs.virtualCamera ?? PersistedVirtualCamera()
        restoreProductionSettings(prefs)
        recordingFolderChoice = prefs.recordingFolder ?? ""
        lastSavedPrefs = prefs
        prefsRestored = true
    }

    /// True once start() has run its setup. start() is reached from the main
    /// window's .onAppear, and a second main-window view (File > New Window, or a
    /// Dock reopen after the main window was closed) would otherwise re-restore
    /// prefs over live state, add a second autosave timer and launch a second core
    /// while orphaning the first. Everything start() sets up happens once per process.
    private(set) var didStart = false
    /// Test seam: replaces start()'s setup body so the once-only gate can be
    /// exercised without launching a core or reading the operator's files.
    var startBodyForTesting: (() -> Void)?

    func start() {
        guard !didStart else {
            ShellLog.write("start: already started; ignoring repeat call from a new main-window view")
            return
        }
        didStart = true
        if let body = startBodyForTesting { body(); return }
        performStart()
    }

    private func performStart() {
        refreshMediaBin()
        restorePrefs()
        // Local control API (Companion / Stream Deck): 127.0.0.1:8011 by default.
        // Never in the TESTS/SELFCHECK harness; a bind failure is a warning.
        controlApi.start(model: self)
        // Camera extension (roadmap M9.2): activate after an earlier opt-in,
        // else only query. Proof shots stage their own state instead.
        if !Self.vcamProofRequested(ProcessInfo.processInfo.environment) {
            startCameraExtensionAtLaunch()
        }
        // Self-snapshot harness: render our own window to PNG (no TCC, no
        // focus games — AX/screen-recording automation proved unreliable) and
        // exit. Pairs with COREVIDEO_SHELL_TAB for per-tab proof shots.
        if let snapshotPath =
            ProcessInfo.processInfo.environment["COREVIDEO_SHELL_SNAPSHOT"] {
            // Proof-shot only: COREVIDEO_SHELL_FTB=black shows the transport in its
            // settled-black state.
            if ProcessInfo.processInfo.environment["COREVIDEO_SHELL_FTB"] == "black" {
                DispatchQueue.main.asyncAfter(deadline: .now() + 6) { [weak self] in
                    self?.applyProgramBlackout(["programBlackout": true, "programBlackoutOpacity": 1.0])
                    self?.blackoutProofHold = true
                }
            }
            // Proof-shot only: COREVIDEO_SHELL_WINDOW=WxH sizes the window for the
            // narrow/wide footer comparison.
            if let size = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_WINDOW"] {
                let parts = size.split(separator: "x").compactMap { Double($0) }
                if parts.count == 2 {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 5) {
                        NSApplication.shared.windows.first?.setContentSize(
                            NSSize(width: parts[0], height: parts[1]))
                    }
                }
            }
            // Proof-shot only: COREVIDEO_SHELL_QUITPANEL=1 raises the main-window-closed
            // quit panel and snapshots THAT panel at its real size (clipping check).
            let quitPanelProof = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_QUITPANEL"] == "1"
            if quitPanelProof {
                DispatchQueue.main.asyncAfter(deadline: .now() + 4) { [weak self] in
                    guard let self else { return }
                    self.quitPromptVisible = true
                    QuitPromptPanel.show(model: self)
                }
            }
            // Proof-shot only: COREVIDEO_SHELL_VCAM=off|live|mixed|needsApproval photographs
            // the virtual camera flyout in that state. It is staged on a SEPARATE,
            // never-started model, so nothing is sent, persisted or activated.
            var vcamProofView: NSView?
            if Self.vcamProofRequested(ProcessInfo.processInfo.environment),
               let vcamProof = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_VCAM"] {
                DispatchQueue.main.asyncAfter(deadline: .now() + 4) {
                    let proofModel = AppModel()
                    proofModel.stageVirtualCameraProof(vcamProof)
                    let host = NSHostingView(rootView: VirtualCameraFlyout().environmentObject(proofModel))
                    host.frame = NSRect(origin: .zero, size: host.fittingSize)
                    let window = NSWindow(contentRect: host.frame, styleMask: [.borderless],
                                          backing: .buffered, defer: false)
                    window.contentView = host
                    window.orderFrontRegardless()
                    vcamProofView = host
                    Self.vcamProofRetain = (proofModel, window)
                }
            }
            DispatchQueue.main.asyncAfter(deadline: .now() + 8) {
                let target: NSView? = vcamProofView ?? (quitPanelProof
                    ? QuitPromptPanel.proofView
                    : NSApplication.shared.windows.first?.contentView)
                if let view = target,
                   let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
                    view.cacheDisplay(in: view.bounds, to: bitmap)
                    if let data = bitmap.representation(using: .png, properties: [:]) {
                        try? data.write(to: URL(fileURLWithPath: snapshotPath))
                    }
                }
                NSApplication.shared.terminate(nil)
            }
        }
        // Screenshot/verification harness: open on a named tab. Harness runs
        // skip the Keychain read — every ad-hoc rebuild is a new signing
        // identity and the access prompt would nag the operator.
        if let tabName = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_TAB"],
           let tab = StudioTab(name: tabName) {
            selectedTab = tab
        } else {
            refreshZoomSignedIn()
        }  // after the bin so the logo bug can re-resolve
        // Bind weak->strong BEFORE the Task, not inside it: `[weak self]` makes
        // `self` a mutable optional in the closure body, and a Task capturing
        // that var is "reference to captured var 'self' in concurrently-executing
        // code" (an error under strict concurrency). The guard makes it a `let`.
        Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in
                let prefs = self.currentPrefs()
                if prefs != self.lastSavedPrefs {
                    prefs.save()
                    self.lastSavedPrefs = prefs
                }
            }
        }
        let paths = ShellPaths.resolveForLaunch()
        guard let corePath = paths.corePath else {
            status = .failed("corevideo-native not found — run scripts/run-mac-shell.sh")
            return
        }
        let env = CoreLaunchEnvironment.extras(
            paths: paths, processEnvironment: ProcessInfo.processInfo.environment,
            zoomPublicAppKey: UserDefaults.standard.string(forKey: "zoomPublicAppKey"))
        let bridge = MediaCoreBridge(corePath: corePath, environmentExtras: env)
        self.bridge = bridge
        bridge.onStatus = { [weak self] status in
            Task { @MainActor in self?.handleBridgeStatus(status) }
        }
        bridge.onEvent = { [weak self] event in
            guard let self else { return }
            Task { @MainActor in self.handleEvent(event) }
        }
        bridge.onStderrLine = { line in
            // Core stderr is the diagnostic firehose. LaunchServices launches
            // have NO stderr, so it also lands in a file — the only way to
            // diagnose engine/join failures from a Finder-launched session.
            FileHandle.standardError.write(Data((line + "\n").utf8))
            CoreLog.write(line)
        }
        bridge.start()
    }

    /// Every bridge status change, including each new core generation
    /// (`.connected` after a launch or a respawn).
    func handleBridgeStatus(_ status: BridgeStatus) {
        self.status = status
        if case .connected(let renderer) = status {
            ShellLog.write("core connected renderer=\(renderer)")
            self.onConnected()
        }
        if case .exited(let code) = status {
            self.monitorControl.resetForProcess()
            self.monitorControlNotice = "Monitor edit reconciling with restarted core"
            self.audioRouteControl.resetForProcess()
            self.audioRouteControlNotice = "Audio route reconciling with restarted core"
            self.rosterProjection.resetForProcess()
            self.sourceFacts.mutate { $0.engineStopped() }
            self.rosterRecoveryNotice = "Zoom roster reconciling with restarted core"
            self.recordingCommands.interrupted()
            self.recordingDesired = false
            self.streamingDesired = false
            self.recordingStatus = "interrupted"
            self.streamStatus = "interrupted"
            self.streamDetail = "Media core exited; output continuity was interrupted."
            self.destinationRows = [:]
            self.ndiSenders = []          // a stopped core reads Starting, never a stale Live
            self.warnedUnguardedChild = false
            self.streamChildGuard = nil
            self.pushWarning("media core exited (code \(code)) — relaunching")
        }
    }

    /// Test-only: stops the poll timers onConnected() schedules.
    func stopTimersForTesting() {
        syncTimer?.invalidate(); syncTimer = nil
        zoomTimer?.invalidate(); zoomTimer = nil
        cameraUpdateTimer?.invalidate(); cameraUpdateTimer = nil
        commsLeaseTimer?.invalidate(); commsLeaseTimer = nil
    }

    // The Studio center is ONE multiview canvas (reference 01-studio): the
    // core composites PREVIEW + PROGRAM tiles INSIDE the multiview texture in
    // the pgmPvw layout modes, plus the live input tiles below.
    @Published var multiviewLayoutMode = "pgmPvwTop"

    func configureMultiviewer(mode: String) {
        multiviewLayoutMode = mode
        applyMultiviewConfig()
    }

    /// Multiview overlays (Windows MultiviewShowLabels/Tally/Meters/Clock): restored
    /// from prefs before the first configure-multiviewer, carried on every one.
    @Published var multiviewOverlays = MultiviewOverlays()

    /// One overlay flag: persists and sends ONE configure-multiviewer when it changes;
    /// a repeat of the current value sends nothing.
    func setMultiviewOverlay(_ keyPath: WritableKeyPath<MultiviewOverlays, Bool>, _ on: Bool) {
        guard multiviewOverlays[keyPath: keyPath] != on else { return }
        multiviewOverlays[keyPath: keyPath] = on
        persistPrefsNow()
        applyMultiviewConfig()
    }

    /// Every core generation (first handshake AND a supervisor respawn) starts
    /// at its built-in 1920x1080@60 / grid defaults. Re-send the operator's
    /// production settings, or a respawned core silently renders the wrong
    /// canvas while Settings still shows the right one (gotchas.md: a one-shot
    /// command must be re-applied on every core generation).
    func reassertProductionSettings() {
        let body: JSONObject = [
            "type": "media-core-sync", "elapsedMs": elapsedMs(),
            "commands": productionSettingsCommands(),
        ]
        Task { _ = try? await self.outputRequest(body) }
    }

    // ── virtual camera: intent, wire, activation ─────────────────────────────

    var virtualCameraLabel: String {
        VirtualCameraPresentation.label(intent: virtualCamera.enabled, ext: cameraExtensionState,
                                        core: virtualCameraCore, updating: cameraUpdateInProgress)
    }

    /// Second line under the status while an update waits (flyout, Settings, Health).
    var virtualCameraUpdateNotice: String? {
        VirtualCameraPresentation.updateNotice(
            ext: cameraExtensionState, othersHolding: cameraUpdateOthersHolding,
            ownOutputOn: virtualCamera.enabled && !virtualCameraPausedForUpdate && !virtualCameraHeldForLaunch,
            updating: cameraUpdateInProgress)
    }

    var virtualCameraFramerLabel: String {
        VirtualCameraPresentation.framerLabel(framerEnabled: virtualCamera.framerEnabled,
                                              intent: virtualCamera.enabled, core: virtualCameraCore)
    }

    /// The footer toggle and (later) the control API. Enabling while the
    /// extension is not active asks the OS to install/approve it; the first
    /// enable records the opt-in so a relaunch re-activates silently.
    func setVirtualCamera(enabled: Bool) {
        var next = virtualCamera
        next.enabled = enabled
        if enabled {
            if cameraExtensionOptedIn != true { cameraExtensionOptedIn = true }
            // .updateReady: the installed copy works, the update waits;
            // .needsReboot: only a restart finishes it. Neither re-submits.
            if ![.active, .activating, .updateReady, .needsReboot].contains(cameraExtensionState) {
                activateCameraExtension()
            }
        }
        applyVirtualCamera(next)
        // Our own output was what held the update back: try it now.
        if !enabled { retryCameraUpdateIfFree() }
    }

    /// Settings "Remove virtual camera". Turns the intent off (one
    /// sync-virtual-camera {on:false}) and asks the OS to uninstall. The opt-in
    /// is cleared only once the uninstall succeeded (a cancelled password or a
    /// failure leaves the extension installed, so it must stay opted in) and a
    /// failure is reported in `virtualCameraRemovalStatus` because the flyout
    /// label reads "off" whenever the intent is off.
    func removeVirtualCamera() {
        cameraExtensionQueryToken = nil
        virtualCameraRemovalStatus = nil
        var next = virtualCamera
        next.enabled = false
        applyVirtualCamera(next)
        cameraExtension.deactivate { [weak self] state in
            Task { @MainActor in
                guard let self else { return }
                // A removal that needs a restart says so in
                // virtualCameraRemovalStatus; the state must not read as an
                // unfinished UPDATE ("Restart your Mac to finish updating").
                self.setCameraExtensionState(state == .needsReboot ? .notInstalled : state)
                switch state {
                case .notInstalled, .needsReboot:
                    self.cameraExtensionOptedIn = false
                    self.persistPrefsNow()
                    self.virtualCameraRemovalStatus = state == .needsReboot
                        ? "Restart your Mac to finish removing the virtual camera." : nil
                default:
                    ShellLog.write("vcam-ext: removal did not complete (\(state)); opt-in kept")
                    self.virtualCameraRemovalStatus = "Couldn't remove the virtual camera. It is still installed."
                }
            }
        }
    }

    @Published var virtualCameraRemovalStatus: String?

    func setVirtualCameraMirror(_ on: Bool) {
        var next = virtualCamera
        next.mirror = on
        applyVirtualCamera(next)
    }

    func setVirtualCameraFramer(_ on: Bool) {
        var next = virtualCamera
        next.framerEnabled = on
        applyVirtualCamera(next)
    }

    /// Committed on field submit or focus loss, never per keystroke. The
    /// operator's text is kept as typed (trimmed); the wire sanitizes it and a
    /// blank name sends the default.
    func setVirtualCameraDeviceName(_ name: String) {
        // An unchanged name (as the core would receive it) sends nothing.
        guard DeviceNamePolicy.sanitized(name) != DeviceNamePolicy.sanitized(virtualCamera.deviceName) else { return }
        var next = virtualCamera
        next.deviceName = name.trimmingCharacters(in: .whitespacesAndNewlines)
        applyVirtualCamera(next)
    }

    /// The ONE place a virtual camera field change lands: store, send, persist.
    private func applyVirtualCamera(_ next: PersistedVirtualCamera) {
        let changed = next != virtualCamera
        virtualCamera = next
        sendVirtualCameraSync()
        if changed { persistPrefsNow() }
    }

    /// One-shot command, so it is also re-sent on every core generation
    /// (onConnected): a respawned core forgets it and answers with its default.
    private func sendVirtualCameraSync() {
        var wire = virtualCamera
        if virtualCameraPausedForUpdate || virtualCameraHeldForLaunch { wire.enabled = false }
        let body = CoreCommands.mediaCoreSync(elapsedMs: Double(elapsedMs()),
                                              commands: [CoreCommands.syncVirtualCamera(wire)])
        if let capture = coreCommandCaptureForTesting { capture(body); return }
        guard let bridge else { return }
        Task {
            if let response = try? await bridge.request(body),
               let snapshot = response["snapshot"] as? JSONObject {
                applySnapshot(snapshot)
            }
        }
    }

    /// Saves now instead of waiting for the 5 s autosave, but never before the
    /// operator's file has been restored (that would write defaults over it).
    func persistPrefsNow() {
        guard prefsRestored else { return }
        let prefs = currentPrefs()
        guard prefs != lastSavedPrefs else { return }
        prefs.save()
        lastSavedPrefs = prefs
    }

    /// Launch: after an opt-in, ask macOS what is installed first. Our exact
    /// version already enabled submits NOTHING (an activation request for an
    /// installed extension is the step that can disturb a running one);
    /// otherwise activate (first install, a pending approval, an update).
    /// Without an opt-in, only ask.
    func startCameraExtensionAtLaunch() {
        if cameraExtensionOptedIn == true {
            if virtualCamera.enabled {
                virtualCameraHeldForLaunch = true
                let limit = cameraLaunchHoldLimit
                DispatchQueue.main.asyncAfter(deadline: .now() + limit) { [weak self] in
                    Task { @MainActor in
                        guard let self, self.virtualCameraHeldForLaunch else { return }
                        ShellLog.write("vcam-ext: launch decision took over \(limit) s; releasing the output")
                        self.releaseCameraLaunchHold()
                    }
                }
            }
            queryCameraExtensionInstalls { [weak self] installs in
                // One device reading decides the whole launch (registered? in use?).
                self?.readCameraDevice { [weak self] device in
                    guard let self else { return }
                    // Every path below that does not start a swap releases the
                    // hold now; a swap releases it on its result.
                    var startedSwap = false
                    defer { if !startedSwap { self.releaseCameraLaunchHold() } }
                    let action = CameraExtensionPolicy.launchAction(
                        installs: installs, bundledVersion: CameraExtensionPolicy.bundledVersion,
                        deviceRegistered: device.registered)
                    // A working installed copy is fine from anywhere; anything
                    // that would SUBMIT first passes the preflight (review I2b), so
                    // "Move to Applications" is never hidden behind "Update camera".
                    if action == .update || action == .activate, let blocked = self.cameraExtension.preflightState() {
                        ShellLog.write("vcam-ext: launch blocked by preflight: \(blocked)")
                        self.recordCameraExtensionResult("blocked at launch: \(blocked)")
                        self.setCameraExtensionState(blocked)
                        return
                    }
                    switch action {
                    case .needsReboot:
                        ShellLog.write("vcam-ext: version \(CameraExtensionPolicy.bundledVersion) is installed but not enabled, an older copy is uninstalling and no device is registered; restart pending, nothing submitted")
                        self.recordCameraExtensionResult("restart pending at launch (nothing submitted)")
                        self.setCameraExtensionState(.needsReboot)
                    case .alreadyActive:
                        ShellLog.write("vcam-ext: version \(CameraExtensionPolicy.bundledVersion) already active; nothing submitted")
                        self.recordCameraExtensionResult("already active at launch (nothing submitted)")
                        self.setCameraExtensionState(.active)
                    case .activate:
                        startedSwap = true
                        self.activateCameraExtension()
                    case .update:
                        if self.cameraInUse(given: device) {
                            // Our output is held (or off), so this is another app.
                            self.cameraUpdateOthersHolding = true
                            ShellLog.write("vcam-ext: update to \(CameraExtensionPolicy.bundledVersion) deferred; the camera is in use")
                            self.recordCameraExtensionResult("update deferred at launch (camera in use)")
                            self.setCameraExtensionState(.updateReady)
                        } else {
                            startedSwap = true
                            self.activateCameraExtension()
                        }
                    }
                }
            }
        } else {
            queryCameraExtension()
        }
    }

    /// "Is the camera in use?" over a NEW device reading and our own intent.
    /// The reading is made off the main actor; `done` runs on it. Whoever asks
    /// re-checks its own state in `done`: the answer arrives later.
    func checkCameraInUse(_ done: @escaping @MainActor (Bool) -> Void) {
        readCameraDevice { [weak self] device in
            guard let self else { return }
            done(self.cameraInUse(given: device))
        }
    }

    /// The same question answered in place, for the one caller that cannot
    /// wait: the delegate call in which macOS asks whether to replace the
    /// installed extension (CameraDeviceReader.readNow; a short bound, once
    /// per replacement offer). Never from a poll or a timer.
    func cameraInUseNow() -> Bool {
        cameraInUse(given: noteCameraDevice(cameraDeviceReader.readNow()))
    }

    /// CameraExtensionPolicy.inUse over a device reading and our own intent.
    func cameraInUse(given device: CameraDeviceStatus) -> Bool {
        // Our output off (or held off): a running device is someone else.
        if device.runningSomewhere && (!virtualCamera.enabled || virtualCameraHeldForLaunch) {
            cameraUpdateOthersHolding = true
        }
        return CameraExtensionPolicy.inUse(device: device, intentOn: virtualCamera.enabled,
                                           pausedForUpdate: virtualCameraPausedForUpdate || virtualCameraHeldForLaunch)
    }

    private func releaseCameraLaunchHold() {
        guard virtualCameraHeldForLaunch else { return }
        virtualCameraHeldForLaunch = false
        sendVirtualCameraSync()
    }

    private func recordCameraExtensionResult(_ what: String) {
        cameraExtensionLastResult = what + " (" + ISO8601DateFormatter().string(from: Date()) + ")"
    }

    /// Health tab / support bundle snapshot, over the LAST device reading
    /// (nil = never read). It never reads the device itself: a read blocks,
    /// and both callers are on the main actor (a view body, and the support
    /// bundle export). The Health tab asks for a new reading when it appears.
    func virtualCameraDiagnostics() -> VirtualCameraDiagnostics {
        let device = cameraDeviceStatus
        return VirtualCameraDiagnostics(
            label: virtualCameraLabel, updateNotice: virtualCameraUpdateNotice,
            state: cameraExtensionState, optedIn: cameraExtensionOptedIn,
            bundledVersion: CameraExtensionPolicy.bundledVersion, installs: cameraExtensionInstalls,
            device: device,
            inUse: device.map { CameraExtensionPolicy.inUse(device: $0, intentOn: virtualCamera.enabled,
                                                            pausedForUpdate: virtualCameraPausedForUpdate) },
            lastResult: cameraExtensionLastResult, pausedForUpdate: virtualCameraPausedForUpdate)
    }

    /// The Health tab asks macOS for the installed copies when it appears,
    /// unless another installed-copies question is already in flight (that one
    /// owns the token and its answer lands in `cameraExtensionInstalls` too).
    func refreshCameraExtensionDiagnostics() {
        readCameraDevice { _ in }
        guard cameraExtensionInstallsToken == nil else { return }
        queryCameraExtensionInstalls { _ in }
    }

    /// One read from a new process, made off the main actor (it is a
    /// spawn-and-wait of up to 2 s; CameraDeviceReader) and remembered for the
    /// Health tab and the support bundle. `done` runs on the main actor with a
    /// reading that started after this call.
    func readCameraDevice(_ done: @escaping @MainActor (CameraDeviceStatus) -> Void) {
        cameraDeviceReader.read { [weak self] status in
            guard let self else { return }
            done(self.noteCameraDevice(status))
        }
    }

    @discardableResult
    private func noteCameraDevice(_ status: CameraDeviceStatus) -> CameraDeviceStatus {
        if status != cameraDeviceStatus { cameraDeviceStatus = status }
        // Nobody on the camera right now: whoever held it has let go.
        if !status.runningSomewhere && cameraUpdateOthersHolding { cameraUpdateOthersHolding = false }
        return status
    }

    private func observeVirtualCameraLoss() {
        let lost = virtualCameraCore.map { core in
            core.status == "failed"
                && VirtualCameraPresentation.connectingPrefixes.contains { core.warning.hasPrefix($0) }
        } ?? false
        guard lost, virtualCamera.enabled, cameraExtensionState == .active else {
            virtualCameraLostSince = nil
            virtualCameraLostRechecked = false
            return
        }
        let since = virtualCameraLostSince ?? Date()
        virtualCameraLostSince = since
        guard !virtualCameraLostRechecked, Date().timeIntervalSince(since) >= cameraLostRecheckAfter else { return }
        virtualCameraLostRechecked = true
        let recheckAfter = cameraLostRecheckAfter
        readCameraDevice { [weak self] device in
            // Decided when the reading arrives: the loss may have ended, or
            // the state moved on, while the probe ran.
            guard let self, self.virtualCameraLostSince != nil, self.cameraExtensionState == .active,
                  !device.isOurs(CameraExtensionPolicy.bundledVersion) else { return }
            ShellLog.write("vcam-ext: the core has not reached the camera for \(recheckAfter) s; checking what is installed")
            self.queryCameraExtensionInstalls { [weak self] installs in
                guard let self, self.cameraExtensionState == .active else { return }
                if CameraExtensionPolicy.needsReboot(unregisteredWith: installs,
                                                     bundledVersion: CameraExtensionPolicy.bundledVersion) {
                    self.setCameraExtensionState(.needsReboot)
                }
            }
        }
    }

    /// Activation said done; prove it. Our device must be visible to a NEW
    /// process within `cameraRegistrationTimeout` (never to this one: its
    /// CoreMediaIO view predates the extension it just installed). If it never
    /// is, ask macOS what is installed: only OUR version reported as not
    /// enabled means a restart finishes it.
    private func verifyCameraRegistration() {
        let token = UUID()
        cameraRegistrationToken = token
        pollCameraRegistration(token: token, deadline: Date().addingTimeInterval(cameraRegistrationTimeout))
    }

    private func pollCameraRegistration(token: UUID, deadline: Date) {
        guard cameraRegistrationToken == token, cameraExtensionState == .active else { return }
        readCameraDevice { [weak self] device in
            self?.cameraRegistrationRead(device, token: token, deadline: deadline)
        }
    }

    /// The token is checked again here: a reading that comes back after a
    /// newer check started (or after the state left `.active`) decides nothing.
    private func cameraRegistrationRead(_ device: CameraDeviceStatus, token: UUID, deadline: Date) {
        guard cameraRegistrationToken == token, cameraExtensionState == .active else { return }
        // OUR version's device, not merely "a device": the old copy's device
        // has the same UID and lingers while its process exits (review I4).
        if device.isOurs(CameraExtensionPolicy.bundledVersion) {
            cameraRegistrationToken = nil
            ShellLog.write("vcam-ext: device registered (version \(CameraExtensionPolicy.bundledVersion))")
            return
        }
        if Date() < deadline {
            DispatchQueue.main.asyncAfter(deadline: .now() + cameraRegistrationPollInterval) { [weak self] in
                Task { @MainActor in self?.pollCameraRegistration(token: token, deadline: deadline) }
            }
            return
        }
        cameraRegistrationToken = nil
        ShellLog.write("vcam-ext: no device \(cameraRegistrationTimeout) s after activation; asking what is installed")
        queryCameraExtensionInstalls { [weak self] installs in
            guard let self, self.cameraExtensionState == .active else { return }
            if CameraExtensionPolicy.needsReboot(unregisteredWith: installs,
                                                 bundledVersion: CameraExtensionPolicy.bundledVersion) {
                self.setCameraExtensionState(.needsReboot)
            } else {
                ShellLog.write("vcam-ext: version \(CameraExtensionPolicy.bundledVersion) is enabled but a new process sees no device; not a restart (is the extension running?)")
                self.recordCameraExtensionResult("enabled, but no device visible to a new process")
            }
        }
    }

    /// The `.updateReady` retry: the 5 s timer, and our intent turning off.
    func retryCameraUpdateIfFree() {
        guard cameraExtensionState == .updateReady, !cameraUpdateInProgress, !cameraUpdateRetryChecking else { return }
        cameraUpdateRetryChecking = true
        checkCameraInUse { [weak self] inUse in
            guard let self else { return }
            self.cameraUpdateRetryChecking = false
            // Still the state this check was made for (an operator's Update
            // camera, or another result, may have landed meanwhile).
            guard self.cameraExtensionState == .updateReady, !self.cameraUpdateInProgress, !inUse else { return }
            ShellLog.write("vcam-ext: the camera is free; installing the update")
            self.activateCameraExtension()
        }
    }

    /// The "Update camera" button. The operator asked, so our OWN output is
    /// paused for the swap (the wire says off; the saved intent is untouched),
    /// then the shell waits up to `cameraUpdateReleaseWait` for the device to
    /// stop. Still running = another app has it: stay `.updateReady` and
    /// resume our output.
    func updateCameraExtension() {
        guard cameraExtensionState == .updateReady, !cameraUpdateInProgress else { return }
        cameraUpdateInProgress = true
        ShellLog.write("vcam-ext: Update camera clicked")
        if virtualCamera.enabled && !virtualCameraPausedForUpdate {
            virtualCameraPausedForUpdate = true
            sendVirtualCameraSync()
        }
        waitForCameraRelease(until: Date().addingTimeInterval(cameraUpdateReleaseWait))
    }

    private func waitForCameraRelease(until deadline: Date) {
        guard cameraUpdateStillWaitingForRelease() else { return }
        checkCameraInUse { [weak self] inUse in
            self?.cameraReleaseRead(inUse: inUse, deadline: deadline)
        }
    }

    /// False once this Update camera is no longer waiting on the device (and
    /// then it has been finished, unless the replacement owns the ending).
    private func cameraUpdateStillWaitingForRelease() -> Bool {
        guard cameraUpdateInProgress else { return false }
        guard cameraExtensionState == .updateReady else {
            // The replacement is in flight: its result finishes the update
            // (resuming our output now would put it back on the device that
            // is being swapped, and the delegate would refuse the swap).
            if cameraExtensionState == .activating { return false }
            finishCameraUpdate()
            return false
        }
        return true
    }

    /// Checked again here: the state may have moved while the probe ran.
    private func cameraReleaseRead(inUse: Bool, deadline: Date) {
        guard cameraUpdateStillWaitingForRelease() else { return }
        if !inUse {
            activateCameraExtension()
            return
        }
        if Date() >= deadline {
            cameraUpdateOthersHolding = true
            ShellLog.write("vcam-ext: another app still has the camera; the update stays ready")
            finishCameraUpdate()
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + cameraUpdatePollInterval) { [weak self] in
            Task { @MainActor in self?.waitForCameraRelease(until: deadline) }
        }
    }

    /// Ends an operator's Update camera: our output resumes, the button returns.
    private func finishCameraUpdate() {
        cameraUpdateInProgress = false
        resumeVirtualCameraAfterUpdate()
    }

    private func resumeVirtualCameraAfterUpdate() {
        guard virtualCameraPausedForUpdate else { return }
        virtualCameraPausedForUpdate = false
        sendVirtualCameraSync()
    }

    /// Runs the `.updateReady` retry while (and only while) that state holds.
    private func updateCameraUpdateTimer() {
        let wanted = cameraExtensionState == .updateReady
        if wanted, cameraUpdateTimer == nil {
            cameraUpdateTimer = Timer.scheduledTimer(withTimeInterval: cameraUpdateRetryInterval,
                                                     repeats: true) { [weak self] _ in
                Task { @MainActor in self?.retryCameraUpdateIfFree() }
            }
        } else if !wanted {
            cameraUpdateTimer?.invalidate()
            cameraUpdateTimer = nil
        }
    }

    /// Every installed copy of the extension, or nil when macOS gives no answer
    /// within `cameraExtensionQueryTimeout` (a properties request that ends
    /// with didFinish and no foundProperties never calls back).
    func queryCameraExtensionInstalls(_ done: @escaping @MainActor ([CameraExtensionInstall]?) -> Void) {
        let token = UUID()
        cameraExtensionInstallsToken = token
        cameraExtension.installs { [weak self] installs in
            Task { @MainActor in
                guard let self, self.cameraExtensionInstallsToken == token else { return }
                self.cameraExtensionInstallsToken = nil
                self.cameraExtensionInstalls = installs
                done(installs)
            }
        }
        let timeout = cameraExtensionQueryTimeout
        DispatchQueue.main.asyncAfter(deadline: .now() + timeout) { [weak self] in
            Task { @MainActor in
                guard let self, self.cameraExtensionInstallsToken == token else { return }
                self.cameraExtensionInstallsToken = nil
                ShellLog.write("vcam-ext: installed-copies query gave no answer in \(timeout) s")
                done(nil)
            }
        }
    }

    private func activateCameraExtension() {
        cameraExtensionQueryToken = nil  // an activation outranks any pending query
        // Re-checked at the moment macOS offers the replacement (main queue).
        let inUse: () -> Bool = { [weak self] in
            MainActor.assumeIsolated { self?.cameraInUseNow() ?? false }
        }
        cameraExtension.activate(deferringReplacementWhile: inUse) { [weak self] state in
            Task { @MainActor in
                guard let self else { return }
                self.setCameraExtensionState(state)
                // Any result but "still working" ends an Update camera (a
                // needsApproval included: staying paused gains nothing).
                if state != .activating {
                    self.recordCameraExtensionResult("activation: \(state)")
                    self.finishCameraUpdate()
                    self.releaseCameraLaunchHold()
                }
            }
        }
    }

    private func queryCameraExtension() {
        let token = UUID()
        cameraExtensionQueryToken = token
        cameraExtension.query { [weak self] state in
            Task { @MainActor in
                guard let self, self.cameraExtensionQueryToken == token else { return }
                self.cameraExtensionQueryToken = nil
                self.setCameraExtensionState(state)
            }
        }
        let timeout = cameraExtensionQueryTimeout
        DispatchQueue.main.asyncAfter(deadline: .now() + timeout) { [weak self] in
            Task { @MainActor in
                guard let self, self.cameraExtensionQueryToken == token else { return }
                self.cameraExtensionQueryToken = nil
                ShellLog.write("vcam-ext: properties query gave no answer in \(timeout) s; treating as notInstalled")
                self.setCameraExtensionState(.notInstalled)
            }
        }
    }

    private func setCameraExtensionState(_ state: CameraExtensionState) {
        guard state != cameraExtensionState else { return }
        ShellLog.write("vcam-ext: shell state \(cameraExtensionState) -> \(state)")
        cameraExtensionState = state
        if state != .updateReady { cameraUpdateOthersHolding = false }
        updateCameraUpdateTimer()
        if state == .active { verifyCameraRegistration() } else { cameraRegistrationToken = nil }
    }

    /// A virtual camera proof shot needs BOTH variables: COREVIDEO_SHELL_VCAM
    /// alone is inert (it neither stages a flyout nor skips the launch check).
    nonisolated static func vcamProofRequested(_ env: [String: String]) -> Bool {
        env["COREVIDEO_SHELL_VCAM"] != nil && env["COREVIDEO_SHELL_SNAPSHOT"] != nil
    }

    /// Proof-shot only (COREVIDEO_SHELL_VCAM): keeps the staged model and its window alive.
    static var vcamProofRetain: (AppModel, NSWindow)?

    /// Proof-shot only: stages a flyout state on a never-started model by
    /// assigning fields directly (no send, no persist, no activation).
    func stageVirtualCameraProof(_ name: String) {
        switch name {
        case "live":
            virtualCamera = PersistedVirtualCamera(enabled: true, mirror: true, framerEnabled: true,
                                                   deviceName: "")
            cameraExtensionState = .active
            var core = VirtualCameraReadModel()
            core.enabled = true; core.status = "live"; core.width = 1920; core.height = 1080
            core.framerState = "active"; core.framesPublished = 600
            virtualCameraCore = core
        case "mixed":  // framer ON next to mirror OFF in one frame
            virtualCamera = PersistedVirtualCamera(enabled: true, mirror: false, framerEnabled: true,
                                                   deviceName: "Booth Cam")
            cameraExtensionState = .active
            var core = VirtualCameraReadModel()
            core.enabled = true; core.status = "live"; core.width = 1920; core.height = 1080
            core.framerState = "active"
            virtualCameraCore = core
        case "needsApproval":
            virtualCamera = PersistedVirtualCamera(enabled: true)
            cameraExtensionState = .needsApproval
            virtualCameraCore = nil
        default:
            virtualCamera = PersistedVirtualCamera()
            cameraExtensionState = .notInstalled
            virtualCameraCore = nil
        }
    }

    func openCameraExtensionSettings() {
        let major = ProcessInfo.processInfo.operatingSystemVersion.majorVersion
        NSWorkspace.shared.open(CameraExtensionPolicy.settingsURL(osMajor: major))
    }

    /// Every core generation (initial handshake AND respawn). Internal: the
    /// harness drives it to prove the console is re-asserted.
    func onConnected() {
        tilesMeetingStateObserved = false   // a new core generation: the meeting state is unknown again
        // Comms: a new core generation has no held keys and no report yet
        // (every-core-generation rule). Private is re-sent only when the new
        // report disagrees, never blindly.
        comms.coreGenerationChanged()
        // Social review: decisions made against the old core do not carry over.
        if let review = graphics.socialReviewIfStarted { Task { @MainActor in await review.coreGenerationChanged() } }
        startCommsLeaseTimer()
        installCommsReleaseObservers()
        resetMasterMetersForProcess()
        monitorControl.resetForProcess()
        audioRouteControl.resetForProcess()
        rosterProjection.resetForProcess()
        sourceFacts.mutate { $0.engineStopped() }
        rosterRecoveryNotice = rosterProjection.notice
        // The audio inputs BEFORE the console reassert (review #2): a saved local
        // audio source resolves its device from this list, and an empty list on
        // the first connect of a launch left it unsent — silent, toggle on.
        refreshAudioInputs(push: false)
        if !audioInputsWatched {
            audioInputsWatched = true
            AudioInputs.watch { [weak self] in self?.refreshAudioInputs(push: true) }
        }
        // FIRST, before anything that can make a source audible (monitor, capture
        // mics, scenes with media): see reassertAudioConsole.
        reassertAudioConsole()
        // A fresh core opens the system default output until told otherwise:
        // re-list the devices and re-send the operator's monitor device on
        // EVERY core generation (gotchas: one-shot commands are lost on respawn).
        // After reassertAudioConsole: the refresh can call setMonitor itself.
        refreshMonitorOutputDevices()
        if !monitorDevicesWatched {
            monitorDevicesWatched = true
            MonitorOutputDevices.watch { [weak self] in self?.refreshMonitorOutputDevices() }
        }
        setMonitor(enabled: monitorEnabled, volume: monitorVolume)
        reassertProductionSettings()
        // A fresh core generation has no capture devices connected.
        captureReconnectPending.formUnion(
            slots.filter { $0.kind == "capture" }.map(\.sourceId))
        // Re-assert a restored grade: the core starts neutral every launch, so
        // a persisted grade that is never pushed silently does nothing.
        if !gradeIsNeutral { applyColorGrade() }
        // Per-source grades: forget every epoch/revision and re-assert on this generation.
        sourceGrades.coreGenerationChanged()
        // Graphics: a new core has none of the old one's instances (spec 5.3).
        graphics.coreGenerationChanged()
        // One-shot: the brand kit is lost on respawn (L18a, gotchas). Sent at
        // launch and on every generation, not only when the operator edits it.
        pushBrandKitNow()
        for editor in gradeEditors.values { editor.previewCoreReset() }   // the preview lease died with the core
        // S6: a fresh core holds no preview-demand lease and has no sheet yet.
        sourceSheet = nil
        sourcePreviewDemand.coreConnected()
        // One-shot: a respawned core forgets the virtual camera (CLAUDE.md).
        sendVirtualCameraSync()
        // N1b / review focus 1: NDI is persisted on, but a fresh core has no
        // output session. Re-arm it on EVERY generation (the core-exit handler
        // already cleared Record/Stream intent, so this sends what is still wanted).
        rearmNdiOutput()
        // A scene always sits on PROGRAM (the Windows shell starts on its
        // first scene) — the PGM tile composites from the first frame.
        if programSceneId.isEmpty {
            programSceneId = scenes[0].id
        }
        // Arm PREVIEW on a DIFFERENT scene than program: mirroring program
        // made the two monitors show the same picture and left Take disabled
        // (it no-ops when preview == program). Cue the next scene instead, so
        // the buses differ and Take is live from the first click.
        if previewSceneId.isEmpty {
            previewSceneId = scenes.first { $0.id != programSceneId }?.id ?? programSceneId
        }
        syncScenes()
        if let auto = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_AUTOJOIN"],
           !auto.isEmpty, joinMeetingId.isEmpty {
            joinMeetingId = auto
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
                self?.joinZoom()
            }
        }
        startedAt = Date()
        programOutputStarted = false
        syncTimer?.invalidate()
        // 2Hz, not 10Hz: every sync renders inside applyCommands (~100ms+ with
        // readbacks on the Metal path today), so a faster cadence outruns the
        // RPC thread and queues starve one-shot requests (zoom-join sat behind
        // an unbounded sync backlog). Status/meters are fine at 2Hz; the
        // program surface rides push events.
        // The meter-age policy allows exactly this period (AudioObservationPolicy).
        syncTimer = Timer.scheduledTimer(
            withTimeInterval: AudioObservationPolicy.snapshotPollInterval, repeats: true
        ) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in await self.syncTick() }
        }
        zoomTimer?.invalidate()
        zoomTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in await self.zoomTick() }
        }
    }

    func elapsedMs() -> Int {
        Int(Date().timeIntervalSince(startedAt) * 1000)
    }

    // ── ticks ────────────────────────────────────────────────────────────────

    private func syncTick() async {
        guard let bridge else { return }
        let commands: [JSONObject] = []
        do {
            let response = try await bridge.request([
                "type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": commands,
            ])
            applySnapshot(response["snapshot"] as? JSONObject ?? [:])
            statusDetail = ""  // a recovered core clears the transient error
        } catch {
            statusDetail = error.localizedDescription
        }
    }

    private var loggedZoomSnapshot = false

    private func zoomTick() async {
        guard let bridge else { return }
        guard let response = try? await bridge.request(["type": "zoom-snapshot"]),
              let snapshot = response["snapshot"] as? JSONObject
        else { return }
        if !loggedZoomSnapshot, meetingState != "idle" {
            loggedZoomSnapshot = true
            ShellLog.write("zoom-snapshot keys: \(snapshot.keys.sorted()) participants=\(String(describing: snapshot["participants"]).prefix(400))")
        }
        applyZoom(snapshot)
    }

    /// Footer health readouts, derived from the last core snapshot (StudioHealth.swift).
    var stalledFeedsLine: String? { StudioHealthReadout.stalledFeeds(lastSnapshot) }
    var streamDegradationLine: String? { StudioHealthReadout.streamDegradation(lastSnapshot) }

    func observeAudioObservation(_ snapshot: JSONObject, now: Date = Date()) {
        audioObservation = AudioObservation(snapshot)
        audioObservationReceivedAt = now
    }

    /// Words for the meters when the audio observation is aging/stale (nil = live).
    /// A new core generation has measured nothing yet. The snapshot parser holds
    /// the last value when a key is absent, so without this a respawned core
    /// showed the DEAD core's loudness, peak and levels (never fabricate a level).
    func resetMasterMetersForProcess() {
        masterLevel = 0; masterLevelLeft = 0; masterLevelRight = 0
        shortTermLufs = -120; integratedLufs = -120; momentaryLufs = -120; loudnessLufs = -120
        truePeakDbfs = -120; mixedFrameCount = 0
        limiterActive = false; masteringRideDb = 0
    }

    func meterWarning(now: Date = Date()) -> String? {
        AudioObservationPolicy.warning(audioObservation, receivedAt: audioObservationReceivedAt, now: now)
    }

    private func applySnapshot(_ snapshot: JSONObject) {
        snapshotSequence &+= 1
        // Keep the last NON-EMPTY snapshot: a failed/empty sync must not blank
        // the Health surface (an operator reads it hardest right when the
        // core is unhappy).
        if !snapshot.isEmpty {
            lastSnapshot = snapshot
            // GET /snapshot never serves a social card's chat (author, message).
            controlSnapshotBox.publish(SocialRedaction.scrub(snapshot: snapshot), at: Date())
            observeAudioObservation(snapshot)
        }
        if !snapshot.isEmpty { applyProgramBlackout(snapshot) }
        if !snapshot.isEmpty { applyMediaReadModels(snapshot) }
        if !snapshot.isEmpty { graphics.observe(snapshot: snapshot) }
        if !snapshot.isEmpty {
            virtualCameraCore = VirtualCameraReadModel.parse(snapshot)
        }
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        clockText = formatter.string(from: Date())
        if let devices = snapshot["captureDevices"] as? [JSONObject] {
            captureDevices = Self.parseCaptureDevices(devices)
            refreshSlotHealth()
            reconnectRestoredCaptures()
            reconcileCaptureAudioSyncOffsets(devices)   // Z2: the device copy, from what the core reports
        }
        applySourceFacts(snapshot)
        if let preview = snapshot["gradePreview"] as? JSONObject, let supported = preview["supported"] as? Bool,
           supported != gradePreviewSupported {
            gradePreviewSupported = supported
            for editor in gradeEditors.values { editor.setPreviewSupported(supported) }
        }
        if !snapshot.isEmpty {
            sourceGrades.observe(sourceGrades: snapshot["sourceGrades"] as? [JSONObject],
                                 sources: gradeSources, now: sourceGrades.now())
        }
        if let recording = snapshot["recording"] as? JSONObject {
            let nextStatus = RecordingLifecycleReadModel.status(recording)
            recordingCommands.observe(
                nextStatus, sessionId: RecordingLifecycleReadModel.sessionId(recording))
            recordingDesired = recordingCommands.desired
            if nextStatus == "recording", recordingStatus != "recording" {
                recordingStartedAt = Date()
            }
            recordingStatus = nextStatus
            recordingArtifactPath = recording["artifactPath"] as? String ?? recordingArtifactPath
            // FRAME DROPS. The core has always published these; nothing read
            // them, so a recording could be losing frames with no sign of it
            // anywhere in the operator's view. A dropped frame is a compromised
            // master — it belongs on the status strip, not buried in Health.
            recordingDroppedFrames =
                (recording["totalDroppedFrames"] as? NSNumber)?.intValue ?? recordingDroppedFrames
            recordingFramesWritten =
                (recording["totalFramesWritten"] as? NSNumber)?.intValue ?? recordingFramesWritten
            let warning = recording["warning"] as? String ?? ""
            if !warning.isEmpty, warning != recordingWarning {
                pushWarning("recording: \(warning)")
            }
            recordingWarning = warning
        }
        if let mix = snapshot["audioMixSession"] as? JSONObject {
            let monitorAccepted = monitorControl.observe(mix)
            monitorControlNotice = monitorControl.notice
            masterLevel = mix["masterLevel"] as? Int ?? masterLevel
            loudnessLufs = (mix["loudnessLufs"] as? NSNumber)?.doubleValue ?? loudnessLufs
            mixedFrameCount = (mix["mixedFrameCount"] as? NSNumber)?.intValue ?? mixedFrameCount
            limiterActive = mix["limiterActive"] as? Bool ?? limiterActive
            let sides = StudioFooterReadout.masterLevels(snapshot: snapshot, fallback: masterLevel)
            masterLevelLeft = sides.left
            masterLevelRight = sides.right
            if monitorAccepted {
                monitorStatus = mix["monitorStatus"] as? String ?? monitorStatus
                monitorFeedbackRisk = mix["monitorFeedbackRisk"] as? Bool ?? monitorFeedbackRisk
            }
            if let meter = mix["masterMeter"] as? JSONObject {
                shortTermLufs = (meter["shortTermLufs"] as? NSNumber)?.doubleValue ?? shortTermLufs
                integratedLufs = (meter["integratedLufs"] as? NSNumber)?.doubleValue ?? integratedLufs
                momentaryLufs = (meter["momentaryLufs"] as? NSNumber)?.doubleValue ?? momentaryLufs
                truePeakDbfs = (meter["truePeakDbfs"] as? NSNumber)?.doubleValue ?? truePeakDbfs
            }
            if let participants = mix["participants"] as? [JSONObject] {
                applyMixParticipants(participants)
            }
            masteringRideDb = (mix["masteringRideDb"] as? NSNumber)?.doubleValue ?? 0
        }
        if let matrix = snapshot["audioRoutingMatrix"] as? JSONObject {
            applyRoutingSnapshot(matrix)
        }
        if let pluginHost = snapshot["pluginHost"] as? JSONObject {
            applyPluginHostSnapshot(pluginHost)
        }
        if let preview = snapshot["programFramePreview"] as? JSONObject,
           let texture = preview["sharedTexture"] as? JSONObject {
            applySharedTexture(texture)
        }
        // Cold-start surfaces: the shared-texture EVENTS are structural-change
        // gated, so a shell that (re)connects after the last structural change
        // never hears them — the snapshot carries the current ids.
        if let program = snapshot["programSharedTexture"] as? JSONObject {
            applySharedTexture(program)
        }
        if let multiview = snapshot["multiviewSharedTexture"] as? JSONObject {
            if let texture = multiview["texture"] as? JSONObject,
               let id = texture["iosurfaceId"] as? NSNumber {
                multiviewSurfaceId = id.uint32Value
            }
            applyMultiviewTiles(multiview["tiles"] as? [JSONObject])
        }
        if let session = snapshot["outputSenderSession"] as? JSONObject {
            applyStreamSenders(session)
            applyNdiSenders(session)
        }
        applyNdiCapability(snapshot)
        if let auto = snapshot["autoProduction"] as? JSONObject {
            autoRuleId = auto["ruleId"] as? String ?? ""
            autoSceneId = auto["recommendedSceneId"] as? String ?? ""
            autoConfidence = (auto["confidence"] as? NSNumber)?.intValue ?? 0
            autoRationale = auto["rationale"] as? String ?? ""
        }
        if let overlay = snapshot["overlayState"] as? JSONObject {
            overlaysOnAir = overlay["onAirCount"] as? Int ?? overlaysOnAir
            if let overlays = overlay["overlays"] as? [JSONObject],
               let key = overlays.first(where: {
                   $0["overlayId"] as? String == "key:lower-third"
               }) {
                lowerThirdPhase = key["keyPhase"] as? String ?? lowerThirdPhase
                lowerThirdVisible = key["visible"] as? Bool ?? false
                if let name = key["sourceName"] as? String, !name.isEmpty { lowerThirdKeyedName = name }
            } else {
                lowerThirdPhase = "hidden"
                lowerThirdVisible = false
            }
        }
    }

    private func applyZoom(_ snapshot: JSONObject) {
        if rosterProjection.isRetiredOrOlder(snapshot) {
            _ = rosterProjection.observe(snapshot)
            return
        }
        if !snapshot.isEmpty { lastZoomSnapshot = snapshot }
        zoomJoin.observe(snapshot)
        rawMediaActive = snapshot["rawMediaActive"] as? Bool ?? rawMediaActive
        if let error = snapshot["lastError"] as? String, !error.isEmpty {
            pushWarning("zoom: \(error)")
        }
        let acceptRoster = rosterProjection.observe(snapshot)
        rosterRecoveryNotice = rosterProjection.notice
        if acceptRoster {
            // Observed only with the meetingState it vouches for (S2 review: a
            // rejected stale snapshot must not mark a state we did not adopt).
            if let state = snapshot["meetingState"] as? String, !state.isEmpty { tilesMeetingStateObserved = true }
            let previous = meetingState
            meetingState = snapshot["meetingState"] as? String ?? meetingState
            if meetingState == "idle", previous != "idle" { pruneStaleZoomIsoIntent() }
        }
        if acceptRoster, Self.meetingHasEnded(snapshot["meetingState"] as? String ?? "") {
            endMeetingAudioSession()
            sourceGrades.meetingEnded()
        }
        if acceptRoster, let participants = snapshot["participants"] as? [JSONObject] {
            // Wire shape (ZoomEngineRuntime::rawCaptureSnapshotLocked, same as
            // the WinUI shell consumes): userId/displayName/videoOn/muted/talking.
            let cameraOnBefore = Set(roster.filter(\.hasVideo).map(\.id))
            let presentBefore = Set(roster.map(\.id))
            roster = RosterParticipant.parse(participants, assignedIds: assignedIds)
            // THE LAW (ShowInputAssignmentLaw): free departed guests, fill only
            // newcomers. Gated like Windows (StudioViewModel.cs:11128): an
            // unrevisioned EMPTY roster is no roster fact and frees nothing.
            var lawChanged = false
            let revision = (snapshot["rosterRevision"] as? NSNumber)?.int64Value ?? 0
            if !participants.isEmpty || revision > 0 {
                let state = snapshot["meetingState"] as? String ?? meetingState
                let lawRoster = Self.isInMeeting(state) ? roster : []
                if lawRoster.isEmpty { productionRoles.clear() }
                lawChanged = ShowInputWriteScope.run("roster-sync") {
                    runAssignmentLaw { law, slots in law.syncFromMeeting(&slots, roster: lawRoster) }
                }
            }
            let rosterFact = Self.zoomRosterFact(
                epoch: rosterProjection.epoch,
                meetingState: snapshot["meetingState"] as? String ?? meetingState, roster: roster)
            sourceFacts.mutate { $0.applyRoster(rosterFact) }
            refreshSlotHealth()
            if lawChanged {
                ShellLog.write("roster: slot law changed the assignments")
                recomputeFromSlots()
            }
            // A camera turning ON must re-push the spine: the budget filters on
            // camera-on, so without this the participant never re-subscribes (the
            // Windows 2026-08-09 frozen-tile defect, mirrored).
            let cameraOnAfter = Set(roster.filter(\.hasVideo).map(\.id))
            // ...and so must any change of WHO is in the room: the audio source set
            // keeps camera-off sources and drops departed ones, and meeting-audio
            // exists only while the meeting has participants. A camera-off guest
            // returning to their slot must get their audio back.
            let presentAfter = Set(roster.map(\.id))
            if (cameraOnAfter != cameraOnBefore || presentAfter != presentBefore) && !lawChanged {
                syncSpine()
                // A wall's members ARE the camera-on roster: re-send the scene
                // graph when a wall is on either bus (Windows rebuilds the tiles
                // wire with every production sync). recomputeFromSlots already
                // syncs scenes when the slot law changed.
                if isTilesScene(programSceneId) || isTilesScene(previewSceneId) { syncScenes() }
            }
        }
    }

    /// THE LAW's memory: who was seen this meeting and which slots the operator
    /// emptied. The only writer of auto-assignment.
    private(set) var assignmentLaw = ShowInputAssignmentLaw()
    /// The auto-assign toggle ("Fill free inputs when participants join"), mirrored
    /// from `assignmentLaw.autoAssignEnabled` for the views.
    @Published private(set) var autoAssignInputsEnabled = true
    /// The Sources status line (a guest left, a stale picker choice, a device offline).
    @Published var sourcesStatus = ""
    /// The Sources table's UI state (selection, filter, columns). Never writes slots;
    /// the column choices are saved under ShellPrefs.sourcesWorkspace.
    @Published var sourcesWorkspace = SourcesWorkspaceState()
    /// D4: the Role control is built and stored but hidden until role-targeted routes exist.
    static let roleControlAvailable = false
    /// Meeting-scoped production roles (one holder per role); cleared on an empty roster.
    private(set) var productionRoles = ProductionRoles()

    func setParticipantRole(_ participantId: String, role: String?) {
        if let line = productionRoles.set(participantId, role: role, present: roster) {
            sourcesStatus = line
            syncSpine()
        }
    }

    static func isInMeeting(_ state: String) -> Bool {
        state == "in_meeting" || state == "in-meeting"
    }

    /// Runs one law pass over a copy of the slots and writes them back once.
    /// Returns true when any assignment field (kind, sourceId, inShow) changed;
    /// the caller then calls `recomputeFromSlots()` exactly once.
    private func runAssignmentLaw(
        _ pass: (inout ShowInputAssignmentLaw, inout [ShowInputSlot]) -> [ShowInputAssignmentLaw.Freed]
    ) -> Bool {
        var next = slots
        let freed = pass(&assignmentLaw, &next)
        let changed = ShowInputAssignmentLaw.assignmentsDiffer(slots, next)
        if next != slots { slots = next }
        if let last = freed.last {
            sourcesStatus = "\(last.name) left the meeting — input \(last.slotId) is free."
        }
        return changed
    }

    /// A1 (Windows SetZoomAudioMode): the mode, the status sentence, persisted
    /// intent, and ONE full-state routing push. The flip is one matrix, so there
    /// is never a window with both paths routed or neither. Subscriptions do not
    /// change: stems stay subscribed in ProgramMix (they meter, and ISO recording
    /// takes stems from source PCM, not from buses).
    func setZoomAudioMode(_ mode: ZoomAudioMode) {
        guard mode != zoomAudioMode else { return }
        zoomAudioMode = mode
        takeStatus = mode.statusSentence
        persistPrefsNow()
        pushRoutingBase()
    }

    /// The auto-assign toggle. Either direction forgets both memories and reruns the
    /// roster pass, which fills only when on (D7; Windows MagicSceneCoordinator.cs:509-514).
    func setAutoAssignInputs(_ on: Bool) {
        guard on != assignmentLaw.autoAssignEnabled else { return }
        assignmentLaw.autoAssignEnabled = on
        autoAssignInputsEnabled = on
        let current = Self.isInMeeting(meetingState) ? roster : []
        let changed = ShowInputWriteScope.run("auto-assign-toggle") {
            runAssignmentLaw { law, slots in law.reapply(&slots, roster: current) }
        }
        if changed { recomputeFromSlots() }
        persistPrefsNow()
    }

    /// Capture device ids whose slot was restored (or survived a core respawn)
    /// and still need a connect once the core lists them.
    var captureReconnectPending: Set<String> = []

    // ── show-input slots (inputs 1–10, the Sources patch bay) ────────────────

    @Published var slots: [ShowInputSlot] = (1...10).map { ShowInputSlot(id: $0) } {
        didSet { slotsDidChange(from: oldValue) }
    }
    /// TEST-ONLY: receives every slot-write line. Set only by ShellTests.
    var slotWriteObserverForTesting: ((String) -> Void)?
    /// Observed row facts, one box per slot. NOT @Published (frame-fact rate).
    let sourceFacts = SourceFactsStore()
    /// Comms (Zoom talkback): the tab's model, fed by zoom-comms-state facts
    /// and the meeting state; it sends through the core like every request.
    lazy var comms: CommsModel = makeCommsModel()
    private var commsLeaseTimer: Timer?
    var commsLeaseTimerIntervalForTesting: TimeInterval? { commsLeaseTimer?.timeInterval }

    private func makeCommsModel() -> CommsModel {
        let model = CommsModel(send: { [weak self] verb, payload in
            guard let self else { return "The media core is not running." }
            return await self.sendComms(verb, payload)
        })
        model.onPrivatePreferenceChanged = { [weak self] _ in self?.persistPrefsNow() }
        return model
    }

    /// One comms-* request. nil = accepted; otherwise the core's refusal
    /// message verbatim (Windows shows it and never branches on the code).
    private func sendComms(_ verb: String, _ payload: JSONObject) async -> String? {
        do {
            _ = try await gradeRequest(["type": verb, "payload": payload])
            return nil
        } catch BridgeError.remote(let message) {
            return message
        } catch {
            return error.localizedDescription
        }
    }

    /// The meeting phase and its label reach the Comms model on every change.
    private func feedCommsMeeting() {
        comms.applyMeeting(Self.isInMeeting(meetingState),
                           label: CommsModel.meetingLabel(for: joinMeetingId))
    }

    /// The 500 ms lease renewal (Windows StudioViewModel.Comms.cs:36-43):
    /// always scheduled, sends only while a key is held.
    func commsLeaseTick() async {
        guard comms.anyHeld else { return }
        await comms.renewLeases()
    }

    // ── Comms release triggers and panel keys (plan Task 10) ────────────────
    let commsKeyMonitor = CommsKeyMonitor()
    private var commsKeys = CommsKeys()
    private var commsReleaseObservers: [NSObjectProtocol] = []

    /// Every held key up, and the key set forgotten. Each trigger calls this:
    /// leaving the tab, the app resigning active, the main window resigning key.
    func releaseCommsKeys() {
        commsKeys.reset()
        guard comms.anyHeld else { return }
        Task { await comms.releaseAll() }
    }

    /// Installed while the Comms tab is selected.
    func syncCommsKeys() {
        commsKeyMonitor.setActive(selectedTab == .comms) { [weak self] event in
            guard let self else { return event }
            let key = CommsKeys.event(from: event, isCommsTab: self.selectedTab == .comms,
                                      deskVisible: self.comms.deskVisible,
                                      firstResponder: event.window?.firstResponder)
            return self.handleCommsKey(key) ? nil : event
        }
    }

    /// Routes one key; true when consumed.
    @discardableResult
    func handleCommsKey(_ key: CommsKeyEvent) -> Bool {
        let decision = commsKeys.decide(key) { [comms] digit in comms.cell(forDigit: digit)?.slot }
        switch decision {
        case .ignore: return false
        case .swallow: return true
        case .allPress: Task { await comms.allPress() }
        case .allRelease: Task { await comms.allRelease() }
        case .press(let slot): Task { await comms.press(slot: slot) }
        case .release(let slot): Task { await comms.release(slot: slot) }
        }
        return true
    }

    /// The app or the main window losing focus releases every held key
    /// (Windows MainWindow.xaml.cs:675-676). Installed once.
    func installCommsReleaseObservers() {
        guard commsReleaseObservers.isEmpty else { return }
        let center = NotificationCenter.default
        commsReleaseObservers.append(center.addObserver(
            forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.releaseCommsKeys() }
        })
        commsReleaseObservers.append(center.addObserver(
            forName: NSWindow.didResignKeyNotification, object: nil, queue: .main) { [weak self] note in
            MainActor.assumeIsolated {
                guard MainWindowTracker.isMain(note.object as? NSWindow) else { return }
                self?.releaseCommsKeys()
            }
        })
    }

    /// Quit: every held key up in one line, bounded so a hung core never
    /// delays quitting (Windows ShutdownComms runs first). 350 ms.
    static let commsQuitReleaseBudget: TimeInterval = 0.35
    func sendCommsReleaseAllBounded() async {
        // An unstructured race, not a task group: a group waits for EVERY child
        // at scope exit, so a hung core would hold the quit despite the bound.
        let comms = self.comms
        final class Once { var done = false }
        let once = Once()
        let social = graphics.socialReviewIfStarted
        await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
            func finish() { if !once.done { once.done = true; cont.resume() } }
            Task { @MainActor in
                // Comms keys first, then social review (retires a card) in the same bound.
                if comms.inMeeting { await comms.sendReleaseAll() }
                await social?.stopForShutdown()
                finish()
            }
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: UInt64(Self.commsQuitReleaseBudget * 1e9))
                finish()
            }
        }
    }

    private func startCommsLeaseTimer() {
        guard commsLeaseTimer == nil else { return }
        commsLeaseTimer = Timer.scheduledTimer(withTimeInterval: Double(CommsModel.renewEveryMs) / 1000, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.commsLeaseTick() }
        }
    }
    /// S6: the core's source sheet (surface + cell identities). Written only on change; the
    /// surface id is stable and cells change only on reassignment.
    @Published private(set) var sourceSheet: SourceSheetState?
    private(set) lazy var sourcePreviewDemand = SourcePreviewDemandLease { [weak self] enabled in
        self?.sendSourcePreviewDemand(enabled)
    }
    private var sourcePreviewDemandTimer: Timer?
    func setSourceSheet(_ sheet: SourceSheetState?) { sourceSheet = sheet }
    /// TEST-ONLY clock (ms) for the measured capture FPS.
    var sourceFactsClockForTesting: (() -> Double)?
    var sourceFactsNowMs: Double { sourceFactsClockForTesting?() ?? ProcessInfo.processInfo.systemUptime * 1000 }

    /// The slot choke point (Windows ShowInputModels.cs:165-190). Logs each
    /// assignment-field change with the ambient ShowInputWriteScope reason, then
    /// saves at once: the save used to ride only the 5 s autosave, so an operator
    /// change followed by a crash was lost (ShowInputsCoordinator.cs:206-213).
    /// Never writes `slots`.
    private func slotsDidChange(from oldValue: [ShowInputSlot]) {
        guard oldValue != slots else { return }
        sourceFacts.mutate { $0.setSlots(slots.map(SourceSlotRef.init)) }
        for line in ShowInputWriteScope.lines(old: oldValue, new: slots,
                                              reason: ShowInputWriteScope.reason) {
            ShellLog.write(line)
            slotWriteObserverForTesting?(line)
        }
        persistPrefsNow()
    }

    // assignedIds stays the wire-facing derivation (spine/scenes/multiview all
    // read it) — recomputed whenever slots change.
    // Every in-show slot with a live source, zoom OR capture — the readiness
    // strip and multiviewer header report this, not just the zoom subset.
    var liveInputCount: Int {
        slots.filter { $0.kind != "unassigned" && $0.inShow && !$0.offline }.count
    }

    func recomputeFromSlots() {
        assignedIds = Set(slots.filter { $0.kind == "zoom" && $0.inShow }
            .map(\.sourceId))
        // Project the selection onto the (possibly new) slot set. This used to
        // REBUILD the selection from the slot flags, and a freshly assigned slot
        // has iso=false — so the first assignment after a relaunch wiped the
        // restored ISO selection (sources audit row 76).
        projectIsoSelection()
        syncSpine()
        syncScenes()
    }

    /// The row picker's assign: refused when the slot changed while its menu was
    /// open (`openedFor` = the slot's sourceKey when the menu was built; nil = it
    /// was unassigned). Audit row 12.
    func assignSlot(_ slotId: Int, kind: String, sourceId: String, name: String, openedFor: String?) {
        guard let slot = slots.first(where: { $0.id == slotId }) else { return }
        guard slot.sourceKey == openedFor else {
            sourcesStatus = "Input \(slotId) changed while its menu was open. Choose again."
            return
        }
        assignSlot(slotId, kind: kind, sourceId: sourceId, name: name)
    }

    func assignSlot(_ slotId: Int, kind: String, sourceId: String, name: String) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }) else { return }
        let named = SlotNamePolicy.afterAssign(slots[index], kind: kind, sourceId: sourceId,
                                               sourceName: name)
        var slot = slots[index]
        slot.kind = kind
        slot.sourceId = sourceId
        slot.name = named.name
        slot.sourceName = named.sourceName
        slot.inShow = true
        slot.offline = false
        ShowInputWriteScope.run("operator-assign") { slots[index] = slot }
        recomputeFromSlots()
    }

    func unassignSlot(_ slotId: Int) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }) else { return }
        // Reserved against auto-assign until the roster empties, whatever its kind
        // (ShowInputsCoordinator.cs:517).
        assignmentLaw.operatorUnassigned(slotId: slotId)
        ShowInputWriteScope.run("operator-unassign") { slots[index] = ShowInputSlot(id: slotId) }
        recomputeFromSlots()
    }

    func toggleSlotInShow(_ slotId: Int) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }),
              slots[index].kind != "unassigned" else { return }
        // The slot still holds its guest, so auto-assign never touches it: no
        // reservation, nothing freed.
        ShowInputWriteScope.run("operator-in-show") { slots[index].inShow.toggle() }
        recomputeFromSlots()
    }

    func toggleSlotIso(_ slotId: Int) {
        guard let slot = slots.first(where: { $0.id == slotId }),
              let key = slot.sourceKey else { return }
        recordingIso.noteOperatorEdit()
        if isoSelectedSourceIds.contains(key) {
            isoSelectedSourceIds.remove(key)
        } else {
            isoSelectedSourceIds.insert(key)
        }
        isoRecordingEnabled = !visibleIsoSelection.isEmpty
        recomputeFromSlots()
    }

    // Keep slot health truthful as rosters/devices churn.
    func refreshSlotHealthForTesting() { refreshSlotHealth() }

    private func refreshSlotHealth() {
        for index in slots.indices {
            switch slots[index].kind {
            case "zoom":
                let live = roster.first { $0.id == slots[index].sourceId }
                slots[index].offline = live == nil
                adoptLiveSourceName(index, live?.name)
            case "capture":
                slots[index].offline = !captureDevices.contains {
                    $0.id == slots[index].sourceId && $0.connectionState == "connected"
                }
                adoptLiveSourceName(index, captureDevices.first { $0.id == slots[index].sourceId }?.name)
            case "media":
                // Never substituted: an asset that left the bin marks the slot offline.
                let asset = mediaAssets.first { $0.id == slots[index].sourceId }
                if slots[index].offline != (asset == nil) { slots[index].offline = asset == nil }
                adoptLiveSourceName(index, asset?.name)
            default:
                break
            }
        }
    }

    private func adoptLiveSourceName(_ index: Int, _ liveName: String?) {
        let named = SlotNamePolicy.withLiveName(name: slots[index].name,
                                                sourceName: slots[index].sourceName,
                                                liveName: liveName ?? "")
        if slots[index].name != named.name { slots[index].name = named.name }
        if slots[index].sourceName != named.sourceName { slots[index].sourceName = named.sourceName }
    }

    private func firstEmptySlotIndex() -> Int? {
        slots.firstIndex { $0.kind == "unassigned" }
    }

    // The Capture button (Windows "Engine On"): ON → spine startCapture arms
    // raw media (this is when Zoom's record-privilege request fires); OFF →
    // zoom-stop-capture stops raw media in the ENGINE (clears the recording
    // indicator for participants) plus the spine flag drops.
    @Published var captureEnabled = false

    func setCapture(enabled: Bool) {
        captureEnabled = enabled
        if !enabled {
            sourceFacts.mutate { $0.engineStopped() }
            tilesAudioLatch.clear()   // Windows Engine off forgets every sticky member
        }
        syncSpine()
        if !enabled {
            guard let bridge else { return }
            Task { _ = try? await bridge.request(["type": "zoom-stop-capture"]) }
        }
    }

    private func handleEvent(_ event: JSONObject) {
        switch event["type"] as? String {
        case "program-shared-texture":
            applySharedTexture(event)
        case "program-frame-preview":
            if let texture = event["sharedTexture"] as? JSONObject {
                applySharedTexture(texture)
            }
        case "multiview-shared-texture":
            if let texture = event["texture"] as? JSONObject,
               let id = texture["iosurfaceId"] as? NSNumber {
                multiviewSurfaceId = id.uint32Value
            }
            applyMultiviewTiles(event["tiles"] as? [JSONObject])
        case "preview-shared-texture":
            if let id = event["iosurfaceId"] as? NSNumber {
                previewSurfaceId = id.uint32Value
            }
        case "grade-preview":
            // Phase 2 Task 8: route to the editor that owns the instance (spec §5.5).
            let instance = event["instanceId"] as? String
            gradeEditors.values.first { $0.previewInstanceId == instance }?.observePreview(event)
        case "zoom-comms-state":
            // Comms: the core relays the engine's talkback report (or null
            // when the report drops). A response with an id is never a fact.
            if let fact = ZoomCommsProtocol.parseFact(event) { comms.apply(fact) }
        case "zoom-source-format":
            // One row's facts; no sync and no AppModel @Published write (spec §4.4).
            if let fact = ZoomFormatFact.parse(event: event) {
                sourceFacts.mutate { $0.applyFormat(fact) }
            }
        default:
            break
        }
    }

    private func applyMultiviewTiles(_ tiles: [JSONObject]?) {
        guard let tiles else { return }
        multiviewTiles = tiles.enumerated().map { index, entry in
            let role = entry["role"] as? String ?? "source"
            let sourceId = entry["sourceId"] as? String ?? ""
            return MultiviewTile(
                id: sourceId.isEmpty ? "\(role)-\(index)" : sourceId,
                label: entry["label"] as? String ?? "",
                role: role,
                tally: entry["tally"] as? String ?? "none",
                activeSpeaker: entry["activeSpeaker"] as? Bool ?? false,
                x: (entry["x"] as? NSNumber)?.doubleValue ?? 0,
                y: (entry["y"] as? NSNumber)?.doubleValue ?? 0,
                w: (entry["w"] as? NSNumber)?.doubleValue ?? 0,
                h: (entry["h"] as? NSNumber)?.doubleValue ?? 0)
        }
    }

    // ── canvas / output profile ──────────────────────────────────────────────

    @Published var canvasWidth = 1920
    @Published var canvasHeight = 1080
    @Published var canvasFps = 60
    @Published var streamBitrateMbps = 6.0
    @Published var overlays = OverlaysState()
    @Published var audioInputs: [AudioInputDevice] = []
    /// A5: the local audio source (LocalAudioSource.swift). Set via setLocalAudio.
    @Published var localAudio = LocalAudioSetting()
    /// Harness seam: the audio input list (the real one enumerates AVCaptureDevice).
    var audioInputsProvider: () -> [AudioInputDevice] = { AudioInputs.available() }
    private var audioInputsWatched = false
    /// Z2: per capture device id, session only (CaptureAudioSync.swift). Absent = 0.
    @Published var captureAudioSyncOffsets: [String: Int] = [:]
    var captureSyncInFlight: Set<String> = []
    var captureAudioSyncTask: Task<Void, Never>?
    var brandSyncTask: Task<Void, Never>?
    /// Test seam: sees every overlay command before it goes to the bridge.
    var overlayCommandObserver: ((JSONObject) -> Void)?
    /// Retire bookkeeping for hideLowerThird (see cancelPendingLowerThirdRetire).
    var lowerThirdRetireGeneration = 0
    var lowerThirdRetireInFlight = false
    /// Test seam: shortens the build-out settle delay.
    var lowerThirdSettleOverrideNs: UInt64?
    /// How many keyed graphics are on air (the WinUI "N on air" readout).
    /// Counted from what the core DRAWS (overlayState visible), not the phase
    /// the shell requested: a key the on-Program gate suppresses is not on air.
    var lowerThirdOnAirCount: Int {
        lowerThirdVisible && (lowerThirdPhase == "on-air" || lowerThirdPhase == "building-in") ? 1 : 0
    }

    @Published var streamCodec = "h264"
    /// O1 (OutputProfiles.swift): the recording render profile and the stream
    /// encoder settings, persisted and sent on every start.
    @Published var recordingProfile = RecordingProfile()
    @Published var streamEncoder = StreamEncoderSettings()
    /// N1b (NdiOutput.swift): NDI output, persisted; independent of Record and Stream.
    @Published var ndiOutput = NdiOutputSettings()
    /// Why the last NDI-on was refused or NDI was turned off (Windows NdiOutputStatusReason).
    @Published var ndiRefusalReason: String?
    /// This core's `ndi` sender rows (destination "ndi"); [] when no core runs.
    @Published var ndiSenders: [JSONObject] = []
    /// profile.capabilityStates["ndi-output"] == available; nil before a core answered.
    @Published var coreHasNdiOutput: Bool?

    /// Codecs this build can actually ENCODE, per the core's EncoderPolicy:
    /// HEVC encode is not shipped on any platform, and AV1 is non-Apple only, so
    /// macOS is H.264 via VideoToolbox and nothing else. Offering an unsupported
    /// codec is a control that silently does nothing — the operator picks it,
    /// the core refuses it, and the stream is not what the UI claims.
    static let supportedStreamCodecs: [(id: String, label: String)] = [
        ("h264", "H.264 (VideoToolbox)"),
    ]
    @Published var recordFormat = "mp4"
    /// The operator's recording folder ("" = default). See RecordingFolder.swift.
    @Published var recordingFolderChoice = ""
    /// Why the chosen folder was refused or not used ("" = fine).
    @Published var recordingFolderWarning = ""
    @Published var recordPrefix = "show"
    @Published var multiviewTileCount = 10  // ATEM-style 10-way wall

    // PROGRAM COLOR GRADE. The core has always accepted `set-color-grade`
    // (exposure/contrast/saturation/temperature, each clamped) and the Metal
    // shader already consumes the constants per layer — the macOS shell simply
    // never exposed it, so a Mac operator had no picture control at all while
    // the Windows shell ships a whole ColorGradeEditorWindow.
    @Published var gradeExposure: Double = 0
    @Published var gradeContrast: Double = 0
    @Published var gradeSaturation: Double = 0
    @Published var gradeTemperature: Double = 0

    var gradeIsNeutral: Bool {
        gradeExposure == 0 && gradeContrast == 0 && gradeSaturation == 0 && gradeTemperature == 0
    }

    private var colorGradeSyncTask: Task<Void, Never>?

    // COALESCED. A slider drag emits a change per frame, and one request per
    // delta is exactly what starved the RPC queue during canvas drags until
    // every command timed out. Grading still feels live: 60ms is well under the
    // threshold where an operator riding a slider notices lag.
    func applyColorGrade() {
        colorGradeSyncTask?.cancel()
        colorGradeSyncTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 60_000_000)
            guard !Task.isCancelled else { return }
            await self?.pushColorGrade()
        }
    }

    // Counts pushes that actually made it PAST the debounce. Exists so the
    // coalescing can be asserted: a slider emits a change per frame, and one
    // request per delta is what starved the command queue until every request
    // timed out. Incremented before the bridge guard so the behaviour is
    // observable without a running core.
    private(set) var colorGradePushCount = 0

    @MainActor
    private func pushColorGrade() async {
        colorGradePushCount += 1
        guard let bridge else { return }
        _ = try? await bridge.request([
            "type": "media-core-sync", "elapsedMs": elapsedMs(),
            "commands": [[
                "type": "set-color-grade",
                "exposure": gradeExposure, "contrast": gradeContrast,
                "saturation": gradeSaturation, "temperature": gradeTemperature,
            ]],
        ])
    }

    func resetColorGrade() {
        gradeExposure = 0; gradeContrast = 0; gradeSaturation = 0; gradeTemperature = 0
        applyColorGrade()
    }

    func applyOutputProfile() {
        guard let bridge else { return }
        let command = outputProfileCommand()
        Task {
            _ = try? await bridge.request([
                "type": "media-core-sync", "elapsedMs": elapsedMs(),
                "commands": [command],
            ])
        }
        syncScenes()
    }

    /// Sources tab visible ↔ lease held; the 1 s renewal timer runs only while held.
    func updateSourcePreviewDemand() {
        let visible = selectedTab == .sources
        sourcePreviewDemand.tabVisible(visible)
        if visible, sourcePreviewDemandTimer == nil {
            sourcePreviewDemandTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
                MainActor.assumeIsolated { self?.sourcePreviewDemand.tick() }
            }
        } else if !visible {
            sourcePreviewDemandTimer?.invalidate()
            sourcePreviewDemandTimer = nil
        }
    }

    private func sendSourcePreviewDemand(_ enabled: Bool) {
        let body: JSONObject = [
            "type": "media-core-sync", "elapsedMs": elapsedMs(),
            "commands": [["type": "set-source-preview-demand", "enabled": enabled] as JSONObject],
        ]
        if let capture = coreCommandCaptureForTesting { capture(body); return }
        guard let bridge else { return }
        Task { _ = try? await bridge.request(body) }
    }

    func applyMultiviewConfig() {
        let body: JSONObject = [
            "type": "media-core-sync", "elapsedMs": elapsedMs(),
            "commands": [multiviewConfigCommand()],
        ]
        if let capture = coreCommandCaptureForTesting { capture(body); return }
        guard let bridge else { return }
        Task { _ = try? await bridge.request(body) }
    }

    private func applySharedTexture(_ texture: JSONObject) {
        if let id = texture["iosurfaceId"] as? NSNumber {
            if programSurfaceId != id.uint32Value {
                print("shell: program IOSurface id=\(id.uint32Value)")
            }
            programSurfaceId = id.uint32Value
        }
        if let frame = texture["frameNumber"] as? NSNumber {
            programFrameNumber = frame.int64Value
        }
    }

    func pushWarning(_ message: String) {
        ShellLog.write("warning: \(message)")
        warnings.insert(message, at: 0)
        if warnings.count > 20 {
            warnings.removeLast()
        }
    }

    // ── operator commands ────────────────────────────────────────────────────

    // Accepts a bare meeting number OR a full Zoom URL
    // (https://…zoom.us/j/<id>?pwd=<passcode>) — the engine wants the bare
    // number, so parse URL forms here (the WinUI shell does the same).
    static func parseZoomTarget(_ raw: String, fallbackPasscode: String)
        -> (meetingId: String, passcode: String) {
        let trimmed = raw.trimmingCharacters(in: .whitespaces)
        guard trimmed.lowercased().contains("zoom.us"),
              let url = URL(string: trimmed) else {
            return (trimmed, fallbackPasscode)
        }
        let meetingId = url.pathComponents.last { component in
            !component.isEmpty && component.allSatisfy(\.isNumber)
        } ?? trimmed
        let pwd = URLComponents(url: url, resolvingAgainstBaseURL: false)?
            .queryItems?.first { $0.name == "pwd" }?.value ?? fallbackPasscode
        return (meetingId, pwd)
    }

    // Zoom sign-in surface (broker OAuth). userZak on the join payload gives
    // the join the signed-in account's privileges (host: no waiting room, no
    // record-permission dance) — the product's designed flow.
    @Published var zoomSignedIn = false
    @Published var zoomOAuthStatus = ""
    /// The Zoom pane's join line: the sign-in hint, Zoom's own failure reason,
    /// or "" (ZoomJoinStatus decides; late truth replaces a timeout).
    @Published private(set) var zoomJoin = ZoomJoinStatusTracker()
    var zoomJoinStatus: String { zoomJoin.line }
    /// Offer "Join and end my other Zoom session" (Windows CanEndOtherZoomSession).
    var canEndOtherZoomSession: Bool { zoomJoin.canEndOtherSession }
    @Published var webinar = false
    @Published var recentMeetings: [String] = []

    func signInWithZoom() {
        zoomOAuthStatus = "Waiting for the browser…"
        Task {
            await ZoomOAuth.shared.beginSignIn { [weak self] status in
                Task { @MainActor in await self?.finishOAuthSignIn(status) }
            }
        }
    }

    func signOutZoom() {
        Task {
            await ZoomOAuth.shared.signOut()
            zoomSignedIn = false
            zoomOAuthStatus = ""
        }
    }

    /// The corevideo:// fallback (older brokers / a manual link). The normal
    /// path is the loopback listener, which reports through finishOAuthSignIn.
    func handleOAuthCallback(_ url: URL) {
        Task {
            let error = await ZoomOAuth.shared.handleCallback(url)
            await finishOAuthSignIn(error)
        }
    }

    /// One place both callback paths land ("" = signed in). Brings the app
    /// forward: the operator is looking at a browser tab when this fires.
    func finishOAuthSignIn(_ error: String) async {
        zoomOAuthStatus = error
        zoomSignedIn = await ZoomOAuth.shared.signedIn
        if error.isEmpty {
            ShellLog.write("zoom sign-in complete")
            zoomJoin.signedIn()
        } else {
            pushWarning("zoom sign-in: \(error)")
        }
        // A timeout is not the operator coming back from the browser: don't
        // steal focus five minutes later for it.
        if error != ZoomOAuth.signInTimedOut,
           !ShellHarness.detect(in: ProcessInfo.processInfo.environment) {
            NSApp.activate(ignoringOtherApps: true)
        }
    }

    func refreshZoomSignedIn() {
        Task { zoomSignedIn = await ZoomOAuth.shared.signedIn }
    }

    func openInZoomApp() {
        let trimmed = joinMeetingId.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return }
        let url = trimmed.lowercased().hasPrefix("http")
            ? trimmed
            : "https://zoom.us/j/" + trimmed
        if let parsed = URL(string: url) {
            NSWorkspace.shared.open(parsed)
        }
    }

    /// `endOtherMeeting` is the operator's answer to "your account is already
    /// in this meeting" (#475, Windows EndOtherZoomSessionAndJoinAsync): a
    /// SECOND, explicit gesture that ends their own Zoom client's session, so
    /// it is never the ordinary Join and never remembered for the next one.
    func joinZoom(endOtherMeeting: Bool = false) {
        guard let payload = prepareJoin(endOtherMeeting: endOtherMeeting) else { return }
        Task { await sendJoin(payload) }
    }

    /// The awaitable join (remote control): the same two halves joinZoom runs.
    /// Returns false when the join was refused before anything was sent.
    @discardableResult
    func performJoin(endOtherMeeting: Bool = false) async -> Bool {
        guard let payload = prepareJoin(endOtherMeeting: endOtherMeeting) else { return false }
        await sendJoin(payload)
        return true
    }

    /// The synchronous half of a join: guards, status, recents, the payload.
    private func prepareJoin(endOtherMeeting: Bool) -> JSONObject? {
        guard bridge != nil || quitRequestForTesting != nil else { return nil }
        let target = Self.parseZoomTarget(joinMeetingId, fallbackPasscode: joinPasscode)
        let meetingId = target.meetingId
        let parsedPasscode = target.passcode
        joinPasscode = parsedPasscode
        guard !meetingId.isEmpty, !joinInFlight else { return nil }
        joinInFlight = true
        // Never blocks the join (some meetings allow guests) — just says why it
        // may fail before Zoom does.
        zoomJoin.beginJoin(signedIn: zoomSignedIn)
        recentMeetings.removeAll { $0 == joinMeetingId }
        recentMeetings.insert(joinMeetingId, at: 0)
        recentMeetings = Array(recentMeetings.prefix(5))
        var payload: JSONObject = [
            "meetingId": meetingId,
            "meetingNumber": meetingId,
            "passcode": parsedPasscode,
            "password": parsedPasscode,
            "displayName": displayName,
            "webinar": webinar,
        ]
        // Z1: the camera ceilings, latched by this join (Windows JoinZoomAsync
        // cameraMaxResolution / cameraMaxFps, MediaCoreSupervisor.cs:568-569).
        let ceiling = zoomCameraJoinCeiling
        payload["cameraMaxResolution"] = ceiling.tier
        payload["cameraMaxFps"] = ceiling.fps
        zoomCameraJoinSent = ceiling
        // Absent, not false, on an ordinary join (the core reads it per join).
        if endOtherMeeting { payload["endOtherMeeting"] = true }
        ShellLog.write("joining meeting \(meetingId)\(endOtherMeeting ? " (end my other Zoom session)" : "")")
        return payload
    }

    /// The request half of a join (credentials, zoom-join, the reply applied).
    private func sendJoin(_ payload: JSONObject) async {
        var payload = payload
        defer { joinInFlight = false }
        do {
            // Credentials BEFORE the request timer (the Windows shape):
            // a fresh ZAK per join, never cached, never logged.
            // The harness never reads the operator's Keychain or calls Zoom.
            if quitRequestForTesting == nil,
               let credentials = await ZoomOAuth.shared.ensureJoinCredentials(),
               let zak = credentials.userZak {
                payload["userZak"] = zak
            } else {
                zoomJoin.joiningAsGuest()
            }
            let response = try await quitRequest(
                ["type": "zoom-join", "payload": payload],
                timeout: ZoomJoinStatus.joinRequestTimeout(
                    environment: ProcessInfo.processInfo.environment))
            let snapshot = response["snapshot"] as? JSONObject
            zoomJoin.joinAnswered(snapshot)
            if let snapshot {
                applyZoom(snapshot)
                for warning in snapshot["warnings"] as? [String] ?? [] {
                    pushWarning("zoom: \(warning)")
                }
            }
        } catch {
            zoomJoin.joinAnswered(nil)
            pushWarning("join failed: \(error.localizedDescription)")
        }
    }

    func leaveZoom() {
        guard let bridge else { return }
        assignedIds.removeAll()
        Task { _ = try? await bridge.request(["type": "zoom-leave"], timeout: 15) }
    }

    /// transport.stream.rtmp.configure (Windows StreamDestinationControl.Configure):
    /// validate everything, save the key FIRST, then apply; never starts streaming.
    func configureRtmpForControl(_ args: [Any?]) -> ControlInvokeResult {
        let decision: RtmpDestinationPolicy.Decision
        switch RtmpDestinationPolicy.configure(args: args, busy: streamingDesired || streamingObservedLive,
                                               currentKey: streamKey) {
        case .failure(let refusal): return .fail(refusal.message)
        case .success(let value): decision = value
        }
        guard streamKeySaver(decision.key) else {
            return .fail("Destination settings were not saved. Previous settings remain active; retry after resolving preference storage.")
        }
        streamKey = decision.key
        streamUrl = decision.url
        persistPrefsNow()
        return .success
    }

    /// transport.stream.srt.configure (Windows StreamDestinationControl.Configure, SRT):
    /// validate everything (SrtDestinationControlPolicy), SAVE (Keychain passphrase +
    /// prefs) before anything in memory changes, then apply; never starts streaming.
    func configureSrtForControl(_ args: [Any?]) -> ControlInvokeResult {
        let decision: SrtDestinationControlPolicy.Decision
        switch SrtDestinationControlPolicy.configure(args: args, busy: streamDestinationsLocked,
                                                     current: streamDestinations, currentPassphrase: srtPassphrase) {
        case .failure(let refusal): return .fail(refusal.message)
        case .success(let value): decision = value
        }
        var prefs = currentPrefs()
        prefs.streamDestinations = decision.config
        // Never write prefs before the operator's file is restored (persistPrefsNow's rule).
        guard srtDestinationSaver(prefsRestored ? prefs : nil, decision.passphrase) else {
            return .fail("Destination settings were not saved. Previous settings remain active; retry after resolving preference storage.")
        }
        streamDestinations = decision.config
        srtPassphrase = decision.passphrase
        if prefsRestored { lastSavedPrefs = prefs }
        return .success
    }

    // ── remote control: awaitable twins of the fire-and-forget UI calls ──────
    // (ControlSurface+Session.swift; Windows answers zoom.join/leave from
    // IsInMeeting/JoinStatus, StudioControlSurface.cs:228-251.)

    /// Polls `done` on the main actor for up to controlSettleSeconds.
    private func settle(_ done: () -> Bool) async -> Bool {
        let deadline = Date().addingTimeInterval(controlSettleSeconds)
        while !done() {
            if Date() >= deadline { return false }
            try? await Task.sleep(nanoseconds: 25_000_000)
        }
        return true
    }

    func joinZoomForControl(target: String, displayName name: String?, webinar asWebinar: Bool) async -> ControlInvokeResult {
        let unavailable = ControlInvokeResult.fail("Zoom join is unavailable in the current state.")
        joinMeetingId = target
        if let name, !name.trimmingCharacters(in: .whitespaces).isEmpty { displayName = name }
        webinar = asWebinar   // the UI toggle's field (D8)
        guard bridge != nil || quitRequestForTesting != nil,
              !Self.parseZoomTarget(target, fallbackPasscode: joinPasscode).meetingId.isEmpty,
              !joinInFlight else { return unavailable }
        guard await performJoin() else { return unavailable }
        _ = await settle { meetingState == "in_meeting" || Self.meetingHasEnded(meetingState) }
        if meetingState == "in_meeting" { return .success }
        if Self.meetingHasEnded(meetingState) {
            return .fail(zoomJoinStatus.isEmpty ? "Zoom join did not complete." : zoomJoinStatus)
        }
        return .fail("Zoom join is still in progress (\(meetingState)); read zoomStatus for the outcome.")
    }

    func leaveZoomForControl() async -> ControlInvokeResult {
        guard bridge != nil || quitRequestForTesting != nil, !Self.meetingHasEnded(meetingState) else {
            return .fail("Zoom leave is unavailable in the current state.")
        }
        assignedIds.removeAll()
        if let snapshot = (try? await quitRequest(["type": "zoom-leave"], timeout: 15))?["snapshot"] as? JSONObject,
           !snapshot.isEmpty {
            applyZoom(snapshot)
        }
        if await settle({ Self.meetingHasEnded(meetingState) }) { return .success }
        return .fail("Zoom leave did not complete (\(meetingState)).")
    }

    func setCaptureDeviceOnline(_ deviceId: String, online: Bool) async -> ControlInvokeResult {
        guard let device = captureDevices.first(where: { $0.id == deviceId }) else {
            return .fail("The capture device is unavailable.")
        }
        // The UI entry point is a toggle: a matching state must not flip it.
        if (device.connectionState == "connected") == online { return .success }
        do {
            _ = try await captureRequest(online
                ? CoreCommands.connectCaptureDevice(deviceId: device.id, outputSourceId: device.id)
                : CoreCommands.disconnectCaptureDevice(deviceId: device.id))
            let reply = try await captureRequest(["type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": [JSONObject]()])
            if let snapshot = reply["snapshot"] as? JSONObject { applySnapshot(snapshot) }
            syncSpine()   // multiview sources include connected capture devices (the UI path's follow-up)
        } catch {
            return .fail("Capture device '\(deviceId)' could not be \(online ? "connected" : "taken offline"): \(error.localizedDescription)")
        }
        let state = captureDevices.first { $0.id == deviceId }?.connectionState ?? "missing"
        if (state == "connected") == online { return .success }
        return .fail("Capture device '\(deviceId)' is \(state), not \(online ? "connected" : "offline").")
    }

    func toggleAssigned(_ participant: RosterParticipant) {
        if let index = slots.firstIndex(where: {
            $0.kind == "zoom" && $0.sourceId == participant.id
        }) {
            unassignSlot(slots[index].id)
        } else if let empty = firstEmptySlotIndex() {
            assignSlot(slots[empty].id, kind: "zoom",
                       sourceId: participant.id, name: participant.name)
        }
    }

    // The assign path: mirrors buildZoomMediaSpineSyncPayload (spec section in
    // docs/mac-port-phase4-swiftui-shell.md). Subscriptions request raw video
    // for every assigned participant; the compositor's fallback grid composes
    // whatever frames arrive.
    /// Count of spine re-push requests (harness evidence for the triggers).
    private(set) var spineSyncRequests = 0

    private func syncSpine() {
        spineSyncRequests += 1
        spineSyncTask?.cancel()
        spineSyncTask = Task { [weak self] in
            await self?.timerSleep(nanoseconds: 120_000_000)
            guard !Task.isCancelled else { return }
            await self?.pushSpine()
        }
    }

    /// Test seam for the spine push only (nil = the real bridge). Separate from
    /// quitRequestForTesting so harnesses that count scene-sync bodies are not
    /// handed spine requests they never asked for.
    var spineRequestForTesting: ((JSONObject) async throws -> JSONObject)?

    private func pushSpine() async {
        let send: (JSONObject) async throws -> JSONObject
        if let seam = spineRequestForTesting { send = seam }
        else if let bridge { send = { try await bridge.request($0) } }
        else { return }
        let requests = spinePushRequests()
        for request in requests {
            do {
                _ = try await send(request)
            } catch {
                if request["type"] as? String == "media-core-sync" {
                    // Not delivered: forget the memo so the debounced paths retry.
                    lastSyncedStripIds = nil
                    lastRoutingBaseSent = nil
                    pushWarning("audio console sync failed: \(error.localizedDescription)")
                } else {
                    pushWarning("assign failed: \(error.localizedDescription)")
                }
            }
        }
    }

    /// The ordered requests one spine push sends. When the audio console changed
    /// with this source set, the channel list and/or base routing go FIRST, in
    /// their own media-core-sync, AHEAD of the zoom-media-spine-sync that
    /// subscribes the audio (round-2 review): a returning A1-muted guest's mute
    /// (I2's memory) — and the routing that keeps the core off its raw unmuted
    /// fallback sum — must be in the core before that guest's PCM can be mixed.
    /// The core's mailbox is FIFO and pushSpine awaits each request in order; the
    /// 150 ms mix debounce used to let the subscription win by that much.
    /// Internal: the harness checks the order.
    func spinePushRequests() -> [JSONObject] {
        let participants: [JSONObject] = roster.map { participant in
            [
                "sdkUserId": participant.id,
                "displayName": participant.name,
                "role": productionRoles.byParticipant[participant.id] ?? "guest",
                "videoOn": participant.hasVideo,
                "muted": participant.muted,
                "talking": participant.talking,
            ]
        }
        let parts = resolveSpineParts()
        let subscriptions = parts.subscriptions
        refreshStrips()  // the console follows the audio source set just decided
        var audio: [JSONObject] = []
        let stripIds = Set(strips.map(\.id))
        if !strips.isEmpty, stripIds != lastSyncedStripIds {
            mixSyncTask?.cancel()
            scheduledStripIds = nil
            audio.append(audioMixCommand())
            lastSyncedStripIds = stripIds
        }
        if let routing = routingBaseCommand(), let sends = Self.routingMemo(routing),
           !(lastRoutingBaseSent.map { NSArray(array: $0).isEqual(to: sends) } ?? false) {
            routingBaseTask?.cancel()
            audio.append(routing)
            lastRoutingBaseSent = sends
        }
        // Multiview sources = the in-show slots that still HAVE a source. An
        // offline slot (participant left, device unplugged) kept its tile and
        // rendered as an unlabeled gray placeholder — a dead box on the wall.
        // The budget's labels ride the sources ("<name> · no video: subscription
        // limit (10)"); the bus notices name a cued guest it left out.
        let multiviewSources = parts.multiviewSources
        let payload: JSONObject = [
            // startCapture is what arms raw media (engine start_media →
            // start_raw_media): without it every video subscription stays
            // "deferred" and no frames ever flow. Operator-driven via the
            // Capture button (the Windows "Engine On" toggle), never automatic.
            "startCapture": captureEnabled,
            "multiview": [
                "canvasWidth": 1920, "canvasHeight": 1080,
                "sources": multiviewSources,
                "programNotice": parts.programNotice,   // always present: "" clears it
                "previewNotice": parts.previewNotice,
            ],
            "readiness": [
                "status": "ready", "platform": "darwin", "sdkVersion": "",
                "checks": [], "blockers": [], "warnings": [], "summary": "mac shell",
            ],
            "participants": participants,
            "subscriptions": subscriptions,
            // The core's speaker director follows the talker only among these (#478 R1).
            "sourceParticipantIds": parts.sourceParticipantIds,
            "videoSubscriptionShortfall": parts.shortfall,
            "blocked": false,
            "warnings": parts.warnings,
            "summary": "mac shell spine sync",
        ]
        var requests: [JSONObject] = []
        if !audio.isEmpty {
            requests.append(["type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": audio])
        }
        requests.append(["type": "zoom-media-spine-sync", "spinePayload": payload,
                         "elapsedMs": elapsedMs()])
        return requests
    }

    /// The spine `subscriptions` array for the current show, and — as the one
    /// place the audio source set is decided — records it for the audio console
    /// (`zoomAudioSourceIds` / `meetingAudioSubscribed`). Internal so the harness
    /// drives the real call site, not a hand-copied list.
    func resolveSpineSubscriptions() -> [JSONObject] {
        resolveSpineParts().subscriptions
    }

    /// The source-set half of a spine build: observes the sticky Tiles latch ONCE
    /// with the FULL in-room roster, then resolves subscriptions, source ids, the
    /// shortfall, the warning, the bus notices and the multiview labels.
    func resolveSpineParts() -> ZoomSourceBudget.SpineParts {
        var input = spineSourceInput()
        input.stickyAudio = tilesAudioLatch.observe(
            inMeeting: tilesLatchMeetingState,
            programSceneId: programSceneId, programTiles: input.programTiles,
            previewSceneId: previewSceneId, previewTiles: input.previewTiles,
            present: Set(roster.map(\.id)))
        let parts = ZoomSourceBudget.spineParts(input, names: Dictionary(
            roster.map { ($0.id, $0.name) }, uniquingKeysWith: { first, _ in first }))
        zoomAudioSourceIds = parts.subscriptions
            .filter { $0["kind"] as? String == "participant-audio" }
            .compactMap { $0["participantId"] as? String }
        meetingAudioSubscribed = !roster.isEmpty
        return parts
    }

    /// The source-set input for the current show (Windows ZoomSourceSetPolicy.Input):
    /// the room, both buses' routes AND Tiles walls (the same `tilesPayload(for:)` the
    /// scene sync sends), the multiview wall, and ISO-armed guests.
    func spineSourceInput() -> ZoomSourceBudget.Input {
        var input = ZoomSourceBudget.Input()
        input.participants = roster.map { ($0.id, $0.hasVideo) }
        if !programSceneId.isEmpty {
            input.programRoutes = buildRoutes(for: programSceneId, isProgram: true)
            input.programTiles = tilesPayload(for: programSceneId)
        }
        if !previewSceneId.isEmpty {
            input.previewRoutes = buildRoutes(for: previewSceneId)
            input.previewTiles = tilesPayload(for: previewSceneId)
        }
        input.multiview = Self.multiviewSourcePayloads(slots, mediaAssets: mediaAssets)
        input.iso = isoRecordingEnabled ? slots.filter { $0.kind == "zoom" && $0.iso }.map(\.sourceId) : []
        return input
    }

    // The multiview roster on the wire: one tile per in-show slot that still has
    // a source. `slot` is the 0-based slot number (Windows sends SlotNumber - 1,
    // ShowInputRosterService.cs:735-755): without it the core numbers by list position,
    // so a filtered list put slot 5's source in cell 3 of the source sheet. `kind` is the MULTIVIEW vocabulary the core's resolveMultiviewFeed
    // reads (native/src/core/MultiviewFeed.h): "capture" | "media" | anything else
    // = a Zoom participant. It is NOT the scene-route `mode` ("capture-input").
    static func multiviewSourcePayloads(_ slots: [ShowInputSlot], mediaAssets: [MediaAsset] = []) -> [JSONObject] {
        slots
            .filter { $0.kind != "unassigned" && $0.inShow && !$0.offline }
            .compactMap { slot -> JSONObject? in
                switch slot.kind {
                case "zoom":
                    return ["sourceId": "zoom:" + slot.sourceId, "kind": "participant-video",
                            "participantId": slot.sourceId, "slot": slot.id - 1, "label": slot.name]
                case "media":
                    // Windows ShowInputRosterService.cs:743-755, plus the path and kind so the
                    // core can decode a still that only the wall shows (plan Task 5).
                    guard let asset = mediaAssets.first(where: { $0.id == slot.sourceId }) else { return nil }
                    return ["sourceId": "media:" + slot.sourceId, "kind": "media", "mediaAssetId": slot.sourceId,
                            "mediaAssetPath": asset.filePath, "mediaAssetKind": asset.kind,
                            "slot": slot.id - 1, "label": slot.name]
                default:
                    return ["sourceId": "capture:" + slot.sourceId, "kind": "capture",
                            "captureDeviceId": slot.sourceId, "slot": slot.id - 1, "label": slot.name]
                }
            }
    }

    // The exact objects placed in the spine `subscriptions` array — the Windows
    // ZoomMediaSpinePayloadBuilder.BuildSubscriptions shape, field for field:
    //
    //  - `meeting-audio` for the synthetic MeetingMixParticipantId ("0"), purpose
    //    "program", priority 0, whenever the meeting has participants. The Zoom
    //    program mix is an audio source in its own right and is deliberately NOT
    //    narrowed to the source set. Without it there is no "zoom-mix" PCM at all.
    //  - `participant-video` per budget entry, priority 10+i. The tiered kind
    //    (E3/E4/E6, ZoomSubscriptionResolutionPolicy.h): the core grants 1080P to
    //    the first 8 camera-on entries in this order, and its engine-side identity
    //    ("participant-video-<pid>-camera") excludes purpose, so the real
    //    `entry.purpose` rides the wire without churning on a bus move.
    //  - `participant-audio` per audio source (camera-off sources included),
    //    purpose "mix", priority 40+i. The engine identity for audio DOES include
    //    purpose ("participant-audio-<pid>-mix", ZoomEngineRuntime.cpp), so "mix" is
    //    a fixed constant — never a bus purpose that could churn the subscription.
    //
    // Until this shipped the Mac subscribed VIDEO ONLY: a live 9-guest meeting had
    // raw media active and audioMixSession.participants == 0 — no channels, nothing
    // to monitor.
    nonisolated static func spineSubscriptionPayloads(
        _ entries: [ZoomSourceBudget.Entry], audioParticipantIds: [String],
        meetingHasParticipants: Bool
    ) -> [JSONObject] {
        var payloads: [JSONObject] = []
        if meetingHasParticipants {
            payloads.append([
                "participantId": ZoomSourceBudget.meetingMixParticipantId,
                "kind": "meeting-audio",
                "purpose": "program",
                "priority": 0,
            ])
        }
        for (index, entry) in entries.enumerated() {
            payloads.append([
                "participantId": entry.participantId,
                "kind": "participant-video",
                "purpose": entry.purpose,
                "priority": 10 + index,
            ])
        }
        for (index, id) in audioParticipantIds.enumerated() {
            payloads.append([
                "participantId": id,
                "kind": "participant-audio",
                "purpose": "mix",
                "priority": 40 + index,
            ])
        }
        return payloads
    }

    // Per-source dropout policy commands (core contract #535 slice 4a,
    // E9/E10). Zoom slots ONLY — a capture/media id carrying a dropoutPolicy
    // pushes a sticky scene-validation warning EVERY sync, which is the
    // expensive regression this filter guards. An explicit "hold" IS sent
    // (PRESENT-OR-KEEP means omission can never overwrite a stored "black"),
    // but a slot with no operator choice ("") emits nothing — the core's own
    // default applies. Sorted by sourceId ordinal for determinism (E10, mirrors
    // Windows' MediaCoreCommandBuilder) and de-duplicated by sourceId
    // keep-first: two slots holding the same participant must not race.
    // displayName is deliberately not sent here — Windows sends it for slate
    // names, but Mac's slate-name path is unverified; a parity follow-up, not
    // shipped blind.
    /// One `set-source-policy` per assigned zoom/capture source, de-duplicated by wire id
    /// (keep first) and sorted by the wire id. Each carries `displayName` (the failed-source
    /// slate name); `dropoutPolicy` only when the operator chose hold/black — an explicit
    /// hold IS sent (PRESENT-OR-KEEP, MediaCore.cpp:1898-1905).
    static func sourcePolicyCommands(slots: [ShowInputSlot],
                                     displayName: (ShowInputSlot) -> String) -> [JSONObject] {
        var seen = Set<String>()
        var commands: [(wire: String, command: JSONObject)] = []
        for slot in slots {
            guard let wire = slot.sourceKey, seen.insert(wire).inserted else { continue }
            var command: JSONObject = ["type": "set-source-policy", "sourceId": wire,
                                       "displayName": displayName(slot)]
            if slot.dropoutPolicy == "hold" || slot.dropoutPolicy == "black" {
                command["dropoutPolicy"] = slot.dropoutPolicy
            }
            commands.append((wire, command))
        }
        return commands.sorted { $0.wire < $1.wire }.map(\.command)
    }

    /// Appends the source-policy commands AFTER whatever is already in the
    /// batch. The core clears `sceneValidationWarnings_` on `load-scene-graph`,
    /// so a policy placed before it would have any refusal warning silently
    /// wiped — policies must always come last.
    /// The name rides the session-only slot name (#1213, SlotNamePolicy): there is no
    /// per-source name store (plan Task 8 superseded).
    static func appendSourcePolicies(to commands: [JSONObject], slots: [ShowInputSlot],
                                     displayName: (ShowInputSlot) -> String = { $0.name }) -> [JSONObject] {
        commands + sourcePolicyCommands(slots: slots, displayName: displayName)
    }

    /// Operator sets a slot's dropout policy. Validated against the core's
    /// exact vocabulary; an unknown value is silently refused rather than
    /// risking a sticky scene-validation warning.
    func setDropoutPolicy(slotId: Int, policy: String) {
        guard policy == "hold" || policy == "black",
              let index = slots.firstIndex(where: { $0.id == slotId }),
              slots[index].kind == "zoom" || slots[index].kind == "capture"
        else { return }
        slots[index].dropoutPolicy = policy
        syncScenes()
    }

    // ── scenes + Take ────────────────────────────────────────────────────────

    // Rects lifted from SceneCanvasLayoutService: single, two-up (0.02 gap),
    // pip (full frame + 0.28 corner inset).
    static let layoutRects: [String: [(Double, Double, Double, Double)]] = [
        "single": [(0, 0, 1, 1)],
        "two-up": [(0, 0, 0.49, 1), (0.51, 0, 0.49, 1)],
        "pip": [(0, 0, 1, 1), (0.68, 0.68, 0.28, 0.28)],
    ]

    // `isProgram` so only the PROGRAM bus honors a cut — preview keeps showing
    // its cued scene, exactly like a switcher's cut bus.
    /// The `chromaKey` node for a slot, or nil when keying is off.
    ///
    /// Sent ONLY when enabled: the core treats a chromaKey object without an
    /// explicit `enabled:false` as "key this", so shipping settings for a
    /// switched-off key would punch holes in a live program.
    func chromaKeyNode(for slot: ShowInputSlot) -> JSONObject? {
        guard slot.keyEnabled else { return nil }
        let rgb = Self.rgbComponents(slot.keyColorHex)
        return [
            "enabled": true,
            "keyR": rgb.r, "keyG": rgb.g, "keyB": rgb.b,
            "similarity": slot.keySimilarity,
            "smoothness": slot.keySmoothness,
            "spill": slot.keySpill,
        ]
    }

    /// "#rrggbb" to 0..1 components. Falls back to green — the common case, and
    /// never a black key, which would key shadows out of every source.
    static func rgbComponents(_ hex: String) -> (r: Double, g: Double, b: Double) {
        let cleaned = hex.trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: "#", with: "")
        guard cleaned.count == 6, let value = UInt32(cleaned, radix: 16) else {
            return (0, 1, 0)
        }
        return (Double((value >> 16) & 0xff) / 255.0,
                Double((value >> 8) & 0xff) / 255.0,
                Double(value & 0xff) / 255.0)
    }

    /// The ONE place a scene route takes a slot's source, for every route builder
    /// (preset, edited layer, solo cue). Zoom: fixed + participantId. Capture: the
    /// "capture-input" route mode + captureDeviceId. Media (plan Task 4): fixed + the
    /// mediaAsset* identity the core decodes from (the same shape as a media layer);
    /// a missing asset leaves a fixed route with no source, which the core draws blank.
    func bindSlot(_ slot: ShowInputSlot, to route: inout SceneRoute) {
        switch slot.kind {
        case "zoom":
            route.mode = "fixed"
            route.participantId = slot.sourceId
        case "media":
            route.mode = "fixed"
            route.media = mediaAssets.first { $0.id == slot.sourceId }.map(Self.mediaRouteFields)
        default:
            route.mode = "capture-input"
            route.captureDeviceId = slot.sourceId
        }
        route.chromaKey = chromaKeyNode(for: slot)
    }

    static func mediaRouteFields(_ asset: MediaAsset) -> JSONObject {
        ["mediaAssetId": asset.id, "mediaAssetName": asset.name, "mediaAssetKind": asset.kind,
         "mediaAssetPath": asset.filePath, "mediaAssetLoop": MediaRoutePlayback.isLoopingKind(asset.kind)]
    }

    func buildRoutes(for sceneId: String, isProgram: Bool = false) -> [JSONObject] {
        if Self.isSoloScene(sceneId) {
            // Each solo scene owns its source: re-cueing one never touches the
            // other, so the one on Program never changes without a Take.
            // The source captured at cue time (Windows BuildSoloRoute), never the
            // slot's current assignment: a slot edit must not change Program.
            guard let slot = soloCueByScene[sceneId],
                  slot.kind != "unassigned" else { return [] }
            var route = SceneRoute(
                routeId: sceneId + "-solo", mode: "fixed",
                participantId: nil, captureDeviceId: nil,
                rect: (0, 0, 1, 1), zIndex: 0)
            bindSlot(slot, to: &route)
            return [route.json]
        }
        // A Tiles scene sends NO routes (Windows BuildProductionSyncContext): the
        // wall owns the scene, and a route beside it (the logo bug included)
        // shares its order namespace and composites between tiles.
        if isTilesScene(sceneId) { return [] }
        guard let scene = scenes.first(where: { $0.id == sceneId }),
              let rects = Self.layoutRects[scene.layout] else { return [] }
        let inShow = slots.filter { $0.kind != "unassigned" && $0.inShow && !$0.offline }
        // An edited canvas OWNS its scene: layers carry their own rects, fit
        // and opacity, and bind to a slot (nil = active speaker).
        if !scene.layers.isEmpty {
            var edited: [JSONObject] = scene.layers.enumerated().compactMap { index, layer -> JSONObject? in
                var route = SceneRoute(
                    routeId: "\(sceneId)-layer-\(index)", mode: "active-speaker",
                    participantId: nil, captureDeviceId: nil,
                    rect: (layer.x, layer.y, layer.width, layer.height), zIndex: index)
                // An overlay layer bound to a media-bin still (Add overlay):
                // the mediaAsset* fields are the media identity on the wire,
                // exactly like the logo bug below.
                if let assetId = layer.mediaAssetId {
                    // A still that left the bin sends nothing: the core warns
                    // on every sync about an id that arrives without a path.
                    guard let asset = mediaAssets.first(where: { $0.id == assetId }) else { return nil }
                    return [
                        "routeId": "\(sceneId)-layer-\(index)", "mode": "fixed", "audioRole": "mix",
                        "fitMode": layer.fitMode, "opacity": max(0, min(1, layer.opacity)),
                        "zIndex": index,
                        "mediaAssetId": assetId, "mediaAssetName": asset.name,
                        "mediaAssetKind": asset.kind, "mediaAssetPath": asset.filePath,
                        "mediaAssetLoop": MediaRoutePlayback.isLoopingKind(asset.kind),
                        "rect": ["x": layer.x, "y": layer.y, "width": layer.width, "height": layer.height],
                    ]
                }
                if let slotId = layer.slotId,
                   let slot = slots.first(where: { $0.id == slotId }),
                   slot.kind != "unassigned" {
                    bindSlot(slot, to: &route)
                }
                var json = route.json
                json["fitMode"] = layer.fitMode
                json["opacity"] = max(0, min(1, layer.opacity))
                return json
            }
            if let bug = logoBug {
                let rect = MediaBin.bugRect(naturalWidth: bug.naturalWidth,
                                            naturalHeight: bug.naturalHeight)
                edited.append([
                    "routeId": "\(sceneId)-bug", "mode": "fixed", "audioRole": "mix",
                    "fitMode": "fit", "opacity": 1.0, "zIndex": edited.count,
                    "mediaAssetId": bug.id, "mediaAssetName": bug.name,
                    "mediaAssetKind": bug.kind, "mediaAssetPath": bug.filePath,
                    "mediaAssetLoop": MediaRoutePlayback.isLoopingKind(bug.kind),
                    "rect": ["x": rect.x, "y": rect.y,
                             "width": rect.w, "height": rect.h],
                ])
            }
            return edited
        }
        // Otherwise slots fill the layout preset in slot order (zoom AND
        // capture); empty cells follow the active speaker so a scene is never
        // a black hole.
        var routes: [JSONObject] = rects.enumerated().map { index, rect in
            var route = SceneRoute(
                routeId: "\(sceneId)-\(index)", mode: "active-speaker",
                participantId: nil, captureDeviceId: nil,
                rect: (rect.0, rect.1, rect.2, rect.3), zIndex: index)
            if index < inShow.count {
                bindSlot(inShow[index], to: &route)
            }
            return route.json
        }
        // The logo bug rides every scene topmost — the mediaAsset* fields ARE
        // the media identity on the wire (no sourceId; the core derives
        // media:<assetId> and the StillMediaFrameCache decodes the path).
        if let bug = logoBug {
            let rect = MediaBin.bugRect(naturalWidth: bug.naturalWidth,
                                        naturalHeight: bug.naturalHeight)
            routes.append([
                "routeId": "\(sceneId)-bug", "mode": "fixed", "audioRole": "mix",
                "fitMode": "fit", "opacity": 1.0, "zIndex": routes.count,
                "mediaAssetId": bug.id, "mediaAssetName": bug.name,
                "mediaAssetKind": bug.kind, "mediaAssetPath": bug.filePath,
                "mediaAssetLoop": MediaRoutePlayback.isLoopingKind(bug.kind),
                "rect": ["x": rect.x, "y": rect.y, "width": rect.w, "height": rect.h],
            ])
        }
        return routes
    }

    // ── scene canvas editing ─────────────────────────────────────────────────

    // Seed the editable layer list from the layout preset the scene already
    // shows, so opening the editor never changes what is on screen.
    func ensureLayers(for sceneId: String) {
        // A Tiles scene never gets layers (the wall owns it). Silent here: the
        // scene editor calls this on appear; the explicit layer actions say why.
        if isTilesScene(sceneId) { return }
        guard let index = scenes.firstIndex(where: { $0.id == sceneId }),
              scenes[index].layers.isEmpty,
              let rects = Self.layoutRects[scenes[index].layout] else { return }
        let inShow = slots.filter { $0.kind != "unassigned" && $0.inShow && !$0.offline }
        scenes[index].layers = rects.enumerated().map { slot, rect in
            SceneLayer(id: "\(sceneId)-layer-\(slot)",
                       slotId: slot < inShow.count ? inShow[slot].id : nil,
                       x: rect.0, y: rect.1, width: rect.2, height: rect.3)
        }
    }

    // `sync: false` for live drag deltas — the gesture's onEnded syncs once.
    func updateLayer(sceneId: String, layerId: String, sync: Bool = true,
                     _ apply: (inout SceneLayer) -> Void) {
        guard let sceneIndex = scenes.firstIndex(where: { $0.id == sceneId }),
              let layerIndex = scenes[sceneIndex].layers
                  .firstIndex(where: { $0.id == layerId }) else { return }
        apply(&scenes[sceneIndex].layers[layerIndex])
        if sync { syncScenes() }
    }

    func addLayer(to sceneId: String) {
        if refuseLayerEditOnTiles(sceneId) { return }
        guard let index = scenes.firstIndex(where: { $0.id == sceneId }) else { return }
        ensureLayers(for: sceneId)
        let count = scenes[index].layers.count
        scenes[index].layers.append(SceneLayer(
            id: "\(sceneId)-layer-\(count)-\(Int(Date().timeIntervalSince1970))",
            slotId: nil, x: 0.55, y: 0.55, width: 0.4, height: 0.4))
        syncScenes()
    }

    func removeLayer(sceneId: String, layerId: String) {
        guard let index = scenes.firstIndex(where: { $0.id == sceneId }) else { return }
        scenes[index].layers.removeAll { $0.id == layerId }
        syncScenes()
    }

    func moveLayer(sceneId: String, layerId: String, forward: Bool) {
        guard let sceneIndex = scenes.firstIndex(where: { $0.id == sceneId }),
              let from = scenes[sceneIndex].layers
                  .firstIndex(where: { $0.id == layerId }) else { return }
        let to = forward ? from + 1 : from - 1
        guard scenes[sceneIndex].layers.indices.contains(to) else { return }
        scenes[sceneIndex].layers.swapAt(from, to)
        syncScenes()
    }

    // Explicit save (prefs also autosave on the 5s change tick — this is the
    // operator action, and it reports back).
    @Published var sceneSaveStatus = ""

    func saveScenes() {
        let prefs = currentPrefs()
        prefs.save()
        lastSavedPrefs = prefs
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        sceneSaveStatus = "Saved \(formatter.string(from: Date()))"
    }

    // ── scene naming + removal rules (Windows #1196 / #1070) ─────────────────
    // Ported from SceneNamingPolicy.Validate and SceneCatalogPolicy.RemovalRefusal;
    // the message text is the Windows text, verbatim.
    static let sceneNameBlankMessage = "Enter a scene name."
    static let sceneNameDuplicateMessage =
        "A scene with that name already exists. Choose a different name."
    static let sceneOnAirRefusal = "The on-air scene cannot be removed"
    static let sceneLastRefusal = "The last scene cannot be removed"

    /// Operator-facing result of the last rename/delete (refusal text or "").
    @Published var sceneEditMessage = ""
    /// Studio "Edit layout" overlay open (Tier 0.14, StudioSceneEditing.swift).
    @Published var studioLayoutEditing = false
    /// Windows CommandStatus for the Studio scene gestures (overlay, swap, duplicate).
    @Published var studioCommandStatus = ""

    /// How many scene-graph syncs have been requested. Test seam: a refused
    /// edit must leave this unchanged (nothing reached the wire).
    private(set) var sceneSyncRequestCount = 0

    /// The scene the Scenes tab edits: the PREVIEW scene only. With Preview
    /// empty there is nothing to edit — it never falls back to Program, which
    /// is live (#1196: editing must not change what is on air). Preview ==
    /// Program (the on-air rail row clicked, or the one-scene launch fallback)
    /// is the on-air scene too, so it is no target either.
    var sceneEditTargetId: String? {
        if previewSceneId.isEmpty || previewSceneId == programSceneId { return nil }
        // A multiview solo cue is not an operator scene: its route is the cued
        // source, and canvas layers on it would never render.
        if Self.isSoloScene(previewSceneId) { return nil }
        return previewSceneId
    }

    /// SceneNamingPolicy.Validate: blank, or another scene's name
    /// (case-insensitive, trimmed) -> the Windows message; nil when fine.
    func sceneNameRefusal(sceneId: String?, name: String) -> String? {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return Self.sceneNameBlankMessage }
        let clash = railScenes.contains { $0.id != sceneId
            && $0.name.compare(trimmed, options: .caseInsensitive) == .orderedSame }
        return clash ? Self.sceneNameDuplicateMessage : nil
    }

    /// Why a scene cannot be removed right now (SceneCatalogPolicy.RemovalRefusal):
    /// the on-air scene, or the last visible one. Nil when it can go.
    func sceneRemovalRefusal(_ sceneId: String) -> String? {
        if sceneId == programSceneId { return Self.sceneOnAirRefusal }
        let reserved = Self.isSoloScene(sceneId)
        if !reserved && railScenes.count <= 1 { return Self.sceneLastRefusal }
        return nil
    }

    /// Metadata only: every other field (layers, background) is carried, and
    /// nothing is synced — a name never reaches the wire. Returns the refusal
    /// message, or nil when applied (the trimmed name is stored).
    @discardableResult
    func renameScene(_ sceneId: String, to name: String) -> String? {
        guard let index = scenes.firstIndex(where: { $0.id == sceneId }) else {
            return "This scene is no longer available."
        }
        if let refusal = sceneNameRefusal(sceneId: sceneId, name: name) {
            sceneEditMessage = refusal
            return refusal
        }
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if scenes[index].name != trimmed { scenes[index].name = trimmed }
        sceneEditMessage = ""
        return nil
    }

    func duplicateScene(_ sceneId: String) {
        guard let source = scenes.first(where: { $0.id == sceneId }) else { return }
        var name = source.name + " copy"
        var suffix = 2
        while scenes.contains(where: { $0.name == name }) {
            name = source.name + " copy \(suffix)"
            suffix += 1
        }
        let newId = source.id + "-copy-\(Int(Date().timeIntervalSince1970))"
        // Every field is carried (background included); only identity and
        // the per-layer ids change.
        let copy = SceneDef(id: newId, name: name, layout: source.layout,
                            layers: source.layers.map { layer in
                                var l = layer
                                l = SceneLayer(id: newId + "-" + layer.id,
                                               slotId: l.slotId, x: l.x, y: l.y,
                                               width: l.width, height: l.height,
                                               fitMode: l.fitMode, opacity: l.opacity,
                                               mediaAssetId: l.mediaAssetId)
                                return l
                            },
                            backgroundAssetId: source.backgroundAssetId,
                            tiles: source.tiles)
        scenes.append(copy)
        previewSceneId = newId
        studioCommandStatus = "\(name) duplicated from \(source.name) and cued on preview"
        syncScenes()
        saveScenes()
    }

    func newScene(layout: String) {
        var newId = "scene-\(Int(Date().timeIntervalSince1970))"
        while scenes.contains(where: { $0.id == newId }) { newId += "-n" }   // two new scenes in one second
        var suffix = railScenes.count + 1
        var name = "Scene \(suffix)"
        while scenes.contains(where: { $0.name == name }) {
            suffix += 1
            name = "Scene \(suffix)"
        }
        scenes.append(SceneDef(id: newId, name: name, layout: layout))
        previewSceneId = newId
        ensureLayers(for: newId)
        syncScenes()
        saveScenes()
    }

    /// Windows #1070: the on-air scene and the last visible scene are refused
    /// (message in `sceneEditMessage`, nothing synced). Removing the Preview
    /// scene empties Preview; Program is never touched here.
    @discardableResult
    func deleteScene(_ sceneId: String) -> String? {
        if let refusal = sceneRemovalRefusal(sceneId) {
            sceneEditMessage = refusal
            return refusal
        }
        guard let scene = scenes.first(where: { $0.id == sceneId }) else { return nil }
        scenes.removeAll { $0.id == sceneId }
        if Self.starterSceneIds.contains(sceneId), !removedStarterSceneIds.contains(sceneId) {
            removedStarterSceneIds.append(sceneId)
        }
        // Windows SceneCatalogPolicy.PreviewAfterRemoval: Preview moves to
        // Program. An EMPTY Preview sends no set-preview-scene, so the core's
        // PVW bus kept compositing the deleted scene while the chip read blank.
        // PVW == PGM keeps Take disabled; the on-air scene is never removable.
        if previewSceneId == sceneId { previewSceneId = programSceneId }
        sceneEditMessage = "\(scene.name) removed"
        syncScenes()
        saveScenes()
        return nil
    }

    // Back to the layout preset (drops the edited canvas).
    func resetLayers(for sceneId: String) {
        guard let index = scenes.firstIndex(where: { $0.id == sceneId }) else { return }
        scenes[index].layers = []
        syncScenes()
    }

    // ── multiview source bus (StudioViewModel.PreviewMultiviewTile) ──────────
    //
    // Clicking a multiviewer source tile CUES that source solo to PREVIEW —
    // then Take puts it on air. Ported from Windows PreviewMultiviewTile:
    //  - TWO reserved solo scene ids; the cue always lands on the one NOT on
    //    Program, so Preview can never equal Program after a cue (Take stays
    //    possible) and the on-air solo scene is never re-pointed.
    //  - The solo scene is a REAL entry in `scenes` (EnsureMultiviewSoloScene),
    //    because Take and canTake only move scenes that exist.
    //  - Each solo scene owns its route (`soloCueByScene`), so a cue changes
    //    Preview only: "program changes exclusively on Take". The entry is a
    //    SNAPSHOT of the slot taken at cue time — Windows BuildSoloRoute copies
    //    the participant / capture device id into the route — so unassigning or
    //    reassigning that slot later never re-points (or blacks) Program.
    // Solo scenes are reserved: never in the rail, never persisted, never in
    // the control API's scene.list.
    static let soloSceneA = "mv-solo-a"
    static let soloSceneB = "mv-solo-b"
    static let soloSceneName = "Manual one-up"
    @Published var soloCueByScene: [String: ShowInputSlot] = [:]

    static func isSoloScene(_ sceneId: String) -> Bool {
        sceneId == soloSceneA || sceneId == soloSceneB
    }

    // Reserved solo scenes never appear in the rail.
    var railScenes: [SceneDef] {
        scenes.filter { !Self.isSoloScene($0.id) }
    }

    private func ensureSoloScene(_ sceneId: String) {
        guard !scenes.contains(where: { $0.id == sceneId }) else { return }
        scenes.append(SceneDef(id: sceneId, name: Self.soloSceneName, layout: "single"))
    }

    /// The PGM/PVW chip label for a bus scene id: a solo scene names its SOURCE
    /// (Windows ResolveOnAirSceneLabel), any other scene its name.
    func busLabel(for sceneId: String) -> String {
        if Self.isSoloScene(sceneId) {
            if let slot = soloCueByScene[sceneId] {
                return slot.name.isEmpty ? "Input \(slot.id)" : slot.name
            }
            return "Solo"
        }
        return scenes.first { $0.id == sceneId }?.name ?? "—"
    }

    // Tile ids are wire source ids ("zoom:<pid>" / "capture:<id>").
    func slotForSourceId(_ sourceId: String) -> Int? {
        let bare = sourceId.contains(":")
            ? String(sourceId.drop(while: { $0 != ":" }).dropFirst()) : sourceId
        return slots.first { $0.kind != "unassigned" && $0.sourceId == bare }?.id
    }

    func previewMultiviewSlot(_ slotId: Int) {
        guard let slot = slots.first(where: { $0.id == slotId }),
              slot.kind != "unassigned" else { return }
        let sceneId = programSceneId == Self.soloSceneA ? Self.soloSceneB : Self.soloSceneA
        ensureSoloScene(sceneId)
        soloCueByScene[sceneId] = slot
        previewSceneId = sceneId
        // Always sync, even when the id did not change (a re-cue into the same
        // solo scene changes only its route — Windows SyncPreviewSceneChangeAsync).
        ShellLog.write("multiview cue → input \(slotId) (\(slot.name))")
        syncScenes()
    }

    func selectPreviewScene(_ sceneId: String) {
        // Mirrors StudioViewModel.SelectScene: selection ONLY arms preview —
        // program changes exclusively on Take.
        previewSceneId = sceneId
        syncScenes()
    }

    /// Session-only like Windows (`_takeTransitionMode`, not persisted).
    @Published var takeTransitionMode = "fade"
    @Published var takeStatus = ""
    @Published private(set) var takeInFlight = false
    private var takeRevision = 0

    enum TakeResult: Equatable {
        case success
        case failed(String)
    }

    /// Windows `NormalizeTakeTransitionMode`: cut/dip/wipe pass, anything else is fade.
    static func normalizeTakeTransitionMode(_ mode: String?) -> String {
        switch mode?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "cut": return "cut"
        case "dip": return "dip"
        case "wipe": return "wipe"
        default: return "fade"
        }
    }

    /// Windows `SetTakeTransition`: normalize, then announce.
    func setTakeTransition(_ mode: String?) {
        takeTransitionMode = Self.normalizeTakeTransitionMode(mode)
        takeStatus = "Take transition set to \(takeTransitionLabel)"
    }

    var takeTransitionLabel: String {
        switch takeTransitionMode {
        case "cut": return "Cut"
        case "dip": return "Dip"
        case "wipe": return "Wipe"
        default: return "Fade"
        }
    }

    /// Windows `CanTake` (StudioViewModel.cs:4275) minus the Shot Layout and
    /// draft terms Mac has no equivalent of, plus the coordinator's in-flight
    /// guard. Deliberately NOT gated on `quitFinishing`: Windows CanTake has no
    /// quit term (only Record/Stream do).
    var canTake: Bool {
        !takeInFlight
            && scenes.contains { $0.id == previewSceneId }
            && scenes.contains { $0.id == programSceneId }
            && previewSceneId != programSceneId
    }

    /// The one Take entry (button, and later keyboard / Companion): a port of
    /// `TransportCoordinator.TakeAsync`. Swaps Program/Preview, sends
    /// `begin-take-transition` first in the SAME media-core-sync batch as the
    /// scene graphs (no 120 ms debounce), and rolls both ids back when the core
    /// rejects or the request fails, unless the operator has edited them since
    /// (the #286 rule: newer edits are preserved, never clobbered).
    @discardableResult
    func take() async -> TakeResult {
        if takeInFlight { return .failed("A Take is already in progress.") }
        takeInFlight = true
        defer { takeInFlight = false }
        let previousProgram = programSceneId
        let taken = previewSceneId
        guard scenes.contains(where: { $0.id == taken }),
              scenes.contains(where: { $0.id == previousProgram }) else {
            let unavailable = "Take unavailable - select an available Preview scene."
            takeStatus = unavailable
            return .failed(unavailable)
        }
        guard taken != previousProgram else {
            let same = "Take unavailable - Preview is already on Program."
            takeStatus = same
            return .failed(same)
        }
        // Windows snapshots BOTH scenes' routes and refuses the rollback if either
        // changed while the Take was pending. Only solo scenes can be re-pointed
        // without an id change (a mid-Take multiview cue reuses the id not on the
        // new Program), so their captured cues are the routes to compare.
        let cuesAtTake = (soloCueByScene[taken], soloCueByScene[previousProgram])
        // The media selection follows the LOCAL swap, before the sync, and is put
        // back if the Take is rolled back (Windows TransportCoordinator, T1.3).
        let mediaBeforeTake = captureMediaSelection()
        let previousProgramRoutes = buildRoutes(for: previousProgram, isProgram: true)
        previewSceneId = previousProgram
        programSceneId = taken
        let attemptedProgramRoutes = buildRoutes(for: taken, isProgram: true)
        promoteMediaSelectionAfterLocalTake(previous: previousProgramRoutes, current: attemptedProgramRoutes)
        let mediaAfterTake = captureMediaSelection()
        sceneSyncTask?.cancel()           // the Take batch carries the full current state
        sceneSyncRequestCount += 1
        guard bridge != nil || quitRequestForTesting != nil else {
            let offline = "Program selected locally; Take was not sent because the media core is offline."
            takeStatus = offline
            return .failed(offline)
        }
        takeRevision += 1
        let transition: JSONObject = [
            "type": "begin-take-transition",
            "operationId": UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased(),
            "revision": takeRevision,
            "mode": Self.normalizeTakeTransitionMode(takeTransitionMode),
            "durationMs": 300,
            "direction": "left-to-right",
            "dipColor": "#000000",
        ]
        let batch = sceneSyncBatch(transition: transition)
        do {
            _ = try await outputRequest([
                "type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": batch,
            ])
            takeStatus = "Program updated"
            syncSpine()   // the buses swapped: the source set follows
            return .success
        } catch {
            let untouched = programSceneId == taken && previewSceneId == previousProgram
                && soloCueByScene[taken] == cuesAtTake.0
                && soloCueByScene[previousProgram] == cuesAtTake.1
            let failure: String
            if untouched {
                programSceneId = previousProgram
                previewSceneId = taken
                syncScenes()              // reconcile the core with the restored scenes
                // Only on a real scene rollback; a refused one leaves the selection alone.
                restoreMediaSelection(TakeMediaSelectionRollback.resolve(
                    beforeTake: mediaBeforeTake, afterTake: mediaAfterTake, current: captureMediaSelection(),
                    attemptedProgramRoutes: attemptedProgramRoutes,
                    restoredProgramRoutes: buildRoutes(for: previousProgram, isProgram: true),
                    rows: mediaSources))
                failure = "Take was not confirmed; previous local Program restored. "
                    + "Verify live output before retrying. \(error.localizedDescription)"
            } else {
                failure = "Take was not confirmed; newer local edits preserved. "
                    + "Verify live output. \(error.localizedDescription)"
            }
            takeStatus = failure
            pushWarning(failure)
            return .failed(failure)
        }
    }

    // ── Fade to Black (audit §2; Windows StudioViewModel.ProgramBlackout.cs) ──
    /// Core-owned state, observed from the snapshot (`programBlackout` /
    /// `programBlackoutOpacity`). No preference engages it; a new session starts clear.
    @Published private(set) var programBlackout = false
    @Published private(set) var programBlackoutLabel = "Fade to Black"
    private var blackoutInFlight = false

    enum BlackoutResult: Equatable {
        case success
        case failed(String)
    }

    /// Windows `ApplyProgramBlackoutSnapshot` label switch, case for case.
    static func programBlackoutLabel(blackout: Bool?, opacity: Double) -> String {
        switch blackout {
        case true? where opacity < 0.999: return "Fading to black"
        case true?: return "BLACK · Restore"
        case false? where opacity > 0.001: return "Restoring picture"
        default: return "Fade to Black"
        }
    }

    /// JSON booleans only (an int or string is "no observation", as with
    /// System.Text.Json's `bool?` on Windows).
    private static func strictBool(_ value: Any?) -> Bool? {
        guard let number = value as? NSNumber, CFGetTypeID(number) == CFBooleanGetTypeID() else { return nil }
        return number.boolValue
    }

    /// Proof-shot only (COREVIDEO_SHELL_FTB): keeps a live core's snapshots from clearing the shown state.
    private var blackoutProofHold = false

    private func applyProgramBlackout(_ snapshot: JSONObject) {
        if blackoutProofHold { return }
        let observed = Self.strictBool(snapshot["programBlackout"])
        let opacity = (snapshot["programBlackoutOpacity"] as? NSNumber)?.doubleValue ?? 0
        programBlackout = observed == true
        programBlackoutLabel = Self.programBlackoutLabel(blackout: observed, opacity: opacity)
    }

    /// Windows `SetProgramBlackoutAsync`: every caller (button, keyboard, later
    /// the control API) shares this admission and the native observation.
    /// Every refusal and failure is both returned and left in `takeStatus`, so a
    /// direct caller (the control API) can report it without the button wrapper.
    func setProgramBlackout(_ enabled: Bool?) async -> BlackoutResult {
        let result = await performProgramBlackout(enabled)
        if case .failed(let why) = result { takeStatus = why }
        return result
    }

    private func performProgramBlackout(_ enabled: Bool?) async -> BlackoutResult {
        if blackoutInFlight { return .failed("FTB command is already in progress.") }
        guard bridge != nil || quitRequestForTesting != nil else {
            return .failed("FTB unavailable: media core is offline.")
        }
        blackoutInFlight = true
        defer { blackoutInFlight = false }
        do {
            // Poll first: an older core without the observation rejects FTB.
            let polled = try await outputRequest([
                "type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": [JSONObject](),
            ])
            guard let current = Self.strictBool((polled["snapshot"] as? JSONObject)?["programBlackout"]) else {
                return .failed("This media core does not support Fade to Black.")
            }
            let target = enabled ?? !current
            let response = try await outputRequest([
                "type": "media-core-sync", "elapsedMs": elapsedMs(),
                "commands": [["type": "program-blackout", "enabled": target, "durationMs": 300] as JSONObject],
            ])
            let snapshot = response["snapshot"] as? JSONObject ?? [:]
            applySnapshot(snapshot)
            guard Self.strictBool(snapshot["programBlackout"]) == target else {
                return .failed("Media core did not acknowledge FTB.")
            }
            takeStatus = target ? "Fading Program to black — audio continues" : "Restoring Program picture"
            return .success
        } catch {
            return .failed("FTB unavailable: \(error.localizedDescription)")
        }
    }

    /// Windows `ToggleProgramBlackout` command: the button and Cmd+Shift+B.
    func toggleProgramBlackout() async {
        _ = await setProgramBlackout(nil)
    }

    // ── keyboard Take / FTB (audit §5) ───────────────────────────────────────
    let transportHotkeys = TransportHotkeyMonitor()

    /// Installed while the Studio or Graphics tab is active: transport keys act on
    /// Studio only (TransportKeys.decide checks), graphics F-keys on both (plan D7).
    func syncTransportHotkeys() {
        transportHotkeys.setActive(selectedTab == .studio || selectedTab == .overlays) { [weak self] nsEvent in
            guard let self else { return false }
            let key = TransportKeys.event(
                from: nsEvent, isStudioTab: self.selectedTab == .studio,
                firstResponder: nsEvent.window?.firstResponder)
            return self.handleGraphicsKey(key) || self.handleTransportKey(key)
        }
    }

    /// Graphics F-keys (GraphicsHotkeyRouter): Studio and Graphics tabs, never
    /// while a text field has focus, never under the quit prompt. A held key
    /// fires once (repeats are swallowed so a hold never re-Shows).
    func handleGraphicsKey(_ event: TransportKeyEvent) -> Bool {
        guard selectedTab == .studio || selectedTab == .overlays, !quitPromptVisible, !quitFinishing,
              case .function(let number) = event.kind,
              let route = GraphicsHotkeys.route(function: number, modifiers: event.modifiers,
                                                textFocus: event.isTextEntryFocus, rows: graphics.rows)
        else { return false }
        if event.isRepeat { return true }
        return graphics.perform(route, subjectSourceId: programLowerThirdSubject()?.source.sourceId)
    }

    /// Executes a routed key against the real commands; returns whether it was
    /// consumed. `takeOverride` lets tests observe a Take without a core.
    func handleTransportKey(_ event: TransportKeyEvent, takeOverride: (() -> Void)? = nil) -> Bool {
        func fireTake() {
            if let takeOverride { takeOverride() } else { Task { @MainActor in await self.take() } }
        }
        // The in-window quit prompt owns the keyboard: pass keys through so Space
        // can activate its focused button and nothing is Taken under it.
        if quitPromptVisible || quitFinishing { return false }
        switch TransportKeys.decide(event) {
        case .ignore: return false
        case .swallow: return true
        case .take:
            // Consumed even when Take is unavailable: Space must never activate
            // whatever control happened to hold focus.
            if canTake { fireTake() }
            return true
        case .alternateTake:
            guard canTake else { return false }
            fireTake()
            return true
        case .toggleBlackout:
            Task { @MainActor in await self.toggleProgramBlackout() }
            return true
        }
    }

    private var sceneSyncTask: Task<Void, Never>?
    private var spineSyncTask: Task<Void, Never>?

    // Coalesced: UI gestures (canvas drags, slot edits) can fire per frame,
    // and one full scene-graph request per delta starves the core's RPC queue
    // until every command times out — the owner hit exactly that. Callers stay
    // naive; the debounce is here.
    func syncScenes() {
        sceneSyncRequestCount += 1
        sceneSyncTask?.cancel()
        sceneSyncTask = Task { [weak self] in
            await self?.timerSleep(nanoseconds: 120_000_000)
            guard !Task.isCancelled else { return }
            await self?.pushScenes()
        }
    }

    /// The `background` node for a scene, or nil when it has none.
    ///
    /// The core enables a background only when BOTH mediaAssetId and
    /// mediaAssetPath are non-empty, and warns when an id arrives without a
    /// path — so an asset that has vanished from the bin must send nothing
    /// rather than a half-populated node that trips that warning every sync.
    func backgroundPayload(for sceneId: String) -> JSONObject? {
        guard let scene = scenes.first(where: { $0.id == sceneId }),
              !scene.backgroundAssetId.isEmpty,
              let asset = mediaAssets.first(where: { $0.id == scene.backgroundAssetId })
        else { return nil }
        return [
            "mediaAssetId": asset.id,
            "mediaAssetName": asset.name,
            "mediaAssetKind": asset.kind,
            "mediaAssetPath": asset.filePath,
        ]
    }

    func setSceneBackground(sceneId: String, assetId: String) {
        guard let index = scenes.firstIndex(where: { $0.id == sceneId }) else { return }
        scenes[index].backgroundAssetId = assetId
        syncScenes()
    }

    /// The scene-graph half of a sync. A Take prepends `begin-take-transition`
    /// (Windows BuildSyncCommands: the transition leads the batch).
    private func sceneSyncBatch(transition: JSONObject? = nil) -> [JSONObject] {
        var commands: [JSONObject] = []
        if let transition { commands.append(transition) }
        if !programSceneId.isEmpty {
            var graph: JSONObject = [
                "type": "load-scene-graph", "sceneId": programSceneId,
                "routes": buildRoutes(for: programSceneId, isProgram: true),
            ]
            if let background = backgroundPayload(for: programSceneId) {
                graph["background"] = background
            }
            if let tiles = tilesPayload(for: programSceneId) { graph["tiles"] = tiles }
            commands.append(graph)
        }
        if !previewSceneId.isEmpty {
            var preview: JSONObject = [
                "type": "set-preview-scene", "sceneId": previewSceneId,
                "routes": buildRoutes(for: previewSceneId),
            ]
            if let background = backgroundPayload(for: previewSceneId) {
                preview["background"] = background
            }
            if let tiles = tilesPayload(for: previewSceneId) { preview["tiles"] = tiles }
            commands.append(preview)
        }
        // The bin SELECTION rides every scene sync (Windows BuildSyncCommands), so
        // the core's mediaPlayback status follows it. Root level, not payload-wrapped:
        // MediaCore::setMediaPlayback reads the command root.
        if let selected = mediaAssets.first(where: { $0.id == selectedAssetId }) {
            commands.append([
                "type": "set-media-playback", "mediaAssetId": selected.id, "mediaAssetName": selected.name,
                "mediaAssetKind": selected.kind, "mediaAssetPath": selected.filePath,
            ])
        }
        return Self.appendSourcePolicies(to: commands, slots: slots)
    }

    private func pushScenes() async {
        guard bridge != nil || quitRequestForTesting != nil else { return }
        let batch = sceneSyncBatch()
        guard !batch.isEmpty else { return }
        do {
            _ = try await outputRequest([
                "type": "media-core-sync", "elapsedMs": elapsedMs(),
                "commands": batch,
            ])
        } catch {
            pushWarning("scene sync failed: \(error.localizedDescription)")
        }
        // A scene or bus change changes the source set (a wall's members, a
        // cued route): re-push the spine. Windows rebuilds it every 500 ms.
        syncSpine()
    }

    // ── auto-direct (the MagicScene policy ladder, reduced) ──────────────────

    // The core's director speaks its own scene ids; the shell owns the map to
    // its catalog (the core doesn't care which sceneId gets loaded).
    // Internal (not private): AutomationExtras.swift resolves the recommended
    // scene's name/layout through the same map.
    static let directorSceneMap = [
        "intro": "fullscreen", "interview": "two-up",
        "speaker-slides": "pip", "panel": "two-up",
    ]

    private var autoTimer: Timer?
    private var pendingAutoSceneId = ""
    private var pendingAutoSince = Date.distantPast

    func setAutoDirect(enabled: Bool) {
        autoDirectEnabled = enabled && Self.sceneAutomationAvailable
        autoTakeEnabled = autoTakeEnabled && Self.sceneAutomationAvailable
        pendingAutoSceneId = ""
        autoTimer?.invalidate()
        autoTimer = nil
        guard autoDirectEnabled else {
            autoStatus = "Scene automation is unavailable in this release"
            evaluateAutomationLowerThird()  // retires an automation-owned key
            return
        }
        autoTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in self.evaluateAutoDirect() }
        }
    }

    private func evaluateAutoDirect() {
        guard Self.sceneAutomationAvailable && autoDirectEnabled else { return }
        // Overlay policy runs BEFORE the roster/confidence guards — the
        // MagicSceneCoordinator.EvaluateAutomationPolicy order. It is a no-op
        // unless the resolved program source actually changed.
        evaluateAutomationLowerThird()
        guard roster.contains(where: \.hasVideo) else {
            autoStatus = "Waiting for meeting participants"
            return
        }
        guard autoConfidence >= Int(autoConfidenceThreshold) else {
            autoStatus = "Holding — confidence \(autoConfidence) below threshold"
            return
        }
        guard let target = Self.directorSceneMap[autoSceneId], !target.isEmpty else {
            autoStatus = "Director idle"
            return
        }
        if target == programSceneId {
            autoStatus = "On recommended scene (\(target))"
            pendingAutoSceneId = ""
            return
        }
        if target != pendingAutoSceneId {
            pendingAutoSceneId = target
            pendingAutoSince = Date()
        }
        let held = Date().timeIntervalSince(pendingAutoSince)
        guard held >= autoHoldSeconds else {
            autoStatus = String(format: "Switching to %@ in %.0fs…", target,
                                autoHoldSeconds - held)
            return
        }
        previewSceneId = target
        syncScenes()
        if autoTakeEnabled {
            autoStatus = "Taking \(target) (\(autoRuleId))…"
            Task { @MainActor in
                switch await self.take() {
                case .success: self.autoStatus = "Took \(target) (\(self.autoRuleId))"
                case .failed(let why): self.autoStatus = "Take of \(target) failed — \(why)"
                }
            }
        } else {
            autoStatus = "Queued \(target) on preview"
        }
        pendingAutoSceneId = ""
    }

    // ── routing matrix (video: ISO columns exclusive, multiview membership) ──

    // One source per ISO column (the Windows exclusive-destination rule).
    @Published var isoColumnAssignments: [Int: String] = [:]  // 1...8 → sourceId

    struct RoutingRow: Identifiable {
        let id: String       // routing sourceId (zoom:<pid> / capture:<id> / synthetic)
        let label: String
        let inMultiview: Bool
        let multiviewToggleable: Bool
    }

    var routingRows: [RoutingRow] {
        var rows: [RoutingRow] = []
        for slot in slots where slot.kind != "unassigned" {
            rows.append(RoutingRow(
                id: slot.sourceKey ?? slot.sourceId,
                label: "Input \(String(format: "%02d", slot.id)) · \(slot.name)",
                inMultiview: slot.inShow,
                multiviewToggleable: true))
        }
        rows.append(RoutingRow(id: "active-speaker", label: "Active Speaker",
                               inMultiview: false, multiviewToggleable: false))
        rows.append(RoutingRow(id: "screen-share", label: "Screen Share",
                               inMultiview: false, multiviewToggleable: false))
        rows.append(RoutingRow(id: "media", label: "Media",
                               inMultiview: false, multiviewToggleable: false))
        return rows
    }

    func isoColumn(of sourceId: String) -> Int? {
        isoColumnAssignments.first { $0.value == sourceId }?.key
    }

    func toggleIsoCell(column: Int, sourceId: String) {
        recordingIso.noteOperatorEdit()
        var selection = isoSelectedSourceIds
        if isoColumnAssignments[column] == sourceId {
            isoColumnAssignments[column] = nil
            selection.remove(sourceId)
        } else {
            // Exclusive per column AND one column per source: the column's
            // previous occupant leaves the ISO set; this source moves here.
            if let occupant = isoColumnAssignments[column] { selection.remove(occupant) }
            if let previous = isoColumn(of: sourceId) {
                isoColumnAssignments[previous] = nil
            }
            isoColumnAssignments[column] = sourceId
            selection.insert(sourceId)
        }
        // The selection is the authority; the didSet re-projects the slot flags
        // and normalises the columns (the same path every other writer takes).
        isoSelectedSourceIds = selection
        isoRecordingEnabled = !visibleIsoSelection.isEmpty
        syncSpine()   // a newly armed ISO guest is a source (S2 review)
    }

    /// The selected sources the operator can SEE (an assigned slot, so a
    /// Sources checkbox and a Routing row). The master switch follows this,
    /// not stale intent for sources that are no longer in a slot.
    var visibleIsoSelection: Set<String> {
        isoSelectedSourceIds.intersection(slots.compactMap(\.sourceKey))
    }

    /// Zoom participant ids are per meeting: once the meeting is over (or on a
    /// relaunch), `zoom:` intent for a guest who is in no slot can never
    /// resolve again, and only piles up. Capture ids are stable and stay.
    /// Windows never prunes, but its matrix does not draw the selection.
    func pruneStaleZoomIsoIntent() {
        let assigned = Set(slots.compactMap(\.sourceKey))
        let stale = isoSelectedSourceIds.filter { $0.hasPrefix("zoom:") && !assigned.contains($0) }
        if !stale.isEmpty { isoSelectedSourceIds.subtract(stale) }
    }

    /// Re-derive the projections of `isoSelectedSourceIds`: each slot's ISO
    /// flag, and the matrix columns (Windows VideoRoutingMatrixViewModel
    /// :130-157 keeps existing column choices across rebuilds and normalises).
    /// A selected source keeps its column; one without a column takes the
    /// lowest free one (sorted, so a relaunch draws the same matrix); a column
    /// whose source left the selection is cleared.
    func projectIsoSelection() {
        for index in slots.indices {
            let selected = slots[index].sourceKey.map(isoSelectedSourceIds.contains) ?? false
            if slots[index].iso != selected { slots[index].iso = selected }
        }
        // Columns only for sources with a Routing row (an assigned slot): stale
        // intent must not take ISO cells that draw empty and starve a new pick.
        let visible = visibleIsoSelection
        var columns = isoColumnAssignments.filter { visible.contains($0.value) }
        let placed = Set(columns.values)
        for source in visible.sorted() where !placed.contains(source) {
            guard let free = (1...8).first(where: { columns[$0] == nil }) else { break }
            columns[free] = source
        }
        if columns != isoColumnAssignments { isoColumnAssignments = columns }
    }

    func toggleMultiviewCell(sourceId: String) {
        let bare = sourceId.contains(":")
            ? String(sourceId.drop(while: { $0 != ":" }).dropFirst()) : sourceId
        if let slot = slots.first(where: { $0.sourceId == bare }) {
            toggleSlotInShow(slot.id)
        }
    }

    // ── ISO selection (IsoSourceSelectionResolver port) ──────────────────────

    // OFF → empty (program byte-identical to pre-ISO); drop departed sources;
    // cap 8. Zoom → zoom:<pid>, capture → capture:<id> (already scheme-qualified).
    // Command-send paths only: this MUTATES the take state. Views use
    // peekIsoSourceIds() instead.
    func resolvedIsoSourceIds(recording: Bool? = nil) -> [String] {
        isoResolve(&recordingIso, recording: recording)
    }

    // What resolve would send right now, without touching take state — safe
    // from a SwiftUI body.
    func peekIsoSourceIds(recording: Bool? = nil) -> [String] {
        var copy = recordingIso
        return isoResolve(&copy, recording: recording)
    }

    private func isoResolve(_ selection: inout RecordingIsoSelection,
                            recording: Bool?) -> [String] {
        // Windows ComputeEligiblePresentIsoSourceIds (ShowInputsCoordinator.cs
        // :334-365): only ASSIGNED, present sources are eligible, and a video-off
        // Zoom guest never takes one of the 8 ISO encoder slots. Unassign keeps
        // the source's ISO intent but removes it from what records.
        let present = Set(roster.filter(\.hasVideo).map { "zoom:" + $0.id })
            .union(captureDevices.filter { $0.connectionState == "connected" }
                .map { "capture:" + $0.id })
        let assigned = Set(slots.compactMap(\.sourceKey))
        // While a take is recording, passive roster loss never shrinks the set
        // (#1188); only an operator ISO edit does.
        return selection.resolve(
            recordingRequested: recording ?? isRecordingActive,
            enabled: isoRecordingEnabled, selected: isoSelectedSourceIds,
            eligible: present.intersection(assigned).sorted(), assigned: assigned)
    }

    func toggleIsoSource(_ sourceId: String) {
        recordingIso.noteOperatorEdit()
        if isoSelectedSourceIds.contains(sourceId) {
            isoSelectedSourceIds.remove(sourceId)
        } else {
            isoSelectedSourceIds.insert(sourceId)
        }
    }

    // ── streaming (RTMP via the core's ffmpeg sender) ────────────────────────

    /// The destination list as the wire sees it: `streamUrl` is the RTMP URL.
    var streamConfig: StreamDestinationsConfig {
        var config = streamDestinations
        config.rtmpUrl = streamUrl
        return config
    }

    var streamSecrets: StreamDestinationSecrets {
        StreamDestinationSecrets(rtmpKey: streamKey, srtPassphrase: srtPassphrase)
    }

    /// What is being streamed: the Start-time freeze while live, else the editor.
    var activeStreamConfig: StreamDestinationsConfig { liveStreamDestinations?.config ?? streamConfig }
    var activeStreamSecrets: StreamDestinationSecrets { liveStreamDestinations?.secrets ?? streamSecrets }

    /// "RTMP live, SRT reconnecting in 5 s": the footer's one-line summary of
    /// the rows of the destinations being streamed (projection labels only).
    var streamSummary: String {
        let ids = StreamDestinationPolicy.enabledDestinations(activeStreamConfig, activeStreamSecrets)
        let parts = ids.map { id -> String in
            let label = destinationRows[id]?.label ?? "Starting"
            return "\(StreamDestinationPolicy.displayName(id)) \(label.prefix(1).lowercased() + label.dropFirst())"
        }
        return parts.isEmpty ? "starting" : parts.joined(separator: ", ")
    }

    /// The footer Stream stat's health word, from the projected rows only:
    /// "Live" when every streamed destination is confirmed live, "Live 1/2"
    /// when some are, else the first row's own label. Never the published
    /// status, which can read "live" from a launch.
    var streamHealthLabel: String {
        let ids = StreamDestinationPolicy.enabledDestinations(activeStreamConfig, activeStreamSecrets)
        let rows = ids.map { destinationRows[$0] ?? DestinationStatus(label: "Starting", tone: .neutral) }
        guard !rows.isEmpty else { return "Starting" }
        let live = rows.filter(\.isLive).count
        if live == rows.count { return "Live" }
        if live > 0 { return "Live \(live)/\(rows.count)" }
        return rows.first { $0.tone == .bad || $0.tone == .warn }?.label ?? rows[0].label
    }

    /// Why Start streaming cannot start (Windows ValidateSelectedDestinations), or nil.
    var streamStartRefusal: String? { StreamDestinationPolicy.validateSelected(streamConfig, streamSecrets) }

    /// One row per destination from the core's sender records. The labels are
    /// decided from lifecycle + supervisor (never `status`). `streamStatus` /
    /// `streamDetail` keep feeding the quit guard and the control surface: the
    /// first stream sender (rtmp, srt, hls order) whose lifecycle is not
    /// terminal, so a live SRT-only stream still guards a careless quit.
    func applyStreamSenders(_ session: JSONObject) {
        let desired = Set(streamingDesired
                          ? StreamDestinationPolicy.enabledDestinations(activeStreamConfig, activeStreamSecrets) : [])
        var rows: [String: DestinationStatus] = [:]
        var senders: [JSONObject] = []
        for id in StreamDestinationPolicy.destinationIds {
            let sender = DestinationStatusProjection.sender(id, in: session)
            if let sender { senders.append(sender) }
            rows[id] = DestinationStatusProjection.project(sender, desired: desired.contains(id))
        }
        if rows != destinationRows { destinationRows = rows }
        let runtime = senders.compactMap { $0["runtimeDetail"] as? String }.first { !$0.isEmpty } ?? ""
        if runtime != streamRuntimeDetail { streamRuntimeDetail = runtime }
        // Warn, not fail closed (review default, recorded in the plan): an
        // unguarded ffmpeg still streams, but it can outlive a crashed core.
        let guards = senders.compactMap { $0["childGuard"] as? Bool }
        let guarded: Bool? = guards.isEmpty ? nil : !guards.contains(false)
        if guarded != streamChildGuard { streamChildGuard = guarded }
        if guarded == false && !warnedUnguardedChild {
            warnedUnguardedChild = true
            pushWarning("stream: ffmpeg is running without its child guard; "
                        + "if the media core crashes it can keep streaming. Reinstall CoreVideo Pro.")
        }
        func lifecycleState(_ sender: JSONObject) -> String {
            (sender["lifecycle"] as? JSONObject)?["state"] as? String ?? ""
        }
        let chosen = senders.first { !["completed", "idle"].contains(lifecycleState($0)) } ?? senders.first
        if let chosen {
            streamStatus = chosen["status"] as? String ?? ""
            let code = chosen["lastResultCode"] as? String ?? ""
            let warning = chosen["warning"] as? String ?? ""
            streamDetail = warning.isEmpty ? code : warning
        } else if !streamingDesired {
            streamStatus = ""
            streamDetail = ""
        }
    }

    /// Retry a destination the supervisor gave up on: the documented operator
    /// reset (`recover-output-sender`), which always clears give-up.
    func retryDestination(_ id: String) {
        guard StreamDestinationPolicy.destinationIds.contains(id) else { return }
        guard bridge != nil || quitRequestForTesting != nil else { return }
        Task {
            _ = try? await self.outputRequest([
                "type": "media-core-sync", "elapsedMs": elapsedMs(),
                "commands": [["type": "recover-output-sender", "destination": id,
                              "reason": "operator-retry"] as JSONObject],
            ])
        }
    }

    // start-program-output is FULL-STATE for the destination set, so record
    // and stream always resend the union of what should be live.
    func startProgramOutput(recording: Bool, streaming: Bool) -> JSONObject {
        var destinations: [String] = []
        if recording { destinations.append("recording") }
        if streaming {
            destinations += StreamDestinationPolicy.enabledDestinations(activeStreamConfig, activeStreamSecrets)
        }
        // N1b: NDI is its own output. It rides every full-state start while it is
        // on, whatever Record and Stream are doing (Windows parity).
        let ndiRows = NdiOutputPolicy.destinationSettings(ndiOutput, streamFps: ndiStreamFps)
        if !ndiRows.isEmpty { destinations.append("ndi") }
        let isoIds = resolvedIsoSourceIds(recording: recording)
        var command: JSONObject = [
            "type": "start-program-output", "destinations": destinations,
            "isoSourceIds": isoIds,
            "isoParticipantIds": isoIds.filter { $0.hasPrefix("zoom:") }
                .map { String($0.dropFirst(5)) },
        ]
        if streaming {
            command["streamOutputProfile"] = streamOutputProfile()
            // An explicit destination bitrate WINS over streamOutputProfile in the
            // core (#606), so every row carries the operator's value.
            // O1 (plan D6): the operator's advanced encoder settings overwrite the
            // RTMP and HLS rows' fields; Windows sends none of them on the SRT row
            // (StudioViewModel.StreamDestinations.cs), so neither does the Mac.
            command["destinationSettings"] = StreamDestinationPolicy.destinationSettings(
                activeStreamConfig, activeStreamSecrets, bitrateMbps: streamBitrateMbps)
                .map { ($0["id"] as? String) == "srt" ? $0 : streamEncoder.apply(to: $0) }
        }
        if !ndiRows.isEmpty {
            command["destinationSettings"] = ((command["destinationSettings"] as? [JSONObject]) ?? []) + ndiRows
        }
        // O1: Windows sends recordingOutputProfile on every start
        // (MediaCoreCommandBuilder.cs:743); without it the core records at its
        // built-in 1080p60 whatever the operator chose.
        if recording { command["recordingOutputProfile"] = recordingProfile.payload(fps: canvasFps) }
        return command
    }

    private var isRecordingActive: Bool {
        recordingDesired
    }

    func toggleStreaming() {
        guard bridge != nil || quitRequestForTesting != nil else { return }
        guard !quitFinishing else { return }   // outputs are closing (quit)
        if streamingDesired {
            streamingDesired = false
            // Review focus 2: stopping the stream must not stop NDI (or Record).
            // stop-encoder-session ends EVERY output, so it is sent only when
            // nothing else is left; otherwise the full-state start for what remains.
            let commands: [JSONObject] = isRecordingActive || ndiOutputActive
                ? [startProgramOutput(recording: isRecordingActive, streaming: false)]
                : [["type": "stop-encoder-session",
                    "reason": "Streaming stopped by operator."]]
            Task {
                _ = try? await self.outputRequest([
                    "type": "media-core-sync", "elapsedMs": elapsedMs(),
                    "commands": commands,
                ])
            }
            streamStatus = ""
            streamDetail = ""
            return
        }
        if let refusal = streamStartRefusal {
            pushWarning("stream: \(refusal)")
            return
        }
        streamSecretSaver(streamKey, "rtmp")
        streamSecretSaver(srtPassphrase, StreamKeychain.srtPassphraseAccount)
        streamingDesired = true
        Task {
            do {
                _ = try await outputRequest([
                    "type": "media-core-sync", "elapsedMs": elapsedMs(),
                    "commands": [startProgramOutput(recording: isRecordingActive,
                                                    streaming: true)],
                ])
            } catch {
                streamingDesired = false
                pushWarning("stream start failed: \(error.localizedDescription)")
            }
        }
    }

    // ── audio routing matrix + mastering rack ────────────────────────────────

    // The core's valid bus ids (MediaCore::isAudioRoutingBus). Rows are
    // sources, columns are buses; the command is FULL-STATE with a 25-sync
    // hold-last guard, so a partial send is worse than none.
    // A6: the 15 fixed buses plus the operator's custom buses (AudioBuses.swift).
    @Published private(set) var buses: [AudioBus] = AudioBusPolicy.fixedBuses
    private var customBusCounter = 1
    /// The LISTEN bus (exclusive; nil = none). Session state, re-sent with every
    /// base matrix so a respawned core hears it again (Windows, plan D3).
    @Published private(set) var listenBusId: String?
    var routingBusIds: [String] { buses.map(\.id) }

    @discardableResult
    func addCustomBus() -> AudioBus {
        let result = AudioBusPolicy.addCustom(buses, counter: customBusCounter)
        buses = result.buses
        customBusCounter = result.counter
        return result.added
    }

    /// Unroutes the bus's crosspoints through the operator-edit path, drops it
    /// (its bus sends go with it) and clears LISTEN if it was the listened bus.
    @discardableResult
    func removeBus(_ id: String) -> Bool {
        guard let next = AudioBusPolicy.remove(id, from: buses) else { return false }
        for (source, sendsByBus) in audioSends where sendsByBus[id] != nil {
            audioSends[source]?[id] = nil
            editRoute(source: source, bus: id, enabled: false, gainDb: 0)
        }
        for key in operatorRouteEdits.keys where key.bus == id { operatorRouteEdits[key] = nil }
        if selectedSend?.bus == id { selectedSend = nil }
        buses = next
        if listenBusId == id { listenBusId = nil }
        pushRoutingBase()
        return true
    }

    @discardableResult
    func renameBus(_ id: String, to label: String) -> Bool {
        guard let next = AudioBusPolicy.rename(id, to: label, in: buses) else { return false }
        buses = next
        return true
    }

    @discardableResult
    func setBusSend(_ id: String, target: String, enabled: Bool) -> Bool {
        guard let next = AudioBusPolicy.setSend(id, target: target, enabled: enabled, in: buses) else { return false }
        buses = next
        pushRoutingBase()
        return true
    }

    @discardableResult
    func setBusOutputGain(_ id: String, gainDb: Double) -> Bool {
        guard let next = AudioBusPolicy.setOutputGain(id, gainDb: gainDb, in: buses) else { return false }
        buses = next
        pushRoutingBase()
        return true
    }

    func setListening(_ id: String, on: Bool) {
        guard buses.contains(where: { $0.id == id }) else { return }
        listenBusId = AudioBusPolicy.listen(current: listenBusId, bus: id, on: on)
        pushRoutingBase()
    }

    /// Clear row: unroute one source from every bus (the grid's own removeSend).
    func clearRoutes(source: String) {
        for bus in (audioSends[source] ?? [:]).keys.sorted() { removeSend(source: source, bus: bus) }
    }

    /// Routed sources per bus (Windows RefreshBusRoutedCounts).
    func routedCount(bus: String) -> Int { audioSends.values.filter { $0[bus] != nil }.count }

    /// The base-matrix memo: the sends AND the bus state. Comparing only `sends`
    /// would swallow a bus-send or LISTEN change (nothing else moved).
    static func routingMemo(_ command: JSONObject?) -> [JSONObject]? {
        guard let command, let sends = command["sends"] as? [JSONObject] else { return nil }
        return sends + [["busSends": command["busSends"] ?? [], "monitorBusId": command["monitorBusId"] ?? ""]]
    }

    @Published var audioSends: [String: [String: Double]] = [:]  // source → bus → dB
    @Published var selectedSend: (source: String, bus: String)?
    @Published var mastering = MasteringParams()

    func sendGain(source: String, bus: String) -> Double? {
        audioSends[source]?[bus]
    }

    // Select-never-destroys (the B1 contract): tapping a routed cell selects
    // it for gain editing; removal is the explicit action.
    func selectOrRouteSend(source: String, bus: String) {
        if audioSends[source]?[bus] != nil {
            selectedSend = (source, bus)
            return
        }
        var busesForSource = audioSends[source] ?? [:]
        // ISO columns are exclusive: one source per ISO bus.
        if bus.hasPrefix("iso-") {
            for (otherSource, _) in audioSends where audioSends[otherSource]?[bus] != nil {
                audioSends[otherSource]?[bus] = nil
                editRoute(source: otherSource, bus: bus, enabled: false, gainDb: 0)
            }
        }
        busesForSource[bus] = 0
        audioSends[source] = busesForSource
        editRoute(source: source, bus: bus, enabled: true, gainDb: 0)
        audioRouteControlNotice = audioRouteControl.notice
        selectedSend = (source, bus)
        syncAudioRouting()
    }

    func removeSend(source: String, bus: String) {
        audioSends[source]?[bus] = nil
        editRoute(source: source, bus: bus, enabled: false, gainDb: 0)
        audioRouteControlNotice = audioRouteControl.notice
        if selectedSend?.source == source, selectedSend?.bus == bus { selectedSend = nil }
        syncAudioRouting()
    }

    func setSendGain(source: String, bus: String, gainDb: Double) {
        guard audioSends[source]?[bus] != nil else { return }
        audioSends[source]?[bus] = max(-60, min(10, gainDb))
        editRoute(source: source, bus: bus, enabled: true,
                               gainDb: max(-60, min(10, gainDb)))
        audioRouteControlNotice = audioRouteControl.notice
        syncAudioRouting()
    }

    /// The operator's own crosspoint edits, kept as INTENT so they ride the base
    /// matrix (AudioConsolePolicy.baseRoutingSends) and survive a core restart.
    private(set) var operatorRouteEdits: [AudioRouteKey: AudioRouteControlProjection.Draft] = [:]
    private(set) var lastRoutingBaseSent: [JSONObject]?

    private func editRoute(source: String, bus: String, enabled: Bool, gainDb: Double) {
        audioRouteControl.edit(source: source, bus: bus, enabled: enabled, gainDb: gainDb)
        operatorRouteEdits[AudioRouteKey(source: source, bus: bus)] =
            AudioRouteControlProjection.Draft(enabled: enabled, gainDb: gainDb)
        pushRoutingBase()
    }

    /// The base sync-audio-routing-matrix for the current show, or nil when it
    /// would be empty (never send an empty matrix: the core holds-last for 25
    /// syncs and an empty one reads as "unroute everything"). Internal: the
    /// harness checks it.
    func routingBaseCommand() -> JSONObject? {
        let sends = AudioConsolePolicy.baseRoutingSends(
            zoomAudioIds: zoomAudioSourceIds,
            captureAudioIds: consoleCaptureAudioIds,
            mediaRouted: sceneHasMediaRoute(programSceneId, isProgram: true),
            operatorEdits: operatorRouteEdits,
            zoomAudioMode: zoomAudioMode, meetingAudio: meetingAudioSubscribed)
        let busSends = AudioBusPolicy.busSends(buses)
        let monitorBusId = listenBusId ?? ""
        // Never an EMPTY matrix (the core holds-last on a blank). A matrix whose
        // sends are empty but that carries bus state is not empty: the core
        // applies busSends/monitorBusId from it when it holds no sends
        // (MediaCore.cpp:3245-3393), the nothing-routed case. It also goes out
        // once more after the last bus state is turned off, so the core never
        // keeps a LISTEN or bus send the operator cleared.
        if sends.isEmpty {
            let lastBusState = lastRoutingBaseSent?.last
            let coreMayHoldBusState = !((lastBusState?["busSends"] as? [JSONObject]) ?? []).isEmpty
                || !((lastBusState?["monitorBusId"] as? String) ?? "").isEmpty
            guard !busSends.isEmpty || !monitorBusId.isEmpty || coreMayHoldBusState else { return nil }
        }
        return ["type": "sync-audio-routing-matrix", "sends": sends,
                "busSends": busSends, "monitorBusId": monitorBusId]
    }

    private var routingBaseTask: Task<Void, Never>?
    /// Harness seam: receives the base-routing request instead of the bridge.
    var routingBaseSendObserver: ((JSONObject) -> Void)?

    /// The media-core-sync envelope for a base routing push. Opted in to the core
    /// mailbox's coalescing (CommandMailbox::replaceable: replaceableFullState +
    /// a non-empty coalescingKey, sync-audio-routing-matrix only), so a queued
    /// older matrix is REPLACED by a newer one instead of both occupying the
    /// 128-entry mailbox. A replaced request is answered `superseded: true`.
    func routingBaseEnvelope(_ command: JSONObject) -> JSONObject {
        ["type": "media-core-sync", "elapsedMs": elapsedMs(),
         "replaceableFullState": true, "coalescingKey": "audio-routing-base",
         "commands": [command]]
    }

    /// Sends the base matrix when it changed (or after a core restart reset the
    /// memo). DEBOUNCED latest-wins (review I3): the send-gain slider calls this on
    /// every tick, and an uncoalesced full matrix per tick could fill the core's
    /// 128-entry command mailbox mid-drag and get unrelated commands refused.
    /// One pending push; the matrix is computed when it fires.
    func pushRoutingBase() {
        routingBaseTask?.cancel()
        routingBaseTask = Task { [weak self] in
            await self?.timerSleep(nanoseconds: 100_000_000)
            guard !Task.isCancelled else { return }
            await self?.sendRoutingBaseNow()
        }
    }

    private func sendRoutingBaseNow() async {
        guard let command = routingBaseCommand() else { return }
        let sends = Self.routingMemo(command) ?? []
        if let last = lastRoutingBaseSent, NSArray(array: last).isEqual(to: sends) { return }
        let envelope = routingBaseEnvelope(command)
        if let observer = routingBaseSendObserver {
            lastRoutingBaseSent = sends
            observer(envelope)
            return
        }
        guard let bridge else { return }
        lastRoutingBaseSent = sends
        do {
            _ = try await bridge.request(envelope)
        } catch {
            lastRoutingBaseSent = nil  // not delivered: the next push retries
            pushWarning("audio routing sync failed: \(error.localizedDescription)")
        }
    }

    private var routingSyncTask: Task<Void, Never>?

    func syncAudioRouting() {
        routingSyncTask?.cancel()
        routingSyncTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 150_000_000)
            guard !Task.isCancelled else { return }
            await self?.pushAudioRouting()
        }
    }

    private func pushAudioRouting() async {
        guard let bridge else { return }
        if audioRouteControl.authorityEpoch.isEmpty {
            if let response = try? await bridge.request([
                "type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": [],
            ]), let snapshot = response["snapshot"] as? JSONObject {
                applySnapshot(snapshot)
            }
        }
        if !audioRouteControl.authorityEpoch.isEmpty {
            while let command = audioRouteControl.nextCommand() {
                do {
                    let response = try await bridge.request([
                        "type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": [command],
                    ])
                    if let snapshot = response["snapshot"] as? JSONObject { applySnapshot(snapshot) }
                } catch {
                    if let response = try? await bridge.request([
                        "type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": [],
                    ]), let snapshot = response["snapshot"] as? JSONObject {
                        applySnapshot(snapshot)
                    }
                }
                if audioRouteControl.pendingOperationId != nil {
                    audioRouteControl.markUnconfirmed()
                    audioRouteControlNotice = audioRouteControl.notice
                    break
                }
                if audioRouteControl.notice.contains("conflicted") { break }
            }
            return
        }
        var sends: [JSONObject] = []
        for (source, buses) in audioSends {
            for (bus, gainDb) in buses {
                sends.append([
                    "sourceId": source, "busId": bus,
                    "gainDb": max(-60, min(10, gainDb)),
                    "busPluginInserts": [] as [String],
                ])
            }
        }
        // Never send an empty list: the core holds-last for 25 syncs and an
        // empty matrix during a UI rebuild would read as "unroute everything".
        guard !sends.isEmpty else { return }
        do {
            _ = try await bridge.request([
                "type": "media-core-sync", "elapsedMs": elapsedMs(),
                "commands": [[
                    "type": "sync-audio-routing-matrix", "sends": sends,
                    "busSends": AudioBusPolicy.busSends(buses), "monitorBusId": listenBusId ?? "",
                ]],
            ])
        } catch {
            pushWarning("audio routing sync failed: \(error.localizedDescription)")
        }
    }

    // Hydrate the grid from the CORE's sends (never from client defaults —
    // the ghost-model lesson in docs/audio-tab-redesign.md).
    private func applyRoutingSnapshot(_ matrix: JSONObject) {
        guard audioRouteControl.observe(matrix) else { return }
        audioRouteControlNotice = audioRouteControl.notice
        guard let sends = matrix["sends"] as? [JSONObject],
              !sends.isEmpty || !audioRouteControl.authorityEpoch.isEmpty else { return }
        var next: [String: [String: Double]] = [:]
        for entry in sends {
            guard let source = entry["sourceId"] as? String,
                  let bus = entry["busId"] as? String else { continue }
            next[source, default: [:]][bus] =
                (entry["gainDb"] as? NSNumber)?.doubleValue ?? 0
        }
        audioSends = audioRouteControl.overlayDrafts(on: next)
    }

    /// audio.route.set (Windows ApplyAudioRouteControlAsync): the cell edit the
    /// grid makes (audioSends, operator intent, the ISO-exclusive rule), sent as a
    /// revisioned command pinned to the caller's epoch, revision and operation id,
    /// and answered from the core's result for that operation.
    func requestAudioRouteControl(source: String, bus: String, enabled: Bool, gainDb: Double,
                                  expectedEpoch: String, expectedRevision: Int64,
                                  operationId: String) async -> AudioRouteControlOutcome {
        guard bridge != nil || quitRequestForTesting != nil else { return .unconfirmed }
        if audioRouteControl.authorityEpoch.isEmpty,
           let snapshot = (try? await monitorRequest(["type": "media-core-sync", "elapsedMs": elapsedMs(),
                                                      "commands": [JSONObject]()]))?["snapshot"] as? JSONObject {
            applySnapshot(snapshot)
        }
        guard !audioRouteControl.authorityEpoch.isEmpty else { return .unconfirmed }
        let control = audioRouteControl
        if expectedEpoch != control.authorityEpoch { return .conflict(revision: control.revision) }
        let key = AudioRouteKey(source: source, bus: bus)
        let target = AudioRouteControlProjection.Draft(enabled: enabled, gainDb: gainDb)
        // Nothing to change: answer from the core's authority instead of waiting
        // for a result no command will produce.
        if control.pendingOperationId == nil, control.drafts[key] == nil,
           control.applied[key] == target || (!enabled && control.applied[key] == nil) {
            return expectedRevision == control.revision ? .applied : .conflict(revision: control.revision)
        }
        audioRouteControl.remoteEdit(source: source, bus: bus, enabled: enabled, gainDb: gainDb,
                                     expectedEpoch: expectedEpoch, expectedRevision: expectedRevision,
                                     operationId: operationId)
        audioRouteControlNotice = audioRouteControl.notice
        while let command = audioRouteControl.nextCommand() {
            let reply = (try? await monitorRequest(["type": "media-core-sync", "elapsedMs": elapsedMs(),
                                                    "commands": [command]])) ?? [:]
            if let snapshot = reply["snapshot"] as? JSONObject { applySnapshot(snapshot) }
            if audioRouteControl.pendingOperationId != nil {
                audioRouteControl.markUnconfirmed()
                audioRouteControlNotice = audioRouteControl.notice
                break
            }
            if audioRouteControl.outcome(of: operationId) != nil { break }
        }
        _ = await settle { audioRouteControl.outcome(of: operationId) != nil }
        let outcome = audioRouteControl.outcome(of: operationId) ?? .unconfirmed
        switch outcome {
        case .conflict, .rejected:
            // The core refused: change nothing (Windows). The grid, the operator
            // intent and the unrevisioned base matrix are never written, and the
            // remote draft is dropped so the next snapshot cannot resurface it.
            // A snapshot during the exchange overlaid the draft onto the grid
            // (applyRoutingSnapshot): put the cell back to what the core applied.
            audioRouteControl.discardRemoteDraft(source: source, bus: bus, operationId: operationId)
            if audioRouteControl.drafts[key] == nil {
                if let applied = audioRouteControl.applied[key] { audioSends[source, default: [:]][bus] = applied.gainDb }
                else { audioSends[source]?[bus] = nil }
            }
            audioRouteControlNotice = audioRouteControl.notice
        case .applied, .unconfirmed:
            // Applied, or possibly applied (no answer in the settle window): the
            // grid's own edits (selectOrRouteSend / setSendGain / removeSend), so
            // the base matrix carries it and a respawned core gets it back.
            if enabled, bus.hasPrefix("iso-") {
                for (other, buses) in audioSends where other != source && buses[bus] != nil {
                    audioSends[other]?[bus] = nil
                    editRoute(source: other, bus: bus, enabled: false, gainDb: 0)
                }
            }
            if enabled { audioSends[source, default: [:]][bus] = gainDb } else { audioSends[source]?[bus] = nil }
            operatorRouteEdits[key] = target
            pushRoutingBase()
        }
        return outcome
    }

    func editMastering(_ apply: (inout MasteringParams) -> Void) {
        apply(&mastering)
        scheduleMixSync()
    }

    // ── mastering presets + A/B compare (A3; Windows SVM:3241-3443) ──────────

    /// "A" or "B". Both slots start as the Neutral starting point (SVM:66-68).
    @Published private(set) var masteringCompareSlot = "A"
    @Published private(set) var masteringCompareA = MasteringPresetCatalog.neutral
    @Published private(set) var masteringCompareB = MasteringPresetCatalog.neutral
    @Published private(set) var masteringUserPresets: [MasteringUserPreset] = []
    /// The Windows status sentence for the last rack action.
    @Published var masteringStatus = ""

    /// Windows ApplyMasteringSettings: the whole rack, stored into the ACTIVE
    /// slot, one sync. Slider edits (editMastering) deliberately do not touch
    /// the slot; currentPrefs() refreshes it from the live rack (SVM:12155-12160).
    private func applyMasteringSettings(_ settings: MasteringParams) {
        mastering = settings
        if masteringCompareSlot == "B" { masteringCompareB = settings } else { masteringCompareA = settings }
        scheduleMixSync()
        persistPrefsNow()
    }

    /// Windows SelectMasteringCompareSlot: anything but "B" is "A"; the slot
    /// being left keeps the live rack; the other slot's rack is recalled.
    func selectMasteringCompareSlot(_ requested: String) {
        let slot = requested.trimmingCharacters(in: .whitespacesAndNewlines).uppercased() == "B" ? "B" : "A"
        guard slot != masteringCompareSlot else { return }
        if masteringCompareSlot == "B" { masteringCompareB = mastering } else { masteringCompareA = mastering }
        masteringCompareSlot = slot
        applyMasteringSettings(slot == "B" ? masteringCompareB : masteringCompareA)
        masteringStatus = "Master processor recalled comparison \(slot)."
    }

    private static func titleCase(_ id: String) -> String {
        let t = id.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        return t.prefix(1).uppercased() + t.dropFirst()
    }

    @discardableResult
    func applyMasteringPreset(_ presetId: String) -> Bool {
        guard let settings = MasteringPresetCatalog.create(presetId) else { return false }
        applyMasteringSettings(settings)
        masteringStatus = "\(Self.titleCase(presetId)) mastering starting point applied to comparison \(masteringCompareSlot)."
        return true
    }

    @discardableResult
    func resetMasteringStage(_ stage: String) -> Bool {
        guard let settings = MasteringPresetCatalog.resetStage(mastering, stage: stage) else { return false }
        applyMasteringSettings(settings)
        masteringStatus = "\(Self.titleCase(stage)) stage reset to neutral."
        return true
    }

    @discardableResult
    func saveMasteringUserPreset(name: String) -> MasteringUserPreset? {
        guard let result = MasteringPresetLibrary.save(masteringUserPresets, name: name, settings: mastering) else {
            masteringStatus = MasteringPresetLibrary.isBuiltInName(name)
                ? "That name belongs to a built-in starting point — pick another."
                : "Preset not saved — enter a short name (built-in names are reserved)."
            return nil
        }
        masteringUserPresets = result.presets
        masteringStatus = "Master preset \"\(result.saved.name)\" saved."
        persistPrefsNow()
        return result.saved
    }

    @discardableResult
    func applyMasteringUserPreset(id: String?) -> Bool {
        guard let preset = masteringUserPresets.first(where: { $0.id == id }) else {
            masteringStatus = "Select a saved master preset first."
            return false
        }
        applyMasteringSettings(preset.settings)
        masteringStatus = "Master preset \"\(preset.name)\" applied to comparison \(masteringCompareSlot)."
        return true
    }

    @discardableResult
    func renameMasteringUserPreset(id: String?, to name: String) -> Bool {
        guard let result = MasteringPresetLibrary.rename(masteringUserPresets, id: id, to: name) else {
            masteringStatus = "Rename failed — the new name is empty, reserved, or already in use."
            return false
        }
        masteringUserPresets = result.presets
        masteringStatus = "Master preset renamed to \"\(result.renamed.name)\"."
        persistPrefsNow()
        return true
    }

    @discardableResult
    func deleteMasteringUserPreset(id: String?) -> Bool {
        guard let result = MasteringPresetLibrary.delete(masteringUserPresets, id: id) else {
            masteringStatus = "Select a saved master preset first."
            return false
        }
        masteringUserPresets = result.presets
        masteringStatus = "Master preset \"\(result.deleted.name)\" deleted."
        persistPrefsNow()
        return true
    }

    /// The rack's slice of ShellPrefs (Windows SVM:12155-12163): the active
    /// slot is serialised from the live rack.
    func storeMastering(into prefs: inout ShellPrefs) {
        prefs.masteringCurrent = mastering
        prefs.masteringCompareA = masteringCompareSlot == "A" ? mastering : masteringCompareA
        prefs.masteringCompareB = masteringCompareSlot == "B" ? mastering : masteringCompareB
        prefs.masteringCompareSlot = masteringCompareSlot
        prefs.masteringUserPresets = masteringUserPresets.isEmpty ? nil : masteringUserPresets
    }

    /// Windows RestoreMasteringFromPreferences (SVM:12513-12578). Runs inside
    /// applyRestoredPrefs, before any core connects, so the FIRST mix command a
    /// core receives carries the saved rack. No sync here.
    func restoreMastering(from prefs: ShellPrefs) {
        masteringUserPresets = MasteringPresetLibrary.loaded(prefs.masteringUserPresets)
        guard let current = prefs.masteringCurrent else { return }
        let live = MasteringPresetLibrary.clamped(current)
        masteringCompareSlot = prefs.masteringCompareSlot?.uppercased() == "B" ? "B" : "A"
        masteringCompareA = MasteringPresetLibrary.clamped(prefs.masteringCompareA ?? current)
        masteringCompareB = MasteringPresetLibrary.clamped(prefs.masteringCompareB ?? current)
        mastering = live
    }

    // ── media bin + logo bug ─────────────────────────────────────────────────

    /// Publishes the list at once, then fills durations from an off-main probe:
    /// one write per refresh, never per frame.
    func refreshMediaBin() {
        let known = Dictionary(mediaAssets.compactMap { asset in asset.durationMs.map { (asset.filePath, $0) } },
                               uniquingKeysWith: { first, _ in first })
        mediaAssets = MediaBin.loadAssets().map { asset in
            var asset = asset
            asset.durationMs = known[asset.filePath]
            return asset
        }
        // A media slot follows its asset: online when the bin lists it, offline (never
        // substituted) when it left. The wall and scenes re-sync only when that changed.
        let mediaSlotsBefore = slots.filter { $0.kind == "media" }
        refreshSlotHealth()
        if slots.filter({ $0.kind == "media" }) != mediaSlotsBefore { recomputeFromSlots() }
        mediaDurationProbeGeneration += 1
        let generation = mediaDurationProbeGeneration
        let unprobed = mediaAssets.filter { $0.supportsPlayback && $0.durationMs == nil }
        guard !unprobed.isEmpty else { return }
        Task.detached { [weak self] in
            let durations = await MediaBin.probeDurations(unprobed)
            await self?.applyProbedDurations(durations, generation: generation)
        }
    }

    private func applyProbedDurations(_ durations: [String: Int], generation: Int) {
        // A newer refresh owns the list now; its own probe will fill it.
        guard generation == mediaDurationProbeGeneration, !durations.isEmpty else { return }
        let updated = mediaAssets.map { asset -> MediaAsset in
            guard asset.durationMs == nil, let ms = durations[asset.id] else { return asset }
            var asset = asset
            asset.durationMs = ms
            return asset
        }
        if updated != mediaAssets { mediaAssets = updated }
    }

    /// Import status is Windows' wording, verbatim (StudioViewModel.ImportMediaAsync).
    /// An empty list is the cancelled open panel.
    func importMedia(urls: [URL]) {
        guard !urls.isEmpty else {
            mediaStatus = "Media import canceled"
            return
        }
        let result = MediaBin.importFiles(urls)
        refreshMediaBin()
        guard let first = result.imported.first else {
            mediaStatus = "No supported media files imported"
            return
        }
        selectedAssetId = first.id
        mediaStatus = result.imported.count == 1
            ? "\(first.name) imported and selected"
            : "\(result.imported.count) media assets imported"
    }

    /// Windows SelectMediaAsset: a selection only. It never pauses or plays anything.
    func selectMediaAsset(_ id: String) {
        guard let asset = mediaAssets.first(where: { $0.id == id }) else { return }
        selectedAssetId = asset.id
        // The flag is the REAL on-air state, so selecting a rolling Program clip
        // reads "Pause Program" and never pauses it.
        selectedMediaPlaying = asset.supportsPlayback && MediaRoutePlayback.isPlayingOnAir(asset.id, rows: mediaSources)
        mediaStatus = "\(asset.name) ready to cue or audition"
        syncScenes()   // the selection rides the batch as set-media-playback
    }

    // ── the media selection across a Take ────────────────────────────────────

    /// A clip that ENTERS Program becomes the selection, reading playing (the core
    /// rolls it from 0); a still never does. If nothing entered and the selected
    /// clip left Program, it stops reading playing (Windows
    /// PromoteProgramMediaRouteToPlayback / RefreshMediaBinPlaybackIndicators).
    private func promoteMediaSelectionAfterLocalTake(previous: [JSONObject], current: [JSONObject]) {
        let entering = MediaRoutePlayback.assetsEnteringProgram(previous: previous, current: current)
        let promoted = MediaRoutePlayback.chooseAssetToPromote(
            entering, selected: selectedAssetId.isEmpty ? nil : selectedAssetId,
            supportsPlayback: { [mediaAssets] id in mediaAssets.first { $0.id == id }?.supportsPlayback == true })
        if let promoted, let asset = mediaAssets.first(where: { $0.id == promoted }) {
            selectedAssetId = asset.id
            selectedMediaPlaying = true
            mediaStatus = "Playing \(asset.name) on Program"
            return
        }
        guard Set(MediaRoutePlayback.programMediaAssetIds(previous)) != Set(MediaRoutePlayback.programMediaAssetIds(current)),
              MediaRoutePlayback.selectedAssetLeftProgram(selectedAssetId, previous: previous, current: current)
        else { return }
        selectedMediaPlaying = false
        mediaStatus = selectedMediaAsset.map { "\($0.name) left Program" } ?? "No media asset playing"
    }

    func captureMediaSelection() -> MediaSelectionState {
        let asset = selectedMediaAsset
        return MediaSelectionState(assetId: asset?.id, name: asset?.name,
                                   supportsPlayback: asset?.supportsPlayback ?? false,
                                   playing: selectedMediaPlaying, status: mediaStatus)
    }

    func restoreMediaSelection(_ state: MediaSelectionState) {
        selectedAssetId = state.assetId ?? ""
        selectedMediaPlaying = state.playing
        mediaStatus = state.status
    }

    // ── Play / Pause (Windows PlayMediaAsset / ToggleSelectedMediaPlayback) ──

    private func mediaAssetIsOnProgram(_ id: String) -> Bool {
        MediaRoutePlayback.isRoutedOnProgram(id, programRoutes: buildRoutes(for: programSceneId, isProgram: true))
    }

    var mediaPlaybackButtonLabel: String {
        MediaRoutePlayback.actionLabel(isOnProgram: mediaAssetIsOnProgram(selectedAssetId), playing: selectedMediaPlaying)
    }

    func toggleSelectedMediaPlayback() {
        guard !selectedAssetId.isEmpty else {
            mediaStatus = "Select a media asset before playback"
            return
        }
        playMediaAsset(selectedAssetId)
    }

    /// A tap on a clip. On Program it pauses or resumes ON AIR through a one-shot
    /// gesture; off Program it toggles the muted local audition and sends nothing.
    func playMediaAsset(_ id: String) {
        guard let asset = mediaAssets.first(where: { $0.id == id }) else { return }
        let sameAsset = selectedAssetId == asset.id
        let isOnProgram = mediaAssetIsOnProgram(asset.id)
        let tap = MediaRoutePlayback.resolveTap(
            isOnProgram: isOnProgram, isLooping: MediaRoutePlayback.isLoopingKind(asset.kind),
            state: MediaRoutePlayback.resolveTransportState(asset.id, rows: mediaSources))
        selectedAssetId = asset.id
        switch tap {
        case .pause:
            sendMediaTransportGesture(asset.id, action: "pause")
            selectedMediaPlaying = false
        case .resume:
            sendMediaTransportGesture(asset.id, action: "play")
            selectedMediaPlaying = true
        case .select:
            selectedMediaPlaying = !(sameAsset && selectedMediaPlaying)
        }
        mediaStatus = isOnProgram
            ? (selectedMediaPlaying ? "Playing \(asset.name) on Program" : "\(asset.name) paused on Program")
            : (selectedMediaPlaying ? "Auditioning \(asset.name)" : "\(asset.name) audition paused")
        syncScenes()   // the selection rides the batch; the gesture never does
    }

    private func mediaAssetLabel(_ id: String) -> String {
        mediaAssets.first { $0.id == id }?.name ?? id
    }

    /// ONE-SHOT: never part of sceneSyncBatch, never re-sent on a core respawn.
    private func sendMediaTransportGesture(_ id: String, action: String) {
        let token = mediaTransportGestures.claim(id)
        attemptMediaTransportGesture(id, action: action, token: token, attempt: 0)
    }

    private func attemptMediaTransportGesture(_ id: String, action: String, token: Int64, attempt: Int) {
        // Asked again before EVERY send: a re-armed retry must not land after a newer tap.
        guard mediaTransportGestures.shouldSend(id, token) else { return }
        let body = CoreCommands.mediaCoreSync(elapsedMs: Double(elapsedMs()), commands: [
            ["type": "set-media-transport", "mediaAssetId": id, "action": action],
        ])
        Task { @MainActor [weak self] in
            guard let self else { return }
            do {
                let response: JSONObject
                if let seam = self.mediaTransportSendForTesting { response = try await seam(body) }
                else { response = try await self.outputRequest(body) }
                self.mediaTransportGestures.release(id, token)
                if let snapshot = response["snapshot"] as? JSONObject { self.applySnapshot(snapshot) }
            } catch BridgeError.remote(let message) {
                // The core answered and said no: a refusal, not backpressure. Never retried.
                if self.mediaTransportGestures.release(id, token) {
                    self.mediaStatus = "Could not \(action) \(self.mediaAssetLabel(id)); "
                        + "the media core rejected it. \(message)"
                }
            } catch {
                // .timeout / .notRunning / .processExited: the Mac's "skipped" (plan default D8).
                switch self.mediaTransportGestures.onSkipped(id, token, attempt: attempt) {
                case .superseded:
                    break
                case .retry:
                    await self.timerSleep(nanoseconds: Self.mediaTransportRetryDelayNs)
                    self.attemptMediaTransportGesture(id, action: action, token: token, attempt: attempt + 1)
                case .giveUp:
                    ShellLog.write("media: \(action) for \(id) was skipped \(attempt + 1) times; giving up")
                    self.mediaStatus = "Could not \(action) \(self.mediaAssetLabel(id)) - "
                        + "the media core stayed busy. Press it again."
                }
            }
        }
    }

    // ── Cue in Preview (Windows AddSelectedMediaAssetToPreview) ─────────────

    private var selectedMediaAsset: MediaAsset? {
        selectedAssetId.isEmpty ? nil : mediaAssets.first { $0.id == selectedAssetId }
    }

    /// Why Cue would refuse right now, or nil when it would add the layer.
    /// Status strings are Windows' unless marked Mac.
    var cueRefusalReason: String? {
        guard let asset = selectedMediaAsset else { return "Select a media asset before adding it to Preview" }
        // The solo check comes BEFORE the scene lookup: a solo scene is not in `scenes`.
        guard !Self.isSoloScene(previewSceneId), scenes.contains(where: { $0.id == previewSceneId }) else {
            return "Cue in Preview needs an operator scene in Preview."            // Mac
        }
        let fileExtension = (asset.filePath as NSString).pathExtension.lowercased()
        if MediaBin.audioExtensions.contains(fileExtension) {
            // Mac: the decoder has no picture for an audio-only file, so the layer
            // would sit on Program with no frame (audio-only assets on air are N3).
            return "\(asset.name) is audio only; audio-only assets cannot be cued on Mac yet."
        }
        if asset.supportsPlayback {
            guard mediaClipPlaybackAvailable else { return "Clip playback is unavailable in this build" }   // Mac
            if (asset.filePath as NSString).pathExtension.lowercased() == "webm" { return MediaRoutePlayback.webmRefusal }
        }
        if MediaRoutePlayback.isRoutedOnProgram(asset.id, programRoutes: buildRoutes(for: previewSceneId)) {
            return "\(asset.name) is already cued in Preview"
        }
        return nil
    }

    var canCueSelectedMedia: Bool { cueRefusalReason == nil }

    /// Adds the selection to the Preview scene as a full-canvas media layer on top
    /// (Windows BuildAddedSourceRoute: full canvas, FitMode "fill", appended).
    /// Returns the status it set.
    @discardableResult
    func cueSelectedInPreview() -> String {
        if let refusal = cueRefusalReason {
            mediaStatus = refusal
            return refusal
        }
        guard let asset = selectedMediaAsset,
              let index = scenes.firstIndex(where: { $0.id == previewSceneId }) else { return mediaStatus }
        let sceneId = previewSceneId
        if isTilesScene(sceneId) {   // the wall owns the scene (D5): no layer is added
            mediaStatus = Self.tilesLayersRefusal
            return mediaStatus
        }
        ensureLayers(for: sceneId)
        var free = 0
        while scenes[index].layers.contains(where: { $0.id == "\(sceneId)-layer-\(free)" }) { free += 1 }
        scenes[index].layers.append(SceneLayer(
            id: "\(sceneId)-layer-\(free)", slotId: nil, x: 0, y: 0, width: 1, height: 1,
            fitMode: "fill", opacity: 1, mediaAssetId: asset.id))
        syncScenes()
        mediaStatus = asset.isStillImage
            ? "\(asset.name) cued in Preview; Take to show it on Program"
            : "\(asset.name) cued in Preview; Take to start playback on Program"
        return mediaStatus
    }

    /// The card's one-line summary of the selection (Windows SelectedMediaAssetSummary).
    var selectedMediaSummary: String {
        guard let asset = selectedMediaAsset else {
            return MediaRoutePlayback.selectedSummary(name: nil, isOnProgram: false, isCuedInPreview: false, playing: false)
        }
        return MediaRoutePlayback.selectedSummary(
            name: asset.name,
            isOnProgram: MediaRoutePlayback.isRoutedOnProgram(
                asset.id, programRoutes: buildRoutes(for: programSceneId, isProgram: true)),
            isCuedInPreview: MediaRoutePlayback.isRoutedOnProgram(asset.id, programRoutes: buildRoutes(for: previewSceneId)),
            playing: selectedMediaPlaying)
    }

    /// Every assignment is change-gated: snapshots arrive several times a second
    /// and an unconditional @Published write would invalidate the Media tab at
    /// snapshot rate.
    private func applyMediaReadModels(_ snapshot: JSONObject) {
        if let rows = MediaSourceRow.parse(snapshot), rows != mediaSources { mediaSources = rows }
        if let states = (snapshot["profile"] as? JSONObject)?["capabilityStates"] as? JSONObject {
            let available = (states["media-clip-playback"] as? JSONObject)?["state"] as? String == "available"
            if available != mediaClipPlaybackAvailable { mediaClipPlaybackAvailable = available }
        }
        if let playback = MediaPlaybackReadModel.parse(snapshot), playback != mediaPlayback { mediaPlayback = playback }
        // The core's rows own the selected flag while the clip is on Program.
        let selected = selectedMediaAsset
        let projection = MediaBinPlaybackProjection.resolve(
            rows: mediaSources, lastLiveSignature: mediaLiveSignature,
            lastSelectedTransportState: lastSelectedTransportState,
            selectedId: selected?.id, selectedName: selected?.name, selectedPlaying: selectedMediaPlaying)
        if projection.rewriteBinScalars { mediaLiveSignature = projection.liveSignature }
        lastSelectedTransportState = projection.selectedTransportState
        if projection.rewriteSelection {
            if projection.selectedPlaying != selectedMediaPlaying { selectedMediaPlaying = projection.selectedPlaying }
            if let status = projection.selectedStatus, status != mediaStatus { mediaStatus = status }
        }
        // A refused gesture: the core says why in programFrame.warnings
        // (MediaTransportPolicy.h: "... is a loop ..." / "... is not on Program ...").
        if let selected, let warnings = (snapshot["programFrame"] as? JSONObject)?["warnings"] as? [String] {
            let prefixes = ["media:\(selected.id) is a loop", "media:\(selected.id) is not on Program"]
            // Shown ONCE, when it appears: the core keeps the reason in its warnings
            // until the next scene load, and re-showing it on every snapshot would
            // pin the status line and bury everything the operator does next.
            let refusal = warnings.first(where: { warning in prefixes.contains { warning.hasPrefix($0) } })
            if refusal != lastMediaRefusalSeen {
                lastMediaRefusalSeen = refusal
                if let refusal { mediaStatus = refusal }
            }
        }
    }

    /// Everything that still uses an asset, in `scenes` order, a scene's layers
    /// before its background, then the logo bug.
    func mediaAssetReferences(_ id: String) -> [String] {
        var references: [String] = []
        for scene in scenes {
            if scene.layers.contains(where: { $0.mediaAssetId == id }) { references.append("scene \(scene.name)") }
            if scene.backgroundAssetId == id { references.append("scene \(scene.name) background") }
        }
        if logoBug?.id == id { references.append("logo bug") }
        for slot in slots where slot.kind == "media" && slot.sourceId == id { references.append("input \(slot.id)") }
        return references
    }

    /// Windows RemoveMediaAsset: refused while anything uses the asset (every
    /// reference named); otherwise the file moves to the bin's .trash folder.
    func removeMediaAsset(_ id: String) {
        guard let asset = mediaAssets.first(where: { $0.id == id }) else { return }
        let references = mediaAssetReferences(id)
        guard references.isEmpty else {
            mediaStatus = "'\(asset.name)' is in use (\(references.joined(separator: ", "))). "
                + "Unassign it there before removing."
            ShellLog.write("lifecycle: media remove blocked \(id) - referenced by "
                           + references.joined(separator: "; "))
            return
        }
        guard MediaBin.moveToTrash(asset) else {
            mediaStatus = "Could not remove '\(asset.name)'."
            return
        }
        if selectedAssetId == id { selectedAssetId = "" }
        refreshMediaBin()
        mediaStatus = "Removed '\(asset.name)' (recoverable from the media .trash folder)."
    }

    func toggleLogoBug(_ asset: MediaAsset) {
        logoBug = logoBug?.id == asset.id ? nil : asset
        if programSceneId.isEmpty { programSceneId = scenes[0].id }
        syncScenes()
    }

    // ── lower third ──────────────────────────────────────────────────────────

    /// Key in: names the source the core reports on rendered Program, as the
    /// Windows Key in does (LegacyLowerThirdCommands.cs:7-45). The core's
    /// on-Program gate drops a lower third whose sourceId is not a Program
    /// layer, so the old "mac:manual" key was never drawn (ProgramLowerThird.swift).
    func showLowerThird() {
        guard let subject = programLowerThirdSubject() else {
            lowerThirdCommandStatus = LowerThirdKeyReadout.needsSource
            return
        }
        lowerThirdKeyedSourceId = subject.source.sourceId
        lowerThirdKeyedName = subject.name
        lowerThirdCommandStatus = "Lower third keyed in"
        cancelPendingLowerThirdRetire()
        // building-in only — the core auto-advances to on-air itself.
        sendOverlay([
            "type": "set-overlay-asset", "overlayId": "key:lower-third",
            "text": subject.name, "position": "lower-third", "enabled": true,
            "sourceId": subject.source.sourceId, "sourceName": subject.name,
            "title": lowerThirdTitle, "org": "",
            "keyPosition": lowerThirdPosition, "keyPhase": "building-in",
            "buildInMs": overlays.buildInMs, "buildOutMs": overlays.buildOutMs,
            "keyer": "downstream",
        ])
    }

    /// A show (or a newer retire) supersedes a retire whose hidden ack has not
    /// been sent yet: that ack would otherwise land on the freshly shown key.
    func cancelPendingLowerThirdRetire() {
        lowerThirdRetireGeneration += 1
        lowerThirdRetireInFlight = false
    }

    func hideLowerThird() {
        // Idempotent while a retire is in flight: a second building-out would
        // restart the native build-out and a second ack would race the first.
        guard !lowerThirdRetireInFlight else { return }
        lowerThirdRetireInFlight = true
        lowerThirdRetireGeneration += 1
        let generation = lowerThirdRetireGeneration
        lowerThirdCommandStatus = "Lower third keyed out"
        // Two-step retire: shell-timed build-out, then the hidden ack (an
        // enabled:false WITHOUT keyPhase:"hidden" would start a second native
        // build-out — the WinUI RunLowerThirdKeyOutAsync shape).
        // The build-out keeps the keyed source's id, or the on-Program gate
        // would cut the key instead of animating it out.
        sendOverlay([
            "type": "set-overlay-asset", "overlayId": "key:lower-third",
            "text": lowerThirdKeyedName, "position": "lower-third", "enabled": true,
            "sourceId": lowerThirdKeyedSourceId, "sourceName": lowerThirdKeyedName,
            "title": lowerThirdTitle, "org": "",
            "keyPosition": lowerThirdPosition, "keyPhase": "building-out",
            "buildInMs": overlays.buildInMs, "buildOutMs": overlays.buildOutMs,
            "keyer": "downstream",
        ])
        // Wait the operator's OWN build-out time (plus a little) before the
        // retire ack, or a long build-out is cut off mid-animation.
        let settle = lowerThirdSettleOverrideNs
            ?? UInt64(max(50, overlays.buildOutMs) + 40) * 1_000_000
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: settle)
            // Skip the ack if a show or a newer retire moved the generation.
            guard let self, self.lowerThirdRetireGeneration == generation else { return }
            self.lowerThirdRetireInFlight = false
            self.sendOverlay([
                "type": "set-overlay-asset", "overlayId": "key:lower-third",
                "text": "", "position": "lower-third", "enabled": false,
                "keyPhase": "hidden", "buildInMs": 250, "buildOutMs": 200,
                "keyer": "downstream",  // retire ack: timing is irrelevant here
            ])
        }
    }

    /// Build out then straight back in, so a style or timing change becomes
    /// visible without the operator toggling the key twice.
    func rebuildLowerThird() {
        hideLowerThird()
        let settle = UInt64(max(50, overlays.buildOutMs) + 120) * 1_000_000
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: settle)
            await MainActor.run { self?.showLowerThird() }
        }
    }

    // Internal (not private): AutomationExtras.swift keys the automatic lower
    // third through the same single-command path.
    func sendOverlay(_ command: JSONObject) {
        overlayCommandObserver?(command)
        guard let bridge else { return }
        Task {
            do {
                _ = try await bridge.request([
                    "type": "media-core-sync", "elapsedMs": elapsedMs(),
                    "commands": [command],
                ])
            } catch {
                pushWarning("overlay command failed: \(error.localizedDescription)")
            }
        }
    }

    // ── audio console ────────────────────────────────────────────────────────

    // Local edits win over the in-flight snapshot for 2s (the WinUI shell's
    // quiet period) — without it every fader drag visibly snaps back.
    private var stripEditTimes: [String: Date] = [:]
    private var mixSyncTask: Task<Void, Never>?

    private var lastMixEcho: [JSONObject] = []
    /// The strip ids last sent in a sync-participant-audio-mix (nil = never sent
    /// to this core generation).
    private(set) var lastSyncedStripIds: Set<String>?
    /// The strip set a channel sync is scheduled for (nil = none pending).
    private(set) var scheduledStripIds: Set<String>?

    private func applyMixParticipants(_ participants: [JSONObject]) {
        lastMixEcho = participants
        refreshStrips()
    }

    /// The console's expected sources (AudioConsolePolicy.expectedSourceIds) from
    /// the shell's own model: the subscribed Zoom audio set, paired capture mics,
    /// the media strip.
    var expectedAudioSourceIds: [String] {
        AudioConsolePolicy.expectedSourceIds(
            meetingAudio: meetingAudioSubscribed,
            zoomAudioIds: AudioConsolePolicy.rosterOrdered(
                zoomAudioSourceIds, roster: roster.map(\.id)),
            captureAudioIds: consoleCaptureAudioIds,
            mediaStrip: sceneHasMediaRoute(programSceneId, isProgram: true)
                || sceneHasMediaRoute(previewSceneId, isProgram: false))
    }

    /// Capture-kind console sources: every capture slot with a paired mic, then
    /// the local audio source while it is configured (A5; Windows
    /// BuildExpectedAudioSourceIds adds local-machine-audio after the captures).
    var consoleCaptureAudioIds: [String] {
        slots.filter { $0.kind == "capture" && !$0.sourceId.isEmpty && !$0.audioDeviceId.isEmpty }
            .map { "capture:" + $0.sourceId }
            + (LocalAudioPolicy.isConfigured(localAudio, devices: audioInputs) ? [LocalAudioPolicy.sourceId] : [])
    }

    /// Whether a bus's scene carries a FIXED media route — read off the real route
    /// wire (buildRoutes), the same thing the core is sent.
    func sceneHasMediaRoute(_ sceneId: String, isProgram: Bool) -> Bool {
        guard !sceneId.isEmpty, scenes.contains(where: { $0.id == sceneId }) else { return false }
        return buildRoutes(for: sceneId, isProgram: isProgram).contains {
            ($0["mode"] as? String) == "fixed" && !(($0["mediaAssetId"] as? String) ?? "").isEmpty
        }
    }

    /// A bus scene or the logo bug changed: the media strip and its default sends
    /// follow (review M3).
    private func mediaAudioSceneChanged(_ changed: Bool) {
        guard changed else { return }
        refreshStrips()
        pushRoutingBase()
    }

    /// Operator-facing name for an audio source id (Windows labels: "Zoom program
    /// mix", "Media playback", the guest's roster name, the slot name).
    func audioSourceLabel(_ id: String) -> String {
        switch id {
        case AudioConsolePolicy.meetingMixId: return "Zoom program mix"
        case AudioConsolePolicy.mediaId: return "Media playback"
        case "local-machine-audio": return "Local machine audio"
        default:
            let bare = id.hasPrefix("capture:") ? String(id.dropFirst("capture:".count)) : id
            if let slot = slots.first(where: { $0.sourceId == bare && !$0.name.isEmpty }) {
                return slot.name
            }
            return roster.first { $0.id == id }?.name ?? id
        }
    }

    /// Harness seam: records the console as delivered to the current core, as a
    /// successful sync would (the harness has no bridge).
    func markAudioConsoleDeliveredForTesting() {
        lastSyncedStripIds = Set(strips.map(\.id))
        scheduledStripIds = nil
        lastRoutingBaseSent = Self.routingMemo(routingBaseCommand())
    }

    /// A ONE-SHOT COMMAND MUST BE RE-APPLIED ON EVERY CORE GENERATION (CLAUDE.md).
    /// The core starts with no channels, no mastering and no routing; the Mac kept
    /// showing the A1's mutes, faders, inserts and routes while the respawned core
    /// played everything at unity — and its echoed "unmuted" then overwrote the
    /// A1's mutes. Forget what this core was sent and send the whole console again:
    /// every strip with its mute/gain/pan/solo/inserts + mastering, and the base
    /// routing matrix with the operator's crosspoint edits.
    ///
    /// ONE request, mix FIRST (review M6): the respawned core has no channels, and
    /// with no channels the FADER LAW does not apply — every routed source plays at
    /// unity. Sending the routing now and the mix 150 ms later (the debounce) made
    /// A1-muted sources audible for that window. The mix, the base routing and the
    /// capture-mic pairings (the only thing that can bring PCM into a fresh core
    /// at this point — its Zoom engine is new and not yet in a meeting, and scenes
    /// with media are synced after this) go together, in that order; the core
    /// applies a request's commands in order and its mailbox is FIFO.
    func reassertAudioConsole() {
        mixSyncTask?.cancel()
        routingBaseTask?.cancel()
        lastSyncedStripIds = nil
        scheduledStripIds = nil
        lastRoutingBaseSent = nil
        strips = AudioConsolePolicy.mergeStrips(
            echo: [], prior: strips, remembered: rememberedStripSettings,
            expected: expectedAudioSourceIds, zoomMuted: zoomMutedIds)
        var commands: [JSONObject] = [audioMixCommand()]
        let routing = routingBaseCommand()
        if let routing { commands.append(routing) }
        commands.append(captureAudioSourcesCommand())
        lastSyncedStripIds = Set(strips.map(\.id))
        lastRoutingBaseSent = Self.routingMemo(routing)
        if let observer = audioReassertObserver { observer(commands); return }
        guard let bridge else { return }
        Task { [weak self] in
            do {
                _ = try await bridge.request([
                    "type": "media-core-sync", "elapsedMs": self?.elapsedMs() ?? 0,
                    "commands": commands,
                ])
            } catch {
                self?.lastSyncedStripIds = nil
                self?.lastRoutingBaseSent = nil
                self?.refreshStrips()
                self?.pushRoutingBase()
            }
        }
    }

    /// Harness seam: receives the restart re-assert batch instead of the bridge.
    var audioReassertObserver: (([JSONObject]) -> Void)?

    /// The A1's per-id strip settings for the current meeting (review I2). Cleared
    /// for Zoom ids when the meeting ends: Zoom reuses user ids across meetings,
    /// and a reused id must never inherit another person's mute or fader.
    private(set) var rememberedStripSettings: [String: AudioStrip] = [:]

    /// The meeting is KNOWN to be over. ONLY terminal states clear per-guest audio
    /// memory (round-2 review): ZoomEngineRuntimeState reports idle (initial, and
    /// after the engine's "left" — which the macOS engine sends for both SDK
    /// Disconnecting and Ended, i.e. Leave and meeting-ended), joining, in-meeting
    /// (published as "in_meeting") and error (auth/join failure, meeting_failed, or
    /// engine_disconnected — the engine lost; a mid-meeting error never demotes
    /// the state). The SDK adapter's names add ended / leaving / status-N.
    /// "joining", "leaving", "status-N" (waiting room, breakout moves, reconnects)
    /// and anything unknown are NOT terminal: wiping the A1's mutes there would
    /// put guests back live at unity mid-meeting — the I2 failure.
    static let terminalMeetingStates: Set<String> = ["idle", "ended", "error", "left", "failed"]

    static func meetingHasEnded(_ state: String) -> Bool {
        terminalMeetingStates.contains(state)
    }

    /// The meeting ended (a KNOWN not-in-meeting state; an unknown state never
    /// clears — the Windows ResetMixerMeetingSession rule). Forgets every Zoom
    /// participant's remembered strip settings and route edits, and drops the
    /// meeting's sources from the console now rather than at the next spine push.
    /// Non-Zoom sources (zoom-mix, capture mics, media) keep their settings.
    func endMeetingAudioSession() {
        tilesAudioLatch.clear()   // Zoom reuses per-meeting ids: a latch must not outlive the meeting
        let isZoom = AudioConsolePolicy.isZoomParticipantChannel
        let hasState = !zoomAudioSourceIds.isEmpty || meetingAudioSubscribed
            || rememberedStripSettings.keys.contains(where: isZoom)
            || operatorRouteEdits.keys.contains { isZoom($0.source) }
            || strips.contains { isZoom($0.id) }
        guard hasState else { return }
        rememberedStripSettings = rememberedStripSettings.filter { !isZoom($0.key) }
        operatorRouteEdits = operatorRouteEdits.filter { !isZoom($0.key.source) }
        zoomAudioSourceIds = []
        meetingAudioSubscribed = false
        strips.removeAll { isZoom($0.id) }
        refreshStrips()
        pushRoutingBase()
    }

    /// Rebuilds the strips from the shell's source set + the last echo, and — the
    /// FADER LAW — syncs the full channel list whenever the strip SET changes, so
    /// the core never holds a routed source without a strip (a late guest, a newly
    /// paired mic) and never keeps a strip for a source that is gone.
    /// Internal: the harness drives it.
    /// Who is muted IN ZOOM right now, from the roster only (a roster miss is
    /// not a mute). Feeds AudioStrip.sourceMuted, never the A1's mute.
    var zoomMutedIds: Set<String> { Set(roster.filter(\.muted).map(\.id)) }

    func refreshStrips() {
        strips = AudioConsolePolicy.mergeStrips(
            echo: lastMixEcho, prior: strips, remembered: rememberedStripSettings,
            expected: expectedAudioSourceIds, zoomMuted: zoomMutedIds)
        for strip in strips { rememberedStripSettings[strip.id] = strip }
        // Schedule once per new SET (order is presentation, never a reason to
        // sync): snapshots arrive faster than the 150 ms debounce, and
        // re-scheduling on every one would cancel the sync forever.
        let ids = Set(strips.map(\.id))
        if ids != lastSyncedStripIds && ids != scheduledStripIds {
            scheduledStripIds = ids
            scheduleMixSync()
        }
    }

    func editInserts(_ id: String, _ apply: (inout ChannelInserts) -> Void) {
        var inserts = channelInserts[id] ?? ChannelInserts()
        apply(&inserts)
        channelInserts[id] = inserts
        stripEditTimes[id] = Date()
        scheduleMixSync()
    }

    func editStrip(_ id: String, _ apply: (inout AudioStrip) -> Void) {
        guard let index = strips.firstIndex(where: { $0.id == id }) else { return }
        apply(&strips[index])
        rememberedStripSettings[id] = strips[index]
        stripEditTimes[id] = Date()
        scheduleMixSync()
    }

    // Debounced FULL-channel emit: sync-participant-audio-mix is full-state
    // with a 25-sync hold-last guard core-side; never send a partial list.
    // Internal (not private): VstInserts.swift schedules the same debounced
    // full-channel emit when a VST insert selection changes.
    func scheduleMixSync() {
        mixSyncTask?.cancel()
        mixSyncTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 150_000_000)
            guard !Task.isCancelled else { return }
            await self?.syncAudioMix()
        }
    }

    /// The full-state sync-participant-audio-mix for the current console. Internal:
    /// the harness checks it carries the A1's state for every strip.
    func audioMixCommand() -> JSONObject {
        let channels: [JSONObject] = strips.map { strip in
            let inserts = channelInserts[strip.id] ?? ChannelInserts()
            return [
                "participantId": strip.id,
                "muted": strip.muted,
                "manualGainDb": max(-24, min(24, strip.manualGainDb)),
                "pan": max(-1, min(1, strip.pan)),
                "solo": strip.solo,
                "noiseSuppression": false,
                "pluginInserts": pluginInsertNames(for: strip.id),
                "insertSettings": inserts.settingsJson,
            ]
        }
        return ["type": "sync-participant-audio-mix", "limiterEnabled": mastering.limiterEnabled,
                "mastering": mastering.json, "channels": channels]
    }

    private func syncAudioMix() async {
        scheduledStripIds = nil
        guard let bridge, !strips.isEmpty else { return }
        lastSyncedStripIds = Set(strips.map(\.id))
        do {
            _ = try await bridge.request([
                "type": "media-core-sync", "elapsedMs": elapsedMs(),
                "commands": [audioMixCommand()],
            ])
        } catch {
            lastSyncedStripIds = nil  // not delivered: the next refresh retries
            pushWarning("audio mix sync failed: \(error.localizedDescription)")
        }
    }

    // ── quit guard ───────────────────────────────────────────────────────────

    var quitOutputs: QuitOutputs {
        QuitOutputs(recordingStatus: recordingStatus, recordingDesired: recordingDesired,
                    streamingDesired: streamingDesired, streamStatus: streamStatus)
    }

    /// NSApplicationDelegate.applicationShouldTerminate. Idle quits at once;
    /// live outputs defer the quit behind the in-app confirmation.
    func handleQuitRequest() -> NSApplication.TerminateReply {
        sourceGrades.flush()
        // The 5 s autosave would lose anything changed just before quit (a
        // mastering slider, an A/B pick): write the operator's prefs now.
        persistPrefsNow()
        switch quitGate.request(quitOutputs) {
        case .quitNow:
            // In a meeting, the Comms release-all goes out first (bounded), so a
            // held key never outlives the shell. Otherwise nothing can be held.
            guard comms.inMeeting || graphics.socialReviewIfStarted?.isConnected == true, !quitCoreGone else {
                return .terminateNow
            }
            Task { @MainActor in
                await self.sendCommsReleaseAllBounded()
                self.replyToQuit(true)
            }
            return .terminateLater
        case .ask:
            quitPromptVisible = true
            presentQuitPrompt()
            return .terminateLater
        case .ignore:
            // A repeat quit while asking or finishing re-presents what is
            // pending, so the operator always has something visible to answer.
            presentQuitPrompt()
            return .terminateCancel
        }
    }

    /// Shows the pending prompt (or the finishing notice) where it can be seen:
    /// the main RootView window when it is on screen, otherwise a standalone
    /// design-system panel. Never `windows.first` (a pop-out, or nothing).
    private func presentQuitPrompt() {
        let available = mainWindowAvailableForTesting ?? MainWindowTracker.isAvailable
        let placement = QuitPromptPresenterPolicy.placement(mainWindowAvailable: available)
        if let seam = quitPresentForTesting { seam(placement); return }
        guard quitReplyForTesting == nil else { return }
        NSApplication.shared.activate(ignoringOtherApps: true)
        switch placement {
        case .mainWindow: MainWindowTracker.bringForward()
        case .fallbackPanel: QuitPromptPanel.show(model: self)
        }
    }

    /// "Keep running".
    func cancelQuit() {
        guard quitPromptVisible else { return }
        quitGate.cancel()
        quitPromptVisible = false
        replyToQuit(false)
    }

    /// "Stop and quit": stop streams, then the recording, wait (bounded by ONE
    /// deadline) for the truthful terminal lifecycle, then retire the core with
    /// the stdin-EOF grace.
    func confirmQuit() {
        guard quitGate.confirm() else { return }
        quitFinishing = true        // before the prompt flips off: the panel stays up
        quitPromptVisible = false
        let recordingWasLive = QuitGuardPolicy.recordingLive(quitOutputs)
        let streamWasLive = QuitGuardPolicy.streamLive(quitOutputs)
        ShellLog.write("quit: stopping outputs recording=\(recordingWasLive) stream=\(streamWasLive)")
        let timeout = quitFinishTimeout
        Task { @MainActor in
            // Comms first (Windows ShutdownComms), before any stop or core exit.
            if self.comms.inMeeting || self.graphics.socialReviewIfStarted?.isConnected == true, !self.quitCoreGone {
                await self.sendCommsReleaseAllBounded()
            }
            var baseline = Int.max   // set when the stop lands; only later snapshots count
            let outcome = await QuitFlow.finishOutputs(
                stop: { @MainActor [weak self] remaining in
                    guard let self else { return true }
                    let landed = await self.sendQuitStops(recording: recordingWasLive,
                                                          stream: streamWasLive,
                                                          budget: remaining)
                    if landed { baseline = self.snapshotSequence }
                    return landed
                },
                isFinished: { @MainActor [weak self] remaining in
                    guard let self else { return true }
                    // A missing or exited core can write nothing more: terminal
                    // (Windows treats core exit as the end of the wait).
                    if self.quitCoreGone { return true }
                    // Poll a snapshot directly. The 0.5 s syncTimer is default-mode
                    // only and does not fire while AppKit holds a .terminateLater.
                    do {
                        let response = try await self.quitRequest(
                            ["type": "media-core-sync", "elapsedMs": self.elapsedMs(),
                             "commands": []], timeout: min(2, max(0.1, remaining)))
                        if let snapshot = response["snapshot"] as? JSONObject {
                            self.applySnapshot(snapshot)
                        }
                    } catch {
                        if self.quitCoreGone { return true }
                        if case BridgeError.notRunning = error { return true }
                        if case BridgeError.processExited = error { return true }
                    }
                    guard self.snapshotSequence > baseline else { return false }
                    let recordingDone = !recordingWasLive
                        || QuitGuardPolicy.recordingTerminal(self.recordingStatus)
                    let streamDone = !streamWasLive
                        || !QuitGuardPolicy.streamLive(
                            QuitOutputs(streamStatus: self.streamStatus))
                    return recordingDone && streamDone
                },
                timeout: timeout, poll: quitPollInterval)
            if outcome == .timedOut {
                let message = "quit: outputs did not finish within "
                    + "\(String(format: "%g", timeout)) s; quitting anyway"
                ShellLog.write(message)
                pushWarning(message)
            } else {
                ShellLog.write("quit: outputs finished")
            }
            // Stop the control server before the core goes away, not only at willTerminate.
            controlApi.stop()
            let exited = await bridge?.stopForAppExit(grace: quitCoreGrace) ?? true
            ShellLog.write("quit: core \(exited ? "exited on EOF" : "terminated after grace")")
            quitFinishing = false
            quitGate.reset()
            replyToQuit(true)
        }
    }

    private var quitCoreGone: Bool {
        if quitRequestForTesting != nil { return quitCoreGoneForTesting }
        if bridge == nil { return true }
        switch status {
        case .exited, .failed: return true
        default: return false
        }
    }

    /// The grade store's request path: the test seam, else the bridge.
    func gradeRequest(_ body: JSONObject) async throws -> JSONObject {
        if let seam = quitRequestForTesting { return try await seam(body, 6) }
        guard let bridge else { throw BridgeError.notRunning }
        return try await bridge.request(body)
    }

    private func quitRequest(_ body: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        if let seam = quitRequestForTesting { return try await seam(body, timeout) }
        guard let bridge else { throw BridgeError.notRunning }
        return try await bridge.request(body, timeout: timeout)
    }

    /// The operator's Record/Stream buttons go through here so the harness can
    /// drive the REAL start/stop paths (intent bookkeeping included) through the
    /// same seam the quit flow uses. Production: the bridge, default timeout.
    func outputRequest(_ body: JSONObject) async throws -> JSONObject {
        if let seam = quitRequestForTesting { return try await seam(body, 6) }
        guard let bridge else { throw BridgeError.notRunning }
        return try await bridge.request(body)
    }

    /// Test seam: feed a snapshot through the real read path.
    func applySnapshotForTesting(_ snapshot: JSONObject) { applySnapshot(snapshot) }
    func handleEventForTesting(_ event: JSONObject) { handleEvent(event) }
    /// Test seam: feed a zoom-snapshot through the real read path.
    func applyZoomForTesting(_ snapshot: JSONObject) { applyZoom(snapshot) }

    private func replyToQuit(_ shouldTerminate: Bool) {
        if let seam = quitReplyForTesting { seam(shouldTerminate); return }
        NSApplication.shared.reply(toApplicationShouldTerminate: shouldTerminate)
    }

    /// The same transport stops the operator's buttons send: streams first, then
    /// the recording session, each bounded by what is left of the quit deadline.
    /// True when every needed stop was accepted.
    private func sendQuitStops(recording: Bool, stream: Bool, budget: TimeInterval) async -> Bool {
        if quitCoreGone { return true }   // no core: nothing left to stop
        let deadline = Date().addingTimeInterval(budget)
        func left(_ cap: TimeInterval) -> TimeInterval { min(cap, max(0.1, deadline.timeIntervalSinceNow)) }
        var ok = true
        if stream {
            streamingDesired = false
            // Keep recording in the destination set ONLY while the operator still
            // wants it (toggleStreaming's rule). A stop already in flight
            // (finalizing/stopping, intent off) must NOT be re-requested: the
            // core no longer owns it and would open a NEW session that nothing
            // ever stops (final review CRITICAL 1).
            let commands: [JSONObject] = recording && recordingDesired
                ? [startProgramOutput(recording: true, streaming: false)]
                : [["type": "stop-encoder-session", "reason": "Streaming stopped on quit."]]
            do {
                _ = try await quitRequest([
                    "type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": commands,
                ], timeout: left(15))
            } catch { ok = false }
        }
        // Already stopping/finalizing: the stop landed on an earlier attempt.
        let recordingInFlight = ["stopping", "finalizing"].contains(recordingStatus)
        if recording, !recordingInFlight {
            let operation = recordingCommands.beginStop()
            recordingDesired = recordingCommands.desired
            do {
                _ = try await quitRequest([
                    "type": "media-core-sync", "elapsedMs": elapsedMs(),
                    "commands": [["type": "stop-recording-session", "reason": "Quit."]],
                ], timeout: left(30))
                recordingCommands.finish(operation, failed: false)
            } catch {
                recordingCommands.finish(operation, failed: true)
                ok = false
            }
            recordingDesired = recordingCommands.desired
        }
        return ok
    }

    func toggleRecording() {
        guard bridge != nil || quitRequestForTesting != nil else { return }
        // Windows CanSetRecording: no Record/Stop while outputs are closing.
        guard !quitFinishing else { return }
        let operation = recordingCommands.begin()
        let stop = operation.stop
        recordingDesired = recordingCommands.desired
        Task {
            do {
                if stop {
                    _ = try await outputRequest([
                        "type": "media-core-sync", "elapsedMs": elapsedMs(),
                        "commands": [["type": "stop-recording-session"]],
                    ])
                } else {
                    // Arm the encoder ONLY here: start-program-output flips the
                    // core into per-tick full readbacks (by design, for the
                    // encoder feed) — armed-at-startup starved the RPC thread.
                    // The operator's folder, or the default when it is gone
                    // (unplugged drive) or unwritable — loudly, never silently.
                    let folder = recordingTargetFolder()
                    // The core refuses (loudly) rather than mkdir -p'ing.
                    try? FileManager.default.createDirectory(
                        atPath: folder, withIntermediateDirectories: true)
                    // Always send the iso keys (empty array = disarm): omitting
                    // them PRESERVES the previous selection core-side.
                    let isoIds = resolvedIsoSourceIds()
                    let isoLegacy = isoIds.filter { $0.hasPrefix("zoom:") }
                        .map { String($0.dropFirst(5)) }
                    if let warning = IsoCapturePreflight.warning(
                        zoomIsoCount: isoLegacy.count, captureIntended: captureEnabled,
                        rawMediaActive: lastZoomSnapshot["rawMediaActive"] as? Bool) {
                        pushWarning(warning)
                    }
                    _ = try await outputRequest([
                        "type": "media-core-sync", "elapsedMs": elapsedMs(),
                        "commands": [
                            startProgramOutput(recording: true, streaming: streamingDesired),
                            [
                                "type": "set-recording-targets", "targetFolder": folder,
                                "filenamePrefix": recordPrefix.isEmpty ? "show" : recordPrefix,
                                "format": recordFormat, "quality": "high",
                                // O1 (Windows MediaCoreCommandBuilder.cs:799-805).
                                "targetBitrateMbps": recordingProfile.targetBitrateMbps,
                                "audioBitrateKbps": recordingProfile.audioBitrateKbps,
                                "renderProfile": recordingProfile.payload(fps: canvasFps),
                                "isoSourceIds": isoIds, "isoParticipantIds": isoLegacy,
                            ],
                            ["type": "start-recording-session", "sessionId": UUID().uuidString,
                             "isoSourceIds": isoIds, "isoParticipantIds": isoLegacy],
                        ],
                    ])
                }
                recordingCommands.finish(operation, failed: false)
                recordingDesired = recordingCommands.desired
            } catch {
                guard recordingCommands.finish(operation, failed: true) else { return }
                recordingDesired = recordingCommands.desired
                pushWarning("recording command failed: \(error.localizedDescription)")
            }
        }
    }

    func setMonitor(enabled: Bool, volume: Double) {
        guard bridge != nil || quitRequestForTesting != nil else { return }
        monitorEnabled = enabled
        monitorVolume = volume
        let device = monitorDeviceTarget()
        monitorControl.edit(.init(enabled: enabled, volume: volume,
                                  deviceId: device.deviceId, deviceName: device.deviceName))
        monitorControlNotice = monitorControl.notice
        guard !monitorControlSending else { return }
        monitorControlSending = true
        let legacy = MonitorControlProjection.Draft(enabled: enabled, volume: volume,
                                                    deviceId: device.deviceId, deviceName: device.deviceName)
        Task {
            defer { monitorControlSending = false }
            await runMonitorSendLoop(legacy: legacy)
        }
    }

    /// The monitor send path shared by the UI (setMonitor) and remote control
    /// (requestMonitorControl). `legacy` is what a pre-contract core is sent.
    private func runMonitorSendLoop(legacy: MonitorControlProjection.Draft) async {
        // A core older than this contract still accepts the legacy command.
        // Once an epoch is observed, every subsequent edit is revisioned.
        if monitorControl.authorityEpoch.isEmpty {
            if let response = try? await monitorRequest([
                "type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": [],
            ]), let snapshot = response["snapshot"] as? JSONObject {
                applySnapshot(snapshot)
            }
        }
        if monitorControl.authorityEpoch.isEmpty && monitorControl.legacyCoreConfirmed {
            _ = try? await monitorRequest([
                "type": "media-core-sync", "elapsedMs": elapsedMs(),
                "commands": [["type": "sync-audio-monitor", "enabled": legacy.enabled,
                              "deviceId": legacy.deviceId, "deviceName": legacy.deviceName,
                              "volume": legacy.volume]],
            ])
            return
        }
        if monitorControl.authorityEpoch.isEmpty {
            monitorControl.markUnconfirmed()
            monitorControlNotice = "Monitor edit reconciling; core contract is unconfirmed"
            return
        }
        while let command = monitorControl.nextCommand() {
            do {
                let response = try await monitorRequest([
                    "type": "media-core-sync", "elapsedMs": elapsedMs(),
                    "commands": [command],
                ])
                if let snapshot = response["snapshot"] as? JSONObject { applySnapshot(snapshot) }
            } catch {
                // The core may have applied the edit before its reply was
                // lost. Read a barrier before considering another command.
                if let response = try? await monitorRequest([
                    "type": "media-core-sync", "elapsedMs": elapsedMs(),
                    "commands": [],
                ]), let snapshot = response["snapshot"] as? JSONObject {
                    applySnapshot(snapshot)
                }
            }
            if monitorControl.pendingOperationId != nil {
                monitorControl.markUnconfirmed()
                monitorControlNotice = monitorControl.notice
                break
            }
            if monitorControl.notice.contains("conflicted") { break }
            // A rejected edit keeps its draft; without this the loop re-sent it
            // forever (found by the remote-control port, 2026-11-16).
            if monitorControl.notice.contains("rejected") { break }
        }
    }

    /// audio.monitor.set / audio.monitor.volume (Windows ApplyAudioMonitorControlAsync):
    /// a revisioned edit pinned to the caller's epoch, revision and operation id,
    /// answered from the core's own result for that operation.
    func requestMonitorControl(enabled: Bool?, volume: Double?, expectedEpoch: String?,
                               expectedRevision: Int64?, operationId: String?) async -> MonitorControlOutcome {
        guard bridge != nil || quitRequestForTesting != nil else { return .unconfirmed }
        var next = monitorControl.applied
        if let enabled { next.enabled = enabled }
        if let volume { next.volume = min(max(volume, 0), 1) }   // Windows Math.Clamp
        if next.enabled && next.volume <= 0 { next.volume = 0.7 }
        let device = monitorDeviceTarget()
        next.deviceId = device.deviceId
        next.deviceName = device.deviceName
        // Nothing to change: answer from the core's current authority rather than
        // waiting for a result no command will produce (Windows would send and
        // the core would answer the same way).
        if next == monitorControl.applied, monitorControl.pendingOperationId == nil,
           !monitorControl.authorityEpoch.isEmpty {
            let stale = (expectedEpoch.map { $0 != monitorControl.authorityEpoch } ?? false)
                || (expectedRevision.map { $0 != monitorControl.revision } ?? false)
            return stale ? .conflict(revision: monitorControl.revision) : .applied
        }
        let operation = operationId ?? UUID().uuidString
        monitorControl.remoteEdit(next, expectedEpoch: expectedEpoch, expectedRevision: expectedRevision,
                                  operationId: operation)
        monitorControlNotice = monitorControl.notice
        if !monitorControlSending {
            monitorControlSending = true
            await runMonitorSendLoop(legacy: next)
            monitorControlSending = false
        }
        _ = await settle { monitorControl.outcome(of: operation) != nil }
        let outcome = monitorControl.outcome(of: operation) ?? .unconfirmed
        if outcome == .applied {
            monitorEnabled = monitorControl.applied.enabled
            monitorVolume = monitorControl.applied.volume
            persistPrefsNow()
        }
        return outcome
    }

    /// The monitor path through the same harness seam as Record/Stream.
    private func monitorRequest(_ body: JSONObject) async throws -> JSONObject {
        if let seam = quitRequestForTesting { return try await seam(body, 6) }
        guard let bridge else { throw BridgeError.notRunning }
        return try await bridge.request(body)
    }

    /// One mapping for the snapshot's `captureDevices` and the core's
    /// `list-capture-devices` answer (`{type: "capture-devices", devices}`,
    /// JsonRpcServer.cpp:243-245).
    static func parseCaptureDevices(_ devices: [JSONObject]) -> [CaptureDeviceRow] {
        devices.map { entry in
            CaptureDeviceRow(
                id: entry["id"] as? String ?? "",
                name: entry["name"] as? String ?? "",
                kind: entry["kind"] as? String ?? "video",
                vendor: entry["vendor"] as? String ?? "",
                connectionState: entry["connectionState"] as? String ?? "detected",
                signalPresent: entry["signalPresent"] as? Bool ?? false,
                warning: entry["warning"] as? String ?? "")
        }
    }

    /// Sources "Refresh": asks the core for its device list and shows the answer.
    func refreshCaptureDevices() {
        guard bridge != nil || quitRequestForTesting != nil else { return }
        Task { [weak self] in
            guard let self else { return }
            do {
                let response = try await quitRequest(["type": "list-capture-devices"], timeout: 6)
                guard let devices = response["devices"] as? [JSONObject] else { return }
                captureDevices = Self.parseCaptureDevices(devices)
                refreshSlotHealth()
                reconnectRestoredCaptures()
            } catch {
                pushWarning("capture: refresh failed: \(error.localizedDescription)")
            }
        }
    }

    /// Port of ShowInputsCoordinator.TakeCaptureDeviceOfflineAsync (:460-500): disconnect the
    /// device and unassign every slot holding it. Those slots are NOT reserved — Windows calls
    /// `editor.Unassign()` here, not `UnassignShowInput`.
    func takeCaptureDeviceOffline(_ deviceId: String) {
        guard !deviceId.isEmpty else { return }
        let name = captureDevices.first { $0.id == deviceId }?.name ?? deviceId
        ShellLog.write("lifecycle: take offline \(deviceId)")
        if let bridge {
            Task { _ = try? await bridge.request(CoreCommands.disconnectCaptureDevice(deviceId: deviceId)) }
        }
        var next = slots
        for index in next.indices where next[index].kind == "capture" && next[index].sourceId == deviceId {
            next[index] = ShowInputSlot(id: next[index].id)
        }
        if next != slots {
            ShowInputWriteScope.run("capture-offline") { slots = next }
            recomputeFromSlots()
        }
        sourcesStatus = "\(name) is offline."
    }

    /// The capture connect/disconnect path through its harness seam.
    /// Z2's door to the same capture request path (CaptureAudioSync.swift).
    func captureRequestForSync(_ body: JSONObject) async throws -> JSONObject { try await captureRequest(body) }

    private func captureRequest(_ body: JSONObject) async throws -> JSONObject {
        if let seam = captureRequestForTesting { return try await seam(body) }
        guard let bridge else { throw BridgeError.notRunning }
        return try await bridge.request(body)
    }

    func connectCaptureDevice(_ device: CaptureDeviceRow) {
        guard bridge != nil || captureRequestForTesting != nil else { return }
        Task {
            do {
                // Both commands take a `payload` wrapper (JsonRpcServer).
                if device.connectionState == "connected" {
                    _ = try await captureRequest(
                        CoreCommands.disconnectCaptureDevice(deviceId: device.id))
                } else {
                    _ = try await captureRequest(
                        CoreCommands.connectCaptureDevice(deviceId: device.id,
                                                          outputSourceId: device.id))
                }
                syncSpine()  // multiview sources include connected capture devices
            } catch {
                pushWarning("capture: \(error.localizedDescription)")
            }
        }
    }

    func stopCapture() {
        guard let bridge else { return }
        Task { _ = try? await bridge.request(["type": "zoom-stop-capture"]) }
    }

    // ── support bundle input (Diagnostics.swift owns the writing) ────────────

    // Gathers the shell side of a support bundle ON THE MAIN ACTOR and hands it
    // over as plain Data + value types, so the export can run off-main without
    // dragging live model state across the hop.
    //
    // SECRETS: the stream key and the meeting passcode are DELIBERATELY not put
    // in the payload — only their presence is. They are still passed as
    // `knownSecretLiterals` so the writer can scrub them out of the LOG TAILS
    // by identity and then verify no copy survived anywhere in the bundle.
    func supportBundleInput() -> SupportBundleInput {
        let paths = ShellPaths.resolve()
        let shellState: JSONObject = [
            "bridgeStatus": {
                switch status {
                case .connected(let renderer): return "connected:\(renderer)"
                case .launching: return "launching"
                case .exited(let code): return "exited:\(code)"
                case .failed(let why): return "failed:\(why)"
                }
            }(),
            "statusDetail": statusDetail,
            "selectedTab": selectedTab.rawValue,
            "corePath": paths.corePath ?? "",
            "enginePath": paths.engineBinaryPath ?? "",
            "shellLogPath": ShellLog.path,
            "coreLogPath": CoreLog.path,
            "mediaBinRoot": MediaBin.root,
            "meetingState": meetingState,
            "rawMediaActive": rawMediaActive,
            "captureEnabled": captureEnabled,
            "zoomSignedIn": zoomSignedIn,
            "zoomOAuthStatus": zoomOAuthStatus,
            "webinar": webinar,
            "joinMeetingId": joinMeetingId,
            "joinPasscodePresent": !joinPasscode.isEmpty,
            "displayName": displayName,
            "rosterCount": roster.count,
            "assignedCount": assignedIds.count,
            "liveInputCount": liveInputCount,
            "slots": slots.map { slot in
                [
                    "id": slot.id, "kind": slot.kind, "sourceId": slot.sourceId,
                    "name": slot.name, "inShow": slot.inShow, "iso": slot.iso,
                    "offline": slot.offline,
                ] as JSONObject
            },
            "scenes": scenes.map { scene in
                [
                    "id": scene.id, "name": scene.name, "layout": scene.layout,
                    "layerCount": scene.layers.count,
                ] as JSONObject
            },
            "programSceneId": programSceneId,
            "previewSceneId": previewSceneId,
            "multiviewLayoutMode": multiviewLayoutMode,
            "multiviewTileCount": multiviewTileCount,
            "canvas": ["width": canvasWidth, "height": canvasHeight, "fps": canvasFps],
            "colorGrade": [
                "exposure": gradeExposure, "contrast": gradeContrast,
                "saturation": gradeSaturation, "temperature": gradeTemperature,
            ],
            "recording": [
                "status": recordingStatus, "artifactPath": recordingArtifactPath,
                "warning": recordingWarning, "format": recordFormat, "prefix": recordPrefix,
                "isoEnabled": isoRecordingEnabled,
                "isoSelectedSourceIds": isoSelectedSourceIds.sorted(),
            ],
            "streaming": [
                "desired": streamingDesired, "url": streamUrl,
                // NEVER the value. "is a key configured at all" is the question
                // an operator triaging a dead stream actually needs answered.
                "streamKeyConfigured": !streamKey.isEmpty,
                "status": streamStatus, "detail": streamDetail,
                "bitrateMbps": streamBitrateMbps, "codec": streamCodec,
            ],
            "audio": [
                "masterLevel": masterLevel, "shortTermLufs": shortTermLufs,
                "truePeakDbfs": truePeakDbfs, "limiterActive": limiterActive,
                "masteringRideDb": masteringRideDb, "monitorStatus": monitorStatus,
                "monitorEnabled": monitorEnabled, "monitorFeedbackRisk": monitorFeedbackRisk,
                "stripCount": strips.count,
            ],
            "overlays": [
                "onAir": overlaysOnAir, "lowerThirdPhase": lowerThirdPhase,
                "logoBug": logoBug?.name ?? "",
            ],
            "virtualCamera": virtualCameraDiagnostics().json,
            "autoProduction": [
                "enabled": autoDirectEnabled, "ruleId": autoRuleId,
                "recommendedSceneId": autoSceneId, "confidence": autoConfidence,
                "status": autoStatus,
            ],
        ]
        var input = SupportBundleInput()
        // Social review: a social card's fields ARE chat (author, message), which
        // never goes into a bundle (slice-6 spec :15). The instance stays, its
        // payload does not.
        input.coreSnapshotJson =
            (try? JSONSerialization.data(withJSONObject: SocialRedaction.scrub(snapshot: lastSnapshot))) ?? Data()
        input.zoomSnapshotJson =
            (try? JSONSerialization.data(withJSONObject: lastZoomSnapshot)) ?? Data()
        input.shellStateJson = (try? JSONSerialization.data(withJSONObject: shellState)) ?? Data()
        input.warnings = warnings
        input.knownSecretLiterals = ([streamKey, joinPasscode] + SocialRedaction.secretLiterals()).filter { !$0.isEmpty }
        return input
    }
}

// The environment the shell adds when it launches the core: pure, so the
// shell suite can pin it (ShellTestsGraphicsFonts). Moved out of AppModel's
// launch unchanged, plus COREVIDEO_GRAPHICS_FONTS_DIR.
/// The Comms tab is hidden until the live co-host acceptance passes (owner
/// decision 2; plan Task 11). Until then COREVIDEO_SHELL_COMMS=1 shows it.
enum CommsGate {
    static var enabled: Bool { ProcessInfo.processInfo.environment["COREVIDEO_SHELL_COMMS"] == "1" }
}

enum CoreLaunchEnvironment {
    static func extras(paths: ShellPaths.Paths, processEnvironment: [String: String],
                       zoomPublicAppKey: String?) -> [String: String] {
        var env: [String: String] = [:]
        // A pre-set engine path wins (Windows run-studio.ps1 rule): it is how a
        // developer points the shell at the fake engine on a machine that also
        // has a real engine build.
        if let preset = processEnvironment["COREVIDEO_ZOOM_ENGINE_PATH"], !preset.isEmpty {
            env["COREVIDEO_ZOOM_ENGINE_PATH"] = preset
        } else if let enginePath = paths.engineBinaryPath {
            env["COREVIDEO_ZOOM_ENGINE_PATH"] = enginePath
        }
        if let key = zoomPublicAppKey, !key.isEmpty {
            env["COREVIDEO_ZOOM_PUBLIC_APP_KEY"] = key
        }
        // The core waits this long for SDK auth, then again for the join
        // verdict. Its built-in default is 7 s, which the Windows shell has
        // never used (MediaCorePaths.cs sets 45000). Unset here, a Mac join
        // reported "Timed out waiting for Zoom meeting join result." 7 s in,
        // a moment before Zoom's real answer (2026-11-15, signed 1.0.0).
        env["COREVIDEO_ZOOM_JOIN_WAIT_MS"] = processEnvironment["COREVIDEO_ZOOM_JOIN_WAIT_MS"]
            ?? ZoomJoinStatus.coreJoinWaitMs
        // VST3 discovery. `resolvePluginHostExecutablePath` (MediaCore.cpp) only
        // does sibling-of-the-exe lookup under _WIN32, so on macOS the core finds
        // the host ONLY through this env var — without it pluginHost.status is
        // permanently "absent". Likewise `scanRoots` (plugin-host.cpp) reads the
        // Windows COMMONPROGRAMFILES vars plus this single override, so a macOS
        // scan walks nothing unless we name a root. See VstInserts.swift.
        if let hostPath = paths.pluginHostPath {
            env["COREVIDEO_PLUGIN_HOST_PATH"] = hostPath
        }
        if processEnvironment["COREVIDEO_VST3_SCAN_PATH"] == nil {
            env["COREVIDEO_VST3_SCAN_PATH"] = VstScanRoots.effectiveRoot
        }
        // Graphics fonts (Metal graphics backend G2). On a pkg install the core
        // runs from /Library/Application Support/CoreVideo Pro/ (CoreLocation),
        // where make-macos-pkg.sh stages ONLY the binary, so the core's own
        // lookup (GraphicsFonts/ beside the executable) finds nothing and Segoe
        // UI graphics would fail strict. Name the app's folder explicitly.
        if let override = processEnvironment["COREVIDEO_GRAPHICS_FONTS_DIR"], !override.isEmpty {
            env["COREVIDEO_GRAPHICS_FONTS_DIR"] = override
        } else if let fonts = paths.graphicsFontsDirectory {
            env["COREVIDEO_GRAPHICS_FONTS_DIR"] = fonts
        }
        // Streaming helpers (mac streaming Phase 2): the same pkg layout means
        // nothing sits beside the core, so name the bundle's corevideo-child-guard
        // and the directory of its LGPL ffmpeg (an operator's own
        // COREVIDEO_FFMPEG_BIN_DIR wins). Without these the core ran Homebrew's
        // GPL ffmpeg, or none, and ran it unguarded.
        env.merge(ShellPaths.streamingEnvironment(paths, inherited: processEnvironment)) { $1 }
        return env
    }
}

// Locates the core + engine relative to the app bundle (Resources) or the
// repo build tree (dev runs from run-mac-shell.sh).
enum ShellPaths {
    struct Paths {
        var corePath: String?
        var engineBinaryPath: String?
        var pluginHostPath: String?  // corevideo-plugin-host (VST3 scan on macOS)
        /// The app's bundled graphics fonts (Selawik for Segoe UI). The core
        /// looks beside its own executable, which the pkg-installed copy in
        /// /Library has not got, so the launch names this folder explicitly.
        var graphicsFontsDirectory: String?
        /// corevideo-child-guard and the directory holding the bundled ffmpeg,
        /// in the app's Resources (mac streaming Phase 2). The pkg runs the core
        /// from /Library/Application Support, where neither sits beside it.
        var childGuardPath: String?
        var bundledFfmpegDirectory: String?
        /// True when corePath is the pkg-installed copy the camera extension can
        /// verify (CoreLocation); false means the camera refuses this core's frames.
        var coreCameraEligible = false
    }

    private static let launchLock = NSLock()
    private static var launchedCore: CoreLocation.Choice?

    /// For the core LAUNCH only: decides which core copy runs (reads both
    /// binaries once, CoreLocation) and remembers the decision.
    static func resolveForLaunch() -> Paths {
        let bundleResources = Bundle.main.resourcePath ?? ""
        let candidates = [
            bundleResources + "/corevideo-native",
            repoRoot() + "/native/build-metal/corevideo-native",
        ]
        let core = CoreLocation.resolve(
            bundledCore: candidates.first { FileManager.default.isExecutableFile(atPath: $0) })
        launchLock.lock(); launchedCore = core; launchLock.unlock()
        return resolve()
    }

    /// Everything else (Diagnostics redraws, support bundles): reports the core
    /// the last launch chose, so it names the binary actually running and never
    /// re-reads it. Before any launch it decides fresh without remembering.
    static func resolve() -> Paths {
        var paths = Paths()
        let bundleResources = Bundle.main.resourcePath ?? ""
        launchLock.lock(); let remembered = launchedCore; launchLock.unlock()
        let core = remembered ?? CoreLocation.resolve(bundledCore: [
            bundleResources + "/corevideo-native",
            repoRoot() + "/native/build-metal/corevideo-native",
        ].first { FileManager.default.isExecutableFile(atPath: $0) })
        paths.corePath = core.path
        paths.coreCameraEligible = core.cameraEligible
        let engineCandidates = [
            bundleResources + "/corevideo-zoom-engine.app/Contents/MacOS/corevideo-zoom-engine",
            repoRoot()
                + "/native/build-engine/corevideo-zoom-engine.app/Contents/MacOS/corevideo-zoom-engine",
        ]
        paths.engineBinaryPath = engineCandidates.first {
            FileManager.default.isExecutableFile(atPath: $0)
        }
        let pluginHostCandidates = [
            bundleResources + "/corevideo-plugin-host",
            repoRoot() + "/native/build-metal/corevideo-plugin-host",
        ]
        paths.pluginHostPath = pluginHostCandidates.first {
            FileManager.default.isExecutableFile(atPath: $0)
        }
        paths.graphicsFontsDirectory = graphicsFontsDirectory(bundleResources: bundleResources, repoRoot: repoRoot())
        var helpers = streamingHelpers(resources: bundleResources) {
            FileManager.default.isExecutableFile(atPath: $0)
        }
        if helpers.childGuardPath == nil {
            // Dev runs: the guard is built beside the dev core.
            let dev = repoRoot() + "/native/build-metal/corevideo-child-guard"
            if FileManager.default.isExecutableFile(atPath: dev) { helpers.childGuardPath = dev }
        }
        paths.childGuardPath = helpers.childGuardPath
        paths.bundledFfmpegDirectory = helpers.bundledFfmpegDirectory
        return paths
    }

    /// The app's GraphicsFonts folder, else a dev build's (run-mac-shell.sh).
    static func graphicsFontsDirectory(bundleResources: String, repoRoot: String) -> String? {
        [bundleResources + "/GraphicsFonts", repoRoot + "/native/build-metal/GraphicsFonts"].first {
            var isDirectory: ObjCBool = false
            return FileManager.default.fileExists(atPath: $0, isDirectory: &isDirectory) && isDirectory.boolValue
        }
    }

    /// The bundle's guard and ffmpeg, if this app carries them.
    static func streamingHelpers(resources: String, exists: (String) -> Bool) -> Paths {
        var paths = Paths()
        guard !resources.isEmpty else { return paths }
        let guardPath = resources + "/corevideo-child-guard"
        if exists(guardPath) { paths.childGuardPath = guardPath }
        if exists(resources + "/ffmpeg") { paths.bundledFfmpegDirectory = resources }
        return paths
    }

    /// Environment the core needs to use the bundle's guard and ffmpeg wherever
    /// the core itself runs from. An operator's own COREVIDEO_FFMPEG_BIN_DIR wins.
    static func streamingEnvironment(_ paths: Paths, inherited: [String: String]) -> [String: String] {
        var env: [String: String] = [:]
        if let guardPath = paths.childGuardPath { env["COREVIDEO_CHILD_GUARD_PATH"] = guardPath }
        if let directory = paths.bundledFfmpegDirectory, inherited["COREVIDEO_FFMPEG_BIN_DIR"] == nil {
            env["COREVIDEO_FFMPEG_BIN_DIR"] = directory
        }
        return env
    }

    static func repoRoot() -> String {
        if let override = ProcessInfo.processInfo.environment["COREVIDEO_REPO_ROOT"] {
            return override
        }
        return NSHomeDirectory() + "/Developer/CoreVideoPro"
    }
}


// Core stderr mirror (~/Library/Logs/CoreVideoPro-core.log), size-capped by
// truncation on launch. Serial queue keeps line writes ordered off-main.
enum CoreLog {
    static let path = NSHomeDirectory() + "/Library/Logs/CoreVideoPro-core.log"
    private static let queue = DispatchQueue(label: "us.iamfatness.corevideopro.corelog")
    private static let handle: FileHandle? = {
        FileManager.default.createFile(atPath: path, contents: nil)
        return FileHandle(forWritingAtPath: path)
    }()

    static func write(_ line: String) {
        queue.async {
            handle?.write(Data((line + "\n").utf8))
        }
    }
}

// Always-on file log (~/Library/Logs/CoreVideoPro-shell.log): LaunchServices
// launches have no attached stdout, which made GUI sessions undiagnosable.
enum ShellLog {
    static let path = NSHomeDirectory() + "/Library/Logs/CoreVideoPro-shell.log"

    static func write(_ message: String) {
        let line = "\(Date()) \(message)\n"
        print("shell: \(message)")
        // The in-binary harness must never touch the operator's real log.
        // Cached harness predicate (tests AND selfcheck), not an env read per line.
        if ShellHarness.isActive { return }
        if let handle = FileHandle(forWritingAtPath: path) {
            handle.seekToEndOfFile()
            handle.write(Data(line.utf8))
            try? handle.close()
        } else {
            try? line.write(toFile: path, atomically: true, encoding: .utf8)
        }
    }
}
