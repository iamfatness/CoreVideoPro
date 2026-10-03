# The shell refuses a core that cannot do what it claims (#762, #741)

A real core always starts from the stub module set and swaps in each real adapter that
constructs (`native/src/modules/ModuleComposition.cpp`, `createDefaultModules`). When one
does not, the stub stays, and until 2026-10-02 nothing said so:

- D3D11 device creation fails → `CpuNoopCompositor` reports `live` frames and draws nothing.
  This is the 2026-09-10 "software Stub core looked healthy" incident.
- `MFStartup` fails → `StubRecordingEncoderSink` ("software-counting") reports a recording
  with a growing byte count and writes no file.
- `COREVIDEO_ZOOM_ENGINE_PATH` unset (engine exe missing or quarantined while `sdk.dll` is
  present) → the core "joined" a two-person stub meeting (`operator-1`, `guest-1`).

The core recorded each of these in `profile().capabilityStates`; the shell's
`NativeMediaCoreProfileValidator` had no caller.

## What holds now

Core side:
- `createDefaultModules` records `program-recording` / `iso-recording` as
  `failed-to-construct` (`encoder-adapter-did-not-start`) when the encoder adapter was built
  but did not construct, and sets `ModuleSet::permitStubZoomSession = false` in a non-stub
  build.
- `start-recording-session` on a core with a failed encoder publishes `recording.status =
  failed` with the reason instead of starting the counting stub.
- `zoom-join` on a core that does not permit the stub meeting and has no engine returns
  `ok:false`, code `zoom-engine-not-configured`; readiness reads `status: blocked`,
  `mode: no-engine`. `simulate-breakout-room-change` is compiled only in the stub tier.

Shell side, `MediaCoreOnAirPolicy` (CoreVideoPro.MediaCore/Services): three questions
asked of `IMediaCoreBridge.Profile`, each answering null or the sentence the operator sees.
- `EngineBlockReason`: software renderer or `gpu-compositor` not available → Engine stays
  off (`TransportCoordinator.ToggleEngineAsync`).
- `RecordBlockReason`: the Engine reason, or `program-recording` `failed-to-construct` →
  Record refused before the disk pre-flight (`SetRecordingAsync`).
- `JoinBlockReason`: `zoom-raw-video` not available → Join refused after the SDK readiness
  gate (`SettingsViewModel`).

"omitted" is not a refusal: it is the stub tier's word for "never built", and the stub tier
records on purpose. Only `failed-to-construct` and a software renderer block. A profile with
no `capabilityStates` (an older core) does not block Join.

Tests: `MediaCoreOnAirPolicyTests` (shell), `JsonRpcServer.ARealCoreWithNoZoomEngineRefusesJoin`
and `ARealCoreIgnoresTheBreakoutSimulator` (core, non-stub build only). The stub-session
tests construct `MediaCore(createStubModules())` on purpose.
