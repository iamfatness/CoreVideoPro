# Other gotchas

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

- **A one-apply roster dip must not reflow the multiview (#725, 2026-09-30).**
  - **What happened:** in a long live session, every Take resolved Show Input slots against a
    participant list missing the producer (the app's own camera-off user) for one apply. The
    multiview layout lost a tile and the wall reflowed 5+4 to 4+4 and back on every cut. It
    did not reproduce after an app restart.
  - **Root cause (found 2026-10-01): a core the supervisor RESPAWNED is a new roster authority,
    and the shell rejected it.** The roster epoch is `process:meeting:token`. A new core process
    starts again at `1:1`, and only the diagnostic token differs, so
    `ZoomRosterSnapshotPolicy.Accept` could not order it after the dead core's barrier. That
    policy relies on "a core restart clears the bridge snapshot". `StopCore` did; the
    supervisor's own crash respawn did not. Consequences, for the rest of the app session:
    - The bridge snapshot kept the DEAD core's roster. Joins, leaves and the producer's new
      Zoom id never reached the shell, and slots kept pointing at ids no longer in the meeting.
    - A Take applies its own raw sync response, which carried the true roster, and the next
      bridge snapshot put the stale one back. That alternation was the dip.
    - Measured live: bridge `1:1:18772-0 rev 38 n=8` against core `1:1:23540-0 rev 11 n=9`.
  - **Fix:** `MediaCoreBridgeService.ReleaseRosterBarrierForRespawnedCore` drops the barrier
    (epoch and revision only, participants kept) at the respawned core's handshake, keyed on the
    supervisor restart count. Test: `RosterAfterCoreRespawnTests` (a fake core that dies and
    comes back on a same-numbered epoch at a lower revision).
  - **Still in place:** `MultiviewParticipantGrace` holds an in-show slot's participant for 1 s
    through a dip; a real leave is dropped after the grace.
  - **Log lines:** `mv-roster-miss: slotN pid=... missing from <caller> list ...
    coreRosterHasIt=yes|no` (not for an empty pre-join roster), and `roster-drop: pid=...
    origin=<apply path> ... snap(epoch= rev=) bridge(epoch= rev=)` whenever someone leaves the
    shell's roster. Two different epochs on that line is this defect; one epoch is a real leave.
  - **Rule:** "it only happens in a long session" usually means "after an event the session
    eventually has". Read `media-core.log` for a respawn before theorising about accumulation.
- **AN EMPTY RENDER PLAN IS NOT "DRAW NOTHING" (2026-08-15, CoreVideo Tiles T1).**
  All THREE compositors — `D3D11CompositorAdapter::resolveLayers`,
  `ProgramFramePreview`'s `buildProgramFramePreview`, and
  `MetalCompositorAdapter::resolveLayers` — carry their own `renderPlan.layers.empty()`
  fallback that improvises **one full-canvas grid cell per DECODED FRAME**. So any
  scene path that legitimately produces zero layers puts a grid of whatever the core
  happens to be decoding onto PROGRAM — sources that are not in the scene at all —
  and PROGRAM is inherited by the virtual camera, every recording and every stream.
  The Tiles wall hit this exactly: `buildRenderPlanForScene` suppresses the legacy
  full-canvas fallback whenever a wall is active, and the wall's background layer was
  emitted *inside* the `!admitted.empty()` gate — so a wall whose members were all
  stale (and, transiently, EVERY Tiles take before first frames land) shipped an empty
  plan. **Rule: any code path that owns a scene's video layers must always emit at
  least one layer.** The wall's background `push_back` now sits above the admission
  gate; regression test `TilesRenderPlan.AnAllStaleWallStillEmitsItsBackground`
  asserts the plan is non-empty, not just that the tiles are absent.
  **And the gate deciding "is a wall active" is `wall.present` ALONE — never
  `present && !members.empty()`.** `TilesLayerPayloadBuilder.Build` sends
  `members: []` whenever every guest is video-off or the roster is momentarily empty
  (an ORDINARY meeting state), so a members-aware gate re-opened the identical
  on-air hole one level up: no background AND no fallback suppression, i.e. an
  improvised grid on PROGRAM the moment all cameras went off. A configured wall
  with nobody live shows its BACKGROUND. The same `.present` gate is used by the
  `lastRenderPlan_` cache and the snapshot `tiles` node so they can never disagree
  (and so the all-cameras-off state stays OBSERVABLE — a node that vanishes in the
  case worth detecting is the multiviewer mistake again). The wall also **counts as
  a layer in `hasPreviewScene()`**: that tally was routes + background + overlays
  only, so a Tiles preview scene with no media background and no overlay scored
  ZERO, the third composite never ran, the preview shared-texture handle was
  cleared, and the operator's preview monitor silently fell back to the
  single-source path — never showing the wall it was about to take. Tests:
  `AMemberLessWallStillOwnsTheSceneAndEmitsItsBackground`,
  `APreviewSceneCarryingOnlyAWallStillComposites`.
  Related, same family: **routes and the wall share ONE order namespace** (tiles-bg at
  `wall.order`, tile #i at `wall.order + 1 + i`), so a surviving gallery route at order
  2 composites between tiles. The shell keeps that impossible **by construction** —
  `StudioViewModel.BuildProductionSyncContext` serializes an EMPTY route list for a
  `DynamicGallery` scene, at the point the wire is built, never relying on the
  coalesced UI reconcile pass (`ReconcileDynamicGalleryRoutes`) having run. The
  core still ACCEPTS routes+wall from any producer, so both scene parse sites now
  push a deduped `sceneValidationWarnings_` entry when they arrive together —
  audible drift, not latent. (Dedupe matters: `applyPreviewScene` rides the
  REPEATING spine sync and does NOT clear that vector, so an unconditional push
  there grows without bound on a scene-flip loop.)
  And **Metal has no `hasFillColor` branch** — the wall background renders as a
  dark-grey slab on macOS; named in a comment at the site, owned by
  `docs/corevideo-tiles-iso-scaling-plan.md` implementation slice 3 (Metal parity).

- **A TILES WALL TAKEN FROM PREVIEW IS CUT TO, NEVER REDRAWN (owner report,
  live show 2026-09-09).** "I am ok if panelists leave and join the video but
  what I can't have is a total rerender from what is in preview to program like
  it is loading for the first time." The wall key is `sceneId + ":" + layerId`
  and the layer id is derived from the scene id, so the SAME gallery has the
  SAME key on both buses. **This was first fixed with a HAND-OVER and is now
  fixed STRUCTURALLY — the hand-over is DELETED. See "ONE ANIMATOR PER WALL"
  below; `TilesPlanAnimation::adoptSettledFrom` no longer exists.** The original
  shape: `MediaCore` held two animation objects
  (`programTilesAnimation_` / `previewTilesAnimation_`) and the program one reset
  its animator the moment the key it had never held arrived. `adoptSettledFrom()`
  moved a SETTLED wall's state across on the take tick — settled only, because
  with two animators mid-flight state had no correct owner. The second
  correction survives and still matters: `advance()` does not sample an EMPTY
  target set for a wall that is still present and has already drawn tiles. That
  is what produced the reported rebuild — an all-stale beat
  (`kTilesStaleFrameMs`, an ordinary state — see the empty-plan rule above)
  erased every retained tile AND consumed the animator's adoption.
  **What a reset actually looks like, because the direction is counter-intuitive
  and the docs had it backwards: it does NOT replay from alpha 0. `TilesAnimator`
  treats a reset animator's next non-empty `sample()` as an ADOPTION (content
  already present, not entering), so the wall SNAPS TO ITS FINAL STATE** — alpha
  pops to 1, mid-spring rects jump to their settled positions. On air that is a
  wall that stops moving and jumps, which is what "loading for the first time"
  looked like. The practical consequence for tests: `EXPECT_GE(after, before)` on
  alpha is satisfied by a snap just as well as by continuity and therefore
  catches NOTHING — a falsifying assertion has to bound the other side (alpha
  stays below 0.9, rects stay near their mid-spring values). A COLD wall's first
  tick is untouched, so a wall that was never in preview behaves as before. Not a contributor, measured: preview and
  program share one device and one `sourceTextures_` cache keyed by
  `participantId` (`D3D11CompositorAdapter`), so tile textures are already warm
  across a take. Tests: `TilesRenderPlan.AWallSettledInPreviewIsAlreadySettledOnItsFirstProgramFrame`,
  `AWallTakenWhileItsFramesLapseIsStillCutToNotRedrawn` (fails without the fix),
  `AWallThatWasNeverInPreviewIsHandedNothing`, plus three `TilesAnimator.*`
  hand-off unit tests.
  **The wall's LIVE BACKGROUND had the same defect and needed a different fix
  (same show, follow-up report: "Tiles background still refreshing on cut to
  program, that should be seamless").** The wall emits TWO background layers:
  `tiles-bg:<layerId>`, a sourceless solid, emitted unconditionally above the
  admission gate (safe — it depends only on `!sceneBackground.enabled`, which is
  parsed from the scene payload and cannot move across a take); and
  `tiles-source-bg:<layerId>`, the live background FEED, which rode
  `admitTilesMembers` — the SAME 1500 ms `kTilesStaleFrameMs` gate the tiles go
  through. So one beat of the background source's frameId not advancing dropped
  the layer entirely, program fell through to the solid colour or the scene
  background, and the picture popped back when frames resumed. The gate is now
  `compositor::tilesBackgroundSourceIsDrawable`, which asks the question that
  applies to a BACKDROP instead: is a real-content frame for this source in this
  tick's gather (`TilesMemberFrameAge::hasFrame`)? A stale TILE is still refused
  — it occupies a slot, and holding it seats a dead guest — but a stale
  BACKGROUND competes with nothing, and a backdrop frozen for a beat is
  indistinguishable from a live one where its absence is a full-frame colour
  change on air. `kTilesStaleFrameMs` is deliberately NOT widened: it is shared
  with tile admission and moving it changes wall membership for every source.
  The hold is EVIDENCE, not memory — there is no retained layer and no per-bus
  state, so a source that never arrived or has departed keeps today's behaviour
  exactly and the two buses cannot contaminate each other through it. That
  matters concretely: a `participant-video` layer whose sourceId resolves to no
  frame renders a solid `colorFromParticipantId()` slab OVER the wall background
  (`resolveLayers`), which is worse than the pop; and the layer always carries a
  non-empty participantId, so `RouteSourcePolicy`'s positional-fallback hazard
  stays unreachable. Tests: `TilesRenderPlan.AWallsLiveBackgroundSurvivesATakeAcrossAStaleBeat`
  (the live wall harness, using the new `LiveWallCaptureDevice::freeze()` — a
  frozen feed still DELIVERS with a held frameId; `pause()` is the harsher
  no-frame case) and `AStaleBackgroundIsHeldButAnAbsentOneIsNeverFabricated`.
  Both fail without the gate change.
  **A SECOND, ENGINE-SIDE CONTRIBUTOR EXISTED; IT IS FIXED (#478, 2026-09-11,
  plus fix rounds 1 and 2).** It was: `ZoomMediaSpinePayloadBuilder` ordered video
  candidates active-speaker, program routes, preview routes, then EVERY participant
  in roster order, capped at `maxVideoSubscriptions`; and the core picked resolution
  as `purpose == "active-speaker" ? 1080P : 720P`, with resolution in the dedup key.
  So every speaker change rebuilt two engine renderers, a Tiles scene's EMPTY route
  list reordered the list on a take, and camera-OFF early joiners took the cap ahead
  of a late wall guest (live, 12-person meeting: Alexander, slot 2,
  `subscribed:false`, generation 14, froze whenever he left Preview/Program; three
  camera-off non-wall guests held live subscriptions; `totalChurn` 53->59 in 20 s
  with the owner seeing Tiles "flashing"). Now:
  **(1) ONLY SOURCES ARE SUBSCRIBED (owner rule: "Why are you grabbing sources I
  don't have routed to the multiviewer?").** `MediaCore/Services/ZoomSourceSetPolicy.cs`
  is the ONE pure decision of who is a source, in budget order, PROGRAM FIRST:
  Program routes -> Program Tiles members (+ Zoom wall background) -> Preview routes
  -> Preview Tiles members -> in-show wall slots in slot order -> ISO-armed guests
  (only while "Program + ISOs" is on) -> sticky Tiles audio members. What is on air
  is never disturbed by a cue: an off-air Preview look can never take video (or the
  1080P grant, which the core makes in this same order) from a Program source; a cued
  guest the budget leaves out is named on the multiview PVW cell instead. (Round 1
  briefly ranked Preview routes above Program Tiles; the re-review withdrew it — it
  spent on-air pixels on an off-air cue.) No roster fill, anywhere. VIDEO =
  camera-on sources, capped at 10; AUDIO (`participant-audio`, purpose "mix") = every
  source including camera-off ones, uncapped (owner ruling "sources only": a
  non-source is NOT subscribed and is inaudible in Program, the hardware-switcher
  model); `meeting-audio` (the programMix path) is unchanged. **Tiles audio is
  sticky** (`TilesAudioSourceLatch`), keyed by SCENE: a Tiles member stays an audio
  source while their scene is on EITHER bus, so a panelist who turns their camera off
  (the membership policy drops them) keeps talking on air — including across the Take
  that swaps their gallery from Preview to Program. It forgets a scene on neither bus,
  a participant who leaves, and EVERYTHING on a KNOWN "not in a meeting" or on Engine
  off — a tick whose meeting state is UNKNOWN (no/synthesized snapshot) never clears it (Zoom
  reuses per-meeting user ids; a latch that outlived the meeting would make a different
  person audible). A never-on-camera participant is never a member, so never audible.
  `StudioViewModel.BuildSpinePayload` only plumbs the Tiles layers, ISO ids and the
  latch in, and maps the live roster's `VideoOn` into health (it used to pass raw
  NetworkQuality, so every camera-off guest read as video-on).
  **(2) THE SPEAKER DIRECTOR FOLLOWS ONLY SOURCES** (`ZoomActiveSpeakerDirector::setSourceFilter`,
  fed from the payload's `sourceParticipantIds`). Without this, sources-only
  DEADLOCKED speaker-following: the director only promotes a challenger with fresh
  frames, a non-source never has a subscription, and the meeting's first talker
  (vacancy fill, no freshness check) held the directed slot for the whole show. A
  non-source who talks is ignored for direction; an incumbent that stops being a
  source is released. A follow-speaker (`active-speaker` mode) route adds nobody,
  moves nobody's budget position and GRANTS NO PURPOSE (round 2, N1: round 1 gave the
  speaker the bus's 1080P purpose, which rebuilt two renderers — often on-air Tiles
  tiles — on every speaker change, i.e. #478's flashing driven by talk again). **Known
  limitation: a follow-speaker shot is 720P** (the speaker's own wall/Tiles/ISO tier)
  until an in-place resolution change is proven on a live renderer. **The core
  RENDERS a follow route as the directed speaker, FRAME-VALIDATED**
  (`RouteSourcePolicy` `directedSpeakerParticipantId`, `MediaCore::followSpeakerForRoutes`,
  pure `core/FollowSpeakerHold.h`): the directed speaker if they have a content frame
  THIS tick, else the most recent previously directed speaker who does, else NOBODY —
  the layer renders EMPTY (a transparent fill, opacity 0). Binding a frameless id
  painted the `colorFromParticipantId` slab on Program (the director keeps a departed
  incumbent 60 s; a speaker dropped from the sources mid-talk loses their video in the
  same tick), and an unbound layer paints grey. The history is scoped to one meeting
  by `ZoomEngineRuntime::speakerEpoch()` (moves on join, leave, Engine off and every
  new engine process), so a reused Zoom id from the last meeting is never bound. It
  used to take the positional fallback `videoFrames[routeIndex]` (uuid order), so a
  speaker scene showed the lowest-pid source. #480 removed that fallback for every
  mode: a route with no source id renders BLANK (transparent fill), including an
  emptied OHG box.
  **(3) RESOLUTION IS A STABLE TIER, CAPPED, NO RATCHET.**
  **AMENDED 2026-09-20 — EVERY IN-SHOW CAMERA IS 1080P, NO PURPOSE IS 720P (owner
  ruling: "All sources should be pulling at the highest available for zoom. We
  shouldn't only pull a 720 until they are in preview; that doesn't work once we do
  a cut").** The report was "a color shift in the preview window when selecting a
  source", brief, then settling. Measured live on the engine SHM
  (`video-series-probe.py`, mean Y/U/V per frame, all four guests, the owner cueing
  on cue): every cue flipped that guest's ONE subscription between the 720P
  multiview tier and the 1080P Preview tier (churn 16-20 per guest that morning,
  reason `resolution-change`), and Zoom's 720P and 1080P encodes of the same
  camera differ in tone by the same amount every time, both directions (~+2.5 mean
  Y, ~-1.5 mean U at 1080P; both tiers span 0-255 with the same range-expansion
  comb, so NOT the limited-range defect, NOT a render-target format, NOT our
  shader — each ruled out by measurement). `wantsFullResolution` is now
  `kind == "participant-video"` for every purpose (multiview, iso and the
  follow-speaker's speaker included), so a cue or a Take changes NOTHING for an
  in-show source and the churn ledger reads zero on a cue
  (`SubscriptionChurnNamesResolutionChangesAndTeardowns` now pins that; the old
  "follow-speaker shot is 720P" limitation is gone with it). The 8-camera cap is
  unchanged and is now the ONLY thing that can still flip a guest: past 8 camera-on
  sources a cue re-ranks the budget and the 9th camera trades places with the cued
  one — two re-subscribes per cue, published as `fullResolutionDemoted`. That is
  said, not hidden; raising the cap still needs a bigger-meeting soak. The
  paragraph below describes the tiering as it stood before this amendment.
  `native/src/modules/ZoomSubscriptionResolutionPolicy.h`: FIXED bus routes (purpose
  program/preview), screen share, AND the Tiles wall (program-tiles / preview-tiles)
  at 1080P; multiview, ISO and a follow route's speaker at 720P. At most
  `kMaxConcurrentFullResolutionCameras` (**8**) cameras at 1080P, granted in payload order
  (Program routes first, then Program Tiles, then Preview routes); the rest get 720P
  and `zoomSubscriptionChurn.fullResolutionDemoted`
  counts them.
  **THE WALL WAS RAISED FROM 720P TO 1080P AND THE CAP FROM 4 TO 8 (2026-09-13,
  owner "whole wall at 1080p").** The report was "CoreVideo tiles participants are
  dropping as I go through different people in preview": a wall member was
  program-tiles (720P), and soloing them in Preview made them a preview route
  (1080P). Resolution is part of the engine subscription key, so that 720P->1080P
  was a real renderer teardown/rebuild of a source LIVE ON THE PROGRAM WALL — the
  drop, and the "few hundred ms then color changes" placeholder flash (one feed
  flipping, NOT two grabs: `m_subs` is keyed by participant_id, and every surface
  shares that one decoded frame). Pinning Tiles at 1080P removes the lower tier, so
  a soloed wall member has nowhere to flip to. The cap-4 came from commit bd3bedf/
  bd3ca (SIX concurrent 1080P crashed the SDK subprocess, 0xc000000d) on the
  **CPU-I420 path**; a live soak on today's GPU pipeline (2026-09-13, real 8-person
  meeting) ran all 8 wall members at 1080P on Program for 30+ min — 60fps delivery,
  0 underruns, no monitor shedding, no engine crash/respawn, `totalChurn` FLAT
  across an operator's full preview run (per-source churn = 1, the one-time take).
  8 is the soak-proven number and the meeting's camera count; **raising it further
  needs a bigger-meeting soak — never on extrapolation.** Graceful past 8: members
  are granted before the wall background in payload order, so a 9th 1080P source
  (a live wall background, a >8 wall, a non-wall bus route) is DEMOTED to 720P
  stably, never flips a member; screen share is 1080P and bypasses the camera
  counter (so 8 wall + a share = 9 concurrent 1080P total, one step past what was
  soaked — watch it if a full wall runs with a share). A guest who leaves the buses DROPS BACK to 720P: the engine
  used to ignore a lower request (`video_subscribe_noop_existing`), which over a show
  ratcheted every rotated guest to 1080P; it now rebuilds when the source is the
  renderer's only target (`zoom-engine/shared/engine-resolution-policy.h`). In-place
  `setRawDataResolution` was REJECTED: the SDK header only declares it
  (`h/rawdata/rawdata_renderer_interface.h:50`) and the engine only ever calls it
  before `subscribe()` (`engine-video.cpp:70`). **The unavoidable on-air
  re-subscribe (a cue raises, leaving a bus drops — never who is talking) is HIDDEN by
  holding the last frame:**
  `ZoomEngineRuntime::latestDecodedFrames_` is erased only when a source is RETIRED,
  so across a same-uuid re-subscribe the compositor keeps drawing the last decoded
  frame. A route layer holds it until the rebuilt renderer delivers (no staleness
  gate on routes — same as any stalled feed); a Tiles tile holds it for
  `kTilesStaleFrameMs` (1500 ms) and is then dropped from the wall until frames
  return. Pinned by `ZoomEngineRuntime.AResolutionReSubscribeKeepsTheSourcesLastFrameForTheCompositor`.
  **(4) LOUD, BUT MULTIVIEW-ONLY.** A camera-on source the cap leaves out goes in the
  payload's `videoSubscriptionShortfall` + `warnings`, and its multiview tile label
  reads "<name> · no video: subscription limit (10)". A BUS source with no wall tile
  (a cued Preview guest) is named on the PGM / PVW cell instead: the multiview layout
  carries `programNotice` / `previewNotice`, the core appends it to the cell label and
  the overlay draws "PREVIEW · no video: <names> (subscription limit 10)" (the tile LABEL
  is part of `VideoSurfaceCoordinator.MultiviewLayoutSignature`, so a label-only change
  reaches the overlay; the production sync's plain layout still blinks it for up to one
  spine tick after a user action). The
  multiview is the ONLY place a tile carries text: Program/Preview/Tiles are composited
  pixels. The
  subscription UUID is still `participant-video-<pid>-camera` (purpose excluded).
  Tests: `ZoomMediaSpinePayloadBuilderTests` (`LiveCase478_*`, the follow-route,
  speaker-flip-changes-nothing, at-the-cap, Program-first, sticky and breakout cases),
  `TilesAudioSourceLatchTests` (incl. the swap Take and the reused-id rejoin),
  `MagicSceneCoordinatorTests.AutomationCuesPreviewAtTheStartOfTheHold…`,
  `MultiviewOverlayFormattingTests.ResolveLabel_BusCellCarries…`; native
  `FollowSpeakerHold.*`, `TilesRenderPlan.AFollowSpeakerRouteWhoseSpeakerHasNoFrameRendersEmptyNotASlab`,
  `LeavingTheMeetingForgetsTheFollowRoutesHeldSpeaker`,
  `MediaCoreMultiview.TheBusCellsCarryTheShellsSubscriptionLimitNotice`;
  native `ZoomEngineRuntime.AnActiveSpeakerFlipCausesNoTeardown`,
  `ACueRaisesOnceTheTakeSendsNothingAndLeavingTheBusDropsBack`,
  `TheFullResolutionCapDemotesTheRoutesPastItAndSaysSo`,
  `ANonSourceWhoTalksFirstIsReleasedAndNeverDirected`,
  `ZoomEngineRuntimeState.DirectsOnlyAmongSourcesAndReleasesANonSourceIncumbent`,
  `TilesRenderPlan.AFollowSpeakerRouteShowsTheDirectedSpeakerNotTheFirstFrame`,
  `RouteSourcePolicy.AFollowSpeakerRouteBindsTheDirectedSpeakerNeverAPositionalSource`,
  `EngineResolutionPolicy.*`.
  **Costs, by the owner's rule (expected, not defects):** an unrouted guest has NO
  frames and NO audio anywhere — the Show Input / source pickers and the scene canvas
  editor show "Waiting" for them (roster thumbnails come only from subscribed
  streams), a non-source's mixer strip reads `waiting-for-pcm` (the mixer still lists
  every participant; wiring it to `ZoomSourceSetPolicy` is a follow-up after #481),
  and a scene built only on a follow-speaker route with nobody on the wall has no
  speaker to follow. Magic Scene now cues Preview at the START of its hold so the
  target warms before the auto-Take (a direct operator cut to a non-source still
  subscribes cold).

- **The scene canvas editor cannot show GPU video — DIAGNOSED 2026-08-15, NOT FIXED
  (a redesign is being specced separately; do not patch this ad hoc).** Owner report:
  "layer boxes show live video inconsistently". `VideoSurfaceHost` attaches a
  SwapChainPanel (and hooks its per-vsync present) ONLY when the surface key is
  `program`/`preview`/`multiview` or the kind is Program/Preview
  (`VideoSurfacePresentationRules.UsesGpuSharedTexture`). `StudioViewModel.ResolveLayerSurface`
  hands each layer a surface rewritten to key `scene-layer-N:<tileKey>` + kind
  **Multiview** — matching no clause — so `OnLoaded` early-returns and the core's
  per-source keyed-mutex export (`D3D11CompositorAdapter::exportParticipantTextures`,
  built expressly for this intermittent consumer) is DROPPED. The PREVIEW monitor works
  off the SAME tile only because `ResolvePreviewPrimarySurface` rewrites it to key
  `preview`/kind Preview. Net: editor layers render CPU BGRA only, so —
  media assets: always live (own player); Zoom guests: the 640x360 thumbnail at ~2/s
  (`kThumbnailEmitIntervalMs = 500`) and nothing while capture is unsubscribed;
  managed-bridge UVC cameras: smooth; **native-UVC / screen (WGC) / browser / SRT-ingest:
  placeholder forever** (only `CaptureDeviceFrameReaderService` fills
  `CaptureDeviceSurfaces`). Do NOT "fix" it by whitelisting `scene-layer` keys — that is
  N per-layer swap chains, the retired 0xc000027b pattern, and the per-source export is
  single-consumer keyed-mutex already claimed by the preview host. Characterization
  tests: `SceneCanvasLayerSurfaceTests`.
- **Borders are MULTIVIEW-ONLY — they NEVER composite into program/preview
  (owner rule, 2026-07-31).** Borders exist to separate tiles in the multiview
  (which sets its own explicit accent/program tally borders in
  `buildMultiviewRenderPlan`). Route borders used to default to "accent" (studio
  green, thickness 2 → `computeBorderFraming` ≈ thickness/200 → ~11–19px at
  fullscreen 1080p) and composited INTO THE PROGRAM — the virtual camera,
  recordings, and streams all inherit the composed program, so every output showed
  a green outline ("webcam out green border" regression). Now enforced in layers:
  `buildRenderPlanForScene` hard-forces `borderStyle="none"`/thickness 0 on every
  route layer (program AND preview bus), every default is "none", the Sources
  layer editor has no border controls, and `ScenePersistenceService.FromPersisted`
  retires stale persisted styles to "none". Regression tests:
  `MediaCoreCommand.RouteBordersNeverCompositeIntoProgram` (core — explicit
  "accent" composites identically to "none") and
  `ScenePersistenceServiceTests.DefaultRouteBorderIsNone` (shell). Never render a
  visible adornment on the program path outside the multiview grid.
- **A ONE-SHOT COMMAND MUST BE RE-APPLIED ON EVERY CORE GENERATION (2026-08-08).**
  The core is respawned by the supervisor whenever it dies, *under a live shell*.
  Anything the shell sends once at launch is **silently lost** on that respawn, and
  the fresh core answers with its DEFAULT — which is usually a legal value, so
  nothing looks wrong. This shipped as "the multiviewer is broken":
  `configure-multiviewer` was sent only by `StartMediaCoreOnLaunchAsync`, so a
  respawned core sat on `multiviewLayoutMode_ = "grid"` while the shell still
  believed `pgmPvwTop`. The **PROGRAM and PREVIEW bus cells vanished off the top of
  the wall** and it degraded to a bare source grid. It presented as FIVE separate
  bugs — buses gone, layout wrong, tiles blank, tile-click-to-preview dead, preview
  layer editor dead — but click-to-preview and the editor were fine all along;
  with no PVW cell there was nowhere to show their result. The source roster
  survived because `set-multiview-layout` rides the frequent spine sync, which is
  what made it look like a layout bug rather than a lost command.
  **Fix pattern:** `MediaCoreSupervisor` fires `ProfileChanged` on every core
  generation (initial handshake AND respawn) — re-arm from
  `StudioViewModel.OnBridgeProfileChanged`, reusing the existing debounce rather
  than adding a second retry mechanism. **And make it observable:** `sessionState()`
  publishes a `multiviewer` node with the APPLIED config, unconditionally — a node
  that only appears once configured is absent in exactly the case worth detecting.
  Audit any other launch-time one-shot against this rule.
  Repro (this is the acceptance test): with a healthy wall up, `Stop-Process` the
  `corevideo-native.exe` and watch the wall after the supervisor respawns it.
  Headless oracle: `node scripts/validate-multiview.mjs [--sources N] [--mode M]`
  judges the published wall (PGM + PVW cells, N source tiles, 16:9 in-canvas
  non-overlapping rects, and that the core echoes the configured mode). It proves
  STRUCTURE, never pixels — the event carries a GPU handle, not a frame.
- The WinUI window often **opens minimized off-screen** (rect ≈ -32000,-32000). Restore
  gently with `ShowWindow(SW_RESTORE=9)`; do NOT aggressively maximize/move a
  SwapChainPanel window across monitors — it can kill the window (and resize can crash).
- 60fps needs `timeBeginPeriod(1)` (Windows 15.6ms timer granularity) + a frame-budget
  pace, both already in `JsonRpcServer.cpp`. The deadline accumulates from a FIXED
  ANCHOR with bounded catch-up (a relative `t0 + budget` deadline can only lose time —
  each overshoot becomes the next frame's start), and the post-timer spin tail is
  **200µs**: the old 500µs only existed to mask that drift, and re-measured on this rig
  it costs ~5s of core CPU per 53s wall for nothing. Never raise the guard to paper over
  a pacing bug.
- **Program recording muxes the NV12 program TAP, never `ProgramFrame::preview`**
  (fixed 2026-08-06). `preview` is a **320x180 UI thumbnail**; writing it into a
  writer opened at program dimensions put the entire show into a small corner of a
  black frame — and shipped that way for months (a 2026-07-13 recording: 8995
  frames, flat luma 4/255) because every validator checked stream presence and
  container alignment and **none ever looked at a pixel**. Windows has no full-res
  BGRA readback (that would be 8MB/frame); the full program exists only as NV12
  from `compositor->takeVcamNv12` — the same tap the vcam and RTMP consume — so
  MediaCore takes it ONCE per output tick, attaches it to `work.programFrame`
  BEFORE `encoder->submit`, and the senders inherit it via their copy. Gated by
  `RecordingSessionRequest::programNv12`, which MediaCore sets only when
  `ICompositor::suppliesProgramNv12()` AND the recording is exactly 1080p (the tap
  is a pinned 1080p scale-blit; feeding it to a 4K writer would letterbox — a
  non-1080p recording still takes the old path and is still wrong). macOS is
  unaffected: Metal publishes `programFullBgra` and AVFoundation already read it.
  **When touching the recording path, verify PIXELS** (mean luma of the output),
  not just that frames were written.
- **Zoom ingest runs a FRAME SYNCHRONIZER** (`ZoomEngineRuntime::frameSync_`, owner
  decision 2026-08-06) — the one-frame cushion every hardware switcher input has. A
  latest-wins slot cannot absorb a ~60Hz source and a ~60Hz render free-running against
  each other: ~1ms of jitter puts two frames in one render interval (one destroyed
  unseen) and none in the next (a repeat) — measured 6–10% each way. The cushion is
  built UP FRONT (prime to 2 queued, then serve 1 per fetch); a catch-up buffer that
  fills after the fact was measured and does NOT work, because the starved tick comes
  first. Result: 0.0% overwritten, 99% delivery, for a deliberate +16.7ms. Capped at 3
  deep — sustained overflow drops the OLDEST so latency never accumulates.
  `COREVIDEO_FRAME_SYNC=0` restores latest-wins and is the A/B control; keep it working.
  Note audio now leads video by one more frame in all outputs (within the 50ms G2
  budget; confirm on the clap test).
- **The perf drill is `scripts/mac-show-drill.py` and it runs on Windows** (despite the
  name — it gates SHARED core code, so it must run on every platform that ships it):
  `python scripts/mac-show-drill.py --seconds 40 --load 8` drives N synthetic 1080p60
  Zoom feeds through the real ingest path and gates sustained fps, dropped frames,
  frame DELIVERY, ingest→render latency percentiles, and coreMutex over-budget ratio.
  Defaults to `native/build-dev` + `.exe` here (`COREVIDEO_BUILD_DIR` /
  `COREVIDEO_FAKE_ENGINE_PATH` override). **Mean fps is not a health metric** — an 8ms
  ingest poll silently dropped ~22% of decoded frames while fps read a healthy 60
  (`docs/windows-perf-handoff.md` has the full before/after). Always confirm the fake
  engine actually delivered the rate you asked for (`COREVIDEO_FAKE_ENGINE_LOG`), and
  run `git status` before any measurement build — a stale tree answers a different
  question than the one you asked.
  **The fake engine delivers ONE video stream per participant** (2026-09-09): it used
  to keep its `participant-video-<id>-auto` stand-in alive alongside the app's explicit
  `participant-video-<id>-camera` subscribe, so 3 participants ran 6 targets and every
  participant got 2 x `COREVIDEO_FAKE_ENGINE_FPS` interleaved into one core slot
  (`latestDecodedFrames_[participantId]`) — every fps number from the rig was
  uninterpretable. An explicit subscribe now retires the auto target, and the
  achieved-rate line states target count, participant count and per-participant fps
  next to the configured source rate, so a 6-target log can never again be read as a
  3-participant rate. Every fake-engine harness must PIN `COREVIDEO_FAKE_ENGINE_FPS`
  (`mac-show-drill.py`, `qa/collect-runtime-snapshots.mjs`, `validate-iso-record.mjs`)
  and print it — an unpinned run silently measures at the default 30.
  **The drill now enforces that "did the harness source the load" check itself**
  (2026-08-07): delivery is (frames the compositor saw)/(frames we ASKED for), so a
  harness that under-produces reads as the CORE losing frames. It said "only 51% of
  decoded frames reached the compositor" on a macos-14 runner that sourced ~250 of
  480 frames/s; the same drill on a real box sources 479 f/s (1.49GB/s) and delivers
  101%. A >10% shortfall is now named as a HARNESS failure (still a failure — the run
  proved nothing). **The loaded step is therefore ADVISORY on CI and BLOCKING on real
  hardware**: sizing CI down to `--load 3` scored *worse* (45.2fps vs 59.3), so shared
  runners cannot gate perf at any load. Run `--load 8` locally before shipping perf work
  — that is the real gate.
  **The recorded-rate gate NAMES EVIDENCE, it does not assert a cause (2026-09-09).**
  The drill used to hard-code "encoder->submit rides the ~50Hz audio worker" on that
  failure. For Program video that is stale — the submit moved to the signalled video
  tick and the audio-worker submit is guarded by `videoOutputTickRunning_`, which
  `JsonRpcServer.cpp` sets true unconditionally in any real run — so the message was
  pointing every reader at the wrong stage. It now prints a "Recording-window stage
  rates" block on BOTH paths (compositor render slots/s, video-out tick/s, audio
  worker tick/s, encoder programVideoWritten/s, render skipped/deadline misses,
  encoder droppedVideo, program-buffer underruns/gpuNotReady, the recording proof's
  `encoderQueueDroppedVideoFrames`, and the last full `[render]` window), sampled as
  deltas from `realtimeEvidence`/`encoderEvidence` at both ends of the record window.
  A compositor rate below 60 means the machine never produced 60; a compositor at 60
  with a lower video-out/encoder/muxed rate means the loss is downstream.
  `MIN_RECORDED_FPS_RATIO` is unchanged.
- **`MonitorRenderFaultInjection.*` are TIMING MEASUREMENTS on a real GPU, not unit
  tests.** They drive the real compositor on the real 60Hz production timeline and every
  assertion is relative to an unfaulted baseline window measured moments earlier. That
  baseline is a PRECONDITION, so `measureSettledBaseline` retries up to four windows
  before giving up — but sustained contention (another build, a soak, a second test run)
  can starve every attempt, and then the test fails for machine load rather than for the
  property under test. Observed failing this way while an A/B soak had the box.
  **Run them on a quiet machine**, and exclude them with
  `corevideo-native-tests.exe --gtest_filter=-MonitorRenderFaultInjection.*` when the box
  is busy. Same posture `mac-show-drill.py` already carries: a shared or loaded machine
  cannot gate a timing property at any threshold. Do NOT "fix" a load failure by widening
  the margin — that trades a flaky test for one that asserts nothing.
- **The Wave 0 snapshot judge finally has a producer:**
  `node scripts/qa/collect-runtime-snapshots.mjs --out capture.json [--seconds N]
  [--interval-ms N] [--load N] [--recording]` runs its own core over stdio, samples
  bare `{"type":"snapshot"}` on a DECLARED interval, and writes the
  `{samples[], expectedWorkers, recordingExpected, policy}` envelope that
  `production-qualification.mjs --runtime-snapshots` consumes. Full contract and the
  two deliberate refusals (no fabricated `nativeNowMs`; never a quiet empty envelope)
  are in `docs/qualification/WAVE-0.md`.
- I420→RGB is a GPU HLSL shader in `D3D11CompositorAdapter.cpp`
  (`kCompositorYuvPixelShader`, BT.709 full-range). Zoom frames carry I420
  (`hasI420()`), NOT BGRA — any frame merge/match must check `hasI420()` too or Zoom
  renders blank (see the `renderSyntheticTick` engine-roster merge).
- Audio/output no longer rides the render lock — **Phase 2 shipped**: a dedicated
  ~50Hz worker (`JsonRpcServer` `audioOutputThread`) runs
  `MediaCore::renderAudioOutputTick` with a strict two-lock discipline: `coreMutex`
  briefly for gather/publish, `audioOutputMutex_` for the long DSP/device/network span,
  NEVER both nested on the worker. The render thread is video-only
  (`renderDisplayTick`), and an empty `media-core-sync` poll returns the published
  snapshot without a tick. When touching audio/output control-plane commands, keep the
  `coreMutex` → `audioOutputMutex_` lock ORDER (see `docs/phase2-threading-plan.md`);
  a single missed `audioOutputMutex_` guard is a data race. Engine pipe writes go
  through `ZoomEngineRuntime`'s outbound queue + dedicated sender thread (increment 3)
  — never call `process_->sendLine` directly. Full lock order:
  `coreMutex` → `audioOutputMutex_`, and `coreMutex` → `ZoomEngineRuntime::mutex_` →
  `::sendMutex_` (never reversed). `coreMutex` holds are budgeted sub-ms outside
  sanctioned sites — `core/LockHoldGuardrail` warns (rate-capped) on violations.
