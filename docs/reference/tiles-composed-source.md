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
