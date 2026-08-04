// AppModel — the shell's single observable state root. Runs the 10Hz
// media-core-sync tick + 2Hz zoom-snapshot poll, caches the snapshot slices
// the panes render, surfaces warnings loudly, and owns the command builders.
// Mirrors (in miniature) the WinUI TransportCoordinator/ShowInputsCoordinator
// responsibilities; their characterization tests are the porting spec.

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
    case diagnose = "Diagnose"
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

    var json: JSONObject {
        var object: JSONObject = [
            "routeId": routeId, "mode": mode, "audioRole": audioRole,
            "fitMode": "fill", "opacity": 1.0, "zIndex": zIndex,
        ]
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
    let name: String
    let layout: String
    var layers: [SceneLayer] = []
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
}

// Show-input slot (inputs 1–10): the operator's stable mapping from "what's
// plugged in" to production slots — the ShowInputSlot model from the Windows
// shell (docs/routing-ux-spec.md tab 1). Slots are the AUTHORITY for show
// membership; assignedIds/multiview/scenes derive from them.
struct ShowInputSlot: Identifiable, Equatable {
    let id: Int                 // 1...10
    var kind = "unassigned"     // unassigned | zoom | capture
    var sourceId = ""           // participant id or capture device id
    var name = ""               // editable display name
    var inShow = false
    var iso = false
    var offline = false         // source vanished from the roster/devices
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

struct RosterParticipant: Identifiable, Equatable {
    let id: String
    let name: String
    var hasVideo: Bool
    var muted: Bool
    var talking: Bool
    var assigned: Bool
}

@MainActor
final class AppModel: ObservableObject {
    @Published var status: BridgeStatus = .launching
    @Published var statusDetail = ""
    @Published var meetingState = "idle"
    @Published var rawMediaActive = false
    @Published var roster: [RosterParticipant] = []
    @Published var assignedIds: Set<String> = []
    @Published var recordingStatus = "idle"
    @Published var recordingStartedAt: Date?
    @Published var recordingArtifactPath = ""
    @Published var recordingWarning = ""
    @Published var masterLevel = 0
    @Published var strips: [AudioStrip] = []
    @Published var channelInserts: [String: ChannelInserts] = [:]
    @Published var shortTermLufs = -120.0
    @Published var truePeakDbfs = -120.0
    @Published var limiterActive = false
    @Published var masteringRideDb = 0.0
    @Published var monitorStatus = ""
    @Published var monitorFeedbackRisk = false
    @Published var monitorEnabled = false
    @Published var monitorVolume = 0.7
    @Published var warnings: [String] = []
    @Published var programSurfaceId: UInt32 = 0
    @Published var programFrameNumber: Int64 = 0
    @Published var previewSurfaceId: UInt32 = 0
    @Published var multiviewSurfaceId: UInt32 = 0
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
    @Published var programSceneId = ""
    @Published var previewSceneId = ""
    @Published var mediaAssets: [MediaAsset] = []
    @Published var selectedAssetId = ""
    @Published var logoBug: MediaAsset?  // rides every scene as a top-right route
    @Published var lowerThirdName = ""
    @Published var lowerThirdTitle = ""
    @Published var lowerThirdPosition = "lower-left"
    @Published var lowerThirdPhase = "hidden"  // truthful phase from overlayState
    @Published var overlaysOnAir = 0
    // Director recommendation (core-computed, shell-actuated — the core NEVER
    // switches scenes itself; recommend-auto-production is a documented no-op).
    @Published var autoRuleId = ""
    @Published var autoSceneId = ""
    @Published var autoConfidence = 0
    @Published var autoRationale = ""
    @Published var autoDirectEnabled = false
    @Published var autoTakeEnabled = true
    @Published var autoConfidenceThreshold = 70.0
    @Published var autoHoldSeconds = 4.0
    @Published var autoStatus = "Manual mode"
    @Published var isoRecordingEnabled = false
    @Published var isoSelectedSourceIds: Set<String> = []
    @Published var streamUrl = "rtmp://a.rtmp.youtube.com/live2"
    @Published var streamKey = ""
    @Published var streamingDesired = false
    @Published var streamStatus = ""       // sender status from outputSenderSession
    @Published var streamDetail = ""       // lastResultCode / warning

    private var bridge: MediaCoreBridge?
    private var syncTimer: Timer?
    private var zoomTimer: Timer?
    private var startedAt = Date()
    private var programOutputStarted = false
    private var joinInFlight = false

    private var lastSavedPrefs = ShellPrefs()

    private func currentPrefs() -> ShellPrefs {
        var prefs = ShellPrefs()
        prefs.joinMeetingId = joinMeetingId
        prefs.displayName = displayName
        prefs.monitorEnabled = monitorEnabled
        prefs.monitorVolume = monitorVolume
        prefs.isoRecordingEnabled = isoRecordingEnabled
        prefs.isoSelectedSourceIds = isoSelectedSourceIds.sorted()
        prefs.autoTakeEnabled = autoTakeEnabled
        prefs.autoConfidenceThreshold = autoConfidenceThreshold
        prefs.autoHoldSeconds = autoHoldSeconds
        prefs.lowerThirdName = lowerThirdName
        prefs.lowerThirdTitle = lowerThirdTitle
        prefs.lowerThirdPosition = lowerThirdPosition
        prefs.logoBugAssetId = logoBug?.id ?? ""
        prefs.streamUrl = streamUrl
        prefs.recentMeetings = recentMeetings
        prefs.webinar = webinar
        return prefs
    }

    private func restorePrefs() {
        let prefs = ShellPrefs.load()
        joinMeetingId = prefs.joinMeetingId
        displayName = prefs.displayName
        monitorEnabled = prefs.monitorEnabled
        monitorVolume = prefs.monitorVolume
        isoRecordingEnabled = prefs.isoRecordingEnabled
        isoSelectedSourceIds = Set(prefs.isoSelectedSourceIds)
        autoTakeEnabled = prefs.autoTakeEnabled
        autoConfidenceThreshold = prefs.autoConfidenceThreshold
        autoHoldSeconds = prefs.autoHoldSeconds
        lowerThirdName = prefs.lowerThirdName
        lowerThirdTitle = prefs.lowerThirdTitle
        lowerThirdPosition = prefs.lowerThirdPosition
        if !prefs.logoBugAssetId.isEmpty {
            logoBug = mediaAssets.first { $0.id == prefs.logoBugAssetId }
        }
        streamUrl = prefs.streamUrl
        recentMeetings = prefs.recentMeetings
        webinar = prefs.webinar
        streamKey = StreamKeychain.load()
        lastSavedPrefs = prefs
    }

    func start() {
        refreshMediaBin()
        restorePrefs()
        // Self-snapshot harness: render our own window to PNG (no TCC, no
        // focus games — AX/screen-recording automation proved unreliable) and
        // exit. Pairs with COREVIDEO_SHELL_TAB for per-tab proof shots.
        if let snapshotPath =
            ProcessInfo.processInfo.environment["COREVIDEO_SHELL_SNAPSHOT"] {
            DispatchQueue.main.asyncAfter(deadline: .now() + 8) {
                if let window = NSApplication.shared.windows.first,
                   let view = window.contentView,
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
           let tab = StudioTab.allCases.first(where: {
               $0.rawValue.lowercased() == tabName.lowercased()
           }) {
            selectedTab = tab
        } else {
            refreshZoomSignedIn()
        }  // after the bin so the logo bug can re-resolve
        Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { [weak self] _ in
            Task { @MainActor in
                guard let self else { return }
                let prefs = self.currentPrefs()
                if prefs != self.lastSavedPrefs {
                    prefs.save()
                    self.lastSavedPrefs = prefs
                }
            }
        }
        let paths = ShellPaths.resolve()
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
        let bridge = MediaCoreBridge(corePath: corePath, environmentExtras: env)
        self.bridge = bridge
        bridge.onStatus = { [weak self] status in
            Task { @MainActor in
                self?.status = status
                if case .connected(let renderer) = status {
                    ShellLog.write("core connected renderer=\(renderer)")
                    self?.onConnected()
                }
                if case .exited(let code) = status {
                    self?.pushWarning("media core exited (code \(code)) — relaunching")
                }
            }
        }
        bridge.onEvent = { [weak self] event in
            Task { @MainActor in self?.handleEvent(event) }
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

    // The Studio center is ONE multiview canvas (reference 01-studio): the
    // core composites PREVIEW + PROGRAM tiles INSIDE the multiview texture in
    // the pgmPvw layout modes, plus the live input tiles below.
    @Published var multiviewLayoutMode = "pgmPvwTop"

    func configureMultiviewer(mode: String) {
        multiviewLayoutMode = mode
        guard let bridge else { return }
        Task {
            _ = try? await bridge.request([
                "type": "media-core-sync", "elapsedMs": elapsedMs(),
                "commands": [["type": "configure-multiviewer", "layoutMode": mode]],
            ])
        }
    }

    private func onConnected() {
        configureMultiviewer(mode: multiviewLayoutMode)
        // A scene always sits on PROGRAM (the Windows shell starts on its
        // first scene) — the PGM tile composites from the first frame.
        if programSceneId.isEmpty {
            programSceneId = scenes[0].id
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
        syncTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.syncTick() }
        }
        zoomTimer?.invalidate()
        zoomTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.zoomTick() }
        }
    }

    private func elapsedMs() -> Int {
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

    private func applySnapshot(_ snapshot: JSONObject) {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        clockText = formatter.string(from: Date())
        if let devices = snapshot["captureDevices"] as? [JSONObject] {
            captureDevices = devices.map { entry in
                CaptureDeviceRow(
                    id: entry["id"] as? String ?? "",
                    name: entry["name"] as? String ?? "",
                    kind: entry["kind"] as? String ?? "video",
                    vendor: entry["vendor"] as? String ?? "",
                    connectionState: entry["connectionState"] as? String ?? "detected",
                    signalPresent: entry["signalPresent"] as? Bool ?? false,
                    warning: entry["warning"] as? String ?? "")
            }
            refreshSlotHealth()
        }
        if let recording = snapshot["recording"] as? JSONObject {
            let nextStatus = recording["status"] as? String ?? "idle"
            if nextStatus == "recording", recordingStatus != "recording" {
                recordingStartedAt = Date()
            }
            recordingStatus = nextStatus
            recordingArtifactPath = recording["artifactPath"] as? String ?? recordingArtifactPath
            let warning = recording["warning"] as? String ?? ""
            if !warning.isEmpty, warning != recordingWarning {
                pushWarning("recording: \(warning)")
            }
            recordingWarning = warning
        }
        if let mix = snapshot["audioMixSession"] as? JSONObject {
            masterLevel = mix["masterLevel"] as? Int ?? masterLevel
            limiterActive = mix["limiterActive"] as? Bool ?? limiterActive
            monitorStatus = mix["monitorStatus"] as? String ?? monitorStatus
            monitorFeedbackRisk = mix["monitorFeedbackRisk"] as? Bool ?? monitorFeedbackRisk
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
        if let multiview = snapshot["multiviewSharedTexture"] as? JSONObject,
           let texture = multiview["texture"] as? JSONObject,
           let id = texture["iosurfaceId"] as? NSNumber {
            multiviewSurfaceId = id.uint32Value
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
            } else {
                lowerThirdPhase = "hidden"
            }
        }
    }

    private func applyZoom(_ snapshot: JSONObject) {
        meetingState = snapshot["meetingState"] as? String ?? meetingState
        rawMediaActive = snapshot["rawMediaActive"] as? Bool ?? rawMediaActive
        if let error = snapshot["lastError"] as? String, !error.isEmpty {
            pushWarning("zoom: \(error)")
        }
        if let participants = snapshot["participants"] as? [JSONObject] {
            // Wire shape (ZoomEngineRuntime::rawCaptureSnapshotLocked, same as
            // the WinUI shell consumes): userId/displayName/videoOn/muted/talking.
            roster = participants.compactMap { entry in
                guard let idValue = entry["userId"] ?? entry["id"] else { return nil }
                let id = "\(idValue)"
                return RosterParticipant(
                    id: id,
                    name: entry["displayName"] as? String ?? (entry["name"] as? String ?? id),
                    hasVideo: entry["videoOn"] as? Bool ?? (entry["hasVideo"] as? Bool ?? false),
                    muted: entry["muted"] as? Bool ?? (entry["isMuted"] as? Bool ?? false),
                    talking: entry["talking"] as? Bool ?? (entry["isTalking"] as? Bool ?? false),
                    assigned: assignedIds.contains(id))
            }
            // Auto-assign video participants into empty slots (the Windows
            // AutomationAutoAssignInputsEnabled default) — join → tiles with
            // zero clicks. Manual unassigns are respected via the tombstones.
            let slotted = Set(slots.filter { $0.kind == "zoom" }.map(\.sourceId))
            var assignedAny = false
            for participant in roster where participant.hasVideo
                && !slotted.contains(participant.id)
                && !manuallyUnassigned.contains(participant.id) {
                guard let empty = firstEmptySlotIndex() else { break }
                slots[empty].kind = "zoom"
                slots[empty].sourceId = participant.id
                slots[empty].name = participant.name
                slots[empty].inShow = true
                assignedAny = true
            }
            refreshSlotHealth()
            if assignedAny {
                ShellLog.write("auto-assigned into slots")
                recomputeFromSlots()
            }
        }
    }

    private var manuallyUnassigned: Set<String> = []

    // ── show-input slots (inputs 1–10, the Sources patch bay) ────────────────

    @Published var slots: [ShowInputSlot] = (1...10).map { ShowInputSlot(id: $0) }

    // assignedIds stays the wire-facing derivation (spine/scenes/multiview all
    // read it) — recomputed whenever slots change.
    private func recomputeFromSlots() {
        assignedIds = Set(slots.filter { $0.kind == "zoom" && $0.inShow }
            .map(\.sourceId))
        isoSelectedSourceIds = Set(slots.filter(\.iso).compactMap { slot in
            slot.kind == "zoom" ? "zoom:" + slot.sourceId
                : slot.kind == "capture" ? "capture:" + slot.sourceId : nil
        })
        isoRecordingEnabled = !isoSelectedSourceIds.isEmpty
        syncSpine()
        syncScenes()
    }

    func assignSlot(_ slotId: Int, kind: String, sourceId: String, name: String) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }) else { return }
        slots[index].kind = kind
        slots[index].sourceId = sourceId
        if slots[index].name.isEmpty { slots[index].name = name }
        slots[index].inShow = true
        slots[index].offline = false
        manuallyUnassigned.remove(sourceId)
        recomputeFromSlots()
    }

    func unassignSlot(_ slotId: Int) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }) else { return }
        if slots[index].kind == "zoom" {
            manuallyUnassigned.insert(slots[index].sourceId)  // no auto re-add
        }
        slots[index] = ShowInputSlot(id: slotId)
        recomputeFromSlots()
    }

    func toggleSlotInShow(_ slotId: Int) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }),
              slots[index].kind != "unassigned" else { return }
        slots[index].inShow.toggle()
        if slots[index].kind == "zoom" {
            if slots[index].inShow {
                manuallyUnassigned.remove(slots[index].sourceId)
            } else {
                manuallyUnassigned.insert(slots[index].sourceId)
            }
        }
        recomputeFromSlots()
    }

    func toggleSlotIso(_ slotId: Int) {
        guard let index = slots.firstIndex(where: { $0.id == slotId }) else { return }
        slots[index].iso.toggle()
        recomputeFromSlots()
    }

    // Keep slot health truthful as rosters/devices churn.
    private func refreshSlotHealth() {
        for index in slots.indices {
            switch slots[index].kind {
            case "zoom":
                slots[index].offline = !roster.contains { $0.id == slots[index].sourceId }
            case "capture":
                slots[index].offline = !captureDevices.contains {
                    $0.id == slots[index].sourceId && $0.connectionState == "connected"
                }
            default:
                break
            }
        }
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
        case "preview-shared-texture":
            if let id = event["iosurfaceId"] as? NSNumber {
                previewSurfaceId = id.uint32Value
            }
        default:
            break
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
    @Published var webinar = false
    @Published var recentMeetings: [String] = []

    func signInWithZoom() {
        zoomOAuthStatus = "Waiting for the browser…"
        Task { await ZoomOAuth.shared.beginSignIn() }
    }

    func signOutZoom() {
        Task {
            await ZoomOAuth.shared.signOut()
            zoomSignedIn = false
            zoomOAuthStatus = ""
        }
    }

    func handleOAuthCallback(_ url: URL) {
        Task {
            let error = await ZoomOAuth.shared.handleCallback(url)
            zoomOAuthStatus = error
            zoomSignedIn = await ZoomOAuth.shared.signedIn
            if error.isEmpty {
                ShellLog.write("zoom sign-in complete")
            } else {
                pushWarning("zoom sign-in: \(error)")
            }
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

    func joinZoom() {
        guard let bridge else { return }
        let target = Self.parseZoomTarget(joinMeetingId, fallbackPasscode: joinPasscode)
        let meetingId = target.meetingId
        let parsedPasscode = target.passcode
        joinPasscode = parsedPasscode
        guard !meetingId.isEmpty, !joinInFlight else { return }
        joinInFlight = true
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
        ShellLog.write("joining meeting \(meetingId)")
        Task {
            defer { joinInFlight = false }
            do {
                // Credentials BEFORE the request timer (the Windows shape):
                // a fresh ZAK per join, never cached, never logged.
                if let credentials = await ZoomOAuth.shared.ensureJoinCredentials(),
                   let zak = credentials.userZak {
                    payload["userZak"] = zak
                }
                let response = try await bridge.request(
                    ["type": "zoom-join", "payload": payload], timeout: 45)
                if let snapshot = response["snapshot"] as? JSONObject {
                    applyZoom(snapshot)
                    for warning in snapshot["warnings"] as? [String] ?? [] {
                        pushWarning("zoom: \(warning)")
                    }
                }
            } catch {
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
    private func syncSpine() {
        guard let bridge else { return }
        let participants: [JSONObject] = roster.map { participant in
            [
                "sdkUserId": participant.id,
                "displayName": participant.name,
                "role": "guest",
                "videoOn": participant.hasVideo,
                "muted": participant.muted,
                "talking": participant.talking,
            ]
        }
        let subscriptions: [JSONObject] = assignedIds.enumerated().map { index, id in
            [
                "participantId": id,
                "kind": "video",
                "purpose": "program",
                "priority": index,
            ]
        }
        // Multiview sources = the in-show slots, in slot order.
        let multiviewSources: [JSONObject] = slots
            .filter { $0.kind != "unassigned" && $0.inShow }
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
                        "kind": "capture-input",
                        "captureDeviceId": slot.sourceId,
                        "label": slot.name,
                    ]
            }
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
        Task {
            do {
                _ = try await bridge.request([
                    "type": "zoom-media-spine-sync", "spinePayload": payload,
                    "elapsedMs": elapsedMs(),
                ])
            } catch {
                pushWarning("assign failed: \(error.localizedDescription)")
            }
        }
    }

    // ── scenes + Take ────────────────────────────────────────────────────────

    // Rects lifted from SceneCanvasLayoutService: single, two-up (0.02 gap),
    // pip (full frame + 0.28 corner inset).
    private static let layoutRects: [String: [(Double, Double, Double, Double)]] = [
        "single": [(0, 0, 1, 1)],
        "two-up": [(0, 0, 0.49, 1), (0.51, 0, 0.49, 1)],
        "pip": [(0, 0, 1, 1), (0.68, 0.68, 0.28, 0.28)],
    ]

    private func buildRoutes(for sceneId: String) -> [JSONObject] {
        guard let scene = scenes.first(where: { $0.id == sceneId }),
              let rects = Self.layoutRects[scene.layout] else { return [] }
        let inShow = slots.filter { $0.kind != "unassigned" && $0.inShow }
        // An edited canvas OWNS its scene: layers carry their own rects, fit
        // and opacity, and bind to a slot (nil = active speaker).
        if !scene.layers.isEmpty {
            var edited: [JSONObject] = scene.layers.enumerated().map { index, layer in
                var route = SceneRoute(
                    routeId: "\(sceneId)-layer-\(index)", mode: "active-speaker",
                    participantId: nil, captureDeviceId: nil,
                    rect: (layer.x, layer.y, layer.width, layer.height), zIndex: index)
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
        let inShow = slots.filter { $0.kind != "unassigned" && $0.inShow }
        scenes[index].layers = rects.enumerated().map { slot, rect in
            SceneLayer(id: "\(sceneId)-layer-\(slot)",
                       slotId: slot < inShow.count ? inShow[slot].id : nil,
                       x: rect.0, y: rect.1, width: rect.2, height: rect.3)
        }
    }

    func updateLayer(sceneId: String, layerId: String,
                     _ apply: (inout SceneLayer) -> Void) {
        guard let sceneIndex = scenes.firstIndex(where: { $0.id == sceneId }),
              let layerIndex = scenes[sceneIndex].layers
                  .firstIndex(where: { $0.id == layerId }) else { return }
        apply(&scenes[sceneIndex].layers[layerIndex])
        syncScenes()
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

    // Back to the layout preset (drops the edited canvas).
    func resetLayers(for sceneId: String) {
        guard let index = scenes.firstIndex(where: { $0.id == sceneId }) else { return }
        scenes[index].layers = []
        syncScenes()
    }

    func selectPreviewScene(_ sceneId: String) {
        // Mirrors StudioViewModel.SelectScene: selection ONLY arms preview —
        // program changes exclusively on Take.
        previewSceneId = sceneId
        syncScenes()
    }

    func take() {
        // There is no `take` wire command: Take = swap the two scene ids and
        // resend both scene graphs in one batch (TransportCoordinator shape).
        guard !previewSceneId.isEmpty, previewSceneId != programSceneId else { return }
        let taken = previewSceneId
        previewSceneId = programSceneId
        programSceneId = taken
        syncScenes()
    }

    func syncScenes() {
        guard let bridge else { return }
        var commands: [JSONObject] = []
        if !programSceneId.isEmpty {
            commands.append([
                "type": "load-scene-graph", "sceneId": programSceneId,
                "routes": buildRoutes(for: programSceneId),
            ])
        }
        if !previewSceneId.isEmpty {
            commands.append([
                "type": "set-preview-scene", "sceneId": previewSceneId,
                "routes": buildRoutes(for: previewSceneId),
            ])
        }
        guard !commands.isEmpty else { return }
        Task {
            do {
                _ = try await bridge.request([
                    "type": "media-core-sync", "elapsedMs": elapsedMs(),
                    "commands": commands,
                ])
            } catch {
                pushWarning("scene sync failed: \(error.localizedDescription)")
            }
        }
    }

    // ── auto-direct (the MagicScene policy ladder, reduced) ──────────────────

    // The core's director speaks its own scene ids; the shell owns the map to
    // its catalog (the core doesn't care which sceneId gets loaded).
    private static let directorSceneMap = [
        "intro": "fullscreen", "interview": "two-up",
        "speaker-slides": "pip", "panel": "two-up",
    ]

    private var autoTimer: Timer?
    private var pendingAutoSceneId = ""
    private var pendingAutoSince = Date.distantPast

    func setAutoDirect(enabled: Bool) {
        autoDirectEnabled = enabled
        pendingAutoSceneId = ""
        autoTimer?.invalidate()
        autoTimer = nil
        guard enabled else {
            autoStatus = "Manual mode"
            return
        }
        autoTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.evaluateAutoDirect() }
        }
    }

    private func evaluateAutoDirect() {
        guard autoDirectEnabled else { return }
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
            take()
            autoStatus = "Took \(target) (\(autoRuleId))"
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
        if isoColumnAssignments[column] == sourceId {
            isoColumnAssignments[column] = nil
        } else {
            // Exclusive per column AND one column per source.
            if let previous = isoColumn(of: sourceId) {
                isoColumnAssignments[previous] = nil
            }
            isoColumnAssignments[column] = sourceId
        }
        // The recording payload reads the flat selection; matrix is authority.
        isoSelectedSourceIds = Set(isoColumnAssignments.values)
        isoRecordingEnabled = !isoColumnAssignments.isEmpty
        for index in slots.indices where slots[index].kind != "unassigned" {
            let sid = (slots[index].kind == "zoom" ? "zoom:" : "capture:")
                + slots[index].sourceId
            slots[index].iso = isoSelectedSourceIds.contains(sid)
        }
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
    func resolvedIsoSourceIds() -> [String] {
        guard isoRecordingEnabled else { return [] }
        let present = Set(roster.map { "zoom:" + $0.id })
            .union(captureDevices.filter { $0.connectionState == "connected" }
                .map { "capture:" + $0.id })
        return isoSelectedSourceIds.sorted().filter(present.contains).prefix(8).map { $0 }
    }

    func toggleIsoSource(_ sourceId: String) {
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
            "fps": 30, "targetBitrateMbps": 6.0, "audioBitrateKbps": 160,
            "videoCodec": "h264", "encoderMode": "auto",
            "keyframeIntervalSeconds": 2.0, "rateControl": "cbr",
            "h264Profile": "high", "bFrames": 2, "allowEnhancedRtmp": false,
        ]]
    }

    // start-program-output is FULL-STATE for the destination set, so record
    // and stream always resend the union of what should be live.
    private func startProgramOutput(recording: Bool, streaming: Bool) -> JSONObject {
        var destinations: [String] = []
        if recording { destinations.append("recording") }
        if streaming { destinations.append("rtmp") }
        let isoIds = resolvedIsoSourceIds()
        var command: JSONObject = [
            "type": "start-program-output", "destinations": destinations,
            "isoSourceIds": isoIds,
            "isoParticipantIds": isoIds.filter { $0.hasPrefix("zoom:") }
                .map { String($0.dropFirst(5)) },
        ]
        if streaming {
            command["streamOutputProfile"] = [
                "width": 1920, "height": 1080, "fps": 30,
                "targetBitrateMbps": 6.0, "videoCodec": "h264",
            ]
            command["destinationSettings"] = streamDestinationSettings()
        }
        return command
    }

    private var isRecordingActive: Bool {
        recordingStatus == "recording" || recordingStatus == "warning"
    }

    func toggleStreaming() {
        guard let bridge else { return }
        if streamingDesired {
            streamingDesired = false
            let commands: [JSONObject] = isRecordingActive
                ? [startProgramOutput(recording: true, streaming: false)]
                : [["type": "stop-encoder-session",
                    "reason": "Streaming stopped by operator."]]
            Task {
                _ = try? await bridge.request([
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
                _ = try await bridge.request([
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
            }
        }
        busesForSource[bus] = 0
        audioSends[source] = busesForSource
        selectedSend = (source, bus)
        syncAudioRouting()
    }

    func removeSend(source: String, bus: String) {
        audioSends[source]?[bus] = nil
        if selectedSend?.source == source, selectedSend?.bus == bus { selectedSend = nil }
        syncAudioRouting()
    }

    func setSendGain(source: String, bus: String, gainDb: Double) {
        guard audioSends[source]?[bus] != nil else { return }
        audioSends[source]?[bus] = max(-60, min(10, gainDb))
        syncAudioRouting()
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
        guard let sends = matrix["sends"] as? [JSONObject], !sends.isEmpty else { return }
        var next: [String: [String: Double]] = [:]
        for entry in sends {
            guard let source = entry["sourceId"] as? String,
                  let bus = entry["busId"] as? String else { continue }
            // Don't fight the operator mid-edit.
            if selectedSend?.source == source, selectedSend?.bus == bus,
               let existing = audioSends[source]?[bus] {
                next[source, default: [:]][bus] = existing
                continue
            }
            next[source, default: [:]][bus] =
                (entry["gainDb"] as? NSNumber)?.doubleValue ?? 0
        }
        audioSends = next
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

    func showLowerThird() {
        guard !lowerThirdName.isEmpty else { return }
        // building-in only — the core auto-advances to on-air itself.
        sendOverlay([
            "type": "set-overlay-asset", "overlayId": "key:lower-third",
            "text": lowerThirdName, "position": "lower-third", "enabled": true,
            "sourceId": "mac:manual", "sourceName": lowerThirdName,
            "title": lowerThirdTitle, "org": "",
            "keyPosition": lowerThirdPosition, "keyPhase": "building-in",
            "buildInMs": 250, "buildOutMs": 200, "keyer": "downstream",
        ])
    }

    func hideLowerThird() {
        // Two-step retire: shell-timed build-out, then the hidden ack (an
        // enabled:false WITHOUT keyPhase:"hidden" would start a second native
        // build-out — the WinUI RunLowerThirdKeyOutAsync shape).
        sendOverlay([
            "type": "set-overlay-asset", "overlayId": "key:lower-third",
            "text": lowerThirdName, "position": "lower-third", "enabled": true,
            "sourceId": "mac:manual", "sourceName": lowerThirdName,
            "title": lowerThirdTitle, "org": "",
            "keyPosition": lowerThirdPosition, "keyPhase": "building-out",
            "buildInMs": 250, "buildOutMs": 200, "keyer": "downstream",
        ])
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 220_000_000)
            self?.sendOverlay([
                "type": "set-overlay-asset", "overlayId": "key:lower-third",
                "text": "", "position": "lower-third", "enabled": false,
                "keyPhase": "hidden", "buildInMs": 250, "buildOutMs": 200,
                "keyer": "downstream",
            ])
        }
    }

    private func sendOverlay(_ command: JSONObject) {
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

    private func applyMixParticipants(_ participants: [JSONObject]) {
        var next: [AudioStrip] = []
        for entry in participants {
            guard let id = entry["participantId"] as? String, !id.isEmpty else { continue }
            let editing = stripEditTimes[id].map { Date().timeIntervalSince($0) < 2 } ?? false
            let existing = strips.first { $0.id == id }
            if editing, var held = existing {
                held.outputLevel = entry["outputLevel"] as? Int ?? held.outputLevel
                held.status = entry["status"] as? String ?? held.status
                next.append(held)
                continue
            }
            next.append(AudioStrip(
                id: id,
                outputLevel: entry["outputLevel"] as? Int ?? 0,
                manualGainDb: (entry["manualGainDb"] as? NSNumber)?.doubleValue
                    ?? existing?.manualGainDb ?? 0,
                muted: entry["muted"] as? Bool ?? false,
                solo: entry["solo"] as? Bool ?? false,
                pan: (entry["pan"] as? NSNumber)?.doubleValue ?? 0,
                status: entry["status"] as? String ?? ""))
        }
        strips = next
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
        stripEditTimes[id] = Date()
        scheduleMixSync()
    }

    // Debounced FULL-channel emit: sync-participant-audio-mix is full-state
    // with a 25-sync hold-last guard core-side; never send a partial list.
    private func scheduleMixSync() {
        mixSyncTask?.cancel()
        mixSyncTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 150_000_000)
            guard !Task.isCancelled else { return }
            await self?.syncAudioMix()
        }
    }

    private func syncAudioMix() async {
        guard let bridge, !strips.isEmpty else { return }
        let channels: [JSONObject] = strips.map { strip in
            let inserts = channelInserts[strip.id] ?? ChannelInserts()
            return [
                "participantId": strip.id,
                "muted": strip.muted,
                "manualGainDb": max(-24, min(24, strip.manualGainDb)),
                "pan": max(-1, min(1, strip.pan)),
                "solo": strip.solo,
                "noiseSuppression": false,
                "pluginInserts": inserts.activeNames,
                "insertSettings": inserts.settingsJson,
            ]
        }
        do {
            _ = try await bridge.request([
                "type": "media-core-sync", "elapsedMs": elapsedMs(),
                "commands": [
                    ["type": "sync-participant-audio-mix", "limiterEnabled": true,
                     "mastering": mastering.json, "channels": channels]
                ],
            ])
        } catch {
            pushWarning("audio mix sync failed: \(error.localizedDescription)")
        }
    }

    func toggleRecording() {
        guard let bridge else { return }
        let stop = recordingStatus == "recording" || recordingStatus == "warning"
        Task {
            do {
                if stop {
                    _ = try await bridge.request([
                        "type": "media-core-sync", "elapsedMs": elapsedMs(),
                        "commands": [["type": "stop-recording-session"]],
                    ])
                } else {
                    // Arm the encoder ONLY here: start-program-output flips the
                    // core into per-tick full readbacks (by design, for the
                    // encoder feed) — armed-at-startup starved the RPC thread.
                    let folder = (NSSearchPathForDirectoriesInDomains(
                        .moviesDirectory, .userDomainMask, true).first ?? NSTemporaryDirectory())
                        + "/CoreVideoPro"
                    // The core refuses (loudly) rather than mkdir -p'ing.
                    try? FileManager.default.createDirectory(
                        atPath: folder, withIntermediateDirectories: true)
                    // Always send the iso keys (empty array = disarm): omitting
                    // them PRESERVES the previous selection core-side.
                    let isoIds = resolvedIsoSourceIds()
                    let isoLegacy = isoIds.filter { $0.hasPrefix("zoom:") }
                        .map { String($0.dropFirst(5)) }
                    _ = try await bridge.request([
                        "type": "media-core-sync", "elapsedMs": elapsedMs(),
                        "commands": [
                            startProgramOutput(recording: true, streaming: streamingDesired),
                            [
                                "type": "set-recording-targets", "targetFolder": folder,
                                "filenamePrefix": "show", "format": "mp4", "quality": "high",
                                "isoSourceIds": isoIds, "isoParticipantIds": isoLegacy,
                            ],
                            ["type": "start-recording-session", "sessionId": UUID().uuidString,
                             "isoSourceIds": isoIds, "isoParticipantIds": isoLegacy],
                        ],
                    ])
                }
            } catch {
                pushWarning("recording command failed: \(error.localizedDescription)")
            }
        }
    }

    func setMonitor(enabled: Bool, volume: Double) {
        guard let bridge else { return }
        monitorEnabled = enabled
        monitorVolume = volume
        Task {
            _ = try? await bridge.request([
                "type": "media-core-sync", "elapsedMs": elapsedMs(),
                "commands": [
                    [
                        "type": "sync-audio-monitor", "enabled": enabled, "deviceId": "",
                        "deviceName": "System default output", "volume": volume,
                    ]
                ],
            ])
        }
    }

    func connectCaptureDevice(_ device: CaptureDeviceRow) {
        guard let bridge else { return }
        Task {
            do {
                // Both commands take a `payload` wrapper (JsonRpcServer).
                if device.connectionState == "connected" {
                    _ = try await bridge.request([
                        "type": "disconnect-capture-device",
                        "payload": ["deviceId": device.id],
                    ])
                } else {
                    _ = try await bridge.request([
                        "type": "connect-capture-device",
                        "payload": ["deviceId": device.id, "outputSourceId": device.id],
                    ])
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
}

// Locates the core + engine relative to the app bundle (Resources) or the
// repo build tree (dev runs from run-mac-shell.sh).
enum ShellPaths {
    struct Paths {
        var corePath: String?
        var engineBinaryPath: String?
    }

    static func resolve() -> Paths {
        var paths = Paths()
        let bundleResources = Bundle.main.resourcePath ?? ""
        let candidates = [
            bundleResources + "/corevideo-native",
            repoRoot() + "/native/build-metal/corevideo-native",
        ]
        paths.corePath = candidates.first { FileManager.default.isExecutableFile(atPath: $0) }
        let engineCandidates = [
            bundleResources + "/corevideo-zoom-engine.app/Contents/MacOS/corevideo-zoom-engine",
            repoRoot()
                + "/native/build-engine/corevideo-zoom-engine.app/Contents/MacOS/corevideo-zoom-engine",
        ]
        paths.engineBinaryPath = engineCandidates.first {
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
        if let handle = FileHandle(forWritingAtPath: path) {
            handle.seekToEndOfFile()
            handle.write(Data(line.utf8))
            try? handle.close()
        } else {
            try? line.write(toFile: path, atomically: true, encoding: .utf8)
        }
    }
}
