// Virtual camera updates without support tickets (owner requirement
// 2026-11-15). Kept in its own file, like ShellTestsTier0.swift, so parallel
// branches extending ShellTests.swift do not collide line-for-line.
//
// CI cannot activate a system extension or see a CoreMediaIO device, so every
// case drives FakeCameraExtensionActivator / FakeCameraDeviceProbe, or the real
// SystemCameraExtensionActivator with a capturing `submit` and hand-built
// OSSystemExtensionProperties.

import CameraExtensionKit
import Foundation
import SystemExtensions

extension ShellTests {

    /// Appended to the main case list by runAndExit().
    @MainActor
    static var vcamUpdateCases: [(String, () -> Void)] {
        [
            ("vcam-update/equal-versions-never-replace", testEqualExtensionVersionsNeverReplace),
            ("vcam-update/launch-with-our-version-active-submits-nothing",
             testLaunchWithOurVersionActiveSubmitsNothing),
            ("vcam-update/in-use-defers-to-update-ready", testInUseUpdateIsDeferredAndNothingSubmitted),
            ("vcam-update/free-camera-replaces", testFreeCameraUpdateReplaces),
            ("vcam-update/update-ready-retries-when-free", testUpdateReadyRetriesWhenTheCameraFrees),
            ("vcam-update/update-button-pauses-our-output", testUpdateCameraButtonPausesOurOutputThenReplaces),
            ("vcam-update/replacement-offered-in-use-is-cancelled",
             testReplacementOfferedWhileInUseEndsUpdateReady),
        ]
    }

    /// OSSystemExtensionProperties has no public initializer; a subclass with
    /// overridden getters stands in for what sysextd hands the delegate.
    final class StubExtensionProperties: OSSystemExtensionProperties {
        let version: String
        let stubEnabled: Bool
        let stubUninstalling: Bool
        init(_ version: String, enabled: Bool = true, uninstalling: Bool = false) {
            self.version = version
            stubEnabled = enabled
            stubUninstalling = uninstalling
            super.init()
        }
        override var bundleVersion: String { version }
        override var bundleShortVersion: String { version }
        override var isEnabled: Bool { stubEnabled }
        override var isUninstalling: Bool { stubUninstalling }
        override var isAwaitingUserApproval: Bool { false }
    }

    static func ours(enabled: Bool = true, uninstalling: Bool = false) -> CameraExtensionInstall {
        CameraExtensionInstall(version: CameraExtensionPolicy.bundledVersion, enabled: enabled,
                               uninstalling: uninstalling)
    }

    /// The delegate's replacement answer for a real request: equal versions
    /// cancel (keep the running copy), a different version replaces.
    static func testEqualExtensionVersionsNeverReplace() {
        var submitted: [OSSystemExtensionRequest] = []
        let a = SystemCameraExtensionActivator(bundlePath: "/Applications/CoreVideo Pro.app",
                                               hasProfile: true, submit: { submitted.append($0) })
        a.activate { _ in }
        guard let r = submitted.first else { expect(false, "activation submitted"); return }
        let v = CameraExtensionPolicy.bundledVersion
        expectEqual(a.request(r, actionForReplacingExtension: StubExtensionProperties(v),
                              withExtension: StubExtensionProperties(v)), .cancel,
                    "the same extensionVersion is never replaced")
        a.activate { _ in }
        guard submitted.count == 2 else { expect(false, "second activation submitted"); return }
        expectEqual(a.request(submitted[1], actionForReplacingExtension: StubExtensionProperties("2883"),
                              withExtension: StubExtensionProperties(v)), .replace,
                    "a different version (here a pre-2026-11-15 build-number install) is replaced")
    }

    /// Opted-in launch: our version already enabled means NO activation request
    /// at all; an older install still activates.
    @MainActor
    static func testLaunchWithOurVersionActiveSubmitsNothing() {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(script: [.active], installed: [ours()])
        model.cameraExtension = fake
        model.cameraExtensionOptedIn = true
        model.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(fake.calls, ["installs"], "our version enabled: nothing is submitted")
        expectEqual(model.cameraExtensionState, .active, "and the state is active")
        model.stopTimersForTesting()

        let uninstalling = AppModel()
        let fake2 = FakeCameraExtensionActivator(script: [.active], installed: [ours(uninstalling: true)])
        uninstalling.cameraExtension = fake2
        uninstalling.cameraExtensionOptedIn = true
        uninstalling.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(fake2.calls, ["installs", "activate"], "our version on its way out does not count")
        uninstalling.stopTimersForTesting()
    }

    // ── item 2: never replace while in use ──────────────────────────────────

    static let olderInstall = CameraExtensionInstall(version: "2883", enabled: true)

    @MainActor
    static func launchedModel(installed: [CameraExtensionInstall], device: CameraDeviceStatus,
                              intentOn: Bool = false, script: [CameraExtensionState] = [.activating, .active])
        -> (AppModel, FakeCameraExtensionActivator, FakeCameraDeviceProbe) {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(script: script, installed: installed)
        let probe = FakeCameraDeviceProbe(device)
        model.cameraExtension = fake
        model.cameraDeviceProbe = probe
        model.coreCommandCaptureForTesting = { _ in }
        model.cameraExtensionOptedIn = true
        model.virtualCamera = PersistedVirtualCamera(enabled: intentOn)
        model.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        return (model, fake, probe)
    }

    @MainActor
    static func testInUseUpdateIsDeferredAndNothingSubmitted() {
        let (model, fake, _) = launchedModel(installed: [olderInstall],
                                             device: CameraDeviceStatus(registered: true, runningSomewhere: true))
        expectEqual(fake.calls, ["installs"], "camera streaming somewhere: no request is submitted")
        expectEqual(model.cameraExtensionState, .updateReady, "the update waits as .updateReady")
        expectEqual(model.virtualCameraLabel,
                    "An update to CoreVideo Pro Camera is ready. Quit apps that are using the camera (Zoom, Teams, Photo Booth\u{2026}), then click Update camera.",
                    "the label says what to do, even with our output off")
        expect(VirtualCameraPresentation.showsUpdateButton(model.cameraExtensionState), "the Update camera button shows")
        expect(!VirtualCameraPresentation.showsUpdateButton(.active), "and only then")
        model.stopTimersForTesting()

        let (own, ownFake, _) = launchedModel(installed: [olderInstall],
                                              device: CameraDeviceStatus(registered: true, runningSomewhere: false),
                                              intentOn: true)
        expectEqual(ownFake.calls, ["installs"], "our own output on counts as in use")
        expectEqual(own.cameraExtensionState, .updateReady, "and defers the update too")
        own.stopTimersForTesting()
    }

    @MainActor
    static func testFreeCameraUpdateReplaces() {
        let (model, fake, _) = launchedModel(installed: [olderInstall],
                                             device: CameraDeviceStatus(registered: true, runningSomewhere: false))
        expectEqual(fake.calls, ["installs", "activate"], "a free camera is updated at launch")
        expectEqual(model.cameraExtensionState, .active, "and the activation result lands")
        model.stopTimersForTesting()
    }

    @MainActor
    static func testUpdateReadyRetriesWhenTheCameraFrees() {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(script: [.activating, .active], installed: [olderInstall])
        let probe = FakeCameraDeviceProbe(CameraDeviceStatus(registered: true, runningSomewhere: true))
        model.cameraExtension = fake
        model.cameraDeviceProbe = probe
        model.coreCommandCaptureForTesting = { _ in }
        model.cameraExtensionOptedIn = true
        model.cameraUpdateRetryInterval = 0.1
        model.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.25))
        expectEqual(fake.calls, ["installs"], "still in use: the retry submits nothing")
        probe.status.runningSomewhere = false
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        expectEqual(fake.calls, ["installs", "activate"], "the camera freed: the retry installs the update once")
        expectEqual(model.cameraExtensionState, .active, "and the update completes")
        model.stopTimersForTesting()

        // Our own output was the only user: switching it off retries at once.
        let (own, ownFake, _) = launchedModel(installed: [olderInstall],
                                              device: CameraDeviceStatus(registered: true, runningSomewhere: false),
                                              intentOn: true)
        own.cameraUpdateRetryInterval = 60
        expectEqual(own.cameraExtensionState, .updateReady, "precondition: deferred by our own output")
        own.setVirtualCamera(enabled: false)
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(ownFake.calls, ["installs", "activate"], "turning our output off installs the update")
        own.stopTimersForTesting()
    }

    @MainActor
    static func testUpdateCameraButtonPausesOurOutputThenReplaces() {
        let (model, fake, _) = launchedModel(installed: [olderInstall],
                                             device: CameraDeviceStatus(registered: true, runningSomewhere: false),
                                             intentOn: true)
        var sent: [Bool] = []
        model.coreCommandCaptureForTesting = { body in
            for c in (body["commands"] as? [JSONObject]) ?? [] where c["type"] as? String == "sync-virtual-camera" {
                sent.append(c["on"] as? Bool ?? true)
            }
        }
        expectEqual(model.cameraExtensionState, .updateReady, "precondition: deferred by our own output")
        model.updateCameraExtension()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(fake.calls, ["installs", "activate"], "Update camera replaces once our output is paused")
        expectEqual(sent, [false, true], "our output is paused for the swap, then resumed")
        expectEqual(model.virtualCamera.enabled, true, "the saved intent is never touched")
        expectEqual(model.cameraExtensionState, .active, "the update completes")
        model.stopTimersForTesting()

        // Another app still streaming: no request, stay ready, resume our output.
        let (busy, busyFake, _) = launchedModel(installed: [olderInstall],
                                                device: CameraDeviceStatus(registered: true, runningSomewhere: true),
                                                intentOn: true)
        var busySent: [Bool] = []
        busy.coreCommandCaptureForTesting = { body in
            for c in (body["commands"] as? [JSONObject]) ?? [] where c["type"] as? String == "sync-virtual-camera" {
                busySent.append(c["on"] as? Bool ?? true)
            }
        }
        busy.cameraUpdateReleaseWait = 0.2
        busy.cameraUpdatePollInterval = 0.05
        busy.cameraUpdateRetryInterval = 60
        busy.updateCameraExtension()
        RunLoop.current.run(until: Date().addingTimeInterval(0.45))
        expectEqual(busyFake.calls, ["installs"], "another app holds the camera: nothing is submitted")
        expectEqual(busy.cameraExtensionState, .updateReady, "the update stays ready")
        expectEqual(busySent, [false, true], "and our output is resumed")
        busy.stopTimersForTesting()
    }

    /// Belt and braces at the moment macOS offers the replacement: a different
    /// version while in use is cancelled and ends `.updateReady`; equal versions
    /// are kept whatever the use.
    static func testReplacementOfferedWhileInUseEndsUpdateReady() {
        let extDomain = "OSSystemExtensionErrorDomain"
        let v = CameraExtensionPolicy.bundledVersion
        func run(inUse: Bool, existing: String) -> (OSSystemExtensionRequest.ReplacementAction?, [CameraExtensionState]) {
            var submitted: [OSSystemExtensionRequest] = []
            let a = SystemCameraExtensionActivator(bundlePath: "/Applications/CoreVideo Pro.app",
                                                   hasProfile: true, submit: { submitted.append($0) })
            var states: [CameraExtensionState] = []
            a.activate(deferringReplacementWhile: { inUse }) { states.append($0) }
            guard let r = submitted.first else { return (nil, states) }
            let action = a.request(r, actionForReplacingExtension: StubExtensionProperties(existing),
                                   withExtension: StubExtensionProperties(v))
            if action == .cancel {
                a.request(r, didFailWithError: NSError(domain: extDomain, code: 11))
            } else {
                a.request(r, didFinishWithResult: .completed)
            }
            return (action, states)
        }
        let busy = run(inUse: true, existing: "2883")
        expectEqual(busy.0, .cancel, "in use: the replacement is refused")
        expectEqual(busy.1, [.activating, .updateReady], "and the request ends .updateReady, never .failed")
        let free = run(inUse: false, existing: "2883")
        expectEqual(free.0, .replace, "free: replaced")
        expectEqual(free.1, [.activating, .active], "and active")
        let same = run(inUse: true, existing: v)
        expectEqual(same.0, .cancel, "equal versions are kept")
        expectEqual(same.1, [.activating, .active], "kept, not an update")
    }
}
