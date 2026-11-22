# Metal graphics backend G1: outcomes

Plan: `docs/superpowers/plans/2026-11-21-mac-metal-graphics-g1.md`. Branch `mac-metal-graphics-g1`
(base fc675541). Date 2026-11-21. Hardware: this Mac (Apple Silicon), other agents building in
parallel during every measurement.

## Commits

| sha | task | what |
|---|---|---|
| cd13d991 | 1 | plan |
| b90ab58a | 2 | `CoreTextOverlayRaster` (async Core Text twin of `CompositorOverlayRaster`), prepare hook, `MetalOverlayRaster.*` |
| 2351f396 | 3 | adapter draws from the raster; render-thread raster deleted; Windows band/fallback/warning rules |
| 57cc4f51 | 4 | `holdLastRaster` retention (Tiles name lines) |
| a337df68 | 5 | slate name label (`TODO(4a-metal-text)` closed) |
| (this) | 6 | drill `--lower-third`, outcomes |

## Tests

| suite | count | command |
|---|---|---|
| Metal overlay + compositor | 46 (9 `MetalOverlayRaster.*` new, 7 `MetalCompositor.*` new, 1 updated) | `./native/build-metal/corevideo-native-tests --gtest_filter='MetalCompositor.*:MetalOverlayRaster.*'` (5x loop green) |
| full Metal ctest, `-LE realtime` | 4/4 ctest entries; 2116 gtest cases in the binary, the 10 realtime-labelled ones excluded | `ctest --test-dir native/build-metal --output-on-failure -LE realtime` |
| stub ctest | 5/5 ctest entries (incl. the realtime-labelled one), 2014 gtest cases | `cmake -S native -B native/build-stub -G Ninja && cmake --build native/build-stub && ctest --test-dir native/build-stub --output-on-failure` |
| shell tests | none touched | |

New tests: `MetalOverlayRaster.{LowerThirdGlyphsLandInTheirLineRectsAndNowhereElse,
NameLineIsSemiboldAndTitleIsRegular, AnOverflowingLineEndsInAnEllipsisNotAClippedGlyph,
AppearanceBandIsRoundedAndCarriesTheLayoutBackground, ACaptionDrawsNoBand, ACacheHitAllocatesNothing,
ATileIsPreparedPerPixelSize, AdmissionRulesMatchWindowsWording, AMissingFontWithAnAppearanceWarnsAndStillDraws}`;
`MetalCompositor.{ABlockedOverlayPreparationNeverStallsTheRenderTick, TwoOverlaysAndTwoPassesRasterOncePerContent,
LegacyLowerThirdTextLandsInsideItsBand, AnAppearanceBandIsDrawnOnceNotTwice,
AHeldNameLineKeepsItsTextWhileTheResizedRasterPrepares, AFailedSlateCarriesItsNameBottomLeftAndNothingElse}`.
Updated: `MetalCompositor.OverlayRastersNonUniformBand` waits for readiness (async contract).

## Red before green, and mutations

- Task 2: suite did not compile before the class existed. Mutations: band regardless of appearance
  (4 red), no truncation (red), hard clip instead of ellipsis, i.e. the old Mac behaviour (red: "last
  glyph spans 11 rows"), regular weight for line 0 (red), Request copy on the hit path (red, after
  a fixture fix, below), no text (4 red), no corner radius (red).
- Task 3: with the prepare hook wired into the OLD inline raster, `ABlockedOverlay...`,
  `TwoOverlays...` and `AnAppearanceBand...` failed. Mutations: render thread waits for the raster
  (red), band quad unconditional (red), warnings dropped (4 red), no `beginFrame` around monitor passes
  (green, expected: state only), cache key without size (green on the compositor tests; added
  `ATileIsPreparedPerPixelSize`, which reds).
- Task 4: retention test failed before the port and with the retention removed.
- Task 5: slate test failed before the port; label for every health (red on the live control),
  full-height label (red), label as wide as the layer (initially GREEN; added the text-column
  assertion, now red).

Fixture defects caught by mutation (plan-authoring rules 7, 10): (1) the allocation test used strings
short enough for libc++'s small-string buffer, so a copied `Request` on the hit path allocated nothing
and passed; the fixture now uses strings longer than 22 bytes. (2) The slate test did not constrain
the label's width; the label-as-wide-as-the-layer mutation passed until the text-column check. (3) The
glyph test needed one pixel of antialiasing bleed: the "A" of "ADA" inks one pixel left of its pen
origin (real, and true of D2D too). Also found: my mutation harness restored files with their old
mtime, so ninja kept the mutated object; fixed (touch after restore) and every green re-verified.

## Proven

- No overlay raster on the render tick: with the worker blocked for 1.5 s, 20 renders each took
  < 4 ms (worst 1.9-3.3 ms over runs), reported `graphics-preparing`, drew the accent-bar fallback and
  no text; text arrives after release.
- One preparation per distinct content across two overlays, Program + Preview passes, 30 ticks.
- A cache hit performs zero `operator new` calls (thread-local counter, 100 calls).
- Pixel proofs at full resolution: glyph coverage inside the layout's line rects and none outside
  them (raster level, 600x120 readback via `getBytes`); text on Program inside the band at 320x180
  (pixel-exact preview); one 50% band, not two; rounded band corners transparent; caption has no
  band; ellipsis, not a clipped glyph; semibold first line; slate label glyphs inside the label and
  every other pixel exactly the slate colour (1280x720 full-res Program readback).
- Windows admission wording for metadata and dimension refusals.

## Unverified

- Visual parity with D2D (vertical position D3, weights, advance widths): G4's reference fixtures.
- **Frame delivery with a lower third cycling: not established.** New drill flag `--lower-third`
  (new text every second, hide/re-show every five). Four 40 s runs, this branch, Release core,
  `--load 8`, `COREVIDEO_FAKE_ENGINE_FPS=60`, load average 6-7 from other agents:
  baseline (no overlay) FAILED twice on decoded-frame delivery (92%, then 82%), every other gate
  passing; `--lower-third` PASSED once (60.0 fps, 5.1 ms hold, worst 20.4 ms, 0 dropped, 99%
  delivered) and FAILED once (3 dropped, worst 26.2 ms, p99 93.8 ms vs 93.3 budget). Both
  configurations fail intermittently under this load, so the drill cannot judge G1 here (same posture
  as `docs/reference/gotchas.md`: a loaded machine cannot gate a timing property). Needs a quiet-machine
  pair, and ideally a fc675541 baseline. The drill also does not assert the overlay was composited;
  that is proven only by the Metal pixel tests.
- `COREVIDEO_SHELL_SNAPSHOT` before/after capture of the shipped lower third (spec 7.5): not taken.
  The look changes deliberately: ellipsis, semibold name line, rounded band for an appearance, no
  doubled band, and "Font unavailable; using Helvetica Neue." on every appearance lower third that
  asks for Segoe UI (the appearance default) until G2's table maps it to Selawik.
- Package images (`graphicsImage`) and file images: code ported, no pixel test in G1 (G2 group B
  covers package images).

## Defaults taken (owner may override)

D1 legacy missing font falls back to Helvetica Neue (warning only with an appearance; strict fails
with the Windows wording). D2 `holdLastRaster` retention in G1, not G3. D3 vertical centring on ascent
+ descent. D4 no per-line clip (D2D parity). D5 no thumbnails (G5). D6 legacy overlays report
`graphics-preparing` / `degraded` while preparing, as Windows does. D7 RGB images re-tagged to the
context colour space. Full text in the plan.

## What G2 needs

1. `resolveRasterFont` in `CoreTextOverlayRaster.mm` is the single font seam. Drop the owner's table
   into `kFontSubstitutions` (Segoe UI and Segoe UI Semibold to bundled Selawik after the OFL check;
   Bahnschrift, Cambria, Consolas to the chosen system fonts) and add bundled registration. A table
   hit already produces "Font X is not installed on this Mac; using Y." and never fails, including
   in strict mode. Note: the legacy path only consults fonts when `appearance.enabled` (Windows rule);
   the template path will call the same resolver per text primitive.
2. Template branch of `prepareRaster` (it currently returns "Template graphics are not prepared on
   Metal yet."), atlas runs, `activeRuns()`. The admission estimate for layered templates is already
   ported.
3. `preparedResource()` allocates per call when ready (as Windows). G2's facts loop calls it per
   preparation per Program tick: cache it.
4. Adapter rules 5.8/5.9: preparations loop + facts, bound-source record, `graphicsAwaitingEntrance`,
   then `supportsGraphicsInstances()` true. The strict flag and prepared resource already flow into
   `rasterOverlayTexture`; an instance layer with no raster already draws nothing.
5. The prepare hook (`setOverlayRasterPrepareHookForTest`) is available for G2's blocked-preparation
   and capacity tests. The plan's `overlayRasterPrepareHookArmed()` was not needed (the slot itself is
   the armed state).
