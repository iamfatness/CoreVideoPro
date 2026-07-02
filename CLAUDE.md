# CLAUDE.md — working guide for CoreVideo Pro

Operational notes for working in this repo. Product/positioning lives in `README.md`
and `COREVIDEO_PRO_PRODUCT_SPEC.md`; this file is the "how to build, run, and not break
it" guide.

## North star (non-negotiable)

Low-latency **and** high-quality A/V is the entire product. It must beat vMix, Ecamm,
and mimoLive. All Zoom is 1080p and up to 60fps — never downgrade quality to dodge a
performance problem; fix the pipeline. CPU per-pixel work does not scale to 8 Zoom + 2
capture @1080p60 — the compositor path must stay on the GPU.

## What this app is

Three processes, not a web app:

- **WinUI 3 (.NET 9) shell** — `native-shell/CoreVideoPro.WinUI/` — the operator console
  (the product). It owns no real-time media; it sends commands and renders shared textures.
- **C++ media core** — `native/` → `corevideo-native.exe` — real-time pixels/PCM:
  D3D11 compositor, audio mixer, recorder, output senders.
- **Zoom engine subprocess** — `native/zoom-engine/` → `corevideo-zoom-engine.exe` —
  speaks the Zoom Meeting SDK, writes raw **I420** frames to shared memory.

IPC: JSON-line commands/snapshots over named pipes; video as keyed-mutex **DXGI shared
textures** (cross-process) for program/preview, and shared-memory I420 for Zoom frames.

## Build & run (Windows)

```powershell
npm run app                       # build best-available native core + build/launch WinUI
npm run app -- -StubOnly          # skip the native core rebuild, just (re)launch the shell
npm run app -- -Rebuild           # force a native rebuild
npm run build:native-dev          # build the dev native core (needs ZOOM_SDK_DIR set)
dotnet build native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj -c Release -p:Platform=x64
```

`ZOOM_SDK_DIR` must point at the staged Zoom SDK x64 dir for the full core. `npm run app`
runs `scripts/app.ps1`; the dev launcher is `scripts/run-studio.ps1` (now respects a
pre-set `COREVIDEO_ZOOM_ENGINE_PATH`).

Logs: `%LOCALAPPDATA%\CoreVideoPro\launch.log` (WinUI) and `media-core.log` (core).

## Testing multi-participant WITHOUT a real meeting (important)

There is a **synthetic Zoom engine**: `native/zoom-engine/fake/fake-engine.cpp` →
`corevideo-zoom-engine-fake.exe`. It emits N participants + animated I420 + roster/
active-speaker churn over the real IPC. Build with `npm run build:native-dev` (or
directly with `cl.exe` via vcvars64 — the CMake VS-generator build can time out).

To drive the full WinUI app headlessly:
1. Stop any instance; copy the fake exe over **all three** `corevideo-zoom-engine.exe`
   paths (`native/build-dev/`, `native/build-dev/Release/`, and the WinUI `…/publish/`).
   **Back each up as `*.realbak` and RESTORE afterward** — the real engine is ~201728
   bytes, the fake ~87040.
2. `npm run app -- -StubOnly`, then drive Join via UI Automation:
   nav Button Name `"Zoom"` → Edit Name `"Zoom meeting URL or ID"` (ValuePattern.SetValue)
   → Button Name `"Join Zoom"`.
Caveat: fake participants populate the roster but only become live GPU tiles when
assigned to Show Inputs — so per-tile crash repros need that assignment.

## The crash class you WILL hit: CoreMessagingXP 0xc000027b

`CoreMessagingXP.dll +0x93b66`, exception `0xc000027b`, **no managed stack** — a native
WinUI fail-fast that bypasses managed handlers (so no first-chance logger catches it).
It is a churn/reentrancy fail-fast, NOT (usually) off-thread access (instrumented: the
off-thread guards never fired). Confirmed and suspected triggers:

- **Frame-rate rebuilds of x:Bound collections.** `OnSurfacesChanged` fires ~100/s; its
  coalesce did not cap the *rate*, so `RefreshSurfaceBindings` ran ~98/s rebuilding
  `MultiviewTiles` wholesale. FIXED by throttling to ~12.5/s (`RsbMinIntervalMs`).
- **Per-tile GPU swap chains.** One DXGI swap chain per multiview participant tile,
  created/reloaded on roster/active-speaker churn, fail-fasts. FIXED by the
  **core-composited single-texture multiview** (`docs/gpu-multiview-plan.md`): the core
  renders the whole grid into ONE keyed-mutex shared texture
  (`D3D11CompositorAdapter::renderMultiview`), presented by a single
  `ShowMultiviewHost` surface with XAML overlays (labels/tally/meters/clock) that
  rebuild only on structural change. The old per-tile CPU grid has been deleted.
- **ComboBox ItemsSource churn.** The Sources editors were keyed on participant `Health`
  (toggles constantly), rebuilding 10 source dropdowns per tick → blank flicker + churn.
  FIXED by keying the signature on the participant id-set only.
- **Window resize (mitigated by design, not soak-verified).** A crash reproduced right
  after a programmatic window resize. `Direct3D11InteropService` now creates the swap
  chain at the **source** size and never calls `ResizeBuffers` on panel resize — it only
  re-applies a matrix scale (`ApplyPanelTransform`), so the resize-vs-present race cannot
  occur by construction. No dedicated regression soak has confirmed it closed; treat any
  resize-adjacent fail-fast as this until the alpha soak passes.

Rules of thumb: never replace a bound collection at frame rate (sync in place / diff);
keep one stable swap chain per surface (program, preview, one multiview);
present with **skip-present** (only on a new keyed-mutex frame) — smooth-present crashes
~31s in.

## Other gotchas

- The WinUI window often **opens minimized off-screen** (rect ≈ -32000,-32000). Restore
  gently with `ShowWindow(SW_RESTORE=9)`; do NOT aggressively maximize/move a
  SwapChainPanel window across monitors — it can kill the window (and resize can crash).
- 60fps needs `timeBeginPeriod(1)` (Windows 15.6ms timer granularity) + a frame-budget
  pace, both already in `JsonRpcServer.cpp`.
- I420→RGB is a GPU HLSL shader in `D3D11CompositorAdapter.cpp`
  (`kCompositorYuvPixelShader`, BT.709 full-range). Zoom frames carry I420
  (`hasI420()`), NOT BGRA — any frame merge/match must check `hasI420()` too or Zoom
  renders blank (see the `renderSyntheticTick` engine-roster merge).
- Audio/output no longer rides the render lock — **Phase 2 shipped**: a dedicated
  ~50Hz worker (`JsonRpcServer` `audioOutputThread`) runs
  `MediaCore::renderAudioOutputTick` with a strict two-lock discipline: `coreMutex`
  briefly for gather/publish, `audioOutputMutex_` for the long DSP/device/network span,
  NEVER both nested on the worker. The render thread is video-only
  (`renderDisplayTick`), and an empty `media-core-sync` poll returns the published
  snapshot without a tick. When touching audio/output control-plane commands, keep the
  `coreMutex` → `audioOutputMutex_` lock ORDER (see `docs/phase2-threading-plan.md`);
  a single missed `audioOutputMutex_` guard is a data race.

## Current state (2026-07-02)

Working: Zoom video stable under multi-participant churn; program-zoom on the GPU I420
path (zero-copy ingest + 60fps pacer); **GPU core-composited multiview** live (single
shared texture, 4 layout modes, overlay labels/tally/meters/clock, multi-layer PREVIEW
composite bus); **Phase 2 audio/output worker decouple** live (increments 1, 2, 4, 5);
routing honored by Sources + multiview; compact Sources routing table.

In progress / next: (1) the **alpha validation pass** on the Windows rig — every
checkbox in `docs/alpha-plan.md` Tracks A–F is still unchecked, including the ≥10-min
audio-glitch-freedom soak that Phase 2 shipped without, plus a smoke of the new
DirectWrite/WIC overlay raster (code-complete, dev-gated, never run on Windows);
(2) **real device capture** (UVC first, then DeckLink/AJA); (3) Phase 2 leftovers:
engine `sendLine` still blocks under `coreMutex` (increment 3) and the sub-ms-hold
guardrails (increment 6; TSan now runs in CI). Overlay/lower-third/caption
rasterization shipped: shared content tile (`OverlayTileRaster`, full-ASCII font,
signature-cached) blitted by the CPU preview and uploaded by the D3D11 compositor,
DirectWrite/D2D + WIC upgrade behind the D3D11 gate.
