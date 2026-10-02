# The source bus (#535 slice 0, 2026-09-18)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

`docs/superpowers/specs/2026-09-18-source-bus-design.md` is the spec. Slice 0 lands
the CONTRACT and the BUS, alongside the three existing live pipes (Zoom, capture,
media), untouched. Nothing on air changes yet.

- **`ISource`** (`native/src/core/SourceBus.h`) is the one ingest contract every
  source kind will eventually implement: a pull `poll(programTime100ns)` returning a
  `SourceTick{video, audio, health, clockOffset100ns}` — no `layers` argument, no push
  callback. `TestPattern.h` / `TestPatternSource.h` are the first (and, in production,
  ONLY) implementation: a header-only SMPTE-bars generator.
- **`SourceBus`** is core-owned and header-only. It ingests under `coreMutex`
  (`SourceBus::ingest(programTime100ns, nowNs)`), zero-copy (shared_ptr/move), and
  counts `framesIngested`/`droppedFrames` per source; health is decided at READ time
  (`snapshot(nowNs)`), not latched at ingest — the same "peek is not an observation"
  discipline as `RenderedSceneAttributionPolicy` elsewhere in this file.
- **In PRODUCTION the bus was EMPTY at slice 0** — true only as of this slice.
  `TestPatternSource` is registered ONLY through the `MediaCore::addSourceForTest`
  test seam — never wired to any real join/capture path, never on air — and a
  slice-0 production build's `sources[]` snapshot node is present and empty,
  exactly like the multiviewer-node rule: absence of activity is not absence of
  the node. **This stopped being true at slice 1**, below: Zoom video, then
  capture, are real production sources on the bus now. See "Slice 1" and
  "Slice 2" for the current state — do not read this bullet as describing
  today's bus.
- **`source_id == VideoFrame::participantId`.** The bus deliberately reuses the
  existing frame-keying scheme (`zoom:<pid>` / `capture:<id>` / `media:<assetId>`
  today) so a migrated source is a drop-in replacement for its old frame producer,
  not a new addressing scheme downstream code has to learn.
- **The snapshot carries a `sources[]` node** with `framesIngested`/`droppedFrames`/
  health present even at zero (Task 4) — the test source never drops, so `slice 0`
  reports `droppedFrames: 0` honestly rather than omitting the field; real back-
  pressure numbers land only once a high-rate live kind migrates onto the bus.
- **What's next, not done here:** slices 1-3 migrate Zoom, capture and media onto
  `ISource` one at a time (each is its own real-consumer PR, per the #419
  foundation-lands-with-a-consumer rule); slice 4 retires the three old poll
  interfaces once nothing downstream still calls them. Deferred by design, NOT a
  gap in this slice: done-when #2 (Zoom/UVC/media on the contract) and #3
  (downstream consumes ONLY the bus).

Tests: `native/tests/SourceBusTest.cpp` — `SourceContract.*` (the test-pattern
generator), `SourceBus.*` (ingest/dedup/stale-decay/remove), `SourceBusMediaCore.*`
(a bus source composites into Program end to end via `addSourceForTest`), and
`SourceBusSnapshot.*` (the `sources[]` node, including the empty-bus case). Verified
green on both gates 2026-09-18: the stub build (`scripts/test-native.ps1`, 1015
tests) and the Windows dev suite (`native/build-dev/corevideo-native-tests.exe`,
1046 tests, Release core confirmed at 2,258,432 bytes) — the latter including the
UNCHANGED `CaptureIngest.*`, proving the `StubModules` generator swap stayed
byte-identical.

**Slice 1 (2026-09-19): Zoom VIDEO migrated onto the bus, one `ISource` per
participant.** `ZoomParticipantSource : ISource` (`native/src/core/ZoomParticipantSource.h`)
holds one participant's latest decoded frame; `poll()` returns it keyed by the
RAW participant id — deliberately **NOT** `zoom:<pid>`, so the engine-roster merge
and `SourceContinuityLedger` (frameId regression / cold-start detection, "the
Live-meeting QA day" section above) keep working unchanged. MediaCore's decode
tap feeds them through the PURE `syncZoomParticipantSources(SourceBus&,
vector<VideoFrame>)` helper (`ZoomBusRoster.*`/`SourceBus.*` tests) — arrivals
add/update a source, departures remove by **`descriptor().kind == "zoom"`**, not
by id prefix (a prefix check would misclassify any future `zoom:`-prefixed
non-Zoom id). The synthetic slate still serves the no-engine case entirely
through `RealZoomCaptureSource` — untouched, still the wrapper
`RealZoomCaptureSourceTest.*` exercises. **Deliberately NOT done here:** Zoom
AUDIO stays on its existing SHM-ring path, and `IZoomCaptureSource`/
`IUvcCaptureSource`/`IMediaFrameSource` are not deleted (spec §5 slice 4 retires
them once every kind is migrated) — the cushion/churn/speaker-director logic
stays exactly where it was, in `ZoomEngineRuntime`.
**The engine-live path had no unit test by design; this task's fake-engine drill
is its proof, and it shows NO regression** (`python scripts/mac-show-drill.py
--seconds 40 --load 8`, `COREVIDEO_FAKE_ENGINE_FPS=60` pinned, 8x1080p60
synthetic Zoom feeds through the real per-participant bus ingest path). Baseline
= `main` at 7ad7d589 (the slice-0 commit, same Release core size 2,258,432
bytes) in a separate worktree, same rig, same pinned rate:

| metric | main (pre-slice-1) | this branch (slice 1) |
|---|---|---|
| sustained render fps | 60.0 of 60, 0 dropped | 60.0 of 60, 0 dropped |
| render hold | 3.9ms | 4.0ms |
| worst frame | 17.1ms | 17.0ms |
| coreMutex over-budget | 47/3174 (1%) | 40/3262 (1%) |
| command round-trip p50/p99 | 3.2ms / 46.8ms | 3.2ms / 9.7ms |
| source->render latency p50/p99 | 26.5ms / 37.1ms | 17.8ms / 27.1ms |
| decoded-frame delivery | 100% | 101% |
| Zoom ingest accepted rate | ~480 f/s (8 x 60fps) | ~480 f/s (8 x 60fps) |

Every gated metric matches or improves on baseline — the lower per-participant
bus-source latency is consistent with removing a layer of indirection between
decode and the render gather, not a fluke of one run (both runs used the same
40s/load 8/pinned-fps harness). `node scripts/validate-multiview.mjs` and
`node scripts/validate-iso-record.mjs` both still PASS unchanged: Zoom frames
reach multiview and the ISO writers keyed by the raw participant id exactly as
before (ISO manifest still names `zoom:101`/`zoom:102`, clap-align 0.0ms).

**Slice 1 shipped a live regression the drill could not see (#554, found by the
owner 2026-09-19, ~90 min after merge).** The old `RealZoomCaptureSource` store
NEVER erased a participant's frame: when the engine retired a video
subscription (video-budget eviction, spine churn around a Take) the last frame
kept painting until the engine roster dropped the participant. The bus removed
the source on the first tick without a decoded frame, so a routed participant
fell to the compositor's fallback slate for the whole gap — the owner saw it as
"sources flashing". Parity rule now in `ZoomBusRoster.h`: a Zoom source is
removed only when the participant is absent from BOTH the tick's decoded frames
AND the engine's subscription roster (`pollCompositorVideoFrames`, polled ONCE
per tick and reused by the merge); an empty roster removes nothing, mirroring
the merge gate. Two lessons, both already in this file's spirit:
- **"Frames are byte-identical" is a steady-state argument. Lifecycle is where
  a migration regresses.** The first-pass analysis proved every field of every
  frame matched and cleared slice 1; the owner's A/B ("it's new") was right.
  Enumerate the erase/retire/clear paths of the OLD store before calling a
  replacement equivalent.
- **The perf drill never removes a subscription, and nothing on the wire carries
  per-layer pixels or geometry** (`programPixelSignature` is 0 on the display
  tick; `RenderedProgramSources` publishes ids only). The oracle that caught it
  is `scripts/qa/zoom-gap-hold-ab.py`: record PROGRAM to MP4 around a forced
  subscription gap and judge it with `ffmpeg signalstats` per-frame YAVG — the
  slice-1 core dropped from ~188 to ~150 for the 500 ms gap, the
  `beta-2026-09-18-c80ee51` core held ~188. Run it (both cores if in doubt)
  before any further bus slice; a source migration is not done until the gap
  test holds the picture.
The owner also reported a NEW "first Take of a fresh source renders half
off-screen until re-cued" on the same build (#555). Not reproduced headlessly:
the fake engine honors a resolution upgrade in place and never restarts a
stream, so it cannot model the real engine's renderer rebuild on the
preview→program 1080P flip. Owner re-test on the #554 fix decides whether it
was the same gap; if not, #555 names the next diagnostic (a compositor
geometry log line on change).

**Slice 2 (2026-09-19): capture onto the bus.** `CaptureDeviceSource : ISource`
(`native/src/core/CaptureDeviceSource.h`) is one bus source per `capture:<id>`
frame — one instance per capture device id, mirroring `ZoomParticipantSource`'s
shape from slice 1. It is fed from the SAME unchanged adapter poll (the WinUi
SHM bridge, the browser host, `SrtIngestCaptureAdapter`'s `channel->latest`,
native UVC, and WGC screen capture — all five verified) through a new pure
helper, `syncCaptureSources(SourceBus&, const vector<VideoFrame>&)`
(`CaptureBusRoster.h`), mirroring `syncZoomParticipantSources`'s call shape from
slice 1 but making the OPPOSITE removal decision on purpose:

- **The bus MIRRORS the adapters' tick and REMOVES a capture source absent from
  it — the deliberate opposite of the Zoom parity rule (#554) above.** #554
  taught that a Zoom source must survive an empty tick because the engine holds
  the participant's last frame across a subscription gap and the bus removing
  it early caused the fallback-slate flash. Capture is the opposite case:
  EVERY capture adapter already re-emits its last held frame on every tick
  while the device stays connected (proven for all five adapters, above, incl.
  `DisconnectedDeviceEmitsNoFrame`) — so a capture id's absence from the tick
  means the device is actually gone, not merely paused mid-gap the way a Zoom
  subscription eviction is. Removing on absence is therefore the correct
  parity for THIS kind, not a regression of the #554 lesson: the lesson was
  "match the producer's real hold behavior," and the two producers hold
  differently.
- **MediaCore now partitions the bus's output by kind** so capture frames keep
  their pre-bus position AHEAD of Zoom/test frames in the merged set, and still
  ride the existing roster merge unchanged — pinned by
  `CaptureBusFramesGatherBeforeOtherBusKinds`. This exists because downstream
  ordering-sensitive code (the no-routes grid fallback) predates the bus and
  was never rewritten to be order-independent for capture specifically.
- **Two accepted behavior notes, not gaps:**
  1. Capture frames' relative order AMONG THEMSELVES changed from adapter
     emission order to sourceId-sorted (`SourceBus` is a `std::map`). Every
     consumer matches capture tiles by `participantId`, so this is invisible
     everywhere except the no-routes grid fallback, which is the one place
     order was ever load-bearing.
  2. A capture device that has never delivered a frame is not on the bus at
     all (its adapter emits nothing for it) — there is no per-kind
     warming/stalled slate for a probe-only device yet. The spec's "warming
     instead of a per-kind slate" for that case lands in slice 4 with the
     compositor fallback collapse; `droppedFrames` for capture also waits for
     slice 4 (today it lives on `CaptureDeviceInfo`, not on the frame, so the
     bus has nowhere honest to read it from yet).
- **`deliveredFps` is now measured from a per-decoded-frame `framesIngested`
  counter**, not from the engine's `"frame"` IPC event — that event is a ~1/s
  beacon, so `framesReceived` was never actually a frame count, and any prior
  reading of the Zoom spine snapshot's `deliveredFps` as a real rate was wrong
  before this fix landed alongside slice 2. **It is the AVERAGE since the
  feed's first decoded frame, not a sliding window** — `(framesIngested - 1)`
  over the elapsed time between the first and most recent frame. A live
  mid-show drop (a stall, a resolution re-subscribe) therefore moves the
  number slowly, diluted by every good frame already averaged in before it —
  it is not a real-time rate. A true recent-window rate is a follow-up, not
  what this field reports today.
- **Task 5 regression numbers (2026-09-19, this branch):** Windows dev suite
  `native/build-dev/corevideo-native-tests.exe` — 1059 tests passed, 0 failed
  (unchanged count from slice 1/#554). Stub gate
  (`scripts/test-native.ps1`) — green, 100% tests passed, 0 failed.
  `node scripts/validate-multiview.mjs` — PASS (capture/Zoom tiles still reach
  multiview). `scripts/qa/zoom-gap-hold-ab.py --label slice2` — Program luma
  held 187.5–204.8 across the whole recorded window including the forced
  subscription gap (the animated test pattern's normal sawtooth range); no dip
  toward the ~150 slate-fallback signature anywhere in 243 sampled frames. The
  rig's `bus=` tuples showed a real capture source (`capture:decklink-1`)
  alongside Zoom the whole run — this rig has a capture device connected under
  the fake engine, so slice 2's capture-on-the-bus path was exercised live in
  this same gate, not just in the stub `FakeCaptureDevice` unit path. Show
  drill (`COREVIDEO_FAKE_ENGINE_FPS=60`, `mac-show-drill.py --seconds 40
  --load 8`) — PASSED: 60.0fps of 60 sustained, 0 dropped, 5.3ms render hold,
  100% decoded-frame delivery, coreMutex over-budget 110/3217 (3%), command
  round-trip p50 4.7ms/p99 12.1ms — run alongside an unrelated idle soak
  instance of the packaged app on this machine, so these numbers are advisory
  under that contention rather than a clean-machine baseline, but they clear
  every gate threshold with margin (no gross failure: delivery was 100%, not
  <90%; fps was 60, not <50).
- **Still NOT done, by design (slice 4 retires the old interfaces):**
  `ICaptureDevice`/`IUvcCaptureSource`/`IZoomCaptureSource`/`IMediaFrameSource`
  are not deleted; Zoom audio and media stay on their pre-bus paths untouched;
  `sources[]` now lists capture sources alongside Zoom, but nothing downstream
  reads the bus exclusively yet — the old per-kind poll paths are still the
  producers MediaCore actually gathers from outside the bus-ordering test.

**Slice 3a (2026-09-19): media onto the bus, parity.** `SourceBus::ingest` gained
a THIRD, **selector**, overload — `ingest(programTime100ns, nowNs, select)` polls,
counts and returns frames only for sources whose descriptor passes `select`;
unselected sources are neither polled nor counted (the 2-arg overload is now the
selector overload with an always-true predicate). `MediaAssetSource` (kind
`"media"` or `"still"`) is the bus-side producer, and `syncMediaSources(bus,
frames, kind)` adds/updates/removes per kind — mirroring the producer, exactly
like slice 2's capture rule and the OPPOSITE of slice 1/#554's Zoom-parity rule:
the media/still owner (`OwnedMediaFrameSource` / `StillMediaFrameCache`) re-emits
a frame for EVERY requested key each tick, held/paused keys included, so a key
absent from the poll really is gone (not requested, or not decoded yet) and the
bus removing it matches what nothing draws for it today.

- **What moved:** stills are synced from `StillMediaFrameCache::collectFrames`
  and produced by a `"still"`-kind bus ingest at the SAME injection point they
  always used (after the engine-roster merge, before the ISO snapshot); decoded
  media is synced from `pollMediaFramesAt100ns(mediaLayers, …)` and produced by a
  `"media"`-kind bus ingest at the same post-plan point (after the render plan,
  before render/continuity-observe/take-record). `MediaCore`'s EARLY ingest (the
  one before the roster merge) now excludes both media kinds — a stale media
  frame injected before its request update would otherwise get duplicated once
  the real one lands.
- **What did NOT move in 3a, and why it could not yet** (all of it moved in 3b —
  next bullet). The request set (which decoders
  exist, `requests(layers)`), pause/hold (`mediaAssetPlaying`, `clockFrozen`), the
  cue→Program hand-off (#449), and the `preview:media:<id>` poster key all stay
  inside `OwnedMediaFrameSource`. Media is the one kind whose producer is
  request-driven FROM THE RENDER PLAN every tick — `selectVideo(layers, t)` reads
  the Program+Preview plan layers to decide what to decode, applies play-state,
  and adopts a cued decoder — so it can only be polled after the plan is built.
  Moving that decision onto the bus means dropping the `layers` argument
  entirely, and that is deferred to **slice 3b** (own spec — see below): it hangs
  on the owner's Take-semantics ruling (#449 step 1, "hold outgoing picture on a
  plain cut"), because go-live/roll-from-0 and the hand-off are one decision.
- **Removal mirrors the producer, like capture (opposite of the Zoom/#554
  rule).** A held/paused media key still composites every tick but is not
  evidence of an active decoder stalling — it is the owner correctly repeating
  itself — so `syncMediaSources` removing an absent key is the SAME parity
  argument as slice 2's capture rule, not slice 1's: the media/still owner
  already re-asserts every requested key each tick, so an absence is real.
- **Three accepted behavior notes, not gaps:** (1) media/still frames' order
  among themselves is now sourceId-sorted (the bus is a `std::map`) instead of
  request order — only the no-routes grid fallback is order-sensitive, and it
  does not touch media; (2) a held (paused) clip still composites every tick but
  does not bump `framesIngested`, so it decays to `stalled` health after 200 ms
  on the bus — honest for a paused clip, not a defect; (3) duplicate effective
  ids (e.g. a route and a background resolving to the same source id) now
  collapse to one bus entry (last write wins) instead of the old two-list model
  emitting both.
- **`sources[]` now covers every live kind**: zoom / capture / still / media.
- **Task 4 regression numbers (2026-09-19, this branch):** Windows dev suite
  `native/build-dev/corevideo-native-tests.exe` — 1065 tests passed, 0 failed.
  Stub gate (`scripts/test-native.ps1`) — green, 100% tests passed, 0 failed.
  `node scripts/validate-multiview.mjs` — PASS. `node
  scripts/validate-show-engine.mjs` requires a running WinUI app (none was
  running, and the app was deliberately not launched for this task) — headless
  `node scripts/validate-tiles.mjs` ran instead and PASSED (fingerprint sanity,
  per-tile identity, background colour, tile-boundary/rect match, structural
  invariants, aspect, gutter/margin, fill-not-letterbox, reflow-after-departure —
  10/10 checks green, 2 skipped for not-yet-built features). `scripts/qa/zoom-gap-hold-ab.py
  --label slice3` — Program luma held 187.49–204.74 across the whole recorded
  window including the forced subscription gap, no dip toward the ~150
  slate-fallback signature anywhere in 246 sampled frames; the rig's `bus=`
  tuples list every live source id with its dims and per-tick state
  (`producing`/`stalled`/`holding`) — `capture:decklink-1` rode alongside Zoom
  the whole run. Show drill (`COREVIDEO_FAKE_ENGINE_FPS=60`, `mac-show-drill.py
  --seconds 40 --load 8`) — PASSED: 60.0fps of 60 sustained, 0 dropped, 5.3ms
  render hold, 100% decoded-frame delivery, coreMutex over-budget 140/3207
  (4%), command round-trip p50 4.6ms/p99 13.9ms. Artifacts archived under
  `artifacts/qa/slice3-gap-hold/` (gitignored). **The gap-hold recording itself
  ran with only zoom + capture sources on the bus** (the rig's `bus=` tuples show
  no media/still entries) — for THIS slice it is a non-regression gate on the
  zoom/capture path, not on-air proof of media on the bus. The media on-air
  evidence is `CompositesMediaRoutePixelsIntoProgramPreview` and
  `StillMediaRouteCompositesDecodedPixels` passing through the bus path, plus the
  new tests' pixel probe (`MediaCoreCommand.MediaRouteAppearsOnTheSourceBusAndLeavesWhenUnrouted`,
  `StillMediaFrameCache.StillRouteAppearsOnTheSourceBusAsKindStill`).
- **Slice 3b DID THIS, merged 2026-09-21 in
  [#567](https://github.com/iamfatness/CoreVideoPro/pull/567)** — it drops the
  `layers` argument: media request state moves to source state set at command
  time, `poll(ts)` serves what that state is due, and the cue hand-off and the
  `preview:` poster are DELETED rather than moved. See "Slice 3b" below and
  `docs/superpowers/specs/2026-09-20-source-bus-slice3b-media-source-state-design.md`.

**Slice 4a (2026-09-19): bus health on air.** Retires the per-kind "pink tile"
(`colorFromParticipantId` placeholder) everywhere a layer's source is not
producing, and lands #535 done-when #3 ("no per-kind special case for empty
frames") for the RENDER path — the three old poll interfaces
(`IZoomCaptureSource`/`ICaptureDevice`/`IMediaFrameSource`) still exist behind
the bus; retiring them is **slice 4b**
(`docs/superpowers/specs/2026-09-19-source-bus-slice4-scoping.md`).

- **The owner's rulings (2026-09-19, on #449/#535):** *warming* (no content
  frame, health not failed) = a neutral dark slate; *failed/missing* (no
  content frame, health `failed` or `stalled`) = a dark slate WITH the
  source's RESOLVED name (operator override, else the roster/device-derived
  name — see R5 below, never a raw id); *stalled* (had frames, none recently)
  is the OPERATOR's per-source choice — hold last frame (default) or black —
  never a fixed rule, and (final-review correction, same day) **applies to the
  PROGRAM pass only** (see R1/R3 below) and **only for Zoom sources this
  slice** (see R2 below).
- **Final-review fix wave (2026-09-19, same branch) — the first cut above was
  too broad in three ways, all corrected same day:**
  - **R1 — on-air "stalled" is Zoom-only and hysteretic (1.5s), not the bus's
    200ms diagnostic.** `SourceBus::kStaleAfterNs` (200ms) still drives the
    `sources[]` diagnostic snapshot unchanged; `core::kOnAirStallNs`
    (1'500'000'000 ns, `SourceBus.h`) is a SEPARATE, longer threshold used
    only by `MediaCore::annotateLayerSource` to decide a layer's on-air
    `sourceHealth`. A layer's health is `"stalled"` only when the bus source's
    `descriptor().kind == "zoom"` AND at least 1.5s have passed with no new
    frameId; every other kind (capture/media/still — browser sources, WGC
    screen capture, stills, paused clips) reads `"producing"` regardless of
    frame cadence, because those kinds legitimately re-serve the same frameId
    for long stretches while healthy. `SourceBus::onAirStatusFor` exposes the
    `{health, sinceLastNewFrameNs, kind}` triple this decision needs, as a
    sibling to `healthFor` (which still serves the 200ms diagnostic
    unchanged).
  - **R2 — a dropout POLICY is Zoom-only this slice.** `MediaCore::setSourcePolicy`
    refuses (loud scene-validation warning, no state change) a `dropoutPolicy`
    for any id not starting `zoom:` — capture liveness via `signalPresent`
    isn't wired to a stall decision yet, so a capture "black" policy would act
    on cadence noise, not a real dropout; that wiring is a **follow-up**. A
    source's `displayName` is NOT gated the same way — it is accepted and
    stored for ANY id (capture included), because the failed slate needs names
    for capture too (R5). The shell's capture-row "On dropout" ComboBox is
    REMOVED from the Sources page (the Zoom guest row's combo stays);
    `CaptureDevice.DropoutPolicy` plumbing is kept as harmless dead weight
    (`ProductionStateHelper.PopulateCaptureDeviceDropoutPolicy` always resolves
    it to `"hold"` now, since a capture key can no longer reach the persisted
    dictionary). `StudioViewModel.SetSourceDropoutPolicy` and the
    `source.dropout.set` control action both refuse a non-`zoom:` id
    (`ControlInvokeResult.Fail`); `ApplyProductionOutputPreferences` PRUNES any
    persisted non-`zoom:` `SourceDropoutPolicies` key on load (a pre-fix-wave
    file, or a hand edit, cannot resurrect a capture policy).
  - **R3 — black is a PROGRAM-only cut.** Preview and multiview keep the HELD
    frame regardless of the stored policy, so the operator can watch a stalled
    source for recovery on the monitoring surfaces even while Program has cut
    to black. `MediaCore`'s `holdDropoutForMonitoring(plan)` forces
    `dropoutPolicy = "hold"` on every layer of `buildPreviewCompositorRenderPlan`'s
    plan and of `buildMultiviewRenderPlan`'s plan (both the PGM/PVW mirror
    cells and the source tiles), applied AFTER `annotateLayerSource` so
    `sourceHealth`/`sourceDisplayName` are still correct on those planes —
    only `buildCompositorRenderPlan` (the real Program render) reads the true
    per-source policy.
- **One resolution rule, identical in all three compositors, lives in
  `native/src/compositor/CompositorLayout.h`:** the constants
  `kWarmingSlateRgba = 0xff1b1f27` (neutral dark), `kFailedSlateRgba =
  0xff23181c` (dark, faintly warm so failed reads distinguishably from
  warming), `kDropoutBlackRgba = 0xff000000`, and the two pure functions
  `slateColorFor(sourceHealth)` (`"failed"` OR `"stalled"` → `kFailedSlateRgba`
  — R4, final review: a source with nothing to hold reads the SAME failed
  slate whether the bus calls it failed or stalled; `"warming"`/anything else
  → `kWarmingSlateRgba`) and `blackOnStalled(health, policy)` (`stalled` +
  `black` → true). `colorFromParticipantId` STAYS (Tiles membership + some
  tests still use it for distinct-source identity over REAL frames) but no
  layer-resolution path may call it any more — every compositor
  (`ProgramFramePreview`, `D3D11CompositorAdapter::resolveLayers`,
  `MetalCompositorAdapter`) and every layer kind (participant-video,
  media-video, the legacy positional fallback, the empty-render-plan grid
  fallback) now resolves through `slateColorFor`/`blackOnStalled` alone. The
  D3D11 failed-slate name label (`drawFailedSlateName`) also draws for BOTH
  `"failed"` and `"stalled"` with no frame, matching the colour rule.
- **The wire: `set-source-policy`** `{sourceId, dropoutPolicy: "hold"|"black",
  displayName?}`. The core keys by the RAW frame key it looks up at plan
  time — `zoom:<pid>` has its `zoom:` prefix stripped to the raw pid — and
  rejects (loudly, scene-validation warning) any `sourceId` that isn't
  `zoom:<pid>` when a `dropoutPolicy` is present (R2), or any policy string
  that isn't `hold`/`black` for a valid zoom id, in both cases leaving the
  existing value unchanged; `displayName` is accepted for any id regardless.
  `sources[]` echoes `dropoutPolicy` + `displayName` per source. The shell
  RE-SENDS every persisted policy (or a source with only a known display
  name, so the core learns names for the failed slate) on every production
  sync — present-or-keep, the same one-shot-command-must-be-re-applied rule as
  `configure-multiviewer` above, since the core can respawn and lose anything
  sent only once.
- **Shell:** a per-source "On dropout" ComboBox (Hold last frame | Black) on
  the Sources page for Zoom guest rows ONLY (R2: the capture-row combo is
  removed) — persisted in `ProductionOutputPreferences.SourceDropoutPolicies`
  (prefs **v13**; v12→v13 migrates to an empty map, and load now prunes any
  non-`zoom:` key), and a control action `source.dropout.set {sourceId,
  policy}` that refuses a non-`zoom:` id. `StudioViewModel.BuildSourcePolicyWires`
  (R5) is a thin forwarder to the pure `ProductionStateHelper.BuildSourcePolicyWires`
  (participants + capture devices + policies + display-name overrides →
  wires), which is what makes it unit-testable without constructing the
  (non-constructible-in-tests) view model — it names EVERY current Zoom
  participant and capture device with its RESOLVED name (override, else
  roster/device-derived), not only ids that already had an override or an
  explicit policy.
- **Metal is colour-only, deliberately.** The D3D11 failed slate carries a
  small name label via the existing overlay text raster (bottom-left band, not
  a full band — the raster never paints a background plate behind it, see the
  `drawFailedSlateName` comment fix below). Metal renders the correct slate
  COLOUR but not the name text yet — `TODO(4a-metal-text)` at the site — because
  `rasterOverlayTileCoreText` did not compose cleanly into the failed-slate
  path in ≤ 30 lines; CI-only (`native-metal-macos`), so this has not been
  rig-verified on a real Mac.
- **Task 4 gate numbers (2026-09-19, first cut):** Windows dev suite 1075/0,
  stub gate green, `validate-multiview.mjs`/`validate-tiles.mjs` PASS, show
  drill PASSED (60fps/100%/0 dropped). Superseded by the final-review numbers
  immediately below — the first cut's `zoom-gap-hold-ab.py` runs used the
  harness's original ~450ms DROPOUT window, which is now SHORTER than R1's
  1.5s on-air hysteresis, so that "black held ~300ms" result no longer applies
  once R1 landed (a `--policy black` run at the default gap never reaches
  "stalled" at all now, correctly — see below).
- **Final-review fix wave gate numbers (2026-09-19, R1–R6):** Windows dev
  suite `native/build-dev/corevideo-native-tests.exe` — 1080 tests passed, 0
  failed. `dotnet test`: MediaCore.Tests 2228/0, Control.Tests 74/0, WinUI.Tests
  1510/0. `dotnet build native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj
  -c Release -p:Platform=x64` — 0 errors. `scripts/qa/zoom-gap-hold-ab.py`
  gained a `--gap-seconds` flag (default 0.45, matching the original window)
  because R1's 1.5s on-air window made the old fixed ~450ms DROPOUT phase too
  short to ever observe a black cut. Two runs, archived under
  `artifacts/qa/slice4a/`: `--label s4a-hold-v2 --policy hold` (default
  `--gap-seconds 0.45`) — luma held 187.5–204.7 across the WHOLE recording
  including the dropout window (no dip — correctly under the 1.5s window, so
  the stalled+black rule never engages); `--label s4a-black-v2 --policy black
  --gap-seconds 2.5` — luma held ~199–205 for the first ~1.3s of the gap (bus
  health still short of the 1.5s window), THEN dropped to EXACTLY luma 16.08
  for ~1.35s once bus health crossed to on-air "stalled", and recovered to
  ~199+ immediately after RESTORE — exactly the R1/R3 behaviour: black is a
  genuine on-air cut that respects the 1.5s hysteresis, never the ~200-450ms
  window the diagnostic health alone would suggest.
- **Known follow-ups, not gaps in scope:** the multiview tile now shows the
  core's own bus-health name label UNDER the shell's existing XAML tile label
  when a tile is failed — this stacks two labels and has not been eyeballed
  live yet (headless pixel oracles cannot judge text overlap); a long
  `sourceDisplayName` clips at the D3D11 label's 40%-of-rect width cap rather
  than truncating with an ellipsis; Metal text (`TODO(4a-metal-text)` above);
  media/still sources have no "warming" desired-set wiring yet (R1 only covers
  Zoom vs everything-else, not a richer per-kind warming signal); capture
  dropout liveness/policy is gated on wiring `signalPresent` into a real stall
  decision (R2 follow-up, tracked as [#562](https://github.com/iamfatness/CoreVideoPro/issues/562));
  the sourceHealth/dropoutPolicy strings are
  allocated per layer per tick rather than interned; a non-`hold`/`black`
  `dropoutPolicy` string still only warns rather than refusing the whole
  command; a long `sourceDisplayName` on the multiview tile has not been
  live-eyeballed for the double-label stacking noted above.

**Slice 3b (merged 2026-09-21 in
[#567](https://github.com/iamfatness/CoreVideoPro/pull/567)): media
play/pause/cue state lives in the CORE.** Spec:
`docs/superpowers/specs/2026-09-20-source-bus-slice3b-media-source-state-design.md`.
Slice 3a put media FRAMES on the bus but left the decoder request-driven from the
render plan every tick and the play state decided in the shell. 3b makes a media
asset ONE core source owning its decoder, its clock and its transport state, and
closes the MEDIA half of #449.

- **What moved.** `core::MediaTransports` (`native/src/core/MediaTransports.h`,
  evolved from the now-deleted `modules::OwnedMediaFrameSource`) owns one `Entry`
  per source id: the decoder instance, its worker thread, the presentation queue,
  the audio queue, the warnings and the transport state.
  `MediaCore::syncMediaTransportsDesired()` computes a DESIRED SET at COMMAND
  time from BOTH scene graphs — the `syncStillMediaDesired()` shape, run at the
  end of `loadSceneGraph` and `applyPreviewScene`, on the command thread, never
  on a render or audio tick — and hands it to
  `MediaTransports::apply(desired, nowNs)`, which returns the bus membership
  changes. Ids are unchanged: `media:<assetId>` for a route,
  `background:<assetId>` for a scene background, so the same file behind a scene
  and routed as a clip is still TWO sources (a loop role and a clip role).
  `native/src/core/MediaTransportPolicy.h` is the PURE transition table (the
  `CaptureReaderStallPolicy`/`TakeRecordPolicy` shape — decoder-free, one test per
  row). `MediaAssetSource` wraps an entry on the bus, so media video rides the
  ORDINARY EARLY bus ingest alongside Zoom and capture (3a's post-plan media poll
  is gone; only stills still ingest at their own later point), and media audio is
  drained on the audio worker with no render plan built for it at all.
  **Corrected for #583 (2026-09-21, merged after 3b):** 3b popped media PCM
  straight out of `MediaTransports::popAudio(nowMs)` into the mix. Media PCM now
  crosses the bus like every other kind — `core::ingestSourceAudio`
  (`native/src/core/SourceAudioIngress.h`, called once from
  `MediaCore::gatherAudioOutputWork`) drains `popAudio` and STAGES each frame onto
  its `MediaAssetSource`, and `SourceBus::ingestAudio` is what emits it, counting
  `audioPacketsIngested`/`audioSamplesIngested` per source on the way. The staging
  step refuses PCM whose id has no owned kind-`"media"` transport
  (`[source-bus] media PCM without owned transport`), because bus membership is
  command-owned and a stray packet must never resurrect a retired decoder.
  `ModuleSet::mediaFrames` became
  `ModuleSet::mediaDecoderFactory`; `IMediaFrameSource`/`IMediaVideoPrefetch`
  survive ONLY as the per-entry DECODER contract (retired in slice 4b).
- **The transitions, stated as rules** (`decideMediaTransport`, decided against
  the PREVIOUS desired row — never a clock, never a generation string): a source
  that appears on Program (or any loop) opens and **rolls from 0 with audio**; one
  that appears on Preview only opens a **poster at 0**, silent (`Cued`); **a cued
  clip entering Program RESUMES THE SAME DECODER** — this is what replaced the
  #449 cue hand-off, and there is nothing left to re-key because a cue and its
  live clip are now one id and one entry; a clip that LEAVES Program while still
  cued gets a new decoder at 0 **behind the held frame**; a clip that STAYS on
  Program across a Take does **nothing** (it never left Program, so it is not a
  go-live); a path or loop-flag change on the same id is a Reopen; absent from
  both buses is a Release; and an IDENTICAL desired row is a **no-op**, so the
  repeating sync and a respawn re-apply cost nothing. A LOOP is live on any bus,
  never pauses and never restarts on a Take: `decideMediaOperator` refuses
  pause/play for a loop — and for anything not on Program — with a
  scene-validation warning and no state change.
- **`Ended` is DECODER EVIDENCE, and it is RECOVERABLE IN PLACE.** It comes from
  `IMediaVideoPrefetch::mediaEnded()`; the MF adapter implements it off its own
  `state.ended` and NEVER reports it for a loop (a loop reopens at EOS behind its
  last good frame, so `ended` is a transient there). A 6 s no-new-frame window
  survives only as a BACKSTOP for a decoder that cannot say, and a clip cannot end
  before it has produced a frame. A new frameId returns an Ended entry to Live
  keeping its position, so the decoder layer reports Ended as still PLAYING and
  the worker keeps polling — the stated price is that an Ended clip's worker runs
  its 20 ms loop instead of idling, bought deliberately because a worker that
  stops reading can never see the frame that proves the stall is over.
  **This is the trap, and it nearly shipped:** the first cut inferred Ended from a
  **500 ms frame-arrival heuristic** and made it TERMINAL. That is shorter than a
  cold Media Foundation open (95-250 ms) plus an FFmpeg spawn, shorter than one
  rung of the FFmpeg resume ladder (250 ms / 500 ms / 1 s / 2 s), and shorter than
  an ordinary hiccup on a loaded box — and the presentation queue is only 3 deep
  (~50 ms at 60 fps), so `queued() == 0` is the ordinary state of a HEALTHY clip.
  On air it would have frozen and silenced a Program clip whose only exit gesture
  restarts it from 0. No test could see it: every fake decoder answers instantly.
- **`Release` has a 750 ms grace, and a source inside it is published on NEITHER
  bus.** `applyPreviewScene` rides the REPEATING spine sync as well as the
  production sync, so ONE spine tick carrying the post-swap Preview while Program
  still holds the outgoing scene leaves a CUED clip absent from both desired sets
  for that single tick; retiring on the spot destroys the warm decoder the Take is
  about to claim, and 3b deleted the hand-off that used to be the second chance.
  The grace is swept by a later `apply()` **and** by a render-tick
  `collectExpiredReleases()` — `apply()` only runs at command time, so without the
  sweep a source nothing mentions again would hold its decoder forever. The
  desired row is carried forward so a re-claim is an ordinary transition, and the
  audio goes silent IMMEDIATELY (a clip genuinely cut off Program must not keep
  feeding the mix for the length of the grace). **And `snapshot()` publishes a
  source in its grace with `onProgram`/`onPreview` FALSE.** That is a READ-SIDE
  projection only and must stay one — it can never reach `decideMediaTransport`,
  which reads `Entry::desired`. Without it an ordinary Take left the row reporting
  `onProgram: true` for up to three 250 ms polls, `MediaBinPlaybackProjection`
  (which gates its whole selection rewrite on `OnProgram`) re-set
  `SelectedMediaAssetPlaying = true`, and the wrong value LATCHED once the row
  finally went: the operator's toggle read "Pause Program" for an off-air clip and
  the core refused the gesture. Same family as the T1.3 rollback defect above.
- **Threading.** Lock order is `coreMutex` -> `MediaTransports::mutex_` ->
  `Entry::mutex`, never reversed. `apply()` starts worker threads (outside the
  owner lock) and **never opens a decoder inline** — the worker opens, exactly as
  the retired `OwnedMediaFrameSource::run` did, and a restart is a flag the worker
  acts on. The 16-decoder cap and its rate-limited `[media-decoder]` refusal moved
  with the code, and a refused open is retried on the next apply rather than
  remembered as permanent. `mediaTransports_` is **declared after `sourceBus_`**
  so it is DESTROYED FIRST: every kind-`"media"` bus source holds a `shared_ptr`
  to one of its entries and the transports' destructor joins the decoder workers.
- **The wire.** New one-shot **`set-media-transport {mediaAssetId, action:
  "pause"|"play"}`** — a GESTURE, not desired state, so it is deliberately **not**
  re-sent per sync (a core respawn reloads the scene and rolls the clip from 0,
  unchanged from today) and it never calls `syncMediaTransportsDesired()`,
  because pause/play moves a transport's STATE and never which sources are on the
  bus. `set-media-playback` is **selection only**. A route's `mediaPlaybackKey` /
  `mediaAssetPlaying` and a scene background's `playing` are **IGNORED, never
  refused** (an old shell against a new core plays clips by scene membership and
  loses shell-side pause until it updates; a new shell against an old core has no
  transport command and is refused loudly by the unknown-command path). Snapshot:
  a new **`mediaSources[]`** node published unconditionally (the multiviewer-node
  rule; an empty array is the honest answer to "no media is routed") carrying
  `{sourceId, mediaAssetId, state, loop, positionMs, durationMs, onProgram,
  onPreview}`, and `mediaPlayback.status`/`playing` now report the SELECTED
  asset's real transport state over the vocabulary
  `idle | cued | live | paused | ended | unavailable` — `unavailable` meaning
  selected but on neither bus, which a stale shell flag used to report as
  "paused". `mediaPlaybackKey` survives as an empty string so an older reader
  finds a value rather than a missing key.
- **The shell half.** No playback keys, no go-live generations, no operator-paused
  set. `MediaRoutePlaybackService` keeps only what the shell still decides: which
  asset a route names, `IsLoopingAsset`, `AssetsEnteringProgram` (the pure set
  diff that replaced `MediaGoLiveLedger.RecordTake` — it stores nothing),
  `ChooseAssetToPromote`, `ResolveTap`, and `IsPlayingOnAir`/`ResolveTransportRow`
  read back from the core's rows. `MediaBinPlaybackProjection` is the WHOLE
  snapshot-apply decision as one pure function — the "test the whole decision, not
  the leaf" rule, and the only way to pin it while `StudioViewModel` is not
  constructible: it adopts the core's flag for the selection **only while that
  asset's row is `OnProgram`**, because a clip cued in Preview has a row too
  (state `cued`) and adopting it switched off an audition the operator had started
  <=250 ms earlier. A **`media:` row beats a `background:` row** for an asset's
  tap/played state, and that ordering is load-bearing: a background is a loop, so
  its row is permanently `live`; letting it answer showed a cued or paused clip as
  playing, made `ResolveTap` return Pause, and sent a pause the core correctly
  refuses for a loop — the UI flipped and nothing on air moved.
  `MediaTransportGestureLedger` makes the gesture **latest-wins per asset**: a
  claim token checked immediately before EVERY send, attempt 0 included (the first
  cut gated only the retry SCHEDULING, so a stale pause already scheduled still
  landed after a newer resume), with a bounded re-arm and a VISIBLE give-up.
- **Deleted, so nobody hunts for them:** `OwnedMediaFrameSource`,
  `MediaCueHandoff` (with `adoptCuedDecoders` and `Entry::everPlayed`),
  `MediaGoLiveLedger`, `BuildSceneMediaPlaybackKey` /
  `ResolveSceneRoutePlayback` / `ShouldPlaySceneMediaRoute`, the
  `preview:media:<id>` namespace and the `pausedClipCue` re-key in
  `buildPreviewCompositorRenderPlan`, and `ModuleSet::mediaFrames` (now
  `ModuleSet::mediaDecoderFactory`).
- **Gate numbers — WHAT THE BRANCH MEASURED AT MERGE (2026-09-20, on the merged
  head `45644587`), not a claim about main today:** full Windows dev suite
  **1113 passed / 0 failed**; stub gate
  `scripts/test-native.ps1` exit 0, **100% passed**; `dotnet test` MediaCore
  **2232/0**, Control **74/0**, WinUI **1519/0**, and the WinUI Release x64 build
  **0 errors**; `node scripts/validate-multiview.mjs` PASS and `node
  scripts/validate-tiles.mjs` PASS (10/10 checks);
  `scripts/qa/zoom-gap-hold-ab.py --label s3b-hold` — 248 frames, Program luma
  held **187.5-204.7**, 0 frames below 150, take record verdict `cut` with
  `restartedSources=[]` and `missingSources=[]`; show drill
  (`COREVIDEO_FAKE_ENGINE_FPS=60`, `mac-show-drill.py --seconds 40 --load 8`, on a
  quiet box) **PASSED** — 100% of decoded frames delivered, source->render p50
  **26.9 ms** / p99 **37.7 ms**.
- **The new live oracle is FALSIFIABLE, which is the whole point.**
  `scripts/qa/media-take-ab.py` cues a clip in Preview, records Program, Takes,
  and judges the MP4 with `ffmpeg signalstats` YAVG: the first Program frames
  after the Take must carry the clip's own first-frame luma (never a slate), and
  the luma must then MOVE. Run after the Ended/Release fix wave (`--label
  s3b-final`): **PASS** — take frame #54 at YAVG 85.00, all 15 post-take frames
  85.00, rolling window 49/49 at 125.6, 251 frames at 60.23 fps. Its
  **`--skip-cue` control** — the same Take with the clip never cued, i.e. exactly
  the #449 step 1 cold cut — **FAILS (exit 1)** with 4 warming-slate frames at
  YAVG 42. A run that passes with `--skip-cue` is judging nothing. (Calibration
  worth keeping: the slate lumas quoted elsewhere in this file are FULL-range, and
  a limited-range recording carries them as 42 warming / 39 failed; the fixture is
  neutral grey 85 because the spec's `0x10A0F0` read 116 against `testsrc2`'s 126
  — too close to separate.)
- **What is NOT done, honestly.** Nothing here has run against a real Zoom meeting
  or the real WinUI app: there is **no live cue / Take / pause / resume gate**.
  **#449 step 1 — hold the outgoing picture on a COLD cut, a clip that was never
  cued — is still open**, and the `--skip-cue` control measures exactly that gap.
  The 750 ms release grace is REASONED, not measured against the real spine
  cadence. And only the MF adapter implements `mediaEnded()`, so the stub and
  macOS decoders fall back to the 6 s backstop.
