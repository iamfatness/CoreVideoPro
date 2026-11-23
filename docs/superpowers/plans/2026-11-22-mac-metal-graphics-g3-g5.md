# Metal graphics backend G3-G5 (bounds and failure, parity reference, thumbnails) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the Metal graphics backend for macOS parity Phase 3: prove the remaining bounds and failure paths on Metal (G3), set a measured Windows-to-Mac raster parity standard with Mac goldens and a graphics drill (G4), and answer `graphics-template-list includeThumbnails` with real Core Text thumbnails (G5).

**Architecture:** No new subsystem. G3 is mostly ports of the Windows graphics tests that never reached Metal, plus failure-path tests that exist on neither platform. These are written once as a contract header that both rasters instantiate. G4 adds input-only parity fixtures, a portable metrics and judging library, Mac goldens, a Windows reference writer (run in one Windows session) and a `--graphics-cue` drill leg. G5 adds a thumbnail path to `CoreTextOverlayRaster` behind a plain C++ entry point, so the shared `GraphicsThumbnail.cpp` can call it.

**Tech Stack:** C++20 / ObjC++ (ARC), Metal, Core Text, Core Graphics, ImageIO; the in-tree gtest subset (`native/tests/gtest/gtest.h`: no `GTEST_SKIP`, and `ASSERT_*` does not abort); Python 3 for the drill; Windows: D3D11, D2D, WIC (one session only).

**Spec:** `docs/superpowers/specs/2026-11-15-mac-metal-graphics-backend-design.md` (sections 3, 5.10, 5.13, 6 rows G3-G5, 7.1-7.4). Read with `docs/superpowers/plans/2026-11-21-mac-metal-graphics-g1-outcomes.md` and `...-g2-outcomes.md`. Several spec assumptions are corrected below.

**Branch:** `mac-metal-graphics-g3-g5` from `origin/main` (e117a8fa or later).

## What changed since the spec (verified in code 2026-11-22, e117a8fa)

These change the scope. Each was read in code, not taken from an outcomes doc.

1. **Most of G3 already passed on Metal in G2.** `GraphicsRpcPixelTest.cpp` group D runs on Metal: failed-font repair, cache pressure plus a new renderer, cue/Program separation and the eight-instance limit (`GraphicsRpcPixelTest.cpp:1086-1302`, gate at `:20`). G3 is now the parts no test covers on **either** platform, and the D3D11 tests that never reached Metal.
2. **Six graphics tests in `D3D11CompositorTest.cpp` never reached Metal.** That file is D3D11-gated, and G1 wrote new Mac tests instead of porting these:
   - `OversizedGraphicsAreRefusedAndValidContentRecovers` (`:1122`)
   - `GraphicsCueAndProgramAdoptCompleteNativeRevisionsIndependently` (`:1141`)
   - `OverlayRasterCacheReusesAndInvalidates` (`:1097`)
   - `AnimatedOverlayHonorsCompositeClipRect` (`:1041`)
   - `GuardedShotLayoutDrawsBothPreparedGraphicsOnItsFirstFrame` (`:1634`)
   - the Program half of `CrawlLoopSwitchKeepsFrameMatchedProgramAndKeyAt1080p` (`:1680`)

   The spec listed the second and fifth as group C. The G2 outcomes' "group C green on Metal" refers only to `GraphicsRpcPixelTest.cpp`.
3. **Rules 7 and 8 of spec 3.2 are untested everywhere.** No test on either platform asserts these wordings: "Graphics scale preparation pending; retaining the accepted raster.", "...16-revision capacity limit.", "...waiting for shared resource capacity.", the atlas-space failure, or "Graphics image could not be decoded:". (`grep` over `native/tests`: zero hits.) The strings are identical in `CompositorOverlayRaster.cpp:486-578` and `CoreTextOverlayRaster.mm:523-650`.
4. **The eGPU device-identity item is moot.** The Mac build is Apple Silicon only (owner decision), and Apple Silicon has no eGPU support. A second compositor in the same process shares the GPU's `registryID`, and G2 already pins that (`GraphicsRpcPixelTest.cpp:1181-1189`). G3 keeps one raster-level case: a resource from a foreign device identity is never adopted.
5. **The spec's drill assertion "budget counters return to their pre-cue values after the instances are removed" is wrong on both platforms.** `PreparationCache` evicts only on capacity or `GraphicsCachePressure` (`PreparationCache.h:157-185`), and has no time-based or removal-driven retire. Removed revisions stay resident, and so counted, until pressure. Task 9 asserts what the code guarantees instead: the resident high water never exceeds 16, no preparation fails, and a new graphic after removal still reaches `ready`.
6. **The spec's reference format cannot support its own tier 1.** Tier 1 compares pixels per channel at 1920x1080, but the spec's reference holds only metrics and a 1/8-scale thumbnail. This plan stores the Windows raster itself as a PNG holding the premultiplied BGRA samples verbatim (Task 6). The metrics are computed from it on the Mac by the same code that computes the Mac side.
7. **Layout parity needs no change to shared Windows code.** Every measurement-dependent layout decision lands in the resolved `GraphicsDrawing`:
   - crawl item `box.width = ceil(measureText)` (`GraphicsTemplate.cpp:317`)
   - auto-width extents (`:277`)
   - `minFontSize` fit (`:506-516`)
   - `crawl.passWidth`

   The reference records a summary of the resolved drawing. `measureText` stays file-local.
8. **G5 has no Mac consumer until Graphics shell G8 (Phase 4).** The Mac shell never sends `includeThumbnails` and does not decode thumbnails (`grep thumbnail mac-shell/Sources`: none). The Windows shell ignores `backend` (`GraphicsThumbnailBytes.cs`). G5 makes the core answer correctly. It reaches an operator only when G8 lands (owner decision 2).
9. **Graphics-shell G5 ("Metal tables") and G6 ("Metal crawl") already have their native prerequisites on Metal after G2.**
   - Tables are expanded into rect and text primitives in shared code (`GraphicsTemplate.cpp:322-395`). `DataTablePublishesCompleteRowsAndKeepsAcceptedPixelsOnFailure` is green on Metal and installs its table over `graphics-template-install` with a manifest and no package root (`GraphicsRpcPixelTest.cpp:699`).
   - Crawls draw from the Metal atlas, and `CrawlUsesPreparedTextAndClipsBothDirections` is green on Metal (`:1303`). `advanceCrawls` runs on every core, ungated (`MediaCore.cpp:5851`).
   - The "crawls refuse on the Mac" behaviour is a **shell-side** refusal: the Mac has no dataset publishing yet (graphics-shell G1-G3 outcomes, review round).
   - The two remaining native proofs are owned by **this plan**: 1080p crawl boundary adoption on Metal (Task 3) and a drill with a crawl on air (Task 9). Dataset staging, publishing, table manifests and crawl controls (`GraphicsLocalData`, `GraphicsDataTable`, `GraphicsCrawlData` ports) belong to the **Graphics-shell lane** G5/G6.
10. **The G6 dependency (Metal `supportsGradePreview`) now exists.** `MetalCompositorAdapter.mm:260`, `MetalGradePreview.mm`. Metal refuses `lowerThirdPreview` explicitly (`MetalGradePreview.mm:159`). G6 is unblocked but stays out of this plan, because the master plan puts it in Phase 5.

## Global Constraints

- Admission wording, bounds and states match `CompositorOverlayRaster.cpp` byte for byte (spec 3.2 rules 1-8). The Windows file is the reference. Never edit it on this Mac.
- Bounds (spec 5.10): raster 4096 x 4096; atlas 16384 per axis and 256 MiB; cache 256 MiB, 32 queued, 64 resident; process budget 128 MiB CPU, 256 MiB GPU, 2 preparations, 16 resident revisions; metadata 64 KiB; package image 32 MiB; thumbnail 320 x 180 and 256 KiB PNG; eight instances per bus.
- Thumbnail answer on Metal: `{state: "ready", reason, backend: "metal-native", appearance: "reference", width, height, pngBase64}`. On failure: `{state: "unavailable", reason, backend: "metal-native"}`. These mirror `GraphicsThumbnail.cpp:30-32` with the backend string changed.
- Pixel format `MTLPixelFormatBGRA8Unorm`, premultiplied over transparent. Thumbnail PNGs hold straight alpha.
- **Expected values come from real output only.** Windows expectations come from the Windows reference written in the Windows session (Task 7). Mac goldens come from Mac output, regenerated only by the explicit command in Task 6. This plan authors only **inputs**: template manifests, field values and raster sizes. Comparator self-tests may use synthetic images, and are labelled as tests of the comparator, never as parity evidence.
- Anything the core loads from beside its executable must also work from `/Library/Application Support/CoreVideo Pro/`. Thumbnails resolve fonts through `COREVIDEO_GRAPHICS_FONTS_DIR`, which the shell already passes (`CoreLaunchEnvironment.extras`, G2 review fix 1). Task 5 extends the installed-layout script to prove it.
- No new timing assertion on CI. A wait for a preparation worker is a bounded poll (at most 5 s) that asserts a state, never a duration. Crawl and clock time come from the injected tick (`GraphicsInstances(epoch, tick)`). The frame-delivery gate runs only on this Mac (Task 9). The CI runner GPU is about 30x slower.
- `MetalCompositorAdapter.mm` is a merge hotspot: Tiles N1 runs in the same native lane. No task plans an adapter edit. If a ported test needs an adapter fix, keep it inside `drawOverlayLayer` or the graphics facts loop, as its own commit, rebased on `origin/main` just before review.
- New tests go in new files. Do not add to `MetalCompositorTest.cpp` (Tiles N1 territory). No new `operator new` replacement: `MetalOverlayRasterTest.mm` already owns the binary-wide one.
- Native build on this Mac:
  ```bash
  export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk
  cmake --build native/build-metal --target corevideo-native-tests corevideo-native
  ```
  Configure as CI does (`.github/workflows/ci.yml` native-metal-macos) if `native/build-metal` is missing.
- Commits end with the attribution lines the session supplies.

## Review Focus

These are inputs the spec implies but no ported test exercises. Each is pinned by a test in the named task.

1. **A crawl at the item maximum (64 items) in a long font.** It prepares, or fails with the atlas-space wording. It never stalls the tick, and the accepted crawl keeps airing. Task 2: `CrawlAtTheItemLimitPreparesOrFailsWithAtlasWording`.
2. **A strict Segoe UI field holding emoji and CJK text.** It prepares `ready` through the table substitution and the Core Text cascade, with the substitution warning and no failure. Task 2: `StrictSubstitutedTextWithEmojiAndCjkPrepares`.
3. **An update storm on air: one update per rendered frame, with no wait for readiness, more than 16 revisions.** The accepted Program pixels never change until the last revision is ready. The latest revision is eventually adopted. No preparation fails. Task 4: `AnUpdatePerFrameKeepsAcceptedPixelsAndAdoptsTheLatest`.
4. **A template whose package image cannot decode, while thumbnails are requested.** That row is `unavailable` with the decode reason, every other row still completes, and the registry is not wedged. Task 5: `ABrokenTemplateThumbnailDoesNotBlockTheOthers`.
5. **Thumbnails requested while a graphic is on Program.** They share the 2-preparation process budget. Program's accepted pixels are unchanged on every frame, and the thumbnails still complete. Task 5: `ThumbnailsDuringAirtimeLeaveProgramUntouched`.

---

## Carried obligations (plan-authoring rule 6)

| From | Obligation | Discharged in |
|---|---|---|
| G1 outcomes, Unverified | Visual parity with D2D (baseline, weights, advance widths) | Tasks 6-8 |
| G1 outcomes, Unverified | Frame delivery with a lower third cycling not established (loaded machine) | Task 9, quiet-machine pair incl. `--lower-third` |
| G1 outcomes, Unverified | `COREVIDEO_SHELL_SNAPSHOT` before/after capture of the shipped lower third | Task 9 step 6 |
| G2 outcomes, Unverified | Cross-rasterizer parity; Selawik vs Segoe UI on air | Tasks 7-8, owner sheet |
| G2 outcomes, Unverified | No drill with a template graphics cue | Task 9 |
| G2 D5, D6, D9; image filter constant | Decide from fixtures `gradient-alpha`, `text-emoji`, `text-small-14px`, `image-scale` | Task 8 |
| G2 note 1 | Social-card brightness floor 110 vs 140 at 3.7 px stems | Task 8 records it beside the `text-small-14px` result |
| G2 "What G3 needs" | Tiles name-line retention under load; eGPU device identity | Task 4 (retention under pressure); item 4 above (moot) |
| G2 "What G5 needs" | `thumbnailPng` PNG encode + crawl thumbnail placement | Task 5 |
| Warm-up outcomes, open 5 | Cold preparation of crawl / image templates not timed | Task 9 prints command-to-ready for the crawl |
| G2 D7 / review 2 | Windows `preparedResource()` allocation and D3D11 fact copy | **Not here.** Windows-session agenda, Task 7 last step (listed, not done) |

---

## File structure

| File | New/Modify | Responsibility |
|---|---|---|
| `native/tests/OverlayRasterContractCases.h` | new | Spec 7.1 contract: admission and failure cases as templates over a backend traits struct; included by one Mac and one Windows test file |
| `native/tests/MetalOverlayRasterContractTest.mm` | new | Metal instantiation of the contract cases (ObjC++) |
| `native/tests/MetalGraphicsBoundsTest.cpp` | new | Compositor/instance-level G3 tests on Metal (ports + storm + retention under pressure) |
| `native/src/compositor/CoreTextThumbnail.h` | new | Plain C++ declaration of the Metal thumbnail entry point |
| `native/src/compositor/CoreTextOverlayRaster.{h,mm}` | modify | `Request.thumbnail`; a device-free, cache-free thumbnail prepare path; ImageIO PNG encode |
| `native/src/graphics/GraphicsThumbnail.cpp` | modify | `#elif` Apple Metal branch; the D3D11 branch is left textually unchanged |
| `native/src/graphics/GraphicsTextCoreText.cpp` | modify | One log line when bundled fonts register (or are missing) |
| `native/tests/fixtures/graphics-parity/index.json` + `*.json` | new | Parity **inputs**: manifests, fields, brand, raster size |
| `native/tests/GraphicsRasterParity.{h,cpp}` | new | Portable: `ParityImage`, layout summary, region derivation, metrics, tier 1/2 judges, SSIM |
| `native/tests/GraphicsRasterParityTest.cpp` | new | Comparator self-tests (synthetic; not parity evidence) |
| `native/tests/GraphicsParityImageApple.mm` | new | Verbatim-sample PNG read/write with ImageIO |
| `native/tests/MetalGraphicsParityTest.mm` | new | Render fixtures on Metal; Mac goldens; compare to the Windows reference; artifacts sheet |
| `native/tests/D3D11GraphicsReferenceTest.cpp` | new | Windows only: render fixtures with D2D, write the reference and PNGs, D3D11 contract instantiation |
| `native/tests/golden/metal-graphics/` | new (generated) | Mac goldens + `manifest.json` |
| `native/tests/golden/windows-graphics/` + `contracts/graphics-raster-reference.json` | new (generated on Windows) | Windows reference |
| `native/CMakeLists.txt` | modify | New test sources (`:1022` list; ObjC++ block `:1109`); a `golden`-labelled ctest split like `realtime` (`:1244-1262`) |
| `.github/workflows/ci.yml` | modify | Metal ctest `-LE "realtime|golden"` (goldens advisory on CI, blocking in `release-macos.sh`'s full ctest) |
| `native/tests/GraphicsTemplateTest.cpp` | modify `:376-382` | Metal now expects `ready` / `metal-native` |
| `native/tests/GraphicsRpcPixelTest.cpp` | modify `:928` | Thumbnail test gated `D3D11 || METAL` via a raster-neutral thumbnail helper |
| `scripts/mac-show-drill.py` | modify | `--graphics-cue` |
| `scripts/tests/test_core_graphics_fonts_location.py` | modify | Thumbnail leg on the installed layout |

---

### Task 1: Port the six D3D11-only graphics tests to Metal

**Size:** M. **Depends on:** none.

**Files:**
- Create: `native/tests/MetalGraphicsBoundsTest.cpp`, gated `#if COREVIDEO_WITH_METAL && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS`.
- Modify: `native/CMakeLists.txt` (add it to the list at `:1022`, after `tests/MetalCompositorTest.cpp`).

**Interfaces:**
- Consumes: `modules::createMetalCompositor()`, `graphics::GraphicsInstances(std::string epoch, std::function<double()> tick)`, `append(plan, preview)`, `observe(frame)`, `unavailableLayoutCue`, `commitLayoutTake`, `promoteCuedForTake`, `advanceCrawls`.
- Produces: file-local helpers `renderUntilPrepared(ICompositor&, CompositorRenderPlan) -> ProgramFrame` (renders until no `graphics-preparing` warning, at most 5 s) and `MetalGraphicsRig` (`compositor`, `graphics`, `command(type, payload)`, `plan(width, height)`). Tasks 3 and 4 add to this file and reuse both.

Port each test with its assertions unchanged. Only the compositor factory and the D3D11 helpers change (rebuild `makeGpuOverlayLayer`, `gpuDistinctColorsInRegion`, `previewPixelRgba` locally from their D3D11 definitions). Names drop the `D3D11` suite and keep the case name:

- [ ] **Step 1: Write the five non-crawl ports.** Use suite `MetalGraphicsBounds`, with the same case names as `D3D11CompositorTest.cpp` `:1041`, `:1097`, `:1122`, `:1141`, `:1634`. The Shot Layout test replaces `KeyRig` with `MetalGraphicsRig` and never sets `graphicsKeyReadback`. Metal has no graphics key; spec 10 rules it out of scope. Its assertions are the top-half and bottom-half pixel changes on the first frame after `commitLayoutTake`.
- [ ] **Step 2: Build and run.**
  Run: `./native/build-metal/corevideo-native-tests --gtest_filter='MetalGraphicsBounds.*'`
  Expected: every case runs. Record which fail. Any red is a real Metal gap: fix it in `CoreTextOverlayRaster.mm` or, only if unavoidable, in `MetalCompositorAdapter.mm` (see Global Constraints), test-first, one commit per fix.
- [ ] **Step 3: Mutations.** Each must turn the named test red. Record the results in the commit message.
  - Skip the scissor for a layer with `hasClipRect` (`AnimatedOverlayHonorsCompositeClipRect`).
  - Drop `graphics-preparation-failed` from the frame warnings (`OversizedGraphics…`).
  - Make the Preview pass draw the Program revision (`GraphicsCueAndProgram…`).
  - Ignore the brand colour in `overlayContentSignature`'s use on Metal, i.e. key the cache without it (`OverlayRasterCacheReusesAndInvalidates`; confirm its NE assertion reds).
  - Draw only the first layout cue on the commit frame (`GuardedShotLayout…`).
- [ ] **Step 4: Full Metal suite.**
  Run: `ctest --test-dir native/build-metal --output-on-failure -LE realtime`
  Expected: all entries pass.
- [ ] **Step 5: Commit.** `test(metal-graphics): port the D3D11-only graphics compositor tests to Metal (G3)`

### Task 2: Raster contract cases: admission, scale retention, capacity, failures

**Size:** M. **Depends on:** none. Run it before Task 5, because both may touch `CoreTextOverlayRaster.mm`.

**Files:**
- Create: `native/tests/OverlayRasterContractCases.h`, `native/tests/MetalOverlayRasterContractTest.mm`.
- Modify: `native/CMakeLists.txt` (ObjC++ block at `:1109`, `-fobjc-arc`). Move the two cases in `MetalOverlayRasterTest.mm:334` (`AdmissionRulesMatchWindowsWording`) into the header and delete them from the old file.

**Interfaces:**
- Produces: the traits contract below. Task 7 instantiates the same header for D3D11.

```cpp
// A Backend supplies:
//   using Raster;                      // CoreTextOverlayRaster | CompositorOverlayRaster
//   using Device;                      // id<MTLDevice> | ID3D11Device*
//   static Device device();            // null -> print "[contract] unavailable" and return
//   static bool raster(Raster&, Device, const modules::CompositorOverlayContent&, int width, int height,
//                      bool strict, std::shared_ptr<const graphics::GraphicsPreparedResource> prepared);
//                                      // one render-loop call: beginFrame + rasterOverlayTexture; true = texture returned
//   static constexpr graphics::GraphicsPreparedResource::Backend kind;
template <class Backend> void contractMetadataBound();
template <class Backend> void contractDimensions();
template <class Backend> void contractCompatibleSameSizeIsAdoptedOnTheFirstCall();
template <class Backend> void contractForeignDeviceOrHashIsNeverAdopted();
template <class Backend> void contractScaleRetentionDrawsTheAcceptedRaster();
template <class Backend> void contractFailedScaleKeepsTheAcceptedRaster();
template <class Backend> void contractCapacityWordingAtTheResidentLimit();
template <class Backend> void contractBudgetWaitIsRetryableNotSticky();
template <class Backend> void contractAtlasSpaceFailureWording();
template <class Backend> void contractImageDecodeFailureWording();
template <class Backend> void contractCrawlAtTheItemLimitPreparesOrFailsWithAtlasWording();
template <class Backend> void contractStrictSubstitutedTextWithEmojiAndCjkPrepares();   // Metal-only instantiation
```

Each case takes its expected strings from `CompositorOverlayRaster.cpp`, copied verbatim. They are the Windows behaviour in code, not invented output. Each case states its fixture invariant (rule 1) in a comment.

- [ ] **Step 1: Write the cases.** Assertions:
  - **Metadata bound and dimensions:** move the existing assertions unchanged.
  - **Same-size adoption:** raster A on device D prepares legacy overlay O at 600x120 until `ready`. It takes `P = A.preparedResource()`. A **fresh** raster B on D gets `raster(B, D, O, 600, 120, false, P)`. On its first call this returns `true` with state `"ready"`. Invariant: B has never seen O, so only adoption can return a texture on call one.
  - **Foreign device or hash:** copy P with `deviceIdentity + 1`, and separately with `contentHash[0] ^= 1` and `hasContentHash` unchanged. On a fresh B each first call returns `false` with state `"preparing"`.
  - **Scale retention:** B gets P (prepared at 600x120) and a request at 1200x240, with the worker blocked by `compositor::setOverlayRasterPrepareHookForTest`. The call returns `true` (the old texture), state `"preparing"`, warning exactly `"Graphics scale preparation pending; retaining the accepted raster."`. Release the hook in a scope guard.
  - **Failed scale:** P is from a template with an image asset. The new size uses the same content with the image bytes made undecodable after the header (`GraphicsAssetFixture.h` `png()` with its IDAT bytes zeroed). Same content hash means a compatible P. The call returns `true` with state `"failed"`, and the warning ends with `" Retaining the accepted raster."` and begins with `"Graphics image could not be decoded: "`. Invariant: the corrupt PNG must pass `openPackagedImage`'s header check. Assert the header decodes in the fixture itself.
  - **Capacity wording:** reserve leases from `GraphicsResourceBudget::process().tryReserve(1, 1, "contract-hold-N", true)` with `finishPreparation()` until `counters().residentResources == residentLimit`. Then a template overlay with a content hash returns `false`, state `"preparing"`, warning exactly `"Graphics preparation pending at the shared 16-revision capacity limit."`. Release the leases, poll at most 5 s, and the overlay reaches `ready`. Invariant: restore the budget in a scope guard, because the budget is process-global and later tests depend on it.
  - **Budget wait:** same setup. The worker's result is not published as failed. The cache's `failed` counter does not rise on Metal; on Windows, assert state only. After release it reaches `ready` without the content changing.
  - **Atlas space:** a layered-motion template built from `GraphicsMotionFixture.h` `bar-first.json` with enough animated layers that `graphics::packAtlas` refuses at raster height 4096. Invariant: the test first calls `planRuns` + `packAtlas` on the resolved drawing and asserts they return false, so the fixture is proven to exceed the atlas before the raster is judged. The raster ends `failed` with exactly `"Graphics motion needs more texture space than one graphic allows; give fewer layers their own motion."`.
  - **Image decode:** strict template with the corrupt image ends `failed`, warning begins `"Graphics image could not be decoded: "`.
  - **Crawl at the limit (Review Focus 1):** a 64-item crawl, each item 64 `W` characters, Arial 72. Ends `ready` or `failed` with the atlas wording (either is Windows-legal). It never returns any other failure text, and 20 calls made while it prepares each return without waiting (the prepare hook is not used here; the assertion is that each call returns `false` with `"preparing"` until terminal).
  - **Emoji and CJK (Review Focus 2):** strict template text `"Café 東京 🎬"` in `Segoe UI` ends `ready`, warning `"Font Segoe UI is not installed on this Mac; using Selawik."`, and has non-zero ink.
- [ ] **Step 2: Instantiate for Metal** in `MetalOverlayRasterContractTest.mm` as `TEST(OverlayRasterContract, <CaseName>)`, one per case.
- [ ] **Step 3: Run.**
  Run: `./native/build-metal/corevideo-native-tests --gtest_filter='OverlayRasterContract.*:MetalOverlayRaster.*'`
  Expected: all pass. A red one is a Metal divergence from the Windows code: fix `CoreTextOverlayRaster.mm` to match `CompositorOverlayRaster.cpp`.
- [ ] **Step 4: Mutations**, each red on the named case:
  - Return `nil` instead of the retained texture in rule 7 (scale retention).
  - Drop the `" Retaining the accepted raster."` suffix (failed scale).
  - Skip the capacity check (capacity).
  - Publish a retryable result as a cached failure (budget wait: the `ready` after release never comes).
  - Remove the `deviceIdentity` comparison from `compatible` (foreign device).
  - Remove the 16384 check (atlas: only if the fixture's atlas is wider than 16384 but packs; otherwise record "covered by packAtlas" and say so).
- [ ] **Step 5: Commit.** `test(metal-graphics): raster contract cases for admission, scale retention, capacity and failures (G3)`

### Task 3: Native proofs for graphics-shell G6 and G7 on Metal: crawl boundary at 1080p

**Size:** S. **Depends on:** Task 1 (rig).

**Files:**
- Modify: `native/tests/MetalGraphicsBoundsTest.cpp`.

**Interfaces:**
- Consumes: `MetalGraphicsRig` with an injected `double tick`, `advanceCrawls()`, `promoteCuedForTake()`.

- [ ] **Step 1: Write `MetalGraphicsBounds.CrawlLoopSwitchAdoptsAtTheBoundaryOnA1080pProgram`.** Port `D3D11CompositorTest.cpp:1680-1790` without the key half. Keep the same manifest, but `"font":"Arial"`; Arial is on both platforms, and the D3D11 test's Segoe UI would test the substitution, not the boundary. Plan 1920x1080, `fullProgramReadback = true`. Assertions:
  - at `tick = 65` Program shows the first list;
  - after the program update reaches `preparation == "ready"`, `programRevision` is still `1`, and a render before the boundary is byte-equal to the first;
  - at `tick = boundary + 65` (`boundary` = the prepared resource's `crawl->passWidth`, read from the plan as the D3D11 test does) `programRevision == 2`, Program pixels differ from the first, and `after.frameNumber == before.frameNumber + 1`;
  - all crawl ink lies inside the clip rect computed from the manifest exactly as `:1735-1744`.
- [ ] **Step 2: Run** `--gtest_filter='MetalGraphicsBounds.CrawlLoop*'`. Expected: PASS. If it is red, the crawl path is not shell-ready: fix it before Task 9.
- [ ] **Step 3: Mutations.**
  - Adopt the pending crawl revision immediately instead of at the boundary (`GraphicsInstances::advanceCrawls`). This is shared code: revert after the run. The before-boundary equality must red.
  - Ignore `graphicsCrawlDistance` in the Metal run loop. The `after != first` assertion must red.
- [ ] **Step 4: Commit.** `test(metal-graphics): crawl boundary adoption on a 1080p Metal Program (graphics-shell G6 prerequisite)`

### Task 4: Pressure, storms and font-directory visibility

**Size:** S. **Depends on:** Task 1.

**Files:**
- Modify: `native/tests/MetalGraphicsBoundsTest.cpp`, `native/src/graphics/GraphicsTextCoreText.cpp:53-81`, `native/tests/GraphicsFontResolverTest.cpp`.

**Interfaces:**
- Produces: `std::string graphics::bundledGraphicsFontsSummary()`, declared in `GraphicsTextCoreText.h`. It returns `"graphics fonts: registered N faces from <dir>"` or `"graphics fonts: none found at <dir>; Windows families in the substitution table will fail"`. `registerBundledFonts()` logs it once through `core::nativeLogf` (`core/BoundedAsyncLog.h:40`).

- [ ] **Step 1: Write `AnUpdatePerFrameKeepsAcceptedPixelsAndAdoptsTheLatest` (Review Focus 3).** Show `compact-solid` and wait for `firstDrawSubmittedFrame > 0`, then capture `accepted`. Then for 40 consecutive renders send `graphics-instance-update target:"cue"` with `name = "Storm N"`, with no readiness wait. On every render: `programFields.name == "Accepted"` and Program pixels equal `accepted`. Then wait (at most 5 s) for `preparation == "ready"` and `revision == 41`. `instances[0].warning` never contains `"failed"`, and `resourceBudgetAtStatePublication.residentHighWater <= 16`. Then `graphics-instance-update target:"program" expectedRevision:41`, and Program changes. Invariant: 40 > 16, so the storm must cross the resident limit.
- [ ] **Step 2: Write `AHeldNameLineSurvivesACacheTrimWhileItsNewSizePrepares`.** Start from G1's `AHeldNameLineKeepsItsTextWhileTheResizedRasterPrepares` (`MetalCompositorTest.cpp:933`; read it, do not edit that file). With the worker blocked, call `graphics::GraphicsCachePressure::requestTrim()` three times between renders. The held text pixels are unchanged on every render, and after release the new size draws.
- [ ] **Step 3: Write `GraphicsFontResolver.BundledFontsSummaryNamesTheDirectoryAndCount`.** With `COREVIDEO_GRAPHICS_FONTS_DIR` set to the build's `GraphicsFonts`, the summary is `"graphics fonts: registered 3 faces from " + dir`. Registration is once per process, so this test only reads the summary after the first resolution. The missing-directory wording is pinned by Task 5's installed-layout script, which runs a fresh process.
- [ ] **Step 4: Implement the summary and log line.**
- [ ] **Step 5: Run** `--gtest_filter='MetalGraphicsBounds.*:GraphicsFontResolver.*'`, then the full Metal ctest. Expected: all pass.
- [ ] **Step 6: Mutations.**
  - Let the storm's cue update retarget Program (`programFields` check reds).
  - Release the retention lease on trim (the held-text check reds).
  - Report the face count from a constant instead of the registration loop. The summary test asserts the count equals the number of `.ttf` files the test itself counts in the directory, so point the test at a copy of the directory with one face removed and confirm it reds.
- [ ] **Step 7: Commit.** `test(metal-graphics): update storms, retention under cache pressure; log the graphics font directory (G3)`

### Task 5: Library thumbnails on Metal (G5)

**Size:** M (spec said S; the PNG encode, crawl placement and registry wiring make it M). **Depends on:** Task 2.

**Files:**
- Create: `native/src/compositor/CoreTextThumbnail.h`.
- Modify: `native/src/compositor/CoreTextOverlayRaster.{h,mm}`, `native/src/graphics/GraphicsThumbnail.cpp`, `native/tests/GraphicsTemplateTest.cpp:376-382`, `native/tests/GraphicsRpcPixelTest.cpp:928-958`, `native/tests/MetalOverlayRasterContractTest.mm` (two new cases below), `scripts/tests/test_core_graphics_fonts_location.py`.

**Interfaces:**
- Produces, in `CoreTextThumbnail.h`, plain C++ and Apple-only:
  ```cpp
  namespace corevideo::modules {
  // Library worker only. Never touches a render thread, a cache or an MTLDevice.
  std::vector<uint8_t> coreTextThumbnailPng(const CompositorOverlayContent& overlay, int width, int height,
                                            std::string& warning);
  }
  ```
- `CoreTextOverlayRaster::Request` gains `bool thumbnail = false`. The prepare body becomes a static, member-free function. The `!request.device` refusal applies only when `!request.thumbnail`.

Behaviour to match `CompositorOverlayRaster.cpp:517-521, 551-557, 690-714`:
- Size outside 1..320 x 1..180 returns empty with `"Invalid thumbnail dimensions."`.
- Strict resources.
- For a crawl, each `crawlStart` primitive is shifted by its `crawlStart` and clipped to `crawl.clip`, and no atlas is built.
- The budget is reserved and finished as for a raster.
- The PNG is encoded from the premultiplied bitmap with `CGImageDestination` (`public.png`). ImageIO writes straight alpha. Encoded size above 256 KiB returns `"Native thumbnail exceeds its encoded limit."`; an encode failure returns `"Native thumbnail PNG encoding failed."`.
- `GraphicsThumbnail.cpp` gains `#elif !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL`. That branch duplicates the D3D11 branch's overlay setup and calls `coreTextThumbnailPng`, with `backend` `"metal-native"`. Leave the D3D11 branch byte-identical, because it cannot be compiled here. Folding both onto one helper is on the Windows-session agenda.

- [ ] **Step 1: Write the failing tests.**
  - `GraphicsTemplateTest.cpp:376`: change the `#if` to `D3D11 || METAL`, with backend `"d3d11-native"` or `"metal-native"` per platform. Same `ready`, PNG prefix and size assertions.
  - `GraphicsRpcPixelTest.cpp:928`: gate `D3D11 || METAL`. Add a file-local `thumbnail(overlay, w, h, error)` that calls `CompositorOverlayRaster{}.thumbnailPng` on D3D11 and `coreTextThumbnailPng` on Metal. Assertions unchanged: layered and settled thumbnails are byte-equal and longer than 100 bytes.
  - `OverlayRasterContract.ThumbnailPngIsStraightAlphaAndMatchesItsHeader` (Metal-only): a 200x100 template with one rect, fill `#FFFFFF80`. The PNG is at most 256 KiB, starts with the PNG signature, IHDR width/height are 200/100 (the same checks as `GraphicsThumbnailBytes.Decode`), and the decoded centre pixel (ImageIO, non-premultiplied RGBA) is within ±1 of `(255,255,255,128)`.
  - `OverlayRasterContract.CrawlThumbnailPlacesItemsAtTheirStartsInsideTheClip`: reuse the crawl manifest from `GraphicsRpcPixelTest.cpp:1314`. Ink lies only inside the crawl clip scaled to the thumbnail, and is non-empty.
  - `ABrokenTemplateThumbnailDoesNotBlockTheOthers` (Review Focus 4, in `GraphicsTemplateTest.cpp`): install a template with the Task 2 corrupt image plus a valid one. After `requestThumbnails()`, the broken row is `unavailable` with a reason beginning `"Graphics image could not be decoded: "`, and every other row is `ready`.
  - `ThumbnailsDuringAirtimeLeaveProgramUntouched` (Review Focus 5, in `MetalGraphicsBoundsTest.cpp`): show `compact-solid` and capture `accepted`. Send `graphics-template-list {includeThumbnails:true}` through `GraphicsInstances::apply`. Render until every `graphics.templates[].thumbnail.state` is terminal (at most 20 s, the bound the registry test uses). Every render equals `accepted`, and every curated row is `ready`.
- [ ] **Step 2: Run them.** Expected: FAIL (`unsupported`; the helper does not link).
- [ ] **Step 3: Implement** the thumbnail path, the header and the `GraphicsThumbnail.cpp` branch.
- [ ] **Step 4: Run.**
  Run: `--gtest_filter='GraphicsTemplateRegistry.*:GraphicsCompositionPixel.Thumbnail*:OverlayRasterContract.*:MetalGraphicsBounds.*'`, then the full Metal ctest and the stub ctest. The stub keeps `unsupported` via the `#else`.
  Expected: PASS.
- [ ] **Step 5: Installed-layout leg.** In `test_core_graphics_fonts_location.py`, add `thumbnail(core_path, fonts_dir)`. It sends `graphics-template-list` with `includeThumbnails: true` (body shape from `GraphicsGateway.swift:237`) and polls the snapshot `graphics.templates` until terminal.
  - With the variable: `compact-solid` is `ready` / `metal-native`.
  - Without it: `compact-solid` is `unavailable`, with reason `"Requested graphics font is unavailable; choose an installed font."`, and the core's stderr carries `"graphics fonts: none found at"` (Task 4).

  Run: `python3 -I scripts/tests/test_core_graphics_fonts_location.py`. Expected: PASS.
- [ ] **Step 6: Mutations.**
  - Encode the premultiplied bytes as straight (alpha test reds: decoded RGB ≈ 128).
  - Skip the crawl shift (crawl thumbnail reds).
  - Build an atlas for thumbnails (layered == settled reds).
  - Keep the device refusal for thumbnails (registry test reds).
  - Hold the budget lease in a static (airtime test: a later Program revision cannot reserve; red).
- [ ] **Step 7: Commit.** `feat(metal-graphics): library thumbnails on Metal (G5)`

### Task 6: Parity fixtures, comparator and Mac goldens (G4, Mac half)

**Size:** M. **Depends on:** none (can run beside Tasks 1-5).

**Files:**
- Create: `native/tests/fixtures/graphics-parity/index.json` + manifests, `native/tests/GraphicsRasterParity.{h,cpp}`, `native/tests/GraphicsRasterParityTest.cpp`, `native/tests/GraphicsParityImageApple.mm`, `native/tests/MetalGraphicsParityTest.mm`, `native/tests/golden/metal-graphics/` (generated).
- Modify: `native/CMakeLists.txt` (sources; a third Apple ctest `corevideo-native-tests-golden`, filter `GraphicsRasterGolden.*`, `LABELS golden`, excluded from the main entry the way `COREVIDEO_REALTIME_TEST_FILTER` is), `.github/workflows/ci.yml` (Metal job ctest `-LE "realtime|golden"`, with a comment that goldens are blocking in `scripts/release-macos.sh`'s full ctest and advisory on runners whose macOS differs).

**Fixture index (inputs only).** One entry per fixture:
`{name, source: "manifest:<file>" | "curated:<templateId>" | "legacy:<lower-third|caption>", fields, brand, width, height}`.

- The closed list from spec 7.3: `lower-third-corporate`, one per curated template id in `GraphicsTemplate::curated()` at default fields, `text-wrap-ellipsis-3-lines`, `text-fit-min-size`, `text-align-center-right`, `text-outline-shadow-blur`, `shapes-inset-strokes`, `gradient-alpha`, `image-contain-cover-align` (it includes a PNG with an embedded ICC profile), `image-scale`, `table-12x8`, `crawl-atlas-two-items`, `layered-motion-mid-entrance`, `legacy-lower-third-appearance`, `legacy-caption`, `segoe-substitution`.
- Plus, for the G2 carried decisions: `text-small-14px` (D9), `text-emoji` (D6), and `text-advance-crawl`. The last is a crawl of ten one-word items in Arial 40, whose resolved item boxes **are** the per-word advance widths.
- Manifests use `Arial` or `Georgia` except `segoe-substitution` and the curated looks.
- `layered-motion-mid-entrance` is rastered as the atlas (the slices are compared). Its motion sample point is not a raster input.
- Every raster is 1920x1080 unless the template's own bounds say otherwise. The raster is `rect {0,0,1,1}` times the fixture size.
- Invariants, checked by `GraphicsRasterParity.IndexIsWellFormed`: every manifest loads, every field id exists in its manifest, and every name is unique.

**Interfaces** (`GraphicsRasterParity.h`, portable C++, no platform headers):
```cpp
struct ParityImage { int width = 0, height = 0; std::vector<uint8_t> bgraPremultiplied; };
struct ParityRegion { std::string id; bool text; double x, y, width, height; double linePitch; int maxLines; };
rpc::Json layoutSummary(const graphics::GraphicsDrawing&, double scale);   // primitives {id,type,box,fontSize,crawlStart}, crawl.passWidth
std::vector<ParityRegion> regionsFromLayout(const rpc::Json& summary);     // text and geometry regions in raster px
struct RegionMetrics { int inkX0, inkY0, inkX1, inkY1; double summedAlpha; int lineBands; int baselineRow; };
RegionMetrics measureRegion(const ParityImage&, const ParityRegion&);       // ink: alpha > 8; baseline: lowest row with alpha > 128
struct Verdict { bool pass; std::vector<std::string> failures; };          // each failure names fixture, region, metric, both values, tolerance
Verdict judgeGeometry(const ParityImage& reference, const ParityImage& candidate, const std::vector<ParityRegion>&);  // tier 1
Verdict judgeText(const ParityImage& reference, const ParityImage& candidate, const std::vector<ParityRegion>&, double toleranceScale); // tier 2
Verdict judgeLayout(const rpc::Json& reference, const rpc::Json& candidate);  // widths/passWidth 1.5 %, fit size within (fontSize - minFontSize) / 256
double alphaSsim(const ParityImage&, const ParityImage&, const ParityRegion&);
```

Tolerances are the spec 7.3 numbers verbatim. `segoe-substitution` and the curated looks use `toleranceScale = 2` for text. Definitions the spec leaves open, decided here:

- **Edge pixel:** its 3x3 neighbourhood in the reference image has any channel range above 2; otherwise it is interior.
- **Blur:** the 3x3 binomial kernel `[1 2 1]/4`, separable.
- **SSIM:** 7x7 windows at stride 1, `C1 = (0.01*255)^2`, `C2 = (0.03*255)^2`, mean over the region.
- **Line bands:** the number of `linePitch`-high bands starting at the region top that contain ink.
- **Atlas fixtures:** regions come per slice, from the layout summary's slices.

**Truncation:** the spec's truncated flag is not observable without editing the Windows raster. A different truncation point is judged through the ink right edge (±2 px), summed alpha (8 %) and line bands. Record this as a known limit in the outcomes.

**PNG container (`GraphicsParityImageApple.mm`):** `writeParityPng(path, ParityImage)` / `readParityPng(path, ParityImage&, std::string& error)`. The PNG is colour type 6, 8-bit. The samples **are** the premultiplied B,G,R,A bytes, reordered to R,G,B,A and written verbatim. A reader must take the samples verbatim: `CGImageSource` → `CGDataProviderCopyData`, never a drawing context. Windows writes the same layout with WIC `GUID_WICPixelFormat32bppBGRA` from memory (Task 7).

- [ ] **Step 1: Write the comparator self-tests** (`GraphicsRasterParityTest.cpp`, synthetic images, header comment: "tests of the comparator, not parity evidence").
  - `IdenticalImagesPassBothTiers`.
  - `ATwoPixelShiftFailsTheInkBox`.
  - `AlphaScaledBy0_85FailsSummedAlpha`.
  - `AOnePixelBaselineMoveFailsTheBaseline`: a 1 px move passes and a 2 px move fails.
  - `InteriorChannelOff3Fails`, with `off2` passing.
  - `StrayInkOutsideTheGrownBoxFails`: alpha 9 at 2 px outside fails, alpha 8 passes.
  - `LayoutWidthOff1_6PercentFails`, with 1.4 % passing.
  - `IndexIsWellFormed`.
  - `ParityPng.RoundTripKeepsPremultipliedSamplesVerbatim` with pixels `(10,20,30,40)`, `(0,0,0,0)`, `(255,255,255,255)`, `(1,0,0,1)`.
- [ ] **Step 2: Run them.** Expected: FAIL (no implementation).
- [ ] **Step 3: Implement** `GraphicsRasterParity.cpp` and the ImageIO reader/writer. Run again: PASS.
- [ ] **Step 4: Write `MetalGraphicsParityTest.mm`.**
  - `renderFixture(name) -> {ParityImage, rpc::Json layout, std::string state, std::string warning}`. It uses `CoreTextOverlayRaster` with strict resources, waits for terminal state, reads back with `getBytes`, and takes the layout from `definition->resolve` with the fixture's fields and brand.
  - `GraphicsRasterGolden.MacMatchesMacGoldens`: for each fixture, judge geometry + text + layout against `native/tests/golden/metal-graphics/<name>.png` and `manifest.json`. With `COREVIDEO_WRITE_MAC_GOLDENS=1` it instead writes the PNGs and a `manifest.json` (`{macOS: sw_vers, commit, fixtures:[{name, sha256, state, warning, layout}]}`) and fails with "goldens written; review the diff". A write run can never pass silently.
  - `GraphicsRasterParity.MacMatchesWindowsReference`: if `contracts/graphics-raster-reference.json` is missing, print `"[parity] UNVERIFIED: no Windows reference (Task 7 not run)"` and return. Otherwise run the full judgement per fixture, and with `COREVIDEO_GRAPHICS_PARITY_ARTIFACTS=<dir>` write `<name>-{windows,mac,diff}.png` and an `index.html` table of every metric with pass/fail.
- [ ] **Step 5: Generate the Mac goldens** on this Mac:
  Run: `COREVIDEO_WRITE_MAC_GOLDENS=1 ./native/build-metal/corevideo-native-tests --gtest_filter='GraphicsRasterGolden.*'`, then the same without the variable.
  Expected: the first run fails with "goldens written"; the second PASSES. Look at three PNGs yourself (`lower-third-corporate`, `table-12x8`, `segoe-substitution`) before committing. A golden of a wrong raster would pin the wrong raster.
- [ ] **Step 6: Mutations.**
  - Move the template baseline by 2 px in `CoreTextOverlayRaster.mm` (goldens red on text fixtures).
  - Swap the gradient stops (`gradient-alpha` red).
  - Change `kCGInterpolationMedium` to `None` (`image-scale` red).
- [ ] **Step 7: Run** `ctest --test-dir native/build-metal -LE "realtime|golden"` and plain `ctest --test-dir native/build-metal`. Expected: both pass; the second includes goldens.
- [ ] **Step 8: Commit.** `test(metal-graphics): parity fixtures, comparator and Mac goldens (G4)`

### Task 7: Windows reference writer and the D3D11 contract instantiation (the one Windows session)

**Size:** M. **Depends on:** Tasks 2 and 6. Code is written on the Mac, then compiled and run on Windows. **Needs the owner's Windows session** (owner decision 1).

**Files:**
- Create: `native/tests/D3D11GraphicsReferenceTest.cpp` (whole file under `#if COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS`).
- Modify: `native/CMakeLists.txt`. Add it to the all-platform test list next to `tests/D3D11CompositorTest.cpp` (`:1018`). It is gated inside, as that file is, and the `GraphicsRasterParity.cpp` sources are already listed by Task 6.
- Generated on Windows: `contracts/graphics-raster-reference.json`, `native/tests/golden/windows-graphics/<fixture>.png`, `native/tests/golden/windows-graphics/catalog.json`.

**Interfaces:**
- Consumes: the Task 6 index, `layoutSummary`, `regionsFromLayout`, `judge*`; the Task 2 contract templates.
- Produces the reference JSON:
  ```
  {schemaVersion:1, generator:"GraphicsRasterReference.WriteWindowsReference", commit,
   environment:{windowsBuild, fonts:{"arial.ttf":ver,"georgia.ttf":ver,"segoeui.ttf":ver,"seguisb.ttf":ver}, "d2d1.dll":ver, "dwrite.dll":ver},
   fixtures:[{name, width, height, png, sha256, samples:[{x,y,bgra:[b,g,r,a]} x3], state, warning, layout, slices?}]}
  ```
  `samples` are three non-transparent pixels, so the Mac can prove it reads the Windows PNG verbatim.

- [ ] **Step 1 (Mac): Write the Windows file.**
  - A `D3D11ContractBackend` traits struct (device from `D3D11CreateDevice` hardware, then WARP).
  - Each contract case, as `TEST(OverlayRasterContract, <CaseName>)`, minus the Metal-only emoji case.
  - `GraphicsRasterReference.WriteWindowsReference`, which runs only when `COREVIDEO_WRITE_GRAPHICS_REFERENCE=<repo root>` is set (otherwise it prints "not requested" and returns). It renders each fixture through `CompositorOverlayRaster` with strict resources until terminal. It reads the texture back by `CopyResource` into a `D3D11_USAGE_STAGING` texture, then `Map`. It writes the PNG (WIC, `GUID_WICPixelFormat32bppBGRA`, from memory) and re-reads it with WIC asserting byte equality. It records the layout from `definition->resolve` and the slices from `activeRuns()`, then writes the JSON.
  - `GraphicsRasterReference.WindowsMatchesItsOwnReference`, which re-renders and judges against the committed reference (catches a Windows font or OS update later).

  Keep it to `CompositorOverlayRaster`'s public surface (`CompositorOverlayRaster.h:42-87`).
- [ ] **Step 2 (Mac): Make the Mac side verify the samples.** `MacMatchesWindowsReference` asserts that each recorded `samples[]` pixel equals the decoded PNG pixel at `(x, y)`.
- [ ] **Step 3 (Mac): Push the branch.** It cannot compile here; say so in the commit message.
- [ ] **Step 4 (Windows session): Build and run.**
  ```powershell
  cmake --build native/build-dev --config Release --target corevideo-native-tests
  native\build-dev\corevideo-native-tests.exe --gtest_filter=OverlayRasterContract.*:GraphicsRasterParity*:GraphicsCompositionPixel.*
  ```
  Expected: the contract cases pass on D3D11. A red case is a real Windows/Mac contract difference: record it and do not edit the Windows raster in this plan (see the agenda). Check the binary size and `--config Release` per CLAUDE.md before trusting anything.
- [ ] **Step 5 (Windows session): Write the reference.**
  ```powershell
  $env:COREVIDEO_WRITE_GRAPHICS_REFERENCE=(Get-Location).Path
  $env:COREVIDEO_GRAPHICS_CATALOG_EXPORT="native\tests\golden\windows-graphics\catalog.json"
  native\build-dev\corevideo-native-tests.exe --gtest_filter=GraphicsRasterReference.*:GraphicsTemplateRegistry.ThumbnailsAreExplicitWorkerProductsWithoutInstallingVersions
  ```
  Expected: one PNG per fixture, the reference JSON, and the catalog export with real D2D thumbnails. Then run `WindowsMatchesItsOwnReference`: PASS. Commit the generated files from the Windows machine with the environment block filled in.
- [ ] **Step 6 (Windows session): Agenda items. List them, do not do them in this plan.**
  - D3D11 fact warning copy (G2 review 2).
  - `CompositorOverlayRaster::preparedResource()` memo (G2 D7).
  - Fold the `GraphicsThumbnail.cpp` D3D11 branch onto a shared overlay helper (Task 5).
  - Extract the raster front half (spec 5.1).

  Each becomes its own backlog item if the owner wants it.
- [ ] **Step 7: Commit (Windows).** `test(graphics): Windows D2D raster reference for the Metal parity standard (G4)`

### Task 8: Judge Mac against Windows; decide D5/D6/D9; owner sheet

**Size:** S. **Depends on:** Task 7.

**Files:**
- Modify: `native/src/compositor/CoreTextOverlayRaster.mm`, only if a decision below changes the raster. Then regenerate the Mac goldens with the Task 6 command, in a separate commit.

- [ ] **Step 1: Run the parity test with artifacts.**
  Run: `COREVIDEO_GRAPHICS_PARITY_ARTIFACTS=artifacts/graphics-parity ./native/build-metal/corevideo-native-tests --gtest_filter='GraphicsRasterParity.*'`
  Expected: the samples match (else the PNG container is wrong: stop and fix Task 6/7). The judgement result is whatever it is. **Do not widen a tolerance to make a fixture pass.**
- [ ] **Step 2: Decide from the data**, each written into the outcomes with the measured numbers:
  - **D5 gradient alpha:** if `gradient-alpha` fails tier 1 interior while other geometry passes, draw the gradient from a 256-step table computed as D2D does (spec 5.4) and re-judge.
  - **D9 small text:** compare `text-small-14px` summed alpha with smoothing on and off. Keep the setting whose summed-alpha error is smaller, and record the social-card floor note beside it.
  - **D6 emoji:** record the `text-emoji` difference. Colour stays, per the default.
  - **Image filter:** keep the interpolation constant that passes `image-scale` tier 1, trying `Medium` then `Low`.
- [ ] **Step 3: Hand the sheet to the owner.** Open `artifacts/graphics-parity/index.html` and give it to the owner. Owner decision 4 if any Arial/Georgia fixture fails tier 2; owner decision 5 for Selawik on air.
- [ ] **Step 4: Commit** any raster change plus regenerated goldens: `fix(metal-graphics): <decision> from the Windows parity reference (G4)`.

### Task 9: `--graphics-cue` drill and the quiet-machine delivery pair

**Size:** M. **Depends on:** Task 3 (crawl proven); otherwise independent. `scripts/mac-show-drill.py` is shared with sibling lanes: rebase before review.

**Files:**
- Modify: `scripts/mac-show-drill.py` (argument block `:435-480`, traffic loop `:613-657`, assertions after `:660`).

**Behaviour:**
- The flag requires `graphics-instances` in the snapshot's `capabilities`, else FAIL "core does not advertise graphics-instances".
- It reads `coreEpoch` from `snapshot.graphics.coreEpoch`. Every graphics request is `{"type": "graphics-...", "graphics": {schemaVersion:1, requestId, clientId:"show-drill", coreEpoch, ...}}`, the wire `GraphicsGateway.swift:237` sends.
- After configure, it installs `drill-crawl` by `graphics-template-install` with an inline manifest: the Task 3 crawl manifest at 1920x96 bounds, Arial 48, speed 240, loop.
- It then creates three global instances:
  - `drill-lt` from `transparent-name` (lower-third family, no image field), fields `name` / `title`;
  - `drill-clock` from `clock-panel`, configured with the clock wire row in `contracts/graphics-shell/names-clock.vectors.json` (real Windows output);
  - `drill-crawl`.

  Each is cued and shown.
- Traffic, on the existing 2 Hz loop, replacing the plain poll on non-recue ticks:
  - every second, `graphics-instance-update target:"program"` on `drill-lt` with `name: "DRILL GUEST <second>"`. This is a fresh preparation each second, so a 40 s run crosses the 16-revision limit about 2.5 times;
  - every fifth second, `graphics-instance-trigger action:"hide"` then, the next second, `"show"`, on `drill-lt`;
  - every tenth second, an update of `drill-crawl`'s `crawl-items` (boundary adoption under load).
- Assertions, sampled at each 2 Hz poll (the drill sees snapshots, not every frame, and prints that it is sampling):
  - every instance reaches `firstDrawSubmittedFrame > 0`;
  - no instance ever shows `preparation == "failed"` or `cuePreparation == "failed"`;
  - `programFrame.warnings` never contains `graphics-preparation-failed` at a sample;
  - `graphics.resourceBudgetAtStatePublication.residentHighWater <= residentLimit`;
  - at the end every instance is removed, then one new `drill-after` instance reaches `firstDrawSubmittedFrame > 0`, proving the resident limit is not wedged (correction 5).
- Printed, not gated: command-to-`ready` per update at poll granularity (say so), the crawl's first cold preparation, and the final budget counters.

- [ ] **Step 1: Prove the guard on a stub core.** Run `python3 scripts/mac-show-drill.py --seconds 10 --graphics-cue --core native/build-stub/corevideo-native`. Expected: FAIL "core does not advertise graphics-instances". That proves the guard is live.
- [ ] **Step 2: Implement the flag.**
- [ ] **Step 3: Unloaded run.** Run `python3 scripts/mac-show-drill.py --seconds 25 --graphics-cue --advisory-recorded-rate`. Expected: PASS, all graphics assertions printed.
- [ ] **Step 4: Mutations against the drill**, each red with the named assertion:
  - Create `drill-lt` with a font not installed and not in the table (`preparation failed`).
  - Skip the trigger (`firstDrawSubmittedFrame`).
  - Point `coreEpoch` at a stale value (a FAIL with the refusal reason, not a silent pass).
- [ ] **Step 5: Quiet-machine pair (owner decision 3: no other agent building; record the load average before each run).** Release core (`ls -l native/build-metal/corevideo-native` and record the size), `COREVIDEO_FAKE_ENGINE_FPS=60`:
  ```bash
  python3 scripts/mac-show-drill.py --seconds 40 --load 8
  python3 scripts/mac-show-drill.py --seconds 40 --load 8 --graphics-cue
  python3 scripts/mac-show-drill.py --seconds 40 --load 8 --lower-third
  ```
  Pass rule (spec 7.4, CLAUDE.md frame-delivery acceptance): both graphics runs pass every existing gate and show no dropped frame or delivery shortfall that the baseline did not show. If the baseline itself fails, the pair is **inconclusive**: record it as unverified with the load average, and do not call it a pass.
- [ ] **Step 6: Shell capture (G1 carried).** With the shipped lower third on air, take a `COREVIDEO_SHELL_SNAPSHOT` Program capture and attach it to the outcomes.
- [ ] **Step 7: Commit.** `test(mac-drill): --graphics-cue drill with a lower third, clock and crawl (G4)`

### Task 10: Outcomes

**Size:** S. **Depends on:** all.

- [ ] **Step 1: Write `docs/superpowers/plans/2026-11-22-mac-metal-graphics-g3-g5-outcomes.md`.** Include:
  - commits;
  - test counts with commands;
  - every mutation and its result;
  - fixture defects found;
  - the Task 8 numbers per fixture and decisions;
  - the drill pair (load averages, binary size);
  - what stayed unverified;
  - the Windows-session agenda;
  - corrections to the master plan (section "What changed since the spec" above, plus anything new).
- [ ] **Step 2: Commit.** `docs: Metal graphics G3-G5 outcomes`

---

## Owner decisions needed

1. **Where the Windows reference comes from.** Recommendation: one session on the owner's Windows 11 PC. It has the real customer fonts and it also runs the D3D11 contract cases. The alternative is a `workflow_dispatch` job on `windows-latest`: no owner time, but Windows Server font versions, so the reference describes a machine no customer uses.
2. **Land G5 now or with Graphics shell G8.** No Mac surface shows thumbnails until G8 (Phase 4). Recommendation: land now. It is small and tested end to end through the registry, and it removes a standing `unsupported` parity gap in the shared contract.
3. **A quiet-machine window for the drill pair.** G1 and G2 could not judge frame delivery at load averages of 6-9. About 10 minutes with no other agents building.
4. **If an Arial/Georgia fixture fails tier 2** (a rasterizer difference, not a font substitution): accept and name the difference, or fund raster work. Recommendation: decide per fixture from the sheet. Tolerances are never widened silently.
5. **Selawik in place of Segoe UI on air** (spec decision 1, still open for "acceptable on air"), judged from the Task 8 sheet.

## Risks

- **The Windows test code is written blind.** No D3D11 headers exist here. Budget a compile-fix round at the start of the Windows session. Everything the session needs is in one file plus a CMake line.
- **Ported D3D11 tests may expose real Metal gaps (Task 1),** most likely the clip-rect scissor on a remapped Program monitor and Preview-pass cue drawing. Fixes may need `MetalCompositorAdapter.mm`, which Tiles N1 also edits.
- **Mac goldens churn across macOS updates.** They are blocking only in the release ctest on this Mac. A macOS update that moves them needs a reviewed regeneration, not a tolerance change.
- **Process-global budget in tests.** The capacity cases fill the shared `GraphicsResourceBudget::process()`. A missed release would break later tests in the same binary, hence the scope guards. Run `OverlayRasterContract.*` alone and in the full binary.
- **Parity may simply fail for Core Text vs DirectWrite at small sizes** (G2's 3.7 px stem note). The plan reports the numbers. It cannot make Core Text hint like DirectWrite.

## Self-review notes

- Spec rows: G3 maps to Tasks 1-4, G4 to Tasks 6-9 (7.3 standard and 7.4 drill), G5 to Task 5. Spec 7.1 maps to Task 2 (Mac) and Task 7 (Windows). G6 is excluded on purpose (master plan Phase 5).
- Names used across tasks: `MetalGraphicsRig` and `renderUntilPrepared` (Task 1 → 3, 4, 5); `OverlayRasterContractCases.h` cases (Task 2 → 5, 7); `ParityImage`, `layoutSummary`, `regionsFromLayout`, `judgeGeometry`, `judgeText`, `judgeLayout` (Task 6 → 7, 8); `coreTextThumbnailPng` (Task 5); `bundledGraphicsFontsSummary` (Task 4 → 5).
