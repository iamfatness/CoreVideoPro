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

final class SystemCameraExtensionActivator: NSObject, CameraExtensionActivating, OSSystemExtensionRequestDelegate {
    private enum Pending { case activate, deactivate, query }
    private var state: CameraExtensionState = .unknown
    private var callback: ((CameraExtensionState) -> Void)?
    private var pending: Pending = .query
    private var canceledByUs = false
    private let bundlePath: String
    private let hasProfile: Bool

    init(bundlePath: String = Bundle.main.bundlePath,
         hasProfile: Bool = Bundle.main.url(forResource: "embedded", withExtension: "provisionprofile",
                                            subdirectory: nil) != nil
            || FileManager.default.fileExists(atPath: Bundle.main.bundlePath + "/Contents/embedded.provisionprofile")) {
        self.bundlePath = bundlePath
        self.hasProfile = hasProfile
    }

    private func move(to new: CameraExtensionState) {
        ShellLog.write("vcam-ext: \(state) -> \(new)")
        state = new
        callback?(new)
    }

    func query(_ done: @escaping (CameraExtensionState) -> Void) {
        callback = done
        pending = .query
        let req = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: CameraConstants.extensionBundleID, queue: .main)
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
    }

    func activate(_ update: @escaping (CameraExtensionState) -> Void) {
        callback = update
        pending = .activate
        canceledByUs = false
        if let blocked = CameraExtensionPolicy.preflight(bundlePath: bundlePath, hasEmbeddedProfile: hasProfile) {
            move(to: blocked)
            return
        }
        move(to: .activating)
        let req = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: CameraConstants.extensionBundleID, queue: .main)
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
    }

    func deactivate(_ update: @escaping (CameraExtensionState) -> Void) {
        callback = update
        pending = .deactivate
        let req = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: CameraConstants.extensionBundleID, queue: .main)
        req.delegate = self
        OSSystemExtensionManager.shared.submitRequest(req)
    }

    // MARK: OSSystemExtensionRequestDelegate

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        switch CameraExtensionPolicy.replacement(existingVersion: existing.bundleVersion,
                                                 newVersion: ext.bundleVersion) {
        case .replace: return .replace
        case .keep:
            canceledByUs = true
            return .cancel
        }
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        move(to: .needsApproval)
    }

    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch (pending, result) {
        case (.deactivate, _): move(to: .notInstalled)
        case (_, .willCompleteAfterReboot): move(to: .needsReboot)
        default: move(to: .active)
        }
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let ns = error as NSError
        move(to: CameraExtensionPolicy.state(forActivationError: ns.domain, code: ns.code,
                                             canceledByUs: canceledByUs))
    }

    func request(_ request: OSSystemExtensionRequest, foundProperties properties: [OSSystemExtensionProperties]) {
        move(to: CameraExtensionPolicy.state(forProperties: properties.map {
            (enabled: $0.isEnabled, awaitingApproval: $0.isAwaitingUserApproval, uninstalling: $0.isUninstalling)
        }))
    }
}

/// Holder for the temporary Debug-menu spike item (removed in Task 6).
enum CameraExtensionSpike {
    static let activator = SystemCameraExtensionActivator()

    static func run() {
        ShellLog.write("vcam-ext: spike activate requested")
        activator.activate { state in ShellLog.write("vcam-ext: spike state \(state)") }
    }
}
