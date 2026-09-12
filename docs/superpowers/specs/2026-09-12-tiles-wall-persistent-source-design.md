# The Tiles wall as a persistent source (persistent-sources slice 2)

**Status:** approved in conversation 2026-09-12; not implemented.
**Issue:** [#448](https://github.com/iamfatness/CoreVideoPro/issues/448) (T5.2).
**Answers:** [#447](https://github.com/iamfatness/CoreVideoPro/issues/447) (T5.1) —
which #419 foundations slices 2-3 build on.
**Parent spec:** `docs/superpowers/specs/2026-09-10-persistent-sources-design.md`
(slice 2 of its section 5 phasing).

## 1. Why

A Tiles wall cued in Preview and taken to Program re-animates on the cut. The
owner's words, live show 2026-09-09:

> I am ok if panelists leave and join the video but what I can't have is a total
> rerender from what is in preview to program like it is loading for the first
> time.

That report produced a **partial** fix, which is what ships today:
`TilesPlanAnimation::adoptSettledFrom` MOVES spring state from the preview
animator to the program animator on the take tick. It is scoped hard — exact wall
key match, and **every sampled tile must be `atRest`**:

```cpp
// compositor/TilesPlanAnimation.h
for (const auto& tile : previous.sampled_) {
  if (!tile.atRest) return false;
}
```

So a wall taken **mid-animation still re-animates**. The hand-off refuses it
deliberately — "mid-flight state belongs to the bus that is flying it" — because
with two animators there is no correct answer. That residual defect is #448.

The structural cause is that the wall's animation lives on the **bus**
(`MediaCore::programTilesAnimation_`, `MediaCore::previewTilesAnimation_`), so a
cut is a hand-over between two owners rather than a change of who is looking at
one object. The parent spec's model says a bus owns no source state: "A cut
changes which stack a bus samples and nothing else."

This slice also carries the **first real consumer** of the #419 realtime
architecture foundations, so those foundations can land on `main` wired rather
than as an unwired island (CLAUDE.md: "A #419 architecture foundation lands on
`main` only together with its first real consumer").

## 2. The model and identity

**The wall becomes a source.** A `TilesWallSource` owns three things that live on
the buses today:

- the spring animator (`compositor::TilesAnimator`),
- one transparent, canvas-sized texture,
- an input signature: admitted members and their frame ids, wall settings,
  canvas dimensions.

It re-composites **only** when the signature changes. A settled wall on static
input does no GPU work.

**Identity is the Tiles layer id, which is already `tiles:<sceneId>`.**

Verified, not assumed: the shell builds it as
`LayerId: $"tiles:{scene.Id}"` (`TilesLayerPayloadBuilder.cs:50`) and the core
takes it verbatim off the wire (`tiles.layerId = node.getString("layerId")`,
`MediaCore.cpp:1808`). So it is unique per scene and already carries the
`tiles:` prefix the parent spec's `tiles:<wallId>` calls for.

Today's animation key is `sceneId + ":" + layerId`, i.e. `<sceneId>:tiles:<sceneId>`
— the scene id twice. Dropping the redundant prefix yields exactly the same
equivalence classes: a gallery settled in Preview and the same gallery a Take
puts on Program still share one identity (which is why `adoptSettledFrom` can
match them at all), and two different scenes' walls remain distinct. This slice
therefore changes **continuity**, not **which walls are the same wall**.

A consequence worth stating: because identity is scene-derived, editing a scene
in place keeps one wall, while a different scene is always a different wall.

Cross-scene wall sharing — one wall referenced by two different scenes — is the
parent spec's "placeable wall source" and is explicitly **out of scope**.

**Registration.** `SourceRegistry` (from #419) gains a composed kind. A wall
registers on first scene reference and is released when no scene references it,
per the parent spec's Lifetime rule ("A source referenced by no scene is released
after a short grace period").

The registry's existing fields are capture-oriented — `externalId`,
`subscriptionRequested`, `subscriptionObserved`, `availability` — and are
meaningless for a composed source. The composed kind carries only `Token`,
`displayName`, `format` and publication state. Five inert fields on a type is how
a registry starts lying about what it knows; the type says what applies.

**What this deletes:** `adoptSettledFrom`, `programTilesAnimation_`,
`previewTilesAnimation_`, and the take-tick hand-off in
`MediaCore::renderSyntheticTick` (`MediaCore.cpp:6266-6277`). With one animator
there is nothing to hand over — which is precisely why a wall taken mid-animation
becomes continuous.

**What `AtomicTakeCoordinator` buys.** The cut becomes one atomic revision
transition, so "which bus samples this wall" changes at a single defined instant.
That also closes the race CLAUDE.md documents on the take record:

> If a repeating spine sync applies the swapped Preview (the outgoing scene)
> BEFORE that `load-scene-graph` lands, the "outgoing Preview plan" is already
> the new one and the before-union can miss incoming sources.

## 3. Components and data flow

Three units, each understandable and testable alone.

| Unit | Owns | Depends on |
|---|---|---|
| `core/TilesWallSource` | animator, input signature, and the decision "does this wall need re-compositing this tick?" | nothing GPU — pure, unit-testable |
| `SourceRegistry` (composed kind) | the wall's `Token` and lifetime by scene reference | #419 |
| compositor adapters | a `wallTextures_` map keyed by `wallId` -> render target + SRV | per-backend |

Per render tick:

1. The gather asks each referenced wall's source whether its signature changed.
2. If so, the adapter re-composites that wall into its own texture.
3. Each bus that references the wall draws that texture as **one layer**.

**The plan carries one wall layer.** The render plan emits a single
`tiles-wall:<wallId>` layer instead of N expanded `tile:` layers. This is a
deliberate choice over collapsing the expanded layers inside the D3D adapter:
grouping layers by wall membership inside a backend is inference that rots, and
slice 3 (transitions blending finished bus images) wants the bus-image shape
anyway.

The cost is that **Metal and the CPU preview must implement the new layer kind in
this slice**, pulling some slice-4 parity work forward. That is accepted.

**Drawing rides machinery that already exists.** `ResolvedLayer::retainedProgram`
is an `ID3D11ShaderResourceView*` already used to draw the delivered Program
texture into the multiview PGM cell (`D3D11CompositorAdapter.cpp:323-340`). The
wall texture and the PVW cell below both use that same path.

**The multiview PVW cell samples the Preview bus texture.** Today it
re-composites the whole preview stack into the PVW sub-rect
(`MediaCore::buildMultiviewRenderPlan`, the `hasPreviewScene()` branch), remapping
every preview layer. It will instead sample the preview composite the core already
produces, the way the PGM cell already samples the delivered Program texture. This
is the parent spec's "Also" item, included here at the owner's direction.

**Threading is unchanged.** Everything stays on the single render thread and
immediate context. No second device. No pixel work under `coreMutex` or on a hot
tick.

## 4. Invariants this must not break

Every rule here was written after something broke on air. They are constraints on
the design, not advice.

1. **An empty render plan is not "draw nothing."** All three compositors
   improvise a full-canvas grid per decoded frame when a plan has zero layers —
   on PROGRAM, which the virtual camera, recordings and streams inherit. The
   wall's background layer is emitted **above** the admission gate today and must
   stay there. Collapsing to one layer must never make an all-stale wall emit
   zero layers.
2. **`wall.present` ALONE decides whether a wall is active** — never
   `present && !members.empty()`. `members: []` is an ordinary state (every
   camera off, or a momentarily empty roster), and a members-aware gate re-opened
   the on-air hole once already.
3. **The stale rules are asymmetric and stay that way.** A stale *tile* is
   refused — it occupies a slot and would seat a dead guest. A stale *background*
   is held (`compositor::tilesBackgroundSourceIsDrawable`) — a backdrop frozen
   for a beat is invisible, where its absence is a full-frame colour change on
   air. `kTilesStaleFrameMs` (1500 ms) does not move: it is shared with tile
   admission and changing it changes wall membership for every source.
4. **The all-stale transient must not wipe retained tiles.** Today's guard is
   `if (targets.empty() && !sampled_.empty()) return;`. With one animator this
   matters MORE, not less: a single wipe would now affect every bus at once.
5. **Ordering gets structurally safer, and that is worth keeping.** Today routes
   and the wall share one order namespace (tiles-bg at `wall.order`, tile *i* at
   `wall.order + 1 + i`), so a surviving gallery route at order 2 can composite
   BETWEEN tiles. One wall layer makes that impossible by construction.
6. **The `tiles` snapshot node stays published unconditionally.** A node that
   vanishes in exactly the case worth detecting is the multiviewer mistake.
7. **Borders stay multiview-only.** Route layers are forced to
   `borderStyle="none"`; a wall layer must not become a new way to composite an
   adornment into Program.

**Failure handling.** If a wall texture cannot be created (device loss, resource
exhaustion), the wall draws its **background but not its tiles**, and says so
through `sceneValidationWarnings_` — never a silent empty layer set, never an
improvised grid. Wall textures are per device generation: they retire with the
generation and are rebuilt on adoption, like every other per-generation resource
under `DeviceLossPolicy`.

## 5. Proof

"Nothing re-rendered" is measured, not eyeballed.

**The headline test, and it must be RED first.**
`AWallTakenMidAnimationIsContinuous` — cue a wall in Preview, Take it **while
tiles are still flying**, assert the wall's generation does not change and
sampled tile positions continue rather than restart. Today this must FAIL:
`adoptSettledFrom` refuses a non-settled wall by design, so the program animator
resets. **The red is verified by reverting the change, not by assuming it** —
three tests in the 2026-09-12 session initially passed without their fix.

1. **Generation counters.** The wall carries `generation` (bumped on animator
   reset or texture recreate) and `lastFrameId`, and rides the take record
   through the existing `core/SourceContinuityLedger.h`. A take record cannot
   read `cut` while the wall's generation moved.
2. **Pixel continuity.** `ProgramPixelContinuityTest.cpp` gains a wall case using
   the luma-comb technique. A re-animation is a luma discontinuity on the ticks
   around the Take and fails regardless of what the counters say.
3. **Live soak.** `scripts/qa/live-meeting-soak.mjs --takes N` against a real
   meeting. **It has never been run** (CLAUDE.md records this), so slice 2 is the
   first time; expect it to surface its own problems.

**Rewritten, never deleted** — these pin today's per-bus contract and must pin the
one-animator contract instead: `TilesRenderPlanTest.cpp:519,916-1081`,
`TilesAnimatorTest.cpp:97-163`, `RenderedSceneAttributionTest.cpp:196`.

**New, for the #419 wiring:** `SourceRegistry` accepting and releasing a composed
wall by scene reference; the Take running through `AtomicTakeCoordinator` as one
revision transition, including the interleaving race it closes.

## 6. What lands, and what does not

**The carve.** Only the #419 foundations this slice actually consumes come to
`main`, together with slice 2, as one reviewable change: `SourceRegistry` (plus
the composed kind), `AtomicTakeCoordinator`, and their tests. The rest of #419
stays a draft and shrinks as later slices land.

**Gates, and their honest state:**

| Gate | Status |
|---|---|
| `MonitorRenderFaultInjection` on a quiet machine | can be run here before merge |
| Integrated-GPU budget number | **CANNOT be produced** — needs the reference machines in #425. Recorded as an unmet gate, not skipped quietly |
| Metal parity | compiles and passes CI's `native-metal-macos`; **not executed** — no Mac here, and the M3 Max is QA-only |

**Out of scope:** the placeable wall source and scene-as-source; per-layer in/out
animation; scene UI changes; lower-thirds and graphics as composed sources
(slice 3); full Metal and CPU-preview parity beyond the new layer kind (slice 4).

## 7. Risks

- **This slice touches the render thread**, which is the highest-consequence code
  in the product. Fault-injection tests run on a quiet machine before merge; a
  loaded machine cannot gate a timing property at any threshold.
- **The one-layer plan change reaches macOS**, which cannot be executed here.
  A Metal regression would be invisible until someone runs the M3 Max.
- **The live soak is unproven tooling.** Its first real run is part of this slice,
  so a soak failure may be the harness rather than the product — that must be
  diagnosed, not assumed either way.
- **The 16-decoder cap** (`OwnedMediaFrameSource.h`) becomes a real limit as
  sources outlive buses. It must warn loudly, never drop silently.
- **Scope grew during design, deliberately.** The owner chose option C (wall
  source + PVW cell) and plan shape (ii) (one wall layer, absorbing parity work).
  Both are recorded here so the size is not a surprise at review.
