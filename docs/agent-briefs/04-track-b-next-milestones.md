# Track B — Next Milestones (Agent B / Codex): Real Native Implementations

**Status baseline (already built — do not redo):** `native/` has a C++20 core
(`CMakeLists.txt` with `COREVIDEO_STUB` default ON and
`COREVIDEO_ENABLE_DEV_ADAPTERS` default OFF), `src/main.cpp` JSON-RPC stdio loop,
`src/rpc/JsonRpcServer`, `src/core/MediaCore`, `src/modules/Interfaces.h`
(`ZoomFrameSource`, `Compositor`, `Encoder`, `OutputSender`, `RecordingSink`),
`StubModules.cpp`, a gated `PlatformAdapters.cpp`, and tests incl.
`ContractParityTest`. A parallel Node `native-core/` service mirrors the same
loop for in-container runtime. The stubs work end-to-end.

**Theme:** implement the real adapters behind the existing interfaces, gated by
`COREVIDEO_ENABLE_DEV_ADAPTERS` so the default in-container build stays
stub-only and green. Order by MVP capability gates.

- **B1 — Zoom media spine handler.** Implement the `zoom-media-spine-sync`
  request (added by Agent A in `native shell protocol`) in both the C++ core
  and the Node `native-core/src/zoomMediaSpine.ts` runtime: consume
  `ZoomMediaSpineSyncPayload`, return `ZoomMediaSpineNativeSnapshot`. Stub path
  returns synthetic; dev path calls the real SDK. *Gate:* parity test covers the
  new request; stub round-trips with Agent A's supervisor.
- **B2 — Zoom Meeting SDK adapter (Windows first).** Behind
  `COREVIDEO_ENABLE_DEV_ADAPTERS` + a new `COREVIDEO_WITH_ZOOM`, implement
  `ZoomFrameSource`: join meeting, subscribe raw video/audio/screen-share,
  roster + active-speaker, recording proof — matching the readiness contract in
  `src/engine/zoomSdkReadiness.ts` and file layout in
  `src/engine/zoomWindowsSdkPackage.ts`. *Gate:* compiles+links only where the
  SDK is present; announces `zoom-raw-video`/`zoom-raw-audio` in the profile.
- **B3 — GPU compositor adapter.** Implement `Compositor` consuming the
  `RenderPlan` (`src/engine/nativeMediaCoreRenderPlan.ts`): D3D11 on Windows,
  Metal on macOS. Announce `gpu-compositor`/`scene-graph-rendering`/
  `dynamic-overlays`/`chroma-key`/`smart-framing` when active. *Gate:* renders
  the scene graph for the integration take on a dev GPU.
- **B4 — Hardware encoder + recording.** Implement `Encoder` (NVENC / Quick Sync
  / VideoToolbox) and `RecordingSink` (real MP4/MOV writes) driven by the
  encoder-session + recording-session commands in `nativeMediaCoreCommands.ts`.
  Announce `program-recording`/`iso-recording`. *Gate:* records a real 1080p MP4
  on a dev machine.
- **B5 — Output senders.** Implement `OutputSender` for RTMP first (libavformat),
  then the Phase-2 protocols (SRT via libsrt, WebRTC, NDI) matching
  `src/engine/{srtOutput,webrtcOutput,ndiOutput}.ts`. Announce the matching
  `*-output` capability only when built. *Gate:* RTMP push to a test ingest.
- **B6 — Capture devices.** Implement `ICaptureDevice`/capture path for
  Blackmagic DeckLink + AJA NTV2 behind their own flags, matching
  `src/engine/captureDevices.ts`. *Gate:* enumerates a real device on a dev rig.

**Ownership:** Agent B never edits the TS protocol files — mirror them in
`native/src/core/Protocol.h` / `native-core/src/protocol.ts`. Keep
`ContractParityTest` green and extend it for every new request/response type.

**Verification:** `cmake -S native -B native/build -DCOREVIDEO_STUB=ON &&
cmake --build native/build && ctest --test-dir native/build` green in-container;
`npm run test:native-core` green. Dev-adapter builds validated on Mac/Windows.
</content>
