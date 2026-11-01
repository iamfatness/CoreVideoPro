# The Tiles wall is a composed source, and composed sources are ERASED not tombstoned (#448 slice 2 task 4, 2026-09-12)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

# Local sources as Tiles members (#1071, 2026-10-31)

Owner request (live show prep, 2026-10-31): "the ability to add local sources
to Tiles." Until this change the wall's membership was decided in the shell
from the Zoom room only; a webcam, capture card, screen or media asset could be
the wall's BACKGROUND but never a tile. The core was never the blocker — its
frame-age gather already matches a `capture:`/`media:` member by its full id
and the tile layer keeps that id as `participantId` — so this is a shell
membership change plus a native test that pins the claim.

**The eligible set is "video-on Zoom participants + every show-input slot bound
to a local source with video".** `TilesLocalSourcePolicy.LocalVideoSources`
(`native-shell/CoreVideoPro.WinUI/Services/TilesLocalSourcePolicy.cs`) reads the
show-input roster: every slot whose kind is not Unassigned/Zoom and whose
`ShowInputRosterService.SlotSourceId` is canonical (`capture:<id>`,
`capture:screen:N`, `media:<assetId>`), in slot order, de-duplicated. A Media
slot counts only when its asset is among the VISUAL assets (video or still;
an audio-only asset has no picture to tile, and an asset that left the bin is
not invented). Slots, not enumerated devices: a member is something the
operator put in the show, the same rule routed mode and the multiviewer use.
`TilesLayerPayloadBuilder.Build` takes the ids as `localVideoSourceIds` and
appends them after the roster participants, accepting only canonical local ids
(a bare device id would be qualified `zoom:` and match no frame). `routed`
mode keeps filtering to the InShow slot ids, which already carried local ids.

**Identity: a local id IS its own live id.** `TilesIdentityPolicy.ResolveLiveSourceId`
and `Persist` return a `capture:`/`media:` id unchanged — never resolved
against the roster, never stale, in any meeting or none — so manual slots and
the never-show list hold them as written (`scene.tiles.slot.assign` with a
canonical local id works unchanged). `Label` returns the canonical id for an
unbound local source rather than "Unavailable source", which reads as a Zoom
guest who left. Zoom re-join handling (#479) is untouched.

**The member picker lists local sources under their show-input groups.**
`StudioViewModel.GalleryMemberChoices` = Zoom participants (roster order) then
the local sources ordered by `ShowInputRosterService.UnifiedSourceGroups`
(Camera, Screen, Browser, Media, SRT, NDI), slot order within a group;
`GalleryMemberChoice.Group`/`Label` give the Members list "Camera · Logitech
Brio" the way the show-input picker button reads. The list and the summary are
re-raised from `ApplyShowInputRefresh` and the media-bin load, so a slot
binding appears as a candidate without a gallery edit. Because the picker and
the eligible set come from one function, what the operator can pick is what
auto fill can seat; `SetTilesBackground`, `SetTilesOverride`,
`AssignTilesSlot`, `SetTilesExclusion` validate against the same list.

**Dropout follows the source, not a Tiles rule.** Nothing Tiles-specific was
added: a local member runs through the same `admitTilesMembers` staleness gate
as a Zoom member (frameId stops advancing -> out of the wall after
`kTilesStaleFrameMs`, the wall reflows) and `annotateLayerSource` attaches the
source's own `dropoutPolicy` when one is set. The core still refuses a dropout
policy for a non-`zoom:` id (#535 slice 4a R2), so a capture member's hold/black
behaviour is #562's job (capture liveness), not this change's.

**Decisions the issue left open, taken here:** SRT, RTMP, NDI and Browser
inputs are admitted too (they are `capture:` sources with video and already
carry the show-input groups); local members are seated AFTER the Zoom
participants in eligible mode; a Media slot with an audio-only asset is not a
candidate; the CLAUDE.md reference-table row was deliberately left as #1078
edits it. Tests: `TilesLocalSourcePolicyTests`, `TilesMembershipPolicyTests.
Local1071_*`, `TilesLayerPayloadBuilderTests.Build_SeatsABoundCaptureSlot*` /
`Build_RoutedModeFiltersLocal*` / `Build_IgnoresANonCanonicalLocalId*`,
`TilesIdentityPolicyTests.Local1071_*`, and the native
`TilesRenderPlan.AZoomGuestAndACaptureDeviceShareOneWallAndDropOutIndependently`
(real tick: one ISource Zoom guest + one capture device on one wall; freezing
the capture ages only its tile out). **Not verified live** as of the PR: no
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
