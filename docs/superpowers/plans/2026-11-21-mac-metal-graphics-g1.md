# Metal graphics backend G1: async Core Text overlay raster, legacy overlays converge, slate name label

Status: plan, 2026-11-21. Branch `mac-metal-graphics-g1` (based on fc675541, Mac Phase 1).

> **For agentic workers:** use superpowers:executing-plans. `docs/superpowers/plan-authoring-rules.md`
> is binding: each task lists named mutations and the report says which went red. When a supplied test
> contradicts a reasonable implementation, the test is the first suspect.

**Goal.** The Mac stops rastering overlay text on the render thread. Every legacy overlay the Mac ships
today (lower third, caption, brand bug, Tiles name line) and the failed-source slate name label are
prepared by one Core Text raster that runs on a persistent worker behind the shared
`graphics::PreparationCache`, and is drawn from an immutable cached `MTLTexture`. The legacy drawing
rules become the Windows rules (spec section 3.4 legacy paragraph, 3.5).

**Spec:** `docs/superpowers/specs/2026-11-15-mac-metal-graphics-backend-design.md`, slice G1 (section 6),
sections 4.2, 5.1-5.4, 5.10, 5.12. Master plan: `docs/superpowers/plans/2026-11-15-mac-parity-master-plan.md`
Phase 3 row "Metal graphics backend | G1".

**Architecture in one paragraph.** A new ObjC++ class `CoreTextOverlayRaster`
(`native/src/compositor/CoreTextOverlayRaster.{h,mm}`) is the Metal twin of `CompositorOverlayRaster`: same
public surface (`beginFrame`, `rasterOverlayTexture`, `preparationState`, `lastWarning`,
`pendingThisFrame`, `failedThisFrame`, `preparedResource`, `activeRuns`, `lease`, `refuseGpuLease`) with
`id<MTLDevice>` / `id<MTLTexture>` in place of the D3D types, the same admission rules 1-8 (spec 3.2)
copied line for line, and the same `PreparationCache<Key, Request, Asset>` (256 MiB, 32 queued, 64
resident, one worker). The render thread does a metadata check, a signature hash and a lock-free
`find`; on a miss it enqueues and draws the Windows fallback (band + accent bar). The worker builds a
`CGBitmapContext` (premultiplied BGRA, DeviceRGB, transparent), draws the legacy layout from the shared
`computeOverlayTileLayout`, creates a `MTLTexture` and uploads once. `MetalCompositorAdapter.mm` owns
one raster, deletes its inline raster (`rasterOverlayTileCoreText` and friends, the one-entry
`overlayTex_`), and adopts the D3D11 `drawOverlayLayer` legacy rules, `OverlayRasterRetention` for
`holdLastRaster`, the `graphics-preparing` / `graphics-preparation-failed` frame warnings, and
`drawFailedSlateName`. Template content (`overlay.graphicsTemplate`) is G2: the worker refuses it with
an explicit warning, and the core still refuses every `graphics-*` command on Metal
(`supportsGraphicsInstances()` stays false), so no template can reach it in G1.

---

## Defaults taken (owner may override)

| # | Default | Why | Tasks affected if overridden |
|---|---|---|---|
| D1 | **Legacy missing font falls back to Helvetica Neue.** When `appearance.enabled` and the requested family is not installed, the raster warns "Font unavailable; using Helvetica Neue." (Windows says "...using Segoe UI."); without an appearance the fallback is silent, as DirectWrite's is. Strict mode (instances only, unreachable in G1) fails with the Windows wording. | Helvetica Neue is the fallback the shipped Mac raster already used (`MetalCompositorAdapter.mm:139`). G2's owner-decided substitution table (Segoe UI to bundled Selawik, Bahnschrift / Cambria / Consolas to chosen system fonts, warn never fail) drops into the explicit, currently empty `kFontSubstitutions` table in the one resolver function `resolveRasterFont`. | 2 |
| D2 | **`holdLastRaster` retention ships in G1, not G3.** | With an async raster a Tiles reflow would otherwise draw the bare accent bar for several frames on every name line, a regression from today's synchronous Mac raster. The spec puts "Tiles name-line retention on Metal" in G3; the code is the shared `OverlayRasterRetention` template and costs nothing here. | 4 |
| D3 | **Legacy text is vertically centred on its ascent + descent**, as the shipped Mac raster did. DirectWrite centres its own line box (which may include line gap). | Parity of the vertical position is measured by G4's reference fixtures (`legacy-lower-third-appearance`, `legacy-caption`), not guessed here. | 2 |
| D4 | **Legacy text is not clipped to its line rect**, matching D2D `DrawText` with no clip option. It is still bounded by the tile. | Parity. The pixel tests prove the default layout keeps ink inside the line rects anyway. | 2 |
| D5 | **Thumbnails (`thumbnailPng`) are not in G1.** | G5 by the spec; nothing calls it on Metal. | none |
| D6 | **Legacy overlay frames report `graphics-preparing` and `health: degraded` for the frames before their raster is ready**, exactly as Windows does (`D3D11CompositorAdapter.cpp:545-547`). | Parity; a lower third's first frames on Windows already read this way. | 3 |
| D7 | **Images are re-tagged with the context colour space before drawing** (no ICC conversion) when the image is RGB; other models draw as decoded. | Spec 5.4; WIC does no conversion. | 2 |

---

## Spec corrections found while verifying (planned against the code)

1. Spec 4.2 says the band is "always painted opaque in the raster, and also as a quad". True
   (`MetalCompositorAdapter.mm:209-214`, `:1557`), and the raster's band is the *layout* background
   while the quad is the *brand* background; for an appearance-enabled lower third the two colours
   differ and the quad shows round the raster's corners. Windows draws the quad only when
   `!appearance.enabled || isCaption` and paints the rounded band in the raster otherwise. Task 3
   tests the Windows rule.
2. Windows calls `beginFrame()` in `renderMultiview` as well as `render`
   (`D3D11CompositorAdapter.cpp:968`); the spec lists only `:498`, `:522`. The Metal port resets the
   frame state around every pass (program, multiview, preview).
3. The slate name label is drawn by Windows for `failed` **and** `stalled` frameless layers
   (`D3D11CompositorAdapter.cpp:1957-1960`), not failed only as the Metal TODO says.
4. `preparedResource()` on Windows allocates a new `GraphicsPreparedResource` on every call when
   ready (`CompositorOverlayRaster.cpp:504`). Legacy layers never call it, so G1 is unaffected; G2's
   facts loop calls it per preparation per Program tick and must cache it (carried to G2).
5. The existing Metal test `OverlayRastersNonUniformBand` reads pixels from the first rendered frame.
   Under the async contract the first frame legitimately shows the fallback, so the test is updated
   to wait for readiness; this is a contract change, not a weakened check.

---

## Build and test commands

Configure exactly as CI's `native-metal-macos` job:

```
cmake -S native -B native/build-metal -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCOREVIDEO_STUB=OFF -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DCOREVIDEO_WITH_METAL=ON \
  -DCOREVIDEO_WITH_AVF_ENCODER=ON -DCOREVIDEO_WITH_AVF_MEDIA_DECODER=ON -DCOREVIDEO_WITH_COREAUDIO=ON \
  -DCOREVIDEO_WITH_AVF_CAPTURE=ON -DCOREVIDEO_WITH_SCK=ON -DCOREVIDEO_WITH_RTMP_OUTPUT=ON \
  -DCOREVIDEO_WITH_MAC_VIRTUALCAM=ON
cmake --build native/build-metal --target corevideo-native-tests
./native/build-metal/corevideo-native-tests --gtest_filter='MetalCompositor.*:MetalOverlayRaster.*'
ctest --test-dir native/build-metal --output-on-failure -LE realtime
```

Stub gate: `cmake -S native -B native/build-stub -G Ninja && cmake --build native/build-stub && ctest
--test-dir native/build-stub --output-on-failure`. The runner has no `--gtest_repeat`; repeat in a shell
loop. Timing tests are load-sensitive (other agents build on this machine): rerun before concluding.

---

## Task 1: this plan

Commit the plan.

## Task 2: `CoreTextOverlayRaster`, legacy branch, standalone

**Files:** new `native/src/compositor/CoreTextOverlayRaster.h`, `.mm`; new
`native/tests/MetalOverlayRasterTest.mm`; `native/CMakeLists.txt` (both `.mm` files, `-fobjc-arc`, the
test only when `COREVIDEO_WITH_METAL AND NOT COREVIDEO_STUB`); `native/src/compositor/CompositorFaultInjection.h`
(the prepare hook).

**Interfaces.**

```cpp
// CompositorFaultInjection.h (portable, test-only, same guarding as the monitor stall)
using OverlayRasterPrepareHookFn = void (*)();
void setOverlayRasterPrepareHookForTest(OverlayRasterPrepareHookFn fn) noexcept;
bool overlayRasterPrepareHookArmed() noexcept;          // one relaxed load
void invokeOverlayRasterPrepareHook() noexcept;         // called once at the start of every preparation

// CoreTextOverlayRaster.h (ObjC++ only; gated !STUB && DEV_ADAPTERS && WITH_METAL)
class CoreTextOverlayRaster {
 public:
  void beginFrame();
  bool pendingThisFrame() const; bool failedThisFrame() const;
  const char* preparationState() const;            // unprepared | preparing | ready | failed
  const std::string& lastWarning() const;
  std::shared_ptr<const void> lease() const;
  std::shared_ptr<const graphics::GraphicsRunLayout> activeRuns() const;   // null until G2
  std::shared_ptr<const graphics::GraphicsPreparedResource> preparedResource();  // backend Metal
  void refuseGpuLease();                           // surface parity; Metal never calls it
  id<MTLTexture> rasterOverlayTexture(id<MTLDevice>, const CompositorOverlayContent&,
      const compositor::LayerRect& rect, int targetWidth, int targetHeight,
      const std::string& preparationSlot = {}, bool strictResources = false,
      std::shared_ptr<const graphics::GraphicsPreparedResource> prepared = {});
  static id<MTLTexture> textureOfLease(const std::shared_ptr<const void>& lease);
  graphics::PreparationCache<...>::Counters cacheCounters() const;          // tests + diagnostics
};
```

**Behaviour.**
- `rasterOverlayTexture` reproduces `CompositorOverlayRaster.cpp:414-495` exactly: the 19-field 64 KiB
  metadata bound and its wording; the 1..4096 dimension rule and wording; `Key{registryID,
  signature ^ strict-salt, contentHash, hasContentHash}`; prepared-resource compatibility with
  `Backend::Metal`; the admission estimate (incl. the layered-template atlas bound and +128 MiB for an
  `imageUri`); scale retention; the 16-revision capacity wording. On a cache hit it performs **no heap
  allocation** (the `Request` is built only on a miss).
- The worker (`prepareRaster`) runs in an `@autoreleasepool`, calls the prepare hook first, refuses a
  template with "Template graphics are not prepared on Metal yet." (G2), resolves the font through
  `resolveRasterFont` (exact family, then the empty `kFontSubstitutions` table, then D1), opens a
  packaged `graphicsImage` (<= 32 MiB encoded, decoded size must equal the declared size) or a local
  `imageUri` (no `http`; <= 4096 square, <= 16 MP), reserves `GraphicsResourceBudget::process()` exactly
  as Windows (retryable refusal wording), and draws into a zeroed premultiplied BGRA bitmap:
  - band: only when `appearance.enabled && !isCaption`, `layout.backgroundArgb`, rounded by
    `layout.cornerRadius` clamped to half the shorter side;
  - accent bar always (`layout.accentArgb`);
  - image aspect-fitted and centred in `layout.imageRect`; a failed decode is incomplete with
    " Logo unavailable; image omitted.";
  - each text line: em = `line.fontSize` if > 0 else `max(4, 0.72 * rect.height)`; one line; leading;
    vertically centred (D3); truncated at the end with "…" when wider than `rect.width`; weight
    semibold for line 0 when `!isCaption || !speaker.empty()`, else regular.
  - strict + incomplete fails with the worker warning. Output texture `MTLPixelFormatBGRA8Unorm`,
    shader-read, written once, then immutable.

**Tests (`MetalOverlayRasterTest.mm`, suite `MetalOverlayRaster`; all read the texture back with
`getBytes`, full resolution).** A helper polls `rasterOverlayTexture` (with `beginFrame`) every 2 ms
for up to 5 s until `ready` or `failed`.
1. `LowerThirdGlyphsLandInTheirLineRectsAndNowhereElse` — 600x120 tile, `text="ADA OTIENO"`,
   `title="AETHELRED LABS"`, accent `#ff0000`. Fixture-derived constants: the line rects from
   `computeOverlayTileLayout(overlay, 600, 120)` (name at y 0.18h..0.52h, title 0.56h..0.84h, x from
   0.06w to 0.88w), accent bar `x < 0.04w`. Asserts: each line rect holds >= 150 pixels with alpha > 128;
   every pixel with alpha > 8 lies inside a line rect or the accent bar; the accent bar is exactly
   premultiplied `#ff0000` opaque; a pixel between the lines and right of the column is alpha 0 (no band).
2. `NameLineIsSemiboldAndTitleIsRegular` — same text on both lines (`text` and `org` equal strings
   rendered as two lines of identical rect height via an appearance with `nameSize == titleSize`):
   summed alpha of line 0 exceeds line 1 by >= 8%.
3. `AnOverflowingLineEndsInAnEllipsisNotAClippedGlyph` — `text` of 60 `M`s in a 300 px tile. The
   rightmost ink column is inside the line rect; the ink in the last `0.3 * em` columns spans at most
   35% of the em vertically (an ellipsis is three dots on the baseline; a clipped `M` stem spans ~70%).
4. `AppearanceBandIsRoundedAndCarriesTheLayoutBackground` — appearance enabled, `cornerRadius` 16,
   `backgroundColor #204060`, opacity 1: pixel (1,1) alpha 0; pixel (w/2, h-3) equals premultiplied
   `#204060`; no appearance: the same pixel is alpha 0.
5. `ACaptionDrawsNoBand` — caption with appearance enabled: pixel in the body area outside text is alpha 0.
6. `ACacheHitAllocatesNothing` — after ready, 100 calls of `beginFrame` + `rasterOverlayTexture` under
   a thread-local `operator new` counter (this test file replaces global `operator new`/`delete` as a
   malloc pass-through that counts only while armed on the calling thread): count == 0, and the same
   `id<MTLTexture>` is returned each time.
7. `AdmissionRulesMatchWindowsWording` — oversize metadata (70000-byte title) and 5000 px rect: state
   `failed` with the exact Windows strings; a template overlay: `failed` with the G2 wording.
8. `AMissingFontWithAnAppearanceWarnsAndStillDraws` — family "NoSuchFamily CVP": `ready`, warning
   "Font unavailable; using Helvetica Neue.", text pixels present; strict: `failed`, "Requested graphics
   font is unavailable; choose an installed font."

**Mutations (each must go red, report results):** (a) draw the band regardless of appearance -> 1, 4
red; (b) drop the truncation -> 3 red; (c) weight regular for line 0 -> 2 red; (d) build the `Request`
before `find` -> 6 red; (e) clip text to nothing (skip text) -> 1 red; (f) skip the radius -> 4 red.

## Task 3: the Metal adapter uses the raster; the inline raster is deleted

**Files:** `MetalCompositorAdapter.mm`, `tests/MetalCompositorTest.cpp`.

**Red first.** Before switching, wire `invokeOverlayRasterPrepareHook()` into today's inline
`rasterOverlayTexture` (top of the miss path) and write tests 1-2 below; run them and record red
(the inline raster blocks the tick for the hook's stall; two overlays thrash the one-entry cache).

**Behaviour.** One `CoreTextOverlayRaster overlayRaster_`. `beginFrame()` before every pass and after
the program pass's warnings are read. `drawOverlayLayer` follows `D3D11CompositorAdapter.cpp:2064-2215`
for legacy content: key transform; `rasterOverlayTexture(device_, overlay, rect, targetW, targetH, {},
!graphicsInstanceId.empty(), graphicsPreparedResource)`; an instance layer with no raster draws nothing;
band quad only when `!appearance.enabled || isCaption`; the raster composited through the premultiplied
pipeline over the animated rect; else the accent-bar fallback. Program frame warnings
`graphics-preparing` / `graphics-preparation-failed` and `health = degraded` (D6). Deleted:
`createColorFromArgb`, `bottomUpRect`, `drawOverlayImageWithImageIo`, `drawOverlayTextLineCoreText`,
`rasterOverlayTileCoreText`, `OverlayTex`, `overlayTex_`, the straight-to-premultiplied fallback loop,
and the CoreText/ImageIO includes from the adapter.

**Tests (`MetalCompositor.*`).** Canvas 320x180 so the 320x180 preview readback is pixel-exact.
1. `ABlockedOverlayPreparationNeverStallsTheRenderTick` — hook stalls every preparation up to 1.5 s
   until released. 20 renders with a lower third: every render returns in < 50 ms, each frame carries
   `graphics-preparing`, `health == degraded`, and the overlay region shows the fallback accent bar
   (brand colour at `x = rect.x + 0.01`) and no text. Release; within 2 s a frame has no
   `graphics-preparing` and text pixels appear in the name line rect.
2. `TwoOverlaysAndTwoPassesRasterOncePerContent` — hook counts. Two different lower thirds in Program
   and the same two in `renderPreview` (same canvas size), 30 ticks after both are ready: the counter
   equals exactly 2.
3. `LegacyLowerThirdTextLandsInsideItsBand` — 320x180, overlay rect (0, 0.5, 1, 0.5) over a solid black
   layer, no appearance: after readiness, bright pixels (R,G,B > 200) exist inside the name line rect
   (from `computeOverlayTileLayout(overlay, 320, 90)` offset by 90 rows) and none outside the overlay
   rect; the band colour (brand background `#0c1118`) fills band pixels away from text.
4. `AnAppearanceBandIsDrawnOnceNotTwice` — appearance enabled, `backgroundOpacity 0.5`, over a solid
   white layer: a band pixel away from text equals one 50% blend (within 3/255), not two.
5. `OverlayRastersNonUniformBand` (existing) — waits for readiness before sampling.

**Mutations:** (a) call the old synchronous path (prepare inline) -> 1 red; (b) skip `beginFrame` in
the preview pass -> none expected red (state only); report; (c) draw the band quad unconditionally ->
4 red; (d) drop the warnings -> 1 red; (e) key the cache without the size -> 3 or 2 red.

## Task 4: `holdLastRaster` retention (D2)

**Files:** `MetalCompositorAdapter.mm`, `tests/MetalCompositorTest.cpp`.

**Behaviour.** `compositor::OverlayRasterRetention<const void>` keyed by `layerId`, holding the raster
lease and the bridged texture pointer; pruned once per program frame. Exactly the D3D11 block at
`:2107-2122`: draw the fresh raster, else the retained one scaled through the viewport, else nothing
(never the accent bar) for a `holdLastRaster` layer.

**Test.** `AHeldNameLineKeepsItsTextWhileTheResizedRasterPrepares` — a `holdLastRaster` overlay ready
at rect A; arm the stalling hook; change to rect B (different pixel size): frames show text pixels in
rect B (the old raster scaled) and no accent-bar-only frame; a non-held overlay under the same change
shows the accent bar (control). **Mutation:** remove the retention -> red.

## Task 5: slate name label (`TODO(4a-metal-text)`)

**Files:** `MetalCompositorAdapter.mm`, `tests/MetalCompositorTest.cpp`.

**Behaviour.** Port `drawFailedSlateName` (`D3D11CompositorAdapter.cpp:1968-2049`): a frameless layer
whose `sourceHealth` is `failed` or `stalled` and whose `sourceDisplayName` is non-empty gets a label
bottom-left: height 8% of the layer, width `min(40% of layer, 0.55 * labelHeightPx * chars +
labelHeightPx)`; content `title = name`, `brandColor = #<kFailedSlateRgba>`; drawn through the
premultiplied pipeline, scissored to the layer (or its clip rect), no key transform.

**Test.** `AFailedSlateCarriesItsNameBottomLeftAndNothingElse` — 320x180, full-canvas failed layer named
"CAM 2". After readiness: pixels inside the label rect with R,G,B > 200 >= 20; every pixel outside the
label rect equals the failed slate colour (within 2/255); a `stalled` layer also shows the label; a
`live` frameless layer does not. **Mutations:** skip the label -> red; draw it for every health -> the
`live` control red; label full-height -> the "nothing else" assertion red.

## Task 6: verification and outcomes

- Metal suite (`MetalCompositor.*:MetalOverlayRaster.*`) repeated 5x in a shell loop; full Metal ctest
  `-LE realtime`; stub ctest.
- `python3 scripts/mac-show-drill.py --seconds 40 --load 8` (Release core, `COREVIDEO_FAKE_ENGINE_FPS=60`)
  on this branch, compared with the same drill on fc675541 if time and machine load allow; advisory
  under load, stated as such.
- Outcomes doc `docs/superpowers/plans/2026-11-21-mac-metal-graphics-g1-outcomes.md`: commits, counts,
  proven vs unverified, defaults, and what G2 needs.

## Carried to G2 (written here so G2 reads it as a checklist)

1. Template branch of `prepareRaster` (spec 3.3/3.4), atlas runs, `activeRuns()`.
2. Fill `kFontSubstitutions` with the owner's table (Segoe UI to Selawik after the OFL check;
   Bahnschrift, Cambria, Consolas to chosen system fonts; warn, never fail) and add bundled
   registration; `resolveRasterFont` is the single seam.
3. Cache the `GraphicsPreparedResource` so the per-tick facts loop does not allocate (correction 4).
4. Adapter rules 5.8/5.9: preparations loop + facts, bound-source record, `graphicsAwaitingEntrance`,
   then `supportsGraphicsInstances()` true.
