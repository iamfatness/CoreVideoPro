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
            ("vcam-update/update-button-race-never-resumes-mid-swap", testUpdateCameraNeverResumesOurOutputMidSwap),
            ("vcam-update/launch-holds-our-output-until-the-swap", testLaunchHoldsOurOutputUntilTheSwap),
            ("vcam-update/notice-is-a-second-line", testUpdateNoticeIsASecondLineWithTheRightCopy),
            ("vcam-update/zoom-quits-notice-stops-saying-quit", testZoomQuittingClearsTheQuitAppsNotice),
            ("vcam-update/old-device-is-not-the-new-one", testTheOldCopysDeviceIsNotTheNewOne),
            ("vcam-update/mid-session-loss-rechecks", testMidSessionLossRechecksAndReportsARestart),
            ("vcam-update/replacement-offered-in-use-is-cancelled",
             testReplacementOfferedWhileInUseEndsUpdateReady),
            ("vcam-update/completed-unregistered-old-uninstalling-needs-reboot",
             testCompletedButUnregisteredWithOldUninstallingNeedsReboot),
            ("vcam-update/owner-mac-stuck-state-at-launch-needs-reboot", testOwnerMacStuckStateAtLaunchNeedsReboot),
            ("vcam-update/will-complete-after-reboot", testWillCompleteAfterRebootIsNeedsReboot),
            ("vcam-update/needs-reboot-policy", testNeedsRebootPolicy),
            ("vcam-update/owner-mac-today-restart-not-another-swap", testOwnerMacTodayGoesStraightToNeedsReboot),
            ("vcam-update/preflight-outranks-update", testPreflightOutranksAnUpdate),
            ("vcam-update/diagnostics-in-support-bundle", testVirtualCameraDiagnosticsReachTheSupportBundle),
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

    /// Our bundled version's device (its model UID names the version).
    static func ourDevice(running: Bool = false) -> CameraDeviceStatus {
        CameraDeviceStatus(registered: true, runningSomewhere: running, version: CameraExtensionPolicy.bundledVersion)
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

    /// Every sync-virtual-camera the model sends, with the extension state at
    /// the moment it was sent.
    final class WireLog {
        var sent: [(on: Bool, state: CameraExtensionState)] = []
        var ons: [Bool] { sent.map(\.on) }
    }

    /// Wires the model the way production runs: every core command goes to the
    /// probe (our sink stream follows the wire) and to `log`.
    @MainActor
    static func wire(_ model: AppModel, _ probe: FakeCameraDeviceProbe, _ log: WireLog) {
        model.coreCommandCaptureForTesting = { [weak model] body in
            probe.observe(body)
            for c in (body["commands"] as? [JSONObject]) ?? [] where c["type"] as? String == "sync-virtual-camera" {
                log.sent.append((c["on"] as? Bool ?? true, model?.cameraExtensionState ?? .unknown))
            }
        }
    }

    @MainActor
    static func launchedModel(installed: [CameraExtensionInstall], device: CameraDeviceStatus,
                              intentOn: Bool = false, script: [CameraExtensionState] = [.activating, .active],
                              settle: TimeInterval = 0.05)
        -> (AppModel, FakeCameraExtensionActivator, FakeCameraDeviceProbe, WireLog) {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(script: script, installed: installed)
        let probe = FakeCameraDeviceProbe(device)
        let log = WireLog()
        model.cameraExtension = fake
        model.cameraDeviceProbe = probe
        wire(model, probe, log)
        model.cameraExtensionOptedIn = true
        model.virtualCamera = PersistedVirtualCamera(enabled: intentOn)
        model.startCameraExtensionAtLaunch()
        // The core connecting re-sends the intent (onConnected's resend).
        model.handleBridgeStatus(.connected(renderer: "test"))
        RunLoop.current.run(until: Date().addingTimeInterval(settle))
        return (model, fake, probe, log)
    }

    @MainActor
    static func testInUseUpdateIsDeferredAndNothingSubmitted() {
        let (model, fake, _, _) = launchedModel(installed: [olderInstall],
                                                device: ourDevice(running: true))
        expectEqual(fake.calls, ["installs"], "another app streaming: no request is submitted")
        expectEqual(model.cameraExtensionState, .updateReady, "the update waits as .updateReady")
        expectEqual(model.virtualCameraLabel, "Virtual camera off", "the status line stays the status")
        expectEqual(model.virtualCameraUpdateNotice,
                    "An update to CoreVideo Pro Camera is ready. Quit apps that are using the camera (Zoom, Teams, Photo Booth\u{2026}), then click Update camera.",
                    "another app holds it: the second line says to quit apps")
        expect(VirtualCameraPresentation.showsUpdateButton(model.cameraExtensionState), "the Update camera button shows")
        expect(!VirtualCameraPresentation.showsUpdateButton(.active), "and only then")
        model.stopTimersForTesting()
    }

    @MainActor
    static func testFreeCameraUpdateReplaces() {
        let (model, fake, _, _) = launchedModel(installed: [olderInstall],
                                                device: ourDevice(running: false))
        expectEqual(fake.calls, ["installs", "activate"], "a free camera is updated at launch")
        expectEqual(model.cameraExtensionState, .active, "and the activation result lands")
        model.stopTimersForTesting()
    }

    @MainActor
    static func testUpdateReadyRetriesWhenTheCameraFrees() {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(script: [.activating, .active], installed: [olderInstall])
        let probe = FakeCameraDeviceProbe(ourDevice(running: true))
        model.cameraExtension = fake
        model.cameraDeviceProbe = probe
        wire(model, probe, WireLog())
        model.cameraExtensionOptedIn = true
        model.cameraUpdateRetryInterval = 0.1
        model.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.25))
        expectEqual(fake.calls, ["installs"], "still in use: the retry submits nothing")
        probe.status.runningSomewhere = false
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        expectEqual(fake.calls, ["installs", "activate"], "the other app quit: the retry installs the update once")
        expectEqual(model.cameraExtensionState, .active, "and the update completes")
        model.stopTimersForTesting()
    }

    /// The production path of the button: our output is streaming (the HAL
    /// says running), Update camera sends on:false, the stream stops a few
    /// reads later, THEN the replacement is submitted, then our output resumes.
    @MainActor
    static func testUpdateCameraButtonPausesOurOutputThenReplaces() {
        // Another app held the camera at launch, then quit; our output is on.
        let (model, fake, probe, log) = launchedModel(
            installed: [olderInstall], device: ourDevice(running: true),
            intentOn: true)
        model.cameraUpdateRetryInterval = 60
        model.cameraUpdatePollInterval = 0.03
        probe.status.runningSomewhere = false
        expectEqual(model.cameraExtensionState, .updateReady, "precondition: an update is waiting")
        expect(probe.ownStreaming, "precondition: our output is streaming")
        log.sent.removeAll()
        model.updateCameraExtension()
        RunLoop.current.run(until: Date().addingTimeInterval(0.3))
        expectEqual(fake.calls, ["installs", "activate"], "Update camera replaces once our stream has stopped")
        expectEqual(log.ons, [false, true], "our output is paused for the swap, then resumed")
        expect(probe.reads >= 3, "the release was polled, not assumed")
        expectEqual(model.virtualCamera.enabled, true, "the saved intent is never touched")
        expectEqual(model.cameraExtensionState, .active, "the update completes")
        model.stopTimersForTesting()

        // Another app still streaming: no request, stay ready, resume our output.
        let (busy, busyFake, _, busyLog) = launchedModel(
            installed: [olderInstall], device: ourDevice(running: true),
            intentOn: true)
        busy.cameraUpdateReleaseWait = 0.2
        busy.cameraUpdatePollInterval = 0.05
        busy.cameraUpdateRetryInterval = 60
        busyLog.sent.removeAll()
        busy.updateCameraExtension()
        RunLoop.current.run(until: Date().addingTimeInterval(0.45))
        expectEqual(busyFake.calls, ["installs"], "another app holds the camera: nothing is submitted")
        expectEqual(busy.cameraExtensionState, .updateReady, "the update stays ready")
        expectEqual(busyLog.ons, [false, true], "and our output is resumed")
        busy.stopTimersForTesting()
    }

    /// Review I1: a double click, and the 5 s retry firing while the click
    /// waits, must not start a second swap or resume our output while the
    /// replacement is still in flight.
    @MainActor
    static func testUpdateCameraNeverResumesOurOutputMidSwap() {
        let (model, fake, probe, log) = launchedModel(
            installed: [olderInstall], device: ourDevice(running: true),
            intentOn: true)
        probe.status.runningSomewhere = false   // the other app quit; our output still streams
        fake.completionDelay = 0.3              // sysextd takes a while
        model.cameraUpdateRetryInterval = 0.05   // the retry fires during the wait and the swap
        model.cameraUpdatePollInterval = 0.03
        log.sent.removeAll()
        model.updateCameraExtension()
        expectEqual(model.virtualCameraLabel, "Updating camera\u{2026}", "the click shows progress")
        expect(!VirtualCameraPresentation.showsUpdateButton(model.cameraExtensionState,
                                                           updating: model.cameraUpdateInProgress),
               "and the button goes away while it runs")
        RunLoop.current.run(until: Date().addingTimeInterval(0.04))
        model.updateCameraExtension()            // the double click
        RunLoop.current.run(until: Date().addingTimeInterval(0.7))
        expectEqual(fake.calls, ["installs", "activate"], "exactly one replacement")
        expectEqual(log.ons, [false, true], "paused once, resumed once")
        expect(log.sent.last.map { $0.state != .activating } ?? false,
               "our output resumes only after the replacement finished, never mid-swap")
        expectEqual(model.cameraExtensionState, .active, "the update completes")
        expect(!model.cameraUpdateInProgress, "and the progress state clears")
        model.stopTimersForTesting()
    }

    /// Review I3: with the output on (the normal setup) and no other app on
    /// the camera, an update swaps at launch: the first on:true waits until
    /// the replacement finished, so our own stream never blocks it.
    @MainActor
    static func testLaunchHoldsOurOutputUntilTheSwap() {
        let (model, fake, _, log) = launchedModel(
            installed: [olderInstall], device: ourDevice(running: false),
            intentOn: true, script: [.activating, .active])
        expectEqual(fake.calls, ["installs", "activate"], "our output on, nobody else: swapped at launch")
        expectEqual(log.ons.first, false, "the core's first sync is held off")
        expectEqual(log.ons.last, true, "then our output starts")
        expect(log.sent.filter { $0.on }.allSatisfy { $0.state == .active },
               "on:true only after the replacement finished")
        expectEqual(model.cameraExtensionState, .active, "and the update completes")
        model.stopTimersForTesting()

        // Nothing to swap: the hold releases at once.
        let (same, sameFake, _, sameLog) = launchedModel(
            installed: [ours()], device: ourDevice(), intentOn: true)
        expectEqual(sameFake.calls, ["installs"], "our version active: nothing submitted")
        expectEqual(sameLog.ons.last, true, "and our output starts right away")
        same.stopTimersForTesting()
    }

    @MainActor
    static func testUpdateNoticeIsASecondLineWithTheRightCopy() {
        typealias P = VirtualCameraPresentation
        var live = VirtualCameraReadModel()
        live.status = "live"; live.width = 1920; live.height = 1080
        expectEqual(P.label(intent: true, ext: .updateReady, core: live), "Virtual camera live (1920x1080)",
                    "an update waiting never hides the live status")
        let neutral = "An update to CoreVideo Pro Camera is ready. Close any other apps using the camera, then click Update camera \u{2014} your camera output pauses for a few seconds."
        expectEqual(P.updateNotice(ext: .updateReady, othersHolding: true, ownOutputOn: true, updating: false),
                    neutral, "our output on: we cannot tell who else holds it, so the copy covers both")
        expectEqual(P.updateNotice(ext: .updateReady, othersHolding: false, ownOutputOn: false, updating: false),
                    neutral, "nobody seen: neutral")
        expect(P.updateNotice(ext: .updateReady, othersHolding: true, ownOutputOn: false, updating: false)?
                .contains("Quit apps") == true, "our output off and another app on the camera: quit apps")
        expect(P.updateNotice(ext: .active, othersHolding: true, ownOutputOn: false, updating: false) == nil,
               "no notice otherwise")
        expect(P.updateNotice(ext: .updateReady, othersHolding: false, ownOutputOn: true, updating: true) == nil,
               "none while Updating camera runs")
    }

    /// Re-review NEW-I1: launch while Zoom has the camera, then Zoom quits.
    /// The notice must not keep saying "Quit apps" for the rest of the session.
    @MainActor
    static func testZoomQuittingClearsTheQuitAppsNotice() {
        // Output on: Zoom held it at launch, then quit; our stream still runs.
        let (on, _, onProbe, _) = launchedModel(installed: [olderInstall], device: ourDevice(running: true),
                                                intentOn: true)
        on.cameraUpdateRetryInterval = 60
        expectEqual(on.cameraExtensionState, .updateReady, "precondition: deferred by Zoom")
        onProbe.status.runningSomewhere = false   // Zoom quits
        _ = on.cameraInUse()                      // a later re-check (the 5 s retry)
        expect(on.virtualCameraUpdateNotice?.contains("Quit apps") == false,
               "our output on: the notice no longer says quit apps")
        expect(on.virtualCameraUpdateNotice?.contains("Close any other apps") == true, "it is the neutral copy")
        on.stopTimersForTesting()

        // Output off: Zoom held it; the first free read clears the flag.
        let (off, _, offProbe, _) = launchedModel(installed: [olderInstall], device: ourDevice(running: true))
        off.cameraUpdateRetryInterval = 60
        expect(off.virtualCameraUpdateNotice?.contains("Quit apps") == true,
               "precondition: our output off, Zoom on the camera: quit apps")
        offProbe.status.runningSomewhere = false   // Zoom quits
        _ = off.readCameraDevice()
        expect(!off.cameraUpdateOthersHolding, "a free read clears 'another app holds it'")
        expect(off.virtualCameraUpdateNotice?.contains("Quit apps") == false, "and the quit-apps copy goes")
        off.stopTimersForTesting()

        // Any state away from .updateReady clears it too, even with Zoom
        // still on the camera (here: the operator removes the camera).
        let (left, _, _, _) = launchedModel(installed: [olderInstall], device: ourDevice(running: true))
        expect(left.cameraUpdateOthersHolding, "precondition: Zoom seen")
        left.removeVirtualCamera()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(left.cameraExtensionState, .notInstalled, "precondition: left .updateReady")
        expect(!left.cameraUpdateOthersHolding, "and the flag is gone with .updateReady")
        left.stopTimersForTesting()
    }

    /// Review I4: after a replacement the OLD copy's device (same UID, no
    /// version in its model) can still be in the HAL. That is not success.
    @MainActor
    static func testTheOldCopysDeviceIsNotTheNewOne() {
        let stuck = [ours(), CameraExtensionInstall(version: "2883", enabled: false, uninstalling: true)]
        let lingering = CameraDeviceStatus(registered: true, runningSomewhere: false, version: nil)
        let (model, fake) = activatedModel(installed: stuck, device: lingering)
        expectEqual(fake.calls, ["activate", "installs"], "the old device is not taken for the new one")
        expectEqual(model.cameraExtensionState, .needsReboot, "and the stuck swap is reported")
        model.stopTimersForTesting()

        let (fresh, freshFake) = activatedModel(installed: stuck, device: ourDevice())
        expectEqual(freshFake.calls, ["activate"], "the new version's device is success")
        expectEqual(fresh.cameraExtensionState, .active, "and stays active")
        fresh.stopTimersForTesting()

        expectEqual(CameraConstants.extensionVersion(fromDeviceModel: CameraConstants.deviceModel),
                    CameraExtensionPolicy.bundledVersion, "the device model carries the extension version")
        expect(CameraConstants.extensionVersion(fromDeviceModel: "CoreVideo Pro Camera") == nil,
               "a pre-3001 model names no version")
    }

    /// Review I4: the camera vanishing mid-session (the core reports it lost
    /// for longer than the re-check delay) re-runs the check.
    @MainActor
    static func testMidSessionLossRechecksAndReportsARestart() {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(installed: [ours()])
        let probe = FakeCameraDeviceProbe(ourDevice())
        model.cameraExtension = fake
        model.cameraDeviceProbe = probe
        wire(model, probe, WireLog())
        model.cameraExtensionOptedIn = true
        model.cameraLostRecheckAfter = 0.1
        model.virtualCamera = PersistedVirtualCamera(enabled: true)
        model.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(model.cameraExtensionState, .active, "precondition: active with our device")
        // The extension dies; macOS now lists an older copy uninstalling.
        probe.status = CameraDeviceStatus()
        fake.installed = [ours(), CameraExtensionInstall(version: "2883", enabled: false, uninstalling: true)]
        var lost = VirtualCameraReadModel()
        lost.status = "failed"; lost.warning = "CAMERA_EXTENSION_LOST: sink vanished"
        model.virtualCameraCore = lost
        RunLoop.current.run(until: Date().addingTimeInterval(0.15))
        model.virtualCameraCore = lost   // the next snapshot, still lost
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(model.cameraExtensionState, .needsReboot, "a lost camera with a restart pending says so")
        model.stopTimersForTesting()
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

    // ── item 3: detect stuck / needs-restart ────────────────────────────────

    @MainActor
    static func activatedModel(installed: [CameraExtensionInstall], device: CameraDeviceStatus)
        -> (AppModel, FakeCameraExtensionActivator) {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(script: [.activating, .active], installed: installed)
        model.cameraExtension = fake
        model.cameraDeviceProbe = FakeCameraDeviceProbe(device)
        model.coreCommandCaptureForTesting = { _ in }
        model.cameraRegistrationTimeout = 0.2
        model.cameraRegistrationPollInterval = 0.05
        model.setVirtualCamera(enabled: true)  // first enable: activates
        RunLoop.current.run(until: Date().addingTimeInterval(0.5))
        return (model, fake)
    }

    @MainActor
    static func testCompletedButUnregisteredWithOldUninstallingNeedsReboot() {
        let stuck = [ours(), CameraExtensionInstall(version: "2883", enabled: false, uninstalling: true)]
        let (model, fake) = activatedModel(installed: stuck, device: CameraDeviceStatus())
        expectEqual(fake.calls, ["activate", "installs"], "no device after completion: ask what is installed")
        expectEqual(model.cameraExtensionState, .needsReboot, "an older copy uninstalling: only a restart finishes it")
        expectEqual(model.virtualCameraLabel, "Restart your Mac to finish updating the virtual camera.",
                    "the label says so")
        model.stopTimersForTesting()

        let (registered, regFake) = activatedModel(installed: stuck,
                                                   device: ourDevice())
        expectEqual(regFake.calls, ["activate"], "a registered device needs no question")
        expectEqual(registered.cameraExtensionState, .active, "and stays active")
        registered.stopTimersForTesting()

        let (clean, _) = activatedModel(installed: [ours()], device: CameraDeviceStatus())
        expectEqual(clean.cameraExtensionState, .active, "unregistered but nothing pending: no restart claim")
        clean.stopTimersForTesting()
    }

    /// The owner-Mac state after a stuck swap to OUR version: ours
    /// [activated enabled], the old one uninstalling, no device.
    @MainActor
    static func testOwnerMacStuckStateAtLaunchNeedsReboot() {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(
            installed: [ours(), CameraExtensionInstall(version: "2881", enabled: false, uninstalling: true)])
        model.cameraExtension = fake
        model.cameraDeviceProbe = FakeCameraDeviceProbe(CameraDeviceStatus())
        model.coreCommandCaptureForTesting = { _ in }
        model.cameraRegistrationTimeout = 0.2
        model.cameraRegistrationPollInterval = 0.05
        model.cameraExtensionOptedIn = true
        model.virtualCamera = PersistedVirtualCamera(enabled: true)
        model.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.5))
        expectEqual(fake.calls, ["installs"], "nothing is submitted")
        expectEqual(model.cameraExtensionState, .needsReboot, "the stuck swap is reported as needing a restart")
        model.stopTimersForTesting()
    }

    /// Review I2a: the owner's Mac as it is NOW (2883 enabled, 2881 waiting
    /// to uninstall, no device). Not another swap, not "Update camera":
    /// "Restart your Mac", nothing submitted, whatever the intent.
    @MainActor
    static func testOwnerMacTodayGoesStraightToNeedsReboot() {
        let today = [CameraExtensionInstall(version: "2883", enabled: true),
                     CameraExtensionInstall(version: "2881", enabled: false, uninstalling: true)]
        for intentOn in [false, true] {
            let (model, fake, _, _) = launchedModel(installed: today, device: CameraDeviceStatus(),
                                                    intentOn: intentOn)
            expectEqual(fake.calls, ["installs"], "intent \(intentOn): nothing is submitted")
            expectEqual(model.cameraExtensionState, .needsReboot, "intent \(intentOn): a restart is what finishes it")
            model.stopTimersForTesting()
        }
    }

    /// Review I2b: from the DMG (or a dev build) with an older copy installed,
    /// the preflight's own message comes first; a working copy of our version
    /// is still fine from anywhere.
    @MainActor
    static func testPreflightOutranksAnUpdate() {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(installed: [olderInstall])
        fake.preflight = .notInApplications
        let probe = FakeCameraDeviceProbe(ourDevice(running: true))
        model.cameraExtension = fake
        model.cameraDeviceProbe = probe
        wire(model, probe, WireLog())
        model.cameraExtensionOptedIn = true
        model.virtualCamera = PersistedVirtualCamera(enabled: true)
        model.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(model.cameraExtensionState, .notInApplications, "Move to Applications, not Update camera")
        expectEqual(fake.calls, ["installs"], "and nothing is submitted")
        model.stopTimersForTesting()

        let fine = AppModel()
        let fake2 = FakeCameraExtensionActivator(installed: [ours()])
        fake2.preflight = .notInApplications
        fine.cameraExtension = fake2
        fine.cameraDeviceProbe = FakeCameraDeviceProbe(ourDevice())
        fine.coreCommandCaptureForTesting = { _ in }
        fine.cameraExtensionOptedIn = true
        fine.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(fine.cameraExtensionState, .active, "our version already working stays active from the DMG")
        fine.stopTimersForTesting()
    }

    /// macOS's own "will complete after reboot" answer maps to .needsReboot.
    static func testWillCompleteAfterRebootIsNeedsReboot() {
        var submitted: [OSSystemExtensionRequest] = []
        let a = SystemCameraExtensionActivator(bundlePath: "/Applications/CoreVideo Pro.app",
                                               hasProfile: true, submit: { submitted.append($0) })
        var states: [CameraExtensionState] = []
        a.activate { states.append($0) }
        guard let r = submitted.first else { expect(false, "activation submitted"); return }
        a.request(r, didFinishWithResult: .willCompleteAfterReboot)
        expectEqual(states, [.activating, .needsReboot], "willCompleteAfterReboot -> needsReboot")
        expectEqual(VirtualCameraPresentation.label(intent: false, ext: .needsReboot, core: nil),
                    "Restart your Mac to finish updating the virtual camera.",
                    "persistent: shown even with the output off")
    }

    static func testNeedsRebootPolicy() {
        let v = CameraExtensionPolicy.bundledVersion
        let f = { CameraExtensionPolicy.needsReboot(unregisteredWith: $0, bundledVersion: v) }
        expect(!f(nil), "no answer never claims a restart")
        expect(!f([ours()]), "ours enabled, nothing uninstalling: no")
        expect(f([ours(), CameraExtensionInstall(version: "2883", uninstalling: true)]), "older uninstalling: yes")
        expect(f([ours(enabled: false)]), "ours not enabled: yes")
        expect(f([]), "ours missing after a completed activation: yes")
        expect(!f([CameraExtensionInstall(version: v, awaitingApproval: true)]), "awaiting approval is not a restart")
    }

    // ── item 4: diagnose ─────────────────────────────────────────────────────

    /// The support bundle's shell-state.json carries everything support needs:
    /// bundled version, installed versions and states, device registered,
    /// in use, the last activation result.
    @MainActor
    static func testVirtualCameraDiagnosticsReachTheSupportBundle() {
        let installed = [ours(), CameraExtensionInstall(version: "2881", enabled: false, uninstalling: true)]
        let (model, _, _, _) = launchedModel(installed: installed,
                                          device: ourDevice(running: true))
        let input = model.supportBundleInput()
        guard let state = try? JSONSerialization.jsonObject(with: input.shellStateJson) as? JSONObject,
              let vcam = state["virtualCamera"] as? JSONObject else {
            expect(false, "shell-state.json has a virtualCamera node"); return
        }
        expectEqual(vcam["bundledExtensionVersion"] as? String, CameraExtensionPolicy.bundledVersion,
                    "the bundled extension version")
        let rows = vcam["installed"] as? [JSONObject] ?? []
        expectEqual(rows.compactMap { $0["version"] as? String }, [CameraExtensionPolicy.bundledVersion, "2881"],
                    "every installed version")
        expectEqual(rows.last?["uninstalling"] as? Bool, true, "with its state")
        expectEqual(vcam["deviceRegistered"] as? Bool, true, "device registered yes/no")
        expectEqual(vcam["inUse"] as? Bool, true, "the in-use flag")
        expect((vcam["lastActivationResult"] as? String ?? "").hasPrefix("already active at launch"),
               "the last activation result")
        let diag = model.virtualCameraDiagnostics()
        expect(diag.rows.contains { $0.label == "installed" && $0.value.contains("2881 [disabled, uninstalling") },
               "the Health tab row names the uninstalling copy")
        model.stopTimersForTesting()
    }
}
