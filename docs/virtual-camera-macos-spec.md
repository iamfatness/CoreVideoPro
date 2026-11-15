# Virtual Camera on macOS — addendum to `virtual-camera-spec.md`

Status: owner-requested 2026-11-15 (roadmap M9.2, "a very important part of this app").
Goal: Program appears as the system camera **"CoreVideo Pro Camera"** in Zoom, Teams,
Meet (Chrome/Safari), FaceTime, OBS and Photo Booth, with the Windows feature set:
enable toggle, OH Framer, mirror, rename, status. Read the Windows spec first; this file
records only what differs and why.

## 1. The platform decision

**CoreMediaIO Camera Extension** (macOS 12.3+, used from 13 — the app floor; arm64 only).
A sandboxed system extension the OS camera service hosts; every AVFoundation/WebRTC
client sees it, including sandboxed browsers — which the retired DAL plug-ins never did.
Bundled at `CoreVideo Pro.app/Contents/Library/SystemExtensions/us.iamfatness.corevideopro.shell.camera.systemextension`,
activated by the app with `OSSystemExtensionRequest`, approved once by the user.

**Producer = the core** (`corevideo-native`), not the shell. The core already owns the
camera publisher (`AsyncVirtualCameraPublisher`: lifecycle worker, framer fork, retry,
snapshot status). A new Mac backend behind `IVirtualCameraPublisher` feeds the extension's
**sink stream** through the CoreMediaIO C API (the pattern of Apple's sample and OBS
`mac-virtualcam`). Frames reach it from a new Metal-compositor NV12 tap — the Mac twin of
the D3D11 `setIdentifiedVcamFrameSink` path.

**Built without Xcode project files.** `swift build` (a new executable target in
`mac-shell/Package.swift`) + a hand-written Info.plist + `codesign` with the embedded
Developer ID profile, exactly as the shell is built today. Nothing here needs xcodebuild.

Facts verified 2026-11-15 against the macOS 26 SDK on the owner's Mac:
`CMIOExtensionStreamDirectionSink`, `consumeSampleBufferFromClient:`,
`CMIOExtensionPropertyStreamSinkBufferQueueSize`, `CMIOExtensionClient.signingID` (13.0),
`CMIOStreamCopyBufferQueue`, `OSSystemExtensionRequest.propertiesRequest` (12.0) and
`OSSystemExtensionErrorUnsupportedParentBundleLocation` all exist. Both provisioning
profiles expire 2044-11-10. The certificate inside them, `Developer ID Application:
John Wallace (42394L3353)`, expires **2027-02-01**. That is the date that matters: after
it, builds need a new certificate and new profiles.

## 2. Frame path

```
corevideo-native (core, our signature)                      camera extension (sandboxed,
  Metal compositor ── tap thread: blit→readback→NV12 1080p   runs under a system role user)
        │ setIdentifiedVcamFrameSink                          ┌──────────────────────────┐
  AsyncVirtualCameraPublisher (worker: framer, retry)         │ sink stream  "…Sink"     │
        │ publishNv12IdentifiedOnWorker                       │   consume → latest buffer│
  SinkVirtualCameraPublisher (mirror, size gate, lost→retry)  │ FrameSelector            │
        │ IVirtualCameraSink                                  │   fresh ≤500 ms → Program│
  CmioVirtualCameraSink ── IOSurface CVPixelBuffer ──CMSimpleQueue──▶ else → standby slate│
                                                              │ source stream (timer @fps)│
                                                              └────────────┬─────────────┘
shell (SwiftUI): OSSystemExtensionRequest, footer flyout,                  ▼
  sync-virtual-camera {on, mirror, framerEnabled, deviceName}   Zoom / Teams / FaceTime / OBS
```

The shell never touches pixels. It activates the extension, sends the same
`sync-virtual-camera` command Windows sends, and reads the same `virtualCamera` snapshot.

## 3. Formats

The sink takes **1920×1080 NV12 ('420v', BT.601 video range)** only — the size the core
already pins (`MediaCore::syncVirtualCamera`, #764). Correction to the brief: Windows does
**not** negotiate. Its media source is fixed at 1920×1080@60 (`MediaSource.h`). The
"720p30 default" in the Windows spec §3 is historical. The Mac source stream offers
1920×1080 at 60/30 fps (format 0, default) and 1280×720 at 60/30 (format 1, scaled inside
the extension) so consumers that ask for 720p get it without a second producer size.

## 4. Laws (Windows §4, carried over)

1. **No torn frame.** Windows' seqlock becomes buffer handoff. Each frame is its own
   pool-backed `CVPixelBuffer`, and the core never writes to it after the enqueue. A
   torn frame cannot happen by construction. A full queue drops the *new* frame and
   counts the drop. It never blocks.
2. **Always serve something at the declared fps.** The extension's source timer runs
   whenever a consumer streams. A frame older than **500 ms** (Windows: 30 frames at
   60 fps) gives way to the standby slate. That slate is byte-identical to
   `fillVirtualCameraStandbySlate`. When the producer stops, the slate shows at once,
   with no hold. Quit, crash, sleep and disable all end on the slate, never a frozen frame.
3. **Explicit and reversible, adapted.** Activation happens only on operator intent: the
   first enable, or a relaunch after an earlier opt-in. Disable stops the feed, and the
   camera shows the slate. **Mac deviation:** the device stays listed while the
   extension is installed (as with OBS and Camo), because a system extension cannot
   appear and vanish per enable. "Remove virtual camera" in Settings deactivates it.
4. **No pixel work on the audio worker or the render tick.** NV12 conversion runs on the
   compositor's tap thread. Mirror and enqueue run on the publisher worker. The
   extension does its own work on its own dispatch queues.
5. **Everything logged.** Core `[virtualcam]` lines: connect, lost, reconnect, rename.
   Extension: `os_log` subsystem `us.iamfatness.corevideopro.camera` (start/stop per
   stream, producer authorized/denied, slate↔program transitions). Shell: every
   activation-state transition goes to `ShellLog`.
6. **(Mac) Only our core may feed the sink.** `authorizedToStartStream(for:)` on the sink
   checks the client's code signature against
   `anchor apple generic and certificate leaf[subject.OU] = "42394L3353" and (identifier "us.iamfatness.corevideopro.core" or identifier "us.iamfatness.corevideopro.vcam-probe")`.
   It fails closed. Any consumer may read the source stream.

## 5. Health as data

The snapshot `virtualCamera` is unchanged in shape. Mac meanings: `live` = sink connected
and frames enqueuing; `failed` + `CAMERA_EXTENSION_NOT_FOUND:` = device not present (not
installed, awaiting approval, or restarting), retried every 1 s by the existing worker;
`failed` + `CAMERA_EXTENSION_LOST:` = sink vanished mid-stream (extension restart/update),
retried the same way. The shell layers its own **activation state** on top: notInstalled,
activating, needsApproval, active, needsReboot, notInApplications, unavailable, failed.
That layer drives the Mac-only approval UI. Approval lives in System Settings › General ›
Login Items & Extensions › Camera Extensions on macOS 15+, and in Privacy & Security on
macOS 13–14.

## 6. Rename

Windows passes the name to `MFCreateVirtualCamera` at each start. On Mac the extension
owns the device name. Correction to the brief: an **app-group container cannot carry the
name**, because camera extensions run under a system role user, not the logged-in user,
so they share no container with the app. The core instead writes the name through a
**custom CMIO device property** (`4cc_cvnm_glob_0000`, a CFString) when it connects. The
extension sanitizes it and persists it in its own defaults. It rebuilds the device under
the new name once no consumer is streaming. The core's existing lost→retry path
reconnects. The rule matches Windows: "takes effect next time you enable the camera".

## 7. Out of scope

Audio (virtual microphone). An Intel slice. macOS < 13. Moving NV12 conversion onto the
GPU is a follow-up if the measured publish rate at 1080p60 is under 50 fps (plan Task 5).
