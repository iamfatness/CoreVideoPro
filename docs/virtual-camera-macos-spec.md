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
`mac-virtualcam`). Frames come from a new GPU NV12 tap in the Metal compositor (the twin of D3D11 `setIdentifiedVcamFrameSink`).

**Built without Xcode project files.** `swift build` (a new executable target in
`mac-shell/Package.swift`) + a hand-written Info.plist + `codesign` with the embedded
Developer ID profile, exactly as the shell is built today. Nothing here needs xcodebuild.

Verified in the macOS 26 SDK (2026-11-15): sink streams, `consumeSampleBufferFromClient:`,
`CMIOExtensionClient.signingID` (13.0), `CMIOStreamCopyBufferQueue`, `propertiesRequest`. Both provisioning
profiles expire 2044-11-10. The certificate inside them, `Developer ID Application:
John Wallace (42394L3353)`, expires **2027-02-01**. That is the date that matters: after
it, builds need a new certificate and new profiles.

## 2. Frame path

```
corevideo-native (core, our signature)                      camera extension (sandboxed,
  Metal compositor ── GPU kernel: resize+mirror+BT.601 NV12   runs under a system role user)
        │ setIdentifiedVcamFrameSink                          ┌──────────────────────────┐
  AsyncVirtualCameraPublisher (worker: Metal framer, retry)   │ sink stream  "…Sink"     │
        │ publishNv12IdentifiedOnWorker                       │   consume → latest buffer│
  SinkVirtualCameraPublisher (size gate, lost→retry, gaps)    │ FrameSelector            │
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
4. **No CPU per-pixel work (CLAUDE.md north star).** Resize, mirror and BGRA→NV12
   (integer BT.601, bit-identical to `convertBgraToNv12`) run in one Metal compute pass
   after the program pass. Only the finished 3,110,400-byte NV12 is read back. OH Framer
   is a Metal port of `WebcamFramer.hlsl`. The CPU does only bulk copies: at most 3 per
   frame, never on the render thread. The camera must publish **60 frames per second
   with no missed frame** at idle 1080p60. That is a per-frame requirement, not an
   average. The byte-buffer interface the Windows tap uses is kept, so shared and
   Windows code are untouched. The extension does its own work on its own dispatch
   queues.
5. **Everything logged.** Core `[virtualcam]` lines: connect, lost, reconnect, rename.
   Extension: `os_log` subsystem `us.iamfatness.corevideopro.camera` (start/stop per
   stream, producer authorized/denied, slate↔program transitions). Shell: every
   activation-state transition goes to `ShellLog`.
6. **(Mac) Only our core may feed the sink.** `authorizedToStartStream(for:)` on the sink
   checks the client's code signature against
   `anchor apple generic and certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate leaf[field.1.2.840.113635.100.6.1.13] exists and certificate leaf[subject.OU] = "42394L3353" and (identifier "us.iamfatness.corevideopro.core" or identifier "us.iamfatness.corevideopro.vcam-probe")`
   (Developer ID CA intermediate, Developer ID Application leaf; tightened 2026-11-15).
   It fails closed, with no fallback. The check reads the producer from disk, and
   the `cmioextension` sandbox reads `/Library` but neither `/Applications` nor home
   folders. So producers RUN FROM `/Library/Application Support/CoreVideo Pro/`: the
   .pkg (the only macOS installer) installs a byte-identical copy of the core there,
   root-owned, and the shell launches that copy only while it matches the app's own
   (`CoreLocation`; otherwise it runs the app's copy and Diagnostics says the camera
   refuses it). The e2e script installs the signed probe in its `vcam-probe/`
   subfolder with sudo. **The extension carries NO file temporary-exception:** macOS
   26.3's `cmioextension.sb` expands `temporary-exception.files.absolute-path.read-only`
   to a bare `(allow filter)`, which does not compile, and the extension then dies in
   `_libsecinit_appsandbox` before main ("failed to compile sandbox profile"), relaunched
   every 10 s while `systemextensionsctl` still reads `[activated enabled]`. Installers
   3 and 4 (2026-11-15) shipped that exception and never registered a camera; ShellTests
   now fails on any `temporary-exception.files.*` key in the extension entitlements. The
   requirement stays the only gate. A refused producer surfaces in the core as `CAMERA_PRODUCER_REFUSED:`. Without it every producer was
   refused (OSStatus 100001). `CMIOExtensionClient` exposes no audit token, so the
   pid is the only handle. Any consumer may read the source stream.

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

Audio (virtual microphone). An Intel slice. macOS < 13. Zero-copy hand-off (render
straight into the sink's IOSurface pool): revisit only if the measured copies threaten the
60 fps per-frame requirement, as its own task with its own Windows-unchanged proof.
