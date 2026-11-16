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
}
