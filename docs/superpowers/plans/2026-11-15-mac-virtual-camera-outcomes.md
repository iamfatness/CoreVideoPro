# Mac virtual camera (roadmap M9.2) — outcomes

Plan: `docs/superpowers/plans/2026-11-15-mac-virtual-camera.md`. Spec:
`docs/virtual-camera-macos-spec.md`.

This file records what was **observed** on the owner's Mac. Every `TBD` below is
unmeasured. Do not replace one with an expectation or a value read from code.
Write the observed value, the date and the build (app `CFBundleVersion` and
commit).

## 1. Owner spike facts (Task 3 Step 5)

Build: TBD (CFBundleVersion, commit, notarized yes/no). macOS: TBD. Date: TBD.

| | Fact | Observed |
|---|---|---|
| (a) | `systemextensionsctl list` line for `us.iamfatness.corevideopro.shell.camera` | TBD |
| (b) | Approval pane location on macOS 26.3; did the macOS 15+ URL open it? | TBD |
| (c) | Did activation need a notarized build? (error 9 on a non-notarized Developer ID build → rebuilt with `--notarize "CoreVideo Pro"`) | TBD |
| (d) | User the extension process runs as (`ps -o user= -p $(pgrep CoreVideoProCamera)`) | TBD — if it is the logged-in user, spec §6's reason changes (the design still holds) |
| (e) | Photo Booth shows the slate under "CoreVideo Pro Camera" | TBD |
| (f) | `system_profiler SPCameraDataType` Unique ID: legacy ID (`us.iamfatness.corevideopro.camera.device`) or the UUID string (`791E98ED-72E7-4716-9A26-1B4FBB9A1520`)? | TBD — `VcamProbe` and `CmioVirtualCameraSink` accept both until this is known; `VcamProbe list` marks the match |

## 2. Local end-to-end check (Task 7, `scripts/macos-vcam-e2e.sh`)

Command: `scripts/macos-vcam-e2e.sh [--restart-extension] [--app]`. Artifacts:
`dist/vcam-e2e/<timestamp>/` (probe JSON/CSV, feeder logs, `extension.log`,
`virtualcam.log`).

Red-first run (2026-11-15, extension not installed on this Mac): exit 1 at
step 1 — `E2E FAIL (step 1): systemextensionsctl does not list
us.iamfatness.corevideopro.shell.camera as [activated enabled]`. That is the
expected failure before installation, not a result.

Producer admission (spec §4 law 6), checked locally with
`codesign --verify -R=<CameraConstants.producerRequirement>` on Developer ID
signed copies of `corevideo-vcam-feed` (2026-11-15): identifier
`us.iamfatness.corevideopro.vcam-probe` passes, `us.iamfatness.corevideopro.core`
passes, `us.iamfatness.corevideopro.shell`, `us.iamfatness.corevideopro.vcam-probe-x`
and an ad-hoc signature fail. This proves the requirement string, not the
extension's runtime check, which step 3 exercises.

| Step | Assertion | Result | Evidence |
|---|---|---|---|
| 1 | extension `activated enabled` at the app's `CFBundleVersion` | TBD | TBD |
| 2 | no producer: fps ≥ 28, `slateFrames == frames` | TBD | TBD |
| 3 | signed feeder: `programFrames ≥ 0.9·frames`, `centerY` 82 ±3 | TBD | TBD |
| 4 | 1 s after the feed: `slateFrames == frames` (no frozen frame) | TBD | TBD |
| 5 | `--restart-extension`: feeder live again ≤ 3 s, probe sees program | TBD | TBD |
| 6 | `--app`: program via the control API, `framesPublished` advancing, slate ≤ 1 s after quit | TBD (needs Task 11's `transport.virtualcam.set`; the script skips it until then) | TBD |
| 7 | `leaks --atExit` over 300 s: no `CMSampleBuffer`/`CVPixelBuffer` leaks | TBD | TBD |

Harness mutation (Task 7 Step 5): point the probe at the FaceTime camera
(`VcamProbe capture --seconds 3 --json --device-id <FaceTime uniqueID>`). Step 2's
`slateFrames == frames` must fail. Result: TBD.

## 3. Milestone gate — owner, live (Task 7 Step 4)

Build installed to `/Applications`: TBD (CFBundleVersion, commit, notarized).
Enabled from the footer and approved in System Settings: TBD.

| | Check | Result | Notes |
|---|---|---|---|
| (a) | Photo Booth shows Program under "CoreVideo Pro Camera" | TBD | |
| (b) | Zoom shows Program under the same name | TBD | |
| (c) | Toggling off shows the slate in both within 1 s | TBD | |
| (d) | Quitting the app shows the slate | TBD | |

Owner go-ahead for Task 8: TBD.
