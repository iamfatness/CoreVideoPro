// CameraExtensionActivation — shell side of installing the CoreVideo Pro camera
// system extension (roadmap M9.2). The decisions (preflight, error mapping,
// replacement, settings deep link) are pure and live in CameraExtensionPolicy so
// the harness can pin them; the OSSystemExtensionRequest plumbing sits behind
// CameraExtensionActivating so everything CI runs goes through a fake (CI has no
// window server and cannot activate system extensions).

import CameraExtensionKit
import Foundation
import SystemExtensions

enum CameraExtensionState: Equatable {
    case unknown, notInstalled, activating, needsApproval, active, needsReboot, notInApplications
    /// A different extension version is installed and enabled, and replacing it
    /// now would happen while the camera is in use (an app streaming from it, or
    /// our own output on). Nothing was submitted; the old copy keeps working.
    case updateReady
    case unavailable(String), failed(String)
}

enum CameraExtensionReplacement: Equatable { case replace, keep }

/// One OSSystemExtensionProperties entry, reduced to what the policy reads.
/// `version` is the entry's bundleVersion, i.e. CameraConstants.extensionVersion
/// of the build that installed it (an install from before 2026-11-15 carries
/// that app's build number instead).
struct CameraExtensionInstall: Equatable {
    var version: String
    var enabled = false
    var awaitingApproval = false
    var uninstalling = false
}

/// What an opted-in launch does after asking macOS what is installed.
enum CameraExtensionLaunchAction: Equatable {
    /// Our exact version is installed and enabled: submit NOTHING.
    case alreadyActive
    /// Submit an activation (first install, approval still pending, or no answer).
    case activate
    /// Another version is installed and enabled: activating REPLACES it, which
    /// must wait until the camera is not in use (CameraExtensionPolicy.inUse).
    case update
    /// Our version is installed but macOS has not enabled it, an older copy
    /// is still uninstalling and no device exists: a swap is staged for a
    /// restart. Submit NOTHING (another request would stack a second copy on
    /// it) and say "Restart your Mac".
    case needsReboot
}

/// What a new process can learn about OUR camera device from the CoreMediaIO HAL.
struct CameraDeviceStatus: Equatable {
    /// The device is in the HAL device list (the installed extension is running
    /// and published it).
    var registered = false
    /// kCMIODevicePropertyDeviceIsRunningSomewhere: some process has a stream
    /// of the device running (Zoom, Photo Booth, our own core feeding the sink).
    var runningSomewhere = false
    /// The extension version in the device's model UID; nil for a copy from
    /// before 3001 (or an unreadable model). Registration counts only when
    /// it is the bundled version.
    var version: String?
    /// Why the device could not be read (the probe did not run or did not
    /// answer); nil when it was read. A failed read is `unreadable`: never
    /// "registered", and "running" so nothing is replaced on a guess.
    var probeFailure: String?

    func isOurs(_ bundledVersion: String) -> Bool { registered && version == bundledVersion }

    static func unreadable(_ why: String) -> CameraDeviceStatus {
        CameraDeviceStatus(registered: false, runningSomewhere: true, version: nil, probeFailure: why)
    }
}

/// The device probe behind a protocol: CI has no camera device to look at.
protocol CameraDeviceProbing: AnyObject {
    func read() -> CameraDeviceStatus
}

/// `status` is everything except our own core: registration and OTHER apps'
/// streams. Our core's sink stream is modelled the way the HAL shows it:
/// running from the first `sync-virtual-camera {on:true}` the model sends
/// (feed every captured core command to `observe`) until `releaseAfterReads`
/// reads after an `on:false`, because CMIODeviceStopStream lands a moment
/// after the command. A fake that reads "not running" while our output is on
/// tests a state the real HAL never produces.
final class FakeCameraDeviceProbe: CameraDeviceProbing {
    var status: CameraDeviceStatus
    var releaseAfterReads = 2
    private(set) var reads = 0
    private(set) var ownStreaming = false
    private var readsSinceOff: Int?

    init(_ status: CameraDeviceStatus = CameraDeviceStatus()) { self.status = status }

    func read() -> CameraDeviceStatus {
        reads += 1
        if let n = readsSinceOff {
            if n >= releaseAfterReads { ownStreaming = false; readsSinceOff = nil } else { readsSinceOff = n + 1 }
        }
        var s = status
        s.runningSomewhere = s.runningSomewhere || ownStreaming
        return s
    }

    /// Our core started (on:true) or was told to stop (on:false) its sink stream.
    func wire(on: Bool) {
        if on { ownStreaming = true; readsSinceOff = nil } else if ownStreaming, readsSinceOff == nil { readsSinceOff = 0 }
    }

    /// Feed a captured core command body (`coreCommandCaptureForTesting`).
    func observe(_ body: JSONObject) {
        for c in (body["commands"] as? [JSONObject]) ?? [] where c["type"] as? String == "sync-virtual-camera" {
            wire(on: c["on"] as? Bool ?? true)
        }
    }
}

/// What a NEWBORN process sees of our camera device.
///
/// The shell must never judge the device from its own CoreMediaIO view. A
/// process learns the camera-extension providers that exist at its FIRST
/// CoreMediaIO touch and is not told about one that registers later (measured
/// on macOS 26.3, 2026-11-16: the shell reported "no device 10 s after
/// activation" and then "Restart your Mac" for a camera Photo Booth, Zoom and
/// every other new process could see). A first install and every extension
/// update register a provider after the shell started, so the in-process view
/// is wrong in exactly the cases this check exists for.
///
/// So each read runs the core executable in its probe mode
/// (`corevideo-native --vcam-device-probe`, about 20 ms), which prints one line
/// of JSON and exits. The same binary is the sink helper, so the shell and the
/// producer look at the Mac the same way.
enum CameraDeviceProbeReport {
    static let argument = "--vcam-device-probe"

    /// nil when the output is not the probe's JSON line.
    static func parse(_ output: String) -> CameraDeviceStatus? {
        guard let line = output.split(whereSeparator: \.isNewline).last(where: { $0.hasPrefix("{") }),
              let object = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? JSONObject,
              let registered = object["registered"] as? Bool else { return nil }
        guard registered else { return CameraDeviceStatus() }
        // An unreadable "running" flag counts as running: claiming "free"
        // wrongly replaces an extension under a live stream.
        let running = object["runningSomewhere"] as? Bool ?? true
        let model = object["modelUid"] as? String ?? ""
        return CameraDeviceStatus(registered: true, runningSomewhere: running,
                                  version: CameraConstants.extensionVersion(fromDeviceModel: model))
    }
}

final class FreshProcessCameraDeviceProbe: CameraDeviceProbing {
    private let executable: () -> String?
    private let arguments: [String]
    private let timeout: TimeInterval
    private var lastFailure: String?

    /// The core the shell launches (or would launch). The test harness sets
    /// this to "no core": a test must never run whatever core binary happens
    /// to sit in a developer's build folder.
    static var defaultExecutable: () -> String? = { ShellPaths.resolve().corePath }

    init(executable: @escaping () -> String? = { FreshProcessCameraDeviceProbe.defaultExecutable() },
         arguments: [String] = [CameraDeviceProbeReport.argument], timeout: TimeInterval = 2) {
        self.executable = executable
        self.arguments = arguments
        self.timeout = timeout
    }

    func read() -> CameraDeviceStatus {
        let result: Result<String, ProbeError>
        if let path = executable() {
            result = Self.run(path, arguments, timeout: timeout)
        } else {
            result = .failure(ProbeError("no core executable to ask"))
        }
        switch result {
        case .success(let output):
            if let status = CameraDeviceProbeReport.parse(output) {
                lastFailure = nil
                return status
            }
            return failed("the probe's answer was not understood")
        case .failure(let error):
            return failed(error.message)
        }
    }

    /// Logged when it changes, not on every poll.
    private func failed(_ why: String) -> CameraDeviceStatus {
        if lastFailure != why {
            lastFailure = why
            ShellLog.write("vcam-ext: could not read the camera device from a new process: \(why)")
        }
        return .unreadable(why)
    }

    struct ProbeError: Error {
        let message: String
        init(_ message: String) { self.message = message }
    }

    /// Runs `path` and returns its stdout. posix_spawn and waitpid, never
    /// Process.waitUntilExit: that spins the caller's run loop, and callers
    /// are on the main actor in the middle of a decision.
    static func run(_ path: String, _ arguments: [String], timeout: TimeInterval) -> Result<String, ProbeError> {
        var fds: [Int32] = [0, 0]
        guard pipe(&fds) == 0 else { return .failure(ProbeError("pipe failed (errno \(errno))")) }
        var actions: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&actions)
        defer { posix_spawn_file_actions_destroy(&actions) }
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0)
        posix_spawn_file_actions_adddup2(&actions, fds[1], 1)
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0)
        var attributes: posix_spawnattr_t?
        posix_spawnattr_init(&attributes)
        defer { posix_spawnattr_destroy(&attributes) }
        // Only stdin/stdout/stderr reach the probe.
        posix_spawnattr_setflags(&attributes, Int16(POSIX_SPAWN_CLOEXEC_DEFAULT))
        var argv: [UnsafeMutablePointer<CChar>?] = ([path] + arguments).map { strdup($0) }
        argv.append(nil)
        defer { for pointer in argv { free(pointer) } }
        var pid: pid_t = 0
        let spawned = posix_spawn(&pid, path, &actions, &attributes, argv, environ)
        close(fds[1])
        guard spawned == 0 else {
            close(fds[0])
            return .failure(ProbeError("could not start \((path as NSString).lastPathComponent) (errno \(spawned))"))
        }
        defer { close(fds[0]) }
        var output = Data()
        let deadline = Date().addingTimeInterval(timeout)
        var timedOut = false
        while true {
            let remaining = deadline.timeIntervalSinceNow
            if remaining <= 0 { timedOut = true; break }
            var poller = pollfd(fd: fds[0], events: Int16(POLLIN), revents: 0)
            let ready = poll(&poller, 1, Int32(remaining * 1000) + 1)
            if ready < 0 && errno == EINTR { continue }
            if ready <= 0 { timedOut = ready == 0; break }
            var buffer = [UInt8](repeating: 0, count: 4096)
            let got = Darwin.read(fds[0], &buffer, buffer.count)
            if got < 0 && errno == EINTR { continue }
            if got <= 0 { break }  // end of file: the probe is done
            output.append(contentsOf: buffer[0..<got])
            if output.count > 65536 { break }
        }
        if timedOut { kill(pid, SIGKILL) }
        var status: Int32 = 0
        while waitpid(pid, &status, 0) < 0 && errno == EINTR {}
        if timedOut { return .failure(ProbeError("no answer in \(timeout) s")) }
        return .success(String(decoding: output, as: UTF8.self))
    }
}

enum CameraExtensionPolicy {
    /// States that must stop an activation BEFORE a request is submitted.
    static func preflight(bundlePath: String, hasEmbeddedProfile: Bool) -> CameraExtensionState? {
        guard bundlePath.hasPrefix("/Applications/") else { return .notInApplications }
        guard hasEmbeddedProfile else {
            return .unavailable("This build can't install the virtual camera (development build without a provisioning profile).")
        }
        return nil
    }

    /// Codes are OSSystemExtensionErrorCode (verified against the SDK header).
    static func state(forActivationError domain: String, code: Int, canceledByUs: Bool) -> CameraExtensionState {
        let generic = CameraExtensionState.failed("Couldn't install the virtual camera (code \(code)).")
        guard domain == "OSSystemExtensionErrorDomain" else { return generic }
        switch code {
        case 3: return .notInApplications
        case 2, 8: return .unavailable("This copy of CoreVideo Pro isn't signed to install the camera.")
        case 10: return .unavailable("macOS policy blocked the camera extension.")
        case 11 where canceledByUs: return .active
        case 13: return .needsApproval
        default: return generic
        }
    }

    /// Equal versions keep the installed copy (the delegate answers `.cancel` and
    /// the following requestCanceled maps to .active); anything else replaces,
    /// downgrades included (an older install must be able to take over).
    static func replacement(existingVersion: String, newVersion: String) -> CameraExtensionReplacement {
        existingVersion == newVersion ? .keep : .replace
    }

    /// The bundled extension's version as macOS reports it (bundleVersion).
    static let bundledVersion = String(CameraConstants.extensionVersion)

    /// Opted-in launch. Equal versions never replace, and an equal version that
    /// is already enabled is not even re-submitted: an activation request for an
    /// installed extension is exactly the step that can disturb a running one.
    /// nil (macOS never answered) falls back to activating, the pre-2026-11-15
    /// behaviour; the request's own replacement answer still keeps equal versions.
    ///
    /// `.needsReboot` is claimed only when macOS itself reports OUR version
    /// installed but not enabled while an older copy is uninstalling and no
    /// device exists (a staged swap). An older copy "waiting to uninstall on
    /// reboot" proves nothing by itself: on the owner's Mac (2026-11-16) 3002
    /// stayed listed that way AFTER the restart while 3003 ran and served
    /// Photo Booth, and the "no device" half of the old rule was read from the
    /// shell's own stale CoreMediaIO view (see FreshProcessCameraDeviceProbe).
    static func launchAction(installs: [CameraExtensionInstall]?, bundledVersion: String,
                             deviceRegistered: Bool) -> CameraExtensionLaunchAction {
        guard let installs else { return .activate }
        if installs.contains(where: { $0.version == bundledVersion && $0.enabled && !$0.uninstalling }) {
            return .alreadyActive
        }
        let ours = installs.filter { $0.version == bundledVersion }
        if !deviceRegistered, !ours.isEmpty, !ours.contains(where: { $0.awaitingApproval }),
           installs.contains(where: { $0.uninstalling && $0.version != bundledVersion }) {
            return .needsReboot
        }
        if installs.contains(where: { $0.version != bundledVersion && $0.enabled && !$0.uninstalling }) {
            return .update
        }
        return .activate
    }

    /// A completed activation (or an already-active launch) whose device never
    /// appeared, even to a new process: is a restart what finishes it? Only
    /// when macOS reports OUR version itself as not enabled (and not merely
    /// awaiting approval). An older copy still uninstalling is not evidence
    /// (see launchAction). nil (no answer) never claims a restart.
    static func needsReboot(unregisteredWith installs: [CameraExtensionInstall]?, bundledVersion: String) -> Bool {
        guard let installs else { return false }
        let ours = installs.filter { $0.version == bundledVersion }
        if ours.contains(where: { $0.awaitingApproval }) { return false }
        return !ours.contains(where: { $0.enabled && !$0.uninstalling })
    }

    /// "In use" for a replacement: any process streaming our device, or our
    /// own output switched on (the core feeds the sink while the intent is on;
    /// it is paused only for an operator's explicit Update camera).
    static func inUse(device: CameraDeviceStatus, intentOn: Bool, pausedForUpdate: Bool) -> Bool {
        device.runningSomewhere || (intentOn && !pausedForUpdate)
    }

    static func state(forProperties props: [(enabled: Bool, awaitingApproval: Bool, uninstalling: Bool)]) -> CameraExtensionState {
        if props.contains(where: { $0.enabled && !$0.uninstalling }) { return .active }
        if props.contains(where: { $0.awaitingApproval }) { return .needsApproval }
        return .notInstalled
    }

    static func settingsURL(osMajor: Int) -> URL {
        osMajor >= 15
            ? URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension")!
            : URL(string: "x-apple.systempreferences:com.apple.preference.security?Security")!
    }

    static func settingsInstruction(osMajor: Int) -> String {
        osMajor >= 15
            ? "System Settings \u{203A} General \u{203A} Login Items & Extensions \u{203A} Camera Extensions"
            : "System Settings \u{203A} Privacy & Security, then click Allow next to the CoreVideo Pro message"
    }
}

protocol CameraExtensionActivating: AnyObject {
    func query(_ done: @escaping (CameraExtensionState) -> Void)
    func activate(_ update: @escaping (CameraExtensionState) -> Void)
    /// Activation whose replacement answer re-checks `inUse` at the moment
    /// macOS asks: a different version is replaced only while it is false;
    /// otherwise the request is cancelled and ends in `.updateReady`.
    func activate(deferringReplacementWhile inUse: @escaping () -> Bool,
                  _ update: @escaping (CameraExtensionState) -> Void)
    func deactivate(_ update: @escaping (CameraExtensionState) -> Void)
    /// Every installed copy of our extension (any version, any state); nil when
    /// macOS refused the properties request.
    func installs(_ done: @escaping ([CameraExtensionInstall]?) -> Void)
    /// The state that would stop an activation before anything is submitted
    /// (not in /Applications, no provisioning profile), or nil.
    func preflightState() -> CameraExtensionState?
}

extension CameraExtensionActivating {
    func installs(_ done: @escaping ([CameraExtensionInstall]?) -> Void) { done(nil) }
    func preflightState() -> CameraExtensionState? { nil }
    func activate(deferringReplacementWhile inUse: @escaping () -> Bool,
                  _ update: @escaping (CameraExtensionState) -> Void) { activate(update) }
}

/// Scripted stand-in: `activate` delivers `script` in order; `deactivate`
/// delivers .notInstalled; `query` delivers the last state delivered;
/// `installs` delivers `installed`.
final class FakeCameraExtensionActivator: CameraExtensionActivating {
    private(set) var calls: [String] = []
    private let script: [CameraExtensionState]
    private var current: CameraExtensionState = .unknown
    var installed: [CameraExtensionInstall]?
    var preflight: CameraExtensionState?
    func preflightState() -> CameraExtensionState? { preflight }

    init(script: [CameraExtensionState] = [], installed: [CameraExtensionInstall]? = []) {
        self.script = script
        self.installed = installed
    }

    func installs(_ done: @escaping ([CameraExtensionInstall]?) -> Void) {
        calls.append("installs")
        done(installed)
    }

    func query(_ done: @escaping (CameraExtensionState) -> Void) {
        calls.append("query")
        done(current == .unknown ? .notInstalled : current)
    }

    /// > 0: the first scripted state is delivered at once, the rest after this
    /// delay (a request that stays `.activating` while sysextd works).
    var completionDelay: TimeInterval = 0

    func activate(_ update: @escaping (CameraExtensionState) -> Void) {
        calls.append("activate")
        guard completionDelay > 0, let first = script.first else {
            for s in script { current = s; update(s) }
            return
        }
        current = first; update(first)
        let rest = script.dropFirst()
        DispatchQueue.main.asyncAfter(deadline: .now() + completionDelay) { [weak self] in
            for s in rest { self?.current = s; update(s) }
        }
    }

    func deactivate(_ update: @escaping (CameraExtensionState) -> Void) {
        calls.append("deactivate")
        current = .notInstalled
        update(.notInstalled)
    }
}

/// Pure per-request state machine: one instance per submitted request. It owns
/// the request's kind, its own canceledByUs flag and whether it has finished, so
/// delegate events for one request can never leak into another.
struct CameraExtensionRequestReducer {
    enum Kind { case activate, deactivate, query }
    enum Event {
        case needsUserApproval
        case finished(rebootRequired: Bool)
        case failed(domain: String, code: Int)
        case foundProperties([(enabled: Bool, awaitingApproval: Bool, uninstalling: Bool)])
    }

    let kind: Kind
    private(set) var canceledByUs = false
    /// We cancelled a REPLACEMENT because the camera was in use.
    private(set) var deferredInUse = false
    private(set) var isDone = false

    init(kind: Kind) { self.kind = kind }

    /// The delegate's replacement answer; `.keep` marks this request as canceled
    /// by us. A different version found while `inUse` is also kept (never
    /// replace under a running stream) and the request then ends `.updateReady`.
    mutating func replacement(existingVersion: String, newVersion: String,
                              inUse: Bool = false) -> CameraExtensionReplacement {
        var r = CameraExtensionPolicy.replacement(existingVersion: existingVersion, newVersion: newVersion)
        if r == .replace && inUse {
            r = .keep
            deferredInUse = true
        }
        if r == .keep { canceledByUs = true }
        return r
    }

    /// The state this event produces, or nil when nothing should be emitted.
    /// A finished request ignores every later event.
    mutating func reduce(_ event: Event) -> CameraExtensionState? {
        guard !isDone else { return nil }
        switch event {
        case .needsUserApproval:
            return kind == .activate ? .needsApproval : nil
        case .finished(let reboot):
            isDone = true
            switch kind {
            case .query: return nil  // never overwrites what foundProperties reported
            case .deactivate: return .notInstalled
            case .activate: return reboot ? .needsReboot : .active
            }
        case .failed(let domain, let code):
            isDone = true
            if deferredInUse && domain == "OSSystemExtensionErrorDomain" && code == 11 { return .updateReady }
            return CameraExtensionPolicy.state(forActivationError: domain, code: code,
                                               canceledByUs: canceledByUs)
        case .foundProperties(let props):
            guard kind == .query else { return nil }
            isDone = true
            return CameraExtensionPolicy.state(forProperties: props)
        }
    }
}

final class SystemCameraExtensionActivator: NSObject, CameraExtensionActivating, OSSystemExtensionRequestDelegate {
    /// Everything one in-flight request needs. Holding the request here keeps it
    /// alive (OSSystemExtensionRequest.delegate is weak); whoever owns the
    /// activator must hold IT strongly (AppModel.cameraExtension does).
    private struct InFlight {
        var reducer: CameraExtensionRequestReducer
        var callback: (CameraExtensionState) -> Void
        var last: CameraExtensionState = .unknown
        let request: OSSystemExtensionRequest
        var inUse: () -> Bool = { false }
    }

    private var inFlight: [ObjectIdentifier: InFlight] = [:]
    /// Properties requests asked for the raw install list (`installs`), kept
    /// apart from the state-reducing requests above.
    private var installRequests: [ObjectIdentifier: (request: OSSystemExtensionRequest,
                                                      done: ([CameraExtensionInstall]?) -> Void)] = [:]
    private let bundlePath: String
    private let hasProfile: Bool
    private let submit: (OSSystemExtensionRequest) -> Void

    init(bundlePath: String = Bundle.main.bundlePath,
         hasProfile: Bool = FileManager.default.fileExists(
            atPath: Bundle.main.bundlePath + "/Contents/embedded.provisionprofile"),
         submit: @escaping (OSSystemExtensionRequest) -> Void = {
             OSSystemExtensionManager.shared.submitRequest($0)
         }) {
        self.bundlePath = bundlePath
        self.hasProfile = hasProfile
        self.submit = submit
    }

    private func start(_ request: OSSystemExtensionRequest, kind: CameraExtensionRequestReducer.Kind,
                       last: CameraExtensionState = .unknown,
                       inUse: @escaping () -> Bool = { false },
                       callback: @escaping (CameraExtensionState) -> Void) {
        request.delegate = self
        inFlight[ObjectIdentifier(request)] = InFlight(
            reducer: CameraExtensionRequestReducer(kind: kind), callback: callback,
            last: last, request: request, inUse: inUse)
        submit(request)
    }

    private func deliver(_ event: CameraExtensionRequestReducer.Event, for request: OSSystemExtensionRequest) {
        let key = ObjectIdentifier(request)
        guard var entry = inFlight[key] else { return }  // unknown or finished request
        let new = entry.reducer.reduce(event)
        if let new {
            ShellLog.write("vcam-ext: \(entry.last) -> \(new)")
            entry.last = new
        }
        if entry.reducer.isDone { inFlight[key] = nil } else { inFlight[key] = entry }
        if let new { entry.callback(new) }
    }

    func query(_ done: @escaping (CameraExtensionState) -> Void) {
        start(OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: CameraConstants.extensionBundleID, queue: .main),
              kind: .query, callback: done)
    }

    func activate(_ update: @escaping (CameraExtensionState) -> Void) {
        activate(deferringReplacementWhile: { false }, update)
    }

    func preflightState() -> CameraExtensionState? {
        CameraExtensionPolicy.preflight(bundlePath: bundlePath, hasEmbeddedProfile: hasProfile)
    }

    func activate(deferringReplacementWhile inUse: @escaping () -> Bool,
                  _ update: @escaping (CameraExtensionState) -> Void) {
        if let blocked = CameraExtensionPolicy.preflight(bundlePath: bundlePath, hasEmbeddedProfile: hasProfile) {
            ShellLog.write("vcam-ext: \(CameraExtensionState.unknown) -> \(blocked)")
            update(blocked)
            return
        }
        ShellLog.write("vcam-ext: \(CameraExtensionState.unknown) -> \(CameraExtensionState.activating)")
        update(.activating)
        start(OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: CameraConstants.extensionBundleID, queue: .main),
              kind: .activate, last: .activating, inUse: inUse, callback: update)
    }

    func installs(_ done: @escaping ([CameraExtensionInstall]?) -> Void) {
        let request = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: CameraConstants.extensionBundleID, queue: .main)
        request.delegate = self
        installRequests[ObjectIdentifier(request)] = (request, done)
        submit(request)
    }

    static func install(_ p: OSSystemExtensionProperties) -> CameraExtensionInstall {
        CameraExtensionInstall(version: p.bundleVersion, enabled: p.isEnabled,
                               awaitingApproval: p.isAwaitingUserApproval, uninstalling: p.isUninstalling)
    }

    func deactivate(_ update: @escaping (CameraExtensionState) -> Void) {
        start(OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: CameraConstants.extensionBundleID, queue: .main),
              kind: .deactivate, callback: update)
    }

    // MARK: OSSystemExtensionRequestDelegate

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        let key = ObjectIdentifier(request)
        guard var entry = inFlight[key] else { return .cancel }
        let differs = existing.bundleVersion != ext.bundleVersion
        let busy = differs && entry.inUse()
        let r = entry.reducer.replacement(existingVersion: existing.bundleVersion, newVersion: ext.bundleVersion,
                                          inUse: busy)
        ShellLog.write("vcam-ext: replace \(existing.bundleVersion) with \(ext.bundleVersion)? "
                       + (r == .replace ? "yes" : (busy ? "no, the camera is in use" : "no, same version")))
        inFlight[key] = entry
        return r == .replace ? .replace : .cancel
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        deliver(.needsUserApproval, for: request)
    }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        // A properties request that finishes without foundProperties never
        // answers; the caller's timeout covers it (same as `query`).
        if installRequests[ObjectIdentifier(request)] != nil { return }
        deliver(.finished(rebootRequired: result == .willCompleteAfterReboot), for: request)
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let ns = error as NSError
        if let pending = installRequests.removeValue(forKey: ObjectIdentifier(request)) {
            ShellLog.write("vcam-ext: installed-copies query failed (\(ns.domain) \(ns.code))")
            pending.done(nil)
            return
        }
        deliver(.failed(domain: ns.domain, code: ns.code), for: request)
    }

    func request(_ request: OSSystemExtensionRequest, foundProperties properties: [OSSystemExtensionProperties]) {
        if let pending = installRequests.removeValue(forKey: ObjectIdentifier(request)) {
            let list = properties.map(Self.install)
            ShellLog.write("vcam-ext: installed copies \(list.map { "\($0.version)[\($0.enabled ? "enabled" : "disabled")\($0.uninstalling ? ",uninstalling" : "")\($0.awaitingApproval ? ",awaiting" : "")]" })")
            pending.done(list)
            return
        }
        deliver(.foundProperties(properties.map {
            (enabled: $0.isEnabled, awaitingApproval: $0.isAwaitingUserApproval, uninstalling: $0.isUninstalling)
        }), for: request)
    }
}
