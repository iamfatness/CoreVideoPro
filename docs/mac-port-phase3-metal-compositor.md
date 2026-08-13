# macOS port Phase 3 — Metal compositor

Status: M1 merged (#337); M2 merged WITH the IOSurface export originally
scoped as M3 — `renderMultiview`/`renderPreview` return a shared texture, so
landing them without a real cross-process identifier would have returned a
lie; IOSurface-backed targets made the honest version barely more work. The
wire contract gained `ProgramFrameSharedTexture::iosurfaceId` (platform-
exclusive sibling of `sharedHandleHex`, emitted in the shared-texture JSON
when nonzero; Windows shapes unchanged). Remaining: M4 (full-res encoder
handoff → VideoToolbox spec). Prior phases: native core green on Apple
Silicon (#334), macOS Zoom engine live-verified (#335/#336).

## Scope and non-goals

Phase 3 replaces the Windows-only GPU pieces of the media core with macOS
twins, compositor first because it gates everything visual. This spec covers
the **Metal compositor adapter**. VideoToolbox (encoder), CoreAudio
(capture/monitor), ScreenCaptureKit/AVFoundation (capture), and the
CoreMediaIO camera extension are separate follow-ups.

Non-goals for M1: no shell presentation (no IOSurface export — Phase 4), no
vcam NV12 tap (rides the camera-extension work), no CoreText overlay raster
(the portable CPU raster is uploaded as a texture instead).

## What the boundary map established (2026-08-03)

- `ICompositor` requires only `rendererName()` + `render()`;
  `renderMultiview` / `renderPreview` / `takeVcamNv12` / `sourceTexStats`
  have safe defaults — a Metal adapter lands incrementally.
- `render()` must fill `ProgramFrame{width, height, layerCount, frameNumber,
  renderPlanId, renderer, gpuComposed, health, warnings, renderPlanSignature,
  programPixelSignature, preview}`. `preview` is tightly packed top-down BGRA
  capped 320x180 — if left empty, MediaCore back-fills with the CPU
  synthetic path and every consumer degrades.
- Factory seam: `createDefaultModules` (StubModules.cpp) upgrades
  `ModuleSet::compositor` when the GPU factory returns non-null. A
  `createMetalCompositor()` mirrors `createD3D11Compositor()`.
- Fully portable, reuse as-is: `compositor/CompositorLayout.h` (framing UV
  math), `modules/OverlayTileRaster.h` (overlay tile geometry + CPU raster),
  `modules/ProgramFramePreview.{h,cpp}` (preview helpers + downscale),
  `modules/ImageResize.h`, plan validation + deterministic sort
  (`validateRenderPlan` / `sortCompositorRenderPlan`), and all plan structs.
- Windows-only, needs Metal twins: `CompositorShaders.h` (6 small HLSL
  shaders), `CompositorOverlayRaster` (DirectWrite/D2D), `ComPtrLite.h`
  (ObjC ARC instead), and the keyed-mutex DXGI export cluster (IOSurface,
  Phase 4).
- Invariants that carry over verbatim: borders never composite into
  program/preview (owner rule 2026-07-31; the plan builder already forces
  `borderStyle="none"`), multiview PGM/PVW cells clip their layers,
  deterministic layer sort before drawing, unmatched capture layers warn
  loudly (rate-limited) instead of silently rendering pink.

## Design

### Gate and factory

`COREVIDEO_WITH_METAL` CMake option, requiring `COREVIDEO_ENABLE_DEV_ADAPTERS`
and APPLE (mirror of the D3D11 gate, which FATAL_ERRORs off Windows).
`MetalCompositorAdapter.mm` compiles under
`!COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL`;
otherwise `createMetalCompositor()` returns nullptr, exactly like the D3D11
null factory. `createDefaultModules` tries D3D11 then Metal (each is
non-null on at most one platform).

Stub-vs-real: `COREVIDEO_STUB=ON` stays the default everywhere (CI parity);
the Metal build is a dev/CI-explicit configuration like the Windows dev
build.

### Shaders

Runtime-compiled MSL via `newLibraryWithSource:` (the Metal equivalent of
the D3DCompile-at-startup approach — no offline metallib step, same
fail-loud-at-init semantics). One vertex function (fullscreen triangle from
vertex_id, the `(uv << 1) & 2` trick) and pixel functions ported 1:1 from
`CompositorShaders.h`:

- solid color + grade
- textured BGRA + grade (framing UV window via uvScale/uvOffset)
- overlay (premultiplied alpha, scale all four channels by layer alpha,
  premultiplied blend state)
- I420 (three R8 textures, parameterized BT.709/BT.601 + full/studio range
  via the same `YuvShaderParams` numbers — the struct itself is portable and
  moves to a shared header)

The color-grade math (contrast/exposure around 0.5, Rec.601 luma saturation
lerp, temperature ±0.05 r/b) must be byte-identical in expression order so
GPU output matches the D3D11 adapter and the CPU preview's expectations.
The vcam NV12 shader pair is NOT ported in M1 (no vcam tap on mac yet).

### Adapter structure (M1)

`native/src/compositor/MetalCompositorAdapter.mm` — one class implementing
`ICompositor`, mirroring the D3D11 adapter's shape:

- Device/queue/pipelines built lazily on first render; init failure →
  loud warning in the ProgramFrame + `health:"degraded"`, never a crash.
- Per-source texture cache keyed by sourceId (BGRA `MTLTexture` or Y/U/V R8
  triple), same one-upload-per-frame accounting surfaced through
  `sourceTexStats()` (the existing counters struct), same ~300-frame
  eviction.
- Program target: BGRA8 `MTLTexture` with `storageModeShared` on Apple
  Silicon — CPU readback is a plain `getBytes:`, no staging dance, so
  `skipCpuReadback` gates work but cost far less than on D3D11.
- Layer draw: viewport from rect (letterbox handled by content rect from
  `resolveSourceFraming`), scissor from clipRect, constants struct identical
  to `LayerShaderConstants`.
- Overlay layers: reuse the portable CPU raster
  (`OverlayTileRaster` / `drawOverlayContentBgra`) into a BGRA buffer,
  upload as a texture, draw with the premultiplied overlay pipeline.
  Content-signature cache like the D3D11 D2D raster cache. CoreText twin is
  a later increment if raster cost shows up.
- Readbacks fill `preview` (via the shared `downscaleBgraNearestNeighbor`)
  and `programPixelSignature` (shared helper), honoring `skipCpuReadback`
  and `fullProgramReadback` (fills `programFullBgra` — which also positions
  VideoToolbox to receive full-res program later, resolving the
  320x180-into-1080p encoder oddity the boundary map flagged).

### Tests

- The 17 stub-mode compositor tests and all pure-logic suites already run on
  macOS CI and are unaffected.
- New `MetalCompositorTest.cpp`, gated on `COREVIDEO_WITH_METAL`:
  - **Skip, don't assert**: if `MTLCreateSystemDefaultDevice()` returns nil,
    `GTEST_SKIP()` — a GPU-less runner must not brick CI (deliberate
    divergence from the D3D11 tests' hard `ASSERT_NE`).
  - Parity oracle: composite representative plans (solid layers, BGRA
    frame, I420 frame full/studio range, overlay, opacity, clip, grade) and
    compare the Metal `preview` against `fillSyntheticProgramFramePreview`
    output — geometry/signature equality where the CPU path blits real
    pixels, tolerance-based channel comparison for the YUV convert.
  - Cache accounting test mirroring the D3D11 source-texture-cache suite.
- `GpuCompositorAdapter.FactoryIsDisabledUnlessD3D11GateIsEnabled` gains a
  Metal sibling; the existing assertion stays valid (D3D11 factory is still
  null on mac).
- CI: extend `native-stub-macos` with a second configure+build of the Metal
  configuration; run the Metal tests (they self-skip if the runner has no
  usable device — macos-latest runners expose an Apple paravirtual Metal
  device, so they are expected to actually run).

### Increments

- **M1 (this PR series):** gate + factory + adapter with program `render()`
  at full parity (solid/BGRA/I420/overlay layers, grade, framing, clip,
  opacity, preview + signatures, source-tex cache) + tests + CI.
- **M2:** `renderMultiview` + `renderPreview` into their own targets (same
  passes, borders per multiview plan, PGM/PVW clip), tiles metadata already
  portable.
- **M3 (start of Phase 4 handshake):** IOSurface-backed program/multiview/
  preview export + a `ProgramFrameSharedTexture` sibling field
  (`iosurfaceId`) and its serialization, consumed by whatever shell hosts
  the mac operator UI.
- **M4:** full-res encoder handoff (`programFullBgra` → VideoToolbox spec).

## Verification bar for M1

Metal test suite green on this Mac (arm64) and on `macos-latest` CI; stub
suite untouched on all three platforms; Windows dev build unaffected
(no shared-file edits except additive portable-helper moves).
