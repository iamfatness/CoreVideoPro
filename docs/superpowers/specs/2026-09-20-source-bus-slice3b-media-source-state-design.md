# Source bus slice 3b — media transport lives in the source (design)

**Date:** 2026-09-20  
**Parent:** `docs/superpowers/specs/2026-09-18-source-bus-design.md` §5 ("Slice 3b — `layers` dropped")  
**Issue:** #535 (source bus), closes the media half of #449 (clip go-live)  
**Owner rulings (2026-09-19 / 2026-09-20):** a clip restarts from 0 on the cut; a clip
rolling on Program that is also in the cued Preview scene shows **the same rolling picture**
on Preview; **the core source owns play/pause state, driven by Take**; approach = source-owned
transport, scene-driven.

## 1. Problem

Slice 3a put decoded media on the bus but left the decoder request-driven **from the render
plan every tick** (`IMediaFrameSource::pollMediaFramesAt100ns(layers, ts)`). Play state is
decided in the shell and shipped as a per-route `mediaPlaybackKey` (`media:<id>:live:<n>`) plus
a `mediaAssetPlaying` flag; the core reconstructs intent from those strings (`MediaGoLiveLedger`
generations, the `preview:media:<id>` poster namespace, `MediaCueHandoff` re-keying a warm
decoder onto the live request). It works, but it is a split brain: two owners of one clock,
three special cases that only exist to reconcile them, and a `layers` argument that blocks
retiring `IMediaFrameSource` in slice 4b.

## 2. What the operator gets

1. A clip cued in Preview shows its first frame. **Take rolls it from 0 with audio on**, no
   slate, no second decoder: the cue's decoder is the one that rolls.
2. The same clip on Preview and Program shows **one rolling picture on both**.
3. A clip that leaves Program and later re-enters **rolls from 0 again**. A Take between two
   scenes that both hold the clip **does not restart it**.
4. **Pause / Play** from the media bin act on the clip on Program only. **Loops and
   backgrounds never pause and never restart on a Take.**
5. A non-loop clip that reaches its end **holds its last frame** (unchanged).
6. Play state survives a shell restart (it lives in the core). A core respawn reloads the
   scene and rolls the clip from 0 (unchanged from today).

Deliberately unchanged: a clip cut to Program that was **never cued** still cold-starts and
shows the warming slate until its first frame (#449 step 1, "hold the outgoing picture", is
its own slice).

## 3. Core design

### 3.1 One source per media layer, owning its transport

`core::MediaAssetSource` (kind `"media"`) stops mirroring a frame the plan poll handed it and
**owns** what `OwnedMediaFrameSource::Entry` owns today: the decoder instance (from the module
factory), its worker thread, `MediaVideoPresentation`, the audio queue, warnings, and a new
transport state. Source ids are unchanged: `media:<assetId>` for a route, `background:<assetId>`
for a scene background. A background and a route of the same asset remain two sources (a loop
role and a clip role).

```
struct MediaTransportDesired {           // computed by MediaCore at command time
  std::string sourceId, assetId, path, kind;
  bool loop;                             // route.mediaAssetLoop; backgrounds always true
  bool onProgram, onPreview;
};
enum class MediaTransportState { Cued, Live, Paused, Ended };
```

| state | picture | clock | audio |
|---|---|---|---|
| `Cued` | poster (frame 0), held | stopped at 0 | none |
| `Live` | rolling | running | on |
| `Paused` | held on-air frame | frozen (T1.2 machinery) | none |
| `Ended` | last decoded frame | stopped at end | none |

A loop (`loop == true`) is only ever `Live` while it exists; it ignores pause and ends never.

### 3.2 The transition policy is pure

`native/src/core/MediaTransportPolicy.h` (the `CaptureReaderStallPolicy` / `TakeRecordPolicy`
shape): given the previous desired entry (or none), the current desired entry (or none), the
current state and an operator action, it returns one of:

| from | event | action |
|---|---|---|
| absent | appears with `onProgram` | **open + roll from 0**, audio on → `Live` |
| absent | appears `onPreview` only | **open poster at 0** → `Cued` |
| `Cued` | gains `onProgram` | **resume the same decoder** from 0, audio on → `Live` (this IS the old cue hand-off, without a re-key) |
| `Live`/`Paused`/`Ended` | loses `onProgram`, keeps `onPreview` | **restart the decoder at 0 behind the held frame** → `Cued` (the loop-restart pattern in the MF adapter: last good frame stays up until the new poster lands) |
| `Live` | stays `onProgram` across a Take (scene A → scene B) | **nothing** — never left Program, so it is not a go-live |
| `Live` | operator `pause` | freeze → `Paused` |
| `Paused` | operator `play` | resume from the frozen position → `Live` |
| `Live` (non-loop) | decoder reports end | → `Ended`, hold last frame |
| any | absent from both buses | **release**: retire decoder + bus source |
| any | same desired set re-sent | **nothing** (repeating sync / respawn re-apply is idempotent) |
| loop | `pause`/`play` | refused with a scene-validation warning |
| not `onProgram` | `pause`/`play` | refused with a scene-validation warning |

"Enters Program" is decided against the **previous Program desired set** held by `MediaCore`,
not against the clock or a generation string. Path change on the same source id (a repointed
bin row) is a release + open.

### 3.3 MediaCore feeds the desired set at command time

`MediaCore::syncMediaTransportsDesired()` runs where `syncStillMediaDesired()` already runs:
the end of `loadSceneGraph` (Program), the end of `applyPreviewScene` (Preview), and after the
new transport command. It walks `sceneRoutes_` + `sceneBackground_` (onProgram) and
`previewSceneRoutes_` + `previewSceneBackground_` (onPreview), skipping stills
(`isStillImageMediaAsset`, which stay on `StillMediaFrameCache` / kind `"still"`), and hands
the set to `MediaTransports::apply(desired, nowNs)`, which:

- adds a new `MediaAssetSource` to `sourceBus_` for each new source id (bus membership is now
  **command-driven**, so `syncMediaSources(bus, frames, "media")` is deleted; the `"still"`
  call stays),
- applies the policy's action to each existing source,
- removes released sources from the bus.

`apply` runs under `coreMutex` on the command thread. It starts worker threads and never does
I/O or a decoder open inline (the worker opens, exactly as `OwnedMediaFrameSource::run` does
today); a decoder restart is a flag the worker acts on. The 16-decoder cap and its loud refusal
move with the code.

### 3.4 Render and audio ticks

- **Video:** media rides the ordinary early bus ingest with every other kind (the kind
  exclusion added in 3a is removed); `MediaAssetSource::poll(programTime100ns)` returns the
  frame `MediaVideoPresentation` has due at that time, or the held frame. The post-plan media
  poll block in `renderSyntheticTick` and `IMediaFrameSource` in `ModuleSet` are deleted; the
  factory becomes `ModuleSet::mediaDecoderFactory` (the same `std::function<std::unique_ptr<
  IMediaFrameSource>()>` `OwnedMediaFrameSource` takes now — `IMediaFrameSource` survives 3b as
  the **decoder** contract only, retired in 4b).
- **Audio:** the audio gather calls `MediaTransports::popAudio(nowMs)` under `coreMutex`
  (a queue pop per live source, the same cost as today's `pollMediaAudioFrames`), with no plan
  built for it. The frameless `buildCompositorRenderPlan({})` call there is deleted.
- **Plan:** `CompositorRenderPlanLayer` loses `mediaPlaybackKey` and `mediaAssetPlaying`;
  `ProgramFramePreview`'s plan hash and the preview-scene dedup signature drop them. The
  `pausedClipCue` → `preview:` re-key in `buildPreviewCompositorRenderPlan` is deleted, which
  is what makes ruling 2 (one rolling picture) true by construction. `annotateLayerSource`,
  slice 4a health, `SourceContinuityLedger` and the take record are untouched.

### 3.5 Decoder contract inside the source

The worker still drives the decoder through `IMediaFrameSource` / `IMediaVideoPrefetch` with a
**layer the source builds from its own state** (`mediaAssetId/Path/Kind/Loop`, `playing` =
state is `Live`). The MF adapter's playback identity becomes `path|loop` (the key drops out);
"restart from 0" is a **new decoder instance in the same source** while the presentation keeps
its current frame — the adapter's existing loop-restart-behind-the-last-frame shape. `everPlayed`,
`MediaCueHandoff.h` and `adoptCuedDecoders` are deleted.

### 3.6 Wire

- `load-scene-graph` / `set-preview-scene` routes: `mediaPlaybackKey` and `mediaAssetPlaying`
  are **no longer read**; `mediaAssetLoop` stays. A scene background's `playing` is no longer
  read (loops always play). Old shells sending them are ignored, never refused.
- `set-media-playback {mediaAssetId, mediaAssetName, mediaAssetKind, mediaAssetPath}` becomes
  **selection only** (it still rides the repeating production sync; a `playing` field is
  ignored).
- New one-shot **`set-media-transport {mediaAssetId, action: "pause"|"play"}`**, acting on the
  `media:<assetId>` source. Refusals (§3.2) are scene-validation warnings and leave state
  unchanged. It is a gesture, not desired state, so it is deliberately **not** re-sent per sync;
  a core respawn reloads the scene and rolls from 0 regardless.
- Snapshot: `mediaPlayback` keeps its shape but `status`/`playing` now report the selected
  asset's **real** transport state (`playing` = `Live`), and `mediaPlaybackKey` is `""`. New
  node **`mediaSources[]`** `{sourceId, mediaAssetId, state, loop, positionMs, durationMs,
  onProgram, onPreview}`, published unconditionally (empty array when none — the multiviewer-node
  rule).

## 4. Shell design

- `MediaCoreCommandBuilder`: routes omit `mediaPlaybackKey`/`mediaAssetPlaying`; the playback
  command omits `playing` and the key; new `BuildMediaTransportCommand(assetId, action)`.
- `MediaRoutePlaybackService`: **delete** `BuildSceneMediaPlaybackKey`,
  `ResolveSceneRoutePlayback`, `ShouldPlaySceneMediaRoute`, `SceneRoutePlayback`, and
  `MediaGoLiveLedger`. **Keep** `ResolveTap(isOnProgram, isLooping, isOperatorPaused)`,
  `IsLoopingAsset/Kind`, `ChooseAssetToPromote` (selection UX only), `SelectedAssetLeftProgram`.
  `IsPlayingOnAir(assetId, snapshot.MediaSources)` reads the core's node.
- `TransportCoordinator.TakeAsync`: `ITransportHost.RecordProgramMediaGoLive` is deleted; the
  selection promotion stays. `TakeMediaSelectionRollback` keeps the selection rule and takes
  the playing flag from the snapshot.
- `StudioViewModel`: a bin-row tap or the transport toggle sends `set-media-transport`; the
  bin rows' `IsPlaying` and `SelectedMediaAssetPlaying` / `MediaPlaybackButtonLabel` are
  refreshed from `mediaSources[]` on snapshot apply as **per-row scalar updates**, never a
  `MediaBinGroups` rebuild (0xc000027b rule). `RefreshMediaBinPlaybackIndicators` after a Take
  becomes that same scalar pass.
- Snapshot models: `NativeMediaCoreMediaSource` + `MediaSources` on the wire/base/synthesized
  snapshots; the mapper carries it like `MediaPlayback`. No prefs change.

## 5. Proof

**Core (MediaCoreCommandTest / new MediaTransportPolicyTest / MediaPlaybackTimelineTest):**
- policy table in §3.2, one test per row, decoder-free;
- cued clip taken to Program rolls from 0 with audio and **one** decoder instance;
- the same clip on both buses yields one frame id per tick on both plans;
- clip leaves Program then re-enters → frame id regresses to the poster then rolls (restart);
- Take between two scenes both holding the clip → no restart (frame ids keep advancing);
- pause holds the frame and emits no audio, play resumes at the frozen position;
- loop ignores pause and never restarts across a Take;
- non-loop end holds the last frame;
- identical `load-scene-graph` re-sent → no transition, no decoder churn;
- `ProgramPixelContinuity.ACuedClipTakenToProgramNeverShowsThePlaceholder` re-pointed at the
  new path; the take record still reads `missingSources=[]` for a cued clip;
- `MediaRouteAppearsOnTheSourceBusAndLeavesWhenUnrouted` now asserts command-time membership.

**Shell:** builder omits the retired fields and emits the transport command; tap → command;
rows/labels driven by a synthesized `mediaSources[]`; Take rollback playing-from-snapshot.

**Gates:** Windows dev suite + stub gate (`scripts/test-native.ps1`), `validate-multiview.mjs`,
`validate-tiles.mjs`, `scripts/qa/zoom-gap-hold-ab.py` (non-regression), the show drill at
`--load 8` with `COREVIDEO_FAKE_ENGINE_FPS=60`.

**Live oracle (new):** `scripts/qa/media-take-ab.py` — cue a known clip (first frame a solid
colour, then motion) in Preview, record Program, Take, and judge with `ffmpeg signalstats` that
the first Program frames after the Take carry the clip's first-frame luma (never the slate
luma) and that luma then moves. Run on the branch build before the PR is opened.

## 6. Out of scope

Holding the outgoing picture on a cold Program cut (#449 step 1); seek/scrub/in-out/speed (a
transport UI); moving `StillMediaFrameCache` inside the still source; retiring
`IMediaFrameSource`/`IMediaVideoPrefetch` (slice 4b); media audio on the bus `SourceTick.audio`
(still popped directly, like Zoom audio on its ring).

## 7. Risks

- **Restart-behind-held-frame on Live→Cued** relies on the MF adapter keeping `lastFrame`
  across a reopen; the FFmpeg fallback path (ProRes) restarts a process — bounded by the
  existing resume ladder, but Preview may show the held frame for longer on those assets.
- **Audio on the cue resume:** the poster decoder never decoded audio; the resume must arm
  audio at position 0 (today `adoptCuedDecoders` sets `wantsAudio` — the same flag flips in
  the policy action).
- **Shell/core version skew during rollout:** the core ignores the retired fields, so an old
  shell against a new core plays clips by scene membership (loses shell-side pause until it
  updates); a new shell against an old core has no transport command (pause refused loudly by
  the unknown-command path).
