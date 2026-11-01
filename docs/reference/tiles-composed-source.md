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
