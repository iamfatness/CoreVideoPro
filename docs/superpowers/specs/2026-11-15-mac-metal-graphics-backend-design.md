# Metal graphics backend: Core Text raster twin and Metal adapter (macOS parity, M3)

Status: DRAFT design for owner review, 2026-11-15.

Covers roadmap milestone **M3** (M3.1, M3.2, M3.3) in
`docs/superpowers/plans/2026-11-14-mac-parity-1.0-roadmap.md`, the native half of gap **2.2** in
`docs/superpowers/audits/2026-11-15-mac-ui-parity-consolidated.md`, and row **G0** of
`docs/superpowers/audits/2026-11-14-mac-parity-graphics.md`. Owner priority #4 (graphics + Tiles),
native half. The Swift Graphics surfaces (M4) are a separate spec; section 5.11 states what this
backend hands to it.

## 1. Why

On a Mac the operator cannot put a native graphic on air. No template lower third, no title card,
no table, no crawl, no clock, no social card, no Shot Layout graphic. The shared core has the whole
state machine and its tests pass on macOS CI, but the core refuses every `graphics-*` command on a
Metal build and there is nothing that could draw the result if it accepted one.

Everything in roadmap M4 and M6 waits on this. It is the critical path for graphics.

A second, smaller reason: the Mac already draws the legacy lower third, captions and the brand bug,
and it draws them on the render thread with a one-entry cache (section 4.2). That path breaks the
"never raster on the render tick" rule today, before any new feature is added.

## 2. Corrections to the inputs

These were found while verifying the roadmap and audit claims in code. Each changes the design.

1. **There are three Windows-only seams, not one.** The roadmap names the rasterizer. Two more
   sit in shared files:
   - Text measurement. `measureText` in `native/src/graphics/GraphicsTemplate.cpp:152-185` uses
     DirectWrite under `COREVIDEO_WITH_D3D11` and otherwise returns `characters * size * 0.55`.
     On a Mac every auto-width plate, every crawl item width (`:309`) and every `minFontSize` fit
     (`:506-511`) is laid out from that estimate.
   - Library thumbnails. `native/src/graphics/GraphicsThumbnail.cpp:10-38` is D3D11-gated and
     answers `state: "unsupported"` elsewhere.
2. **The Mac overlay raster is synchronous on the render thread.**
   `MetalCompositorAdapter.mm:1585-1619` rasters with Core Text and uploads inside
   `drawOverlayLayer`, and keeps exactly one texture (`overlayTex_`, `:1706`). Two overlays in one
   pass, or one overlay drawn by two passes at different target sizes, re-raster every frame.
   The cost has not been measured (UNVERIFIED); the structure is verified.
3. **The failure warning is `graphics-preparation-failed`**, not `graphics-failed`
   (`D3D11CompositorAdapter.cpp:545-546`). `graphics-preparation-failed` is also a command refusal
   reason (`GraphicsInstances.cpp:692,720`).
4. **"#350" is the commit that added the Mac Core Text raster** (`41d15693`). Its alpha rule is
   the comment at `MetalCompositorAdapter.mm:61-66` and the fallback premultiply at `:1603-1611`.
   There is no separate alpha issue to port.
5. **The private graphics preview is out of reach on Metal today.** It rides `set-grade-preview`
   with `lowerThirdPreview` (`native/tests/GraphicsRpcPixelTest.cpp:57-60`) and needs
   `ICompositor::supportsGradePreview()`, default false (`Interfaces.h:1408`), which Metal does
   not override. The Designer (M4.7) needs it. It is slice G6 here and depends on another spec.
6. **Consolidated row 0.6 (manual lower third never drawn, `mac:manual`) is already being fixed in
   this worktree** (`mac-shell/Sources/CoreVideoProShell/ProgramLowerThird.swift:5-8`,
   `ShellTestsTier0.swift:599-619`). This spec does not own it.

## 3. Windows behaviour to match

### 3.1 The gate

- `MediaCore.cpp:1271-1274`: a command whose `type` starts with `graphics-` is refused with
  `graphics-instance-backend-not-built` unless `compositor->supportsGraphicsInstances()`; a
  refused request records `graphics-request-refused`.
- `Interfaces.h:1488` defaults to false; `D3D11CompositorAdapter.cpp:268` returns true.
- `CoreStateCapture.cpp:253` advertises the `graphics-instances` capability from the same flag.

### 3.2 The rasterizer contract (`CompositorOverlayRaster`)

Public surface, `native/src/compositor/CompositorOverlayRaster.h:42-87`:

| Member | Contract |
|---|---|
| `beginFrame()` | clears the per-frame pending and failed flags, the active lease and resource, sets state `unprepared` |
| `rasterOverlayTexture(device, ctx, overlay, rect, targetW, targetH, slot, strict, prepared)` | returns a view when a complete raster exists, else null; never blocks; never touches the immediate context |
| `preparationState()` | `unprepared`, `preparing`, `ready` or `failed` for the last call |
| `lastWarning()` | operator-readable reason for the last call |
| `pendingThisFrame()`, `failedThisFrame()` | sticky within a frame |
| `preparedResource()` | a `GraphicsPreparedResource` for the last call, only when `ready` |
| `activeRuns()` | the `GraphicsRunLayout` (runs, atlas slices, drawing) when the raster is an atlas |
| `lease()` | keeps the texture alive across the submitted draw |
| `refuseGpuLease()` | marks the frame failed when the GPU read fence is full |
| `thumbnailPng(overlay, w, h, warning)` | synchronous, worker-only, at most 320x180, at most 256 KiB PNG, straight alpha |

Internal types, `:122-136`:

- `Request { device, overlay, width, height, strictResources, thumbnail }`.
- `Key { device, signature, contentHash[32], hasContentHash }`, totally ordered.
- `OverlayRasterTex { thumbnail, runs, budget lease, texture, view }`.
- `Cache = graphics::PreparationCache<Key, Request, OverlayRasterTex>`, constructed with a
  256 MiB byte budget, 32 queued, 64 resident (`CompositorOverlayRaster.cpp:176-182`). One
  persistent worker thread.

Admission rules in `rasterOverlayTexture` (`CompositorOverlayRaster.cpp:414-495`), all of which
the twin must reproduce exactly because the shared state machine depends on them:

1. Metadata bound: the nineteen string fields listed at `:423-426` total at most 65536 bytes, else
   `failed` with "Graphics content exceeds preparation metadata budget."
2. Dimensions: `rect * target` must be finite, at least 1 and at most 4096 in each axis, else
   `failed` with "Invalid graphics raster dimensions." Width and height are `lround`.
3. Key: device identity, `overlayContentSignature(overlay, w, h)` XOR `0xd9e94a8e64b5fc21` when
   strict, plus the content hash. The signature excludes animation state.
4. A supplied prepared resource is compatible only when backend, device identity, content hash and
   its own signature at its own size all match and it is at most 4096 square (`:445-449`).
5. Same size and compatible: adopt the prepared resource directly, no cache lookup.
6. Otherwise `find`, then `findOrRequest` with an admission estimate: `w * h * 8`, or for a
   layered-motion template the atlas bound at `:460-471`, plus 128 MiB when a legacy `imageUri`
   is present.
7. Scale retention (`:475-487`): when the new size is not ready but a compatible prepared resource
   of another size exists, draw the old one, report `preparing` and "Graphics scale preparation
   pending; retaining the accepted raster." A failed new size reports `failed` and keeps the old.
8. Capacity wording (`:488-491`): pending at the resident limit says "Graphics preparation pending
   at the shared 16-revision capacity limit."

Worker rules in `prepareRaster` (`:523-735`):

- Template content resolves on the worker: `definition->resolve(fields, brand, drawing, error,
  logo)` (`:537`). A resolve error is a failed preparation carrying that error.
- Package images are opened from `asset.encoded` (at most 32 MiB) and must decode to exactly the
  declared `asset.width` x `asset.height` (`:119-133`). Legacy `imageUri` files are at most
  4096 square and 16 megapixels (`:93-94`).
- Budget: `GraphicsResourceBudget::process()`, limits 128 MiB CPU, 256 MiB GPU, 2 concurrent
  preparations, 16 resident revisions (`GraphicsResourceBudget.h:56-58`). Refusal returns
  "Graphics preparation is waiting for shared resource capacity." with `retryable = true` (`:578`).
- Atlas: when the drawing has a crawl, or the template has layered motion and is not using
  whole-graphic motion, `planRuns` + `packAtlas` choose the texture size and each run is drawn at
  its slice offset (`:558-569`, `:612-621`). A failed pack is "Graphics motion needs more texture
  space than one graphic allows; give fewer layers their own motion."
- Strict mode (`strictResources`, set for every instance layer) fails the preparation if any
  resource was incomplete (`:688-689`). Legacy mode substitutes and warns.
- The output is an immutable BGRA8 texture holding premultiplied alpha over transparent (`:722-729`).

### 3.3 What the template raster draws (`drawTemplate`, `:298-412`)

Primitives come from `GraphicsPrimitive` (`GraphicsTemplate.h:22-39`). Coordinates are template
units, scaled by `heightPx / drawing.height`.

| Type | Rule |
|---|---|
| every primitive | axis-aligned, aliased clip to `p.clip`; opacity multiplies the brush |
| `rect` | filled rounded rect, radius `cornerRadius`; optional stroke inset by half its width, radius reduced by the inset |
| `ellipse` | filled, optional inset stroke |
| `line` | stroke only, corner to corner, `lineRises` picks the diagonal |
| fill | solid `color`, or a two-stop linear gradient `color` to `endColor`, horizontal or vertical across the box |
| `image` | `contain` or `cover`, left/center/right and top/center/bottom alignment, linear filter, opacity; a missing bitmap is incomplete with "Graphics image could not be decoded: <id>" |
| `text` | see 3.4 |
| table | not a primitive type: `GraphicsTemplate.cpp:322-395` expands a table into rect and text primitives (cells use `overflow: "ellipsis"`), so the raster needs no table code |
| crawl | text primitives with `crawlStart`, `overflow: "clip"`, one per item (`GraphicsTemplate.cpp:296-317`) |

### 3.4 Text rules

- Family must exist in the system font collection or the primitive is incomplete with "Requested
  graphics font is unavailable: <family>" (`:383-386`). Weights are 400, 500, 600 or 700.
- Alignment leading/center/trailing; paragraph alignment near (top).
- `overflow: "wrap-then-ellipsis"` wraps; anything else is single line.
- Layout box is `box.width` by `min(box.height, fontSize * lineHeight * maxLines)` (`:391-393`).
- Line spacing is **uniform**: line height `fontSize * lineHeight`, baseline at `fontSize * 0.8`
  below each line top (`:397`). Font ascent and descent do not move the baseline.
- Trimming is character granularity with an ellipsis sign (`:396-399`).
- Outline: the glyph outlines stroked at `2 * outline.width` in the outline colour, under the fill
  (`:288-293`). Shadow: the outlines offset by `(x, y)`, filled, and when `blur > 0` preceded by
  eight stroked rings of width `2 * blur * ring/8` at opacity `(1 - 0.8 * ring/8) / 8` (`:268-287`).
- Fit: `minFontSize` runs an eight-step bisection in the layout, using `measureText`
  (`GraphicsTemplate.cpp:506-516`).

Legacy overlay text (`drawOverlayTextLine`, `:135-174`): em size is `line.fontSize` when set, else
72% of the line box; no wrap; vertically centred; ellipsis trimming; first line semibold. The band
is drawn in the raster only when `appearance.enabled` and not a caption, as a rounded rect clip
(`:626-637`); otherwise the adapter draws the band as a separate quad.

### 3.5 How the adapter consumes it (`D3D11CompositorAdapter.cpp`)

- `:498`, `:522`: `beginFrame()` at the start of each pass.
- `:545-547`: after drawing layers, `graphics-preparing` and `graphics-preparation-failed` are
  appended to `frame.warnings` and health becomes `degraded`.
- `:550-557`: every entry of `plan.graphicsPreparations` is submitted with strict resources and its
  `layerId` as the preparation slot, and one `GraphicsFrameFact` is pushed per entry carrying the
  state, warning, prepared resource and requested content hash. These do not degrade Program.
- `:2064-2217` `drawOverlayLayer`:
  - a bound instance draws only when its source was drawn this frame at the bound generation
    (`:2068-2069`);
  - an instance layer with no ready raster draws nothing, never the accent-bar fallback (`:2123`);
  - `graphicsAwaitingEntrance` pushes `composition-eligible` and does not draw (`:2124-2127`);
  - with `activeRuns()`, each run is sampled with `sampleRun`, crawl items are positioned with
    `crawlItemX` and clipped to the crawl window, and each slice is drawn with its own
    destination, scissor and opacity (`:2157-2198`);
  - the legacy band quad is skipped when `appearance.enabled` and not a caption (`:2132`);
  - `holdLastRaster` layers use `OverlayRasterRetention` (`:2107-2122`).
- `:1816-1820`: a composited instance pushes `ready` (when the resource is new) and
  `draw-submitted`.
- `GraphicsInstances::observe` (`GraphicsInstances.cpp:941-1003`) turns those facts into
  `preparation`, `cuePreparation`, `currentlyComposed`, `firstDrawSubmittedFrame` and resource
  adoption. `resourceMatches` (`:21-27`) requires a non-null `native`, a device identity, a size
  and a matching content hash.

### 3.6 Motion and crawls are core decisions

The adapter owns no clocks. `GraphicsInstances::append` (`GraphicsInstances.cpp:808-910`) fills
`overlay.graphicsMotion` (layered templates), or applies the whole-graphic timeline directly to
`content.opacity` and `content.rect` (`:887-893`), and fills `overlay.graphicsCrawlDistance` from
the prepared resource's `crawl` playback (`:869-873`). Crawl timing is `GraphicsCrawl.h`. All of
this already runs on macOS.

### 3.7 Windows tests that prove it

`native/tests/GraphicsRpcPixelTest.cpp` (the whole file is under `#if COREVIDEO_WITH_D3D11`,
`:15-1316`), `D3D11CompositorTest.cpp:991-1141` (overlay raster, cache, oversize, cue/program
adoption) and `:1634`, `GraphicsPreparationCacheTest.cpp` (portable, 21 tests),
`GraphicsInstancesTest.cpp`, `GraphicsTemplateTest.cpp`, `GraphicsMotionTest.cpp` (portable).

## 4. Mac today

### 4.1 Shared and working on macOS

`GraphicsInstances`, templates, curated looks, motion, crawl timing, clocks, automation, the
preparation cache and the resource budget are in the shared source list
(`native/CMakeLists.txt:126-135`). The snapshot `graphics` node is published on every core
(`CoreStateCapture.cpp:484-487`). `GraphicsPreparedResource::Backend` already has `Metal`
(`GraphicsPreparedResource.h:13`) and the snapshot already prints `"metal"`
(`GraphicsInstances.cpp:51-52`). The Metal overlay shader already takes a UV scale and offset
(`MetalCompositorShaders.h:193-198`) and the pipeline already blends premultiplied
(`MetalCompositorAdapter.mm:516-522`), so atlas slices need no shader work.

### 4.2 Present but not to contract

`MetalCompositorAdapter.mm:56-221` is a Core Text and ImageIO raster for the legacy overlay only.
Against section 3 it differs as follows.

| Point | Mac today | Evidence |
|---|---|---|
| Where it runs | render thread, inline | `:1561`, `:1585-1619` |
| Cache | one entry, no budget | `:412-417`, `:1706` |
| Band | always painted opaque in the raster, and also as a quad | `:209-213`, `:1557` |
| Rounded corners, `line.fontSize`, semibold first line | ignored | `:131-190` |
| Overflow | clipped, no ellipsis | `:165-173` |
| Packaged image (`graphicsImage`) | ignored; file `imageUri` only | `:88-128` |
| Missing font | silent Core Text substitution | `:137-140` |
| Failure reporting | none; falls back to the 5x7 bitmap raster | `:1599-1611` |
| Slate name label | not drawn, `TODO(4a-metal-text)` | `:1509-1516` |

### 4.3 Missing

No `supportsGraphicsInstances` override, no handling of `graphicsTemplate`, `graphicsMotion`,
`graphicsCrawlDistance`, `graphicsPreparedResource`, `graphicsAwaitingEntrance` or the bound-source
rule, no `graphicsPreparations` loop, no `graphicsFacts`, no graphics warnings, no thumbnails, no
grade-preview surface, no Core Text measurement, no font policy. The fonts bundled in the app
(`mac-shell/Resources/Fonts`: Space Grotesk, IBM Plex Mono) are the shell's UI fonts; the shell
registers them for its own process (`DesignSystem.swift:36-50`) and the core is a separate
executable in `Contents/Resources` (`scripts/make-macos-shell-bundle.sh:143-144`), so they do not
reach the core. Whether `ATSApplicationFontsPath` would apply to that helper is UNVERIFIED and the
design does not rely on it.

## 5. Design

### 5.1 Components

All new unless marked.

| Component | File | Role |
|---|---|---|
| `CoreTextOverlayRaster` | `native/src/compositor/CoreTextOverlayRaster.h`, `.mm` | the twin of `CompositorOverlayRaster`; same public surface with `id<MTLDevice>` and `id<MTLTexture>` in place of the D3D types |
| `GraphicsFontResolver` | `native/src/graphics/GraphicsFontResolver.h`, `GraphicsFontResolverCoreText.mm` | family + weight to a font, the substitution table, bundled font registration, availability answer |
| Core Text measurement | `native/src/graphics/GraphicsTextMeasureCoreText.mm` | the Apple branch of `measureText`, called from the existing function in `GraphicsTemplate.cpp` |
| Metal adapter changes | `MetalCompositorAdapter.mm` (existing) | owns one raster, runs the section 3.5 rules |
| Thumbnail branch | `GraphicsThumbnail.cpp` (existing) | Apple branch calling `thumbnailPng` |

The legacy functions at `MetalCompositorAdapter.mm:56-221` move into the new raster and are
deleted from the adapter. The portable 5x7 raster stays in `OverlayTileRaster.cpp` for the software
compositor only.

The front half of `rasterOverlayTexture` (rules 1 to 8 in 3.2) is platform-neutral. This design
duplicates it in the twin and holds both to one typed contract test (section 7.1) rather than
extracting a shared header now, because extraction edits a Windows file that cannot be built on
this Mac. Extraction is a follow-up for a Windows session.

### 5.2 Request, Key and prepared resource on Metal

- `Request { id<MTLDevice> device; CompositorOverlayContent overlay; int width, height; bool
  strictResources, thumbnail; }`.
- `Key` is unchanged. `device` is the `MTLDevice` `registryID`, which is stable for the process
  and distinguishes an external GPU.
- `MetalRasterTex { thumbnail; runs; budget lease; id<MTLTexture> texture; }`, with the budget
  lease declared first so the texture releases before its charge.
- `GraphicsPreparedResource`: `backend = Metal`, `deviceIdentity = registryID`, `signature`,
  `width`, `height`, `native` = the cache result (which owns the texture), `contentHash`,
  `hasContentHash`, and `crawl` copied from the drawing exactly as `CompositorOverlayRaster.cpp:509-512`.
- Cache construction matches Windows: 256 MiB, 32 queued, 64 resident, one worker.

Nothing here crosses the wire. The snapshot's `preparedResource`, `cueResource` and
`programResource` rows read `{"backend":"metal","width","height","contentKey"}`.

### 5.3 Preparation off the render tick

```
render thread                              preparation worker (PreparationCache)
rasterOverlayTexture(key, request) ──────► resolve template, measure, lay out
   returns nil, state "preparing"          CGBitmapContext raster (premultiplied BGRA)
   ...                                     newTexture + replaceRegion on the shared MTLDevice
find(key) hit ◄──────────────────────────── publish immutable result
   returns texture, state "ready"
```

- The render thread does a cache lookup and nothing else. It never creates a `CGContext`, a
  `CTLine`, an image decoder or a texture.
- The worker creates the texture and uploads it. `MTLDevice` texture creation and
  `replaceRegion` are safe from any thread, and the texture is never written again after
  publication. The worker wraps each job in an autorelease pool.
- A Metal command buffer retains the textures bound to it (the adapter uses `[queue_
  commandBuffer]`, `MetalCompositorAdapter.mm:603`), so the D3D read-lease fence has no Metal
  equivalent. `lease()` still exists so an instance's accepted resource outlives cache eviction;
  `refuseGpuLease()` is kept for surface parity and is never called.
- Thumbnails run synchronously on the template-library worker that calls
  `renderGraphicsThumbnail`, as on Windows. They never run on the render thread.
- Legacy overlays gain the Windows behaviour: the first frames of new content draw the accent-bar
  fallback until the raster is ready, instead of stalling the tick.

### 5.4 Alpha

One rule, the Windows rule: every prepared texture is premultiplied BGRA8 over transparent and is
drawn with the (One, 1 - SrcAlpha) pipeline that already exists.

- `CGBitmapContextCreate` with `kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little`
  produces that directly; no post pass (the #350 rule).
- Package and legacy images are decoded with ImageIO and drawn through Core Graphics, which
  premultiplies a straight-alpha PNG. Before drawing, the image is re-tagged with the context's
  colour space so an embedded ICC profile is not converted; WIC does no conversion either. EXIF
  orientation is not applied on either platform.
- Thumbnails are encoded as straight-alpha PNG by ImageIO from the premultiplied bitmap.
- Texture pixel format is `MTLPixelFormatBGRA8Unorm`, not sRGB, matching `DXGI_FORMAT_B8G8R8A8_UNORM`.
- The gradient interpolation space (straight or premultiplied, when the two stops differ in
  alpha) is UNVERIFIED for D2D's 1.0 stop collection. Parity fixture `gradient-alpha` decides it;
  if `CGGradient` disagrees, the twin draws the gradient from a 256-step table computed the D2D way.

### 5.5 Fonts

The problem: the core's default family is "Segoe UI" (`LowerThirdAppearance.h:9`,
`GraphicsTemplate.cpp:220`), the Windows builder offers Segoe UI, Segoe UI Semibold, Bahnschrift,
Arial, Verdana, Trebuchet MS, Georgia, Cambria and Consolas (`GraphicsBuilder.cs:125`), and strict
preparation fails on an unknown family. Arial, Verdana, Trebuchet MS and Georgia ship with macOS.
The rest do not. A show document made on Windows must not fail on a Mac.

`GraphicsFontResolver` interface:

- `resolve(family, weight) -> { font, resolvedFamily, substituted }` or unavailable.
- `available(family) -> bool`, used by strict mode.
- `registerBundledFonts(directory)`, called once at core start with a process-scope
  registration; the directory is passed by the shell at launch (new launch argument or environment
  variable, name to be fixed in the plan) and defaults to `Fonts` beside the core binary.

Resolution order:

1. Exact family match in the system plus process-registered collection.
2. The substitution table (a requested Windows family mapped to a named Mac family).
3. Unavailable. In strict mode this is the Windows failure, with the Windows wording. In legacy
   mode the family becomes the default and the warning is "Font unavailable; using <family>."

A substituted family is **not** a failure and is **not** silent: the preparation result carries
the warning "Font <requested> is not installed on this Mac; using <resolved>." which reaches
`instances[].warning`. Substitution uses an explicit table, never Core Text's own automatic
fallback for the primary family, so the same document always resolves the same way.

Proposed table (owner decision 1):

| Requested | Mac family | Source |
|---|---|---|
| Segoe UI, Segoe UI Semibold | Selawik | bundled; Microsoft's metric-compatible open fallback for Segoe UI. Licence to be confirmed before bundling (UNVERIFIED) |
| Bahnschrift | DIN Alternate | system |
| Cambria | Georgia | system |
| Consolas | Menlo | system |
| Inter (legacy `fontFamily` default, `OverlayTileRaster.h:54`) | Inter if installed, else the default | system or bundled |

"Segoe UI Semibold" is a family name on Windows; on the Mac it maps to Selawik at weight 600.

Weight mapping: 400, 500, 600, 700 select the nearest real face in the family. Core Text does not
synthesize bold; a family with no heavier face draws its regular face and the result carries a
warning. DirectWrite may simulate; this difference is accepted and named.

Glyph fallback for characters the family lacks (CJK, emoji) uses the Core Text cascade. Emoji draw
in colour on the Mac and monochrome in D2D; accepted and named.

### 5.6 Text layout rules for the twin

The twin does its own line placement so DirectWrite's layout decisions are reproduced and only
glyph shapes and advances differ.

- Measurement: `CTLine` typographic width including trailing whitespace, and for wrapped text a
  `CTTypesetter` line count at the wrap width. Same function serves `measureText` and the raster,
  so layout and drawing cannot disagree.
- Lines: `CTTypesetterSuggestLineBreak` for `wrap-then-ellipsis`, one line otherwise. Visible line
  count is `floor(min(box.height, lineHeightPx * maxLines) / lineHeightPx)`, at least one.
- Baseline of line `n` is `box.y + n * lineHeightPx + 0.8 * fontSize`. Font ascent, descent and
  leading are not consulted. This is the single most important parity rule.
- Trimming: if text remains after the last visible line, that line is rebuilt from the remaining
  range and truncated at the end with a "…" token (`CTLineCreateTruncatedLine`).
- Alignment by flush factor 0, 0.5 or 1 within `box.width`.
- Kerning and standard ligatures stay at Core Text defaults, as DirectWrite's are.
- Rendering: grayscale antialiasing, font smoothing off (the target has alpha, and the D2D target
  is software grayscale), subpixel positioning on.
- Effects: glyph outlines come from `CTFontCreatePathForGlyph` at each run's glyph positions,
  unioned into one path. Outline and shadow then follow 3.4 literally, including the eight-ring
  blur. `CGContextSetShadow` is not used; it would be a different blur.
- Clip: each primitive is drawn inside `CGContextClipToRect` with antialiasing off for the clip.
- Coordinates: the bitmap is top-down in template space and bottom-up in Core Graphics. Rects are
  converted per primitive (as `bottomUpRect` does today); the text matrix is never mirrored.
- Legacy lines keep the 3.4 legacy rule: vertical centring of the em box, ellipsis, semibold first
  line, `line.fontSize` honoured.

### 5.7 Shapes, images, tables

Direct mapping of 3.3. Rounded-rect radii are clamped to half the shorter side before building the
path (Core Graphics asserts otherwise; D2D clamps). Line and stroke joins use the Core Graphics
default miter, which matches D2D's default stroke style. Images use
`kCGInterpolationMedium`-or-lower linear filtering (exact constant chosen by the `image-scale`
parity fixture). Tables and crawls need no raster code beyond text and rect.

### 5.8 Atlas runs, motion and crawl in the Metal adapter

`drawOverlayLayer` in the Metal adapter is brought to 3.5:

- Whole-graphic motion needs nothing: the core has already moved the rect and set the opacity.
- With `activeRuns()`, the adapter loops runs exactly as `D3D11CompositorAdapter.cpp:2157-2198`:
  `sampleRun` per unit, `crawlItemX` for crawl items, intersect the mask with the layer clip, then
  set scissor, viewport and `setLayerConstants(..., uScale, vScale, uOffset, vOffset)` and draw.
  All the math is shared headers; the Metal code is only the three state calls.
- Crawl scrolling is therefore a per-frame change of destination rect over a static atlas. No
  re-raster while a crawl runs. A crawl revision change (new data) is a new preparation, adopted by
  the core at the pass boundary as today.
- Atlas dimensions are at most 16384 in each axis (`CompositorOverlayRaster.cpp:467-468`). Apple
  Silicon GPUs support 16384 square textures; the twin still checks the device limit and fails the
  preparation with the atlas-space wording if it is exceeded.

### 5.9 Preparations, facts and warnings

Implemented in `MetalCompositor::render` for the Program pass only, as on Windows:

1. `beginFrame()`; draw layers; then append `graphics-preparing` and/or
   `graphics-preparation-failed` to `frame.warnings` and set `health = "degraded"`.
2. For each `plan.graphicsPreparations` entry: call the raster with strict resources, the entry's
   `layerId` as slot and its `graphicsPreparedResource`; push
   `{instanceId, revision, preparationState(), lastWarning(), preparedResource(), contentHash, hasContentHash}`.
3. `beginFrame()` again so step 2 does not leak into the next pass.
4. During layer drawing: push `composition-eligible`, `ready` and `draw-submitted` under the same
   conditions as `D3D11CompositorAdapter.cpp:1816-1820` and `:2124-2127`.
5. The bound-source rule needs the per-frame record of drawn sources and generations
   (`recordGraphicsSourceDraw`, D3D `:1746-1761`); the Metal adapter gains the same fixed-size
   record, filled where a participant or screen-share layer is drawn with a content frame.
6. `observeGraphicsSubjects` subject facts (D3D `:1933-1944`) are ported with it, since graphics
   automation reads them.

Then `supportsGraphicsInstances()` returns true and the capability and the command gate follow.

Preview and multiview passes draw graphics layers through the same `drawOverlayLayer` and emit no
facts, as on Windows.

### 5.10 Memory and time bounds

| Bound | Value | Source |
|---|---|---|
| Raster size | 4096 x 4096 | 3.2 rule 2 |
| Atlas size | 16384 per axis, 256 MiB | `:467-469` |
| Cache | 256 MiB, 32 queued, 64 resident | `:176-182` |
| Process budget | 128 MiB CPU, 256 MiB GPU, 2 preparations, 16 resident revisions | `GraphicsResourceBudget.h:56-58` |
| Metadata | 64 KiB per request | 3.2 rule 1 |
| Package image | 32 MiB encoded; legacy image 4096 square, 16 MP | 3.2 |
| Thumbnail | 320 x 180, 256 KiB | `:518`, `:708` |
| Instances | eight per bus, enforced by the core | `GraphicsInstances.cpp:548-550` |

On Apple Silicon CPU and GPU memory are one pool; the two budget counters are kept as they are so
the snapshot's `resourceBudgetAtStatePublication` means the same thing on both platforms.
`GraphicsCachePressure::requestTrim` is honoured by the same cache. The slate-name and Tiles
name-line paths reuse `OverlayRasterRetention<id<MTLTexture>>` unchanged.

Render-tick cost added by this design: one map lookup per overlay layer per pass and at most
2 * units + 1 draws for a layered graphic. No allocation on a cache hit.

### 5.11 What this backend hands to the shell spec

Nothing new on the wire. After slice G2 a Metal core behaves as a D3D11 core for these:

- **Capability**: `graphics-instances` present in the snapshot's `capabilities`.
- **Commands accepted** (`GraphicsInstances.cpp:360-368`), each as
  `{type, graphics: {schemaVersion: 1, requestId, clientId, coreEpoch, instanceId?, expectedRevision?, ...}}`:
  `graphics-instance-create`, `graphics-instance-update`, `graphics-instance-cue`,
  `graphics-instance-trigger`, `graphics-instance-remove`, `graphics-program-clear`,
  `graphics-template-install`, `graphics-template-list`, `graphics-template-remove`,
  `graphics-clock-control`, `graphics-automation-configure`, `graphics-automation-control`,
  `graphics-authority-set`.
- **Refusals**: `commandProtocolFailures` carries `graphics-instance-backend-not-built` (never
  again on a Metal core after G2) or `graphics-request-refused`; the per-request result and reason
  are in the `graphics` node's `operations`.
- **Snapshot**: the `graphics` node (`schemaVersion`, `coreEpoch`, `instances[]`, `operations`,
  `authoring`, `automation`, `familyAuthorities`, `resourceBudgetAtStatePublication`,
  `layoutTakeReceipts`). Per instance the shell reads at least `instanceId`, `revision`,
  `visibility`, `preparation`, `warning`, `cued`, `cuePreparation`, `cueWarning`, `cueRevision`,
  `programRevision`, `currentlyComposed`, `firstDrawSubmittedFrame`, `showPending`,
  `crawlPlayback`, `crawlPendingRevision`, `clock`, and the three resource rows with
  `backend: "metal"`.
- **Frame warnings**: `graphics-preparing` and `graphics-preparation-failed` in the Program frame's
  warnings.
- **Thumbnails** (after G5): `graphics-template-list` with `includeThumbnails` returns
  `{state: "ready", backend: "metal-native", appearance: "reference", width, height, pngBase64}`.
  `backend: "metal-native"` is a new value of an existing field.
- **Mac-only warning text**: the font substitution warning in 5.5. The shell shows it as it shows
  any `warning`.
- **Not handed over**: Cue-rides-Take needs the shell to send `begin-take-transition` (roadmap
  M1.1; the core side at `MediaCore.cpp:1526` is shared). The private design preview needs G6.
  The NDI graphics key stays unsupported (`supportsGraphicsKey()` false).

### 5.12 The manual lower third and other legacy overlays

They converge onto this backend at the raster level and stay separate at the command level.

- Raster: after G1 the Mac lower third, captions, brand bug, slate name and Tiles name line are
  all produced by `CoreTextOverlayRaster` through the same cache, with the 3.4 legacy rules. The
  Mac look changes slightly and deliberately: ellipsis instead of a hard clip, semibold first
  line, rounded band when an appearance is set, and no doubled band.
- Commands: `set-overlay-asset`, `set-brand-kit` and the caption commands are unchanged
  (`Overlays.swift:8-14`). The legacy lower third does not become a graphics instance here.
- Authority: once instances exist, `graphics-authority-set {family: "lower-third", authority:
  "instances"}` makes the core drop the legacy `overlay:lower-third` layer from the plan
  (`GraphicsInstances.cpp:812-816`). That routing is the shell spec's item (audit row 4b).

### 5.13 Degraded states the operator sees

| Condition | What airs | What is published |
|---|---|---|
| New graphic still preparing | nothing for that graphic; Program otherwise unchanged | `preparation: "preparing"`, `graphics-preparing` |
| Update preparing | the accepted revision keeps airing | same |
| Font missing with no substitute, strict | accepted revision keeps airing; new one refused | `preparation: "failed"`, warning names the font |
| Font substituted | the graphic, in the substitute | `warning` names both families |
| Image cannot decode | accepted revision keeps airing | `failed`, "Graphics image could not be decoded: <id>" |
| Budget full | accepted revision keeps airing | `preparing`, capacity wording |
| Atlas too large | accepted revision keeps airing | `failed`, atlas-space wording |
| Metal device unavailable | compositor already reports "Metal compositor unavailable" | unchanged |

## 6. Slices

| Id | What lands | Lane | Size | Depends on | Proof |
|---|---|---|---|---|---|
| G1 | `CoreTextOverlayRaster` with the async cache and the legacy branch; Metal adapter uses it for lower third, captions, brand bug; slate name label (`TODO(4a-metal-text)`); old inline raster deleted. Consumer: the overlays the Mac ships today. | native | M | none | D3D overlay tests ported to Metal (7.2 group A); "blocked preparation never stalls the tick" test; drill baseline unchanged |
| G2 | Template raster (all of 3.3, 3.4), Core Text measurement, `GraphicsFontResolver` with the substitution table and bundled fonts, atlas runs, adapter rules 5.8 and 5.9, `supportsGraphicsInstances()` true. Consumer: `graphics-*` over stdio, exercised by the ported RPC pixel tests and usable by the M4.1 gateway. | native | L | G1; owner decision 1 | 7.2 groups B and C green on Metal; contract fixtures unchanged |
| G3 | Bounds and failure behaviour: device identity, cache pressure, eight-instance capacity, failed-font retention and repair, accepted raster surviving a new renderer, Tiles name-line retention on Metal. | native | M | G2 | 7.2 group D |
| G4 | Parity standard: Windows reference metrics fixture, Mac comparator, Mac goldens, `mac-show-drill.py --graphics-cue`. | native | M | G2; one Windows build session to emit the reference | section 7.3 and 7.4 pass on this Mac |
| G5 | Library thumbnails on Metal (`renderGraphicsThumbnail` Apple branch). Consumer: `graphics-template-list includeThumbnails`. | native | S | G2 | port of `ThumbnailForALayeredTemplateMatchesItsSettledDesign` |
| G6 | Private graphics preview on Metal (`lowerThirdPreview` on the grade-preview surface). Consumer: the Designer and any Operate preview that uses it. | native | M | G2; Metal `supportsGradePreview` from the per-source grade spec (gap 2.5) | port of `PrivateGraphicsPreviewRequiresExplicitNullAppearanceAndProducesChangingPixels` reading the IOSurface |

G1 is shippable alone and should ship first: it removes a render-thread raster from the build the
Mac already ships. G2 is the point where M4 can start. G3 to G5 can run while M4.1 is built.

## 7. Acceptance and test strategy

### 7.1 Contract test shared by both rasters

`OverlayRasterContractTest` (new, typed over backend, compiled for D3D11 and for Metal): the eight
admission rules of 3.2 asserted through the public surface only, including state strings and
warning wording. This is what keeps the duplicated front half from drifting.

### 7.2 Ported pixel tests

The graphics pixel tests get a `createGraphicsTestCompositor()` helper and the gate
`COREVIDEO_WITH_D3D11 || COREVIDEO_WITH_METAL`. Most already read `frame.preview.bgra`, which the
Metal compositor fills on full ticks. Tests that open a DXGI shared handle stay Windows-only until
G6. Templates in ported tests that hard-code "Segoe UI" keep it, so the substitution path is
exercised by the real suite.

- **A (G1)** from `D3D11CompositorTest.cpp`: `LowerThirdOverlayRastersDirectWriteTextOnGpu`,
  `OverlayTextContentChangesPixelsOnGpu`, `AnimatedOverlayHonorsCompositeClipRect`,
  `CaptionRastersSpeakerAndBodyOnGpu`, `OverlayRasterCacheReusesAndInvalidates`,
  `OversizedGraphicsAreRefusedAndValidContentRecovers`.
- **B (G2)** from `GraphicsRpcPixelTest.cpp`: `CommandsReachCoreStateAndActualProgramPixels`,
  `EveryCuratedLookReachesActualNativeProgramPixels`, `NativeFadeAndReversalReachActualGpuPixels`,
  `TextEffectsDrawShadowOutlineAndFill`, `NewShapesDrawCoverageAndInsetStrokes`,
  `BarLeadsTheWordsOnProgramPixels`, `DataTablePublishesCompleteRowsAndKeepsAcceptedPixelsOnFailure`,
  `CrawlUsesPreparedTextAndClipsBothDirections`,
  `ClockTicksAdoptPreparedDigitsWithoutBlankingProgramOrChangingEditRevision`,
  `VerifiedPackageImagePreservesAlphaAndAcceptedBytesAfterFileChanges`,
  `FullyTransparentPngRevealsBackgroundAndKeepsPartialAlpha`,
  `UploadedDeclarativeLogoPreservesAlphaAspectAndAcceptedProgram`,
  `VerifiedLogoInCompatibilityLookChangesActualProgram...` (Program half only until G6).
- **C (G2)** source binding and social: `LocalCameraAndScreenLowerThirdsDrawAndRejectStaleSourcePixels`,
  `LocalScreenCaptureDrawFactsHonorPresentationSuppression`,
  `SourceGenerationReplacementCannotBurnThePreviousGuestIntoNewVideo`,
  `AutomaticIntroductionUsesActualSourceDrawAndClearPreventsReplay`,
  `ReviewedSocialCardsRenderOnlyOnExplicitShowAndRetire`; plus
  `D3D11Compositor.GraphicsCueAndProgramAdoptCompleteNativeRevisionsIndependently` and
  `GuardedShotLayoutDrawsBothPreparedGraphicsOnItsFirstFrame`.
- **D (G3)**: `FailedFontPreparationRetainsAcceptedPixelsAndRepairsWithoutRestart`,
  `AcceptedRasterSurvivesDraftCachePressureAndANewRendererWithoutAnotherShow`,
  `AcceptedCuePixelsStaySeparateFromPrivateEditsAndProgramUntilExplicitShow`,
  `EightProgramAndCueInstancesRefuseExcessRevisionsWithoutLosingAcceptedPixels`.

New Mac-only tests:

- `MetalGraphics.ABlockedPreparationNeverStallsTheRenderTick`: a test-only prepare hook (same
  structural rule as `CompositorFaultInjection.h`) blocks the worker; N render ticks complete
  inside their budget and report `preparing`. This is the test that must fail against today's
  inline raster.
- `MetalGraphics.TwoOverlaysAndTwoPassesRasterOncePerContent`: counts worker jobs across Program
  and Preview passes with two overlays; expects one job per distinct key.
- `GraphicsFontResolver.*`: exact match, each table row, strict unavailability wording,
  substitution warning, weight selection, bundled registration from a directory.
- `GraphicsTemplate` on Mac: `measureText` returns Core Text widths (not the 0.55 estimate) and
  the raster's drawn ink width agrees with it within 1 px.

### 7.3 Pixel-parity standard between D2D and Core Text

The outputs will not be bit-identical. The standard has two tiers, both judged at 1920x1080 from a
full-resolution Program readback (`fullProgramReadback`, `programFullBgra`), never from the
320x180 thumbnail.

**Reference data.** A new file `contracts/graphics-raster-reference.json` holds, per fixture, the
metrics measured from the D2D raster on Windows: per text primitive the advance width, line count,
truncated flag, ink bounding box and summed alpha; per fixture a 1/8-scale straight-alpha
thumbnail. A Windows test writes and verifies it; the Mac test only reads it. Fixtures use Arial
and Georgia, which both platforms ship, so the comparison measures the rasterizers and not a font
substitution. Segoe UI to Selawik is covered by its own fixture with the text tolerances doubled.

**Tier 1, geometry (rect, rounded rect, ellipse, line, gradient, image, clip, opacity).**

- Interior pixels (more than 1 px from any primitive edge): each channel within 2/255.
- Edge band: each channel within 48/255, and the summed alpha of the fixture within 0.5%.
- No pixel outside the D2D ink bounding box grown by 1 px has alpha above 8/255.

**Tier 2, text.**

- Line count and truncated flag: equal.
- Advance width: within the larger of 1.5 px and 1.5%.
- Baseline: within 1 px (it is computed, so a miss is a bug, not a font difference).
- Ink bounding box: each edge within 2 px.
- Summed alpha per text primitive: within 8% (stem weight differs between the two rasterizers).
- Structure: SSIM at least 0.90 between the two alpha channels after a 1 px Gaussian blur.
- Layout consequences: auto-width plate width and crawl `passWidth` within 1.5%; a `minFontSize`
  fit chooses a size within one bisection step.

**Fixtures.** `lower-third-corporate`, each curated look at its default fields,
`text-wrap-ellipsis-3-lines`, `text-fit-min-size`, `text-align-center-right`,
`text-outline-shadow-blur`, `shapes-inset-strokes`, `gradient-alpha`, `image-contain-cover-align`,
`image-scale`, `table-12x8`, `crawl-atlas-two-items`, `layered-motion-mid-entrance` (sampled at a
fixed `elapsedMs`), `legacy-lower-third-appearance`, `legacy-caption`, `segoe-substitution`.
The list is closed at plan time; these are the minimum.

**Mac goldens.** The same fixtures are also stored as Mac-rendered PNGs under
`native/tests/golden/metal-graphics/` (new) and compared with tier 1 and tier 2 tolerances against
later Mac runs. They catch regressions between macOS releases and Core Text changes; they are
regenerated only by an explicit, reviewed command.

**Eyes.** One side-by-side sheet (Windows capture, Mac capture, difference) for these
fixtures goes to the owner before G4 is closed. Numbers gate; the owner judges whether Selawik in
place of Segoe UI is acceptable on air.

### 7.4 Frame delivery

`scripts/mac-show-drill.py` gains `--graphics-cue` (new flag). With it, after the wall is up, the
drill creates a global instance from a curated lower third and a clock instance, cues and shows
them, updates the lower third's text once a second (forcing a preparation each time), and hides
and re-shows every five seconds for the whole window. A crawl instance is added if a crawl template
can be installed over stdio without a package root (UNVERIFIED; otherwise the crawl is covered by
7.2 only and that is stated in the drill output).

The gate, on this Mac, Release core, `COREVIDEO_FAKE_ENGINE_FPS=60` pinned:

- `python scripts/mac-show-drill.py --seconds 40 --load 8` is run first as the baseline.
- `python scripts/mac-show-drill.py --seconds 40 --load 8 --graphics-cue` must pass every existing
  gate (sustained fps, dropped frames, per-frame delivery, latency percentiles, coreMutex
  over-budget ratio) and must show no dropped frame and no delivery shortfall that the baseline
  did not show. Mean fps is not the judgement.
- Graphics-specific assertions from the snapshot: every instance reaches
  `firstDrawSubmittedFrame > 0`; `graphics-preparation-failed` never appears; the budget counters
  return to their pre-cue values after the instances are removed.
- The same pair is run once on G1 with a legacy lower third toggled, to show the render-thread
  raster is gone (render hold and worst frame no worse than baseline with the overlay cycling).

### 7.5 Shell and live checks

- `COREVIDEO_SHELL_SNAPSHOT` capture of Program with the legacy lower third on air, before and
  after G1, attached to the G1 review.
- Owner check after G2 (no Mac graphics UI exists yet): a scripted stdio session shows a curated
  lower third, a table and a crawl on the Program monitor; the owner watches entrance, hold and
  exit at 60 fps, and a recording is checked for the graphic by pixel probe.
- Owner check after M4.2 lands (other spec): the same on the real Operate surface.

## 8. Risks and unknowns

| Risk | Resolution |
|---|---|
| Core Text advance widths differ enough that Windows-authored layouts ellipsize or wrap differently on a Mac | tier 2 measures it per fixture; Selawik exists to close the Segoe UI case; residual cases are reported as the truncated-flag mismatch, not hidden |
| Selawik licence or quality is not acceptable | owner decision 1; fallback is Helvetica Neue with wider tolerances and a visible substitution warning |
| D2D gradient interpolation with unequal alpha is not what `CGGradient` does (UNVERIFIED) | `gradient-alpha` fixture; table-driven gradient if needed |
| Core Text or ImageIO calls are slower than expected on the worker and delay Show | measure preparation latency in the drill (time from command to `ready`); the worker is off the tick, so this affects readiness time, not frame delivery |
| Image colour management differs (ICC profiles) | re-tag rule in 5.4; `image-contain-cover-align` includes a profiled PNG |
| Goldens churn across macOS versions | tolerances, not exact match; goldens are advisory on CI runners and blocking on this Mac, same posture as the drill |
| The duplicated front half drifts from Windows | contract test 7.1; extraction follow-up |
| G4 needs a Windows build to emit the reference file | one Windows session; until then tier 1 and 2 run against Mac goldens only and the cross-platform claim is reported UNVERIFIED |
| The Metal adapter has no record of drawn sources for the bound-source rule | part of G2; group C tests prove it |
| Bundled font registration from a helper in `Contents/Resources` | explicit registration by path in 5.5; `GraphicsFontResolver` test with a real directory; notarized-build check in the release recipe |
| Converging the legacy overlay changes the Mac lower third's look | before and after capture in 7.5; owner sees it at G1 |
| Total size is larger than the roadmap's L + M + M | see section 9 |

## 9. Owner decisions needed

1. **Font policy for Windows families that macOS lacks.** Recommendation: the substitution table
   in 5.5, with Selawik bundled for Segoe UI (after a licence check), substitution reported as a
   warning and never as a failure. The alternative, failing preparation until the operator picks a
   Mac font, makes every Windows show document unusable on a Mac until edited.
2. **Colour emoji.** Core Text draws emoji in colour; D2D draws them monochrome. Recommendation:
   accept the Mac behaviour and name it in the parity sheet.
3. **G1 before everything else in lane E.** It changes the look of the shipped Mac lower third
   slightly (ellipsis, semibold, rounded band) and removes a render-thread raster. Recommendation:
   yes, ship it first and on its own.
4. **Size.** The roadmap sized M3 as L + M + M. With measurement, fonts, thumbnails, the parity
   reference and the legacy convergence, the honest size is about five weeks of one native lane
   (G1 to G5), plus G6 when its dependency exists. Recommendation: accept the larger number rather
   than drop the parity reference or the font work.

## 10. Out of scope

- All Swift Graphics surfaces, the command gateway, playlist, On Air strip, datasets, feeds, Shot
  Layout UI, Designer UI (roadmap M4, separate spec).
- `begin-take-transition` on the Mac Take (roadmap M1.1).
- NDI graphics key (`supportsGraphicsKey`, `D3DGraphicsKeyTap`); settled out of scope.
- Tiles wall effects on Metal (roadmap M5.1), apart from making the name-line raster retention
  work when Tiles arrives.
- Metal `supportsGradePreview` itself (per-source grade spec); G6 only adds the graphics content
  to that surface.
- Extracting the shared raster front half out of the Windows file.
- SVG import, package reading and writing, social feeds: shell work.
- Auto director, OHG, EngineHost.
