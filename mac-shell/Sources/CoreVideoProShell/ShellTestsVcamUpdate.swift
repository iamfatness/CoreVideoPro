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
            ("vcam-update/completed-unregistered-ours-not-enabled-needs-reboot",
             testCompletedButUnregisteredWithOldUninstallingNeedsReboot),
            ("vcam-update/staged-swap-at-launch-needs-reboot", testOwnerMacStuckStateAtLaunchNeedsReboot),
            ("vcam-update/will-complete-after-reboot", testWillCompleteAfterRebootIsNeedsReboot),
            ("vcam-update/needs-reboot-policy", testNeedsRebootPolicy),
            ("vcam-update/older-enabled-copy-is-updated-not-restarted", testOwnerMacTodayGoesStraightToNeedsReboot),
            ("vcam-update/enabled-old-uninstalling-device-visible-is-active",
             testEnabledWithOldCopyUninstallingAndDeviceVisibleIsActive),
            ("vcam-update/device-is-read-from-a-new-process", testTheDeviceIsReadFromANewProcess),
            ("vcam-update/launch-action-policy", testLaunchActionPolicy),
            ("vcam-update/preflight-outranks-update", testPreflightOutranksAnUpdate),
            ("vcam-update/diagnostics-in-support-bundle", testVirtualCameraDiagnosticsReachTheSupportBundle),
            ("vcam-update/device-is-never-read-on-the-main-thread", testTheDeviceIsNeverReadOnTheMainThread),
            ("vcam-update/device-reads-never-overlap", testDeviceReadsNeverOverlap),
            ("vcam-update/superseded-device-read-is-dropped", testASupersededDeviceReadDoesNotOverwriteANewerOne),
            ("vcam-update/late-device-read-decides-nothing", testADeviceReadThatArrivesLateDecidesNothing),
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
        on.checkCameraInUse { _ in }              // a later re-check (the 5 s retry)
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
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
        off.readCameraDevice { _ in }
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
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
        // 2026-11-16: with OUR version enabled, an older copy still listed as
        // uninstalling is not a restart (it stayed listed after the owner's
        // restart while the new copy worked).
        expectEqual(model.cameraExtensionState, .active, "ours is enabled: no restart is claimed")
        model.stopTimersForTesting()

        let staged = [ours(enabled: false), CameraExtensionInstall(version: "2883", enabled: false, uninstalling: true)]
        let (stagedModel, _) = activatedModel(installed: staged, device: lingering)
        expectEqual(stagedModel.cameraExtensionState, .needsReboot, "ours NOT enabled: the staged swap is reported")
        stagedModel.stopTimersForTesting()

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
        // The extension dies; macOS still lists OUR version enabled next to an
        // older copy uninstalling. Not a restart.
        probe.status = CameraDeviceStatus()
        fake.installed = [ours(), CameraExtensionInstall(version: "2883", enabled: false, uninstalling: true)]
        var lost = VirtualCameraReadModel()
        lost.status = "failed"; lost.warning = "CAMERA_EXTENSION_LOST: sink vanished"
        model.virtualCameraCore = lost
        RunLoop.current.run(until: Date().addingTimeInterval(0.15))
        model.virtualCameraCore = lost   // the next snapshot, still lost
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expect(fake.calls.filter { $0 == "installs" }.count >= 2, "the loss re-asks what is installed")
        expectEqual(model.cameraExtensionState, .active, "ours enabled: a lost camera is not a restart")
        // macOS reports OUR version itself as not enabled: that is a restart.
        fake.installed = [ours(enabled: false), CameraExtensionInstall(version: "2883", enabled: false, uninstalling: true)]
        model.virtualCameraCore = nil     // the loss ends...
        model.virtualCameraCore = lost    // ...and starts again
        RunLoop.current.run(until: Date().addingTimeInterval(0.15))
        model.virtualCameraCore = lost
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(model.cameraExtensionState, .needsReboot, "a lost camera with OUR version not enabled says restart")
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
        let stuck = [ours(enabled: false), CameraExtensionInstall(version: "2883", enabled: false, uninstalling: true)]
        let (model, fake) = activatedModel(installed: stuck, device: CameraDeviceStatus())
        expectEqual(fake.calls, ["activate", "installs"], "no device after completion: ask what is installed")
        expectEqual(model.cameraExtensionState, .needsReboot, "OUR version not enabled: only a restart finishes it")
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

        // OUR version enabled, an older copy uninstalling, no device: the old
        // copy alone is not evidence of a pending restart (2026-11-16).
        let enabledOldLeaving = [ours(), CameraExtensionInstall(version: "2883", enabled: false, uninstalling: true)]
        let (notARestart, _) = activatedModel(installed: enabledOldLeaving, device: CameraDeviceStatus())
        expectEqual(notARestart.cameraExtensionState, .active, "ours enabled: never 'Restart your Mac'")
        expect(notARestart.cameraExtensionLastResult.hasPrefix("enabled, but no device visible to a new process"),
               "and the diagnosis is recorded: \(notARestart.cameraExtensionLastResult)")
        notARestart.stopTimersForTesting()
    }

    /// A swap staged for a restart, seen at launch: OUR version installed but
    /// not enabled, the old one uninstalling, no device.
    @MainActor
    static func testOwnerMacStuckStateAtLaunchNeedsReboot() {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(
            installed: [ours(enabled: false), CameraExtensionInstall(version: "2881", enabled: false, uninstalling: true)])
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

    /// An OLDER version enabled (2883), another waiting to uninstall (2881),
    /// no device: the 2026-11-15 owner Mac. Review I2a answered "Restart your
    /// Mac, submit nothing". A restart did not fix it (2883 was crash-looping
    /// in its sandbox) and the older copy stays listed across restarts, so
    /// that answer would have blocked the one thing that did fix it: our
    /// version replacing 2883. Our version is not installed, so this is an
    /// update like any other.
    @MainActor
    static func testOwnerMacTodayGoesStraightToNeedsReboot() {
        let today = [CameraExtensionInstall(version: "2883", enabled: true),
                     CameraExtensionInstall(version: "2881", enabled: false, uninstalling: true)]
        for intentOn in [false, true] {
            let (model, fake, _, _) = launchedModel(installed: today, device: CameraDeviceStatus(),
                                                    intentOn: intentOn)
            expectEqual(fake.calls, ["installs", "activate"], "intent \(intentOn): the update is submitted")
            expect(model.cameraExtensionState != .needsReboot, "intent \(intentOn): no restart is claimed")
            model.stopTimersForTesting()
        }
    }

    /// THE 2026-11-16 CASE (plan, Task 4). The owner's Mac after the update:
    /// 3003 [activated enabled], 3002 [terminated waiting to uninstall on
    /// reboot], and the device visible to every NEW process. The shell's own
    /// CoreMediaIO view had no device (its provider list predates 3003), and it
    /// said "Restart your Mac". Read from a new process, this is simply active.
    @MainActor
    static func testEnabledWithOldCopyUninstallingAndDeviceVisibleIsActive() {
        let installs = [ours(), CameraExtensionInstall(version: "3002", enabled: false, uninstalling: true)]
        // At launch (the app auto-reopened at login).
        for intentOn in [false, true] {
            let (model, fake, _, _) = launchedModel(installed: installs, device: ourDevice(), intentOn: intentOn)
            expectEqual(fake.calls, ["installs"], "launch, intent \(intentOn): nothing is submitted")
            expectEqual(model.cameraExtensionState, .active, "launch, intent \(intentOn): active, not needsReboot")
            expect(model.virtualCameraLabel != VirtualCameraPresentation.needsRebootLabel,
                   "launch, intent \(intentOn): never 'Restart your Mac'")
            model.stopTimersForTesting()
        }
        // Right after the activation that installed it.
        let (activated, activatedFake) = activatedModel(installed: installs, device: ourDevice())
        expectEqual(activatedFake.calls, ["activate"], "after activation: the device is there, no question asked")
        expectEqual(activated.cameraExtensionState, .active, "after activation: active, not needsReboot")
        activated.stopTimersForTesting()

        let v = CameraExtensionPolicy.bundledVersion
        expectEqual(CameraExtensionPolicy.launchAction(installs: installs, bundledVersion: v, deviceRegistered: true),
                    .alreadyActive, "policy: enabled + device")
        expectEqual(CameraExtensionPolicy.launchAction(installs: installs, bundledVersion: v, deviceRegistered: false),
                    .alreadyActive, "policy: enabled is enabled, whatever one probe saw")
        expect(!CameraExtensionPolicy.needsReboot(unregisteredWith: installs, bundledVersion: v),
               "policy: an older copy uninstalling is not a restart")
    }

    /// The device is judged from a NEW process, never from the shell's own
    /// CoreMediaIO view (which predates any extension it installs).
    @MainActor
    static func testTheDeviceIsReadFromANewProcess() {
        expect(AppModel().cameraDeviceProbe is FreshProcessCameraDeviceProbe,
               "the default probe asks a new process")
        if let source = CoreCommands.locateCoreSource("mac-shell/Sources/CoreVideoProShell/AppModel.swift")
            .map({ ($0 as NSString).deletingLastPathComponent }) {
            let offenders = ((try? FileManager.default.contentsOfDirectory(atPath: source)) ?? [])
                .filter { $0.hasSuffix(".swift") && !$0.hasPrefix("ShellTests") }
                .filter { name in
                    let text = (try? String(contentsOfFile: source + "/" + name, encoding: .utf8)) ?? ""
                    return text.contains("import CoreMediaIO") || text.contains("CMIOObjectGetPropertyData")
                }
            expectEqual(offenders, [], "no shell source reads the CoreMediaIO HAL in-process")
        } else {
            print("SKIPPED: no shell sources alongside this binary (packaged app)")
        }

        // The probe's one line of JSON.
        let P = CameraDeviceProbeReport.self
        expectEqual(P.parse(#"{"registered":true,"runningSomewhere":false,"deviceUid":"us.iamfatness.corevideopro.camera.device","modelUid":"CoreVideo Pro Camera/3003","readMs":17.9}"#),
                    CameraDeviceStatus(registered: true, runningSomewhere: false, version: "3003"), "registered, idle")
        expectEqual(P.parse(#"{"registered":true,"runningSomewhere":true,"modelUid":"CoreVideo Pro Camera"}"#),
                    CameraDeviceStatus(registered: true, runningSomewhere: true, version: nil),
                    "a pre-3001 copy's device names no version")
        expectEqual(P.parse(#"{"registered":false,"runningSomewhere":false,"deviceUid":"","modelUid":""}"#),
                    CameraDeviceStatus(), "not registered")
        expectEqual(P.parse(#"{"registered":true}"#)?.runningSomewhere, true,
                    "a missing running flag counts as running, never as free")
        expectEqual(P.parse("[virtualcam] noise\n{\"registered\":false}\n"), CameraDeviceStatus(),
                    "other lines before the answer are ignored")
        expect(P.parse("") == nil && P.parse("not json") == nil && P.parse(#"{"other":1}"#) == nil,
               "anything else is not an answer")

        // The runner, against executables every Mac has.
        let answer = #"{"registered":true,"runningSomewhere":false,"modelUid":"CoreVideo Pro Camera/\#(CameraExtensionPolicy.bundledVersion)"}"#
        let echo = FreshProcessCameraDeviceProbe(executable: { "/bin/echo" }, arguments: [answer])
        expectEqual(echo.read(), ourDevice(), "a probe's answer becomes the device status")
        let none = FreshProcessCameraDeviceProbe(executable: { nil }).read()
        expect(none.probeFailure != nil && !none.registered && none.runningSomewhere,
               "no core to ask: unreadable (not registered, and never 'free')")
        let silent = FreshProcessCameraDeviceProbe(executable: { "/usr/bin/true" }, arguments: []).read()
        expectEqual(silent.probeFailure, "the probe's answer was not understood", "an empty answer is a failed read")
        let missing = FreshProcessCameraDeviceProbe(executable: { "/nonexistent/corevideo-native" }).read()
        expect(missing.probeFailure?.hasPrefix("could not start") == true, "a missing executable says so")
        let started = Date()
        let hung = FreshProcessCameraDeviceProbe(executable: { "/bin/sleep" }, arguments: ["30"], timeout: 0.3).read()
        expect(hung.probeFailure?.hasPrefix("no answer in") == true, "a probe that hangs is abandoned")
        expect(Date().timeIntervalSince(started) < 2, "within its timeout, not the probe's 30 s")

        // An unreadable device never reads as a fact.
        let model = AppModel()
        model.cameraDeviceProbe = FreshProcessCameraDeviceProbe(executable: { nil })
        var unreadableInUse: Bool?
        model.checkCameraInUse { unreadableInUse = $0 }
        expect(unreadableInUse == nil, "the read is not made in place: no answer before the caller returns")
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(unreadableInUse, true, "unreadable counts as in use: nothing is replaced on a guess")
        expect(model.cameraInUseNow(), "and the in-place check at a replacement offer says the same")
        let diag = model.virtualCameraDiagnostics()
        expect(diag.json["deviceRegistered"] is NSNull, "unreadable is null in the support bundle, never false")
        expect(diag.rows.contains { $0.label.hasPrefix("device registered") && $0.value.hasPrefix("not read:") },
               "and the Health row says it was not read")
        model.stopTimersForTesting()
    }

    static func testLaunchActionPolicy() {
        let v = CameraExtensionPolicy.bundledVersion
        func act(_ installs: [CameraExtensionInstall]?, device: Bool) -> CameraExtensionLaunchAction {
            CameraExtensionPolicy.launchAction(installs: installs, bundledVersion: v, deviceRegistered: device)
        }
        let oldLeaving = CameraExtensionInstall(version: "2881", enabled: false, uninstalling: true)
        expectEqual(act(nil, device: false), .activate, "no answer: activate")
        expectEqual(act([], device: false), .activate, "first install: activate")
        expectEqual(act([ours()], device: false), .alreadyActive, "ours enabled: nothing to submit")
        expectEqual(act([ours(enabled: false), oldLeaving], device: false), .needsReboot,
                    "ours installed but not enabled, old copy leaving, no device: a staged swap")
        expectEqual(act([ours(enabled: false), oldLeaving], device: true), .activate,
                    "a device exists: not a staged swap")
        expectEqual(act([CameraExtensionInstall(version: v, awaitingApproval: true), oldLeaving], device: false),
                    .activate, "awaiting approval is not a restart")
        expectEqual(act([olderInstall, oldLeaving], device: false), .update,
                    "ours not installed: an older copy leaving never blocks the update")
        expectEqual(act([oldLeaving], device: false), .activate, "only a leaving copy: install ours")
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
        expect(!f([ours(), CameraExtensionInstall(version: "2883", uninstalling: true)]),
               "ours enabled with an older copy uninstalling: no (2026-11-16)")
        expect(f([ours(enabled: false), CameraExtensionInstall(version: "2883", uninstalling: true)]),
               "ours not enabled with an older copy uninstalling: yes")
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

    // ── review finding 1 (2026-11-16): the probe never blocks the operator UI ──

    /// A probe that takes `delay` per read and records WHERE it was read. The
    /// real one is a spawn-and-wait of up to 2 s; on the main thread that is
    /// the operator UI frozen, and the operator cannot cut.
    final class SlowSpyCameraDeviceProbe: CameraDeviceProbing {
        private let lock = NSLock()
        private var _status: CameraDeviceStatus
        private var _reads = 0, _mainThreadReads = 0, _active = 0, _maxActive = 0
        let delay: TimeInterval

        init(_ status: CameraDeviceStatus, delay: TimeInterval) { _status = status; self.delay = delay }

        private func locked<T>(_ body: () -> T) -> T { lock.lock(); defer { lock.unlock() }; return body() }
        var status: CameraDeviceStatus {
            get { locked { _status } }
            set { locked { _status = newValue } }
        }
        var reads: Int { locked { _reads } }
        var mainThreadReads: Int { locked { _mainThreadReads } }
        var maxConcurrentReads: Int { locked { _maxActive } }
        var reading: Bool { locked { _active > 0 } }

        func read() -> CameraDeviceStatus {
            let main = Thread.isMainThread
            locked {
                _reads += 1
                if main { _mainThreadReads += 1 }
                _active += 1
                _maxActive = max(_maxActive, _active)
            }
            // What the probe saw when it looked; the answer arrives `delay` later.
            let seen = locked { _status }
            Thread.sleep(forTimeInterval: delay)
            locked { _active -= 1 }
            return seen
        }
    }

    /// Every path that reads the device, with a slow probe: launch, the
    /// `.updateReady` retry timer, the Health tab, the support bundle, Update
    /// camera's release wait and the registration check after the swap. None
    /// may read on the main thread, no two reads may overlap, and the main
    /// run loop must keep turning while a read is in progress.
    @MainActor
    static func testTheDeviceIsNeverReadOnTheMainThread() {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(script: [.activating, .active], installed: [olderInstall])
        let probe = SlowSpyCameraDeviceProbe(ourDevice(running: true), delay: 0.06)
        model.cameraExtension = fake
        model.cameraDeviceProbe = probe
        model.coreCommandCaptureForTesting = { _ in }
        model.cameraExtensionOptedIn = true
        model.cameraUpdateRetryInterval = 0.05
        model.cameraUpdatePollInterval = 0.02
        model.cameraUpdateReleaseWait = 0.3
        model.cameraRegistrationPollInterval = 0.02
        model.cameraRegistrationTimeout = 0.3

        // What the operator's UI is: main-thread work that must keep running.
        var turnsWhileReading = 0
        let heartbeat = Timer.scheduledTimer(withTimeInterval: 0.005, repeats: true) { _ in
            if probe.reading { turnsWhileReading += 1 }
        }
        defer { heartbeat.invalidate() }

        model.startCameraExtensionAtLaunch()
        RunLoop.current.run(until: Date().addingTimeInterval(0.6))
        expectEqual(model.cameraExtensionState, .updateReady, "precondition: another app holds the camera")
        expectEqual(fake.calls, ["installs"], "and nothing was submitted")
        let afterRetries = probe.reads
        expect(afterRetries >= 3, "launch and the retry timer read the device (\(afterRetries) reads)")

        model.refreshCameraExtensionDiagnostics()   // the Health tab appearing
        _ = model.supportBundleInput()              // Export support bundle
        model.updateCameraExtension()               // the Update camera button
        RunLoop.current.run(until: Date().addingTimeInterval(0.7))
        expectEqual(model.cameraExtensionState, .updateReady, "still held: the update stays ready")
        expect(!model.cameraUpdateInProgress, "and the click's wait ended")

        fake.installed = [ours()]                   // what macOS lists once the swap ran
        probe.status = ourDevice()                  // the other app quit; the swap can run
        RunLoop.current.run(until: Date().addingTimeInterval(0.8))
        expectEqual(fake.calls.filter { $0 == "activate" }.count, 1, "the retry installs the update once")
        expectEqual(model.cameraExtensionState, .active, "and the swap completes")
        model.stopTimersForTesting()

        expect(probe.reads > afterRetries + 3, "the later paths read the device too (\(probe.reads) reads)")
        expectEqual(probe.mainThreadReads, 0, "no device read ran on the main thread")
        expectEqual(probe.maxConcurrentReads, 1, "and no two reads overlapped")
        expect(turnsWhileReading > 0,
               "the main run loop kept turning while a read was in progress (\(turnsWhileReading) turns)")
    }

    /// The poll loops ask far faster than a slow probe answers. One probe runs
    /// at a time, and everything asked while it runs shares ONE follow-up that
    /// starts afterwards (so nobody is handed a reading older than their
    /// question).
    @MainActor
    static func testDeviceReadsNeverOverlap() {
        let probe = SlowSpyCameraDeviceProbe(ourDevice(running: true), delay: 0.1)
        let reader = CameraDeviceReader(probe: probe)
        var first: CameraDeviceStatus?
        var later: [CameraDeviceStatus] = []
        reader.read { first = $0 }
        expect(first == nil && reader.isReading, "read returns at once with the probe still out")
        RunLoop.current.run(until: Date().addingTimeInterval(0.02))
        probe.status = ourDevice(running: false)   // the world changes while the first probe is out
        for _ in 0..<4 { reader.read { later.append($0) } }
        RunLoop.current.run(until: Date().addingTimeInterval(0.5))
        expectEqual(first, ourDevice(running: true), "the first caller gets the first reading")
        expectEqual(later, Array(repeating: ourDevice(running: false), count: 4),
                    "everyone who asked meanwhile gets a reading that started after they asked")
        expectEqual(probe.reads, 2, "five requests cost two probes, not five")
        expectEqual(probe.maxConcurrentReads, 1, "and the two never overlapped")
        expectEqual(probe.mainThreadReads, 0, "neither ran on the main thread")
        expectEqual(reader.latest, ourDevice(running: false), "the newest reading is the one kept")
        expect(!reader.isReading, "nothing is left running")
    }

    /// A probe started earlier can answer AFTER a newer reading was applied
    /// (the in-place read at a replacement offer overtakes it). Its stale
    /// answer must not become the current state, and its caller is handed the
    /// newer reading: "running" from before the other app quit must not undo
    /// "free", nor the reverse.
    @MainActor
    static func testASupersededDeviceReadDoesNotOverwriteANewerOne() {
        let probe = SlowSpyCameraDeviceProbe(ourDevice(running: false), delay: 0.15)
        let reader = CameraDeviceReader(probe: probe)
        var asynchronous: CameraDeviceStatus?
        reader.read { asynchronous = $0 }           // looks now: free
        RunLoop.current.run(until: Date().addingTimeInterval(0.03))
        probe.status = ourDevice(running: true)     // Zoom opens the camera
        let inPlace = reader.readNow()              // the newer reading: in use
        expectEqual(inPlace, ourDevice(running: true), "the in-place read sees the camera in use")
        expect(asynchronous == nil, "precondition: the earlier probe has not been delivered yet")
        RunLoop.current.run(until: Date().addingTimeInterval(0.4))
        expectEqual(reader.latest, ourDevice(running: true), "the earlier probe's 'free' did not overwrite 'in use'")
        expectEqual(asynchronous, ourDevice(running: true), "and its caller got the newer reading, not the stale one")

        // The same through the model: the published reading and the in-use answer.
        let model = AppModel()
        let slow = SlowSpyCameraDeviceProbe(ourDevice(running: false), delay: 0.15)
        model.cameraDeviceProbe = slow
        var staleAnswer: Bool?
        model.checkCameraInUse { staleAnswer = $0 }
        RunLoop.current.run(until: Date().addingTimeInterval(0.03))
        slow.status = ourDevice(running: true)
        expect(model.cameraInUseNow(), "in use at the moment macOS offers the replacement")
        RunLoop.current.run(until: Date().addingTimeInterval(0.4))
        expectEqual(model.cameraDeviceStatus, ourDevice(running: true), "the model keeps the newer reading")
        expectEqual(staleAnswer, true, "and the earlier question is answered from it")
        model.stopTimersForTesting()
    }

    /// A reading that comes back after the state it was asked for has moved on
    /// decides nothing: the camera being removed while the registration check's
    /// probe is out must not be followed by that check's verdict.
    @MainActor
    static func testADeviceReadThatArrivesLateDecidesNothing() {
        let model = AppModel()
        let fake = FakeCameraExtensionActivator(script: [.activating, .active], installed: [ours(enabled: false)])
        let probe = SlowSpyCameraDeviceProbe(CameraDeviceStatus(), delay: 0.15)   // no device, slowly
        model.cameraExtension = fake
        model.cameraDeviceProbe = probe
        model.coreCommandCaptureForTesting = { _ in }
        model.cameraRegistrationTimeout = 0.05
        model.cameraRegistrationPollInterval = 0.02
        model.setVirtualCamera(enabled: true)       // activates; .active starts the registration check
        RunLoop.current.run(until: Date().addingTimeInterval(0.05))
        expectEqual(model.cameraExtensionState, .active, "precondition: activation completed")
        expect(model.cameraDeviceReader.isReading, "precondition: the registration check's probe is out")
        model.removeVirtualCamera()                 // the operator removes the camera meanwhile
        RunLoop.current.run(until: Date().addingTimeInterval(0.6))
        expectEqual(model.cameraExtensionState, .notInstalled,
                    "the late 'no device' reading does not turn a removed camera into Restart your Mac")
        expectEqual(fake.calls.filter { $0 == "installs" }.count, 0,
                    "and the abandoned check never asked what is installed")
        model.stopTimersForTesting()
    }
}
