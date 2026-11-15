# macOS Virtual Camera (M9.2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Program appears as the system camera "CoreVideo Pro Camera" in Zoom, Teams, Meet, FaceTime, OBS and Photo Booth on macOS. It has Windows' controls (enable, OH Framer, mirror, rename, status) plus the Mac approval flow.

**Architecture:** A sandboxed CoreMediaIO Camera Extension (Swift, built by `swift build`, no Xcode project) is embedded in the app. It serves a source stream to consumers and accepts frames on a sink stream; when no fresh frame is available it serves the standby slate. The C++ core feeds the sink through a new `SinkVirtualCameraPublisher` behind the existing `IVirtualCameraPublisher` / `AsyncVirtualCameraPublisher`. Its frames come from a new Metal-compositor NV12 tap. The SwiftUI shell activates the extension with `OSSystemExtensionRequest`, sends the existing `sync-virtual-camera` command, and reads the existing `virtualCamera` snapshot.

**Tech Stack:** Swift 5.9 tools / SwiftUI (macOS 13), CoreMediaIO (`CMIOExtension*` and the C HAL API), SystemExtensions, CoreVideo/CoreMedia/IOSurface, C++17 + ObjC++ (core), Metal, gtest, the in-binary shell harness (`COREVIDEO_SHELL_TESTS=1`), bash packaging scripts.

**Spec:** `docs/virtual-camera-macos-spec.md` (Mac addendum). It argues from `docs/virtual-camera-spec.md` (Windows). Read both first.

## Global Constraints

- Branch `mac-virtual-camera` in `.claude/worktrees/mac-virtual-camera`. Commit per task; never push; never touch `main`. End commit messages with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Windows behaviour is bit-identical.** Gate shared `native/` edits on `__APPLE__` / `COREVIDEO_WITH_MAC_VIRTUALCAM`. The one intended shared change is Task 4's retry-after-publication rule in `AsyncVirtualCameraPublisher`, and it is inert for the Windows backend because that backend never fails during publish. A Windows test must prove this.
- macOS floor **13.0**, **arm64 only**. App bundle id `us.iamfatness.corevideopro.shell`. Extension bundle id `us.iamfatness.corevideopro.shell.camera`. Team `42394L3353`.
- Extension path in the app: `Contents/Library/SystemExtensions/us.iamfatness.corevideopro.shell.camera.systemextension`. Mach service and app group: `42394L3353.us.iamfatness.corevideopro.shell.camera`.
- Signing identity `Developer ID Application: John Wallace (42394L3353)`. Installer identity `Developer ID Installer: John Wallace (42394L3353)`. Notary profile `CoreVideo Pro`.
- Profiles are **never committed**. Env `COREVIDEO_APP_PROFILE` defaults to `~/Library/Developer/CoreVideoPro/profiles/CoreVideo_Pro_macOS_Developer_ID.provisionprofile`. Env `COREVIDEO_CAMERA_PROFILE` defaults to `~/Library/Developer/CoreVideoPro/profiles/CoreVideo_Pro_Camera_Extension_Developer_ID.provisionprofile`. The profiles expire 2044-11-10. Their certificate expires **2027-02-01**.
- Core signing identifier `us.iamfatness.corevideopro.core` (already set by `make-macos-shell-bundle.sh`). Probe identifier `us.iamfatness.corevideopro.vcam-probe`.
- Fixed IDs:
  - Device UUID `791E98ED-72E7-4716-9A26-1B4FBB9A1520`.
  - Source stream UUID `88409CE1-9E7E-4E22-817E-7E039C84C1EF`.
  - Sink stream UUID `454F962A-782B-45DB-A9E6-BE09984FBBA7`.
  - Legacy device ID `us.iamfatness.corevideopro.camera.device`.
  - Device and source stream name `CoreVideo Pro Camera`; sink stream name `CoreVideo Pro Camera Sink`.
- Sink format 1920×1080 NV12 `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange`, BT.601 matrix attachment. Hold window **500 ms**. Retry cadence **1 s** (existing).
- Copy that matches Windows is verbatim (StudioWorkspace.xaml): "OH Framer (webcam only)", "Framing guides over live video. Program, Preview, recordings and streams stay clean.", "Mirror (self-view)", "Flips the camera horizontally, like most webcam self-views.", "Camera name", placeholder "CoreVideo Pro Camera", "The name apps show in their camera list. Takes effect next time you enable the camera.", "Virtual camera off", "Virtual camera live (WxH)", "Virtual camera on".
- Builds on this Mac: native per `2026-11-14-mac-parity-m0-fixes.md` Global Constraints, plus `-DCOREVIDEO_WITH_MAC_VIRTUALCAM=ON` once Task 4 lands. Shell: `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 .build/release/CoreVideoProShell`. `bash scripts/lint-mac-design.sh` must stay green. Attach a `COREVIDEO_SHELL_SNAPSHOT` PNG for visible changes.
- CI realities: macOS runners have no window server and cannot activate system extensions. Metal tests self-skip without a device. Everything CI runs goes through fakes. Activation and real CMIO run only in the local e2e script and the owner checklist.
- Rules from `docs/superpowers/plan-authoring-rules.md` are binding. Every task reports the named mutations and whether each went red.
- Backlog: M9.2 needs one GitHub issue labelled `backlog` (roadmap §6). Task 1 Step 0 checks for it and creates it if absent; reference it in every commit body.

## Review Focus

1. **The extension restarts, crashes, or is replaced by an app update while the core is feeding.** The camera must come back on its own within ~2 s, with no toggle. Expected: the snapshot goes `failed` + `CAMERA_EXTENSION_LOST:` and then back to `live`. Owner: Task 4 `ALostSinkReconnectsWithoutAToggle` and `AsyncVirtualCameraPublisher.PublicationLossSchedulesTheRegistrationRetry`.
2. **The approval is never granted, or the app runs from `~/Downloads` or a mounted DMG.** The flyout must say exactly what to do and must never read "live". Owner: Task 3 `testCameraExtensionErrorMapping` and Task 6 `testVirtualCameraLabelPrecedence`.
3. **The Mac sleeps or wakes, the app quits, or the core crashes mid-call.** The consumer sees the slate within 500 ms and never a frozen Program frame. Owner: Task 1 `testFrameSelectorHoldAndStop` and the Task 7 e2e step "stop feed → slate ≤ 1 s".
4. **Ad-hoc and CI builds.** A restricted entitlement without a profile gets the binary killed by AMFI at launch, and a mismatched profile fails activation silently. Owner: Task 2 check-script negative tests.
5. **Keychain items survive the new `application-identifier` entitlement.** The stream key and Zoom tokens must stay readable after the shell is signed with it. Owner: Task 2 Step 7 (owner check on the signed dev bundle) and Task 12 acceptance.

---

### Task 1: `CameraExtensionKit` — the pure rules both sides share

**Files:**
- Modify: `mac-shell/Package.swift` (add library target `CameraExtensionKit`; `CoreVideoProShell` depends on it)
- Create: `mac-shell/Sources/CameraExtensionKit/CameraConstants.swift`, `StandbySlate.swift`, `FrameSelector.swift`, `DeviceNamePolicy.swift`, `SlateClassifier.swift`
- Test: `mac-shell/Sources/CoreVideoProShell/ShellTests.swift` (new cases; register them in `runAndExit`)

**Interfaces:**
- Produces (all `public`):
  - `enum CameraConstants`: `deviceUUID`, `sourceStreamUUID`, `sinkStreamUUID` (`UUID`); `legacyDeviceID`, `defaultDeviceName`, `sinkStreamName`, `machServiceName`, `extensionBundleID`, `producerRequirement` (`String`); `holdNanoseconds: UInt64 = 500_000_000`; `sinkWidth = 1920`, `sinkHeight = 1080`. Values are copied from Global Constraints. `producerRequirement` is spec §4 law 6 verbatim.
  - `struct CameraFormat: Equatable { width: Int; height: Int; fps: [Int] }` and `enum CameraFormats { static let source: [CameraFormat] /* [1920x1080 [60,30]] — Task 10 appends 720p */ }`
  - `enum StandbySlate { static func nv12(width: Int, height: Int) -> [UInt8]? }` must be byte-identical to C++ `fillVirtualCameraStandbySlate`: Y 24, a bar of Y 130 with height `h/12` (minimum 1) starting at `(h - barH)/2`, UV 128. Returns nil for odd or non-positive dimensions.
  - `enum FrameChoice: Equatable { case program, slate }`
  - `struct FrameSelector { mutating func producerDelivered(atNanoseconds: UInt64); mutating func producerStopped(); func choice(atNanoseconds: UInt64) -> FrameChoice }`
  - `enum DeviceNamePolicy { static func sanitized(_ raw: String) -> String }` trims whitespace and newlines, drops control characters, and truncates to 64 Unicode scalars. Empty input gives `defaultDeviceName`.
  - `enum SlateClassifier { static func isSlate(yPlane: UnsafeBufferPointer<UInt8>, width: Int, height: Int, bytesPerRow: Int) -> Bool }` samples rows `h/4`, `h/2` and `3h/4` at columns `w/4`, `w/2` and `3w/4`. It is true only when the samples on rows `h/4` and `3h/4` are within ±2 of 24 and those on row `h/2` are within ±2 of 130.

- [ ] **Step 0:** `gh issue list --label backlog --search "M9.2"`. If no issue exists, create "M9.2 macOS virtual camera (CoreMediaIO extension)" with label `backlog` and a link to this plan.
- [ ] **Step 1: Write the failing tests** in ShellTests:

```swift
private static func testStandbySlateMatchesTheCoreSlate() {
    // Fixture invariant: 1080/12 = 90, top = (1080-90)/2 = 495, bottom = 585.
    let s = StandbySlate.nv12(width: 1920, height: 1080)!
    expectEqual(s.count, 3_110_400, "NV12 size")
    expectEqual(s[494 * 1920], 24, "row above bar is field")
    expectEqual(s[495 * 1920], 130, "first bar row")
    expectEqual(s[584 * 1920 + 1919], 130, "last bar row, last column")
    expectEqual(s[585 * 1920], 24, "row below bar is field")
    expect(s[(1920 * 1080)...].allSatisfy { $0 == 128 }, "neutral chroma")
    expect(StandbySlate.nv12(width: 1919, height: 1080) == nil, "odd width refused")
}
private static func testFrameSelectorHoldAndStop() {
    var f = FrameSelector()
    expectEqual(f.choice(atNanoseconds: 10), .slate, "never delivered -> slate")
    f.producerDelivered(atNanoseconds: 1_000_000_000)
    expectEqual(f.choice(atNanoseconds: 1_500_000_000), .program, "exactly 500 ms -> still program")
    expectEqual(f.choice(atNanoseconds: 1_500_000_001), .slate, "500 ms + 1 ns -> slate")
    expectEqual(f.choice(atNanoseconds: 999_999_999), .slate, "now before delivery -> slate (fail toward law 2)")
    f.producerDelivered(atNanoseconds: 2_000_000_000); f.producerStopped()
    expectEqual(f.choice(atNanoseconds: 2_000_000_001), .slate, "stop -> slate immediately, no hold")
    f.producerDelivered(atNanoseconds: 3_000_000_000)
    expectEqual(f.choice(atNanoseconds: 3_000_000_001), .program, "delivery after stop -> program")
}
private static func testDeviceNamePolicy() {
    expectEqual(DeviceNamePolicy.sanitized("  Studio A \n"), "Studio A", "trimmed")
    expectEqual(DeviceNamePolicy.sanitized("   "), "CoreVideo Pro Camera", "blank -> default")
    expectEqual(DeviceNamePolicy.sanitized("A\u{0007}B"), "AB", "control chars dropped")
    expectEqual(DeviceNamePolicy.sanitized(String(repeating: "é", count: 80)).unicodeScalars.count, 64, "capped at 64 scalars")
}
private static func testSlateClassifier() {
    // Three fixtures with independently known answers.
    let slate = StandbySlate.nv12(width: 1280, height: 720)!            // -> true
    let red = [UInt8](repeating: 82, count: 1280 * 720)                 // BT.601 red Y -> false
    let flat24 = [UInt8](repeating: 24, count: 1280 * 720)              // dark, no bar -> false
    // call isSlate on each Y plane with bytesPerRow 1280; expect true, false, false
}
```

- [ ] **Step 2:** Run `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 .build/release/CoreVideoProShell`. Expected: the build fails (the module is missing).
- [ ] **Step 3:** Implement the five files and the `Package.swift` change (`.target(name: "CameraExtensionKit", path: "Sources/CameraExtensionKit")`).
- [ ] **Step 4:** Run the same command. Expected: all pass, and the harness prints the new case names.
- [ ] **Step 5: Mutations** (report each one; each must go red):
  - `>` → `>=` in the hold comparison. The "exactly 500 ms" case must fail.
  - Remove the immediate slate in `producerStopped`.
  - Change the bar top to `h/2 - barH`. Row 495 must fail.
  - Check only the middle row in `isSlate`. The `flat24` case is confirmatory for this mutation only if it goes red; if it stays green, add a bar-less fixture with Y 130 in the middle band.
- [ ] **Step 6: Commit** `mac: CameraExtensionKit — slate, hold policy, name policy shared by shell and camera extension`.

### Task 2: The camera extension executable, its bundle and its signing

**Files:**
- Modify: `mac-shell/Package.swift` (executable target `CoreVideoProCamera`, depends on `CameraExtensionKit`; links `CoreMediaIO`, `CoreMedia`, `CoreVideo`, `IOSurface`, `Security`, `VideoToolbox`)
- Create: `mac-shell/Sources/CoreVideoProCamera/main.swift`, `CameraProvider.swift`, `CameraDevice.swift`, `SourceStream.swift`, `SinkStream.swift`, `ProducerAuthorizer.swift`
- Create: `scripts/macos-camera-extension.entitlements`, `scripts/macos-camera-extension-adhoc.entitlements`, `scripts/macos-shell-sysext.entitlements`, `scripts/check-macos-camera-extension.sh`
- Modify: `scripts/make-macos-shell-bundle.sh`, `scripts/macos-sign-lib.sh` (add `verify_profile`), `.github/workflows/ci.yml` (`mac-shell-tests` and the syntax-check list in `mac-shell-design-lint`)

**Interfaces:**
- Consumes: `CameraConstants`, `CameraFormats`, `StandbySlate`, `FrameSelector` (Task 1).
- Produces:
  - The bundle layout above, with extension `CFBundleVersion` equal to the app's `BUILD_NUMBER`.
  - `scripts/check-macos-camera-extension.sh [--release] <app>`: exit 0 means pass, non-zero means fail, and the failing check is printed.
  - `verify_profile <profile> <expected-app-id> <min-days>` in `macos-sign-lib.sh`.
  - Env `COREVIDEO_CAMERA_EXT_BIN` (default `mac-shell/.build/release/CoreVideoProCamera`), `COREVIDEO_APP_PROFILE`, `COREVIDEO_CAMERA_PROFILE`.

Extension behaviour (prose; the implementer writes the bodies):
- `main.swift`: `CMIOExtensionProvider.startService(provider: CameraProvider().provider)`, then `CFRunLoopRun()`.
- Provider. The device uses `deviceID: CameraConstants.deviceUUID`, `legacyDeviceID: CameraConstants.legacyDeviceID`, and the name from persisted defaults (Task 8) or the default. Its `deviceModel` is "CoreVideo Pro Camera" and its transport type is virtual (`kIOAudioDeviceTransportTypeVirtual`).
- Streams are added source first, then sink.
- Sink stream properties: `sinkBufferQueueSize` 4, `sinkBuffersRequiredForStartup` 1.
- `authorizedToStartStream(for:)` on the sink calls `ProducerAuthorizer.isAuthorized(pid:)`. That builds the guest code from the pid (`SecCodeCopyGuestWithAttributes`) and checks it against `CameraConstants.producerRequirement`. It fails closed and logs both outcomes. The source stream authorizes everyone.
- Sink `startStream` arms a consume timer at 8 ms on a private queue. Each tick drains `consumeSampleBuffer(from:)` while `hasMoreSampleBuffers` is true and keeps only the newest buffer. For each buffer it calls `notifyScheduledOutputChanged` and `FrameSelector.producerDelivered`. Sink `stopStream` calls `producerStopped`.
- Source `startStream` arms a timer at the active frame duration. Each tick asks `FrameSelector.choice(now)` and sends either the latest buffer, retimed with `CMSampleBufferCreateCopyWithNewTiming`, or a prebuilt slate `CVPixelBuffer` built from `StandbySlate.nv12`. The host time is `mach_absolute_time` converted to nanoseconds.
- Logging: `os_log` subsystem `us.iamfatness.corevideopro.camera`, categories `source`, `sink` and `auth`. Log each stream start/stop and each slate↔program transition, once per transition.

Bundle (prose):
- Info.plist via heredoc, keys exactly:
  - `CFBundleExecutable`=`CoreVideoProCamera`, `CFBundleIdentifier`=extension id, `CFBundleName`=`CoreVideo Pro Camera`, `CFBundlePackageType`=`SYSX`.
  - `CFBundleShortVersionString`=`$VERSION`, `CFBundleVersion`=`$BUILD_NUMBER`, `LSMinimumSystemVersion`=`13.0`.
  - `CMIOExtension`={`CMIOExtensionMachServiceName`=`42394L3353.us.iamfatness.corevideopro.shell.camera`}.
  - `NSSystemExtensionUsageDescription`=`Lets CoreVideo Pro offer its Program output as a camera in Zoom, Teams, FaceTime and other apps.`
- Entitlements:
  - `macos-camera-extension.entitlements`: `com.apple.security.app-sandbox` true, `com.apple.security.application-groups` [the mach service name], `com.apple.application-identifier` `42394L3353.us.iamfatness.corevideopro.shell.camera`, `com.apple.developer.team-identifier` `42394L3353`.
  - The `-adhoc` file has the sandbox and the app group only.
  - `macos-shell-sysext.entitlements` = the current shell entitlements + `com.apple.developer.system-extension.install` true + `com.apple.application-identifier` `42394L3353.us.iamfatness.corevideopro.shell` + team identifier.
- Signing rules:
  - Ad-hoc: embed the extension, use the `-adhoc` entitlements, no profiles, and keep the shell on `macos-shell.entitlements`.
  - Developer ID: **both profiles are required, and a missing one is fatal** with a message naming the env vars. Run `verify_profile` (app id match, `TeamIdentifier` = 42394L3353, profile not expired, and the signing certificate's SHA-1 among the profile's `DeveloperCertificates`) with min-days 0 for dev. Copy each profile to `embedded.provisionprofile` in its bundle.
  - Order: extension first (`sign_one "$EXT" --entitlements …`), then the existing helpers, then the app with `macos-shell-sysext.entitlements`.

- [ ] **Step 1: Write the failing check.** `check-macos-camera-extension.sh` asserts:
  - (a) the extension directory exists at the exact path;
  - (b) the Info.plist keys and values above;
  - (c) extension `CFBundleVersion` equals the app's;
  - (d) the mach service name starts with one of the extension's `application-groups` entitlement values (read through `codesign -d --entitlements :-`);
  - (e) the extension entitlements include the sandbox;
  - (f) in `--release` mode: Developer ID authority and the runtime flag on the extension, `embedded.provisionprofile` in both bundles passing `verify_profile … 14`, and the app's signed entitlements containing `system-extension.install`;
  - (g) no bundle's signed entitlements contain `com.apple.application-identifier` unless that bundle has an `embedded.provisionprofile`. This is the AMFI rule from Review Focus 4.
- [ ] **Step 2:** Run `scripts/check-macos-camera-extension.sh "mac-shell/.build/CoreVideo Pro.app"` on today's bundle. Expected: FAIL at (a).
- [ ] **Step 3:** Implement the extension sources, the entitlements files, the bundle-script changes and `verify_profile`.
- [ ] **Step 4:** Run `swift build -c release`, then `COREVIDEO_REQUIRE_HELPERS=0 scripts/make-macos-shell-bundle.sh`, then the check script. Expected: PASS (ad-hoc mode).
- [ ] **Step 5: Negative self-tests** (rule 10: drive the structure). Each of the following must make the check exit non-zero:
  - Copy the bundle and run `plutil -replace CFBundleVersion -string 0` on the extension Info.plist.
  - Copy the bundle and re-sign the extension ad-hoc with `macos-camera-extension.entitlements`. This trips (g).
  - Copy the bundle and remove `CMIOExtension`.

  Add all three to CI as one step after the build. Add `check-macos-camera-extension.sh` to the `bash -n` list.
- [ ] **Step 6: CI.** Add steps to `mac-shell-tests`: assemble the ad-hoc bundle, run the check, run the negatives.
- [ ] **Step 7: Owner check (local, signed).**
  1. Run `COREVIDEO_SIGN_IDENTITY="Developer ID Application: John Wallace (42394L3353)" scripts/make-macos-shell-bundle.sh`, then `check-macos-camera-extension.sh --release`. Expected: PASS.
  2. Launch the signed bundle. It must not crash (an AMFI kill shows in `log show --last 2m --predicate 'process == "amfid"'`).
  3. The saved stream key and Zoom sign-in must still be present (Review Focus 5). Record the result in the task report.
- [ ] **Step 8: Mutations:**
  - Drop the `-adhoc` split (always use the restricted file). The negative test from Step 5 must catch it.
  - Remove the profile copy for the extension. `--release` (f) must fail.
  - Sign the app before the extension. `codesign --verify --deep --strict` must fail.
- [ ] **Step 9: Commit** `mac: CoreMediaIO camera extension (slate + sink) embedded and signed in the app bundle`.

### Task 3: Shell activation — `OSSystemExtensionRequest` behind a protocol

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/CameraExtensionActivation.swift`
- Modify: `mac-shell/Sources/CoreVideoProShell/ShellPrefs.swift` (optional field `cameraExtensionOptedIn: Bool?`, decoded with `decodeIfPresent` per the file's rule)
- Test: `ShellTests.swift`

**Interfaces:**
- Produces:

```swift
enum CameraExtensionState: Equatable {
    case unknown, notInstalled, activating, needsApproval, active, needsReboot, notInApplications
    case unavailable(String), failed(String)
}
enum CameraExtensionReplacement: Equatable { case replace, keep }
enum CameraExtensionPolicy {
    static func preflight(bundlePath: String, hasEmbeddedProfile: Bool) -> CameraExtensionState?
    static func state(forActivationError domain: String, code: Int, canceledByUs: Bool) -> CameraExtensionState
    static func replacement(existingVersion: String, newVersion: String) -> CameraExtensionReplacement
    static func state(forProperties: [(enabled: Bool, awaitingApproval: Bool, uninstalling: Bool)]) -> CameraExtensionState
    static func settingsURL(osMajor: Int) -> URL
    static func settingsInstruction(osMajor: Int) -> String
}
protocol CameraExtensionActivating: AnyObject {
    func query(_ done: @escaping (CameraExtensionState) -> Void)
    func activate(_ update: @escaping (CameraExtensionState) -> Void)
    func deactivate(_ update: @escaping (CameraExtensionState) -> Void)
}
final class SystemCameraExtensionActivator: NSObject, CameraExtensionActivating, OSSystemExtensionRequestDelegate
final class FakeCameraExtensionActivator: CameraExtensionActivating   // scripted states, records calls
```

- Rules:
  - `preflight` returns `.notInApplications` unless the path has the prefix `/Applications/`. It returns `.unavailable("This build can't install the virtual camera (development build without a provisioning profile).")` when there is no profile. Otherwise it returns nil.
  - Error mapping (domain `OSSystemExtensionErrorDomain`):
    - 3 → `.notInApplications`
    - 2 and 8 → `.unavailable("This copy of CoreVideo Pro isn't signed to install the camera.")`
    - 10 → `.unavailable("macOS policy blocked the camera extension.")`
    - 11 with `canceledByUs` → `.active`
    - 13 → `.needsApproval`
    - anything else → `.failed("Couldn't install the virtual camera (code N).")`
  - `replacement` returns `.keep` for equal versions (the delegate answers `.cancel`, and the following `requestCanceled` maps to `.active`). Otherwise it returns `.replace`.
  - Properties: an empty list → `.notInstalled`. Any entry enabled and not uninstalling → `.active`. Else any entry awaiting approval → `.needsApproval`. Else `.notInstalled`.
  - Settings: macOS ≥15 → `x-apple.systempreferences:com.apple.LoginItems-Settings.extension` and "System Settings › General › Login Items & Extensions › Camera Extensions". macOS 13–14 → `x-apple.systempreferences:com.apple.preference.security?Security` and "System Settings › Privacy & Security, then click Allow next to the CoreVideo Pro message".
  - The delegate's `requestNeedsUserApproval` emits `.needsApproval`. `.completed` emits `.active`. `.willCompleteAfterReboot` emits `.needsReboot`.
  - Every state transition is written to `ShellLog` as `vcam-ext: <from> -> <to>`.

- [ ] **Step 1: Write the failing tests:**
  - `testCameraExtensionErrorMapping`: one assertion per code in the table, plus code 11 without `canceledByUs` → `.failed`.
  - `testCameraExtensionPreflight`: `/Users/x/Downloads/CoreVideo Pro.app` → `.notInApplications`; `/Applications/CoreVideo Pro.app` with no profile → `.unavailable`; `/Applications/…` with a profile → nil; `/ApplicationsX/…` → `.notInApplications` (a fixture that kills a sloppy `contains`).
  - `testCameraExtensionReplacement`: ("100","100") → keep; ("100","101") → replace; ("101","100") → replace (downgrade on an older install).
  - `testCameraExtensionProperties`: four rows from the rule.
  - `testCameraExtensionSettingsLink`: 13, 14 → the security URL; 15, 26 → the login-items URL.
- [ ] **Step 2:** Run the harness. Expected: compile failure.
- [ ] **Step 3:** Implement. `SystemCameraExtensionActivator.activate` runs `preflight` first and never submits a request when preflight returns a state. `query` uses `propertiesRequest(forExtensionWithIdentifier:queue:)`.
- [ ] **Step 4:** Run the harness. Expected: pass.
- [ ] **Step 5: Owner spike** (local, signed bundle copied to `/Applications`). This step is the gate for Tasks 4–7. Add a temporary Debug menu item "Activate camera extension" (removed in Task 6) and record in the task report:
  - (a) the `systemextensionsctl list` line;
  - (b) the approval pane location on macOS 26.3, and whether the macOS 15+ URL opened it;
  - (c) whether activation needed a notarized build (if a non-notarized Developer ID build fails with error 9, rebuild with `--notarize "CoreVideo Pro"` and record that);
  - (d) the user the extension process runs as (`ps -o user= -p $(pgrep CoreVideoProCamera)`);
  - (e) Photo Booth shows the slate under "CoreVideo Pro Camera";
  - (f) `system_profiler SPCameraDataType` Unique ID, which tells whether the HAL UID is the legacy ID or the UUID string.

  If (d) shows the logged-in user, write that in the report: spec §6's reason changes, though its design still holds.
- [ ] **Step 6: Mutations:**
  - Map 13 to `.failed`. The test must go red.
  - Use `contains("/Applications")` in preflight. The `ApplicationsX` and Downloads fixtures must go red.
  - Always return `.replace`. Equal versions must go red.
- [ ] **Step 7: Commit** `mac: activate the camera extension via OSSystemExtensionRequest (testable policy + fake)`.

### Task 4: Core Mac backend — `SinkVirtualCameraPublisher` over `IVirtualCameraSink`

**Files:**
- Create: `native/src/modules/VirtualCameraSink.h`, `native/src/modules/SinkVirtualCameraPublisher.{h,cpp}` (portable C++), `native/src/modules/CmioVirtualCameraSink.{h,mm}` (Apple only)
- Modify: `native/src/modules/VirtualCameraPublisher.cpp` (factory branch), `native/src/modules/AsyncVirtualCameraPublisher.cpp` (retry after publication loss), `native/CMakeLists.txt` (option `COREVIDEO_WITH_MAC_VIRTUALCAM`, Apple + dev adapters only, links `CoreMediaIO CoreMedia CoreVideo IOSurface`; `.mm` compiled with `-fobjc-arc`), `.github/workflows/ci.yml` (`native-metal-macos` configure adds `-DCOREVIDEO_WITH_MAC_VIRTUALCAM=ON`), `scripts/release-macos.sh` (core configure adds the same)
- Test: `native/tests/SinkVirtualCameraPublisherTest.cpp` (registered in `corevideo-native-tests` for every config, because it is portable), `native/tests/AsyncVirtualCameraPublisherTest.cpp`

**Interfaces:**
- Consumes: `IVirtualCameraPublisher`, `VirtualCameraStatus`, `mirrorNv12InPlace`, `nv12FrameSize`, `resizeBgraBilinear`, `convertBgraToNv12` (existing).
- Produces:

```cpp
namespace corevideo::modules {
enum class SinkEnqueueResult { Ok, QueueFull, Lost };
class IVirtualCameraSink {
 public:
  virtual ~IVirtualCameraSink() = default;
  // "" on success; otherwise a warning beginning "CAMERA_EXTENSION_NOT_FOUND:" or "CAMERA_SINK_FAILED:".
  virtual std::string connect() = 0;
  // Copies the bytes; never retains `nv12`. Must not block.
  virtual SinkEnqueueResult enqueueNv12(const uint8_t* nv12, int width, int height, int64_t hostTimeNs) = 0;
  // Task 8. Returns true when the extension accepted a name that differs from the current one.
  virtual bool setDeviceName(const std::string& name) { (void)name; return false; }
  virtual void disconnect() = 0;
};
class SinkVirtualCameraPublisher final : public IVirtualCameraPublisher {
 public:
  SinkVirtualCameraPublisher(std::unique_ptr<IVirtualCameraSink> sink, std::function<int64_t()> hostClockNs);
  // start/stop/publish/publishNv12/publishNv12IdentifiedOnWorker/status/setMirror/setDeviceName overrides
};
std::unique_ptr<IVirtualCameraSink> createCmioVirtualCameraSink();   // Apple + option only
}
```

- Behaviour:
  - `start(w,h,fps)` records the size and calls `connect()`. On a non-empty result: `state="failed"`, `enabled=false`, `warning=<result>`, returns false. On success: `state="live"`, `enabled=true`, warning cleared, and a `[virtualcam] mac sink connected` log line.
  - A publish with a size other than the declared one is a no-op (Windows parity).
  - Mirror writes into a scratch buffer, never into the caller's buffer.
  - `Ok` → `++framesPublished`. `QueueFull` → the frame is dropped, logged at most once per 300 drops. `Lost` → `disconnect()`, `state="failed"`, `enabled=false`, `warning="CAMERA_EXTENSION_LOST: CoreVideo Pro Camera restarted; reconnecting."`
  - `publish(ProgramFrame)` resizes to the declared size and converts (the Windows `publish` recipe), then takes the same enqueue path.
  - `stop()` disconnects and sets `state="off"`.
  - All members are guarded by one mutex. Pixel work happens under it, as on Windows. Callers are only the publisher worker and the command thread.
- `AsyncVirtualCameraPublisher` change: after `observed = backend_->status()`, if `on && !observed.enabled && observed.state == "failed" && observed.warning.rfind("CAMERA_", 0) == 0 && retryAt == max`, set `retryAt = now + 1s`.
- Factory: `#elif COREVIDEO_WITH_MAC_VIRTUALCAM` returns `make_unique<AsyncVirtualCameraPublisher>(make_unique<SinkVirtualCameraPublisher>(createCmioVirtualCameraSink(), machHostClockNs))`.
- `CmioVirtualCameraSink`:
  - `connect`: set `kCMIOHardwarePropertyRunLoop` to NULL once. Enumerate `kCMIOHardwarePropertyDevices` and match `kCMIODevicePropertyDeviceUID` ∈ {legacy ID, UUID string}. Pick the stream whose `kCMIOObjectPropertyName` == `CoreVideo Pro Camera Sink`. Call `CMIOStreamCopyBufferQueue(stream, proc, this, &queue)` and then `CMIODeviceStartStream`. Build a `CVPixelBufferPool` (1920×1080 '420v', IOSurface-backed, minimum 6 buffers) and a cached format description. Add a device-list listener block that sets an atomic `devicesChanged_`.
  - `enqueueNv12`: if `devicesChanged_` is set and the UID is gone → `Lost`. If the queue count ≥ capacity → `QueueFull`. Otherwise create a pool buffer, copy the Y and UV planes row by row with `CVPixelBufferGetBytesPerRowOfPlane`, attach `kCVImageBufferYCbCrMatrix_ITU_R_601_4`, call `CMSampleBufferCreateReadyWithImageBuffer` (pts = hostTimeNs/1e9, duration = 1/60), then `CMSimpleQueueEnqueue`. The queue takes the +1 reference (the OBS pattern); the Task 7 `leaks` run checks this. An enqueue error → `Lost`.
  - `disconnect`: `CMIODeviceStopStream`, remove the listener, release the queue and pool.

- [ ] **Step 1: Write the failing tests** with a `FakeSink` that honours the contract (rule 9). It copies on enqueue, returns `QueueFull` once a scripted capacity is reached and stops counting, returns scripted `connect()` results in order, and records `disconnect()` calls.

```cpp
TEST(SinkVirtualCameraPublisher, ConnectFailureIsACameraRetryableFailure)
  // connect() -> "CAMERA_EXTENSION_NOT_FOUND: x": start returns false; status {failed, enabled=false, warning starts "CAMERA_"}
TEST(SinkVirtualCameraPublisher, PublishesOnlyTheDeclaredSizeAndCountsAcceptedFrames)
  // start(1920,1080,60); 1280x720 frame -> fake saw 0; 1920x1080 frame -> fake saw 1, framesPublished == 1
TEST(SinkVirtualCameraPublisher, MirrorFlipsTheEnqueuedCopyNotTheCallersBuffer)
  // Fixture: Y row ramp 0..255 across width (row 0 col 0 = 0, last col = 255): fake's Y[0] == 255, caller buffer Y[0] still 0
TEST(SinkVirtualCameraPublisher, AFullQueueDropsTheNewFrameWithoutFailing)
  // capacity 2, publish 5: fake holds 2, framesPublished == 2, status live
TEST(SinkVirtualCameraPublisher, ALostSinkReconnectsWithoutAToggle)
  // enqueue -> Lost: status {failed, enabled=false, warning starts "CAMERA_EXTENSION_LOST:"}, disconnect called once;
  // start() again with connect() -> "" -> live, next publish accepted
TEST(AsyncVirtualCameraPublisher, PublicationLossSchedulesTheRegistrationRetry)
  // backend whose publish flips status to failed/CAMERA_EXTENSION_LOST; within 2.5 s start() was called a 2nd time without stop()/start() from the caller
TEST(AsyncVirtualCameraPublisher, NonCameraPublicationFailureDoesNotRetry)
  // warning "Virtual camera operation failed." -> no second start within 2.5 s
```

- [ ] **Step 2:** Run `native/build-metal/corevideo-native-tests --gtest_filter='SinkVirtualCameraPublisher.*:AsyncVirtualCameraPublisher.*'`. Expected: build error, then FAIL.
- [ ] **Step 3:** Implement the portable publisher, the Async change, the CMake option and the factory, and `CmioVirtualCameraSink.mm` (compile-checked only on CI).
- [ ] **Step 4:** Run the filter on `build-metal` (option ON) and on the stub config. Expected: PASS on both. Run the full `ctest --test-dir native/build-metal -j 6`. Expected: PASS.
- [ ] **Step 5: Windows inertness.** On the Windows box (or noted for the owner if no Windows build is available), run `corevideo-native-tests.exe --gtest_filter=AsyncVirtualCameraPublisher.*`. Expected: all existing cases pass unchanged.
- [ ] **Step 6: Mutations:**
  - Delete the Async retry block. `PublicationLossSchedules…` must go red.
  - Drop the `"CAMERA_"` prefix check. `NonCameraPublicationFailure…` must go red.
  - Mirror in place on the caller's buffer. The caller-buffer assertion must go red.
  - Treat `QueueFull` as `Lost`. The full-queue test must go red.
- [ ] **Step 7: Commit** `native: macOS virtual camera backend feeds the CMIO sink stream (reconnects on loss)`.

### Task 5: Metal compositor NV12 tap for the virtual camera

**Files:**
- Modify: `native/src/compositor/MetalCompositorAdapter.mm`
- Test: `native/tests/MetalCompositorTest.cpp`

**Interfaces:**
- Consumes: `ICompositor::setVcamFrameSink` / `setIdentifiedVcamFrameSink` / `publishesVcamFrames` (Interfaces.h:1515-1528); `resizeBgraBilinear`, `convertBgraToNv12`.
- Produces:
  - Under `COREVIDEO_WITH_MAC_VIRTUALCAM`, `publishesVcamFrames()` returns true and `setIdentifiedVcamFrameSink` is overridden. The sink is called on the tap thread with an immutable `shared_ptr` NV12 buffer of exactly 1920×1080 (3,110,400 bytes), `programSequence` = the frame number recorded at kick time, and `deliveredAt100ns` = 0.
  - Setting or clearing the sink waits for any in-flight call (a sink mutex held around the call).
- Behaviour:
  - When `renderPlan.fullProgramReadback` is set and a sink is present, kick the tap blit on **every** tick, light ticks included. A light tick keeps its no-CPU-wait, no-preview-readback rule.
  - A kick is skipped while the tap thread is still reading `tapTex_` (atomic `tapReadInFlight_`). That keeps a blit from overwriting a texture mid-readback (no tearing); the newest frame wins.
  - The tap thread converts after readback, before the existing `tapPublished_` swap: resize to 1920×1080 when the canvas differs, then convert.
  - Log `[virtualcam] mac tap convert mean X ms max Y ms (300 frames)` once per 300 frames via `nativeVerboseLogf`.

- [ ] **Step 1: Write the failing tests** (use the existing `MAKE_COMPOSITOR_OR_SKIP`, `solidLayer` and `bgraFrame` helpers):

```cpp
TEST(MetalCompositor, VcamTapDeliversBt601Nv12OfTheProgram)
  // full-canvas bgraFrame("cam",320,180, b=0,g=0,r=255); plan.fullProgramReadback=true; sink set; render until sink fires (<=2 s)
  // Expected (hand-computed from the BT.601 integer formula in VirtualCameraFrame.h, independent of the code under test):
  // size == 3110400; Y[540*1920+960] == 82 ±2; U at center == 91 ±2; V at center == 240 ±2
TEST(MetalCompositor, VcamTapRunsOnLightTicksWithoutCpuReadback)
  // plan.skipCpuReadback=true, fullProgramReadback=true, sink set: sink fires within 2 s AND frame.preview.bgra.empty()
TEST(MetalCompositor, VcamTapScalesANon1080pCanvasTo1080p)
  // compositor canvas 3840x2160 (or 1280x720 if the adapter cannot allocate 4K on the runner): sink width/height == 1920/1080
TEST(MetalCompositor, ClearingTheVcamSinkStopsDelivery)
  // set sink, receive >=1, clear sink, render 10 more ticks: count unchanged
```

- [ ] **Step 2:** Run `--gtest_filter='MetalCompositor.Vcam*:MetalCompositor.ClearingTheVcamSink*'`. Expected: FAIL (the sink never fires).
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the filter, then all `MetalCompositor.*`, then full ctest. Expected: PASS.
- [ ] **Step 5: Measurement** (owner Mac, not CI). Run the core with an idle 1080p60 Program and the vcam enabled, and read the snapshot `virtualCamera.framesPublished` delta over 10 s. **Required ≥ 50/s.** If it is lower, record the convert/readback means from the log and open a `backlog` issue "Metal NV12 compute tap" (spec §7). Do not lower the requirement.
- [ ] **Step 6: Mutations:**
  - Remove the light-tick kick. `RunsOnLightTicks` must go red.
  - Swap the U and V order. The U/V assertions must go red.
  - Skip the resize. `ScalesANon1080pCanvas` must go red.
  - Remove the `tapReadInFlight_` guard. No test can catch this one; report it as a reviewer-only property (rule 8).
- [ ] **Step 7: Commit** `native: Metal compositor publishes a 1080p NV12 tap to the virtual camera`.

### Task 6: Shell — footer button, flyout, wire command, persistence

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/VirtualCamera.swift` (read model, presentation, command)
- Modify: `StudioFooter.swift` (lines ~90-97 and ~453-468), `AppModel.swift` (state, `applySnapshot` ~875, a resend on every core generation, activation wiring), `ShellPrefs.swift` (optional `virtualCamera: PersistedVirtualCamera?`), `CoreCommands.swift`, `SettingsPane.swift:197` (status row only; Remove button is Task 12), `ShellTests.swift` (update the line-1206 test, which loops over the unavailable strings: drop the vcam string)

**Interfaces:**
- Consumes: Task 3 `CameraExtensionState`, `CameraExtensionActivating`, `CameraExtensionPolicy.settingsURL/settingsInstruction`.
- Produces:

```swift
struct PersistedVirtualCamera: Codable, Equatable { var enabled = false; var mirror = false; var framerEnabled = false; var deviceName = "" }
struct VirtualCameraReadModel: Equatable {
    var enabled = false; var status = "off"; var warning = ""; var width = 0; var height = 0
    var framerState = "off"; var framerWarning = ""; var framesPublished = 0
    static func parse(_ snapshot: JSONObject) -> VirtualCameraReadModel?   // nil when "virtualCamera" absent
}
enum VirtualCameraPresentation {
    static func label(intent: Bool, ext: CameraExtensionState, core: VirtualCameraReadModel?) -> String
    static func showsSettingsButton(_ ext: CameraExtensionState) -> Bool   // true only for .needsApproval
    static func framerLabel(framerEnabled: Bool, intent: Bool, core: VirtualCameraReadModel?) -> String
}
extension CoreCommands { static func syncVirtualCamera(_ v: PersistedVirtualCamera) -> JSONObject }
// -> ["type":"sync-virtual-camera","on":Bool,"mirror":Bool,"framerEnabled":Bool,"deviceName":String]
```

- `label` precedence (Windows wording where it exists):
  1. `!intent` → "Virtual camera off".
  2. ext `.activating` → "Installing the virtual camera…".
  3. `.needsApproval` → "Allow CoreVideo Pro Camera in System Settings to finish setup."
  4. `.notInApplications` → "Move CoreVideo Pro to the Applications folder to use the virtual camera."
  5. `.needsReboot` → "Restart your Mac to finish installing the virtual camera."
  6. `.unavailable(r)` and `.failed(r)` → `r`.
  7. Core `live` → "Virtual camera live (\(w)x\(h))".
  8. Core `failed` with a `CAMERA_EXTENSION_NOT_FOUND:` or `CAMERA_EXTENSION_LOST:` prefix → "Connecting to CoreVideo Pro Camera…".
  9. Core `failed` with another warning → the warning text after the first ": " (or the whole text if it has none).
  10. Otherwise → "Virtual camera on".
- `framerLabel` ports `VirtualCameraFramerStatusLabel` verbatim: "Framer off" / "Ready when webcam output is enabled" / framerWarning on unavailable / "OH Framer active on webcam output" / "Waiting for webcam frame".
- AppModel:
  - The toggle flips `virtualCamera.enabled`.
  - When enabling with ext ≠ `.active`, call `activate`, and set `cameraExtensionOptedIn = true` on the first enable.
  - Always send `media-core-sync` with `[syncVirtualCamera]` on any field change.
  - Resend after every bridge (re)start (the CLAUDE.md one-shot rule: respawned cores forget).
  - At launch: if `cameraExtensionOptedIn == true`, call `activate` (silent update/replace); else call `query`.
  - The device name is committed on field submit or focus loss, not per keystroke.
  - Remove the temporary Debug menu item from Task 3.
- Footer:
  - Main button: `available: true`, `active: virtualCamera.enabled`, `name: "Virtual camera"`, `help` = label.
  - Flyout (width 300): framer toggle + caption + framer label; mirror toggle + caption; Camera name field + caption; status line (label); when `showsSettingsButton`, the instruction text plus a button "Open System Settings" that opens `settingsURL(osMajor:)`.
  - `virtualCameraUnavailable` is deleted.

- [ ] **Step 1: Write the failing tests:**
  - `testVirtualCameraLabelPrecedence`: one row per precedence line, plus two conflict rows. Intent off with core live → "Virtual camera off". `.needsApproval` with core live → the approval text.
  - `testVirtualCameraReadModelParsesTheCoreShape`: feed the exact `CoreStateCapture::virtualCameraState` keys (`enabled, status, deviceName, resolution{width,height}, fps, framesPublished, framerState, framerWarning, warning`). Expect width 1920 and framesPublished 42 from fixture values 1920 and 42. A snapshot without the node → nil.
  - `testSyncVirtualCameraWire`: exact keys and values. Cross-check the key names against `MediaCore::syncVirtualCamera` by reading the core source the way `testPayloadContractMatchesTheCoreSource` does (`command.get("on"|"mirror"|"framerEnabled"|"deviceName")`).
  - `testVirtualCameraPrefsRoundTripAndOlderFile`: an older prefs file without the key decodes to nil and keeps other fields; a round trip keeps all four fields.
  - `testVirtualCameraResentOnNewCoreGeneration`: drive the bridge-generation test seam used by `testBridgeGenerationRejectsStaleWork` with intent enabled. Expect two generations to send two `sync-virtual-camera` commands.
- [ ] **Step 2:** Run the harness. Expected: FAIL.
- [ ] **Step 3:** Implement, then run the design lint.
- [ ] **Step 4:** Run the harness, `COREVIDEO_SHELL_SELFCHECK=1` and `lint-mac-design.sh`. Expected: PASS. Attach `COREVIDEO_SHELL_SNAPSHOT` PNGs of the flyout in states off, live and needsApproval.
- [ ] **Step 5: Mutations:**
  - Move the core-live check above the extension states. The `.needsApproval` + live row must go red.
  - Drop the bridge-restart resend. The generation test must go red.
  - Rename `framerEnabled` to `framer` on the wire. The core-source cross-check must go red.
- [ ] **Step 6: Commit** `mac: virtual camera footer button, flyout and core wiring`.

### Task 7: Local end-to-end check, probe, and the first owner milestone

**Files:**
- Create: `mac-shell/Sources/VcamProbe/main.swift` (executable target `VcamProbe`, depends on `CameraExtensionKit`)
- Create: `native/tools/mac-vcam-feed.mm` (target `corevideo-vcam-feed`, `EXCLUDE_FROM_ALL`, Apple + option), `scripts/macos-vcam-e2e.sh`
- Modify: `mac-shell/Package.swift`, `native/CMakeLists.txt`, `.github/workflows/ci.yml` (syntax list only)

**Interfaces:**
- Consumes: `SlateClassifier`, `CameraConstants` (Task 1); `createCmioVirtualCameraSink` (Task 4).
- Produces:
  - `VcamProbe capture --seconds N [--fps 30|60] --json` prints `{"frames":Int,"fps":Double,"width":Int,"height":Int,"pixelFormat":"420v","slateFrames":Int,"programFrames":Int,"centerY":Int}`. It opens the AVCaptureDevice whose `uniqueID` equals the legacy ID or the UUID string, as recorded in Task 3 Step 5(f).
  - `corevideo-vcam-feed --seconds N --pattern red` feeds 1920×1080 BT.601 red (Y 82) at 60 fps through `SinkVirtualCameraPublisher` and the real CMIO sink, and prints its final status.
  - `scripts/macos-vcam-e2e.sh [--app] [--restart-extension]` exits 0 only if every step passes, and writes `dist/vcam-e2e/<timestamp>/` with the probe JSONs and `log show` output for subsystem `us.iamfatness.corevideopro.camera` plus `[virtualcam]` lines.

E2E steps (the script's asserts):
1. `/Applications/CoreVideo Pro.app` exists, and `systemextensionsctl list` shows the extension as `activated enabled` with a version equal to the app's `CFBundleVersion`.
2. With no producer: probe 3 s at 30 fps → `fps ≥ 28` and `slateFrames == frames`.
3. Build `corevideo-vcam-feed` and sign it with identifier `us.iamfatness.corevideopro.vcam-probe` (Developer ID, runtime). Start a 6 s feed. Probe 3 s starting at t=1 s → `programFrames ≥ 0.9·frames` and `centerY` 82 ±3.
4. After the feed ends: probe 2 s, starting 1 s after the feed exits → `slateFrames == frames` (law 2, no frozen frame).
5. `--restart-extension` (needs sudo): kill the extension process while feeding. The feeder's status returns to `live` within 3 s, and the probe sees program again.
6. `--app`: launch the app, enable the vcam through the control API (Task 11), probe 3 s → program frames, snapshot `framesPublished` advancing. Then quit the app; within 1 s the probe sees slate.
7. Run `leaks --atExit -- corevideo-vcam-feed --seconds 300 --pattern red` → no `CMSampleBuffer`/`CVPixelBuffer` leaks reported.

- [ ] **Step 1:** Write the script with steps 1–4 and 7 first. Run it on the owner Mac before the probe and feeder exist. Expected: FAIL at step 2, missing binary.
- [ ] **Step 2:** Implement the probe and the feeder.
- [ ] **Step 3:** Run `scripts/macos-vcam-e2e.sh`. Expected: PASS for steps 1–4 and 7. Archive the output directory path in the task report.
- [ ] **Step 4: Milestone gate — owner, live.** Install the signed (if Task 3 Step 5(c) requires it, notarized) build to `/Applications`. Enable the virtual camera in the footer and approve in System Settings. Then:
  - (a) Photo Booth shows Program under "CoreVideo Pro Camera";
  - (b) Zoom shows Program under the same name;
  - (c) toggling off shows the slate in both within 1 s;
  - (d) quitting the app shows the slate.

  Record the results in `docs/superpowers/plans/2026-11-15-mac-virtual-camera-outcomes.md` (create it) and stop for the owner's go-ahead before Task 8.
- [ ] **Step 5: Mutation (of the harness).** Point the probe at the built-in FaceTime camera. Step 2 must fail on `slateFrames`; this proves the classifier is not vacuous.
- [ ] **Step 6: Commit** `mac: virtual camera e2e script, capture probe and sink feeder; milestone outcomes`.

### Task 8: Rename reaches the extension (custom CMIO property)

**Files:**
- Modify: `mac-shell/Sources/CoreVideoProCamera/CameraProvider.swift`, `CameraDevice.swift`; `native/src/modules/CmioVirtualCameraSink.mm`; `native/src/modules/SinkVirtualCameraPublisher.cpp`; `mac-shell/Sources/CameraExtensionKit/CameraConstants.swift` (add `deviceNameProperty = "4cc_cvnm_glob_0000"`, `deviceNameSelector: UInt32 = 0x63766E6D /* 'cvnm' */`)
- Test: `native/tests/SinkVirtualCameraPublisherTest.cpp`; `ShellTests.swift`

**Interfaces:**
- Consumes: `IVirtualCameraSink::setDeviceName` (Task 4), `DeviceNamePolicy` (Task 1).
- Produces:
  - Extension: the device exposes `deviceNameProperty` (read/write CFString). A set sanitizes the value, persists it in `UserDefaults.standard["deviceName"]`, and if it differs from the current name: when no source-stream client is streaming, it removes and re-adds the device under the same UUIDs with the new `localizedName`; otherwise it marks the rename pending and applies it on the source stream's `stopStream`.
  - Core: `SinkVirtualCameraPublisher::start` calls `sink->setDeviceName(status_.deviceName)` right after a successful `connect()`. When that returns true, it logs `[virtualcam] mac camera renamed to '<name>'`. The device swap then surfaces as `Lost` on the next enqueue and is reconnected by the Task 4 retry path. `setDeviceName` (publisher) applies only while stopped (Windows parity).

- [ ] **Step 1: Write the failing tests:**
  - `RenameIsSentOnStartAndSurvivesTheDeviceSwap`: the fake `setDeviceName` returns true, then the next enqueue returns `Lost` and the second connect succeeds. Expect `setDeviceName` called with "Studio A" on each start, final status `live`, and `framesPublished` resuming.
  - `RenameWhileRunningAppliesOnNextStart`: `setDeviceName("B")` while live → the fake not called. After stop and start → called with "B".
  - Shell `testDeviceNamePropertyConstants`: `deviceNameSelector` equals the FourCC of the characters 4–7 of `deviceNameProperty`, computed in the test.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement. The core sets the property through `CMIOObjectSetPropertyData` with selector `'cvnm'`, scope global, element main, and a `CFStringRef` payload.
- [ ] **Step 4:** Run the native filter and the harness. Expected: PASS. Owner: rename to "Studio A", toggle off and on, and confirm Photo Booth lists "Studio A" (Zoom may need a relaunch to refresh its list; record what you observe).
- [ ] **Step 5: Mutations:**
  - Call `setDeviceName` before `connect()`. The fake must record the call order and fail.
  - Apply the rename while streaming. This is extension-side and reviewer-only; check it on the owner Mac by renaming mid-call in Zoom: the name must not change until the call stops using the camera.
- [ ] **Step 6: Commit** `mac: camera rename reaches the extension via a custom CMIO property`.

### Task 9: OH Framer on Mac — CPU twin of `GpuWebcamFramer`

**Files:**
- Create: `native/src/modules/WebcamFramerCpu.{h,cpp}` (portable blend), `native/src/modules/WebcamFramerAppleDecode.mm` (PNG via ImageIO → straight RGBA)
- Modify: `native/src/modules/GpuWebcamFramer.cpp` (`#elif defined(__APPLE__) && COREVIDEO_WITH_MAC_VIRTUALCAM` Impl delegating to the CPU twin), `native/CMakeLists.txt`, `native/tests/GpuWebcamFramerTest.cpp` (introduce `COREVIDEO_FRAMER_RENDERS` = `(_WIN32 && COREVIDEO_WITH_D3D11) || (__APPLE__ && COREVIDEO_WITH_MAC_VIRTUALCAM)` and use it in place of the Windows-only guards)

**Interfaces:**
- Consumes: `webcamFramerPng` (generated header), `WebcamLoudnessOverlay`, `shaders/WebcamFramer.hlsl` (the reference algorithm).
- Produces:
  - `bool decodeEmbeddedPngStraightRgba(const uint8_t* png, size_t size, std::vector<uint8_t>& rgba, int& w, int& h, std::string& error)`. It draws into a premultiplied RGBA context, then un-premultiplies (the `StillMediaFrameCache` approach) and requires 1920×1080.
  - `class WebcamFramerCpu { bool prepare(int w, int h, std::string& error); void apply(const uint8_t* clean, uint8_t* out, int w, int h, bool mirror, WebcamLoudnessOverlay meter); }`. This is a line-for-line port of `WebcamFramer.hlsl` (`glyph`, `meterPixel`, `pixel`, `yuv`, `blend`) in `float` arithmetic, so it rounds like the GPU.
  - Cache per (w, h, mirror) the list of luma spans whose overlay alpha > 0. Recompute the meter rectangle every frame only when `meter.show`.

- [ ] **Step 1:** Change the test guards. Run `--gtest_filter='GpuWebcamFramer.*:WebcamLoudnessOverlay.*'` on `build-metal` with the option ON. Expected: FAIL. These are the **unchanged Windows oracle values**: 100, 170, 58, 149, 74, the 235 glyph pixel, the mirror equivalence and the panel-only diff.
- [ ] **Step 2:** Implement.
- [ ] **Step 3:** Run the filter. Expected: PASS, including `Warm1080pProcessingReportsCost`, which now prints on Mac. Record mean and worst; the target is a mean ≤ 4 ms on the owner Mac, reported rather than asserted (Windows parity). Run full ctest.
- [ ] **Step 4:** Run the e2e `--app` step with the framer on. The probe's centerY is unchanged and a guide pixel at (420, 400) reads luma 74 ±3. Add this as e2e step 6b.
- [ ] **Step 5: Mutations:**
  - Use `double` in place of `float` in `blend`. Watch whether any exact-value assertion moves (report either way; this checks whether float parity is load-bearing).
  - Drop the chroma 2×2 averaging. The value 149 must go red.
  - Skip the mirror of the overlay coordinate. `GuidesRemainReadableAfterBackendMirror` must go red.
- [ ] **Step 6: Commit** `native: OH Framer on macOS (CPU twin of the D3D11 shader, Windows oracle values)`.

### Task 10: 720p and 30/60 fps for consumers

**Files:**
- Modify: `mac-shell/Sources/CameraExtensionKit/CameraConstants.swift` (`CameraFormats.source` appends `CameraFormat(1280, 720, [60, 30])`), `mac-shell/Sources/CoreVideoProCamera/SourceStream.swift` (honour `activeFormatIndex` and `frameDuration`; scale with a `VTPixelTransferSession` into a 720p pool when the active format is 720p; build the 720p slate natively, not scaled)
- Test: `ShellTests.swift`; e2e script

**Interfaces:**
- Consumes: Task 2 streams.
- Produces: `public static func frameIntervalNanoseconds(fps: Int) -> UInt64` in `CameraFormats`. It returns 16_666_667 for 60 and 33_333_333 for 30, and rejects anything outside the format's list by falling back to the format's first rate.

- [ ] **Step 1: Failing tests.**
  - `testCameraFormatsTable`: `source.count == 2`; `source[0]` is 1920×1080 [60,30]; `source[1]` is 1280×720 [60,30]; the interval for 60 is 16_666_667 and for 30 is 33_333_333; a request for 24 falls back to the 60 interval.
  - E2E step 2b: probe `--fps 30` with the 720p format selected (add a `--format 720p` flag to the probe) → width 1280, `fps ≥ 28`, the slate classified correctly at 720p.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the harness and the e2e. Expected: PASS. Owner: check FaceTime and Photo Booth still pick 1080p by default.
- [ ] **Step 5: Mutations:**
  - Serve 1080p buffers on the 720p format. Probe width 1280 must go red.
  - Build the 720p slate by scaling the 1080p one. The classifier at 720p stays green, so record this as reviewer-only.
- [ ] **Step 6: Commit** `mac: camera extension offers 720p and 30/60 fps`.

### Task 11: Control API — `transport.virtualcam.*` on Mac

**Files:**
- Modify: `mac-shell/Sources/CoreVideoProShell/ControlApi.swift` (actions, in Windows order and with Windows text from `ControlActionRegistry.cs:80-86`), `ControlSurface.swift` (dispatch), `ControlApiTests.swift` (manifest golden + invoke-every-action test)

**Interfaces:**
- Consumes: Task 6 `AppModel` virtual-camera intent setters.
- Produces:
  - `transport.virtualcam.toggle`.
  - `transport.virtualcam.set {on: bool}`.
  - `transport.virtualcam.mirror.set {on: bool}`.
  - `transport.virtualcam.name.set {name: string, optional}`, where blank means the default.

  Replies follow the existing `outputSet` shape. `set` is idempotent: repeating it does not toggle.

- [ ] **Step 1: Failing tests:**
  - The manifest golden includes the four ids in Windows' order, between `transport.stream.set` and the scene actions as Windows places them. Check the order against `ControlActionRegistry.cs`.
  - The invoke-every-registered-id test (rule 10) covers the new ids.
  - `set true` twice leaves intent true and sends one `sync-virtual-camera` per change, not per call.
- [ ] **Step 2:** Run the harness. Expected: FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the harness and the e2e `--app` step (step 6), which now uses `POST 127.0.0.1:8011` `{"action":"transport.virtualcam.set","args":[true]}`. Expected: PASS.
- [ ] **Step 5: Mutations:**
  - Remove the `name.set` dispatch case. The invoke-every-id test must go red.
  - Make `set` call toggle. The idempotence test must go red.
- [ ] **Step 6: Commit** `mac: control API virtual camera actions (Windows manifest parity)`.

### Task 12: Release pipeline, removal, docs and owner acceptance

**Files:**
- Modify: `scripts/release-macos.sh` (run `check-macos-camera-extension.sh --release` after the Mach-O audit; manifest gains `cameraExtension: {bundleVersion, profileExpires, certificateExpires}`; **fail** if the signing certificate expires within 14 days; warn within 60)
- Modify: `scripts/make-macos-pkg.sh` (after `pkgbuild`, assert that `pkgutil --payload-files` lists `CoreVideo Pro.app/Contents/Library/SystemExtensions/us.iamfatness.corevideopro.shell.camera.systemextension/Contents/MacOS/CoreVideoProCamera`)
- Modify: `mac-shell/Sources/CoreVideoProShell/SettingsPane.swift` (Virtual camera row: status label + "Remove virtual camera" button → `deactivate`, with the caption "macOS may ask for your password. The camera disappears from other apps.")
- Modify: `docs/reference/virtual-camera.md` (a "macOS" section: install/approve, troubleshooting by status text, e2e script, logs); `docs/qa/macos-1.0-owner-acceptance.md` (new section, below); `docs/superpowers/plans/2026-11-14-mac-parity-1.0-roadmap.md` (M9.2 row → link this plan and the outcomes doc)
- Test: `ShellTests.swift`; release rehearsal

**Interfaces:**
- Consumes: Task 3 `deactivate`; Task 2 check script and `verify_profile`.
- Produces: owner acceptance checklist items, each with an expected result:
  1. Fresh install from the notarized `.pkg` → first enable → approval prompt → Program in **Photo Booth**, **FaceTime**, **Zoom**, **Teams in Chrome**, **Google Meet in Chrome and in Safari**, **OBS** (Video Capture Device).
  2. Approval denied, then granted later → the flyout moves from approval text to live with no relaunch.
  3. App launched from the DMG without installing → "Move CoreVideo Pro to the Applications folder…".
  4. Sleep 2 min mid-Zoom-call, wake → Zoom shows the slate during the gap and Program again within 5 s of the core resuming; never a frozen frame.
  5. Quit the app mid-call → slate within 1 s. Relaunch → Program again with no toggle (intent persisted).
  6. Update: install build N+1 over N while Zoom is closed → first launch replaces the extension silently (`systemextensionsctl list` shows N+1); camera works.
  7. Rename to "Studio A" → after toggling off and on, Photo Booth lists "Studio A".
  8. OH Framer and mirror visible in Zoom's self-view; Program recording stays clean (check the recorded file's pixels at a guide location).
  9. "Remove virtual camera" → the device disappears from Photo Booth after relaunching it.
  10. Stream key and Zoom sign-in still present after the first signed launch (Review Focus 5).
  11. Per-frame delivery: the e2e probe at 60 fps for 60 s reports ≥ 59 fps with the vcam fed from a live 1080p60 Program (CLAUDE.md frame-delivery rule; report the worst interval as unverified if the probe cannot measure it).

- [ ] **Step 1: Failing tests:**
  - Shell `testRemoveVirtualCameraCallsDeactivateAndClearsOptIn`, with the fake activator: one `deactivate` call; `cameraExtensionOptedIn == false`; intent off; one `sync-virtual-camera {on:false}` sent.
  - Release rehearsal `scripts/release-macos.sh --skip-notarize` with `COREVIDEO_CAMERA_PROFILE=/nonexistent` → must exit non-zero, naming the env var.
- [ ] **Step 2:** Run. Expected: FAIL.
- [ ] **Step 3:** Implement the scripts, the Settings row and the docs.
- [ ] **Step 4:** Run the harness, then `scripts/release-macos.sh --skip-notarize`. Expected: PASS, with the manifest showing `cameraExtension`. Then do a full notarized run if the owner schedules one, and run `spctl -a -vv -t install` on the pkg.
- [ ] **Step 5: Mutations:**
  - Make `verify_profile` skip the certificate-membership check. Reviewer: sign with a profile whose certificate list lacks the identity (use the extension profile for the app). The check must fail on the app-id mismatch first, so also report whether certificate membership is independently enforced.
  - Remove the pkg payload assertion. Reviewer-only.
- [ ] **Step 6: Commit** `mac: virtual camera release gates, removal, docs and owner acceptance`. Then update the outcomes doc (Task 7) with final results, rulings made on the owner's behalf, and deferred minors.

---

## Self-review (done at authoring time)

- **Spec coverage:**
  - §1 platform: Tasks 2–3.
  - §2 frame path: Tasks 4–5.
  - §3 formats: Tasks 2 and 10.
  - §4 laws. Law 1: Task 4 (copy-on-enqueue, full queue drops new). Law 2: Tasks 1, 2 and 7. Law 3: Tasks 3, 6 and 12. Law 4: Tasks 5 and 4. Law 5: logging in Tasks 2, 3 and 4. Law 6: Task 2 `ProducerAuthorizer` plus the e2e feeder signing.
  - §5 health: Tasks 4 and 6.
  - §6 rename: Task 8.
  - The framer is in the Windows feature set: Task 9.
  - Control-API parity: Task 11.
  - Packaging, notarization and the pkg: Tasks 2 and 12.
- **Carried facts the executor must re-verify, not assume:**
  - The camera-extension process user (Task 3 Step 5d).
  - The HAL UID form (5f).
  - Whether activation needs notarization (5c).
  - That the custom-property string format `4cc_<sel>_glob_0000` is honoured (Task 8 Step 4). It is not in the SDK headers; it comes from WWDC22 material and Apple forum replies.
- **Type consistency:** `CameraExtensionState`, `IVirtualCameraSink`, `SinkEnqueueResult`, `PersistedVirtualCamera` and `VirtualCameraReadModel` have the same names in every task that uses them. `setDeviceName` is declared in Task 4 and implemented in Task 8.
- **Proportion:** signatures, tests and values only. No function bodies except the test assertions and the label precedence, which is exact copy.
