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
    case media = "Media"
    case automation = "Automation"
    case settings = "Settings"
    case diagnose = "Diagnose"

    // Top navigation, matching Windows 1.0 (StudioWorkspace.xaml): one "Produce"
    // group — Studio, Zoom, Sources, Scenes, Routing, Graphics, Audio, Media,
    // Health. Comms and OHG Show are not on Mac in 1.0, so they are omitted
    // rather than shown disabled. Settings trails: Windows reaches its Zoom
    // connection page through "Zoom", which is Mac's Zoom pane; Mac Settings
    // (output/recording/stream) has no Windows nav counterpart.
    static let navGroups = ["Produce"]
    static let navigation: [StudioTab] = [
        .studio, .zoom, .sources, .scenes, .routing, .overlays, .audio, .media,
        .diagnose, .settings,
    ]

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
        }) else { return nil }
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

    var json: JSONObject {
        var object: JSONObject = [
            "routeId": routeId, "mode": mode, "audioRole": audioRole,
            "fitMode": "fill", "opacity": 1.0, "zIndex": zIndex,
        ]
        if let chromaKey { object["chromaKey"] = chromaKey }
        if let participantId { object["participantId"] = participantId }
        if let captureDeviceId { object["captureDeviceId"] = captureDeviceId }
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
}

// Show-input slot (inputs 1–10): the operator's stable mapping from "what's
// plugged in" to production slots — the ShowInputSlot model from the Windows
// shell (docs/routing-ux-spec.md tab 1). Slots are the AUTHORITY for show
// membership; assignedIds/multiview/scenes derive from them.
struct ShowInputSlot: Identifiable, Equatable {
    var id: Int                 // 1...10 (var only so a drag-swap can renumber)
    var kind = "unassigned"     // unassigned | zoom | capture
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
struct MasteringParams: Equatable {
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

    var json: JSONObject {
        [
            "enabled": enabled, "targetLufs": targetLufs,
            "ceilingDbfs": ceilingDbfs, "glueAmount": glueAmount,
            "maxRideDb": maxRideDb, "inputGainDb": inputGainDb,
            "highPassHz": highPassHz, "lowShelfDb": lowShelfDb,
            "presenceDb": presenceDb, "highShelfDb": highShelfDb,
            "stereoWidth": stereoWidth,
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
    var tally: String     // program | preview | none
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
                sourceGeneration: (entry["sourceGeneration"] as? NSNumber)?.int64Value ?? 0)
        }
    }
}

@MainActor
final class AppModel: ObservableObject {
    @Published var status: BridgeStatus = .launching
    @Published var statusDetail = ""
    @Published var meetingState = "idle"
    @Published var rawMediaActive = false
    @Published var roster: [RosterParticipant] = []
    /// The Zoom audio source set last placed on the spine (participant-audio ids,
    /// in budget order) and whether meeting-audio was subscribed with it. The audio
    /// console builds its strips from these — the FADER LAW needs a strip for
    /// every source the core is sent, not only for the ones it has echoed.
    @Published var zoomAudioSourceIds: [String] = []
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
    @Published var truePeakDbfs = -120.0
    @Published var limiterActive = false
    @Published var masteringRideDb = 0.0
    @Published var monitorStatus = ""
    @Published var monitorFeedbackRisk = false
    @Published var monitorEnabled = false
    @Published var monitorVolume = 0.7
    @Published var monitorControlNotice = ""
    private var monitorControl = MonitorControlProjection()
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
    private var audioRouteControl = AudioRouteControlProjection()
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
    @Published var selectedTab: StudioTab = .studio
    @Published var joinMeetingId = ""
    @Published var joinPasscode = ""
    @Published var displayName = "CoreVideo Pro (mac)"
    @Published var scenes: [SceneDef] = [
        SceneDef(id: "fullscreen", name: "Fullscreen", layout: "single"),
        SceneDef(id: "two-up", name: "Side by side", layout: "two-up"),
        SceneDef(id: "pip", name: "Picture in picture", layout: "pip"),
    ]
    @Published var programSceneId = "" { didSet { mediaAudioSceneChanged(oldValue != programSceneId) } }
    @Published var previewSceneId = "" { didSet { mediaAudioSceneChanged(oldValue != previewSceneId) } }
    @Published var mediaAssets: [MediaAsset] = []
    @Published var selectedAssetId = ""
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
    @Published var isoRecordingEnabled = false
    /// THE ISO selection (Windows ShowInputsCoordinator.IsoSelectedSourceIds):
    /// scheme-qualified source ids. The Sources-tab ISO toggles, the Routing
    /// matrix and the footer all write THIS; the slot `iso` flags and the
    /// matrix columns are projections of it, re-derived on every change.
    @Published var isoSelectedSourceIds: Set<String> = [] {
        didSet { projectIsoSelection() }
    }
    @Published var streamUrl = "rtmp://a.rtmp.youtube.com/live2"
    @Published var streamKey = ""
    @Published var streamingDesired = false
    @Published var streamStatus = ""       // sender status from outputSenderSession
    @Published var streamDetail = ""       // lastResultCode / warning

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

    private var lastSavedPrefs = ShellPrefs()
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
        prefs.showInputSlots = slots.filter { $0.kind == "capture" && $0.sourceKey != nil }.map {
            PersistedSlot(slotId: $0.id, kind: $0.kind, sourceId: $0.sourceId,
                          name: SlotNamePolicy.persisted($0), inShow: $0.inShow)
        }
        prefs.vstChannelSelections = vstChannelSelection.isEmpty ? nil : vstChannelSelection
        prefs.autoAssignInputs = autoAssignInputsEnabled
        prefs.sourcesWorkspace = sourcesWorkspace.columns
        prefs.cameraExtensionOptedIn = cameraExtensionOptedIn
        prefs.virtualCamera = virtualCamera == PersistedVirtualCamera() ? nil : virtualCamera
        storeProductionSettings(into: &prefs)
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
                           })
        }
        return prefs
    }

    private func restorePrefs() {
        applyRestoredPrefs(ShellPrefs.load())
        streamKey = StreamKeychain.load()
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
        overlays = prefs.overlays ?? OverlaysState()
        restoreSlotAssignments(prefs.showInputSlots ?? [])
        // No reapply: no roster exists at launch.
        assignmentLaw.autoAssignEnabled = prefs.autoAssignInputs ?? true
        autoAssignInputsEnabled = assignmentLaw.autoAssignEnabled
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
        lowerThirdPosition = prefs.lowerThirdPosition
        if !prefs.logoBugAssetId.isEmpty {
            logoBug = mediaAssets.first { $0.id == prefs.logoBugAssetId }
        }
        streamUrl = prefs.streamUrl
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
                         backgroundAssetId: saved.backgroundAssetId)
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
        var env: [String: String] = [:]
        if let enginePath = paths.engineBinaryPath {
            env["COREVIDEO_ZOOM_ENGINE_PATH"] = enginePath
        }
        if let key = UserDefaults.standard.string(forKey: "zoomPublicAppKey"), !key.isEmpty {
            env["COREVIDEO_ZOOM_PUBLIC_APP_KEY"] = key
        }
        // The core waits this long for SDK auth, then again for the join
        // verdict. Its built-in default is 7 s, which the Windows shell has
        // never used (MediaCorePaths.cs sets 45000). Unset here, a Mac join
        // reported "Timed out waiting for Zoom meeting join result." 7 s in,
        // a moment before Zoom's real answer (2026-11-15, signed 1.0.0).
        env["COREVIDEO_ZOOM_JOIN_WAIT_MS"] = ProcessInfo.processInfo
            .environment["COREVIDEO_ZOOM_JOIN_WAIT_MS"] ?? ZoomJoinStatus.coreJoinWaitMs
        // VST3 discovery. `resolvePluginHostExecutablePath` (MediaCore.cpp) only
        // does sibling-of-the-exe lookup under _WIN32, so on macOS the core finds
        // the host ONLY through this env var — without it pluginHost.status is
        // permanently "absent". Likewise `scanRoots` (plugin-host.cpp) reads the
        // Windows COMMONPROGRAMFILES vars plus this single override, so a macOS
        // scan walks nothing unless we name a root. See VstInserts.swift.
        if let hostPath = paths.pluginHostPath {
            env["COREVIDEO_PLUGIN_HOST_PATH"] = hostPath
        }
        if ProcessInfo.processInfo.environment["COREVIDEO_VST3_SCAN_PATH"] == nil {
            env["COREVIDEO_VST3_SCAN_PATH"] = VstScanRoots.effectiveRoot
        }
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
            self.pushWarning("media core exited (code \(code)) — relaunching")
        }
    }

    /// Test-only: stops the poll timers onConnected() schedules.
    func stopTimersForTesting() {
        syncTimer?.invalidate(); syncTimer = nil
        zoomTimer?.invalidate(); zoomTimer = nil
        cameraUpdateTimer?.invalidate(); cameraUpdateTimer = nil
    }

    // The Studio center is ONE multiview canvas (reference 01-studio): the
    // core composites PREVIEW + PROGRAM tiles INSIDE the multiview texture in
    // the pgmPvw layout modes, plus the live input tiles below.
    @Published var multiviewLayoutMode = "pgmPvwTop"

    func configureMultiviewer(mode: String) {
        multiviewLayoutMode = mode
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
    private func persistPrefsNow() {
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
        monitorControl.resetForProcess()
        audioRouteControl.resetForProcess()
        rosterProjection.resetForProcess()
        sourceFacts.mutate { $0.engineStopped() }
        rosterRecoveryNotice = rosterProjection.notice
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
        // One-shot: a respawned core forgets the virtual camera (CLAUDE.md).
        sendVirtualCameraSync()
        audioInputs = AudioInputs.available()
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
            controlSnapshotBox.publish(snapshot, at: Date())
            observeAudioObservation(snapshot)
        }
        if !snapshot.isEmpty { applyProgramBlackout(snapshot) }
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
        }
        applySourceFacts(snapshot)
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
            let senders = session["senders"] as? [JSONObject] ?? []
            if let rtmp = senders.first(where: { $0["destination"] as? String == "rtmp" }) {
                streamStatus = rtmp["status"] as? String ?? ""
                let code = rtmp["lastResultCode"] as? String ?? ""
                let warning = rtmp["warning"] as? String ?? ""
                streamDetail = warning.isEmpty ? code : warning
            } else if !streamingDesired {
                streamStatus = ""
                streamDetail = ""
            }
        }
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
            let previous = meetingState
            meetingState = snapshot["meetingState"] as? String ?? meetingState
            if meetingState == "idle", previous != "idle" { pruneStaleZoomIsoIntent() }
        }
        if acceptRoster, Self.meetingHasEnded(snapshot["meetingState"] as? String ?? "") {
            endMeetingAudioSession()
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
        if !enabled { sourceFacts.mutate { $0.engineStopped() } }
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

    func applyMultiviewConfig() {
        guard let bridge else { return }
        let command = multiviewConfigCommand()
        Task {
            _ = try? await bridge.request([
                "type": "media-core-sync", "elapsedMs": elapsedMs(),
                "commands": [command],
            ])
        }
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
        guard bridge != nil || quitRequestForTesting != nil else { return }
        let target = Self.parseZoomTarget(joinMeetingId, fallbackPasscode: joinPasscode)
        let meetingId = target.meetingId
        let parsedPasscode = target.passcode
        joinPasscode = parsedPasscode
        guard !meetingId.isEmpty, !joinInFlight else { return }
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
        // Absent, not false, on an ordinary join (the core reads it per join).
        if endOtherMeeting { payload["endOtherMeeting"] = true }
        ShellLog.write("joining meeting \(meetingId)\(endOtherMeeting ? " (end my other Zoom session)" : "")")
        Task {
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
    }

    func leaveZoom() {
        guard let bridge else { return }
        assignedIds.removeAll()
        Task { _ = try? await bridge.request(["type": "zoom-leave"], timeout: 15) }
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
            try? await Task.sleep(nanoseconds: 120_000_000)
            guard !Task.isCancelled else { return }
            await self?.pushSpine()
        }
    }

    private func pushSpine() async {
        guard let bridge else { return }
        let requests = spinePushRequests()
        for request in requests {
            do {
                _ = try await bridge.request(request)
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
        let subscriptions = resolveSpineSubscriptions()
        refreshStrips()  // the console follows the audio source set just decided
        var audio: [JSONObject] = []
        let stripIds = Set(strips.map(\.id))
        if !strips.isEmpty, stripIds != lastSyncedStripIds {
            mixSyncTask?.cancel()
            scheduledStripIds = nil
            audio.append(audioMixCommand())
            lastSyncedStripIds = stripIds
        }
        if let routing = routingBaseCommand(), let sends = routing["sends"] as? [JSONObject],
           !(lastRoutingBaseSent.map { NSArray(array: $0).isEqual(to: sends) } ?? false) {
            routingBaseTask?.cancel()
            audio.append(routing)
            lastRoutingBaseSent = sends
        }
        // Multiview sources = the in-show slots that still HAVE a source. An
        // offline slot (participant left, device unplugged) kept its tile and
        // rendered as an unlabeled gray placeholder — a dead box on the wall.
        let multiviewSources = Self.multiviewSourcePayloads(slots)
        let payload: JSONObject = [
            // startCapture is what arms raw media (engine start_media →
            // start_raw_media): without it every video subscription stays
            // "deferred" and no frames ever flow. Operator-driven via the
            // Capture button (the Windows "Engine On" toggle), never automatic.
            "startCapture": captureEnabled,
            "multiview": [
                "canvasWidth": 1920, "canvasHeight": 1080,
                "sources": multiviewSources,
            ],
            "readiness": [
                "status": "ready", "platform": "darwin", "sdkVersion": "",
                "checks": [], "blockers": [], "warnings": [], "summary": "mac shell",
            ],
            "participants": participants,
            "subscriptions": subscriptions,
            "blocked": false,
            "warnings": [],
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
        // Budget-ordered, camera-on-filtered, capped (E1/E2/E5 — see
        // ZoomSourceBudget.swift). Replaces the old `assignedIds` Set, which was
        // nondeterministic, uncapped and subscribed camera-off participants.
        let programRouted = programSceneId.isEmpty
            ? [] : ZoomSourceBudget.routedZoomPids(buildRoutes(for: programSceneId, isProgram: true))
        let previewRouted = previewSceneId.isEmpty
            ? [] : ZoomSourceBudget.routedZoomPids(buildRoutes(for: previewSceneId))
        let multiviewAssigned = slots
            .filter { $0.kind == "zoom" && $0.inShow && !$0.offline }
            .map(\.sourceId)
        let isoArmed = isoRecordingEnabled
            ? slots.filter { $0.kind == "zoom" && $0.iso }.map(\.sourceId) : []
        let cameraOn = Set(roster.filter(\.hasVideo).map(\.id))
        let audioIds = ZoomSourceBudget.audioParticipantIds(
            programRouted: programRouted, previewRouted: previewRouted,
            multiviewAssigned: multiviewAssigned, isoArmed: isoArmed,
            present: Set(roster.map(\.id)))
        zoomAudioSourceIds = audioIds
        meetingAudioSubscribed = !roster.isEmpty
        return Self.spineSubscriptionPayloads(
            ZoomSourceBudget.videoEntries(
                programRouted: programRouted, previewRouted: previewRouted,
                multiviewAssigned: multiviewAssigned, isoArmed: isoArmed, cameraOn: cameraOn),
            audioParticipantIds: audioIds,
            meetingHasParticipants: !roster.isEmpty)
    }

    // The multiview roster on the wire: one tile per in-show slot that still has
    // a source. `kind` is the MULTIVIEW vocabulary the core's resolveMultiviewFeed
    // reads (native/src/core/MultiviewFeed.h): "capture" | "media" | anything else
    // = a Zoom participant. It is NOT the scene-route `mode` ("capture-input").
    static func multiviewSourcePayloads(_ slots: [ShowInputSlot]) -> [JSONObject] {
        slots
            .filter { $0.kind != "unassigned" && $0.inShow && !$0.offline }
            .map { slot in
                slot.kind == "zoom"
                    ? [
                        "sourceId": "zoom:" + slot.sourceId,
                        "kind": "participant-video",
                        "participantId": slot.sourceId,
                        "label": slot.name,
                    ]
                    : [
                        "sourceId": "capture:" + slot.sourceId,
                        "kind": "capture",
                        "captureDeviceId": slot.sourceId,
                        "label": slot.name,
                    ]
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
    static func spineSubscriptionPayloads(
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
    private static let layoutRects: [String: [(Double, Double, Double, Double)]] = [
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
            if slot.kind == "zoom" {
                route.participantId = slot.sourceId
            } else {
                route.mode = "capture-input"
                route.captureDeviceId = slot.sourceId
            }
            route.chromaKey = chromaKeyNode(for: slot)
            return [route.json]
        }
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
                        "mediaPlaybackKey": "", "mediaAssetPlaying": false,
                        "rect": ["x": layer.x, "y": layer.y, "width": layer.width, "height": layer.height],
                    ]
                }
                if let slotId = layer.slotId,
                   let slot = slots.first(where: { $0.id == slotId }),
                   slot.kind != "unassigned" {
                    if slot.kind == "zoom" {
                        route.mode = "fixed"
                        route.participantId = slot.sourceId
                    } else {
                        route.mode = "capture-input"
                        route.captureDeviceId = slot.sourceId
                    }
                    route.chromaKey = chromaKeyNode(for: slot)
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
                    "mediaPlaybackKey": "", "mediaAssetPlaying": false,
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
                let slot = inShow[index]
                if slot.kind == "zoom" {
                    route.mode = "fixed"
                    route.participantId = slot.sourceId
                } else {
                    route.mode = "capture-input"
                    route.captureDeviceId = slot.sourceId
                }
                route.chromaKey = chromaKeyNode(for: slot)
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
                "mediaPlaybackKey": "", "mediaAssetPlaying": false,
                "rect": ["x": rect.x, "y": rect.y, "width": rect.w, "height": rect.h],
            ])
        }
        return routes
    }

    // ── scene canvas editing ─────────────────────────────────────────────────

    // Seed the editable layer list from the layout preset the scene already
    // shows, so opening the editor never changes what is on screen.
    func ensureLayers(for sceneId: String) {
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
                            backgroundAssetId: source.backgroundAssetId)
        scenes.append(copy)
        previewSceneId = newId
        studioCommandStatus = "\(name) duplicated from \(source.name) and cued on preview"
        syncScenes()
        saveScenes()
    }

    func newScene(layout: String) {
        let newId = "scene-\(Int(Date().timeIntervalSince1970))"
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
        previewSceneId = previousProgram
        programSceneId = taken
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

    /// Installed only while the Studio tab is the active tab.
    func syncTransportHotkeys() {
        transportHotkeys.setActive(selectedTab == .studio) { [weak self] nsEvent in
            guard let self else { return false }
            let key = TransportKeys.event(
                from: nsEvent, isStudioTab: self.selectedTab == .studio,
                firstResponder: nsEvent.window?.firstResponder)
            return self.handleTransportKey(key)
        }
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
            try? await Task.sleep(nanoseconds: 120_000_000)
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
            "playing": true,
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
            commands.append(preview)
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
                id: (slot.kind == "zoom" ? "zoom:" : "capture:") + slot.sourceId,
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

    private func streamDestinationSettings() -> [JSONObject] {
        [[
            "id": "rtmp", "label": "Stream", "protocol": "rtmp",
            "url": streamUrl, "streamKey": streamKey,
            // An explicit destination bitrate WINS over streamOutputProfile in
            // the core (#606), so this must be the operator's value too.
            "fps": 30, "targetBitrateMbps": streamBitrateMbps, "audioBitrateKbps": 160,
            "videoCodec": "h264", "encoderMode": "auto",
            "keyframeIntervalSeconds": 2.0, "rateControl": "cbr",
            "h264Profile": "high", "bFrames": 2, "allowEnhancedRtmp": false,
        ]]
    }

    // start-program-output is FULL-STATE for the destination set, so record
    // and stream always resend the union of what should be live.
    func startProgramOutput(recording: Bool, streaming: Bool) -> JSONObject {
        var destinations: [String] = []
        if recording { destinations.append("recording") }
        if streaming { destinations.append("rtmp") }
        let isoIds = resolvedIsoSourceIds(recording: recording)
        var command: JSONObject = [
            "type": "start-program-output", "destinations": destinations,
            "isoSourceIds": isoIds,
            "isoParticipantIds": isoIds.filter { $0.hasPrefix("zoom:") }
                .map { String($0.dropFirst(5)) },
        ]
        if streaming {
            command["streamOutputProfile"] = streamOutputProfile()
            command["destinationSettings"] = streamDestinationSettings()
        }
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
            let commands: [JSONObject] = isRecordingActive
                ? [startProgramOutput(recording: true, streaming: false)]
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
        guard !streamKey.isEmpty, !streamUrl.isEmpty else {
            pushWarning("stream: URL and key are required")
            return
        }
        StreamKeychain.save(streamKey)
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
    static let routingBuses = ["master", "pgm-l", "pgm-r", "stream", "mon", "iso-1"]

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
            captureAudioIds: slots
                .filter { $0.kind == "capture" && !$0.sourceId.isEmpty && !$0.audioDeviceId.isEmpty }
                .map { "capture:" + $0.sourceId },
            mediaRouted: sceneHasMediaRoute(programSceneId, isProgram: true),
            operatorEdits: operatorRouteEdits)
        guard !sends.isEmpty else { return nil }
        return ["type": "sync-audio-routing-matrix", "sends": sends,
                "busSends": [] as [JSONObject], "monitorBusId": ""]
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
            try? await Task.sleep(nanoseconds: 100_000_000)
            guard !Task.isCancelled else { return }
            await self?.sendRoutingBaseNow()
        }
    }

    private func sendRoutingBaseNow() async {
        guard let command = routingBaseCommand() else { return }
        let sends = command["sends"] as? [JSONObject] ?? []
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
                    "busSends": [] as [JSONObject], "monitorBusId": "",
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

    func editMastering(_ apply: (inout MasteringParams) -> Void) {
        apply(&mastering)
        scheduleMixSync()
    }

    // ── media bin + logo bug ─────────────────────────────────────────────────

    func refreshMediaBin() {
        mediaAssets = MediaBin.loadAssets()
    }

    func importMedia(urls: [URL]) {
        let imported = MediaBin.importFiles(urls)
        refreshMediaBin()
        if let first = imported.first { selectedAssetId = first.id }
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
            captureAudioIds: slots
                .filter { $0.kind == "capture" && !$0.sourceId.isEmpty && !$0.audioDeviceId.isEmpty }
                .map { "capture:" + $0.sourceId },
            mediaStrip: sceneHasMediaRoute(programSceneId, isProgram: true)
                || sceneHasMediaRoute(previewSceneId, isProgram: false))
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
        lastRoutingBaseSent = routingBaseCommand()?["sends"] as? [JSONObject]
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
            expected: expectedAudioSourceIds)
        var commands: [JSONObject] = [audioMixCommand()]
        let routing = routingBaseCommand()
        if let routing { commands.append(routing) }
        commands.append(captureAudioSourcesCommand())
        lastSyncedStripIds = Set(strips.map(\.id))
        lastRoutingBaseSent = routing?["sends"] as? [JSONObject]
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
    func refreshStrips() {
        strips = AudioConsolePolicy.mergeStrips(
            echo: lastMixEcho, prior: strips, remembered: rememberedStripSettings,
            expected: expectedAudioSourceIds)
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
        return ["type": "sync-participant-audio-mix", "limiterEnabled": true,
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
        switch quitGate.request(quitOutputs) {
        case .quitNow: return .terminateNow
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

    private func quitRequest(_ body: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        if let seam = quitRequestForTesting { return try await seam(body, timeout) }
        guard let bridge else { throw BridgeError.notRunning }
        return try await bridge.request(body, timeout: timeout)
    }

    /// The operator's Record/Stream buttons go through here so the harness can
    /// drive the REAL start/stop paths (intent bookkeeping included) through the
    /// same seam the quit flow uses. Production: the bridge, default timeout.
    private func outputRequest(_ body: JSONObject) async throws -> JSONObject {
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
        Task {
            defer { monitorControlSending = false }
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
                    "commands": [["type": "sync-audio-monitor", "enabled": enabled,
                                  "deviceId": device.deviceId, "deviceName": device.deviceName,
                                  "volume": volume]],
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
            }
        }
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

    func connectCaptureDevice(_ device: CaptureDeviceRow) {
        guard let bridge else { return }
        Task {
            do {
                // Both commands take a `payload` wrapper (JsonRpcServer).
                if device.connectionState == "connected" {
                    _ = try await bridge.request(
                        CoreCommands.disconnectCaptureDevice(deviceId: device.id))
                } else {
                    _ = try await bridge.request(
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
        input.coreSnapshotJson =
            (try? JSONSerialization.data(withJSONObject: lastSnapshot)) ?? Data()
        input.zoomSnapshotJson =
            (try? JSONSerialization.data(withJSONObject: lastZoomSnapshot)) ?? Data()
        input.shellStateJson = (try? JSONSerialization.data(withJSONObject: shellState)) ?? Data()
        input.warnings = warnings
        input.knownSecretLiterals = [streamKey, joinPasscode].filter { !$0.isEmpty }
        return input
    }
}

// Locates the core + engine relative to the app bundle (Resources) or the
// repo build tree (dev runs from run-mac-shell.sh).
enum ShellPaths {
    struct Paths {
        var corePath: String?
        var engineBinaryPath: String?
        var pluginHostPath: String?  // corevideo-plugin-host (VST3 scan on macOS)
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
        return paths
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
