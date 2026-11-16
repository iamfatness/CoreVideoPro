# macOS Sources rebuild, Phase 1 — outcomes

Tracking issue: #1215. Branch `mac-sources-phase1`, cut from `1a3c6a8d`
(the owner's installer 7), not from `mac-tier0b@e98436fc` as the plan says.
`1a3c6a8d` descends from `e98436fc` (14 commits ahead).

## Task 5 Step 0 — the UNVERIFIED items

**Not measured.** The plan asks for a live spike (camera, screen capture, fake
engine, one ISO, a dumped `/snapshot`). That was not possible in this
session: the owner's app was running with the virtual camera live, and the
session was barred from its control API and from starting a second core.
The answers below come from READING the native code at `1a3c6a8d`. Each one
is inference until an owner-run snapshot confirms it.

| # | Question | Code-read answer | Where |
|---|---|---|---|
| a | Do capture `sources[]` rows carry real `liveness`/`onAir`/`lossReason`? | Probably **not on the Mac**. The row reads `liveness` from `CaptureDeviceSource::onAirObservation()`, which stays empty while the frame's `captureLiveness.state` is `Unreported`. The only adapter found that writes `captureLiveness` is `ShmCapturePreparation.cpp` (the Windows SHM bridge); the AVFoundation and ScreenCaptureKit adapters do not. Expected on a Mac: `liveness: "unreported"`, `dropoutPolicyApplies: false`. | `SourceVideoIngress.cpp:123-131`, `CaptureDeviceSource.h:72-85` |
| b | Do ISO stream nodes carry per-writer `status` and `sourceId`, and in what format? | Yes, when the encoder fills `isoStreams`. `AVFoundationEncoderAdapter.mm:869-872` does. `sourceId` is `zoom:<pid>` / `capture:<id>` (`Interfaces.h:814`). The fallback branch (no per-writer telemetry) emits `participantId` only, with `status: "unreported"`, and its id format is whatever `isoIds` holds. `IsoWriterChip` keeps the bare→`zoom:` mapping: it only touches ids without a `:`, so it is a no-op for the normal path. | `CoreStateCapture.cpp:2001-2070` |
| c | Does a `zoom-source-format` event reach `AppModel.handleEvent`? | The path exists on every platform: `ZoomEngineRuntime::drainFrameEvents` → `MediaCore::drainZoomVideoFrameEvents` → `JsonRpcServer` writes it to stdout → `MediaCoreBridge` delivers any non-response object to `onEvent` → `handleEvent`. Before Task 5 the Mac's `handleEvent` dropped it in `default:`. | `ZoomEngineRuntime.cpp:728-733,1947-1966`, `JsonRpcServer.cpp:956,1483`, `MediaCoreBridge.swift:294` |
| d | Are Zoom `sources[]` rows keyed by the raw pid? | Yes, by design (source-bus slice 1: `ZoomParticipantSource` is keyed by the raw participant id, deliberately not `zoom:<pid>`). | CLAUDE.md "Slice 1" |
| e | Do capture `sources[]` rows carry the frame width and height? | Yes: `width`/`height` from the source descriptor, set from the latest frame and zeroed when video clears. | `SourceVideoIngress.cpp:103-104`, `CaptureDeviceSource.h:88-94` |

Consequences, per the plan's rule:
- (a): the capture dropout status line and its control will read as
  "does not report signal loss yet" on a Mac (Task 9 handles that copy). A
  native follow-up issue is needed: the AVFoundation/ScreenCaptureKit
  adapters should report `captureLiveness`. Not filed from this session.
- (c), (d): no stop condition. The Zoom half of Task 5 was built.

**Owner step:** with installer 8+ running in a test meeting, dump one
`GET /snapshot` with a camera, a screen capture and one ISO armed, and check
(a)-(e) against this table.
