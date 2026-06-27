# GPU multiview tiles — implementation plan

_Status: 2026-06-27. Program is now sharp 1080p ~60fps via the GPU shared texture;
the multiview tiles are still low-fps because they render per-participant **base64
thumbnails decoded on the UI thread** (a `SoftwareBitmap` alloc + `CopyFromBuffer`
per tile per frame, behind a drop-oldest queue). No cheap win — the per-tile
throttle is already 16ms (60fps). The fix is to give each tile a GPU shared
texture, exactly like the program. Validated by parallel native + WinUI agent
investigations._

## Native (export one keyed-mutex shared texture per participant)

- Per-participant BGRA frames are available in `ZoomEngineRuntime::latestDecodedVideoFrames()`
  (`native/src/modules/ZoomEngineRuntime.cpp`) — already polled each tick by
  `MediaCore::renderSyntheticTick()`. Format BGRA, full-res, with participantId.
- Add per-participant shared textures in the **D3D11 compositor**
  (`D3D11CompositorAdapter.cpp`) — it already has the device/context and uploads each
  participant's frame for compositing, and runs single-threaded on the render
  thread (so no extra device-context threading). Keep a
  `map<participantId, {ID3D11Texture2D (MISC_SHARED_KEYEDMUTEX), IDXGIKeyedMutex,
  HANDLE, w, h, lastFrameId}>`. Per frame: ensure/resize the texture, AcquireSync(0,0)
  non-blocking → upload BGRA (UpdateSubresource or Map) → ReleaseSync(1); skip if the
  consumer holds it. Drop textures for participants no longer present.
  - Consider capping the per-tile texture size (e.g. 640x360) — full 1080p × N is
    wasteful for small tiles.
- `Interfaces.h`: add `struct ParticipantSharedTextureInfo { participantId,
  sharedHandleHex, width, height, format, frameNumber }`; add
  `std::vector<ParticipantSharedTextureInfo> participantSharedTextures` to `ProgramFrame`.
- `MediaCore`: drain/emit a `participant-shared-texture` event per participant
  (mirror `enqueueProgramSharedTextureEvent` / `drainProgramSharedTextureEvents`);
  pump it in `JsonRpcServer` alongside the program one (low-rate, not the drop-oldest
  frame queue).

## WinUI (present each tile from its handle — reuses the program path)

- `MediaCore` C#: `ParticipantSharedTexture` model + `CoreProtocolParser
  .TryParseParticipantSharedTextureEvent`; `MediaCoreSupervisor` parses it and raises
  `ParticipantSharedTextureReceived` (direct invoke — low rate); `MediaCoreBridgeService`
  forwards it.
- `StudioViewModel`: subscribe with a UI-thread-marshalled handler →
  `_surfaces.OnParticipantSharedTexture(texture)`.
- `VideoSurfaceCoordinator`: store `Dictionary<participantId, SharedTextureHandle>`;
  in `OnParticipantSharedTexture` update the matching `_participantSurfaces[key]` via
  `WithSharedHandle` and only `NotifyChanged()` when the handle value changes (avoid
  per-frame binding churn); in `BuildMultiviewTiles` apply the stored handle to each
  tile's `Surface`.
- `VideoSurfaceHost` (the key gate change): today GPU present is gated on
  `IsProgramSurface` (OnLoaded swap-chain attach + TryPresentPendingSharedHandle +
  CompositionTarget.Rendering hook). Change so **any** surface with a valid
  `PendingSharedHandle` attaches its SwapChainPanel and presents on vsync. Each
  `VideoSurfaceHost` already owns its own `Direct3D11InteropService` + SwapChainPanel,
  so N tiles = N independent swap chains — no Direct3D11InteropService change needed
  (it already caches one shared handle + keyed mutex per instance).

## Risks
- N per-frame GPU uploads + N swap chains + N CompositionTarget.Rendering presents.
  Cap tile texture size; only `NotifyChanged` on handle change; reuse textures.
- Tile lifecycle: participants join/leave → create/dispose textures (native) and the
  tile `VideoSurfaceHost` unload disposes its interop. Same control is used by the
  Studio multiview and the Sources/Inputs multiview.
- Same keyed-mutex + present discipline that the program path needed (always present
  every vsync, re-presenting last on no-new-frame, for smooth pacing).

## Verify
- Per-tile `d3d: present` heartbeats; multiview visibly smooth; app stays stable
  (watch for the off-thread-binding crash class — keep all VM-bound updates marshalled).
