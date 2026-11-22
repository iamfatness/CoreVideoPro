# macOS media Phase 2 (N3, N4): outcomes

Date 2026-11-21. Branch `mac-media-phase2` from `fc675541` (Mac Phase 1, PR #1220). Plan:
`docs/superpowers/plans/2026-11-21-mac-media-phase2.md`. Nothing pushed; no PR opened.

Rig: Apple M3 Max, macOS 26.3. Five other agents were building on this Mac throughout (load
average 6-19), so every timing result below is **UNVERIFIED under load**.

## Commits

| Commit | Task | What |
|---|---|---|
| `1b9f1d5c` | plan | Plan with Defaults taken and spec corrections |
| `bbb64f59` | 1 (N3) | Audio-only assets play, end on their audio, and draw nothing |
| `708a53a5` | 2 (N3) | A rotated (portrait phone) clip decodes upright at its display size |
| `6c289a7b` | 3 (N3) | Test only: an alpha sting keys over the scene on Program as a straight blend |
| `fba5da58` | 4 (N4) | Shared gates accept a GPU-only media frame |
| `a2052973` | 5 (N4) | Metal samples IOSurface frames with no upload, and never a recycled surface |
| `41f85d88` | 6 (N4) | The decoder hands Metal its own IOSurface buffer, with no CPU copy |

Each commit body lists its tests and its mutation results.

## Test counts (exact commands)

Configure uses the CI `native-metal-macos` flags (plan "Builds and gates"). Then:
`cmake --build native/build-metal -j 6 --target corevideo-native corevideo-native-tests corevideo-zoom-engine-fake`.

- `native/build-metal/corevideo-native-tests` (whole binary, realtime included):
  - baseline at `1b9f1d5c`: **2101 passed, 0 failed**
  - branch head `41f85d88`: **2129 passed, 0 failed** (+28 tests)
- Stub build at head: `cmake -S native -B native/build-stub -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build native/build-stub -j 4 && native/build-stub/corevideo-native-tests`
  gives **2021 passed, 0 failed**. The stub baseline was not measured.
- Repeats of the load-sensitive media filters. The runner has no `--gtest_repeat`, so these were
  looped in the shell. `--gtest_filter='AvfMedia*'` gave 36/36 on 8 more runs, with exit 0 and the
  summary line last. On one earlier run, `tail -1` printed a `[take]` log line instead of the summary.
  That run was not captured, so it is unexplained (most likely an async log line that landed after
  the summary). It did not happen again in 8 runs. `MetalZeroCopy.*` 3/3, `MediaTransports.*` 24/24,
  `MediaClipDecoder.*` 27/27 and `MediaVideoPresentation.*` 3/3 were each green twice.

## Proven (by a test that goes red when the code is wrong)

**N3: audio-only (audio beds).**
- Decoder: an audio-only asset presents one transparent 2x2 frame and raises no warning. Its
  duration comes from the audio track. It ends when the audio ends, and a loop never ends.
- Owner: a 7 s bed is still Live after the 6 s no-new-frame backstop and reaches Ended once its
  audio drains. The last audio heard is 6999/7000 ms.
- Real AVF: `.m4a` and `.wav` decode with tone and duration, and with no warning.
- Metal readback: an audio bed over a GREEN background leaves Program GREEN on every tick, with
  no slate. It ends about 1 s after it comes on air.

**N3: rotation.** A clip with a 90° transform decodes at 36x64 with hand-derived upright quadrants;
180° and 0° also pass. On Program it is pillarboxed and upright (Metal readback).

**N3: alpha.** A ProRes 4444 clip at a=0x80 over GREEN reads back as the straight-alpha blend.
That blend is computed from the fixture constants: BGR(32,128,128). An opaque mutation reads RED
and a premultiplied mutation reads R=72, so both go red.

**N4: no CPU copy on the hot path** (counted seams, both sides):
- Decoder: a process-wide copy counter has a delta of 0 across a 30-frame roll. Each frame
  `hasGpuPixels() && !hasPixels()`, with backend `AppleIOSurfaceBgra`.
- Compositor: `cachedUploads`/`scratchUploads` do not move and `zeroCopyBinds` advances.
- End to end through MediaCore, 1.5 s of a rolling clip: the BGRA path made 45 uploads, the
  zero-copy path made 0 uploads and 44 binds.
- In the live core (drill `[render]` line): `zerocopy=180` per 2 s window, which is 60 + 30
  media frames/s. Every media frame was zero-copy, and `uploads` are the 8 Zoom feeds only.

**N4: same pixels as the BGRA path.**
- Compositor: an IOSurface frame against its CPU-uploaded twin matches within 1 at 9 points.
- MediaCore: the zero-copy core against the BGRA core matches within 2 at 16 points.
- All 10 `AvfMediaPixel` tests also run as `…ZeroCopy` with identical expectations: roll, pause
  hold, cue→take, loop, still background, clip end, audio bed, alpha and portrait.

**N4: IOSurface lifetime.** `MetalZeroCopy.ASampledSurfaceIsNeverRecycledBeforeItsPassCompletes`:
- Setup: a pool with allocation threshold 2 and the GPU held by a test-only gate. Frame A then B
  go on one source, so B evicts A from the cache, and the test drops every reference.
- While the GPU is held, the pool must not hand A or B back.
- Once released, pass 1 must draw A's RED, not the GREEN a recycle would have written.
- After completion, A returns to the pool, so no lease leaks.
- Mutations:
  - Removing the lease turns it red.
  - With the ID check disabled, the pixel check reads the torn GREEN, BGR(32,224,32).
  - Releasing on the CPU timeline (at the next pass) also turns it red.
- Decoder side: frames 2-4, held across 40 more decodes, keep their own pixels. A non-retaining
  `PixelBufferVideoFrame` crashes that test.

**Preserved (existing suites green):** one source per asset, pause as a clock state, and the #1214
clip end (`MediaTransports.ANonLoopingClipEndsOnItsLastFrameWithItsWholeAudio`,
`AvfMediaPixel.ANonLoopingClipEndsOnProgramOnItsLastFrame` and its ZeroCopy twin).

## Unverified

- **Drill** (`python3 scripts/mac-show-drill.py --seconds 40 --load 8 --media`), run twice per
  build under load. "Before" is a `git archive` of `fc675541` built in the scratchpad.

  | run | load avg | result | fps / dropped / worst | Zoom delivery | coreMutex over | media decode/present | media copy |
  |---|---|---|---|---|---|---|---|
  | before 1 | 12.8 | FAIL | 60.0 / 0 / 18.3 ms | 73% | 5% | 60/60, 30/30 | avg 0.29 ms, max 2.66 ms |
  | after 1 | 9.1 | **PASS** | 60.1 / 0 / 18.1 ms | 98% | 1% | 60/60, 30/30 | 0.00 (wrap only) |
  | before 2 | 7.4 | FAIL | 60.0 / 0 / 18.1 ms | 78% | 15% | 60/60, 30/30 | avg 0.31-0.45 ms |
  | after 2 | 6.4 | FAIL | 60.0 / 0 / 18.4 ms | 74% | 2% | 60/60, 30/30 | 0.00 |

  - Render cadence was 60 fps with 0 dropped in all four runs, and the media sources held their
    rates with no restarts.
  - The one failing gate is Zoom decoded-frame delivery, on both builds. That points at machine
    load (the fake engine's 8x60 feeds), not the media path.
  - Phase 1 passed this drill on a quiet machine. A quiet re-run is needed before any perf claim.
  - Under the `--media` heading, the `[media-decoder] copy avg` line now times only the wrap.
- **Windows.** No Windows box was available. The shared changes are listed below and are inert by
  construction, but the Windows suite was not run.
- **The VideoToolbox decode-session limit with zero-copy.** The existing 16-decoder test still
  runs only on the BGRA factory.
- **Rotation of an alpha clip** (default P3): the composition output is opaque. That is a known
  limit, and it is not tested.

## Defaults taken (from the plan)

- **P1:** an audio bed's picture is fully transparent.
- **P2:** Mac honours `preferredTransform`.
- **P3:** rotation goes through `AVAssetReaderVideoCompositionOutput`, only for non-identity
  transforms.
- **P4:** zero-copy uses BGRA IOSurfaces only, with no NV12 path.
- **P5:** zero-copy is used only with a Metal compositor. `createAvFoundationMediaDecoderFactory()`
  keeps its CPU-BGRA meaning.
- **P6:** shared changes are additive and inert.
- **P7:** the GPU pass gate is a test-only seam in the `CompositorFaultInjection.h` shape.

## Shared-native changes (these reach Windows; flag in review)

- **`modules/MediaVideoPresentation.h`:**
  - `push`/`hold`/`hasFrame`/`refreshPrepared` now gate on `hasContent()`.
  - New `IMediaVideoPrefetch::mediaHasVideo()`, default true.
- **`core/MediaTransports.h`:**
  - `selectVideo` gates on `hasContent()`.
  - A delivered audio window counts as backstop progress only when `mediaHasVideo()` is false.
- **`core/MediaAssetSource.h`:** the descriptor size falls back to the GPU image's dimensions.
- **`modules/GpuVideoFrame.h`:** new enum value `AppleIOSurfaceBgra`.
- **`modules/Interfaces.h`:**
  - New field `CompositorSourceTexStats::zeroCopyBinds`.
  - New declaration `createAvFoundationZeroCopyMediaDecoderFactory()`.
  - Both are additive, but this widely-included header changed, so build with `--clean-first`
    before believing a crash.
- **`modules/ModuleComposition.cpp`:** chooses the zero-copy factory when the renderer is "metal".
  This is unreachable on Windows, because the MF factory wins first.
- **`core/MediaCore.cpp`:** the `[render]` log line appends `zerocopy=N`. The only parser,
  `scripts/validate-srt-ingest.mjs`, uses an unanchored regex and is unaffected.
- **`native/CMakeLists.txt`:**
  - New Apple sources, plus `-framework CoreVideo` in the Metal block.
  - New Metal-only test `tests/MetalZeroCopyTest.mm`.

The following are Mac-only or not wired on Windows: `MediaClipDecoder.{h,cpp}` (portable, compiled
everywhere, wired only on Mac), `AVFoundationMediaDecoderAdapter.mm`, `MetalCompositorAdapter.mm`
(about 40 lines of hooks; another agent edits this file, expect a small merge),
`MetalPixelBufferTextures.{h,mm}` and `PixelBufferVideoFrame.h`.

## Pkg-install resource rule (orchestrator note)

Phase 2 adds no resource that the core loads from next to its executable. The new code is compiled
into the binary, the Metal shaders were already source strings compiled at runtime, and
AVFoundation, CoreVideo and Metal are system frameworks. No env var or bundle-less test is needed
for this work.

## Follow-ups (not done; for the backlog)

- **Windows audio-only beds** (code-read, unverified): the MF adapter re-opens the video reader on
  every playing poll, so the `NO DECODER` log repeats, and the layer paints a slate over Program.
- **Windows rotation**: `MF_MT_VIDEO_ROTATION` is never read, so a portrait phone clip is likely
  sideways (unverified).
- Run the `--load 8 --media` drill on a quiet machine, before and after.
- Run the 16-decoder test against the zero-copy factory.
- Rename the drill's `copy avg` label in zero-copy mode.
