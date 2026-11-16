// CameraExtensionActivation — shell side of installing the CoreVideo Pro camera
// system extension (roadmap M9.2). The decisions (preflight, error mapping,
// replacement, settings deep link) are pure and live in CameraExtensionPolicy so
// the harness can pin them; the OSSystemExtensionRequest plumbing sits behind
// CameraExtensionActivating so everything CI runs goes through a fake (CI has no
// window server and cannot activate system extensions).

import CameraExtensionKit
import CoreMediaIO
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
}

/// What the shell can learn about OUR camera device from the CoreMediaIO HAL.
struct CameraDeviceStatus: Equatable {
    /// The device is in the HAL device list (the installed extension is running
    /// and published it).
    var registered = false
    /// kCMIODevicePropertyDeviceIsRunningSomewhere: some process has a stream
    /// of the device running (Zoom, Photo Booth, our own core feeding the sink).
    var runningSomewhere = false
}

/// The HAL probe behind a protocol: CI has no camera device to look at.
protocol CameraDeviceProbing: AnyObject {
    func read() -> CameraDeviceStatus
}

final class FakeCameraDeviceProbe: CameraDeviceProbing {
    var status: CameraDeviceStatus
    private(set) var reads = 0
    init(_ status: CameraDeviceStatus = CameraDeviceStatus()) { self.status = status }
    func read() -> CameraDeviceStatus { reads += 1; return status }
}

/// Reads the CoreMediaIO HAL directly (the same calls the core's
/// CmioVirtualCameraSink makes). The HAL device list only updates in a process
/// whose main run loop is serviced; the shell's main thread runs one.
final class SystemCameraDeviceProbe: CameraDeviceProbing {
    static func isOurDeviceUID(_ uid: String) -> Bool {
        uid == CameraConstants.legacyDeviceID
            || uid.caseInsensitiveCompare(CameraConstants.deviceUUID.uuidString) == .orderedSame
    }

    func read() -> CameraDeviceStatus {
        guard let device = Self.ourDevice() else { return CameraDeviceStatus() }
        var address = Self.global(CMIOObjectPropertySelector(kCMIODevicePropertyDeviceIsRunningSomewhere))
        var running: UInt32 = 0
        var used: UInt32 = 0
        let err = CMIOObjectGetPropertyData(device, &address, 0, nil,
                                            UInt32(MemoryLayout<UInt32>.size), &used, &running)
        // An unreadable flag counts as running: claiming "free" wrongly is the
        // failure that strands an update, claiming "busy" only waits.
        return CameraDeviceStatus(registered: true, runningSomewhere: err != noErr || running != 0)
    }

    private static func global(_ selector: CMIOObjectPropertySelector) -> CMIOObjectPropertyAddress {
        CMIOObjectPropertyAddress(mSelector: selector,
                                  mScope: CMIOObjectPropertyScope(kCMIOObjectPropertyScopeGlobal),
                                  mElement: CMIOObjectPropertyElement(kCMIOObjectPropertyElementMain))
    }

    private static func ourDevice() -> CMIOObjectID? {
        var address = global(CMIOObjectPropertySelector(kCMIOHardwarePropertyDevices))
        let system = CMIOObjectID(kCMIOObjectSystemObject)
        var size: UInt32 = 0
        guard CMIOObjectGetPropertyDataSize(system, &address, 0, nil, &size) == noErr, size > 0 else { return nil }
        var ids = [CMIOObjectID](repeating: 0, count: Int(size) / MemoryLayout<CMIOObjectID>.size)
        var used: UInt32 = 0
        guard CMIOObjectGetPropertyData(system, &address, 0, nil, size, &used, &ids) == noErr else { return nil }
        ids = Array(ids.prefix(Int(used) / MemoryLayout<CMIOObjectID>.size))
        for id in ids {
            var uidAddress = global(CMIOObjectPropertySelector(kCMIODevicePropertyDeviceUID))
            var uid: Unmanaged<CFString>?
            var uidUsed: UInt32 = 0
            let err = withUnsafeMutablePointer(to: &uid) { ptr in
                CMIOObjectGetPropertyData(id, &uidAddress, 0, nil,
                                          UInt32(MemoryLayout<Unmanaged<CFString>?>.size), &uidUsed, ptr)
            }
            guard err == noErr, let value = uid?.takeRetainedValue() else { continue }
            if isOurDeviceUID(value as String) { return id }
        }
        return nil
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
    static func launchAction(installs: [CameraExtensionInstall]?, bundledVersion: String) -> CameraExtensionLaunchAction {
        guard let installs else { return .activate }
        if installs.contains(where: { $0.version == bundledVersion && $0.enabled && !$0.uninstalling }) {
            return .alreadyActive
        }
        if installs.contains(where: { $0.version != bundledVersion && $0.enabled && !$0.uninstalling }) {
            return .update
        }
        return .activate
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
}

extension CameraExtensionActivating {
    func installs(_ done: @escaping ([CameraExtensionInstall]?) -> Void) { done(nil) }
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

    func activate(_ update: @escaping (CameraExtensionState) -> Void) {
        calls.append("activate")
        for s in script { current = s; update(s) }
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
