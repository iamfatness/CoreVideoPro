# Media clip playback on macOS: an AVFoundation decoder and the media operator surface

Status: DRAFT design for owner review, 2026-11-15.
Covers gap row **2.1** of `docs/superpowers/audits/2026-11-15-mac-ui-parity-consolidated.md`
(owner priority #1) and Media rows **M1-M14** of
`docs/superpowers/audits/2026-11-15-mac-ui-parity-routing-overlays-media.md`. It is not a numbered
milestone in `docs/superpowers/plans/2026-11-14-mac-parity-1.0-roadmap.md`; it sits beside M3 on
lane E and feeds roadmap M2.4 (Volt rundown media staging).

Path shorthand: `N/` = `native/src/`, `W/` = `native-shell/CoreVideoPro.WinUI/`,
`M/` = `mac-shell/Sources/CoreVideoProShell/`.

## 1. Why

An operator on a Mac cannot play a video or audio clip. Stingers, opens, bumpers, roll-ins, audio
beds and looping video backgrounds are all unavailable. The Media tab imports files and lists
them, and the only thing a file can then do is become the logo bug or a still overlay. A show that
opens with a 10-second sting on Windows cannot be run on a Mac at all.

The cause is one missing platform module. Since #535 slice 3b the shared core owns everything
about a clip: one source per asset, its decoder, its clock, its transport state and its audio.
That code is platform-neutral and already compiles on macOS. It is never constructed there because
the only decoder factory is Media Foundation, and the Mac build gets an empty factory. The rest of
the gap is shell work: the Mac shell never learned to cue, play, pause, select or remove media.

## 2. Windows behaviour to match

### 2.1 Core contract (shared code; the Mac decoder plugs in here)

- **One decoder per source id.** `core::MediaTransports` owns one `Entry` per `media:<assetId>`
  (route) or `background:<assetId>` (scene background): decoder, worker thread, a 3-deep
  presentation queue, an audio queue and the transport state (`N/core/MediaTransports.h:41-93`).
  The cap is 16 decoders with a loud refusal (`:551, :571-583`).
- **Desired set at command time.** `MediaCore::syncMediaTransportsDesired()` builds the set from
  both scene graphs and never from a render tick (`N/core/MediaCore.cpp:2337-2403`). Stills are
  excluded and served by `StillMediaFrameCache` (`:2355`). A scene background always loops
  (`:2371`).
- **Transitions** (`N/core/MediaTransportPolicy.h`, tests `native/tests/MediaTransportPolicyTest.cpp`):
  appears on Program, or is a loop: roll from 0 with audio. Appears on Preview only: poster at
  frame 0, silent (`cued`). A cued clip entering Program resumes the same decoder. A clip that
  stays on Program across a Take does nothing. Leaving Program while cued restarts at 0 behind the
  held frame. Absent from both buses: release after a 750 ms grace (`MediaTransports.h:567`).
- **Pause is a clock state.** `MediaPlaybackTimeline` freezes elapsed time on pause and resumes
  from the same value (`N/modules/MediaPlaybackTimeline.h:22-44`). A paused clip holds its on-air
  frame with the same `frameId`. Audio emits nothing while paused and resumes at the clock
  position; `MediaAudioWindows` keeps 200 ms of history so the resume replays real samples
  (`:74-118`). Tests: `MediaPlaybackTimeline.PauseFreezesElapsedAndResumeContinues`,
  `MediaTransports.PauseAndResumeKeepOneDecoder`, `PauseHoldsTheOnAirFrame`,
  `NoAudioWhilePausedAndAudioResumes` (`native/tests/MediaPlaybackTimelineTest.cpp`).
- **Ended is decoder evidence and is recoverable.** The owner reads
  `IMediaVideoPrefetch::mediaEnded()`; a loop never reports it; a 6 s no-new-frame window is only
  a backstop (`N/modules/MediaVideoPresentation.h:30-39`, `MediaTransports.h:553-564`).
- **Cold cut hold.** A clip taken to Program without a cue keeps the outgoing picture until its
  first frame (`takeTransition_.waitingForColdMedia`, `MediaCore.cpp:2302, 2376-2381`). Oracle:
  `scripts/qa/media-take-ab.py` and its `--skip-cue` run.
- **The decoder contract.** `IMediaDecoder` (`N/modules/Interfaces.h:1620-1635`):
  `pollMediaFrames`, `pollMediaFramesAt100ns`, `pollMediaAudioFrames`, `warnings`, driven by a
  `MediaDecodeRequest {kind, sourceId, assetId, assetKind, assetPath, playing, loop}`
  (`:1610-1618`). The optional `IMediaVideoPrefetch` adds `setMediaWakeCallback`,
  `prefetchMediaVideo` (frames with a due time), `syncMediaClock`, `playbackPositionMs`,
  `mediaDurationMs`, `mediaEnded` (`MediaVideoPresentation.h:13-40`). The worker calls the decoder
  from its own thread, outside the entry lock (`MediaTransports.h:606-760`).
- **Frame format the owner accepts today: BGRA only.** `MediaVideoPresentation::push` drops a
  frame without `hasPixels()` (`MediaVideoPresentation.h:47`); `hold()` and `hasFrame()` test
  `hasPixels()` (`:60, :82`); `MediaTransports::selectVideo` returns nothing without it
  (`MediaTransports.h:393`). The Media Foundation adapter negotiates RGB32 and its FFmpeg path
  asks for `-pix_fmt bgra` (`N/modules/MediaFoundationMediaFrameSourceAdapter.cpp:371, 1018`).
  The Media Foundation path then forces alpha opaque, because RGB32 is BGRX and the compositor
  uses source alpha (`:1194-1200`).
- **Audio.** 48 kHz stereo float, 960-frame windows (`…Adapter.cpp:972-986`). PCM is labelled
  `media:<assetId>` and crosses the bus through `core::ingestSourceAudio`.
- **FADER LAW alias.** Every `media:*` clip without its own strip is pre-summed into one routed
  source keyed `"media"` (`N/core/AudioControlSourcePolicy.h:12-34`). Test:
  `MediaCoreCommand.SceneMediaAudioReachesMasterThroughTheShellMediaStrip`; live proof
  `node scripts/validate-record-audio.mjs --media`.
- **Fallback decoder.** When Media Foundation cannot decode (ProRes), an FFmpeg child produces
  BGRA; it is stopped on pause and restarted at the clock position on a bounded ladder
  (`…Adapter.cpp:230-480, 829-880`). Failures are loud `[media-decoder]` log lines
  (`docs/reference/media-no-decoder.md`).
- **Wire.** Routes carry `mediaAssetId/Name/Kind/Path` and `mediaAssetLoop`
  (`native-shell/CoreVideoPro.MediaCore/Services/MediaCoreCommandBuilder.cs:263-292`); a
  background carries the four asset fields and no `playing` (`:246-261`). The one-shot gesture is
  `set-media-transport {mediaAssetId, action: "pause"|"play"}` (`MediaCore.cpp:3562-3585`).
  `set-media-playback` is selection only (`:3523-3552`). The snapshot publishes `mediaSources[]`
  unconditionally with `sourceId, mediaAssetId, state, loop, onProgram, onPreview, positionMs,
  durationMs` plus the #701 delivery trace (`N/core/CoreStateCapture.cpp:1196-1221`), and
  `mediaPlayback.status` over `idle|cued|live|paused|ended|unavailable`.

### 2.2 Shell behaviour (source of truth for the Mac port)

| Behaviour | Windows | Test |
|---|---|---|
| Bin: 11 extensions, kinds by extension, id = `media-` + sha256(relative path)[..12] | `W/Services/MediaBinService.cs:22-23`; `native-shell/CoreVideoPro.MediaCore/Services/MediaBinClassifier.cs:26-39, 77-107, 121-126` | `MediaBinServiceTests` (both projects) |
| Unknown extension is refused at import | `MediaBinClassifier.cs:106` (`null`), `MediaBinService.cs:54-58` | same |
| Import, multi-select, status message | `W/ViewModels/StudioViewModel.cs:5946-5982` | none found |
| Select a row | `StudioViewModel.cs:6047-6074` | none found |
| Remove to `.trash`, refused while a scene or input uses it | `StudioViewModel.cs:6083-6125`; `MediaBinService.cs:84-100` | none found (UNVERIFIED) |
| Cue in Preview; refuses a duplicate; "Take to start playback" text | `StudioViewModel.cs:14556-14586` | none found |
| A clip is `SupportsPlayback`; a still is not | `W/Models/ProductionModels.cs:894-903` | `AStillGoingLiveIsNeverPromoted` |
| Tap: live Program clip pauses, paused or ended resumes, anything else selects, a loop never pauses | `W/Services/MediaRoutePlaybackService.cs:165-181` | `ResolveTap_*` |
| The route row beats the background row for one asset | `MediaRoutePlaybackService.cs:216-246` | `ResolveTransportState_TheRouteRowWins…`, `ResolveTap_ALiveBackgroundRowCannot…` |
| Take promotes only assets that entered Program | `MediaRoutePlaybackService.cs:81-137` | `AssetsEnteringProgram_*`, `ChooseAssetToPromote_*` |
| Snapshot apply adopts the core flag only while the row is `OnProgram` | `W/Services/MediaBinPlaybackProjection.cs` | `MediaBinPlaybackProjectionTests` |
| Gesture is latest-wins per asset, bounded retry, visible give-up | `W/Services/MediaTransportGestureLedger.cs`; `StudioViewModel.cs:6396-6490` | `MediaTransportGestureLedgerTests` |
| Rolled-back Take restores the media selection | `W/ViewModels/Transport/TakeMediaSelectionRollback.cs` | `TransportCoordinatorTests.Take_Rollback*` |
| Button label: Pause Program / Resume Program / Pause audition / Audition | `StudioViewModel.cs:5901-5904` | none found |
| "media" sends seeded only while Program carries a fixed media route | `StudioViewModel.cs:9213-9215, 9553-9561, 9702` | none found |
| Local preview player mirrors play state | `W/Controls/MediaAssetPreviewHost.xaml.cs` | none |

All of this logic lives in the WinUI project, so none of it is callable from Swift. Its tests are
the acceptance oracle for the Swift port.

## 3. Mac today

**Shared core, already working on macOS**

- `MediaTransports`, `MediaTransportPolicy`, `MediaPlaybackTimeline`, `MediaAudioWindows`,
  `MediaVideoPresentation`, `MediaAssetSource`, `set-media-transport`, `mediaSources[]`, the
  `"media"` audio alias and the cold-cut hold are all platform-neutral.
- Stills work. `stillMediaCache_` is default-constructed (`N/core/MediaCore.h:1107-1108`) and the
  Apple branch decodes with ImageIO (`N/modules/StillMediaFrameCache.cpp:149-200`). The
  `cpuSourcePreparation` re-construction at `MediaCore.cpp:473-476` is D3D11-only and does not
  remove the default cache.
- The Metal compositor draws a media frame like any other: BGRA or I420 through
  `acquireSourceTex`, one CPU upload per new `frameId` (`N/compositor/MetalCompositorAdapter.mm:1333-1380`).

**Windows-only, the actual gap**

- `createMediaFoundationMediaDecoderFactory()` returns an empty function off Windows
  (`…Adapter.cpp:9, 1308-1316`). `ModuleComposition.cpp:440-442` therefore leaves
  `mediaDecoderFactory` empty, `mediaTransports_` stays null (`MediaCore.cpp:469-471`),
  `set-media-transport` answers "no media transports are running" (`:3574-3578`) and
  `mediaSources[]` is always empty. No Mac twin exists. `native/CMakeLists.txt:76-80` has no
  decoder option.
- The Metal adapter has no GPU-frame input. `GpuVideoFrame::Backend` is `D3D11Bgra | D3D11I420`
  only (`N/modules/GpuVideoFrame.h:9`) and `MetalCompositorAdapter.mm` never reads `gpuPixels`
  (grep = 0). Every source is a CPU upload on the render thread.

**Mac shell**

- Bin: `M/MediaBin.swift` matches the Windows id and kind rules (`:36-54`), with two
  differences: an unknown extension is classed `lower-third` and imported (`:46`), and the panel
  has no type filter (`M/ShellApp.swift:1872-1879`).
- Media tab: Import, Refresh, a flat list, Bug/Unbug (`ShellApp.swift:1864-1914`). No Cue, Play,
  Pause, Select, Remove, duration, status or preview. `selectedAssetId` is written on import and
  read nowhere.
- Scenes: a canvas layer can carry a still (`M/AppModel.swift:2580-2596`; the picker is stills
  only, `M/StudioSceneEditing.swift:140`). Routes send the retired `mediaPlaybackKey` and
  `mediaAssetPlaying` fields and do not send `mediaAssetLoop` (`AppModel.swift:2591-2593`).
- Scene background: the wire is correct (`AppModel.swift:3224-3236`), but the picker filters on
  kinds `image`/`background`, which the classifier never produces (`ShellApp.swift:1037`), so it
  is always empty.
- Audio: the `"media"` strip, its default sends and the Routing row exist
  (`M/AudioConsolePolicy.swift:21-24, 40, 129-150`; `AppModel.swift:3392, 3973-3979`). Tier 0 row
  0.2 owns finishing that work.
- Snapshot: `applySnapshot` reads no `mediaSources` or `mediaPlayback` (`AppModel.swift:1592-1720`).
- Take already leads its batch with `begin-take-transition` (`AppModel.swift:3004-3040, 3246-3248`).

## 4. Design

### 4.1 Native: `AvFoundationMediaDecoder`

New file `native/src/modules/AVFoundationMediaDecoderAdapter.mm`, new CMake option
`COREVIDEO_WITH_AVF_MEDIA_DECODER` (Apple only, requires `COREVIDEO_ENABLE_DEV_ADAPTERS`, links
AVFoundation, CoreMedia, CoreVideo, VideoToolbox), new factory
`createAvFoundationMediaDecoderFactory()` declared beside the Media Foundation one in
`Interfaces.h` and returning an empty function when the option is off. `ModuleComposition.cpp:440`
tries Media Foundation first, then AVFoundation, and records a new capability
`media-clip-playback` (`available | omitted | failed-to-construct`) through the existing
`recordCapability`. The option is switched on wherever the Mac build sets
`COREVIDEO_WITH_AVF_ENCODER` (`scripts/run-mac-shell.sh`, `scripts/release-macos.sh`,
`.github/workflows/ci.yml`).

The class implements `IMediaDecoder` and `IMediaVideoPrefetch`. `MediaTransports` hands one
decoder one clip, so it holds one asset state.

**Behaviour, row for row with the Media Foundation adapter**

| Case | Required behaviour | Windows reference |
|---|---|---|
| Identity | path + loop flag; `playing` is never part of it; a new identity is a new reader at 0 | `…Adapter.cpp:733-747` |
| Never played, not playing (cue) | decode exactly one frame, hold it | `:800-819` |
| Playing | frames carry due time = clock epoch + loop offset + media PTS (100 ns) | `:644-652` |
| Paused after rolling | read nothing, hold the presented frame, same `frameId` | `:790-799` |
| Resume | continue from the next sample; no seek, no reopen | `:820-824` |
| End of a clip | hold the last frame, `mediaEnded()` true | `:902-910, 626-630` |
| End of a loop | new reader behind the last good frame, loop offset advances, `mediaEnded()` false | `:885-901` |
| Position | elapsed clock time, wrapped for a loop, clamped for a clip, -1 when unknown | `:599-616` |
| Duration | from the container, -1 when unknown | `:617-621` |
| Still asset on a `media-video` request | return nothing (the cache serves it) | `:660-663` |
| Background still | decode once and hold (ImageIO on Mac) | `:776-789` |
| Frame ids | strictly rising for the life of the decoder, across loops | `:699-701` |
| Audio | none while not playing; 960-frame 48 kHz stereo float windows cut by `MediaAudioWindows` at the clock position | `:938-988` |
| Open failure | a warning with the same wording, and a `[media-decoder]` log line naming the cause | `docs/reference/media-no-decoder.md` |

**Reading.** Video and audio use two separate `AVAssetReader` instances, as Media Foundation uses
two source readers. One reader with two outputs stalls when either output is not drained, and the
owner drains video and audio at different rates.

**Threading.** `copyNextSampleBuffer` blocks while VideoToolbox decodes. The owner's worker also
times audio, so the decoder must never block it. Each reader runs on its own serial dispatch queue
inside the decoder and keeps a small bounded decoded-ahead store (video: one sample beyond the
owner's request; audio: enough for the next window). Poll calls copy out what is ready and return.
The reader queue fires the wake callback when a sample lands, with no decoder lock held, matching
`MediaReadCallback` (`…Adapter.cpp:80-140`). Opening the asset and loading tracks happens on the
reader queue. Nothing here runs on the render tick, the audio worker or the command thread, and
nothing takes `coreMutex`. Teardown cancels both readers and drains both queues before the
destructor returns, because `MediaTransports` destroys a decoder on its worker thread and a late
callback must not touch a retired object.

**Pixel format, stage 1 (slice N1): BGRA in `VideoFrame::pixels`.** The reader output asks for
`kCVPixelFormatType_32BGRA` with BT.709 output colour properties, and the decoder copies rows into
a tightly packed buffer (`pixelStride = width * 4`). This is the format the shared owner already
accepts, so slice N1 changes no shared code and cannot regress Windows. Decode is hardware
(VideoToolbox) for H.264, HEVC and ProRes on Apple Silicon. The cost is two 8.3 MB copies per
1080p frame: one on the reader queue, and one `replaceRegion` on the render thread. That is the
same shape as every other Mac source today, but a BGRA clip is 2.7 times the bytes of an I420 Zoom
feed, so N1 must measure it (section 6) rather than assume it fits.

**Pixel format, stage 2 (slice N4): zero-copy IOSurface.** The reader is asked for
IOSurface-backed, Metal-compatible buffers. The frame carries the retained buffer through a new
`GpuVideoFrame` backend (proposed names `MetalIOSurfaceBgra`, and `MetalIOSurfaceNv12` if the
measurement justifies a second shader path). `MetalCompositorAdapter::acquireSourceTex` gains a
branch that wraps the IOSurface as a texture with `newTextureWithDescriptor:iosurface:plane:`, the
call it already uses for its render targets (`MetalCompositorAdapter.mm:564`), so the render
thread uploads nothing. This needs three shared changes, each with Windows tests: the four
`hasPixels()` gates named in 2.1 become `hasContent()`; `SourceContinuityLedger` and the bus health
checks must count a GPU-only frame as content; and every CPU reader of a media frame on Mac has to
be found and given a path (the 320x180 program preview is drawn by the compositor, but ISO capture
of a media source, thumbnails and any software fallback are UNVERIFIED). Stage 2 is planned work,
not an optional extra: the north star puts source pixels on the GPU. It is sequenced after the
operator can play a clip, and its scope is set by N1's measurement.

**Alpha.** A ProRes 4444 or HEVC-with-alpha sting must key over the scene. The house contract for
`pixels` is straight alpha (`StillMediaFrameCache.cpp:150-153`), and the compositor blends a media
frame with its source alpha. On Windows a clip decoded by Media Foundation is always opaque
(`…Adapter.cpp:1194-1200`); only the FFmpeg path asks for real BGRA, and whether its alpha reaches
Program is UNVERIFIED. The Mac decoder therefore writes opaque alpha for every codec without an
alpha channel and passes real alpha for ProRes 4444 and HEVC-with-alpha. Whether VideoToolbox
returns that alpha straight or premultiplied is UNVERIFIED and is settled by a pixel test in N1.

**Containers AVFoundation cannot open.** `.webm` is in the supported list and AVFoundation does
not read it. ProRes, the reason Windows has a fallback, decodes natively on a Mac. Stage 1 reports
an unsupported file as a loud open failure. Slice N5 ports the FFmpeg child for those files only,
finding the binary with `modules/PosixExecutableLocator.h` as the RTMP sender does, with the same
pause, resume-at-clock-position and stderr-tail rules. See owner decision 1.

### 4.2 Shell: Swift ports of the Windows decisions

New pure types in `M/`, each a direct port with the Windows test names carried over:

- `MediaSourceRow` and its decode from `snapshot["mediaSources"]`, validated against
  `contracts/observation.schema.json`, which already describes the node.
- `MediaRoutePlayback`: `assetsEnteringProgram`, `chooseAssetToPromote`, `selectedAssetLeftProgram`,
  `resolveTap`, `resolveTransportRow`, `isPlayingOnAir`.
- `MediaBinPlaybackProjection`: the whole snapshot-apply decision as one function.
- `MediaTransportGestureLedger`: latest-wins claim token checked before every send.
- `TakeMediaSelectionRollback`.

`AppModel` changes:

- Read `mediaSources[]` in `applySnapshot` and publish rows. Row play state changes are observable
  scalars; the bin list is never rebuilt at snapshot rate.
- Route wire: send `mediaAssetLoop`; stop sending `mediaPlaybackKey` and `mediaAssetPlaying`.
  Background wire: stop sending `playing`.
- New commands: `set-media-transport` (one-shot, through the ledger, never re-sent per sync or on
  a core respawn) and `set-media-playback` (selection, so `mediaPlayback.status` is truthful for
  `/snapshot` readers).
- `cueSelectedInPreview()`: adds the asset as a full-canvas fixed media layer on the Preview
  scene, refuses a duplicate, and sets the Windows status text.
- Take: after a confirmed Take, move the selection to the asset that entered Program; on a
  rollback, restore it with `TakeMediaSelectionRollback`.
- Audio: `sceneHasMediaRoute` must be true for clips as well as stills (it already keys on the
  route wire), and the "media" sends are seeded only while Program carries a media route.

`MediaBin` changes: the 11-extension filter on the open panel and in `classify` (unknown returns
nil), duration and dimensions read with AVFoundation off the main thread, grouping by kind,
`remove(_:)` to `<root>/.trash` with a unique name, and an in-use check over scene layers, scene
backgrounds and the logo bug.

Media tab layout follows `W/Views/MediaPage.xaml`: a selected-asset card with Cue in Preview, the
Play/Pause button with the four Windows labels, playout status and a local preview (`AVPlayerView`
for clips, `NSImage` for stills, muted audition that mirrors play state); then the bin with a
summary line, kind headers, and per-row Select, Play and Remove. Bug/Unbug and the bin path stay
(Mac-only, worth keeping). The scene background picker lists stills and videos.

**Persistence.** Nothing new in the core. The bin is the folder. Scene layers and backgrounds
already persist asset ids (`AppModel.swift:636-643, 708-723`). The selection is not persisted,
matching Windows. Transport state is never persisted: after a core respawn the scene reloads and a
Program clip rolls from 0, as on Windows.

**Degraded states the operator sees**

| Condition | What the operator sees |
|---|---|
| Decoder option absent from the build | Play and Cue for clips disabled with "Clip playback is unavailable in this build"; Health shows capability `media-clip-playback: omitted` |
| File cannot be opened or decoded | Row and status show the core warning text; the layer shows the bus warming or failed slate; `[media-decoder]` line in the core log |
| Unsupported container | Same, with "needs FFmpeg" when the fallback is the missing piece |
| 17th simultaneous decoder | The core's capacity warning, by source name |
| Gesture refused (loop, or not on Program) | Button reverts; the scene-validation warning is shown |
| Gesture not confirmed after the bounded retry | "Pause not confirmed by the media core" |
| Asset removed from disk while routed | Route sends nothing (existing rule, `AppModel.swift:2584-2586`); row shows "missing" |
| Clip ended on Program | Status "ended"; the button reads Resume Program |

## 5. Slices

| Id | What lands | Lane | Size | Depends on | Proven by |
|---|---|---|---|---|---|
| **N1** | `AvFoundationMediaDecoder` video path (BGRA): cue poster, roll, pause, resume, loop, end, position, duration, warnings; factory, CMake option, capability. Consumer in the same PR: the scene background picker lists videos, so a looping video background plays on a Mac. | native + small shell | L | none | New `AvfMediaDecoderTest.cpp`; the `MediaCoreCommandTest.cpp` cases now gated on `COREVIDEO_WITH_MF_ENCODER` (`:1981, 2610, 4077, 5180-5244`) re-gated to run on either decoder; Metal readback of a background clip; `media-take-ab.py` and its `--skip-cue` run on the Mac core; drill with a clip (6.3) |
| **N2** | Clip audio: second reader, 48 kHz stereo float windows, silence while paused, resume at clock position, loop continuity. Consumer: the existing Mac "media" strip. | native | M | N1; Tier 0 row 0.2 | `validate-record-audio.mjs --media` made portable and run on Mac; unit tests on window continuity across pause and loop |
| **S1** | `mediaSources[]` decode, Cue in Preview, selected-asset card, playout status, `mediaAssetLoop` on the wire, legacy fields removed, `set-media-playback`. Stills become cueable at once; clips with N1. | shell | M | none for stills; N1 for clips | Ported tests; contract check against `observation.schema.json`; tab snapshot |
| **S2** | Play/Pause: gesture ledger, `resolveTap`, bin projection, Take promotion and selection rollback, row play state. | shell | M | N1, S1 | Ported `ResolveTap_*`, projection, ledger and rollback tests, each failing on the old code first; owner check |
| **S3** | Bin parity: extension filter, import status, kind headers, duration, summary, Select, Remove to `.trash` with the in-use guard, local preview player. | shell | M | none | Unit tests on classify, remove and the guard; tab snapshot |
| **N3** | Audio-only assets (audio beds) and any Windows behaviour N1/N2 left UNVERIFIED (alpha, rotation). Characterise Windows first, then match. | native | S-M | N2 | Characterisation tests on Windows, the same tests on Mac |
| **N4** | Zero-copy IOSurface frames into Metal; shared `hasContent()` gates. | native (shared) | L | N1 | Windows suite unchanged; Metal readback equal to the BGRA path within tolerance; drill shows render hold no worse than with no clip |
| **N5** | FFmpeg fallback for containers AVFoundation cannot open. | native | M | N1, owner decision 1 | The `.webm` fixture plays; the FFmpeg pause and resume tests run on Mac, skipping loudly when no binary is found |

Order on lane E: N1, N2, N3, N4, N5. Order on lane S: S3 and S1 can start now, S2 after N1.
N1 with S1 is the first owner check ("cue a sting, take it"); N2 makes it audible.

Remote actions `media.rundown.load/preview/take`
(`native-shell/CoreVideoPro.Control/ControlActionRegistry.cs:131-136`) belong to the control spec
(roadmap M2.3/M2.4); they call `cueSelectedInPreview` and Take from S1/S2.

## 6. Acceptance and test strategy

### 6.1 Unit and contract

- **Decoder** (`native/tests/AvfMediaDecoderTest.cpp`, new, built only with the option): fixtures
  are generated at test time with `AVAssetWriter` (the suite already links AVFoundation for
  `tests/AvfMp4Decode.mm`), so no binary media is committed. One test per row of the table in 4.1.
  The pause test asserts the frame after resume is the next frame of the clip, bounded on both
  sides. An assertion that only says "the frame id did not go down" passes for a restart from 0
  and proves nothing.
- **Shared transport tests on a real decoder.** `MediaTransports.*` and
  `ProgramPixelContinuity.ACuedClipTakenToProgramNeverShowsThePlaceholder` run against the
  AVFoundation factory on Mac as they run against Media Foundation on Windows.
- **Shell.** Every Windows test named in 2.2 is ported by name into the `COREVIDEO_SHELL_TESTS`
  suite and must fail against the current Mac code first.
- **Contract.** A captured Windows `mediaSources[]` sample (new fixture) is decoded by the Swift
  type and validated against `contracts/observation.schema.json`.

### 6.2 Pixels and audio

- **Metal readback** (`native/tests/MetalCompositorTest.cpp` pattern): a clip of known flat
  colours, routed full canvas. Program pixels match the clip's first frame while cued, change
  after go-live, hold exactly while paused, and are never a slate colour.
- **Colour.** A fixture with known BT.709 limited-range bars reads back within tolerance. This
  settles the range and matrix question for the BGRA request.
- **Alpha.** A half-transparent fixture over a known background reads back as the straight-alpha
  blend.
- **`scripts/qa/media-take-ab.py`** on the Mac core: the cued run passes and the `--skip-cue` run
  passes through the cold-cut hold. Both judge the recorded Program MP4.
- **`node scripts/validate-record-audio.mjs --media`** on Mac: the recording carries the clip's
  440 Hz tone. The script hard-codes a Windows platform string and FFmpeg path
  (`scripts/validate-record-audio.mjs:59, 172, 315`); N2 makes it portable.
- **Shell.** `COREVIDEO_SHELL_TAB_SNAPSHOT` images of the Media tab in three states: empty, clip
  selected and cued, clip live.

### 6.3 Performance (per-frame delivery, not mean fps)

`python scripts/mac-show-drill.py --seconds 40 --load 8` gains a media step: one 1080p60 H.264
clip live on Program and one looping 1080p background, over the eight synthetic Zoom feeds. Gates
are the drill's existing ones: sustained 60 of 60, zero dropped, delivery, coreMutex over-budget
ratio. Reported beside them: `mediaSources[].decodedVideoFrames`, `presentedVideoFrames` and
`videoQueued` deltas, so a short clip rate is attributed to the decoder or to presentation. The
baseline is the same drill with no clip, run moments earlier on the same quiet machine. N1 passes
only with no delivery regression. If it fails, N1 still ships the decoder and N4 becomes the next
slice instead of the fourth.

### 6.4 Live owner checks

1. Import a sting; cue it; Preview shows its first frame; Take; it rolls from frame 0 with audio
   on master, in the recording and in the stream.
2. Pause on air, wait ten seconds, resume: the picture continues from the held frame, the audio
   does not jump, nothing restarts.
3. Take to another scene and back: the clip rolls from 0.
4. A looping video background sits behind guests across several Takes without restarting.
5. Mute the Media strip: the clip is silent on every bus.
6. Remove a clip that a scene uses: refused, with the scene named.
7. A ProRes 4444 sting keys over a guest.

## 7. Risks and unknowns

| Risk | Resolution |
|---|---|
| Two 8.3 MB copies per BGRA frame cost Program frames at load 8 | Measured in N1 (6.3). N4 removes both copies. |
| VideoToolbox output for alpha codecs: straight or premultiplied | Pixel test in N1; convert on the reader queue if needed. |
| Colour: range, matrix, 10-bit and HDR clips | Bars fixture in N1. HDR tone mapping is left to VideoToolbox and recorded as a known limit unless the owner asks otherwise. |
| Loop seam: a new reader at end of file leaves a held frame for the open time | Measure the gap in N1. If it is visible, open the next reader before the end; the due-time maths is unchanged. |
| Windows behaviour for audio-only assets, clip alpha and rotated phone video is UNVERIFIED in code | N3 characterises Windows first. |
| Reader callbacks outliving a retired decoder | Explicit cancel and queue drain in the destructor; a test that destroys a decoder mid-read in a loop. |
| Limit on simultaneous hardware decode sessions is unknown | A test opens 16 decoders; if VideoToolbox refuses earlier, the refusal is reported like the capacity warning. |
| Widening `hasPixels()` gates (N4) changes shared code Windows ships | Separate PR, Windows suite and the `MediaTransports.*` tests as the gate; a clean rebuild first, since `VideoFrame` is included almost everywhere. |
| Media files on a slow or removed volume | The reader queue absorbs the stall; the transport's `ended` backstop and recovery already cover a decoder that stops and restarts producing. Proven by a test that suspends the fixture's reads. |
| The re-gated `MediaCoreCommandTest` cases may assume Media Foundation details (file types, FFmpeg) | Re-gate case by case in N1; a case that cannot run on Mac says why in a skip message. |
| The audit's line numbers for the Windows adapter were stale by a few lines | Every citation here was re-read in this worktree. |

## 8. Owner decisions needed

1. **WebM and other files AVFoundation cannot read.** Options: (a) refuse them on Mac with a clear
   message; (b) play them when an FFmpeg binary is installed (Homebrew), as RTMP output already
   requires; (c) bundle FFmpeg in the signed package. Recommendation: (b) as slice N5, with (a) as
   the behaviour until then. Bundling adds licence review and notarization work for a format few
   operators use.
2. **Chapter markers card.** On Windows it is a static "No chapters" placeholder
   (`StudioViewModel.cs:1241`). Recommendation: do not port it.
3. **Colour grade readouts on the Media tab.** The Mac has a full grade editor in its own window.
   Recommendation: keep the Mac design; add nothing to the Media tab.
4. **Local audition audio.** Windows plays the selected clip in the page preview. Recommendation:
   mute it on Mac by default, because the operator's monitor bus is the honest place to hear a
   clip and an unrouted second audio path invites feedback.
5. **Zero-copy timing.** Recommendation: ship N1 and N2 first so the owner can run a show with
   clips, then N4, unless the N1 drill fails, in which case N4 moves up.

## 9. Out of scope

- Graphics templates, crawls and the Operate surface (roadmap M3/M4).
- `media.rundown.*` remote actions and the `volt-rundown` kind (roadmap M2.3/M2.4); this spec
  provides the functions they call.
- Per-source colour grade, LUTs and scopes (gap row 2.5).
- The Sources tab showing media as a source type (gap row 2.4).
- Browser overlays, SRT/RTMP/NDI ingest (gap rows 2.10, 2.11).
- Finishing the `"media"` audio strip itself (Tier 0 row 0.2, in progress on `mac-zoom-audio`).
- Brand-kit logo and "Use selected media as logo" (Overlays rows L15/L16).
- Scrubbing, in/out points, playlists and transitions between clips: Windows 1.0 has none.
- Intel Macs and macOS before 13.
