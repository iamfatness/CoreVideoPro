# Persistent sources: buses select, sources live

Status: approved design, 2026-09-10 (owner-reviewed section by section).
Branch: `codex/persistent-sources`, off `main`.

## 1. Why

On the 2026-09-10 live test (meeting 8916561023, 7 participants) the owner cut a
Tiles gallery scene between Preview and Program and watched both the wall's
foreground tiles and the scene's media background re-render on every cut, in both
directions. The core's new Take record reported `verdict=cut` for every one of
those cuts, so the instrument was wrong as well as the product.

Four read-only audits established the cause. It is structural, not a bug in one
source:

- **Media has one decoder per bus, not per asset.** Preview media layers are
  renamed `preview:<id>` (`MediaCore.cpp:5147-5155`), and the media owner keys
  decoders by `sourceId|path|assetId|playbackKey|playing|loop`
  (`OwnedMediaFrameSource.h:119-121`). A background present only in the incoming
  scene cold-starts on Program: new thread, open file, first frame from 0, with a
  solid placeholder colour drawn until it lands (`D3D11CompositorAdapter.cpp:690-696`).
  When both scenes share a background, the two decoders run on independent clocks.
- **Clips restart on every Take.** The shell mints a new playback key per Take
  (`TransportCoordinator.cs:154`, `MediaRoutePlaybackService.cs:87-122`), so a
  clip already playing on Program restarts from frame 0.
- **The Tiles wall has one animator per bus** (`MediaCore.h:499-500`) with a
  one-way Preview→Program hand-off (`MediaCore.cpp:6021-6036`) and no hand-back.
  The default 300 ms fade renames layers `take-in:`/`take-out:`
  (`MediaCore.cpp:5072-5073`), which the animator does not recognise, so tiles can
  replay their entrance when the fade ends.
- **Transitions fade per layer**, so shared content dips to ~75 % brightness at
  the midpoint and an on-air lower-third is drawn twice.
- **Live feeds already behave correctly.** Zoom, capture, browser and still
  pixels are cached by source id in one shared texture cache
  (`D3D11CompositorAdapter.cpp:1345-1403`), which is why they never flash.

Every competitor solved this the same way. OBS, vMix and mimoLive all model a
source as one persistent object with its own clock; scene items / layers are
references to it; Preview and Program are selectors; and "restart" is a
per-source policy triggered by going live, never by a render pass. Our own OBS
plugin's CoreVideo Tiles source is built this way
(`zoom-supersource.cpp:223-400`, state per instance; `:2976-3007`, survives
Studio Mode transitions untouched). This design adopts that model.

## 2. The model

**Source.** A persistent object with one owner, one clock and one texture. It
exists while any scene references it or it is pinned — never because a bus shows
it. Kinds: Zoom participant, capture device, browser, still image, media clip,
media loop, Tiles wall, graphic/lower-third. Identity derives from the asset or
device (`media:<assetId>`, `zoom:<pid>`, `capture:<id>`, `tiles:<wallId>`), never
from the bus. The `preview:` namespace and per-Take playback keys are removed.

**Scene.** An ordered stack of layers. A layer is a reference to a source plus
placement: rect, crop, fit mode, opacity, per-layer colour grade and chroma key.
Any source may sit above any other; a background is the bottom layer, nothing
more.

**Bus** (Program, Preview, Multiview). Each tick a bus composites one scene's
layers by sampling source textures. A bus owns no source state. A cut changes
which stack a bus samples and nothing else.

**Go-live.** The only event a source reacts to. Fired once when a source's scene
enters Program and the source was not already on Program. Policy is per source:

| Kind | On go-live | While in Preview |
|---|---|---|
| Live feed, still, media loop | nothing | live, identical to Program |
| Media clip (non-loop) | roll from 0, audio on | paused on first frame |
| Tiles wall | nothing | live, same animation state |
| Lower-third / graphic | play in-animation | shows on-air state |

The clip default is overridable per source ("never restart"), matching vMix's
Restart-with-Transition and OBS's restart-when-active settings.

**Transition.** Blends two finished bus images; never per-layer opacity. Go-live
fires at transition start so a clip is rolling as it fades in.

**Lifetime.** A source referenced by no scene is released after a short grace
period. That is the one cold start that remains, by design.

## 3. Rendering: once per new frame, never once per bus

Three tiers, cheapest first:

1. **Pass-through** (Zoom, capture, browser, still): already in the shared
   source-texture cache keyed by source id. Buses sample directly. No new cost.
2. **Decoded media**: one decoder per asset, owned by the source, publishing into
   the same cache under the asset id. The Preview decoder is removed: net saving.
3. **Composed** (Tiles wall; later lower-thirds/graphics): the source composites
   into its own transparent canvas-sized texture on the render thread, only when
   an input changed (new member frame, animation step, settings). A settled wall on
   static input costs nothing. Buses draw the texture as one layer, using the
   existing retained-texture layer path (`ResolvedLayer::retainedProgram`,
   `D3D11CompositorAdapter.cpp:1057-1077`).

Also:

- The multiview PVW cell samples the Preview bus texture instead of
  re-compositing the preview stack (`MediaCore.cpp:3501-3526` today), the way the
  PGM cell already samples Program.
- Colour grade moves from source to layer (`Interfaces.h:425-428`), so two scenes
  can grade one source differently without forcing a re-render.
- Everything stays on the single render thread and immediate context under
  `coreMutex`; no second device. Per-tick plan rebuilds (Preview up to 4×,
  Program up to 2×) collapse to one per bus.

**Budget.** Program renders in ~6.6 ms on the RTX 4090 today; a transparent
1080p pass is well under 1 ms and replaces today's per-bus wall drawing, so one
wall is roughly cost-neutral. Unproven on an integrated GPU: that is a measured
gate (section 5), not an assumption.

## 4. Proof: "nothing re-rendered" is measured, not eyeballed

1. **Per-source generation counters.** Every source carries `generation` (bumped
   on any decoder open, animator reset, texture recreate or restart) and
   `lastFrameId`. The Take record captures both for every source in both stacks
   before and after. A clean cut means: no source present in both stacks changed
   generation, and each kept advancing frameId. The verdict cannot read `cut`
   while a decoder cold-started.
2. **Pixel continuity.** A test-only probe samples pixel rows of Program and
   Preview on the ticks around a Take (the luma-comb technique from the
   colour-range fix). A placeholder-colour flash is a luma discontinuity and fails
   the test regardless of counters.
3. **Live soak.** `scripts/qa/live-meeting-soak.mjs` drives cut and fade Takes on
   a real meeting and asserts on 1 and 2 plus fps and program-buffer underruns.

Tests that pin today's per-bus behaviour are rewritten to assert the new
contract, never deleted:
`MediaRoutePlaybackServiceTests.cs:216-316`, `TransportCoordinatorTests.cs:321,460-479`,
`MediaCoreCommandTest.cpp:2850,4095,4393-4495`, `StillMediaFrameCacheTest.cpp:350-380`,
`MediaPlaybackTimelineTest.cpp:94`, `TilesRenderPlanTest.cpp:519,916-1081`,
`TilesAnimatorTest.cpp:97-163`, `RenderedSceneAttributionTest.cpp:196`,
`OverlayTileRasterTest.cpp:193`, `MediaCoreCommandBuilderTests.cs:41-64`,
`LowerThirdPhaseRecoveryTests`.

## 5. Phasing

Each slice ships independently and is verified by section 4.

1. **Instruments + media as persistent sources. Implemented on branch
   `codex/persistent-sources` (`e9cf013..7826c7c` plus the docs commit that
   carries this line), unmerged, live soak not yet run.** Generation counters in
   the Take record (`core/SourceContinuityLedger.h`, `core/TakeRecordPolicy.h`);
   pixel probe (`ProgramPixelContinuityTest.cpp`); one player per media asset
   (`OwnedMediaFrameSource`, `buildPreviewCompositorRenderPlan`); go-live policy
   (`MediaGoLiveLedger`, `MediaRoutePlaybackService`/`TransportCoordinator`) with
   operator pause as per-asset state (the ledger's paused set — promoting another
   asset can never un-pause a clip); the route wire carries the loop flag
   (`mediaAssetLoop`); route stills skip the decoder path (served only by
   `StillMediaFrameCache`); removed the `preview:` namespace and per-Take keys
   (kept only for a paused clip-cue poster); still-image keys unified. Fixes the
   background flash and clip restarts. Known gaps, carried to later slices:
   - **A clip going live still cold-starts.** The Preview cue poster
     (`preview:media:<id>`, paused) and the rolling Program source (`media:<id>`)
     are different decoders, so a clip entering Program opens a fresh decoder:
     the placeholder slab shows for a few ticks and its take record honestly
     reads `rebuilt` with `missingSources=[media:<id>]`. The fix — hand the
     warmed cue decoder to Program — belongs to a later slice.
   - Scene backgrounds cannot be paused today: the shell hard-codes
     `Playing: true` on the background wire (`StudioViewModel.BuildSceneBackgroundWire`).
     A future pause path would collide the same way stills did (the `playing`
     flag is part of the decoder's request key, so a paused copy on one bus and
     a playing copy on the other are two decoders under one id) and must be
     designed with that in mind.
   - A route slot reassigned onto Program records no go-live.
   - The Tiles wall animator is still per bus (slice 2, not this slice).
   - Transitions still fade per layer (slice 3).
   - Metal/CPU parity is untouched (slice 4).
   - `scripts/qa/live-meeting-soak.mjs --takes N` has not yet been run against a
     real meeting.
2. **Tiles wall as a persistent source.** One animator per wall, one offscreen
   texture, buses sample it; hand-off/hand-back code deleted. Integrated-GPU
   budget gate lands here (`MonitorRenderFaultInjection` on a quiet machine, plus
   a measured number on an iGPU).
3. **Transitions blend bus images; lower-thirds become source-owned state.**
   Fixes the fade dip and the double-drawn key.
4. **Metal and CPU-preview parity.** macOS catch-up (Windows-first ruling).

**Out of scope:** the placeable wall source and scene-as-source, per-layer
in/out animation, scene UI changes. The model leaves room for all three.

**Risks:** the media owner's 16-decoder cap (`OwnedMediaFrameSource.h:211`)
becomes a real limit once sources outlive buses: it must warn loudly, never drop
silently. Slice 2 touches the render thread: fault-injection tests run on a
quiet machine before merge. Branch is off `main`, not #419, whose Wave 1/2
source classes are unwired; `SourceRegistry.h` is reused only if it fits
cleanly.
