# 1080p60 Zoom video — GPU shared-texture display fix plan

_Status: 2026-06-27. The operator program view is low-res and unstable because it
renders the **base64 CPU preview**, not the **GPU shared texture**. The native
core already produces a correct 1080p GPU frame; the WinUI side never presents it._

## What is already proven working (do NOT re-investigate)

Driving `corevideo-native.exe` headlessly with `set-output-profile` (1920x1080) +
`start-program-output`, the snapshot reports:

- `compositorRenderer: "d3d11"`, `gpuComposed: true`
- `sharedTexture.sharedHandleHex: 0x80003602` (a real, non-stub handle)
- health: `"GPU compositor active: d3d11"`, `"Hardware encoder active: media-foundation"`

So the **D3D11 compositor composites at 1080p on the GPU and exports a shared
texture every rendered frame** (`D3D11CompositorAdapter::exportSharedTexture`,
`MediaCore::enqueueProgramSharedTextureEvent`). The core is NOT the problem.

## The problem

The WinUI **never presents** the shared texture. With present-path logging added,
there are **zero** `d3d: presented shared handle`, `d3d: present skip`,
`d3d: present FAILED`, or `d3d: swap-chain attached` lines in current sessions
(the only ones are from 2026-06-19). So `Direct3D11InteropService.TryPresentSharedTexture`
and `TryAttachSwapChainPanel` are not running — the program canvas falls back to
the base64 `programFramePreview` path (downscaled + throttled + UI-thread decode →
low res + jumpy/freezing, see `preview: failed to update BGRA preview ...
TaskCanceledException`).

## The chain to verify (in order) — find the first broken link

1. **Core emits it.** `MediaCore::enqueueProgramSharedTextureEvent`
   (`native/src/core/MediaCore.cpp`, throttled ~33ms alongside the base64 preview).
   Confirm the `program-shared-texture` JSON line is actually written to stdout
   when program output is active. (Likely fine — handle is produced.)
2. **C# reads + parses it.** `MediaCoreSupervisor.ReadStdoutLoopAsync` →
   `CoreProtocolParser.TryParseProgramSharedTextureEvent` →
   `ProgramSharedTextureReceived?.Invoke(...)`.
   ⚠️ **TOP SUSPECT:** the frame-event handlers were offloaded to a *bounded
   drop-oldest* queue (`_frameDispatch`, capacity 8) in `MediaCoreSupervisor`.
   If frame/preview events flood that queue, the **shared-texture event can be
   dropped** before its handler runs. The shared-texture handle must NOT be
   subject to drop-oldest — route it on its own (or unbounded/coalesce-latest)
   path, separate from the high-rate frame events.
3. **Coordinator forwards it.** `VideoSurfaceCoordinator.OnProgramSharedTexture`
   → `ApplyProgramSharedTexture` → `_programSurface.WithSharedHandle(handle)`
   (`Services/VideoSurfaceCoordinator.cs:247,331`).
4. **Host receives + presents it.** `VideoSurfaceHost` (program instance) →
   `OnSurfaceStateChanged` → `TryPresentPendingSharedHandle` → `PresentSharedHandle`
   → `Direct3D11InteropService.TryPresentSharedTexture`
   (`Controls/VideoSurfaceHost.xaml.cs:218,305`; `Services/Direct3D11InteropService.cs:82`).
   ⚠️ **SUSPECT:** is the program canvas in `Views/StudioWorkspace.xaml` actually a
   `VideoSurfaceHost` (with the `SwapChainPanel`), or is it a plain `Image` bound to
   `PreviewBgra`? If the program is shown via `Image`, the GPU path is never used —
   that alone explains everything. CHECK THIS FIRST in the XAML.
5. **SwapChainPanel attaches.** `VideoSurfaceHost` OnLoaded →
   `Direct3D11InteropService.TryAttachSwapChainPanel` ("d3d: swap-chain attached").
   If this never logs, the panel/host for the program isn't being loaded.

## Instrumentation (one build pins the break)

Add `LaunchLog.Write` at: core emit (already → `media-core.log`); `MediaCoreSupervisor`
when a `program-shared-texture` line is parsed and when its handler is dispatched
vs dropped; `VideoSurfaceCoordinator.OnProgramSharedTexture` (handle arrival);
`VideoSurfaceHost` OnLoaded + PresentSharedHandle entry. Present-failure logging in
`TryPresentSharedTexture` is already added. Run with program output ACTIVE (Engine
On; **no Zoom needed** — the compositor renders a slate/SuperSource regardless) and
read the `d3d:`/handle lines to see the first missing step.

## Likely fixes by link

- **(2) dropped event:** give the shared-texture handle a dedicated latest-wins slot
  (not the drop-oldest frame queue), or raise priority so it always dispatches.
- **(4) program uses Image, not VideoSurfaceHost:** point the program canvas at the
  `VideoSurfaceHost` GPU surface; keep the base64 `Image` only as the fallback the
  host already manages (`PreviewImage` collapses when GPU present succeeds).
- **(5) attach never runs:** ensure the program `VideoSurfaceHost` is loaded/visible
  and `TryAttachSwapChainPanel` runs once the panel template is realized.
- **cross-process handle:** the compositor uses a legacy `D3D11_RESOURCE_MISC_SHARED`
  + `GetSharedHandle` (worked 2026-06-19). If `OpenSharedResource` now throws
  (`d3d: present FAILED`), switch to NT handles
  (`D3D11_RESOURCE_MISC_SHARED_NTHANDLE` + `IDXGIResource1.CreateSharedHandle`,
  opened via `OpenSharedResource1`). One-time invalidation is permanent
  (`_invalidHandles`), so a single early failure wedges it on base64 — also make
  invalidation retry-friendly.

## 60fps cadence (after the GPU path presents)

- Emit the shared-texture handle every render (decoupled from the base64 preview,
  which can stay ~10fps as a fallback) — the handle is tiny, so 60fps is cheap.
- Ensure the compositor renders at 60fps: the catch-up cap in
  `MediaCore::applyCommands` (currently 2) + the host sync cadence must allow it.

## Verification

Look for continuous `d3d: presented shared handle 0x… 1920x1080` lines and
`IsGpuPathActive == true`; the program canvas should be sharp and smooth with the
`PreviewImage` (base64) collapsed. No Zoom required to verify.
