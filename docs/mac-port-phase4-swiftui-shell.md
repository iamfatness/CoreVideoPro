# macOS port Phase 4 — SwiftUI shell (M1)

Status: M1 in progress (2026-08-03). Decision record: the owner chose a
native SwiftUI shell over Avalonia (performance headroom + platform
alignment outrank C# reuse; the CoreMediaIO camera extension forces Swift
into the project anyway) and over reviving the React renderer (its operator
surface froze ~2026-06-20, a full alpha-war behind the WinUI shell). The
freshly-extracted coordinators (Transport/ShowInputs/MagicScene) + their
characterization tests serve as SPECS for Swift ports, not shared code. The
protocol layer is language-neutral JSON, live-verified through Phase 2/3.

## M1 deliverable

`mac-shell/` SwiftPM package -> `CoreVideo Pro.app` (bundle script pattern
from the engine; ad-hoc signed) that supervises the REAL media core and
proves the architecture end to end: IOSurface program monitor, Zoom join +
roster + assign, record to disk, audio meters + monitor, loud warnings.

## Wire contract (verified against the core, 2026-08-03)

- Spawn `corevideo-native` (build-metal config) with stdio pipes. Env:
  `COREVIDEO_ZOOM_ENGINE_PATH` -> the bundled
  `corevideo-zoom-engine.app/Contents/MacOS/corevideo-zoom-engine`,
  `COREVIDEO_ZOOM_PUBLIC_APP_KEY` (the operator's key, from settings; the
  OBS plugin's key lives in `[ZoomPlugin]` of OBS global.ini).
- stdout line 1: `{"id":"handshake","ok":true,"profile":{...,"renderer":"metal",
  "capabilities":[...]}}`.
- Requests are JSON lines `{"id":"<n>","type":"<t>",...}`; responses echo the
  id (`ok:false` + `error` on failure). Interleaved EVENT lines have `type`
  but no known id — discriminate by pending-id match:
  - `program-frame-preview` (~30fps, base64 320x180 + signatures + embedded
    `sharedTexture{iosurfaceId,width,height,frameNumber}`)
  - `program-shared-texture` (per render; tiny)
  - `multiview-shared-texture`, `preview-shared-texture` (structural)
  - zoom engine debug/roster events
- Bridge request types used in M1:
  - `zoom-join` `{payload:{meetingId,passcode,displayName,...}}` (fields per
    `applyJoinCredentialsFromPayload`/`meetingIdFromJoinPayload`),
    `zoom-leave`, `zoom-snapshot`, `zoom-stop-capture`
  - `zoom-media-spine-sync` `{spinePayload:{participants:[...],...},elapsedMs}`
    — the assign path (participants with sourceUuid/participantId/isolateAudio;
    exact shape mirrored from `src/engine/zoomMediaSpineSync.ts`)
  - `media-core-sync` `{elapsedMs,commands:[...]}` -> `{snapshot}` — sync
    commands: `start-program-output {destinations:["recording",...]}`,
    `set-recording-targets`, `start-recording-session`,
    `stop-recording-session`, `sync-audio-monitor
    {enabled,deviceId,deviceName,volume}`
  - `list-capture-devices`, `connect-capture-device {deviceId,outputSourceId}`
- Snapshot fields M1 reads: `recording{status,warning,artifactPath,...}`,
  `audioMixSession{masterLevel,participants[],monitorStatus,monitorUnderruns}`,
  `captureDevices[]`, `zoom` (roster/meetingState/rawMediaActive),
  `programFramePreview.sharedTexture.iosurfaceId`.

## Presentation (the load-bearing component)

`ProgramMonitorView`: `NSViewRepresentable` -> NSView with a plain CALayer;
on each snapshot/event with a new `iosurfaceId`, `IOSurfaceLookup` once and
set `layer.contents = ioSurface` (CALayer accepts IOSurfaceRef directly);
per-frame updates need only `layer.setContentsChanged()` semantics — the
surface is the compositor's live render target, so re-assigning contents on
frameNumber change suffices at 60fps with zero copies. Aspect-fit via layer
contentsGravity. The SAME view serves multiview/preview surfaces later.

## Structure

- `mac-shell/Package.swift` — executable target `CoreVideoProShell`
  (macOS 13+, SwiftUI + AppKit + IOSurface).
- `Sources/CoreVideoProShell/MediaCoreBridge.swift` — Process spawn, line
  codec, async request/response with 4s timeouts, event pub, supervision
  (relaunch on exit with backoff, status surfaced).
- `AppModel.swift` — ObservableObject: connection state, snapshot cache,
  roster, recording state, warnings ring; 10Hz media-core-sync tick +
  zoom-snapshot poll; command builders.
- `Views/` — RootView (status bar + panes), ProgramMonitorView, ZoomPane
  (join form, roster list with Assign/Unassign), TransportPane (record,
  capture, engine), AudioPane (master meter, monitor toggle+volume),
  WarningsPane.
- `scripts/make-macos-shell-bundle.sh` — .app skeleton, Info.plist with
  camera/mic/screen usage strings, copies corevideo-native + the engine
  bundle into Contents/Resources, ad-hoc sign.
- `scripts/run-mac-shell.sh` — build native (metal config) + engine bundle +
  swift build + bundle + open.

## Verified autonomously (before the owner returns)

1. handshake + profile (renderer "metal") + 10Hz sync loop stable.
2. Program monitor shows REAL compositor pixels without a meeting: a
   still-media route (`load-scene-graph` with a media asset pointing at a
   generated PNG) renders through Metal -> IOSurface -> CALayer.
3. `start-recording-session` produces a playable MP4 (AVF encoder).
4. zoom-join with a bogus meeting surfaces the engine's join_failed error
   in the warnings pane (auth path exercised end to end).

Owner test: real meeting join + admit, assign participants, record, monitor.
