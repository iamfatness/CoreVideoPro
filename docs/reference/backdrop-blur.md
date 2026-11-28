# True backdrop blur ("glass", P9)

Spec: `docs/reference/graphics-styles-color-lower-thirds-spec.md` section 6.1, row P9. Owner ruling
2026-11-27: true backdrop blur is required on Windows and Mac, with no frosted fallback, and it must hold
the outputs release gate (zero dropped frames at 1080p60 with two pack graphics plus a crawl on air).

## What it is

A template `rect` may carry `backdrop: {blur, tint}`:

- `blur`: the Gaussian radius in px at 1080p (template units at placement scale 1). Required, 1 to 64.
  The pass uses sigma = blur / 2, scaled with the graphic's on-screen size.
- `tint`: `#RRGGBB` / `#RRGGBBAA`, or a colour field or brand token, like every colour. Optional
  (default fully transparent). It is mixed over the blurred picture by its alpha.
- At most **four glass plates per template** (`template-backdrop-limit`). This is a frame-budget bound.
- Only on a rectangular `rect` (the plate), optionally rounded. A polygon or skewed plate (P3) is refused
  (`template-backdrop-shape`), because the compositor masks glass to a rounded rectangle and would blur
  outside the outline. Anywhere else the key is refused like any unknown property.

The overlay raster draws the plate's fill as usual and never sees the backdrop. The compositor blurs the
Program pixels already composited beneath the plate, tints them, masks them to the plate's rounded
rectangle, and then draws the graphic over them. Fill opacity, the plate's node opacity, layer opacity,
key/motion opacity, motion transforms and clips all apply to the glass exactly as they apply to the plate.

## Where the code is

| Piece | File |
|---|---|
| Schema, limits, plate collection (`collectBackdrops`) | `native/src/graphics/GraphicsTemplate.{h,cpp}` |
| Kernel plan, fixed-grid region, per-frame geometry, constant layout, **CPU reference** | `native/src/compositor/BackdropBlur.h` (portable, shared) |
| Windows pass (HLSL) | `native/src/compositor/D3D11BackdropBlur.{h,cpp}`; wired in `D3D11CompositorAdapter::drawBackdrops` |
| Mac pass (MSL) | `native/src/compositor/MetalBackdropBlur.{h,mm}`; wired in `MetalCompositorAdapter::drawBackdrops` |
| Plates travel with the prepared raster | `CompositorOverlayRaster::activeBackdrops`, `CoreTextOverlayRaster::activeBackdrops` |
| Designer (Windows) | `GraphicsTemplateEditor.SetBackdrop`, "Glass" expander in `GraphicsDesignerControl.Content.cs` |

## The pass (identical stages on both GPUs)

1. **Downsample** the region by d (a power of two, at least 2) with an exact box filter into a half-float
   plane. d is the smallest factor that keeps sigma/d at or below 4 low-resolution px.
2. **Horizontal**, then 3. **vertical** Gaussian: 2r+1 discrete taps (r at most 12), normalized weights
   from `backdropKernel`, reads clamped to the region.
4. **Composite** into the bound target: manual bilinear upsample, tint, rounded-rectangle coverage
   (signed distance, 1 px AA), straight-alpha blend, scissored to plate ∩ clip.

The region is aligned to a grid fixed to the frame (multiples of d), not to the plate, so a moving plate over
a still picture does not shimmer. The low-resolution sigma subtracts the variance the box downsample and the
bilinear upsample already add.

Windows copies the region out of the bound target (`CopySubresourceRegion`) and reads the copy, so the pass
works on any BGRA8 target without an SRV and without a read/write hazard. Mac ends the render encoder,
reads Program directly in a compute encoder, composites in a render encoder with `loadAction Load`, then the
compositor continues in a fresh encoder. The Mac pass is a shader matching the Windows kernel, not MPS:
MPS's kernel is not the Windows kernel, and parity is judged against one reference.

## Resource and realtime rules

- Shaders, states and the constant buffer compile once with the compositor pipeline.
- Scratch (a BGRA8 region copy on Windows, two half-float planes at half resolution on both) is reserved
  **with the Program target** (`ensureRenderTarget` / Program `ensureTarget`), so a glass graphic going to
  air allocates nothing. Only a secondary target larger than Program grows it, once. At 1080p: 8 MB copy +
  2 x 4 MB planes (Windows), 2 x 4 MB (Mac).
- Per frame and plate: one region copy (Windows) and four draws/dispatches. No create, no map-for-read,
  no flush, no wait. Plates are resolved into a POD (`BackdropDraw`); nothing is allocated.
- Graphics without glass cost nothing: `collectBackdrops` returns null and the compositor skips the pass.

## Failure handling (loud, never a silent fallback)

If the pass is unavailable (shader compile failure, a failed reserve, a target that is not BGRA8, a removed
device), the plate still draws its own fill without blur and the Program frame carries the warning
`graphics-backdrop-unavailable` and `health: degraded`. There is no frosted stand-in. A device removed mid-show
fails every later call the same way; the pass is rebuilt with the compositor that owns the new device
(`d3d-device-loss.md`).

The NDI graphics key pass skips glass: a key is composited downstream over pictures this core never sees,
so there is nothing true to blur. The plate's fill still keys.

## How parity is judged

The raster parity harness (`native/tests/fixtures/graphics-parity/`) compares overlay rasters, which never
contain the backdrop, so it cannot judge this pass, and no fake raster fixture was added. Instead each
GPU's output is compared, on its own platform, against the **CPU reference** `backdropReference` in
`BackdropBlur.h` (half-float intermediates emulated), over the same fixtures (`native/tests/BackdropBlurFixture.h`):

- `D3D11BackdropBlur.MatchesTheSharedCpuReferenceAt1080p`, `...AtSmallBlurAndOddSizes` (Windows);
- `MetalBackdropBlur.MatchesTheSharedCpuReferenceAt1080p`, `...AtSmallBlurAndOddSizes` (Mac CI).

Tolerance: no 8-bit channel may differ from the reference by more than `kBackdropReferenceTolerance` (2)
levels. Windows and Mac therefore match each other within 4 levels through the reference.
End to end, `D3D11BackdropBlur.CompositorBlursProgramUnderAGlassPlate` and its Metal twin push a glass
template over a hard checker through the real compositor and check the picture inside the plate is blurred
and the picture outside is untouched.

## Frame budget

`D3D11BackdropBlurBench.TwoGlassPlatesAt1080p` times the whole pass (copies, both blurs, both composites)
for two lower-third-sized plates at 1080p (1060x168 at blur 32, and 640x288 at the maximum blur 64) with
D3D11 timestamp queries, 600 frames after 30 of warm-up. It reports; it does not assert (CI GPUs differ).
`D3D11BackdropBlurBench.WorstCaseFourFullFramePlatesAt1080p` times the schema's bound: four full-frame
plates at blur 16 (half resolution, full 25-tap kernel: the most work per output pixel a plate can ask for).

Measured 2026-11-28, NVIDIA GeForce RTX 4090, Release `corevideo-native-tests.exe` (core exe 4,379,136
bytes), idle desktop, isolated pass:

| Workload | GPU mean | p50 | p99 | max | CPU encode mean / p99 |
|---|---|---|---|---|---|
| Two plates (1060x168 blur 32, 640x288 blur 64) | 36.3 us | 36.4 us | 38.5 us | 46.1 us | 2.8 us / 22.6 us |
| Worst case: 4 full-frame plates, blur 16 | 204.0 us | 196.6 us | 222.2 us | 224.3 us | 6.1 us / 39.6 us |

Two plates cost about 0.2 % of the 16.67 ms frame, and the worst case the schema permits about 1.3 %. On
this GPU the pass is very likely to fit with large headroom. That is an inference from an isolated
measurement, not proof of the gate.

**Unverified:** the outputs release gate itself. No 30-minute outputs soak with two glass pack graphics
plus a crawl on air at 1080p60 has been run. The bench measures the pass in isolation on an idle GPU, not
the pass inside a loaded show with encoders, ISO and the vcam running. Mac GPU time is not measured on
customer hardware (Mac CI runs on a virtual GPU).
