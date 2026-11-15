// ControlSurface.swift — maps the control API's stable action ids onto the SAME
// AppModel entry points the Studio buttons and keyboard use (Windows
// `StudioControlSurface.DispatchAsync`), plus the server's app lifecycle.
//
// Nothing here re-implements transport logic: Take is `AppModel.take()` (in-
// flight guard, one batch, #286 rollback), FTB is `setProgramBlackout` (Windows
// admission), scene selection is `selectPreviewScene`, Record/Stream are the
// operator toggles (which already refuse while the quit flow is finishing).
// The adapter only adds Windows' remote-specific wrappers: RunTake's CanTake
// gate, SceneSelectArgs' refuse-closed check, RunTransportToggle/RunOutputSet.

import AppKit
import Combine
import Foundation

/// The last core snapshot and when the shell received it, readable from the
/// control server's queue without touching the main actor.
final class ControlSnapshotBox: @unchecked Sendable {
    private let lock = NSLock()
    private var state: JSONObject?
    private var receivedAt: Date?

    func publish(_ state: JSONObject, at date: Date) {
        lock.lock(); defer { lock.unlock() }
        self.state = state
        receivedAt = date
    }

    func read() -> (JSONObject, Date)? {
        lock.lock(); defer { lock.unlock() }
        guard let state, let receivedAt else { return nil }
        return (state, receivedAt)
    }
}

final class AppModelControlSurface: ControlSurface, @unchecked Sendable {
    private weak var model: AppModel?
    private let box: ControlSnapshotBox

    init(model: AppModel, box: ControlSnapshotBox) {
        self.model = model
        self.box = box
    }

    var observesCore: Bool { true }

    func snapshotObservation() throws -> ControlSnapshotObservation {
        guard let (state, receivedAt) = box.read() else {
            return .unavailable("The shell has not received a core snapshot yet (engine off, or the core has not synced).")
        }
        return ControlSnapshotObservation(state: state, receivedAt: receivedAt)
    }

    func invoke(_ actionId: String, _ args: [Any?]) async -> ControlInvokeResult {
        // Hop to the main actor for every model touch (Windows marshals onto the
        // UI dispatcher). `args` are plain values the router already bound.
        let values = args.map { ControlArg($0) }
        return await Task { @MainActor [weak self] in
            guard let self, let model = self.model else { return .fail("Control surface is shutting down.") }
            return await Self.dispatch(model, actionId, values)
        }.value
    }

    func controlState() async -> [String: Any] {
        await Task { @MainActor [weak self] in
            self?.model.map(Self.state) ?? SendableDictionary([:])
        }.value.dictionary
    }

    // ── dispatch (main actor) ────────────────────────────────────────────────

    static let takeUnavailable =
        "Take is unavailable. Queue a valid Preview scene or a changed draft and wait for any current Take to finish."

    @MainActor
    private static func dispatch(_ model: AppModel, _ actionId: String, _ args: [ControlArg]) async -> ControlInvokeResult {
        func bool(_ index: Int) -> Bool { index < args.count ? args[index].bool : false }
        func string(_ index: Int) -> String { index < args.count ? args[index].string : "" }
        switch actionId {
        case "transport.ftb.toggle":
            return blackout(await model.setProgramBlackout(nil))
        case "transport.ftb.set":
            return blackout(await model.setProgramBlackout(bool(0)))
        case "transport.take":
            // Windows RunTake: refuse when CanTake is false, else the one Take path.
            guard model.canTake else { return .fail(takeUnavailable) }
            switch await model.take() {
            case .success: return .success
            case .failed(let why): return .fail(why.isEmpty ? "Take was not confirmed by the media core." : why)
            }
        case "transport.record.toggle":
            return toggle(model, what: "recording", desired: { $0.recordingDesired }, model.toggleRecording)
        case "transport.record.set":
            return outputSet(model, what: "recording", target: bool(0),
                             desired: { $0.recordingDesired }, observedLive: model.recordingObservedLive,
                             // toggleRecording stops when media is still live even with intent off
                             // (RecordingCommandPolicy.begin), so a stop retry is safe; a START while
                             // the previous take is still live would invert into a stop.
                             canInvert: { target in !(target && model.recordingObservedLive) },
                             model.toggleRecording)
        case "transport.stream.toggle":
            return toggle(model, what: "streaming", desired: { $0.streamingDesired }, model.toggleStreaming)
        case "transport.stream.set":
            return outputSet(model, what: "streaming", target: bool(0),
                             desired: { $0.streamingDesired }, observedLive: model.streamingObservedLive,
                             // toggleStreaming flips intent only: with intent off it would START.
                             canInvert: { target in target || !model.streamingObservedLive || model.streamingDesired },
                             model.toggleStreaming)
        case "scene.select":
            // Windows SceneSelectArgs.Refuse: blank or unknown -> refuse, change nothing.
            let sceneId = string(0)
            guard !sceneId.trimmingCharacters(in: .whitespaces).isEmpty,
                  model.scenes.contains(where: { $0.id == sceneId }) else {
                return .fail("Unknown scene '\(sceneId)'.")
            }
            model.selectPreviewScene(sceneId)
            return model.previewSceneId == sceneId
                ? .appliedPreview(sceneId) : .fail("Scene '\(sceneId)' was not queued on Preview.")
        case "scene.list":
            return .withData(sceneListJSON(model))
        default:
            // Unreachable through the router (the catalog binds only the subset).
            return .fail("Unknown action '\(actionId)'.")
        }
    }

    @MainActor
    private static func blackout(_ result: AppModel.BlackoutResult) -> ControlInvokeResult {
        switch result {
        case .success: return .success
        case .failed(let why): return .fail(why)
        }
    }

    /// Windows `RunTransportToggle`: refused when the control is unavailable.
    /// The Mac toggles are fire-and-forget, so "executed" is proven by the
    /// intent actually flipping (a stream with no URL/key refuses in place).
    @MainActor
    private static func toggle(_ model: AppModel, what: String, desired: (AppModel) -> Bool,
                               _ command: () -> Void) -> ControlInvokeResult {
        guard model.canSetOutputs else {
            return .fail("Cannot toggle \(what) right now (the control is unavailable in the current state).")
        }
        let before = desired(model)
        command()
        if desired(model) == before {
            if what == "streaming" && !before {
                return .fail("Cannot toggle streaming right now: the stream URL and key are required.")
            }
            return .fail("Cannot toggle \(what) right now (the control is unavailable in the current state).")
        }
        return .success
    }

    /// Windows `RunOutputSet`: an identical request is an idempotent success —
    /// except a Stop is retried while media is still live (a disarmed intent is
    /// not proof the previous Stop reached the core).
    @MainActor
    private static func outputSet(_ model: AppModel, what: String, target: Bool, desired: (AppModel) -> Bool,
                                  observedLive: Bool, canInvert: (Bool) -> Bool,
                                  _ command: () -> Void) -> ControlInvokeResult {
        if desired(model) == target && (target || !observedLive) { return .success }
        guard model.canSetOutputs, canInvert(target) else {
            return .fail("Cannot set \(what) right now (the control is unavailable in the current state).")
        }
        let before = desired(model)
        command()
        if !target && desired(model) == false { return .success }   // stop issued (or retried)
        if desired(model) == before {
            if what == "streaming" && target {
                return .fail("Cannot set streaming right now: the stream URL and key are required.")
            }
            return .fail("Cannot set \(what) right now (the control is unavailable in the current state).")
        }
        return .success
    }

    /// Windows `SceneCatalogPolicy.SceneListJson`: the rail only (the reserved
    /// multiview solo scenes are filtered), with Preview and Program ids.
    @MainActor
    private static func sceneListJSON(_ model: AppModel) -> String {
        let rows = model.railScenes.map {
            "{\"id\":\(ControlJSON.string($0.id)),\"name\":\(ControlJSON.string($0.name)),\"layout\":\(ControlJSON.string($0.layout))}"
        }
        return "{\"scenes\":[" + rows.joined(separator: ",") + "],"
            + "\"previewSceneId\":\(ControlJSON.string(model.previewSceneId)),"
            + "\"activeSceneId\":\(ControlJSON.string(model.programSceneId))}"
    }

    /// The ControlState fields the Mac can state truthfully (Windows names).
    /// Fields it cannot observe are omitted rather than invented.
    @MainActor
    static func state(_ model: AppModel) -> SendableDictionary {
        SendableDictionary([
            "recording": model.recordingObservedLive,
            "streaming": model.streamingObservedLive,
            "engineOn": model.captureEnabled,
            "canTake": model.canTake,
            "programBlackout": model.programBlackout,
            "commandStatus": model.takeStatus,
            "activeSceneId": model.programSceneId,
            "previewSceneId": model.previewSceneId,
            "multiviewLayoutMode": model.multiviewLayoutMode,
            // Scene automation is retired for 1.0 (first-release-scene-control.md):
            // the true value is false, not "unknown".
            "automationOn": false,
            "autoTake": false,
        ])
    }
}

/// A bound argument crossing to the main actor (Bool / Int / Double / String / nil).
struct ControlArg: @unchecked Sendable {
    let value: Any?
    init(_ value: Any?) { self.value = value }
    var bool: Bool { value as? Bool ?? false }
    var string: String { value as? String ?? "" }
}

struct SendableDictionary: @unchecked Sendable {
    let dictionary: [String: Any]
    init(_ dictionary: [String: Any]) { self.dictionary = dictionary }
}

extension AppModel {
    /// Windows CanSetRecording/CanSetStreaming as the Mac toggles enforce them:
    /// a core (or the test seam) and not closing outputs for quit.
    var canSetOutputs: Bool { (bridge != nil || quitRequestForTesting != nil) && !quitFinishing }
    /// Observed (not requested) — RecordingCommandPolicy's live set.
    var recordingObservedLive: Bool { recordingStatus == "recording" || recordingStatus == "warning" }
    var streamingObservedLive: Bool { streamStatus == "live" }
}

// ─────────────────────────────────────────────────────────────────────────────
// MARK: - Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

/// Owns the server for the app's lifetime: started from `AppModel.start()`
/// (never by the TESTS/SELFCHECK harness — tests start their own on an
/// ephemeral port), stopped at quit, and pushes /ws state on model changes.
@MainActor
final class ControlApiHost {
    nonisolated static func shouldAutoStart(environment: [String: String]) -> Bool {
        // Snapshot proof runs render the window and exit; they must not open a port.
        !ShellHarness.detect(in: environment) && environment["COREVIDEO_SHELL_SNAPSHOT"] == nil
    }

    /// Coalesces model change notifications for the /ws push. A debounce would be
    /// starved forever by a source that never goes quiet for 100 ms; a latest-value
    /// throttle emits at most every 100 ms and always delivers the last value.
    /// Scheduled on DispatchQueue.main, not RunLoop.main: RunLoop.main only fires in
    /// the .default mode, so pushes froze during menu tracking, live resize and the
    /// .terminateLater quit window. The main queue is serviced in every common mode.
    static func stateChangeStream<P: Publisher>(_ source: P) -> AnyPublisher<Void, Never>
    where P.Failure == Never {
        source.map { _ in () }
            .throttle(for: .milliseconds(100), scheduler: DispatchQueue.main, latest: true)
            .eraseToAnyPublisher()
    }

    private var server: ControlHttpServer?
    private var changes: AnyCancellable?
    private var terminate: NSObjectProtocol?
    private var lastBroadcast = ""

    func start(model: AppModel, environment: [String: String] = ProcessInfo.processInfo.environment) {
        guard server == nil, Self.shouldAutoStart(environment: environment) else { return }
        let options = ControlServerOptions.fromEnvironment(environment)
        let surface = AppModelControlSurface(model: model, box: model.controlSnapshotBox)
        let server = ControlHttpServer(router: ControlRouter(surface: surface), options: options)
        self.server = server
        server.start { [weak model] result in
            Task { @MainActor in
                switch result {
                case .success(let port):
                    ShellLog.write("control: HTTP/WS API listening on http://\(options.host):\(port)/ "
                        + "(GET /manifest, /state, /snapshot, /ws; POST /invoke)"
                        + (options.hasToken ? " — bearer token required" : ""))
                case .failure(let error):
                    ShellLog.write("control: HTTP/WS API failed to start (\(error))")
                    model?.pushWarning("Remote control (Companion) unavailable: \(error)")
                }
            }
        }
        // Windows coalesces StateChanged through a feedback timer; push only real changes.
        changes = Self.stateChangeStream(model.objectWillChange)
            .sink { [weak self, weak model] _ in
                guard let self, let model,
                      let json = ControlJSON.encode(AppModelControlSurface.state(model).dictionary),
                      json != self.lastBroadcast else { return }
                self.lastBroadcast = json
                self.server?.broadcastState(json)
            }
        terminate = NotificationCenter.default.addObserver(
            forName: NSApplication.willTerminateNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.stop() }
        }
    }

    func stop() {
        changes = nil
        server?.stop()
        server = nil
        if let terminate { NotificationCenter.default.removeObserver(terminate) }
        terminate = nil
    }
}
