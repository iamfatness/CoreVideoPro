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
    case unavailable(String), failed(String)
}

enum CameraExtensionReplacement: Equatable { case replace, keep }

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
    func deactivate(_ update: @escaping (CameraExtensionState) -> Void)
}

/// Scripted stand-in: `activate` delivers `script` in order; `deactivate`
/// delivers .notInstalled; `query` delivers the last state delivered.
final class FakeCameraExtensionActivator: CameraExtensionActivating {
    private(set) var calls: [String] = []
    private let script: [CameraExtensionState]
    private var current: CameraExtensionState = .unknown

    init(script: [CameraExtensionState] = []) { self.script = script }

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
    private(set) var isDone = false

    init(kind: Kind) { self.kind = kind }

    /// The delegate's replacement answer; `.keep` marks this request as canceled by us.
    mutating func replacement(existingVersion: String, newVersion: String) -> CameraExtensionReplacement {
        let r = CameraExtensionPolicy.replacement(existingVersion: existingVersion, newVersion: newVersion)
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
    }

    private var inFlight: [ObjectIdentifier: InFlight] = [:]
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
                       callback: @escaping (CameraExtensionState) -> Void) {
        request.delegate = self
        inFlight[ObjectIdentifier(request)] = InFlight(
            reducer: CameraExtensionRequestReducer(kind: kind), callback: callback,
            last: last, request: request)
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
        if let blocked = CameraExtensionPolicy.preflight(bundlePath: bundlePath, hasEmbeddedProfile: hasProfile) {
            ShellLog.write("vcam-ext: \(CameraExtensionState.unknown) -> \(blocked)")
            update(blocked)
            return
        }
        ShellLog.write("vcam-ext: \(CameraExtensionState.unknown) -> \(CameraExtensionState.activating)")
        update(.activating)
        start(OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: CameraConstants.extensionBundleID, queue: .main),
              kind: .activate, last: .activating, callback: update)
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
        let r = entry.reducer.replacement(existingVersion: existing.bundleVersion, newVersion: ext.bundleVersion)
        inFlight[key] = entry
        return r == .replace ? .replace : .cancel
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        deliver(.needsUserApproval, for: request)
    }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        deliver(.finished(rebootRequired: result == .willCompleteAfterReboot), for: request)
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let ns = error as NSError
        deliver(.failed(domain: ns.domain, code: ns.code), for: request)
    }

    func request(_ request: OSSystemExtensionRequest, foundProperties properties: [OSSystemExtensionProperties]) {
        deliver(.foundProperties(properties.map {
            (enabled: $0.isEnabled, awaitingApproval: $0.isAwaitingUserApproval, uninstalling: $0.isUninstalling)
        }), for: request)
    }
}
