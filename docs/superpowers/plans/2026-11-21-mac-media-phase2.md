# macOS media clip playback, Phase 2 (N3, N4) implementation plan

Status: executing, 2026-11-21. Branch `mac-media-phase2` (worktree
`.claude/worktrees/mac-media-phase2`), based on `fc675541` (Mac Phase 1, PR #1220).

> `docs/superpowers/plan-authoring-rules.md` is binding: every task names the mutations that must
> turn its tests red, and the task's commit body reports whether each did. When a test written here
> contradicts a reasonable implementation, the test is the first suspect.

**Goal.** Master plan `2026-11-15-mac-parity-master-plan.md` Phase 2, workstream "Media clip
playback":

- **N3** (S-M): audio-only assets (audio beds) play on air; clip alpha keys over the scene on
  Program; rotated (portrait phone) clips display upright.
- **N4** (L): AVFoundation decoder output (an IOSurface-backed `CVPixelBuffer`) is sampled by Metal
  through `CVMetalTextureCache` with no CPU BGRA copy on either side, and no surface is ever
  recycled while a Metal pass can still sample it.

**Spec:** `docs/superpowers/specs/2026-11-15-mac-media-clip-playback-design.md` §4.1 (pixel format
stage 2, alpha), §5 rows N3/N4, §7. Phase 1: `docs/superpowers/plans/2026-11-16-mac-media-phase1.md`.

**What must not regress** (reference `docs/reference/persistent-media-source.md`,
`docs/reference/source-bus.md` slices 3a/3b): one source id per asset; `core::MediaTransports`
owns play/pause/cue; pause is a clock state; #1214 clip end (Ended only after every queued frame was
shown and the audio tail drained, `MediaTransports.h` `kEndedAudioDrainCapMs`); the 6 s
no-new-frame backstop for decoders that cannot say they ended; 60 fps per-frame delivery.

---

## Defaults taken (owner may override)

| # | Default | Why | Tasks that change if overridden |
|---|---|---|---|
| P1 | **An audio-only asset's picture is a fully transparent 2x2 frame.** On air an audio bed draws nothing over the scene (not the warming/failed slate a frameless layer resolves to). | The only shapes are "draw a slate over the whole Program" (today, both platforms) or "draw nothing". A slate over Program for an audio bed is a defect, not a behaviour to match. The decoder is the component that knows the asset has no picture. | 1 |
| P2 | **Mac honours the track's `preferredTransform`** (90/180/270 and mirror): a portrait phone clip decodes upright at its display size. | The Windows adapter never reads `MF_MT_VIDEO_ROTATION` (grep: 0 hits); whether the MF video processor rotates is UNVERIFIED (no Windows box here). Matching a sideways picture is not parity. Recorded as a Windows follow-up in the outcomes doc. | 2 |
| P3 | **Rotation uses `AVAssetReaderVideoCompositionOutput` only when the transform is not identity.** Unrotated clips keep the plain track output. A rotated clip with alpha loses its alpha (the default video compositor renders opaque); recorded as a known limit. | Keeps the Phase 1 path byte-identical for every unrotated clip; rotation is a GPU render inside AVFoundation, not a CPU pass. | 2 |
| P4 | **Zero-copy is BGRA (`kCVPixelFormatType_32BGRA`) IOSurfaces**, one new `GpuVideoFrame` backend `AppleIOSurfaceBgra`. No NV12 shader path. | VideoToolbox already converts to BGRA on the GPU in N1; NV12 would add a second shader path and a colour-matrix question for no measured gain. | 5, 6 |
| P5 | **The production factory picks zero-copy only when the compositor is Metal** (`ModuleComposition.cpp`); `createAvFoundationMediaDecoderFactory()` keeps its N1 meaning (CPU BGRA) for every other configuration and for the Phase 1 tests. | Every CPU reader of a media frame on Mac was enumerated (Spec correction 3); none runs with a Metal compositor, but a Metal-less Mac build composites on the CPU and needs pixels. | 6 |
| P6 | **Shared-native changes are additive and inert on Windows**: `hasPixels()` → `hasContent()` at five media gates, one enum value, one `IMediaVideoPrefetch` virtual with a default, one `CompositorSourceTexStats` field, one MediaTransports backstop line keyed on that virtual. Each is listed in the outcomes doc. | Windows media frames always carry CPU pixels, and `hasContent() ⊇ hasPixels()`, so every Windows path takes the same branch. | 1, 4 |
| P7 | **The GPU-gate test seam** (`MetalPixelBufferTextures::armPassGateForTest`) follows `CompositorFaultInjection.h`: no env var, command or wire field reaches it; disarmed it costs one relaxed atomic load per pass. | A recycled-surface test must hold the GPU deterministically; timing it would be a flaky test that asserts nothing. | 5 |

## Spec corrections (verified against the code at `fc675541`)

1. **There are five `hasPixels()` media gates, not four:** `MediaVideoPresentation.h` `push` (:46),
   `hold` (:61), `hasFrame` (:80), `refreshPrepared` (:85), and `MediaTransports::selectVideo` (:393).
   Plus `MediaAssetSource::poll` derives descriptor size from `pixelWidth`/`i420Width` only (:66-67).
2. **`SourceContinuityLedger` and bus health need no change.** The ledger is fed at
   `MediaCore.cpp:6124` behind `hasContent()` already; `SourceBus::ingest` counts by `frameId` with no
   content gate (`SourceBus.h:105-127`); take-record "missing source" checks use `hasContent()`
   (`MediaCore.cpp:5586, 5605, 5672`).
3. **CPU readers of a media frame on a Metal Mac: none.** `IsoVideoIngress` refuses `media:` ids
   (`IsoVideoIngress.h:44`); `CpuVideoArrival::prepare` returns on `gpuPixels` and `preparation_` is
   D3D11-only (`ModuleComposition.cpp:437`); `ProgramSourceAdmission`, `PreparedProgramComposition`,
   `MonitorInputCache` are included only by `D3D11CompositorAdapter.cpp`/`MonitorRenderWorker.h`; the
   CPU program preview (`fillSyntheticProgramFramePreview`) runs only when the compositor left the
   preview empty on a full tick (`MediaCore.cpp:6231`), which Metal never does; no per-source media
   thumbnail exists on Mac (Sources S6 is unbuilt). A Metal-less build is the one CPU consumer (P5).
4. **Audio-only today, both platforms (code-read):** Mac — `openVideo` fails the reader with "the
   file has no video track" (`AVFoundationMediaDecoderAdapter.mm:273`), so `MediaClipDecoder` warns
   "could not be opened for Program playback" and logs `NO DECODER`; `MediaTransports` can never mark
   it Ended (`everProducedFrame` stays false); the layer has no frame and Metal paints
   `slateColorFor(...)` over Program. Audio does play (the audio reader is independent). Windows —
   `SetCurrentMediaType(FIRST_VIDEO_STREAM)` fails, the FFmpeg fallback fails on a file with no video
   stream, `openVideoReader` is re-tried on every playing poll (`state.reader` stays null), so the
   `NO DECODER` line repeats per poll; audio plays; the layer is a slate. UNVERIFIED by running.
5. **Rotation today:** `AVAssetReaderTrackOutput` hands frames over in storage orientation
   (preferredTransform is not applied), so a portrait phone clip is sideways on Mac; Windows never
   reads `MF_MT_VIDEO_ROTATION`.
6. **Alpha today:** straight alpha reaches the owner (`AvfMediaDecoder.AProRes4444AlphaClipReachesTheOwnerAsStraightAlpha`)
   and the Metal textured pipeline blends `SourceAlpha / OneMinusSourceAlpha`
   (`MetalCompositorAdapter.mm:519`), but nothing proves an alpha clip keys over the scene on
   Program. Windows forces every MF-decoded clip opaque (`MF:1194-1200`).
7. **The spec's N4 says "Windows suite unchanged".** No Windows box is available to this plan; the
   Windows suite is UNVERIFIED and the outcomes doc says so. The shared changes are kept to the inert
   shapes in P6 and the stub (portable) build plus the full Mac suite are the gates run here.

## Builds and gates

- Native (CI flags of `native-metal-macos`):
  `cmake -S native -B native/build-metal -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOREVIDEO_STUB=OFF -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DCOREVIDEO_WITH_METAL=ON -DCOREVIDEO_WITH_AVF_ENCODER=ON -DCOREVIDEO_WITH_AVF_MEDIA_DECODER=ON -DCOREVIDEO_WITH_COREAUDIO=ON -DCOREVIDEO_WITH_AVF_CAPTURE=ON -DCOREVIDEO_WITH_SCK=ON -DCOREVIDEO_WITH_RTMP_OUTPUT=ON -DCOREVIDEO_WITH_MAC_VIRTUALCAM=ON`
  then `cmake --build native/build-metal -j 6 --target corevideo-native corevideo-native-tests corevideo-zoom-engine-fake`.
- Stub (portable code, closest thing to the Windows compile available here):
  `cmake -S native -B native/build-stub -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build native/build-stub -j 6`.
- The test runner is custom: no `--gtest_repeat`; colon filters are unreliable, so filters run one
  pattern at a time and repeats loop in the shell.
- After any change to `Interfaces.h` / `GpuVideoFrame.h`: `--clean-first` before believing a crash.
- Five other agents build on this Mac. Timing tests and the drill are rerun before any conclusion and
  reported UNVERIFIED under load; no gate is loosened.

---

## Task 1 (N3a): audio-only assets play, end, and draw nothing

**Files:** `native/src/modules/MediaClipDecoder.{h,cpp}`, `native/src/modules/AVFoundationMediaDecoderAdapter.mm`,
`native/src/modules/MediaVideoPresentation.h` (one virtual), `native/src/core/MediaTransports.h` (backstop),
tests `MediaClipDecoderTest.cpp`, `MediaPlaybackTimelineTest.cpp` (MediaTransports cases),
`AvfMediaDecoderTest.cpp`, `AvfMediaPixelTest.cpp`, `AvfMediaFixtures.{h,mm}`.

**Interfaces.**

```cpp
// MediaClipDecoder.h
enum class ClipReadStatus { Ready, Pending, EndOfStream, Failed, NoVideoTrack };  // NoVideoTrack: sticky
class IClipAudioReader { ...  virtual int64_t duration100ns() const { return -1; } };
// MediaVideoPresentation.h, IMediaVideoPrefetch
virtual bool mediaHasVideo() const { return true; }  // false only once the decoder KNOWS there is no picture
```

**Behaviour.**
- The AVF video reader reports `NoVideoTrack` (not `Failed`) when the asset opens and has zero video
  tracks. Any other open failure is unchanged.
- `MediaClipDecoder` on `NoVideoTrack`: no warning, no `[media-decoder] NO DECODER` line; the asset
  becomes audio-only for this identity. Its picture is ONE transparent 2x2 BGRA frame (all bytes 0),
  one `frameId`, delivered as the cue poster and while playing (P1). `mediaHasVideo()` is false.
- Audio-only duration comes from the audio reader (`duration100ns()`, the AVF reader loads the
  asset's duration). `mediaDurationMs()` / `playbackPositionMs()` use it.
- Audio-only `mediaEnded()` = the audio reader reached its end (non-loop). A loop never ends; its
  audio pre-open uses the audio duration.
- `MediaTransports` worker: when the decoder says `mediaHasVideo() == false`, a delivered audio
  window counts as progress for the 6 s backstop (`lastNewFrameMs = nowMs`). Nothing else changes;
  #1214's drain rule ends the bed after the last window.

**Tests (red first).**
- `MediaClipDecoder.AnAudioOnlyAssetIsATransparentPictureWithNoWarning` (scripted reader returns
  `NoVideoTrack`): one frame, 2x2, every byte 0; `warnings()` empty; same `frameId` on repeat polls;
  `mediaHasVideo()` false.
- `MediaClipDecoder.AnAudioOnlyClipEndsWhenItsAudioEndsAndALoopNeverDoes`: scripted audio of exactly
  10 windows then EOS → `mediaEnded()` false before the last window is taken, true after; loop variant
  false throughout; `mediaDurationMs()` == the scripted audio duration (fixture constant 200 ms).
- `MediaTransports.AnAudioOnlyBedOutlivesTheNoNewFrameBackstop` (fake decoder, `mediaHasVideo` false,
  one frame, audio for 8 s then ended): state is `Live` at 7 s (past the 6 s backstop) and `Ended` by
  9 s. Fixture invariant: the fake must keep producing audio windows for the full 8 s.
- `AvfMediaDecoder.AnAudioOnlyFileDecodesItsToneAndHasNoVideoWarning` (`.m4a` and `.wav` fixtures,
  1.0 s of 440 Hz): transparent frame, no warning, duration 1000 ± 25 ms, PCM present.
- `AvfMediaPixel.AnAudioBedOnProgramDrawsNothingOverTheSceneAndEnds`: a GREEN still scene background
  plus a full-canvas audio-bed route; Program centre stays GREEN (never a slate) for the whole run;
  `mediaSources[]` row reaches `ended` within duration + 1.5 s; the decoder's warnings (scene
  validation warnings) never contain "could not be opened".

**Mutations (each must go red):** treat `NoVideoTrack` as `Failed` (decoder tests + pixel test red);
make the picture opaque black (transparent-bytes assertion and the GREEN pixel test red); drop the
backstop line (`AnAudioOnlyBedOutlivesTheNoNewFrameBackstop` red); report `mediaEnded()` at the first
audio EOS sighting before the window is taken (end test red).

## Task 2 (N3b): a rotated clip displays upright

**Files:** `AVFoundationMediaDecoderAdapter.mm`; fixtures gain a transform; tests in
`AvfMediaDecoderTest.cpp`, `AvfMediaPixelTest.cpp`.

**Behaviour.** When the video track's `preferredTransform` is not identity, the reader uses
`AVAssetReaderVideoCompositionOutput` with a composition built from the asset's properties (async
`videoCompositionWithPropertiesOfAsset:completionHandler:`), so frames arrive at display size and
orientation; pts/durations come from the composition output. Identity → the Phase 1 track output,
unchanged. Same pixel format and colour properties in both.

**Fixture invariant.** `writeQuadrantClip(path, w=64, h=36, transform)`: storage 64x36, quadrants
TL RED, TR GREEN, BL BLUE, BR WHITE (storage orientation), `AVAssetWriterInput.transform` set. A 90°
clockwise display rotation maps storage TL to display TR, TR→BR, BR→BL, BL→TL, display size 36x64.

**Tests (red first).**
- `AvfMediaDecoder.APortraitPhoneClipDecodesUprightAtItsDisplaySize` (90°): frame 36x64; quadrant
  centres equal the mapped colours above (±16). Also 180° (64x36, TL WHITE) and identity (unchanged).
- `AvfMediaPixel.APortraitClipIsPillarboxedUprightOnProgram`: 90° clip routed full canvas (fit
  contain) on 1920x1080 Program: the frame's display aspect makes left/right edges letterbox colour,
  and the picture's upper-right quarter (display TR) is RED (storage TL).

**Mutations:** force the track output for every clip (both red); swap width/height without rotating
(quadrant asserts red).

## Task 3 (N3c): an alpha clip keys over the scene on Program

**Files:** tests only (`AvfMediaPixelTest.cpp`), unless the test exposes a defect.

**Test.** `AvfMediaPixel.AnAlphaStingKeysOverTheSceneAsAStraightAlphaBlend`: GREEN still background
(BGR 0x20,0xE0,0x20), ProRes 4444 clip BGRA(0x20,0x20,0xE0, a=0x80) routed full canvas. Expected
Program centre = straight blend `src*a + dst*(1-a)` = BGR(0x20, 0x7F, 0x80) ±12, computed in the test
from the two fixture constants. Run on both the BGRA and the zero-copy factory once Task 6 lands.

**Mutations:** force decoded alpha to 0xFF (centre reads RED → red); premultiply in the decoder
(centre R ≈ 0x50 → red).

## Task 4 (N4a): shared gates accept a GPU-only media frame

**Files:** `native/src/modules/GpuVideoFrame.h` (backend `AppleIOSurfaceBgra`),
`native/src/modules/Interfaces.h` (`CompositorSourceTexStats::zeroCopyBinds`),
`native/src/modules/MediaVideoPresentation.h` (four gates → `hasContent()`),
`native/src/core/MediaTransports.h` (`selectVideo` → `hasContent()`),
`native/src/core/MediaAssetSource.h` (descriptor size falls back to `width`/`height` / `gpuPixels`),
`native/src/modules/MediaClipDecoder.{h,cpp}` (`ClipVideoSample::gpu`, deliver gates → `hasContent()`).

**Tests (red first; portable, so they run in the stub build too).** A test-only
`FakeGpuFrame : GpuVideoFrame` (width/height set, no CPU payload):
- `MediaVideoPresentation.AGpuOnlyFrameIsQueuedHeldAndSelected`.
- `MediaTransports.AGpuOnlyDecoderFrameReachesTheBusAndHolds` (fake decoder emitting GPU-only frames:
  `MediaAssetSource::poll` returns it, descriptor size equals the fake's 1920x1080, a pause holds the
  same `frameId`).
- `MediaClipDecoder.AGpuSampleBecomesAGpuFrameWithNoCpuPixels` (scripted reader hands a sample with
  `gpu` set and no `bgra`): frame `hasGpuPixels()`, `!hasPixels()`, cue/pause/loop rows unchanged.

**Mutations:** revert each of the five gates individually; at least one test reds per gate (report
which).

## Task 5 (N4b): Metal samples an IOSurface frame without an upload, and never a recycled surface

**Files:** NEW `native/src/compositor/MetalPixelBufferTextures.{h,mm}`; NEW
`native/src/modules/PixelBufferVideoFrame.h` (Apple: retains a `CVPixelBufferRef`); small hooks in
`native/src/compositor/MetalCompositorAdapter.mm` (`frameHasContent`, `acquireSourceTex` branch, one
`beginPass(commandBuffer)` call in `renderPassInto`, stats); `native/CMakeLists.txt`; tests NEW
`native/tests/MetalZeroCopyTest.cpp`.

**Interfaces.**

```cpp
// PixelBufferVideoFrame.h (Apple only)
struct PixelBufferVideoFrame final : GpuVideoFrame {      // backend = AppleIOSurfaceBgra
  explicit PixelBufferVideoFrame(CVPixelBufferRef);        // CFRetain; dtor CFRelease
  CVPixelBufferRef pixelBuffer() const;
};
std::shared_ptr<const GpuVideoFrame> makePixelBufferVideoFrame(CVPixelBufferRef);  // null unless IOSurface-backed 32BGRA
// MetalPixelBufferTextures.h
class MetalPixelBufferTextures {
 public:
  explicit MetalPixelBufferTextures(id<MTLDevice>);
  static bool canSample(const VideoFrame&);           // AppleIOSurfaceBgra, no CPU payload
  id<MTLTexture> textureFor(const VideoFrame&);        // CVMetalTextureCache; leases the frame to the current pass
  void beginPass(id<MTLCommandBuffer>);                // new lease bag; completion handler releases it
  // test seam (P7)
  static void armPassGateForTest(id<MTLSharedEvent>, uint64_t value);
  static void disarmPassGateForTest();
};
```

**Lifetime rule (the point of the task).** Every frame sampled by a pass is held (the
`GpuVideoFrame` shared_ptr AND its `CVMetalTextureRef`) by that pass's lease bag until the command
buffer's completed handler runs. The per-source cache holding "the current frame" is NOT sufficient:
it is replaced on the render thread when the next frame arrives, while up to three earlier passes are
still in flight (`inflight_` semaphore of 3).

**Tests (red first).**
- `MetalZeroCopy.AnIOSurfaceFrameRendersTheSamePixelsAsItsBgraTwin`: one 320x180 IOSurface BGRA
  buffer filled with a gradient; render it as a CPU BGRA frame and as a pixel-buffer frame; preview
  readbacks equal within 1 per channel at 9 sample points; `cachedUploads` and `scratchUploads` do not
  move on the zero-copy render and `zeroCopyBinds` advances by 1.
- `MetalZeroCopy.ASampledSurfaceIsNeverRecycledBeforeItsPassCompletes`: a `CVPixelBufferPool` with
  allocation threshold 2. Arm the pass gate. Frame A (RED) rendered with `skipCpuReadback` (no wait),
  then frame B (BLUE, a new buffer) replaces it in the source cache; the test drops every reference
  to A and B. While the GPU is gated: asking the pool for a buffer must NOT return A's or B's
  IOSurface (`IOSurfaceGetID`), and any buffer it does return is filled GREEN. Release the gate,
  wait for completion; THEN the pool may hand back A's surface. A full-readback render of A's source
  key after release shows BLUE (the last frame), never GREEN.
- `MetalZeroCopy.AHeldIOSurfaceFrameIsBoundOnceAndReused`: same frame rendered three times →
  `zeroCopyBinds` counts texture creations once per new frame (frameId/buffer), not per draw.

**Mutations:** release the lease at the end of `renderPassInto` instead of in the completed handler
(recycle test red); do not lease at all (red); route IOSurface frames through `uploadRegion` via a
CPU lock (zero-upload assertion red).

## Task 6 (N4c): the decoder hands Metal the decoder's own buffer

**Files:** `AVFoundationMediaDecoderAdapter.mm` (delivery mode), `Interfaces.h` (one declaration
`createAvFoundationZeroCopyMediaDecoderFactory()`), `ModuleComposition.cpp` (pick it when the
compositor is Metal, P5), `StubModules.cpp` if a null twin is needed, tests in
`AvfMediaDecoderTest.cpp`, `AvfMediaPixelTest.cpp`.

**Behaviour.** In zero-copy mode the reader's output settings add `kCVPixelBufferIOSurfacePropertiesKey`
and `kCVPixelBufferMetalCompatibilityKey`; each decoded sample is wrapped as a `PixelBufferVideoFrame`
(retain, no copy, no alpha pass). The decoder counts CPU copies in an atomic the test reads
(`avfMediaDecoderCpuCopiesForTest()`). BGRA mode is unchanged.

**Tests (red first).**
- `AvfMediaDecoder.ZeroCopyFramesCarryTheDecodersSurfaceAndCopyNothing`: 30-frame clip, roll it:
  every frame `hasGpuPixels() && !hasPixels()`, copy counter delta 0, IOSurface ids present.
- `AvfMediaDecoder.AHeldZeroCopyFrameIsNeverOverwrittenByLaterDecodes`: hold references to frames 2,
  3 and 4 of a clip whose frame i has a known per-frame colour (segments of 1 frame each), decode 40
  more, then lock each held buffer read-only and check its colour is still frame i's. Fixture
  invariant: 45 one-frame segments with distinct colours.
- `AvfMediaPixel.*` (roll, pause-hold, cue→take, loop, end, alpha, portrait): each runs with the
  zero-copy factory as a second case (`…ZeroCopy`), with identical pixel expectations, plus
  `AvfMediaPixel.ZeroCopyProgramMatchesTheBgraPathPixelForPixel` (same clip, same tick count, both
  factories, preview readbacks equal within 2 at 9 points) and the compositor's `cachedUploads` not
  moving for the media source.
- `ModuleComposition` capability test: with Metal present the default modules use the zero-copy
  factory (`media-clip-playback: available`, detail `zero-copy`).

**Mutations:** wrap without retaining (held-frame test red or crash); copy into BGRA then wrap (copy
counter red); pick zero-copy for a non-Metal compositor (composition test red).

## Task 7: drill and outcomes

- `python3 scripts/mac-show-drill.py --seconds 40 --load 8` (baseline) then `--load 8 --media` on
  the Phase 1 decoder (before) and on this branch (after). Report per-frame delivery, render hold,
  worst frame, and the `[media-decoder] copy avg` line (before only). Mark UNVERIFIED under load.
- Full native suite (whole binary), stub build + ctest, run twice for load-sensitive cases.
- `docs/superpowers/plans/2026-11-21-mac-media-phase2-outcomes.md`.

## Out of scope

N5 (FFmpeg/WebM); shell work (S1-S3 already shipped); an NV12 path; HDR tone mapping; rotating an
alpha clip with its alpha (P3); Windows rotation/audio-only fixes (follow-ups named in outcomes).
