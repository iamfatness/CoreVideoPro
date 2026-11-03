# The Tiles wall is a composed source, and composed sources are ERASED not tombstoned (#448 slice 2 task 4, 2026-09-12)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

# Local sources as Tiles members (#1071, 2026-10-31; revised after review 2026-11-01)

Owner request (live show prep, 2026-10-31): "the ability to add local sources
to Tiles." Until this change the wall's membership was decided in the shell
from the Zoom room only; a webcam, capture card or screen could be the wall's
BACKGROUND but never a tile. For CAPTURE sources the core was not the blocker
— its frame-age gather matches a `capture:` member by its full id and the
tile layer keeps that id as `participantId` — but two core rules did have to
change (below), and MEDIA is not admitted yet.

**The eligible set is "video-on Zoom participants + every IN-SHOW show-input
slot bound to a local capture source".** `TilesLocalSourcePolicy.LocalVideoSources`
(`native-shell/CoreVideoPro.WinUI/Services/TilesLocalSourcePolicy.cs`) reads
the show-input roster: every slot that is `InShow`, whose kind is not
Unassigned/Zoom/Media and whose `ShowInputRosterService.SlotSourceId` is
canonical (`capture:<id>`, `capture:screen:N`, ...), in slot order,
de-duplicated. IN-SHOW, not merely bound (review of PR #1081, finding 3): a
slot bound for a Camera scene but not in the show is not brought online
(`QueueSelectedCaptureDevicesOnline` connects InShow devices only), so seating
it would hold a never-drawn member or surprise a Zoom-only wall on upgrade with
a webcam tile nobody asked for. The mode label now reads "All eligible: meeting
participants + in-show local sources". `TilesLayerPayloadBuilder.Build` takes
the ids as `localVideoSourceIds` and appends them after the roster participants,
accepting only canonical local ids (a bare device id would be qualified `zoom:`
and match no frame). `routed` mode keeps filtering to the InShow slot ids
(`TilesLocalSourcePolicy.RoutedSourceIds`, one definition for the wire and the
editor); in eligible mode every local candidate is already in-show, so the two
modes differ only for Zoom participants.

**Media is NOT a Tiles member yet (finding 1).** The core opens a media asset
only for a scene ROUTE: `MediaCore::syncStillMediaDesired` and
`syncMediaTransportsDesired` build their desired sets from
`sceneRoutes_`/`previewSceneRoutes_`/outgoing routes, keyed by the asset PATH
the route carries. A Tiles member is an id with no path, so a `media:` member
would never have a frame in the gather and `admitTilesMembers` would never draw
it. Follow-up: carry `{id, path, kind, loop}` for media members on the `tiles`
wire node and add them to both desired sets (both buses), then re-admit Media
in `LocalVideoSources`. `IsLocalSourceId` still recognises `media:` so a saved
id round-trips honestly, and `media:` entries are judged stale (next paragraph)
rather than silently kept.

**Identity: a local id IS its own live id, and it goes stale by leaving the
show.** `TilesIdentityPolicy.ResolveLiveSourceId` and `Persist` return a
`capture:`/`media:` id unchanged — never roster-resolved — so manual slots and
the never-show list hold them as written (`scene.tiles.slot.assign` with a
canonical local id works unchanged). `DescribeStale`/`ClearStale` take the
current live local set (`StudioViewModel.TilesLiveLocalSourceIds`, the ids
`LocalVideoSources` returns now): a local entry outside it is stale ("not in
the show") and "Clear stale" removes it (finding 4; before, a deleted asset or
unplugged device in manual slot 3 kept an empty reserved cell forever). A
`null` set means "do not judge local ids". `Label` returns the canonical id for
an unbound local source rather than "Unavailable source". Zoom re-join handling
(#479) is untouched.

**The member picker lists local sources under their show-input groups.**
`TilesMemberChoicePolicy.Build` (extracted from `StudioViewModel.
GalleryMemberChoices`) = Zoom participants (roster order) then the local
sources ordered by `ShowInputRosterService.UnifiedSourceGroups` (Camera,
Screen, Browser, Media, SRT, NDI), slot order within a group;
`GalleryMemberChoice.Group`/`Label` give the Members list "Camera · Logitech
Brio" the way the show-input picker button reads. `TilesMemberChoicePolicy.
Contains` is the check the four setters apply (`AssignTilesSlot`,
`SetTilesExclusion`, `SetTilesBackground`, `SetTilesOverride`). The list and
the summary are re-raised from `ApplyShowInputRefresh` and the media-bin load.
Picker and eligible set come from one function, so what the operator can pick
is what auto fill can seat.

**Admission: a local member does not age out on a frozen frameId (finding 2).**
Tile admission vetoed any member whose frameId had not advanced for
`kTilesStaleFrameMs` (1.5 s). That is right for a Zoom guest (the I4
frozen-but-subscribed defect) and wrong for a local source: the core itself
documents that capture, media and still sources legitimately re-serve the same
frameId while healthy (an idle desktop share, a paused clip; `StillMediaFrameCache`
keeps a stable frameId by design; see the on-air "stalled" rule in
`MediaCore::annotateLayerSource`, #535 slice 4a R1). A static screen share
would have left the wall 1.5 s after seating and the wall reflowed around it.
`compositor::tilesMemberAgesOut(sourceId)` (`TilesMembership.h`) is now the
rule: only a `zoom:` member is frame-age gated; a local member is admitted on
`hasFrame` alone and drops the tick its frame LEAVES the gather (unplugged,
un-routed) — the fabrication rule is unchanged, `hasFrame` is still evidence
from this tick's gather, never memory. Whether a frozen camera is a stall is
capture liveness, #562's decision. **Merge note for #1074 / PR #1082**
(`classifyTilesMember` / `admitTilesMembersWithHold`): fold the same predicate
in — a non-`zoom:` member with `hasFrame` classifies `Fresh` regardless of age
and is never `Held`; the hold window applies to Zoom members only.

**Dropout, on air.** Nothing Tiles-specific: `annotateLayerSource` attaches the
source's own `dropoutPolicy` and on-air status to a local tile exactly as it
does to a routed local layer. The core still refuses a dropout policy for a
non-`zoom:` id (#535 slice 4a R2), so a capture member's hold/black behaviour
is #562's job, not this change's.

**Decisions the issue left open, taken here:** SRT, RTMP, NDI and Browser
inputs are admitted too (they are `capture:` sources with video and already
carry the show-input groups); local members are seated AFTER the Zoom
participants in eligible mode; Media is deferred (above); the CLAUDE.md
reference-table row was deliberately left as #1078 edits it; under #1078 a
local tile's name line comes from `set-source-policy` displayName, which the
shell sends for capture devices, so capture tiles get names and a future Media
tile would not until that is extended.

Tests: `TilesLocalSourcePolicyTests`, `TilesMemberChoicePolicyTests`,
`TilesMembershipPolicyTests.Local1071_*`, `TilesLayerPayloadBuilderTests.
Build_SeatsInShowCaptureSlots*` / `Build_RoutedModeFiltersLocal*` /
`Build_IgnoresANonCanonicalLocalId*`, `TilesIdentityPolicyTests.Local1071_*`;
native `TilesMembership.AStaticLocalMemberStaysWhileItsFrameIsInTheGather`,
`TilesMembership.ALocalMemberWithNoFrameIsStillNotDrawn`,
`TilesRenderPlan.AZoomGuestAndACaptureDeviceShareOneWallAndDropOutIndependently`
(real tick, no route references the capture device: frozen capture keeps its
tile past the stale threshold, frozen Zoom guest ages out, unplugged capture
drops at once), `TilesRenderPlan.AFrozenCaptureMemberKeepsItsTilePastTheStaleThreshold`,
and the I4 vehicle `AFrozenFrameIdAgesTheMemberOutEvenAsTimestampKeepsAdvancing`
now driven by a frozen Zoom ISource. **Not verified live** as of the PR: no
real wall with a webcam tile has been rendered in the running app.

**It has a production WRITER and, as of this slice, no production READER.**
`MediaCore::renderSyntheticTick` registers and releases walls for real, and the
only consumers are tests (`sourceRegistrySnapshotForTest`). That is deliberate —
per `docs/BACKLOG.md`, a #419 foundation lands on `main` only together with a real
consumer, and wall registration IS that consumer for the registry's write side —
but it means nothing in the product yet behaves differently because of these
entries. Do not describe the registry as "wired" beyond that, and expect the
first real reader (the multiview PVW cell, plan 2) to be where its snapshot shape
gets its first genuine test.

`SourceRegistry` (`native/src/core/SourceRegistry.h`, carved out of #419 unwired
onto main) gained `Kind::Composed` for sources the CORE renders rather than
captures — the Tiles wall is the first one. `MediaCore::renderSyntheticTick`
registers a live wall as `Kind::Composed` (sourceId = its layerId, `externalId`
empty, a fixed `kCoreProcessEpoch`) and releases it the tick nothing on either
bus names it any longer, in lockstep with `tilesWallSources_.releaseAllExcept` —
the same "referenced by a live scene" lifetime, one level up. `registeredWallIds_`
is the idempotence guard so a live wall's steady-state tick never touches the
registry mutex (`unordered_set::contains` before `insert`, not `insert().second`
— MSVC's `unordered_set::insert` has historically built the node before
detecting the duplicate, so this file's render-path no-allocation rule holds by
construction, not by implementation detail). A registration that fails
(`Invalid`/`Conflict`/`Exhausted`) is NOT remembered as registered, so the next
liveness transition retries it rather than abandoning the wall silently forever.

**A composed source carries no SDK handle and never claims a subscription
state.** `personId`, `externalId`, `availability`, `subscriptionRequested`,
`subscriptionObserved` are all `nullopt` for it — `nullopt` means NOT
APPLICABLE, never false — because a wall has no provider process and nothing
ever subscribes to it. `setAvailability`/`setSubscription` refuse `Composed`
outright for exactly this reason — `setSubscription`'s refusal was MISSING and
this file asserted it anyway for a day (final-review finding, fixed with
`SourceRegistryComposed.SetSubscriptionOnAWallIsRefusedOutright`): an
`observed:true` call was already refused as a side effect, because a nullopt
availability is not `Available`, but `requested:true, observed:nullopt` applied
cleanly and turned a NOT-APPLICABLE field into a concrete claim. Nothing in the
tree called it for a wall, so only the documentation was wrong — which is exactly
how an invariant rots.

**A wall id too long to register is SKIPPED, not retried** (same finding).
`SourceRegistry::kMaxIdBytes` (512) is the one declared bound on every id-shaped
field, and it is public precisely so a caller can tell a PERMANENTLY refusable id
from a transiently refused one: the registration loop deliberately does not
remember a failed add as registered (so a transient failure retries on the next
liveness transition), which turned a spelling-based refusal into a registry-mutex
acquisition plus a log line on EVERY render tick. `unregisterableWallIds_` skips
those once and loudly; `warnedWallRegistrationIds_` bounds the retryable
failures' log line to once per id while keeping the retry. Both are pruned on the
same liveness rule as `registeredWallIds_`, or a wall re-cued under a corrected
id would stay skipped or silent for the life of the process.

**A composed source is ERASED (`SourceRegistry::removeComposed`), never
tombstoned — and this is not a simplification, it is the only mechanism that
actually works.** Every other kind's departure is `Availability::Departed`
(kept for diagnostics via `retireProcessEpoch`/`setAvailability`). A composed
entry CANNOT be tombstoned that way even in principle: `setAvailability`
refuses `Composed`, so nothing can ever flip it to `Departed`, and because its
`availability` stays `nullopt` forever, `externalConflict`'s
`availability != Departed` test reads true for it PERMANENTLY — a tombstoned
wall id could never be reused by `add()` again. `removeComposed` erases the
`sources_` entry outright and refuses (`Invalid`) for any non-`Composed` kind.
It takes a bare `SourceId`, deliberately not a `Token`: the caller must be the
SOLE owner of a composed source's lifetime (`replace()` exists precisely so an
OLD callback cannot retire a NEW instance it no longer owns via compare-and-
replace; removal has no such fence and must never grow a second writer).

**Its lifetime is scene-reference, exactly like `TilesWallSources`
(`releaseAllExcept`) one level down** — a wall no live scene names is gone from
the registry the same render tick `tilesWallSources_` releases its animation
object, and a wall released then re-cued under the same scene id is a
genuinely NEW registry entry (a fresh `instanceId`, minted from the registry's
own revision counter), never the old one resurrected.

Tests: `native/tests/SourceRegistryComposedTest.cpp` (`removeComposed`: erases,
frees the id for reuse, refuses non-composed, `NotFound` on an unknown id) and
`native/tests/TilesRenderPlanTest.cpp` (a live wall registers as `Composed`
with all five capture-only fields `nullopt`; an unreferenced wall is gone from
the registry; a released-then-re-cued wall gets a new `instanceId`; a wall
staying live across many ticks keeps the SAME registry identity — pinned by
`instanceId` equality, not a source count, because `sources_` is a
`std::map` keyed by sourceId where a count assertion cannot distinguish "the
guard works" from "every tick refuses `Conflict` while quietly taking the
registry mutex 60x/s").

# Per-tile name line, "Name each tile" (#1073, 2026-10-31)

Owner request (live show, 2026-10-31): "a lower third for each person in a
tiles scene, just the top line." The Tiles scene option **Name each tile**
(Sources > Tiles > Members > NAMES; control action `scene.tiles.names.set`
with `enabled`; control-state field `previewTilesNamesOn`; persisted on the
scene as `DynamicGallerySettings.NameEachTile`; wire key `style.nameEachTile`
on the `tiles` node of `load-scene-graph`) puts ONE line of text inside each
member tile: the source's name, in the scene's lower-third style, top line
only, never a secondary line.

**It is the scene lower third's own raster, not a second text renderer.**
`MediaCore::buildRenderPlanForScene` emits, for every drawn tile, one ordinary
overlay layer `tile-name:<memberId>` (kind `overlay`, `hasOverlayContent`,
`overlay.appearance` enabled, `overlay.title` = the name, `keyPhase` `on-air`)
ordered above every tile and glow of its wall (`tilesBaseOrder + 129 + order`).
The D3D11 compositor draws it through `CompositorOverlayRaster` exactly as it
draws the scene key; the CPU preview through `OverlayTileRaster`. Nothing in
the compositors knows the layer is a tile name.

**Geometry is `compositor/TilesNameLine.h`, pure and tested on its own
(`TilesNameLineTest.cpp`).** `placeTilesNameLine(tileRect, gutterX, gutterY,
style, canvasAspect)`:

- the look is `lowerThirdAppearance_` when the scene has one (#847), else the
  struct's defaults switched on; the style's `anchor` picks the tile corner
  (default lower-left), the band is `style.width` of the tile's width, and it
  is inset from the tile edge by `tilesNameInsetPx`: the grid's own gutter, or
  the decoration's border stroke plus the bite of a rounded corner
  (`radius * (1 - 1/sqrt2)`), whichever is larger, so a rounded corner never
  swallows the band; a pinned tile is measured from its own override rect;
- the text SCALES WITH THE TILE: font px = `nameSize * tileRect.height`,
  floored at `kTilesNameMinimumFontPx` (18 px, 1080-line reference space,
  the same space the scene key's `nameSize` is authored in; a style authored
  smaller than 18 keeps its own size as the floor) and capped at the style's
  own size, so a 1-up tile reads exactly like the scene key;
- SUPPRESSED (no layer at all) when the band would take more than 33% of the
  tile's height or the tile is narrower than six font heights. The numbers
  are judged against the REAL solver's tile heights (margins and gutters
  included), never an idealised 1/N: with the default style and 0.741%
  gutter/margin at 1080p, walls of up to 49 members carry names (7x7,
  145.1 px tiles, 30.1% band); from 50 members the solver goes to 8 columns,
  which are width-bound at 129.9 px tall (33.6% band), and every name is
  suppressed. Pinned by
  `TilesNameLines.TheRealSolverDecidesHowManyNamesAWallCarries` through
  `loadNamedWall` and the production solver;
- a member with no known name gets no line (`tilesMemberDisplayName`: the
  shell's `set-source-policy` displayName, else the render tick's pre-read
  roster for a `zoom:` member). Nothing is invented. **Known limit:**
  `BuildSourcePolicyWires` sends a name only for `zoom:` participants and
  `capture:` devices, so a `media:` or `browser:` wall member (local sources
  as Tiles members, #1071 / PR #1081) is unlabelled until the shell sends its
  name too; the Members-tab help text says so.

**It follows membership and the wall animation by construction.** The layer
is built from the same `slots`/`admitted` loop as its tile, so a member that
leaves, goes stale, or is excluded takes its line with it the same tick.
`TilesPlanAnimation::applyLatest` treats `tile-name:<id>` like `tile:<id>`
and `tiles-glow:<id>`: it multiplies the layer's opacity by the tile's alpha
(so a joining member's name fades in with its tile) and moves the band with
`followTilesNameLine`, which keeps the band's offset from the tile's anchored
corner at the SETTLED rect (`AnimatedTile::target`, new) applied to the
sampled rect, and while the tile is still smaller than its target (growing
into place) scales band and offset uniformly about that corner by
`min(1, sampled.w/target.w, sampled.h/target.h)`, so the band never reaches
past the tile it rides. The scale is applied through the viewport; the
texture is not re-rastered mid-flight. Bounded by the members list (64) like
the tiles themselves. A shown/suppressed flip (a tile crossing the threshold
during a reflow) still appears or disappears with the reflow rather than
animating; that is a known, accepted pop.

**While a raster is preparing, the previous one is drawn, never a bare bar.**
The overlay raster cache keys on band PIXEL SIZE, so one reflow resizes every
band and sends every name back to "preparing" at once, and the raster queue
is bounded (one serial worker, 32 queued, 64 resident). `holdLastRaster` on
`CompositorOverlayContent` (set only for name lines) makes
`D3D11CompositorAdapter::drawOverlayLayer` keep the last ready raster per
resolved layer id (`compositor/OverlayRasterRetention.h`, pruned after 120
idle frames) and draw it, scaled through the viewport, until the new one is
ready; with nothing retained it draws nothing. The legacy fallback (a bare
4%-wide accent bar) is never drawn for a name line. Still bounded: the GPU
read fence holds at most 32 images per frame (`D3DVideoFrame.h`), a
pre-existing cap shared with every other overlay raster, so a wall with more
than ~32 distinct name rasters draws the rest only as fences free up.

**Decisions the issue left open, taken here:** the name rides the TILE's own
entrance (alpha + spring) rather than carrying a separate slide/hold key
animation, so it can never lead or lag its tile; the style anchor is honoured
inside the tile rather than forcing bottom-left; the readable threshold is a
share of the tile (33%) plus a width floor, in reference space, so the rule
reads the same at every output resolution. **Render-path cost.** The band geometry is memoised per member
(`tilesNameLineMemo_`, keyed on tile rect, insets, name and style, bounded at
128 and swept against the drawn set), so a steady wall does not re-derive it
per tick per bus; the per-layer appearance/string copy into the plan remains,
exactly as for the scene key. `sortCompositorRenderPlan` now computes each
layer's semantic depth once before sorting instead of lowercasing three
strings inside the comparator. Tests:
`TilesNameLine.*`, `OverlayRasterRetention.*`, `TilesNameLines.*` (TilesRenderPlanTest.cpp),
`TilesAnimator.ANameLineMovesAndFadesWithItsTile`,
`TilesAnimator.AnEnteringTilesNameLineFadesInWithIt`,
`TilesAnimator.AGrowingTilesNameLineScalesWithItAndStaysInside`, and on the shell side
the command-builder key, the persistence round-trip, the payload builder and
`ControlActionRegistryTests.TilesNamesSet_*`. **Not verified live** as of the
PR: no screenshot of a real wall with names has been taken; the first live
check is the owner's readable-threshold judgement on a 3x3 and a 4x4 wall.

# A stale tile is HELD for a bounded window, then dropped (#1074, live show 2026-10-31)

**Owner-ruling status: working assumption, not yet ruled.** #427 still carries the handoff
decision on tile staleness. This section describes what the code does on
`codex/tiles-hold-1074`; if the owner rules differently, `kTilesHoldWindowMs` is the one
number to change (and the literal 1500/4000 in the real-tick timing test with it).

**What happened.** Streaming to YouTube at 10 Mbps, the operator's uplink stalled for 1 to 3 s
at a time, three or four times in fifteen minutes. On every stall the Zoom raw video for all
video-on participants stopped arriving and the RTMP queue backed up at the same instants
(`[stream-backpressure] enter divisor=2 buffered=445ms`). Zoom subscription churn was zero,
the Program buffer missed no deadline. `admitTilesMembers` dropped every member whose newest
frame was older than `kTilesStaleFrameMs` (1500 ms), so each stall emptied slots, reflowed the
wall, and popped the tiles back. The owner read it as "participants dropping in the tiles
source".

**The rule now** (`classifyTilesMember` in `native/src/compositor/TilesMembership.h`, the one
place the rule lives):

| Member | Age of its newest frame | State | Drawn? | Snapshot |
|---|---|---|---|---|
| any | no frame in this tick's gather (video-off, departure, un-route, never arrived) | Dropped | no | absent from `tiles.members` |
| local (`capture:`, `media:`) | any, while its frame is in the gather | Fresh | yes | `held: false` |
| `zoom:` | 0 .. `kTilesStaleFrameMs` (1500) inclusive | Fresh | yes | `held: false` |
| `zoom:` | 1501 .. `kTilesHoldWindowMs` (4000) inclusive | **Held** | yes, same slot | `held: true`, counted in `tiles.heldCount` |
| `zoom:` | past `kTilesHoldWindowMs` | Dropped | no, wall reflows | absent |

**Only Zoom members age.** `tilesMemberAgesOut(sourceId)` (identical to the predicate PR #1081
adds for #1071) is true for `zoom:` only. A local source re-serves one frameId for long
stretches while healthy (a static screen share, a still, a paused clip), so frame age says
nothing about it: it is admitted while its frame is in the gather, never labelled held, and
drops the tick its frame leaves. Capture liveness is #562's decision, not a frame-age guess.

Age is measured from the tick at which the member's `frameId` last changed (the I4 gather in
`MediaCore::renderSyntheticTick`), never from the frame's own timestamp, which every producer
re-stamps. A held member keeps its slot order among the fresh ones; one new frame clears the
label with no reflow. This is the Tiles form of the #1038 outputs rule: a stall is a labelled,
counted hold caused by the source, never a silent pass and never a fabricated picture
(`hasFrame` still comes from the gather).

**`held` is the slot; `showing` is what is in it.** Each `tiles.members[]` entry carries
`showing: "picture" | "black"`, and the node carries `blackCount`. Under a Zoom source's `black`
dropout policy the bus calls it `stalled` on air at 1.5 s (`kOnAirStallNs`, the bus's real
clock) and the compositor paints black (`compositor::blackOnStalled`, the same predicate
`D3D11CompositorAdapter` uses), while the slot itself stays held until 4 s. The snapshot says
`held: true, showing: "black"` for that span, never a held picture it is not showing. Under
`hold` (the default) a held slot shows its last picture. The two clocks differ (render tick vs
bus steady clock); in production they track each other, in a fast unit test they do not, which
is why the black test sleeps.

**Why 4 s and why a second constant.** The issue asked for 3 to 5 s. `kTilesStaleFrameMs` is
shared with the on-air `stalled` label and the per-source dropout policy, so widening it
would delay those too; the hold window is its own constant with a `static_assert` that it
outlives the stale threshold. The wall BACKGROUND predicate (`tilesBackgroundSourceIsDrawable`)
is untouched: it holds for as long as a frame exists and has no window.

**Telemetry.** Session snapshot (`CoreStateCapture.cpp`): `tiles.members[].held`,
`tiles.members[].showing`, `tiles.heldCount`, `tiles.blackCount`, `tiles.staleAfterMs`,
`tiles.holdWindowMs`; `sources[].sinceLastNewFrameMs` (bus clock, null before the first frame).
Core log, per EPISODE (`compositor/TilesHoldEpisodeLog.h`): `[tiles-hold] <id> held ageMs=N`
once, re-armed only after `kTilesHoldLogRearmMs` (3 s, i.e. 2 x the stale threshold) of
continuous fresh; `dropped` every drop; `recovered` only after a `dropped`; and
`dropped reason=no-frame` when an announced hold ends because the frame left the gather. A
degraded feed delivering a frame every 1.5 to 2 s therefore logs `held` once, not twice per
frame (the re-arm must exceed 1.5 s or it would re-log every frame of exactly that feed).

**Status line.** When a producing stream sender is in backpressure AND two or more Zoom video
sources are stalled by the on-air rule (bus `health: "stalled"` and `sinceLastNewFrameMs >=
1500`), the Studio footer shows one amber FEEDS line: `Stream 445 ms behind and 3 Zoom feeds
stalled` (`Stream at 1/N rate` when `appliedDivisor` is above 1; `Stream backpressure level L`
when the queue has drained but pressure has not released). It states the two symptoms and
claims no cause. Capture sources are not counted (two static screen shares under any stream
pressure would otherwise light it with no fault), nor is the bus's 200 ms diagnostic `stalled`
on its own (it flickers on ordinary cadence noise), nor a row without `sinceLastNewFrameMs`
(older core). `StalledFeedsReadout` in `CoreVideoPro.MediaCore`; the `sources` node is bound
typed for it (`contracts/observation.schema.json`).

**Tests.** `native/tests/TilesMembershipTest.cpp` (`TilesMembership.*`: both edges of both
thresholds, no-frame drops at once, held keeps slot order, local members never held or aged,
only `zoom:` ages, the log latch: degraded feed logs held once, re-arm window, dropped always /
recovered only after dropped). `native/tests/TilesRenderPlanTest.cpp`, real render ticks through
a `kind: "zoom"` bus source (`ControllableZoomSource`):
`AFrozenFrameIdAgesTheMemberOutEvenAsTimestampKeepsAdvancing` asserts the first held tick at
~1.5 s and the first gone tick at ~4 s with LITERAL 1500/4000 (a mutant with the window at 1600
fails it; bounds written via the constants did not);
`AHeldZoomMemberWhoseFrameLeavesTheGatherIsGoneOnTheNextTick`;
`AHeldSlotUnderTheBlackPolicyIsReportedAsShowingBlack` (sleeps 1.6 s real time; also pins
`sources[].sinceLastNewFrameMs`); `AFrozenCaptureMemberIsNeverLabelledHeld`.
`AWallsLiveBackgroundSurvivesATakeAcrossAStaleBeat` no longer requires the capture wall to go
all-stale (local members do not age); it asserts the frozen background directly.
`native-shell/CoreVideoPro.MediaCore.Tests/StalledFeedsReadoutTests.cs` (static-capture false
positive, threshold edges 200/1499/1500/3000 ms, missing age field, wire binding, no "network"
wording).

**Not verified live.** No real uplink stall has been watched with this build, the FEEDS line
has not been seen rendered, and whether Zoom video-off removes the frame from the gather at
once (drop now) or re-serves a frozen frame (drop at 4 s) was reasoned from `ZoomEngineRuntime`,
not observed.
