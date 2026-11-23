# Metal graphics G3-G5: outcomes

Plan: `2026-11-22-mac-metal-graphics-g3-g5.md`. Branch `mac-metal-graphics-g3-g5`, draft PR #1230.
Rebased on `origin/main` 3529d568. Toolchain: native builds with
`DEVELOPER_DIR=/Library/Developer/CommandLineTools` and the CLT `MacOSX26.5.sdk`. One build dir per
toolchain (`native/build-metal`, `native/build-stub`).

## Status by task

| Task | State |
|---|---|
| 1 Port the D3D11-only graphics tests | Done. Five ports pass; no Metal gap found. |
| 2 Raster contract cases | Done. Two real bugs found and fixed (below). |
| 3 1080p crawl boundary on Metal | Done. Passes; no crawl gap found. |
| 4 Storms, retention under pressure, font log | Done. |
| 5 Library thumbnails on Metal (G5) | Done. Installed-layout leg passes. |
| 6 Parity fixtures, comparator, Mac goldens | Done. 23 goldens on macOS 27.2.1. |
| 7 Windows reference writer, D3D11 contract | **Mac half done. Windows half BLOCKED on the owner's Windows session (owner decision 1).** The file has never compiled. |
| 8 Judge Mac vs Windows; D5/D6/D9; owner sheet | **BLOCKED on Task 7.** `MacMatchesWindowsReference` prints `UNVERIFIED: no Windows reference`. |
| 9 `--graphics-cue` drill | Drill done and proven. **Quiet-machine pair and shell capture DEFERRED** (load average 11-38 the whole session). |
| 10 Outcomes | This file. |

## Commits (oldest first)

- `51c7baed` Task 1: port the D3D11-only graphics compositor tests.
- `b1b8e495` fix: an image whose pixels do not decode fails instead of airing blank.
- `0ccdac22` fix: admit a crawl at its atlas bound, not its raster size (shared helper; Metal uses it).
- `a730496d` Task 2: the raster contract cases.
- `3bbbcf4a` Task 3: the crawl boundary on a 1080p Metal Program.
- `b9baf47b` review fixes, Tasks 2-3.
- `a432c2df` Task 4: storms, retention under cache pressure, graphics font log.
- `f5c2142e` Task 5: library thumbnails on Metal.
- `b6fd5be0` review fixes, Tasks 4-5.
- `1e5a6cc2` Task 6: parity fixtures, comparator, Mac goldens.
- `bd92d8b0` Task 7, Mac half: Windows reference writer and D3D11 contract (blind).
- `35a68131` review fixes, Task 6.
- `4a42cf8e` Task 9: `--graphics-cue` drill.
- `35523976` review fixes, Task 7 (the WIC PNG format; the reference must be `ready`).
- `f51fc0aa` review fixes, Task 9 (graphics stay on air through the gated windows).

## Bugs found and fixed

1. **Metal aired a corrupt image as a blank, and reported it `ready`.** ImageIO defers decoding. For a
   PNG with a valid header and a corrupt IDAT, `CGImageSourceCreateImageAtIndex` returns a `CGImage`
   whose data provider yields no bytes and which draws nothing. Windows fails the same asset with
   `Graphics image could not be decoded: <id>`. Fixed by `decodeImageNow()` (forced decode on the
   worker) at both decode sites.
2. **Both platforms refused any crawl longer than about two raster widths.** The cache admitted a crawl
   at `w*h*8`, the plain-raster bound, but a crawl prepares into an atlas of one slice per item. The
   worker then failed it with `Graphics preparation exceeded its admitted budget.` Ten 12-character
   headlines in a 1920x96 crawl fail this way. Found by Review Focus 1. Fixed for Metal through the
   shared `graphics::overlayAdmissionBytes`. **Windows still has it**; see agenda item 1.

## Tests (numbers from this session)

- `OverlayRasterContract.*` (Metal): 14 cases (12 contract and 2 thumbnail), all pass.
- `MetalGraphicsBounds.*`: 9 cases, all pass, each 3x repeat-stable where timing could matter.
- `GraphicsRaster*:ParityPng.*`: 16/16 on Metal, 14/14 on stub (the portable half).
- `GraphicsTemplateRegistry.*`, `GraphicsCompositionPixel.Thumbnail*`, `GraphicsFontResolver.*` and
  `GraphicsTemplate.Crawl*` all pass.
- Stub ctest: 2060 + 2 pass, 0 failed (`ctest --test-dir native/build-stub`).
- Installed-layout script `python3 -I scripts/tests/test_core_graphics_fonts_location.py`: PASS (four
  legs, incl. thumbnails with and without `COREVIDEO_GRAPHICS_FONTS_DIR`).
- Full Metal ctest `-LE realtime`: 4/4 at the start of the session (267 s). **Later runs timed out at
  the 900 s entry limit.** That is NOT this lane: `ZoomEngineRuntime.AuthAndJoinTimeoutsRetireHelperAndRejectLateEvents`
  took 990 s inside a loaded full run and 60 s alone. Two other unrelated tests took 182 s and 138 s.
  Every test in this lane runs in under 1 s. See candidate issues.

## Mutations (all reverted)

Each mutation turned the named test red, except where a row says it survived.

| Task | Mutation | Result |
|---|---|---|
| 1 | clip rect ignored; failed-warning dropped; preview draws Program revision; cache key without brand; first layout cue only | each red (`51c7baed`) |
| 2 | rule 7 returns nil | ScaleRetention red |
| 2 | drop " Retaining the accepted raster." | FailedScale red |
| 2 | skip capacity check | Capacity red |
| 2 | retryable result published | **survived the state-only check**: the refusal's own trim retires it first. A `completed` counter assertion added; now red |
| 2 | no deviceIdentity in compatible | ForeignDevice red |
| 2 | drop the 16384 atlas check | survives by construction: `packAtlas` already refuses (as the plan allowed) |
| 2 | no forced decode | ImageDecode + FailedScale red |
| 2 | admission ignores crawls | crawl contract + `GraphicsTemplate.CrawlAdmission...` red |
| 2 (review) | emoji/CJK text dropped | red (prefix-ink comparison) |
| 3 | adopt the pending crawl immediately | "aired before the boundary" red |
| 3 | ignore graphicsCrawlDistance | **survived the plan's after != first check** (the list change differs anyway); added movement check, now red |
| 4 | a storm cue update retargets Program | **survived the storm loop** (a Program update also waits for ready); post-settle check added, now red |
| 4 | retention keeps no lease | **survived** until the review rebuilt the test (the trims never ran: the blocked worker is the one that processes them). Now held by budget slots; red on tick 0 |
| 4 | face count from a constant, against a font directory copy missing one face | red (control passes) |
| 5 | premultiplied encoded as straight | alpha test red |
| 5 | skip crawl shift / wrong sign | red (after the review added a second item and a direction check) |
| 5 | atlas for thumbnails | layered==settled red |
| 5 | keep the device refusal | registry test red |
| 5 | hold the thumbnail budget lease in a static | **survived the plan's "later Program revision cannot reserve" check** (~230 KB of 128 MiB). Budget-return assertion added, now red |
| 6 | template baseline +2 px | goldens red (`title-text baseline: 118 vs 120`) |
| 6 | gradient stops swapped | gradient-alpha interior red |
| 6 | interpolation Medium -> None | image fixture edge band red |
| 6 (review) | text in tier 1 summed alpha; alpha-only coverage | red |
| 6 (review) | SSIM check removed from judgeText | **survived** the function-only test; isolated case added, now red |
| 9 | unknown font; trigger skipped; stale coreEpoch | each a drill FAIL with the named reason |

Plan mutations that were wrong as written (each recorded in its commit): Task 3 "ignore crawl
distance" (reds `after != first`), Task 5 "static lease" (a Program revision cannot be blocked by it),
Task 4 "release on trim" (the trim never ran in the planned shape).

## Fixture defects found

- Contract crawl fixture: `maxLength` 8192 exceeds the manifest maximum (4096); `"loop": true` is not a
  crawl property (`"mode": "loop"`).
- Corrupt image: removing the IDAT drops the pixel size from ImageIO's header properties, so it tested
  the header check, not the decode. Zeroing the IDAT payload (CRC repaired) is the right fixture.
- Parity fixtures: a line layer needs `"diagonal"` as a string; table rows must come from fields.
- Parity comparator: legacy caption lines report `fontSize` 0 (now derived from the line box).

## Drill

- `--graphics-cue`, unloaded (25 s, release core, load ~12), after the review fixes:
  - PASSED;
  - last render window with graphics on air: 60.1 fps, 0 dropped, worst 22.0 ms;
  - recorded 58.9 fps with traffic running;
  - lower-third command-to-ready n=26, p50 510 ms. The max was 3044 ms: one update waited while the
    budget refused 55 times at the 16-resident cap. This is printed, not gated, and worth watching in
    the quiet-machine pair;
  - crawl cold preparation 518 ms;
  - resident high water 16 of 16.
- The post-removal `drill-after` graphic drew, so the resident limit is not wedged. The resident count
  stays at 16 after every instance is removed, as plan correction 5 predicts.
- **Quiet-machine pair (baseline / `--graphics-cue` / `--lower-third`, `--load 8`): NOT RUN.** It needs
  owner decision 3 (about 10 minutes with no other agent building). Binary size and load average must
  be recorded then.

## Unverified

- Windows parity (Task 7 run, Task 8 judgement and the D5/D6/D9 and image-filter decisions). The
  owner sheet is not produced.
- Frame delivery with graphics on air (the quiet-machine pair). This is the G1 carried obligation,
  still open.
- The `COREVIDEO_SHELL_SNAPSHOT` before/after capture of the shipped lower third (G1 carried).
- The D3D11 contract instantiation: it has never compiled.

## Review findings fixed before the Windows session

- The Windows PNG container first used `GUID_WICPixelFormat32bppRGBA`, which the WIC PNG codec does
  not support. Every fixture would have failed. It now uses `32bppBGRA`, the codec's 8-bit RGBA format,
  and a round-trip test runs first in the file.
- The first drill version cleared Program before the recorded window, so its delivery gates measured
  Program without graphics. Fixed in `f51fc0aa`.

## Windows-session agenda (list only; nothing done here)

1. `CompositorOverlayRaster.cpp`: admit through `graphics::overlayAdmissionBytes` (the crawl bug
   above). Acceptance: `OverlayRasterContract.CrawlAtTheItemLimitPreparesOrFailsWithAtlasWording` on
   D3D11.
2. `CompositorOverlayRaster::prepareRaster`: call `compositor::invokeOverlayRasterPrepareHook()` (a
   one-line test seam), so `ScaleRetentionDrawsTheAcceptedRaster` can block the D3D11 worker.
3. Build `D3D11GraphicsReferenceTest.cpp` and fix compile errors. Run the contract cases. Write the
   reference (plan Task 7 steps 4-7) and commit it with the environment block filled in.
4. Carried from the plan: the D3D11 fact warning copy (G2 review 2); the
   `CompositorOverlayRaster::preparedResource()` memo (G2 D7); folding the `GraphicsThumbnail.cpp`
   D3D11 branch onto a shared helper with the new Metal branch; extracting the raster front half
   (spec 5.1).

## Known limits of the parity comparator

- The spec's "truncated" flag is not observable without editing the Windows raster. A different
  truncation point is judged through the ink right edge, summed alpha and line bands.
- Text is measured as coverage over the region border's most common colour:
  - weak on gradient plates;
  - an outline or shadow weighs by colour distance, not alpha;
  - grown text regions can overlap a neighbouring line (name/title), adding that line's ink.
- The `atlas` flag in `index.json` is hand-set, and nothing checks it against the raster's condition.
- Mac goldens are blocking only in the release ctest on this Mac; CI runs them advisory
  (`-LE "realtime|golden"`).

## Corrections to the master plan

All of the plan's "What changed since the spec" items held. New:

- The crawl admission bug exists on Windows too (plan correction 9 assumed the crawl path was
  shell-ready on Metal after G2; it was, but only for short crawls).
- A thumbnail refused at the 2-preparation limit is cached `unavailable` until the next request, on
  both platforms (the registry ignores the retry flag). This is a candidate issue.
- The full-resolution Program readback (`programFullBgra`) is a tap that fills on some frames only
  and trails the render. It is not a per-frame oracle; tests must harvest until stable.

## Candidate issues (not filed, per lane rules)

- Windows crawl admission (agenda 1). Customer-visible: long crawls fail on Windows today.
- A thumbnail refused by the shared budget is sticky until the next library request.
- `ZoomEngineRuntime.AuthAndJoinTimeoutsRetireHelperAndRejectLateEvents` takes 60 s alone and 990 s
  in a loaded full run, which pushes the Metal ctest main entry past its 900 s limit. Wall-clock bound.
- A crawl admission reserves 214-256 MiB of a raster cache's budget while it prepares. The bound is
  correct but loose; it could use the item count from the field value.
