# The Tiles wall is a composed source, and composed sources are ERASED not tombstoned (#448 slice 2 task 4, 2026-09-12)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

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
