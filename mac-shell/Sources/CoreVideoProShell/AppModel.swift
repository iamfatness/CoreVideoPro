// AppModel — the shell's single observable state root. Runs the 10Hz
// media-core-sync tick + 2Hz zoom-snapshot poll, caches the snapshot slices
// the panes render, surfaces warnings loudly, and owns the command builders.
// Mirrors (in miniature) the WinUI TransportCoordinator/ShowInputsCoordinator
// responsibilities; their characterization tests are the porting spec.

import Foundation
import SwiftUI

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
    @Published var joinMeetingId = ""
    @Published var joinPasscode = ""
    @Published var displayName = "CoreVideo Pro (mac)"

    private var bridge: MediaCoreBridge?
    private var syncTimer: Timer?
    private var zoomTimer: Timer?
    private var startedAt = Date()
    private var programOutputStarted = false

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
                    print("shell: core connected renderer=\(renderer)")
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
        startedAt = Date()
        programOutputStarted = false
        syncTimer?.invalidate()
        syncTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.syncTick() }
        }
        zoomTimer?.invalidate()
        zoomTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.zoomTick() }
        }
    }

    private func elapsedMs() -> Int {
        Int(Date().timeIntervalSince(startedAt) * 1000)
    }

    // ── ticks ────────────────────────────────────────────────────────────────

    private func syncTick() async {
        guard let bridge else { return }
        var commands: [JSONObject] = []
        if !programOutputStarted {
            programOutputStarted = true
            commands.append(["type": "start-program-output", "destinations": ["recording"]])
        }
        do {
            let response = try await bridge.request([
                "type": "media-core-sync", "elapsedMs": elapsedMs(), "commands": commands,
            ])
            applySnapshot(response["snapshot"] as? JSONObject ?? [:])
        } catch {
            statusDetail = error.localizedDescription
        }
    }

    private func zoomTick() async {
        guard let bridge else { return }
        guard let response = try? await bridge.request(["type": "zoom-snapshot"]),
              let snapshot = response["snapshot"] as? JSONObject
        else { return }
        applyZoom(snapshot)
    }

    private func applySnapshot(_ snapshot: JSONObject) {
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
        if let participants = snapshot["participants"] as? [JSONObject] {
            roster = participants.compactMap { entry in
                guard let idNumber = entry["id"] else { return nil }
                let id = "\(idNumber)"
                return RosterParticipant(
                    id: id,
                    name: entry["name"] as? String ?? id,
                    hasVideo: entry["hasVideo"] as? Bool ?? (entry["has_video"] as? Bool ?? false),
                    muted: entry["isMuted"] as? Bool ?? (entry["is_muted"] as? Bool ?? false),
                    talking: entry["isTalking"] as? Bool ?? (entry["is_talking"] as? Bool ?? false),
                    assigned: assignedIds.contains(id))
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
        print("shell: warning: \(message)")
        warnings.insert(message, at: 0)
        if warnings.count > 20 {
            warnings.removeLast()
        }
    }

    // ── operator commands ────────────────────────────────────────────────────

    func joinZoom() {
        guard let bridge else { return }
        let meetingId = joinMeetingId.trimmingCharacters(in: .whitespaces)
        guard !meetingId.isEmpty else { return }
        let payload: JSONObject = [
            "meetingId": meetingId,
            "meetingNumber": meetingId,
            "passcode": joinPasscode,
            "password": joinPasscode,
            "displayName": displayName,
        ]
        Task {
            do {
                _ = try await bridge.request(["type": "zoom-join", "payload": payload], timeout: 30)
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
        let payload: JSONObject = [
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
                    let folder = (NSSearchPathForDirectoriesInDomains(
                        .moviesDirectory, .userDomainMask, true).first ?? NSTemporaryDirectory())
                        + "/CoreVideoPro"
                    _ = try await bridge.request([
                        "type": "media-core-sync", "elapsedMs": elapsedMs(),
                        "commands": [
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
