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
- ~~**Rotation of an alpha clip** (default P3): the composition output is opaque.~~ Fixed in the
  review round below: alpha now survives rotation, and a test covers it.

## Defaults taken (from the plan)

- **P1:** an audio bed's picture is fully transparent.
- **P2:** Mac honours `preferredTransform`.
- **P3:** only non-identity transforms are rotated. The review round below superseded the
  mechanism: it was `AVAssetReaderVideoCompositionOutput`, and is now a per-sample Core Image
  rotation.
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

## Review round (2026-11-22)

An independent review found no show-stopper: lifetime is sound, `leaks` found 0, the pool never
stalls with 90 buffers held, and Windows is inert. Four of its items were fixed on this branch. Item 5
(small per-pass allocations on the render thread) was skipped by orchestrator decision. A reboot
interrupted the round after item 3, and the rest was done in a resumed session.

| item | finding | commit | test (red before → green after) |
|---|---|---|---|
| 3 | the bed backstop test needed a 100 ms real-time window | `a53714b8` | mutation red, green 4/4 |
| 1 | a rotated alpha clip silently went opaque | `e96298c5` | `AvfMediaDecoder.ARotatedAlphaClipStaysUprightAndKeepsItsStraightAlpha`: before BGRA(225,32,32,**255**), after (225,32,32,**128**) at all 4 quadrants, BGRA and zero-copy factories |
| 4 | the rotation frame rate came from `minFrameDuration` (VFR) | `e96298c5` | `AvfMediaDecoder.AVariableFrameRateRotatedClipDecodesOneFramePerSourceSample`, **not red before** (see below) |
| 2 | an audio bed that stalled ≥ 6 s ended for good | `56068a13` | `MediaTransports.AnAudioBedStalledPastTheBackstopRecoversWhenItsAudioComesBack`: before, stayed Ended; after, Live again, 5/5 |

**Items 1 and 4: rotation is per decoded sample on the GPU now.**
- The orchestrator's preferred option was a draw-time affine in Metal. That would make every
  consumer of a media frame (compositor, CPU BGRA factory, thumbnails) honour an orientation field.
- Instead, the decoder always uses the plain track output. A rotated track's samples are turned
  upright by Core Image on the system Metal device, into an IOSurface/Metal-compatible
  `CVPixelBufferPool` buffer.
- Downstream nothing changes: zero-copy wraps the pool buffer like a decoder buffer, and the pool
  cannot recycle a buffer a frame or Metal pass still retains.
- Alpha: colour management is off (`NSNull` working/output/input colour spaces). Measured: Core Image
  reads BGRA as straight alpha, so the destination writes `Unpremultiplied`. Premultiplied output
  read the colour halved, (113,16,16,128).
- Quarter turns and flips sample nearest, so they are bit-exact. Any other transform (e.g. a
  QuickTime matrix with scale) samples linearly at the rounded display size.
- A Core Image failure fails the reader loudly ("cannot play this rotated clip"), never silently.
- Cost: one GPU render per frame for rotated clips only, waited on the reader queue (never the
  render thread). Unrotated clips are byte-for-byte unchanged. The drill was not re-run for this.
- Pkg-install rule: Core Image is a system framework, and nothing loads from next to the core
  executable. `native/CMakeLists.txt` links `-framework CoreImage` in the AVF media decoder block
  (Apple-only).

**Item 4 did not reproduce.**
- The VFR fixture has `minFrameDuration` 1.7 ms against a nominal 32 fps. Through the OLD composition
  path it still produced 16 frames for 16 samples on macOS 27.2, so AVFoundation does not over-render
  there.
- The composition path is gone anyway. The test pins one frame per source sample at its own
  timestamp, as a guard.
- Fixture finding: the H.264 writer merges a 1 ms pair (16 written, 15 decoded, on any path), so the
  VFR fixture is ProRes (`QuadrantClip::prores`).

**Item 2: a stalled bed recovers.**
- A bed in Ended whose decoder is not at EOS (`!mediaEnded()`) is still polled for audio. Delivered
  audio returns it to Live in place, keeping its position, as a new picture does for a video clip.
- A bed at real EOS is not polled. `AnAudioOnlyBedOutlivesTheNoNewFrameBackstop` still ends on drain.
- Shared code: the path needs `mediaHasVideo() == false`, which the MF decoder never reports
  (default true), so Windows behaviour is unchanged. Still not compiled on Windows.
- New test seam `MediaTransports::setWorkerClockForTesting` (production: `steady_clock`). The test
  crosses the 6 s window with one virtual jump; its real-time bounds are waits, not measurements.

**Runs** (load avg 4-9; another session was active):
- `native/build-metal/corevideo-native-tests`: **2132 passed, 0 failed**.
- `native/build-stub/corevideo-native-tests`: **2022 passed, 0 failed**.
- Shell: `cd mac-shell && swift build -c release`, then `COREVIDEO_SHELL_TESTS=1 …`.
  - First run: 5676 run, 1 failed: `ControlApiTests.swift:1162` "the slow client is connected".
    This is a control-API socket timing check, and this round changes no Swift.
  - Re-run: **5676 passed, 0 failed**.
- Selfcheck: PASS.

**Toolchain note (affects every lane on this Mac).**
- The Command Line Tools updated on 2026-11-21 at 23:32 and brought `MacOSX27.2.sdk`, whose `.tbd`
  files list `arm64e.x1` targets.
- The selected Xcode's `ld` (ld-1267) rejects them: "tapi error: unknown architecture". Every native
  link fails.
- Workaround: build native with `DEVELOPER_DIR=/Library/Developer/CommandLineTools` (ld-27106).
- Swift (`mac-shell`) still builds with the default Xcode toolchain, and fails under the CLT one.

**Still open:** the `--load 8 --media` frame-delivery drill on a quiet machine (orchestrator-owned),
and a Windows compile of the shared headers.
