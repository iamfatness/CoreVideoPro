# CoreVideo Tiles T1 — what was knowingly left undone

Companion to `2026-08-15-corevideo-tiles-T1-core-wall.md`. Written at merge time
so the deferred list survives in git rather than in a scratch ledger.

Every item here was found by review, judged, and deliberately not fixed in T1.
Nothing on this list is a surprise; each carries why it waits and what it costs.

## Fixed in the final wave, recorded because the reasoning matters

**A wall with no admitted members used to emit an empty render plan.** All three
compositors (`D3D11CompositorAdapter`, `ProgramFramePreview`,
`MetalCompositorAdapter`) carry their own fallback for an empty plan: one
full-canvas cell per decoded frame. So a Tiles scene whose members all went stale
did not "show nothing" — it showed an improvised grid of whatever the core was
decoding, including sources not on the wall, inherited by the virtual camera,
recordings and streams. The same state occurred transiently on every Tiles take
before members' first frames landed.

This was originally parked as an acceptable product edge case ("wall configured,
nobody live shows nothing"). That reading was wrong: the premise "emits zero
layers" is not "shows nothing", it is "hands the compositor a blank plan and lets
it improvise". A `wallActive` wall now always emits at least its background.

The lesson worth keeping: Task 4 suppressed the legacy fallback *in the render
plan* and no task's scope contained the layer below, where each compositor has
its own. Fixing a fallback in one layer created a new way to reach the trigger
condition in another.

## Deferred — carried into T2 or later, with reasons

### The wall rides the event-driven sync, not the repeating one

`MediaCoreCommandBuilder.BuildSyncCommands` is reachable only from
`StudioViewModel.SyncActiveSceneAsync`, which fires on operator actions (Take,
scene change, Engine toggle). The repeating spine payload carries no scene graph
and no tiles node, and `OnBridgeProfileChanged` re-arms only the multiviewer
config and capture SHM.

**Consequence:** after a supervisor respawn under a live shell, the fresh core has
no wall until the operator touches something. With the background fix above, that
window now shows the wall's background rather than an improvised grid — which is
why this is deferrable rather than blocking.

**Why not fixed here:** it is a pre-existing exposure (the old gallery routes had
it identically), and the correct fix is to re-arm the scene graph from
`OnBridgeProfileChanged` alongside the multiviewer resend — a change to the
shell's respawn contract that deserves its own change and its own test, per
CLAUDE.md's rule that a one-shot command must be re-applied on every core
generation.

### Wall membership is only as fresh as an operator action

Members come from `RoomVideoParticipants`, but roster changes arrive on the
snapshot patch and `TrySyncMediaCoreAsync` returns early while a production patch
is being applied. A guest **joining** does not appear until something else
triggers a sync. Departure is masked, because their frames stop and the
frame-reality veto drops them — which is why this went unnoticed.

The architecture note "the core solves the wall every render tick" is true of
geometry and false of membership. Same pre-existing shape as above; it is the
product promise of a dynamic gallery and belongs with the respawn fix.

### Metal renders the wall background as a grey slab

`MetalCompositorAdapter` has no `hasFillColor` branch, so on macOS the wall's
configured background falls through to the default layer colour. The code fix is
T5's (Metal parity). A comment now marks the site so it cannot be found only by
reading a review transcript.

### The oracle proves placement, not decoration

Recorded in full in the design spec. In short: every geometric probe samples an
edge or tile **midpoint**, so rounded corners are invisible, borders hide inside
the boundary tolerance (a thickness-2 border insets 4–8px against a 10px
tolerance), and glow needs its threshold pinned to a point on the falloff curve
rather than a computed extent. T2 owes a corner probe, border colour/thickness
sampling, and glow threshold semantics.

**Standing rule, also written at the code site:** glow will legitimately push the
measured transition past `tolPx`. Extend the assertion. Never widen the tolerance
to get green.

### Smaller items

- **`sessionState()`'s `tiles` node is program-bus only.** Correct for T1, whose
  oracle judges PROGRAM. T3's canvas editor shows PREVIEW and will need a bus
  discriminator — note the shell currently emits an identical `layerId` for both
  buses when preview and program are the same scene.
- **Staleness is measured on a synthetic frame-counter clock**, not wall time, so
  `kTilesStaleFrameMs = 1500` stretches under a render stall and shortens under
  command traffic. Consistent with the rest of the codebase; nothing states the
  mapping.
- **`detectGridShape`'s row-band epsilon (0.03 normalized, ~32px at 1080p) has no
  boundary test.** Its failure direction is under-counting rows, which silently
  skips an axis the oracle should scan. `--participants` is clamped to the
  reasoned range until a 3+-row fixture exists.
- **`tilesFrameFreshness_` entries are not erased for members dropped from the
  list**, so a member removed and re-added while still frozen is aged out
  immediately on return. Bounded by distinct source ids.
- **`MediaCoreBridgeService.BuildSceneGraphCommand`** emits `load-scene-graph`
  with routes and no `tiles` key. Zero callers, pre-existing dead code — but it
  would now clear a live wall. Delete it when next in that file.
- **No perf evidence exists for the wall-active path.** By inspection the
  per-tick aggregate is ~10–30µs at 8 members against a sub-millisecond budget,
  and the no-wall case is gated. But `scripts/mac-show-drill.py --load 8`, the
  repo's real perf gate, drives Zoom ingest with no tiles scene, so it exercises
  only the gated path. The wall path is **unmeasured**, and saying so is the
  honest state.
