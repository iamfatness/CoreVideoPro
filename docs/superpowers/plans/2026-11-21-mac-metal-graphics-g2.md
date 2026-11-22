# Metal graphics backend G2: template raster, text measurement, font resolver, facts, capability on

Status: plan, 2026-11-21. Branch `mac-metal-graphics-g1` (kept by instruction), on top of G1 (0afd8001).

> **For agentic workers:** use superpowers:executing-plans. `docs/superpowers/plan-authoring-rules.md`
> is binding: each task names mutations and the report says which went red. When a supplied test
> contradicts a reasonable implementation, the test is the first suspect.

**Goal.** A Metal core accepts `graphics-*` commands and puts template graphics on Program the way a
D3D11 core does: `supportsGraphicsInstances()` is true, templates are rastered with Core Text on the
G1 preparation worker, text is measured with Core Text so layout and raster agree, Windows font
families resolve through the owner's substitution table (Segoe UI to bundled Selawik), and the Metal
adapter emits the facts and warnings `GraphicsInstances` consumes.

**Spec:** `docs/superpowers/specs/2026-11-15-mac-metal-graphics-backend-design.md` slice G2 (section 6),
sections 3.2-3.6, 5.2-5.9, 5.11, 7.2 groups B and C. G1 outcomes:
`docs/superpowers/plans/2026-11-21-mac-metal-graphics-g1-outcomes.md` ("What G2 needs" is walked
below, rule 6).

**Owner decision (2026-11-21), font policy.** Segoe UI to **bundled Selawik**, after confirming SIL OFL
1.1, with the source URL, version and a pinned checksum recorded and the OFL text shipped in the app;
registered at runtime from the bundle with process scope. Bahnschrift, Cambria and Consolas to the
closest system fonts, reasons recorded. **A table substitution warns and never fails, strict mode
included.**

---

## Licence and provenance (verified before planning)

- Source: `https://github.com/microsoft/Selawik/releases/download/1.01/Selawik_Release.zip`, release
  **1.01** (published 2015-12-08). Zip SHA-256
  `3f62c51e05e3b5a1e6241cf92a371f0be2ea1183aa87b30718bbd40832a8d423`.
- Licence: `https://raw.githubusercontent.com/microsoft/Selawik/1.01/LICENSE.txt` — "This Font Software
  is licensed under the SIL Open Font License, Version 1.1", Reserved Font Name "Selawik". We ship the
  fonts unmodified, under their own name, with the licence text: OFL-compliant.
- Shipped faces (family name "Selawik", read with Core Text): `selawk.ttf` Regular (weight trait 0.0,
  SHA-256 `e9d98518d8ac2817782a9a382430463a2e0793ea68350b695bb727d9a830ee1c`), `selawksb.ttf` Semibold
  (0.3, `0a9e9d0549a10f24bef9b3a29e06fe6e0b5c21e7a784c503b048a307841a7783`), `selawkb.ttf` Bold (0.4,
  `f0db5e174a90e0956ad7d2844bdca1d5e6da92ec65b2c04e57ba9b180668c904`). Light and Semilight are not
  shipped (no 300 weight is offered by the builder).

## Substitution table

| Requested | Mac family | Why |
|---|---|---|
| Segoe UI, Segoe UI Semibold (600) | Selawik (bundled) | owner decision; Microsoft's metric-compatible open fallback for Segoe UI |
| Bahnschrift | Avenir Next Condensed | Bahnschrift is a narrow DIN-style grotesque with a full weight range. DIN Alternate is the closest letterform but macOS ships it in **Bold only**, so every weight would draw bold and widen. Avenir Next Condensed ships Regular, Medium, Demi Bold and Bold, which map 400/500/600/700, at a comparable narrow width. |
| Cambria | Georgia | transitional serif with a similarly large x-height; ships Regular and Bold (the spec's proposal). Charter was the alternative. |
| Consolas | Menlo | the macOS system monospace, Regular and Bold. Advance is 0.602 em against Consolas' 0.55 em, so monospaced text is about 9% wider: a column sized on Windows can ellipsize sooner on a Mac (named, measured by G4). SF Mono is not exposed to apps. |

Installed on this Mac (checked): Arial, Verdana, Trebuchet MS, Georgia; Segoe UI, Bahnschrift, Cambria,
Consolas and Inter are not.

---

## Defaults taken (owner may override)

| # | Default | Why | Tasks |
|---|---|---|---|
| D1 | **Fonts live in `GraphicsFonts/` beside the core binary** (`Contents/Resources/GraphicsFonts` in the .app; `native/build-*/GraphicsFonts` in a build, copied by CMake at configure). Override: `COREVIDEO_GRAPHICS_FONTS_DIR`. Not `Contents/Resources/Fonts`, which the shell registers via `ATSApplicationFontsPath` for its own UI. | Keeps the core's graphics fonts out of the shell's UI font set; one rule for build, test and bundle. | 2 |
| D2 | **Registration is lazy, once per process** (first font resolution), not an explicit call at core start. | Works identically for the core, the tests and any worker; no new launch argument. | 2 |
| D3 | **A family that is neither installed nor in the table still fails in strict mode** with the Windows wording "Requested graphics font is unavailable: <family>"; legacy (non-strict) falls back with a warning as in G1. Only a **table** substitution is warn-never-fail. | The owner's rule is about the table. Failing an unknown family is what lets an operator see a typo, and `FailedFontPreparationRetainsAcceptedPixelsAndRepairsWithoutRestart` depends on it. | 2, 4 |
| D4 | **No warning for a missing heavier face.** Core Text does not synthesize bold; D2D may. | Every table family has the four weights except Georgia/Menlo (no 500/600: nearest face is used). A per-weight warning would fire on every Cambria/Consolas 600 graphic and is noise; G4 measures the difference. | 2 |
| D5 | **Gradient alpha interpolates as Core Graphics does** (unpremultiplied components). | Spec 5.4 leaves it to G4's `gradient-alpha` fixture. | 4 |
| D6 | **Colour emoji draw in colour** (spec owner decision 2, recommendation). | Core Text default. | 4 |
| D7 | **`preparedResource()` is cached on the Mac side only** (weak, keyed by the cache result, fixed size): same pointer for the same prepared texture, no per-tick allocation once anyone holds it. Windows still allocates per call; flagged for a Windows session, not changed here. | Windows files cannot be built on this Mac. | 5 |
| D8 | **Private preview (`set-grade-preview` `lowerThirdPreview`) and thumbnails stay off on Metal** (G6, G5). `PrivateGraphicsPreview...` and `ThumbnailForALayeredTemplate...` stay D3D11-only. | Spec slices. | 5 |

---

## Carried obligations from G1 (rule 6 walk)

| G1 "What G2 needs" | Discharged in |
|---|---|
| 1. fill `kFontSubstitutions`, bundled registration, `resolveRasterFont` is the seam | Task 2 (the seam moves to `graphics/GraphicsTextCoreText` so measurement and raster share it) |
| 2. template branch, atlas runs, `activeRuns()` | Task 4 |
| 3. `preparedResource()` allocates per call | Task 5 (D7) |
| 4. adapter 5.8/5.9, then capability on | Task 5 |
| 5. prepare hook available | used by Task 5 tests |

## Build and test

As G1 (CI `native-metal-macos` configure). Suites: `MetalCompositor.*:MetalOverlayRaster.*:GraphicsFontResolver.*:GraphicsTextMeasure.*:GraphicsCompositionPixel.*:GraphicsRpcPixel.*`.

---

## Task 1: this plan

## Task 2: font resolver, bundled Selawik

**Files:** new `native/resources/graphics-fonts/` (`selawk.ttf`, `selawksb.ttf`, `selawkb.ttf`,
`OFL.txt` verbatim from the 1.01 tag, `SOURCE.md` with URL, version, checksums); new
`native/src/graphics/GraphicsTextCoreText.{h,cpp}` (C++, Core Text C API, `__APPLE__` only);
`CoreTextOverlayRaster.mm` (uses it; `resolveRasterFont` removed); `native/CMakeLists.txt` (source,
configure-time copy to `${CMAKE_BINARY_DIR}/GraphicsFonts`); `scripts/make-macos-shell-bundle.sh`
(copies fonts + OFL to `Contents/Resources/GraphicsFonts` before signing); new
`native/tests/GraphicsFontResolverTest.cpp`.

**Interfaces.**
```cpp
namespace corevideo::graphics {
struct ResolvedFontFamily { std::string family; bool available = false; bool substituted = false; };
ResolvedFontFamily resolveGraphicsFontFamily(const std::string& requested);  // registers bundled fonts once
std::string graphicsFontSubstitutionWarning(const std::string& requested, const std::string& resolved);
std::string graphicsFontsDirectory();        // env override, else <exe dir>/GraphicsFonts
size_t registeredBundledGraphicsFonts();     // faces registered by the lazy call (tests, diagnostics)
CTFontRef createGraphicsFont(const std::string& family, int weight, double size);  // +1, nearest face
}
```

**Tests (`GraphicsFontResolver.*`).** Each row of the table resolves to its family with
`substituted == true` and the warning "Font <requested> is not installed on this Mac; using <family>.";
an installed family (Arial) resolves to itself unsubstituted; an unknown family is unavailable;
`registeredBundledGraphicsFonts() == 3` and the created Segoe UI 600 font's PostScript name is
`Selawik-Semibold` (400 -> `Selawik-Regular`, 700 -> `Selawik-Bold`); the font files' SHA-256 in
`GraphicsFonts/` match `SOURCE.md`'s pinned values (so a swapped file fails). Raster-level: an
appearance lower third asking for Segoe UI is `ready`, warns with the substitution sentence, and in
strict mode is still `ready` (warn, never fail).

**Mutations:** drop the Segoe UI row (red); skip registration (red: Selawik not found); strict
substitution fails (red); weight ignored (red on the PostScript names).

## Task 3: Core Text text measurement

**Files:** `GraphicsTextCoreText.{h,cpp}` (measure + line layout shared with the raster);
`native/src/graphics/GraphicsTemplate.cpp` (an Apple branch in `measureText` under
`!STUB && DEV_ADAPTERS && WITH_METAL`, mirroring the D3D11 gate); new `native/tests/GraphicsTextMeasureTest.cpp`.

**Behaviour.** `measureGraphicsText(text, family, size, weight, wrapWidth, wrap) -> {width, lines}`:
the resolved family; single line: `CTLine` typographic width including trailing whitespace; wrapped:
`CTTypesetterSuggestLineBreak` line count at `wrapWidth`, width = widest line (incl. trailing
whitespace, as DirectWrite's `widthIncludingTrailingWhitespace`). The raster lays out lines with the
same function, so layout and drawing cannot disagree.

**Tests.** "MMMMMMMMMM" and "iiiiiiiiii" at 40 px measure differently (the 0.55 estimate gives both
220); a wrapped measure of a long sentence at a narrow width reports > 1 line and each reported line
fits; through `GraphicsTemplate`: a curated lower third's auto-width plate differs from the estimate.
**Mutation:** route Apple to the estimate (red).

## Task 4: template raster on the worker

**Files:** `CoreTextOverlayRaster.{h,mm}`, `native/tests/MetalOverlayRasterTest.mm`.

**Behaviour.** `prepareRaster` ports `CompositorOverlayRaster.cpp:532-624` and `drawTemplate`
(`:298-412`) and `drawTextEffects` (`:261-295`): resolve on the worker (resolve error = failed with it);
packaged images opened first (header failure wording); atlas via `planRuns` + `packAtlas` when crawl or
layered non-whole motion (pack failure wording); budget; a top-down CTM `scale * point + offset`; per
primitive an aliased rect clip and `opacity` as the context alpha; `rect` rounded (radius clamped),
inset stroke with reduced radius; `ellipse`; `line` corner to corner with `lineRises`; solid or
two-stop linear gradient; `image` contain/cover with left/center/right, top/center/bottom alignment,
linear filter; `text` per spec 3.4 / 5.6 (resolved family + weight; leading/center/trailing by flush;
wrap only for `wrap-then-ellipsis`; layout box `width x min(height, fontSize*lineHeight*maxLines)`,
clipped to it; uniform line height; baseline `n*lineHeight + 0.8*fontSize`; last visible line truncated
with "…" when text remains; outline stroked at `2*width` under the fill; shadow with eight rings);
strict incomplete fails with the worker warning.

**Tests (full-res `getBytes`).** Template-level fixtures built with `GraphicsTemplate::load` from inline
manifests (shape invariants stated next to each): a rect and an inset-stroked rounded rect (interior
colour exact, stroke colour on the edge band, corner transparent); a two-stop horizontal gradient
(left/right ends equal the stops within 2/255, monotone in between); a text primitive with
`wrap-then-ellipsis`, `maxLines 2`: ink in exactly two line bands at `y + 0.8*fontSize` baselines,
nothing below the second; center alignment puts ink symmetric about the box centre (within 2 px);
outline: ring pixels in the outline colour around white fill; an unknown font strict -> failed with
"Requested graphics font is unavailable: <family>"; Segoe UI strict -> ready with the substitution
warning; a layered-motion template produces `activeRuns()` with as many slices as runs.
**Mutations:** baseline from ascent instead of `0.8*fontSize` (red); no ellipsis on the last line
(red); outline after fill (red: the ring would cover the glyph edge); gradient endpoints swapped (red).

## Task 5: adapter rules 5.8/5.9, prepared-resource cache, capability on

**Files:** `MetalCompositorAdapter.mm`, `CoreTextOverlayRaster.{h,mm}`,
`native/tests/GraphicsRpcPixelTest.cpp` (gate `COREVIDEO_WITH_D3D11 || COREVIDEO_WITH_METAL`, a
`createGraphicsTestCompositor()` helper; D3D-only tests stay gated), `native/tests/MetalCompositorTest.cpp`.

**Behaviour.** Port from `D3D11CompositorAdapter.cpp`: per-frame `graphicsSourceDraws_` record
(fixed 256-entry array, reset per program render) filled where a participant/screen-share layer is
drawn textured with a `sourceGeneration`; `source-/share-draw-submitted` subject facts when
`observeGraphicsSubjects`; `renderPlanEvidence` when `observeGraphicsSubjects`; in
`drawOverlayLayer` the bound-source rule, `graphicsAwaitingEntrance` (`composition-eligible`, no
draw), the atlas runs loop (`sampleRun`, `crawlItemX`, mask clip, per-slice viewport/scissor/UV),
and `ready` / `draw-submitted` after a composited instance layer; after the program pass, the
`plan.graphicsPreparations` loop (strict, slot = layerId, its prepared resource) pushing one fact per
entry; then `supportsGraphicsInstances()` true. `preparedResource()` cached (D7).

**Tests.** Re-gated on Metal — group B: `CommandsReachCoreStateAndActualProgramPixels`,
`EveryCuratedLookReachesActualNativeProgramPixels`, `NativeFadeAndReversalReachActualGpuPixels`,
`TextEffectsDrawShadowOutlineAndFill`, `NewShapesDrawCoverageAndInsetStrokes`,
`BarLeadsTheWordsOnProgramPixels`, `DataTablePublishesCompleteRowsAndKeepsAcceptedPixelsOnFailure`,
`CrawlUsesPreparedTextAndClipsBothDirections`, `ClockTicksAdoptPreparedDigits...`,
`VerifiedPackageImagePreservesAlpha...`, `FullyTransparentPngRevealsBackground...`,
`UploadedDeclarativeLogoPreserves...`, `VerifiedLogoInCompatibilityLook...` (Program half);
group C: `LocalCameraAndScreenLowerThirds...`, `LocalScreenCaptureDrawFacts...`,
`SourceGenerationReplacement...`, `AutomaticIntroduction...`, `ReviewedSocialCards...`; group D
(G3) is run and reported, not required. New Mac tests: `MetalCompositor.AGraphicsCueReachesProgramPixels`
(create, cue, show through `MediaCore` on the stub modules with the Metal compositor: snapshot
capability `graphics-instances`, no `graphics-instance-backend-not-built`, instance reaches
`firstDrawSubmittedFrame > 0`, Program preview pixels change inside the graphic's placement and not
outside it); `MetalOverlayRaster.PreparedResourceIsStableAndAllocationFree`.
**Red first:** the re-gated suite run before the adapter port (expected: every instance test fails;
the command test fails on `graphics-instance-backend-not-built`). **Mutations:** drop the
preparations loop (red: instances never leave `preparing`), drop `draw-submitted` (red:
`firstDrawSubmittedFrame` stays 0), skip the bound-source rule (red: `SourceGenerationReplacement`),
ignore `graphicsCrawlDistance` (red: crawl), capability false (red).

## Task 6: verification and outcomes

Metal suite loop, full Metal ctest `-LE realtime`, stub ctest; drill `--lower-third` pair (and a
fc675541 baseline if the machine is quiet, else unverified); bundle check (`make-macos-shell-bundle.sh`
ad-hoc into a temp dir, confirm `Contents/Resources/GraphicsFonts` holds the three faces + OFL and
`codesign --verify --deep --strict` passes); outcomes doc.
