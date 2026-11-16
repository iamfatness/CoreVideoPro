# macOS virtual camera: core never sees an extension that registers after its first CMIO touch

Date: 2026-11-16. Branch base: `mac-tier0b` @ 366bb534. Status: root cause found, fix not started.
Owner priority: virtual camera is the top priority for the Mac app.

## Symptom

After the reboot that let camera extension 3003 replace 3002, Photo Booth shows
"CoreVideo Pro Camera" as a dark frame with a grey bar. That is the extension's
standby slate (`CameraExtensionKit/StandbySlate.swift`): the extension is healthy
and is receiving nothing from the core.

## Evidence (all measured on the owner's Mac, macOS 26.3, 2026-11-16)

1. Extension 3003 is `activated enabled`, running as `_cmiodalassistants`, zero
   crash reports. (The 12:24:28 `.ips` is 3002's known sandbox-compile crash.)
2. A fresh process finds the device: UID `us.iamfatness.corevideopro.camera.device`,
   streams `CoreVideo Pro Camera` and `CoreVideo Pro Camera Sink`.
3. The running core (`/Library/Application Support/CoreVideo Pro/corevideo-native`)
   logs `CAMERA_EXTENSION_NOT_FOUND` once a second, 210+ attempts, across a
   disable/enable. `/snapshot` `virtualCamera.status = failed`,
   `framesAccepted` rising, `framesPublished = 0`.
4. The core is not blocked from camera extensions in general: its capture list
   has OBS Virtual Camera and OBSBOT Virtual Camera.
5. The core's main thread IS servicing the main run loop (`sample`: `main` →
   `CFRunLoopRun` → `mach_msg`). The earlier fix (`core/MainRunLoopHost.h`) is in
   this build and is not the gap.
6. Timeline from the unified log (`/usr/bin/log`, subsystem `com.apple.cmio`):

   | time | event |
   |---|---|
   | 12:24:14.488 | register assistant submits launchd job for 3002 (path `1B52C927…`) |
   | 12:24:16 / :18 | shell (pid 770) / core (pid 910) start at login |
   | 12:24:18.666 | shell: `CMIODALExtensionSession init` |
   | 12:24:19.251 | core connects to `registerassistantservice.system-extensions` |
   | 12:24:19.274 | shell submits the 3002→3003 activation request |
   | 12:24:19.349 | register assistant terminates 3002, removes its launchd job |
   | 12:24:19.357 | register assistant submits the job for 3003 (path `2DCAC9DF…`) |
   | 12:24:19.413–.530 | core: `CMIODALExtensionSession init`, builds one plug-in per provider: Apple ×5, OBS, OBSBOT. **No plug-in for `us.iamfatness.corevideopro.shell.camera`.** |
   | 12:24:19.823 | `avconferenced` (already running) receives `setExtensions:` and adds our device |
   | 12:24:20.849 | **last CMIO event the core ever logs** (through 12:30+) |
   | 12:24:23 → | processes whose FIRST CMIO touch comes later (parsecd, Discord, Chrome, Safari, system_profiler, Photo Booth) all get our provider |
   | 12:24:29 | shell: "no device 10.0 s after activation" → `needsReboot` (false: the device is present system-wide) |

## Root cause

The core's in-process CoreMediaIO extension session took its provider list in the
~450 ms window when our extension's launchd job had been removed for the
replacement and 3003 had not registered. The later provider-list update
(`setExtensions:`) never reached the core process, so its DAL has no plug-in for
our extension and `findDevice()` (`native/src/modules/CmioVirtualCameraSink.mm:85`)
polls a device list that cannot change for the life of the process.

`MainRunLoopHost` fixed arrival of a DEVICE from a provider the process already
knows (extension process restart under the same launchd job). It does not cover
arrival of a PROVIDER the process has never been told about. That is the case for:

- an extension update at launch (today): the shell spawns the core, then submits
  the replacement;
- **a first install**: the operator approves the extension minutes after the
  core's first CMIO touch. Every new customer's first run;
- any activation the shell submits while the core is running.

The shell has the same blind spot: its "no device 10 s after activation" check
reads its own in-process device list, so it reported `needsReboot` for a device
that every fresh process could see.

Not yet known (Apple-internal, redacted in logs): why `avconferenced` received
the live update and third-party processes did not. Do not build on a guess here.

## Not yet confirmed

- Prediction: quitting and relaunching the app (no reboot) makes the camera show
  Program, because the new core's first CMIO touch follows 3003's registration.
  The owner has not run this yet. If it still shows the slate, this plan's root
  cause is incomplete: stop and return to investigation.
- The rest of the producer path (`ProducerAuthorizer`, sink start, frame enqueue)
  has never run against a live extension from the `/Library` core. Expect a
  second defect behind this one.

## Fix plan

Rules: failing test or measured repro before each fix; one change at a time; no
restart of the owner's running app, no install, no reboot, no push, no PR, no
GitHub issue without the owner's say-so. `superpowers:systematic-debugging` and
`superpowers:test-driven-development` apply.

### Task 1 — Confirm with the owner's relaunch (owner step, 2 minutes)
Owner quits and relaunches CoreVideo Pro, opens Photo Booth. Record: core log
line `mac sink connected …`, `/snapshot` `virtualCamera.framesPublished` rising,
Photo Booth picture. If a second defect appears, root-cause it before Task 3.

### Task 2 — Repro harness (no owner needed)
A provider-arrival repro that does not need an install: a tiny CLI
(`mac-shell/Sources/VcamProbe` gains `watch`, or a new SwiftPM tool) that touches
CMIO, services the main run loop, and prints the device list each second with
timestamps. Then find a way to make a provider arrive after its first touch.
Candidates, cheapest first; record what each does:
1. a second, throwaway camera extension target with its own bundle id, activated
   by a throwaway host app (needs Developer ID signing + notarization; reuses
   `scripts/release-macos.sh` machinery; no reboot, owner approves once);
2. bumping `CoreVideoProCamera` to 3004 and replacing (blocked while 3002 is
   "waiting to uninstall on reboot"; needs an owner reboot per cycle).
If neither is workable without the owner, say so and go to Task 3 with the
design that does not depend on in-process refresh.

### Task 3 — Choose the fix by measurement, in this order
**A. In-process refresh (only if Task 2 gives a repro).** Find a supported call
that makes a running process re-query providers (candidates: a fresh
`AVCaptureDevice.DiscoverySession`; setting a `kCMIOHardwareProperty…` allow
flag; a `kCMIOHardwarePropertyDevices` listener registered before first touch).
Accept only one that is measured to work in the repro three times running.

**B. Sink in a helper process (the design that works by construction).** The CMIO
sink leaves the core. A small signed helper in `/Library/Application
Support/CoreVideo Pro/` (same directory and signing requirement the extension's
`ProducerAuthorizer` already attests) owns `CMIOStreamCopyBufferQueue` /
enqueue. The core hands it finished NV12 Program frames (IOSurface-backed
`CVPixelBuffer` over XPC/mach, or the existing `VirtualCameraShm.h` layout;
measure, 1080p60 is the bar and Program frame delivery must not regress). The
core respawns the helper on a bounded ladder (house ladder:
`OutputDestinationSupervisorPolicy.h`) whenever it reports
`CAMERA_EXTENSION_NOT_FOUND`; a newborn process always sees current providers.
Program is never restarted to recover a camera. This also takes a third-party
HAL client out of the render process.

Rejected: restarting the core when the camera is missing (kills Program);
ordering the shell's activation before the core spawn (fixes updates only, not
first install; still worth doing as a small extra once A or B lands).

### Task 4 — Shell stops judging from its own stale view
`CameraExtensionActivation.swift`: decide "device present" from a fresh process
(the Task 2 probe, or the helper from 3B), never from in-process enumeration.
`needsReboot` only when sysextd reports the NEW version itself is not enabled.
Tests in `ShellTestsVcamUpdate.swift` for: new version enabled + old copy
uninstalling + device visible to a fresh process → `active`, not `needsReboot`.

### Task 5 — Observability
`virtualCamera` snapshot node gains what would have named this in one read:
whether a fresh-process probe sees the device while the sink does not
(`deviceVisibleToSystem`, `sinkAttempts`, `lastConnectedAt`). Core log line on
the transition, once.

### Task 6 — Verify
- Unit: new policy types (pure, `OutputLifecyclePolicy` shape) red→green.
- `scripts/macos-vcam-e2e.sh` with the probe classifying Program vs slate.
- Shell suite + selfcheck, native ctest (Metal/AVF/CoreAudio) green.
- Perf: Program delivery unchanged with the camera on (state hardware, duration).
- Build installer 6 with `scripts/release-macos.sh` (.pkg only), extension
  version bumped only if the extension changed. Do not install.

### Owner gate
Owner installs, then runs the two cases that matter: (1) update at launch with
the app auto-reopened at login, (2) camera turned on in Photo Booth and Zoom
while the app has been running since before activation. Report evidence, not
"should work".

## Files

- `native/src/modules/CmioVirtualCameraSink.mm`, `SinkVirtualCameraPublisher.cpp`
- `native/src/core/MainRunLoopHost.h`, `native/src/main.cpp`
- `mac-shell/Sources/CoreVideoProShell/CameraExtensionActivation.swift`, `VirtualCamera.swift`, `CoreLocation.swift`
- `mac-shell/Sources/CoreVideoProCamera/{SinkStream,ProducerAuthorizer}.swift`
- `mac-shell/Sources/VcamProbe/main.swift`, `scripts/macos-vcam-e2e.sh`, `scripts/release-macos.sh`
- Ledger: `.claude/worktrees/mac-virtual-camera/.superpowers/sdd/2026-11-15-mac-virtual-camera/progress.md`
