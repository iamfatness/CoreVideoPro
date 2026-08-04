// AppModel — the shell's single observable state root. Runs the 10Hz
// media-core-sync tick + 2Hz zoom-snapshot poll, caches the snapshot slices
// the panes render, surfaces warnings loudly, and owns the command builders.
// Mirrors (in miniature) the WinUI TransportCoordinator/ShowInputsCoordinator
// responsibilities; their characterization tests are the porting spec.

import Foundation
import SwiftUI

enum StudioTab: String, CaseIterable {
    case zoom = "Zoom"
    case sources = "Sources"
    case audio = "Audio"
    case diagnose = "Diagnose"
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
    @Published var recordingArtifactPath = ""
    @Published var recordingWarning = ""
    @Published var masterLevel = 0
    @Published var monitorEnabled = false
    @Published var monitorVolume = 0.7
    @Published var warnings: [String] = []
    @Published var programSurfaceId: UInt32 = 0
    @Published var programFrameNumber: Int64 = 0
    @Published var previewSurfaceId: UInt32 = 0
    @Published var multiviewSurfaceId: UInt32 = 0
    @Published var captureDevices: [CaptureDeviceRow] = []
    @Published var clockText = ""
    @Published var selectedTab: StudioTab = .zoom
    @Published var joinMeetingId = ""
    @Published var joinPasscode = ""
    @Published var displayName = "CoreVideo Pro (mac)"

    private var bridge: MediaCoreBridge?
    private var syncTimer: Timer?
    private var zoomTimer: Timer?
    private var startedAt = Date()
    private var programOutputStarted = false
    private var joinInFlight = false

    func start() {
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
            // Core stderr is the diagnostic firehose; keep it on OUR stderr so
            // `log stream`/Console users see it, without flooding the UI.
            FileHandle.standardError.write(Data((line + "\n").utf8))
        }
        bridge.start()
    }

    private func onConnected() {
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
        }
        if let recording = snapshot["recording"] as? JSONObject {
            recordingStatus = recording["status"] as? String ?? "idle"
            recordingArtifactPath = recording["artifactPath"] as? String ?? recordingArtifactPath
            let warning = recording["warning"] as? String ?? ""
            if !warning.isEmpty, warning != recordingWarning {
                pushWarning("recording: \(warning)")
            }
            recordingWarning = warning
        }
        if let mix = snapshot["audioMixSession"] as? JSONObject {
            masterLevel = mix["masterLevel"] as? Int ?? masterLevel
        }
        if let preview = snapshot["programFramePreview"] as? JSONObject,
           let texture = preview["sharedTexture"] as? JSONObject {
            applySharedTexture(texture)
        }
    }

    private func applyZoom(_ snapshot: JSONObject) {
        meetingState = snapshot["meetingState"] as? String ?? meetingState
        rawMediaActive = snapshot["rawMediaActive"] as? Bool ?? rawMediaActive
        if let error = snapshot["lastError"] as? String, !error.isEmpty {
            pushWarning("zoom: \(error)")
        }
        let autoAssign = ProcessInfo.processInfo.environment["COREVIDEO_SHELL_AUTOJOIN"] != nil
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
            if autoAssign {
                let videoIds = Set(roster.filter(\.hasVideo).map(\.id))
                if !videoIds.isEmpty, videoIds != assignedIds {
                    assignedIds = videoIds
                    print("shell: auto-assigning \(videoIds.sorted())")
                    syncSpine()
                }
            }
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

    func joinZoom() {
        guard let bridge else { return }
        let target = Self.parseZoomTarget(joinMeetingId, fallbackPasscode: joinPasscode)
        let meetingId = target.meetingId
        let parsedPasscode = target.passcode
        joinPasscode = parsedPasscode
        guard !meetingId.isEmpty, !joinInFlight else { return }
        joinInFlight = true
        let payload: JSONObject = [
            "meetingId": meetingId,
            "meetingNumber": meetingId,
            "passcode": parsedPasscode,
            "password": parsedPasscode,
            "displayName": displayName,
        ]
        print("shell: joining meeting \(meetingId)")
        Task {
            defer { joinInFlight = false }
            do {
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
        if assignedIds.contains(participant.id) {
            assignedIds.remove(participant.id)
        } else {
            assignedIds.insert(participant.id)
        }
        syncSpine()
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
        let multiviewSources: [JSONObject] = roster
            .filter { assignedIds.contains($0.id) }
            .map { participant in
                [
                    "sourceId": "zoom:" + participant.id,
                    "kind": "participant-video",
                    "participantId": participant.id,
                    "label": participant.name,
                ]
            }
        let payload: JSONObject = [
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
                    _ = try await bridge.request([
                        "type": "media-core-sync", "elapsedMs": elapsedMs(),
                        "commands": [
                            ["type": "start-program-output", "destinations": ["recording"]],
                            [
                                "type": "set-recording-targets", "targetFolder": folder,
                                "filenamePrefix": "show", "format": "mp4", "quality": "high",
                            ],
                            ["type": "start-recording-session", "sessionId": UUID().uuidString],
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
                if device.connectionState == "connected" {
                    _ = try await bridge.request(
                        ["type": "disconnect-capture-device", "deviceId": device.id])
                } else {
                    _ = try await bridge.request([
                        "type": "connect-capture-device", "deviceId": device.id,
                        "outputSourceId": device.id,
                    ])
                }
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
