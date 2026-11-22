# Metal graphics backend G2: outcomes

Plan: `docs/superpowers/plans/2026-11-21-mac-metal-graphics-g2.md`. Branch `mac-metal-graphics-g1` (kept by
instruction), on top of G1. Date 2026-11-21. This Mac (Apple Silicon), other agents building throughout
(load average 6-9).

## Commits

| sha | task | what |
|---|---|---|
| 3e5cea54 | 1 | plan (licence verified before planning) |
| eddf6985 | 2 | font resolver with the owner's table, bundled Selawik + OFL, bundle script |
| ac5c8686 | 3 | `measureText` uses Core Text on a Metal build |
| bb9112b1 | 4 | template raster on the Core Text worker (shapes, gradients, images, text, effects, atlas) |
| 2c7c38f8 | 5 | adapter rules 5.8/5.9, prepared-resource memo, `supportsGraphicsInstances()` true, `GraphicsRpcPixelTest` on Metal |
| (this) | 6 | outcomes |

## Font policy as shipped

- **Selawik 1.01** from `https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip`
  (zip SHA-256 `3f62c51e05e3b5a1e6241cf92a371f0be2ea1183aa87b30718bbd40832a8d423`). Licence verified **SIL OFL 1.1**
  (`LICENSE.txt` at the 1.01 tag, Reserved Font Name "Selawik"), shipped verbatim as `OFL.txt`. Three faces
  unmodified: Regular `e9d98518…0ee1c`, Semibold `0a9e9d05…7783`, Bold `f0db5e17…c904` (full values in
  `native/resources/graphics-fonts/SOURCE.md`, pinned by `GraphicsFontResolver.BundledFilesMatchTheirPinnedChecksums`).
- **Table:** Segoe UI and Segoe UI Semibold (pinned 600) → Selawik; Bahnschrift → Avenir Next Condensed (DIN
  Alternate is the closer letterform but macOS ships it Bold only, so every weight would draw bold and wider);
  Cambria → Georgia; Consolas → Menlo (about 9% wider advance: 0.602 em vs 0.55 em).
- **Warn, never fail, strict included,** for a table hit: "Font <requested> is not installed on this Mac; using
  <family>." reaches `instances[].warning` (seen end to end from the bundled core, below).
- **Registration:** lazy, once per process, `CTFontManagerRegisterFontsForURL` with process scope, from
  `GraphicsFonts/` beside the executable (`Contents/Resources/GraphicsFonts` in the .app; CMake copies into every
  macOS build dir; `COREVIDEO_GRAPHICS_FONTS_DIR` overrides).
- **Reaches the .app and is signed with it:** an ad-hoc `scripts/make-macos-shell-bundle.sh` run into a scratch
  dir produced `Contents/Resources/GraphicsFonts/{selawk,selawksb,selawkb}.ttf,OFL.txt` with the pinned checksums;
  `codesign --verify --deep --strict` reports "valid on disk / satisfies its Designated Requirement" (fonts are
  sealed resources, no code to sign). `make-macos-pkg.sh` packages that .app unchanged. The notarized release was
  **not** run (by instruction).
- **Bundled core, end to end:** the bundled `Contents/Resources/corevideo-native`, driven over stdio with
  `COREVIDEO_GRAPHICS_FONTS_DIR` unset, advertised `graphics-instances`, prepared a `compact-solid` with
  `fontFamily: "Segoe UI"` to `ready`, drew it (`firstDrawSubmittedFrame: 5`), warned "Font Segoe UI is not
  installed on this Mac; using Selawik." and published `programResource.backend: "metal"`.

## Tests

| suite | result | command |
|---|---|---|
| G2 + G1 graphics suites | 85/85, 5x loop green | `./native/build-metal/corevideo-native-tests --gtest_filter='MetalCompositor.*:MetalOverlayRaster.*:GraphicsFontResolver.*:GraphicsTextMeasure.*:GraphicsCompositionPixel.*:GraphicsRpcPixel.*'` |
| full Metal ctest `-LE realtime` | 4/4 entries; 2155 gtest cases in the binary (10 realtime excluded) | `ctest --test-dir native/build-metal --output-on-failure -LE realtime` (CI configure flags incl. `-DCOREVIDEO_WITH_MAC_VIRTUALCAM=ON`) |
| stub ctest | 5/5 entries; 2020 gtest cases | `cmake -S native -B native/build-stub -G Ninja && cmake --build native/build-stub && ctest --test-dir native/build-stub --output-on-failure` |
| shell tests | none touched (the shell was built only to assemble the bundle check) | |

New: `GraphicsFontResolver.*` (4), `GraphicsTextMeasure.*` (3), `MetalOverlayRaster.{SegoeUiResolvesToBundledSelawik…,
TemplateShapes…, TemplateGradient…, TemplateTextBaselines…, TemplateCenterAlignment…, TemplateTextEffects…,
TemplateFonts…, ALayeredTemplatePreparesAnAtlas…, PreparedResourceIsStableAndAllocationFree}`,
`GraphicsRpcPixel.MetalCoreAdvertisesGraphicsAndACuedLowerThirdReachesProgramPixels`. Re-gated to run on Metal:
the whole of `GraphicsRpcPixelTest.cpp` except the private preview and thumbnail tests (D3D11-only, G6/G5):
22 tests, groups B, C **and D** all green on Metal.

## Red before green, and mutations

- Task 2: suite did not compile before the resolver. Mutations: drop the Segoe UI row (3 red), skip registration
  (3 red), fail a strict table substitution (red), ignore weight (2 red).
- Task 3: `TemplateLayoutUsesCoreTextOnMetal` failed before the branch (both 10-character crawl items 111 px).
- Task 4: all 8 template raster tests failed before the port. Mutations: baseline from ascent (red), no ellipsis on
  the last wrapped line (red), swapped gradient (red), no shadow (red), alignment ignored (red), no atlas (red).
- Task 5: on Metal **20 of 22** re-gated tests failed before the adapter port. Mutations: capability false (2 red),
  no preparations loop (8 red), no `draw-submitted` (4 red), no bound-source rule (2 red), crawl distance ignored
  (red). `PreparedResourceIsStableAndAllocationFree` failed before the memo.
- Fixture defects found and fixed: (1) the gradient fixture used `{"color":…}` stops, the schema takes colour
  strings; (2) raster-level template fixtures without a content hash shared one cache key (the overlay signature does
  not hash template content; real instances always carry the hash), so the second fixture read the first's result.

## Two Metal-specific test expectations (both commented in the test)

1. `ReviewedSocialCardsRenderOnlyOnExplicitShowAndRetire`: the card's 22-unit source line is 3.7 px tall on the 320x180
   test canvas. DirectWrite hints stems to whole pixels; Core Text does not, and peak pink coverage stays near 0.6
   (row max r = 140, threshold r > 140). On Metal the brightness floor is 110 with the same rows and the same hue test;
   the plate and the white lines still cannot pass it. This is a real rasterizer difference at tiny sizes, recorded for
   G4, not hidden.
2. `AcceptedRasterSurvivesDraftCachePressureAndANewRendererWithoutAnotherShow`: a second Metal compositor shares the
   GPU's `registryID` (stable per process), so the device identity is EQUAL, where a new D3D11 compositor creates a new
   device. The pixel recovery assertion is unchanged.

## Proven

- A Metal core advertises `graphics-instances`, accepts `graphics-*` (no `graphics-instance-backend-not-built`), and a
  cued lower third, shown, reaches Program pixels in the lower 60% only, while the cue alone leaves Program untouched
  (`MetalCoreAdvertisesGraphicsAndACuedLowerThirdReachesProgramPixels`, through `MediaCore` + `JsonRpcServer`).
- Every curated look, fades and reversals, text effects, shapes, verified package images and logos, alpha-preserving PNGs,
  tables (with accepted pixels kept on failure), crawls clipped both ways, clocks, social cards, local camera/screen
  bindings that reject stale source generations, automatic introductions, failed-font retention and repair, cache
  pressure, cue/program separation and the eight-instance limit: the Windows pixel tests, unchanged except the two
  notes above, green on Metal.
- Template layout measures with Core Text; raster and measurement share one resolver and one line breaker.
- `preparedResource()` returns one identity per prepared texture with zero allocations per tick once held.

## Unverified

- **Cross-rasterizer parity** (advance widths, baselines against D2D, Selawik vs Segoe UI on air): G4 needs the
  Windows reference file. The owner judges Selawik on air from G4's side-by-side sheet.
- **Drill pair against fc675541: not run.** Load average stayed 7-9 (other agents) for the whole session, and G1's
  four runs showed both configurations failing intermittently at that load. One `--load 8 --lower-third` run on the G2
  branch PASSED every gate (60.0 fps, 5.0 ms hold, worst frame 18.5 ms, 0 dropped, 99% delivered, coreMutex 1%):
  a single advisory sample, not a comparison. There is still no drill with a **template** graphics cue (G4's
  `--graphics-cue`).
- Gradient alpha interpolation (D5), colour emoji (D6), image filtering constant: G4 fixtures.
- The notarized-build path (by instruction).

## Defaults taken (owner may override)

D1 fonts in `GraphicsFonts/` beside the core (not the shell's `Fonts/`). D2 lazy registration. D3 an unknown family
(not installed, not in the table) still fails strict with the Windows wording; only table hits are warn-never-fail.
D4 no warning for a missing heavier face. D5 gradient alpha as Core Graphics. D6 colour emoji in colour.
D7 `preparedResource()` memo is Mac-only; **Windows still allocates per call, flagged for a Windows session**
(`CompositorOverlayRaster.cpp:504`). D8 private preview and thumbnails stay off on Metal. **D9 (new):** template
text under 16 device px per em draws with Core Graphics font smoothing (stem dilation), the closest analogue of
DirectWrite's hinted, contrast-enhanced grayscale; larger text keeps true weight so G4 measures it honestly.

## Installed / downloaded

Nothing installed (no Homebrew packages needed). One download, in its own new directory
(`$TMPDIR/…/scratchpad/selawik-dl/`): the Selawik 1.01 release zip and the 1.01 `LICENSE.txt`, checksums above; only
the three TTFs and the licence were committed.

## What G3 to G6 need

- G3: group D already passes on Metal (failed-font repair, cache pressure + new renderer, cue separation, eight
  instances). Remaining G3 scope is Tiles name-line retention under load and device identity on an eGPU.
- G4: the Windows reference file; a `--graphics-cue` drill; decide D5/D9 from fixtures (the social-card floor note is
  the first data point).
- G5: `thumbnailPng` on Metal (the worker already rasters templates; it needs the PNG encode and the crawl
  thumbnail placement).
- G6: Metal `supportsGradePreview`, then the private preview test can be re-gated.
