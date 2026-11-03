# Graphics Designer Slice 3 — Per-layer Motion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each layer of a design gets its own way in and out (start offset, duration, easing, one of seven moves). Presets fill every layer at once. The author retimes the layers on a motion strip under the canvas and plays the result on the private preview. A design shown from Operate plays the same per-layer motion on Program: "the bar leads the words".

**Architecture:** The motion lives in the template document. Each node may carry `animations.in` / `animations.out`. Native validates them at load and records them as *motion units* in the resolved drawing.

The raster owner still prepares one immutable texture per revision, so animation never changes a preparation key. A template with per-layer motion is rastered as an *atlas*: one slice per run of paint-order primitives that share a unit. The D3D11 compositor draws each slice with its own sampled transform, opacity and scissor.

The motion state is a time position, nothing more. On Program it comes from a deterministic per-instance clock (`GraphicsLayerClock`) on the injected monotonic clock. On the private preview it comes from a `play` mode the shell sends once per press. The core then ticks the preview at 30 fps with no further round trip.

Templates without per-layer motion keep today's single texture and `GraphicsTimeline` path unchanged. Shell edits are pure `GraphicsTemplateEditor` document operations through the session's private draft (one undo step each). The Designer gains a motion strip, a Motion inspector tab and Play preview.

On the Operate side, playout adopts a saved design version into an existing instance and sends the design's own motion. A row's own Motion makes the whole graphic move as one.

**Tech Stack:** C++20 native core (gtest shim, D3D11/D2D/DirectWrite dev build); C# / .NET 9 MediaCore + WinUI 3 (code-built controls), xUnit.

**Spec:** `docs/superpowers/specs/2026-11-01-graphics-designer-phase2-design.md`. Covered here: §5 Motion, §3 header "Play preview", Slices item 3, Acceptance 3, owner ruling 3. Mockups: D4 `DesignerAnimate.dc.html` (timeline strip, per-layer "how it comes in", Play In / Play Out / Loop, presets, Out mirrors In) and D1 `DesignerMain.dc.html` (motion strip under the canvas, Play preview in the header).

## Global Constraints

- **Owner ruling 3:** motion is per-layer in/out (start offset, duration, easing, a small set of moves) plus whole-graphic presets. There are no free keyframe curves.
- **Node key `animations`.** It is allowed on every node type (`group`, `rect`, `text`, `image`) and holds `{ in?, out? }` (at least one).
- **Each motion** is `{ type, startMs, durationMs, easing, direction?, distance? }`.
  - `type`: `cut`, `fade`, `slide`, `rise`, `wipe`, `grow`, `pop`.
  - `easing`: `linear`, `ease-in`, `ease-out`, `ease-in-out`. Default `ease-out`.
  - `startMs`: 0–3000. Default 0.
  - `durationMs`: 0 for `cut` (the default for cut, and required to be 0). 1–3000 for every other type (required).
  - `startMs + durationMs` ≤ 3000.
  - `direction` (`left`, `right`, `up`, `down`): only on `slide`, `wipe` and `grow`. Default `left` for slide and wipe, `down` for grow.
  - `distance`: only on `slide` and `rise`. A number 1–4096, or `"edge"`. Default `"edge"` for slide, 24 for rise.
  - At most **16** nodes per template carry `animations`.
- **Native error strings:** `template-layer-motion` (shape, type or range), `template-motion-too-long`, `template-motion-layer-limit`.
- **Shell messages:**
  - "Motion must finish within 3.0 s (starts at + takes)."
  - "Takes must be at least 0.05 s."
  - "Up to 16 layers can have their own motion. Let this one move with the graphic."
  - "Choose a move from the list."
  - "Type a time like 0.2 or 0.20 s."
- **Total.** `T_in` = max over every unit's In of (`startMs + durationMs`). When any layer follows the graphic during In, the graphic-level In duration is included too. `T_out` is computed the same way. Both are ≤ 3000 by construction. **An instance leaves air at `T_out`, when the last layer finishes.**
- **Easing.** The same curves as `GraphicsTimeline`:
  - `ease-in` = t²
  - `ease-out` = t(2−t)
  - `ease-in-out` = t²(3−2t)
  - `linear` = t
- **Out mirrors In** is on by default. It is stored as top-level `metadata.motionOutMirrorsIn: false` only when off.
  - Mirror of an In motion = same type, direction and distance; easing swapped (`ease-in` ↔ `ease-out`, the others unchanged); same duration; `startMs = T_in − (in.startMs + in.durationMs)`.
  - The graphic-level Out mirrors the graphic-level In with the easing swapped. It can only start at 0.
- **Layer-motion mode.** The instance `animations` accepts `layers: "own" | "whole"`, default `"own"`.
  - `"whole"` ignores every node's own motion. The graphic moves as one with today's `GraphicsTimeline`.
  - A playlist row's own `Motion` is sent as `"whole"`.
- **Play preview** runs only on the private preview (`lowerThirdPreview.graphics.play = { mode: "in"|"out"|"cycle"|"loop", token }`). It never touches Preview, Program or any instance. The preview ticks every **33 ms** while a play is active, otherwise 100 ms.
  - Each cycle is: In, hold 1000 ms, Out, hidden 500 ms.
  - Loop period = `T_in + 1000 + T_out + 500`.
- **UI values.**
  - Drag grid: 50 ms; Ctrl: 10 ms. The minimum non-cut duration in the UI is 50 ms.
  - Default durations when a move is first chosen: cut 0, fade 250, slide 400, rise 300, wipe 350, grow 300, pop 350. All with easing `ease-out`, except pop (`linear`).
  - Strip scale: 0 to max(1.0 s, ⌈T + 0.2⌉) s (cap 3.0). Ticks every 0.2 s up to 1.4 s, else every 0.5 s.
- **Unchanged native limits** (GraphicsTemplate.cpp):
  - ≤ 64 layers; nesting ≤ 8; ≤ 32 fields; 256 KiB manifest.
  - Template-level `animations` stays `none|fade|slide` (`GraphicsTransition`).
- **Out of scope:** Slice 4 content (ellipse, line, stroke, outline, shadow, size to content, image import); free curves; motion on the Metal or software compositors (they never drew template graphics); a playlist-row motion picker in Operate.
- **No input injection.** Tests never launch, kill or attach to a running app; the layout probe is its own isolated process.
- **Native tests:**
  - Run from `native\build-dev` with `COREVIDEO_VCAM_SHM_DIR` set and an exact `--gtest_filter=Suite.Name`.
  - **Motion time comes only from the injected clock** (`GraphicsInstances{"test-core", [&]{ return now; }}`, `GradePreviewController::tick(..., now)`, `MonitorRenderRequest` fields). The only wall-clock wait allowed is the existing bounded preparation-ready poll. Never sleep to advance motion (the lesson: wall-clock waits flake under TSan).
  - **Never range-for over a temporary `shared_ptr` member.** Bind the owner to a local first (`const auto layout = resource->runs; for (auto& r : layout->runs)`).
- **Docs-updated is part of done:** the CLAUDE.md row, `docs/reference/graphics-playout.md`, `docs/reference/volt-native-instances.md`, `docs/BACKLOG.md`.
- **Commit trailer:** `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Decisions (each pinned; ⚑ = the owner may disagree)

1. **⚑ A row's own Motion makes the whole graphic move as one.** When it is set, per-layer motion is ignored (native `layers:"whole"`, today's look). When it is unset, a design row plays the design's own motion, per-layer included. Built-in rows keep today's default fade 250/200 ms.
   - Why: "overrides the whole graphic" read literally, one rule, and Operate's "Cut" stays a real cut.
2. **Composition.** A child with no motion of its own moves with its nearest animated ancestor, never also with the graphic. A child with its own motion composes with its group's motion (transform product, opacity product, clip intersection).
   - A root-level node that defines only one phase follows the graphic for the other phase. A nested node that defines only one phase moves with its group in the other phase.
3. **Masking.**
   - Every per-layer move is masked by its parent's area: the group's padded inner box, or the graphic bounds for a root layer. So a root-level slide or wipe enters from the graphic's own edge.
   - Wipe is a scissor rect: the node's box revealed from `direction`. It is intersected with the node's mask, and that mask moves with the node's animated ancestors.
   - The graphic-level slide stays unmasked, as today.
   - The D3D scissor is integer pixels, so a wipe edge snaps to whole pixels.
4. **Moves.** All of them map shown fraction `q` (0 = hidden, 1 = shown).
   - `fade`: opacity q.
   - `slide`: offset −(1−q)·D toward `direction`.
   - `rise`: opacity q plus +(1−q)·D downward.
   - `wipe`: clip rect.
   - `cut`: hidden before `startMs`, shown after.
   - **⚑ `grow`:** scale on one axis from the edge named by `direction`; scale = q; no overshoot. The caption names the movement, so `down` (anchored at the bottom) reads "Grow up".
   - **⚑ `pop`:** uniform scale about the box centre, s = 0.5 + 0.5·backOut(q), where backOut(q) = 1 + 2.70158(q−1)³ + 1.70158(q−1)². The peak is s ≈ 1.04995 at q ≈ 0.58. Opacity = min(1, q/0.4).
   - The `"edge"` distance puts the box just outside its mask in that direction.
5. **Timing.** In: q = ease(u). Out: q = 1 − ease(u). Here u = clamp((t − startMs)/durationMs). For duration 0, u = (t ≥ startMs ? 1 : 0).
   - With these rules, a mirrored Out is the exact frame-reverse of In (proved by a test).
6. **Leaving air.** An instance leaves air when the last layer finishes (`T_out`).
   - **⚑ Reversal.** For per-layer designs, a Show during an exit plays the exit backwards from where it is. A Hide during an entrance plays the entrance backwards. No layer jumps.
   - Whole-graphic templates keep today's tween-from-current behaviour.
7. **⚑ Out mirrors In** is on by default.
   - While it is on, the Out view is read-only, with the hint "Turn off Out mirrors In to edit the way out."
   - The graphic-level Out cannot be offset in time. Layers that follow the graphic leave at 0 rather than last.
8. **Play preview.**
   - One core round trip per press. The core clocks frames at 30 fps from the demand's own start time.
   - **⚑ Play In, Play Out and Play preview stop on any edit, sample switch or undo.** Loop survives edits and restarts its cycle on each new draft revision.
   - The D1 header "Play preview" plays one cycle (In, hold 1 s, Out).
9. **Proving timing in native tests.**
   - Primitive or state asserts on the pure sampler and on the render plan at given injected ms.
   - D3D pixel asserts at given injected ms (Program and the private preview).
   - Exact-reverse asserts every 10 ms.
10. **⚑ Layout.**
    - The motion strip is a collapsible band under the canvas (centre column). It is expanded by default at ≥ 1500 px wide and collapsed at 1360×768.
    - The per-layer panel is a third inspector tab, **Motion**.
    - The strip lists layers back-to-front (paint order, as mockup D4). The Layers panel stays front-to-back.
    - The graphic-level motion is shown read-only as "With the graphic · Fade 0.25 s". Presets set it.
11. **⚑ Limits.**
    - ≤ 16 layers with their own motion.
    - The atlas is one slice per run, stacked with a 2 px gutter, and must fit 16384 px tall. Otherwise the raster fails with "Graphics motion needs more texture space than one graphic allows; give fewer layers their own motion." (cue/preparation failure; Program keeps its previous revision).
    - Per-layer designs use more GPU memory: the sum of the slice areas.

## Acceptance 3 walked end to end (who owns each step)

| Step (owner, installed build) | Code path | Task |
|---|---|---|
| Open the Slice 1/2 lower-third design in the Designer | existing `GraphicsDesignWorkflow.OpenAsync` | — |
| Select **Plate** → Motion tab → **Wipe**. From: Left; Takes 0.35 s | `GraphicsDesignerControl.MotionPanel` → `GraphicsTemplateEditor.SetLayerMotion` (+ mirror) → `EditTemplateSynced` | 7 → 6 |
| Select **Name** → **Fade + rise**; Starts at **0.20 s** | same | 7 → 6 |
| The strip shows the plate bar at 0–0.35 and the name at 0.2–0.5 | `GraphicsMotionModel.Bars` | 6, 7 |
| **Play preview** | session `PlayPreview("cycle")` → `lowerThirdPreview.graphics.play` → `GradePreviewController` 33 ms ticks → `renderGradeBatch` → per-run draw | 7 → 5 → 4 → 2 |
| **Save to playlist · F6** | existing save (new version) + `Carry` takes the saved graphic-level motion; the row is re-pinned to the new version with `Motion` still null | 6 (Carry), existing |
| Operate: **Show**. The row's instance may already exist on the older version from Acceptance 1 | `GraphicsPlayoutService.Ensure` adopts `templateVersion` + `animations{layers:"own"}` on the cue copy before Cue | 8 |
| On air: the bar wipes first, the name rises 0.2 s later, the title fades with the graphic | `GraphicsLayerClock` → plan `graphicsMotion` → atlas runs drawn per unit | 3 → 4 |
| Auto-out after 8 s (row default): the name leaves first and the bar last; the instance leaves air at `T_out` | `GraphicsLayerClock.hide` | 3 |
| A row with its own Motion (e.g. built with "Slide in from left") | `layers:"whole"`: the graphic moves as one | 8 → 3 |

## Review Focus

1. **Operate Show of a row whose instance already exists on the previous design version.** Today `Ensure` only patches fields, so the new motion would never reach air. Pinned in Task 8: `ShowAfterSavingANewVersionAdoptsItAndTheDesignsMotion`.
2. **Every existing template renders unchanged.** These templates have no per-layer motion, so they keep one run, the single-texture draw and the `GraphicsTimeline` path. Pinned in Task 4 (`TemplatesWithoutLayerMotionKeepTheLegacyDrawAndPixels`) and Task 3 (`WholeModeAndPlainTemplatesKeepTheGraphicsTimeline`).
3. **Hide when layers have different out durations, and reversal mid-transition.** The instance must leave air exactly at `T_out`, and no layer may jump. Pinned in Task 3 (`LayeredHideLeavesAirWhenTheLastLayerFinishes`, `LayeredReversalPlaysBackwardsWithoutAJump`).
4. **Atlas leaks into places that expect one picture.** Thumbnails, the cue/Preview bus (settled) and the private preview still must show the whole graphic, never stacked slices. Pinned in Task 4: `ThumbnailAndPreviewBusShowTheSettledGraphicNotSlices`.
5. **Play preview reaches Program/instances, or overflows the event queue at 30 fps.** Pinned in Task 5 (`PrivatePreviewPlaysLayerMotionWithoutTouchingProgram`, `PlayingDemandTicksAtThirtyHertzWithoutRefusedEvents`).

---

### Task 1: Native format — per-node `animations`, motion units in the drawing

**Files:**
- Create: `native/src/graphics/GraphicsMotion.h`. It holds:
  - `GraphicsBox`, moved here from `GraphicsTemplate.h:15` (`GraphicsTemplate.h` includes this header).
  - `GraphicsLayerMotion`, `GraphicsMotionUnit`, `GraphicsMotionSummary`, `GraphicsMotionPhase`, `GraphicsMotionState`.
- Modify: `native/src/graphics/GraphicsTemplate.h`:
  - `GraphicsPrimitive` :16-26 gains `int unit = -1;`.
  - `GraphicsDrawing` :28-34 gains `std::vector<GraphicsMotionUnit> units;`.
  - `GraphicsTemplate` gains `const GraphicsMotionSummary& motionSummary() const` and member `motion_`.
- Modify: `native/src/graphics/GraphicsTemplate.cpp`:
  - `Layout::visit` :183-260: add `animations` to the common `allowed` keys at :186. Add a `unit` parameter; register a unit when the node has `animations`. Tag primitives with the unit.
  - `load` :340-345: count units ≤ 16; compute the summary.
- Create: `native/tests/fixtures/graphics-motion/bar-first.json` and `nested-motion.json`.
- Test: `native/tests/GraphicsTemplateTest.cpp` (new tests after :279).

**Interfaces:**
- Produces in `GraphicsMotion.h` (namespace `corevideo::graphics`):

```cpp
enum class GraphicsMotionPhase { Settled, In, Out, Hidden };
struct GraphicsLayerMotion {
  std::string type = "cut", easing = "ease-out", direction = "left";
  double startMs = 0, durationMs = 0, distance = 24; bool distanceEdge = false;
  // Throws std::runtime_error("template-layer-motion" | "template-motion-too-long").
  static GraphicsLayerMotion read(const rpc::Json& value);
  rpc::Json json() const;            // every key written explicitly (direction/distance only where allowed)
  bool operator==(const GraphicsLayerMotion&) const = default;
};
struct GraphicsMotionUnit {
  std::string nodeId; int parent = -1;   // index of the nearest animated ancestor unit
  bool root = true;                      // no animated ancestor AND the node is a root layer
  std::optional<GraphicsLayerMotion> in, out;
  GraphicsBox box, mask;                 // reference px: the node's laid-out box; the clip passed to visit (parent area)
};
struct GraphicsMotionSummary { bool layered = false, anyFollows = true; size_t units = 0; double inMs = 0, outMs = 0; };
struct GraphicsMotionState { GraphicsMotionPhase phase = GraphicsMotionPhase::Settled; double elapsedMs = 0;
  GraphicsTransition graphicIn, graphicOut; bool operator==(const GraphicsMotionState&) const = default; };
```

- `summary.inMs` / `summary.outMs` = the max of `startMs + durationMs` over the units' own In / Out (0 when none).
- `summary.anyFollows` is true when some node has no animated self-or-ancestor, or when a root unit lacks a phase.
- `summary.layered = units > 0`.

- [ ] **Step 1: Write the failing tests and fixtures.**

**`bar-first.json`** is `contracts/graphics-seeds/lower-third.json` with these changes:
- `templateId` `motion-bar-first`, `name` "Motion bar first".
- On `plate`: `"animations":{"in":{"type":"wipe","startMs":0,"durationMs":350,"easing":"ease-out","direction":"left"},"out":{"type":"wipe","startMs":150,"durationMs":350,"easing":"ease-in","direction":"left"}}`.
- On `name-label`: `"animations":{"in":{"type":"rise","startMs":200,"durationMs":300,"easing":"ease-out","distance":24},"out":{"type":"rise","startMs":0,"durationMs":300,"easing":"ease-in","distance":24}}`.
- `accent` and `title-label` have no motion. The top-level `animations` stay the seed's (in fade 250 ease-out, out fade 200 ease-in).

**`nested-motion.json`**:
- `templateId` `motion-nested`, family `title`, bounds 800×200, brandTokens `brand.primary`, `brand.onPrimary`, `brand.accent`, `brand.fontBody`.
- Field `heading` (string, required, default "Heading").
- One root group `card` (x 100, y 20, w 600, h 120, flow none, padding 0) with `"animations":{"in":{"type":"slide","startMs":0,"durationMs":400,"easing":"linear","direction":"left","distance":200}}`.
- `card`'s children:
  - `card-text`: text x 10, y 10, w 580, h 60, bound to `heading`, font token body, fontSize 40, color token onPrimary. In: `rise` 0/400 linear, distance 24.
  - `card-bar`: rect x 0, y 100, w 600, h 20, fill token accent. In: `wipe` 0/400 linear, direction left.
- Top-level animations: none / none.

```cpp
TEST(GraphicsTemplate, LayerMotionLoadsIntoUnitsAndSummarisesTotals) {
  std::string error; const auto bar = GraphicsTemplate::load(fixture("graphics-motion/bar-first.json"), error);
  ASSERT_TRUE(bar) << error;
  EXPECT_TRUE(bar->motionSummary().layered); EXPECT_EQ(2u, bar->motionSummary().units);
  EXPECT_EQ(500., bar->motionSummary().inMs); EXPECT_EQ(500., bar->motionSummary().outMs); EXPECT_TRUE(bar->motionSummary().anyFollows);
  GraphicsDrawing d; ASSERT_TRUE(bar->resolve(Json::Object{{"name","Ada Lovelace"},{"title","Engineer"}}, brand(), d, error)) << error;
  ASSERT_EQ(2u, d.units.size());
  EXPECT_EQ("plate", d.units[0].nodeId); EXPECT_TRUE(d.units[0].root);
  EXPECT_EQ(0., d.units[0].box.x); EXPECT_EQ(1060., d.units[0].box.width); EXPECT_EQ(168., d.units[0].mask.height); // mask = graphic bounds
  EXPECT_EQ(0, primitive(d, "plate").unit); EXPECT_EQ(-1, primitive(d, "accent").unit);
  EXPECT_EQ(1, primitive(d, "name-label").unit); EXPECT_EQ(-1, primitive(d, "title-label").unit);
  const auto nested = GraphicsTemplate::load(fixture("graphics-motion/nested-motion.json"), error); ASSERT_TRUE(nested) << error;
  GraphicsDrawing n; ASSERT_TRUE(nested->resolve(Json::Object{{"heading","Welcome"}}, brand(), n, error)) << error;
  ASSERT_EQ(3u, n.units.size()); EXPECT_EQ(0, n.units[1].parent); EXPECT_FALSE(n.units[1].root);
  EXPECT_EQ(100., n.units[1].mask.x); EXPECT_EQ(120., n.units[1].mask.height);           // card's inner area
  EXPECT_TRUE(nested->motionSummary().anyFollows);   // root `card` has no "out": a root unit missing a phase follows the graphic
}
```

```cpp
TEST(GraphicsTemplate, LayerMotionValidationRefusesBadShapesWithExactReasons) {
  // mutate bar-first's plate "in" (helper withPlateIn(Json)) and expect load() to fail with the reason:
  const std::vector<std::pair<Json, std::string>> cases = {
    {M{{"type","spin"},{"durationMs",100}}, "template-layer-motion"},
    {M{{"type","fade"},{"startMs",-1},{"durationMs",100}}, "template-layer-motion"},
    {M{{"type","fade"},{"durationMs",3001}}, "template-layer-motion"},
    {M{{"type","fade"},{"startMs",2800},{"durationMs",300}}, "template-motion-too-long"},
    {M{{"type","cut"},{"durationMs",100}}, "template-layer-motion"},
    {M{{"type","fade"},{"durationMs",0}}, "template-layer-motion"},
    {M{{"type","fade"},{"durationMs",100},{"direction","left"}}, "template-layer-motion"},
    {M{{"type","rise"},{"durationMs",100},{"distance",0}}, "template-layer-motion"},
    {M{{"type","slide"},{"durationMs",100},{"direction","diagonal"}}, "template-layer-motion"},
    {M{{"type","fade"},{"durationMs",100},{"loop",true}}, "template-layer-motion"}};
  for (const auto& [motion, reason] : cases) { std::string error; EXPECT_FALSE(GraphicsTemplate::load(withPlateIn(motion), error)); EXPECT_EQ(reason, error) << motion.stringify(); }
  std::string error;
  EXPECT_TRUE(GraphicsTemplate::load(withPlateIn(M{{"type","rise"},{"durationMs",100},{"distance","edge"}}), error)) << error;
  EXPECT_FALSE(GraphicsTemplate::load(withPlateAnimations(Json::Object{}), error)); EXPECT_EQ("template-layer-motion", error);
  EXPECT_FALSE(GraphicsTemplate::load(seventeenAnimatedRects(), error)); EXPECT_EQ("template-motion-layer-limit", error);
}
TEST(GraphicsTemplate, TemplatesWithoutLayerMotionHaveNoUnits) {
  for (const auto& definition : GraphicsTemplate::curated()) { EXPECT_FALSE(definition->motionSummary().layered) << definition->id(); }
  EXPECT_FALSE(GraphicsTemplate::corporateLowerThird()->motionSummary().layered);
  // corporate resolved: units empty, every primitive unit == -1
}
```

  Helpers go in the test file:
  - `fixture(path)`: reads under `COREVIDEO_REPO_ROOT/native/tests/fixtures/`.
  - `brand()`: `{brand.primary:"#203040", brand.onPrimary:"#FFFFFF", brand.accent:"#44C1A1", brand.fontBody:"Segoe UI"}`.
  - `primitive(d, id)`.
  - `withPlateIn` / `withPlateAnimations`: these parse bar-first, mutate it and `stringify`.
  - `seventeenAnimatedRects()`: bar-first plus 15 extra `rect-1..rect-15` (x 0, y 0, w 10, h 10, fill token primary, `animations.in` fade 0/100). That makes 17 animated nodes.
  - `M`: an alias for `Json::Object`.
- [ ] **Step 2: Run.** First `npm run build:native-dev`. Then from `native\build-dev`, with `$env:COREVIDEO_VCAM_SHM_DIR="D:\CoreVideo-QA\tmp\gfx-slice3\vcam-shm"`, run `.\corevideo-native-tests.exe --gtest_filter=GraphicsTemplate.LayerMotion*:GraphicsTemplate.TemplatesWithoutLayerMotionHaveNoUnits`. Expected: a compile failure (`motionSummary`, `units` and `unit` are missing). Once the stubs compile, the tests FAIL because `animations` is an `unsupported-template-property`.
- [ ] **Step 3: Implement.**
  - `GraphicsLayerMotion::read` enforces the Global Constraints table. It accepts only the keys `type, startMs, durationMs, easing, direction, distance` and applies the type-dependent defaults.
  - `visit(..., int unit)`: when `node.get("animations")` is present, parse `{in?, out?}`. The object must be non-empty and contain only `in`/`out`, else `template-layer-motion`. Push a `GraphicsMotionUnit{nodeId, unit, unit < 0 && depth == 0, in, out, p.box, clip}`. The `clip` argument is the parent area. Recurse into children with the new unit index, and set `p.unit` for the node's own primitive. Units are recorded in validate mode as well, so `load` can count them: more than 16 gives `template-motion-layer-limit`.
  - `load`: build `motion_` from the validate-mode analysis (`Layout(...,true).run()` at :342).
- [ ] **Step 4: Run** the filter, then `--gtest_filter=GraphicsTemplate.*:GraphicsEditorShapes.*:GraphicsDesignSeeds.*`. Expected: PASS. Existing fixtures are unchanged.
- [ ] **Step 5: Commit** `Graphics native: per-layer animations in the template format (units, limits, totals)`.

### Task 2: Native motion math — sampling, composition, runs, atlas, preview play modes

**Files:**
- Create: `native/src/graphics/GraphicsMotion.cpp`. Add it to the core source list beside `src/graphics/GraphicsTemplateDraft.cpp` (`native/CMakeLists.txt:130`). It is portable and compiles in the stub builds.
- Modify: `native/src/graphics/GraphicsMotion.h`. Add the function declarations, with a forward-declared `struct GraphicsDrawing;`.
- Create: `native/tests/GraphicsMotionTest.cpp`. Register it in both test lists (`native/CMakeLists.txt:781-786` and `:981-985`).

**Interfaces (Produces):**

```cpp
double motionEase(const std::string& easing, double t);                       // same curves as GraphicsTimeline
struct GraphicsAffine { double sx = 1, sy = 1, tx = 0, ty = 0;                 // p' = (sx*x + tx, sy*y + ty)
  GraphicsBox apply(const GraphicsBox&) const; GraphicsAffine then(const GraphicsAffine& inner) const; };
struct GraphicsMotionSample { GraphicsAffine transform; double opacity = 1; std::optional<GraphicsBox> clip; };
GraphicsMotionSample sampleUnit(const GraphicsLayerMotion&, const GraphicsBox& box, const std::optional<GraphicsBox>& mask,
                                GraphicsMotionPhase, double elapsedMs);
GraphicsLayerMotion graphicMotion(const GraphicsTransition&, double width, double height); // none->cut 0, fade->fade, slide->slide distance=width|height, startMs 0
GraphicsMotionSample sampleRun(const GraphicsDrawing&, int unit, const GraphicsMotionState&);
double motionTotal(const GraphicsMotionSummary&, const GraphicsTransition& graphic, bool in);
struct GraphicsRun { int unit = -1; size_t first = 0, count = 0; GraphicsBox source; };   // source = union of the run's primitive clips
std::vector<GraphicsRun> planRuns(const GraphicsDrawing&);
struct GraphicsAtlasSlice { int x = 0, y = 0, width = 0, height = 0; };
bool packAtlas(const std::vector<GraphicsRun>&, double scale, std::vector<GraphicsAtlasSlice>& slices, int& width, int& height); // false when height > 16384
GraphicsMotionState previewMotion(const std::string& mode, double elapsedMs, double inMs, double outMs,
                                  const GraphicsTransition& graphicIn, const GraphicsTransition& graphicOut);
```

**Algorithms (pinned):**
- **`sampleUnit`.** Compute `u = clamp((e − startMs)/durationMs, 0, 1)`; when `durationMs == 0`, `u = e ≥ startMs ? 1 : 0`. Then In: `q = motionEase(easing, u)`; Out: `q = 1 − motionEase(easing, u)`; Settled: `q = 1`.
  - `D` = `distance`, or for `"edge"`: left `box.x+box.w−mask.x`; right `mask.x+mask.w−box.x`; up `box.y+box.h−mask.y`; down `mask.y+mask.h−box.y`. Clamp to ≥ 1.
  - fade: opacity q.
  - slide: `tx = −(1−q)·D` (left), `+` (right); `ty = −(1−q)·D` (up), `+` (down).
  - rise: opacity q, `ty = +(1−q)·D`.
  - wipe: clip left `{x, y, q·w, h}`; right `{x+(1−q)w, y, q·w, h}`; up `{x, y, w, q·h}`; down `{x, y+(1−q)h, w, q·h}`.
  - grow: left `sx=q` about `box.x`; right `sx=q` about `box.x+w`; up `sy=q` about `box.y`; down `sy=q` about `box.y+h`.
  - pop: `s = 0.5 + 0.5·backOut(q)` about the box centre, opacity `min(1, q/0.4)`.
  - cut: opacity `q ≥ 1 ? 1 : 0`.
  - The clip is then `mask ∩ (wipe rect or nothing)`. A scale about origin `o` is `t = o·(1−s)`.
- **`sampleRun`** (non-obvious, so written out):

```cpp
GraphicsMotionSample sampleRun(const GraphicsDrawing& d, int unit, const GraphicsMotionState& st) {
  GraphicsMotionSample out; if (st.phase == GraphicsMotionPhase::Settled) return out;
  const auto& graphic = st.phase == GraphicsMotionPhase::In ? st.graphicIn : st.graphicOut;
  const GraphicsBox whole{0, 0, d.width, d.height};
  auto apply = [&](const GraphicsLayerMotion& m, const GraphicsBox& box, const std::optional<GraphicsBox>& mask) {
    const auto s = sampleUnit(m, box, mask, st.phase, st.elapsedMs);
    if (s.clip) out.clip = out.clip ? intersect(*out.clip, out.transform.apply(*s.clip)) : out.transform.apply(*s.clip); // own clip lives in the parent's space
    out.transform = out.transform.then(s.transform); out.opacity *= s.opacity;
  };
  std::vector<int> chain; for (int u = unit; u >= 0; u = d.units[size_t(u)].parent) chain.push_back(u);
  if (chain.empty()) { apply(graphicMotion(graphic, d.width, d.height), whole, std::nullopt); return out; }
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {           // outermost first
    const auto& u = d.units[size_t(*it)];
    const auto& own = st.phase == GraphicsMotionPhase::In ? u.in : u.out;
    if (own) apply(*own, u.box, u.mask);
    else if (u.root) apply(graphicMotion(graphic, d.width, d.height), whole, std::nullopt); // root lacking this phase follows the graphic
    // nested lacking this phase: moves with its parent only
  }
  return out;
}
```

- **`planRuns`.** Walk `primitives` in paint order. Start a new run whenever `p.unit` changes. `source` = the union of `p.clip` over the run (skip zero-area clips).
- **`packAtlas`.** Each slice is `ceil(source·scale)` rounded outward to whole pixels. Slices stack vertically at x = 0 with a 2 px gutter. `width` = the widest slice; `height` = Σ (slice heights + 2) − 2. Return false when `height > 16384`.
- **`previewMotion`.** Let `Tin = inMs`, `Tout = outMs` (already include the graphic durations).
  - `in`: e < Tin → (In, e); else Settled.
  - `out`: e < Tout → (Out, e); e < Tout+500 → Hidden; else Settled.
  - `cycle`: [0, Tin) In; [Tin, Tin+1000) Settled; [Tin+1000, Tin+1000+Tout) Out; then 500 ms Hidden; then Settled.
  - `loop`: e mod (Tin+1000+Tout+500), mapped as cycle.
  - `""`: Settled.

- [ ] **Step 1: Write the failing tests.** `D(name, fields)` resolves a fixture with `brand()` (move the helper into a shared `native/tests/GraphicsMotionFixture.h` used by Tasks 1-4). `st(phase, e)` builds a state with the bar-first graphic In fade 250 ease-out and Out fade 200 ease-in.

```cpp
TEST(GraphicsMotion, EasingMatchesTheWholeGraphicTimeline) {
  EXPECT_DOUBLE_EQ(.75, motionEase("ease-out", .5)); EXPECT_DOUBLE_EQ(.25, motionEase("ease-in", .5));
  EXPECT_DOUBLE_EQ(.5, motionEase("ease-in-out", .5)); EXPECT_DOUBLE_EQ(.3, motionEase("linear", .3)); }
TEST(GraphicsMotion, BarFirstSamplesAtGivenMilliseconds) {
  const auto d = D("bar-first", {{"name","Ada Lovelace"},{"title","Engineer"}});
  const auto plate = sampleRun(d, 0, st(In, 100));
  ASSERT_TRUE(plate.clip); EXPECT_NEAR(519.1836734693877, plate.clip->width, 1e-9); EXPECT_EQ(168., plate.clip->height); EXPECT_EQ(1., plate.opacity);
  EXPECT_EQ(0., sampleRun(d, 1, st(In, 100)).opacity);                       // name starts at 200
  EXPECT_NEAR(.64, sampleRun(d, -1, st(In, 100)).opacity, 1e-12);            // title/accent follow the graphic fade
  const auto name = sampleRun(d, 1, st(In, 350));
  EXPECT_NEAR(.75, name.opacity, 1e-12); EXPECT_NEAR(6., name.transform.ty, 1e-12);
  EXPECT_NEAR(1060., sampleRun(d, 0, st(In, 350)).clip->width, 1e-9);
  EXPECT_EQ(500., motionTotal(d_summary("bar-first"), fade(250), true)); }
TEST(GraphicsMotion, MirroredOutIsTheExactFrameReverseOfIn) {
  const auto d = D("bar-first", {{"name","Ada"},{"title","Eng"}});
  for (int e = 0; e <= 500; e += 10) for (int unit : {0, 1}) {
    const auto a = sampleRun(d, unit, st(In, e)), b = sampleRun(d, unit, st(Out, 500 - e));
    EXPECT_NEAR(a.opacity, b.opacity, 1e-9) << unit << "@" << e; EXPECT_NEAR(a.transform.ty, b.transform.ty, 1e-9);
    EXPECT_EQ(a.clip.has_value(), b.clip.has_value()); if (a.clip) EXPECT_NEAR(a.clip->width, b.clip->width, 1e-9); } }
TEST(GraphicsMotion, GroupMotionComposesWithItsChildAndMovesItsMask) {
  const auto d = D("nested-motion", {{"heading","Welcome"}});      // linear, q = .5 at 200 ms
  const auto text = sampleRun(d, 1, st(In, 200));
  EXPECT_NEAR(-100., text.transform.tx, 1e-9); EXPECT_NEAR(12., text.transform.ty, 1e-9); EXPECT_NEAR(.5, text.opacity, 1e-12);
  ASSERT_TRUE(text.clip); EXPECT_NEAR(0., text.clip->x, 1e-9); EXPECT_NEAR(20., text.clip->y, 1e-9); EXPECT_NEAR(600., text.clip->width, 1e-9); }
TEST(GraphicsMotion, WipeInsideAClippedGroupIntersectsTheGroupMask) {
  const auto bar = sampleRun(D("nested-motion", {{"heading","W"}}), 2, st(In, 200));
  ASSERT_TRUE(bar.clip); EXPECT_NEAR(0., bar.clip->x, 1e-9); EXPECT_NEAR(120., bar.clip->y, 1e-9);
  EXPECT_NEAR(300., bar.clip->width, 1e-9); EXPECT_NEAR(20., bar.clip->height, 1e-9); }
TEST(GraphicsMotion, PopOvershootsAboutTheCentreAndGrowScalesFromItsEdge) {
  const GraphicsBox box{100, 50, 200, 100};
  GraphicsLayerMotion pop; pop.type = "pop"; pop.durationMs = 1000; pop.easing = "linear";
  double peak = 0; for (int e = 0; e <= 1000; ++e) peak = std::max(peak, sampleUnit(pop, box, {}, In, e).transform.sx);
  EXPECT_NEAR(1.04995, peak, 1e-4); EXPECT_EQ(1., sampleUnit(pop, box, {}, In, 1000).transform.sx);
  EXPECT_NEAR(200., sampleUnit(pop, box, {}, In, 500).transform.apply(box).x + sampleUnit(pop, box, {}, In, 500).transform.apply(box).width / 2, 1e-9); // centre fixed
  GraphicsLayerMotion grow; grow.type = "grow"; grow.direction = "down"; grow.durationMs = 1000; grow.easing = "linear";
  const auto g = sampleUnit(grow, box, {}, In, 500).transform.apply(box);
  EXPECT_NEAR(.5 * 100, g.height, 1e-9); EXPECT_NEAR(150., g.y + g.height, 1e-9); }   // bottom edge fixed
TEST(GraphicsMotion, RunsFollowPaintOrderAndPlainTemplatesHaveOneRun) {
  const auto runs = planRuns(D("bar-first", {{"name","Ada"},{"title","Eng"}}));
  ASSERT_EQ(4u, runs.size()); EXPECT_EQ(0, runs[0].unit); EXPECT_EQ(-1, runs[1].unit); EXPECT_EQ(1, runs[2].unit); EXPECT_EQ(-1, runs[3].unit);
  EXPECT_EQ(1u, planRuns(resolvedCorporate()).size()); }
TEST(GraphicsMotion, AtlasStacksSlicesWithAGutterAndRefusesOversize) {
  std::vector<GraphicsRun> runs{{0, 0, 1, {0, 0, 100.4, 50}}, {1, 1, 1, {10, 60, 40, 20}}};
  std::vector<GraphicsAtlasSlice> slices; int w = 0, h = 0;
  ASSERT_TRUE(packAtlas(runs, 1., slices, w, h));
  EXPECT_EQ(101, w); EXPECT_EQ(72, h); EXPECT_EQ(52, slices[1].y); EXPECT_EQ(40, slices[1].width);
  std::vector<GraphicsRun> tall(9, GraphicsRun{0, 0, 1, {0, 0, 10, 2000}});
  EXPECT_FALSE(packAtlas(tall, 1., slices, w, h)); }
TEST(GraphicsMotion, PreviewPlayModesMapToPhases) {   // Tin = Tout = 500
  EXPECT_EQ(phaseAt("in", 100), (P{In, 100})); EXPECT_EQ(Settled, phaseAt("in", 600).phase);
  EXPECT_EQ(phaseAt("out", 100), (P{Out, 100})); EXPECT_EQ(Hidden, phaseAt("out", 700).phase); EXPECT_EQ(Settled, phaseAt("out", 1001).phase);
  EXPECT_EQ(phaseAt("cycle", 1600), (P{Out, 100})); EXPECT_EQ(Hidden, phaseAt("cycle", 2050).phase); EXPECT_EQ(Settled, phaseAt("cycle", 2600).phase);
  EXPECT_EQ(phaseAt("loop", 2600), (P{In, 100})); }
```

- [ ] **Step 2: Run** `npm run build:native-dev`, then `--gtest_filter=GraphicsMotion.*`. Expected: compile failure; then FAIL once the declarations are stubbed.
- [ ] **Step 3: Implement** `GraphicsMotion.cpp` per the pinned algorithms (`intersect` is reused; move it from GraphicsTemplate.cpp's anonymous namespace into GraphicsMotion.h as `inline`).
- [ ] **Step 4: Run** `--gtest_filter=GraphicsMotion.*:GraphicsTemplate.*`. Expected: PASS. Also run the stub configure + build (`cmake -S native -B native/build-stub -DCOREVIDEO_STUB=ON -G Ninja; cmake --build native/build-stub`) and `--gtest_filter=GraphicsMotion.*`. Expected: PASS, which shows the code is portable.
- [ ] **Step 5: Commit** `Graphics native: per-layer motion sampling, composition, runs and atlas packing`.

### Task 3: Native instance clock — Show/Hide/reverse, `layers` mode, render-plan motion state

**Files:**
- Modify: `native/src/graphics/GraphicsMotion.h`. Add `GraphicsLayerClock`.
- Modify: `native/src/graphics/GraphicsInstances.h`:
  - `Revision` :46-60 gains `std::string layerMotion = "own";`.
  - `Instance` :61-84 gains `GraphicsLayerClock layered;`.
  - Add private `bool layered(const Revision&) const`.
- Modify: `native/src/graphics/GraphicsInstances.cpp`:
  - `readRevision` animations :162-168 (accept `layers`).
  - update validation :355-360 (size ≤ 3, `layers` ∈ own|whole).
  - `show` :965-980, hide :662-667, clear and other `timeline.clear()` sites :438, :452, :468, :661, :828, :1033.
  - `advanceAnimations` :1040-1052; `append` :783-789 (motion state); snapshot `animations` json :1087, :1104-1105 (add `layers`).
- Modify: `native/src/modules/Interfaces.h:483-516`. `CompositorOverlayContent` gains `std::optional<graphics::GraphicsMotionState> graphicsMotion;` (include `graphics/GraphicsMotion.h`). It is excluded from `overlayContentSignature`.
- Test: `native/tests/GraphicsInstancesTest.cpp`. Place the new tests after `installAndWait` (:1222). Add `motionDocument(name)`, which reads the fixture into a `Json::Object`.

**Interfaces:**

```cpp
class GraphicsLayerClock {             // deterministic: every call takes nowMs from GraphicsInstances::monotonicMs_
 public:
  void configure(double inMs, double outMs);           // totals for the program revision
  void show(double nowMs); void hide(double nowMs); void clear();
  std::string visibility(double nowMs) const;          // "hidden" | "entering" | "on-air" | "exiting"
  GraphicsMotionState state(double nowMs) const;       // phase In/Out with elapsed position, Settled when on air
  bool settle(double nowMs);                           // applies end-of-phase transitions; true when visibility changed
 private: enum class Mode { Hidden, In, OnAir, Out } mode_ = Mode::Hidden; int direction_ = 1; double anchorMs_ = 0, anchorPos_ = 0, inMs_ = 0, outMs_ = 0;
};
```

- **Position:** `pos(now) = clamp(anchorPos + direction·(now − anchorMs), 0, T(mode))`.
- **`show`:**
  - Hidden → In from 0 (+1).
  - In(−1) → In(+1) from pos.
  - Out(+1) → Out(−1) from pos.
  - Otherwise no-op.
- **`hide`:**
  - OnAir → Out from 0 (+1).
  - In(+1) → In(−1) from pos.
  - Out(−1) → Out(+1) from pos.
  - Otherwise no-op.
- **`settle`:**
  - In(+1) at T_in → OnAir.
  - In(−1) at 0 → Hidden.
  - Out(+1) at T_out → Hidden.
  - Out(−1) at 0 → OnAir.
  - `T == 0` settles immediately.
- **Visibility:**
  - In(+1) and Out(−1) → "entering".
  - In(−1) and Out(+1) → "exiting".
- **`layered(rev)`** = `rev.layerMotion == "own" && rev.definition && rev.definition->motionSummary().layered`.
  - Totals: `motionTotal(summary, rev.entrance, true)` and `motionTotal(summary, rev.exit, false)`.
  - On a layered program revision: `awaitingEntrance` is set when `T_in > 0`. `show`, `hide` and `advanceAnimations` use `layered`. `append` sets `content.graphicsMotion = layered.state(now)` (with `graphicIn/Out = rev.entrance/exit`) and leaves `content.opacity`/`rect` unanimated.
  - A non-layered revision keeps `timeline` exactly as today and leaves `graphicsMotion` empty.
  - A program swap while visible (update/Take) between layered and non-layered starts the new clock settled on air: no re-entrance.

- [ ] **Step 1: Write the failing tests.** Each creates the instance on `motion-bar-first` v1 with fields `{name:"Ada Lovelace", title:"Engineer"}`, then calls `ready(state,1)` and `trigger show`. `eligibleAt(t)` sets `now = t` and observes the `composition-eligible` fact. `motion(plan)` returns the layer's `overlay.graphicsMotion`.

```cpp
TEST(GraphicsInstances, LayeredEntranceReportsMotionStateOnTheTickClock) {
  // after show: visibility "awaiting-composition"; eligibleAt(1000)
  now = 1100; CompositorRenderPlan plan; state.append(plan, false);
  ASSERT_EQ(1u, plan.layers.size()); ASSERT_TRUE(motion(plan));
  EXPECT_EQ(GraphicsMotionPhase::In, motion(plan)->phase); EXPECT_DOUBLE_EQ(100., motion(plan)->elapsedMs);
  EXPECT_EQ(1.f, plan.layers[0].opacity); EXPECT_EQ("entering", row(state).getString("visibility"));
  now = 1499; state.observe({}); EXPECT_EQ("entering", row(state).getString("visibility"));
  now = 1500; state.observe({}); EXPECT_EQ("on-air", row(state).getString("visibility"));
  plan = {}; state.append(plan, false); EXPECT_EQ(GraphicsMotionPhase::Settled, motion(plan)->phase);
  EXPECT_EQ("own", row(state).get("animations")->getString("layers")); }
TEST(GraphicsInstances, LayeredHideLeavesAirWhenTheLastLayerFinishes) {
  // eligibleAt(0); now = 600 observe -> on-air; now = 1000; trigger hide -> "exiting"
  now = 1499; state.observe({}); EXPECT_EQ("exiting", row(state).getString("visibility")); EXPECT_EQ(1., row(state).getNumber("programRevision"));
  CompositorRenderPlan plan; state.append(plan, false); EXPECT_EQ(GraphicsMotionPhase::Out, motion(plan)->phase); EXPECT_DOUBLE_EQ(499., motion(plan)->elapsedMs);
  now = 1500; state.observe({}); EXPECT_EQ("hidden", row(state).getString("visibility")); EXPECT_EQ(0., row(state).getNumber("programRevision"));
  plan = {}; state.append(plan, false); EXPECT_TRUE(plan.layers.empty()); }
TEST(GraphicsInstances, LayeredReversalPlaysBackwardsWithoutAJump) {
  // eligibleAt(0); now = 100; hide
  CompositorRenderPlan plan; state.append(plan, false); EXPECT_EQ((GraphicsMotionState{GraphicsMotionPhase::In, 100.}), strip(motion(plan)));
  EXPECT_EQ("exiting", row(state).getString("visibility"));
  now = 150; plan = {}; state.append(plan, false); EXPECT_DOUBLE_EQ(50., motion(plan)->elapsedMs);
  trigger show; plan = {}; state.append(plan, false); EXPECT_DOUBLE_EQ(50., motion(plan)->elapsedMs); EXPECT_EQ("entering", row(state).getString("visibility"));
  now = 600; state.observe({}); EXPECT_EQ("on-air", row(state).getString("visibility"));
  trigger hide; now = 700; trigger show;              // reverse an exit at 100 ms
  plan = {}; state.append(plan, false); EXPECT_EQ(GraphicsMotionPhase::Out, motion(plan)->phase); EXPECT_DOUBLE_EQ(100., motion(plan)->elapsedMs);
  now = 800; state.observe({}); EXPECT_EQ("on-air", row(state).getString("visibility")); }
TEST(GraphicsInstances, WholeModeAndPlainTemplatesKeepTheGraphicsTimeline) {
  // create on motion-bar-first with animations {in:{type:fade,durationMs:1000}, layers:"whole"}; show; eligibleAt(0); now = 500
  CompositorRenderPlan plan; state.append(plan, false);
  EXPECT_EQ(.5f, plan.layers[0].opacity); EXPECT_FALSE(motion(plan)); EXPECT_EQ("whole", row(state).get("animations")->getString("layers"));
  // compact-solid instance (as NativeAnimationUsesMonotonicTimeAndReversesWithoutAnOpacityJump): graphicsMotion empty at every step
  EXPECT_FALSE(state.apply(request("graphics-instance-update", {{"target","cue"},{"animations",Json::Object{{"layers","sideways"}}}})));
  EXPECT_EQ("invalid-animations", lastReason(state)); }
```

  `strip()` copies the state with `graphicIn/Out` reset, so the equality compares only the phase and elapsed time.
- [ ] **Step 2: Run** `--gtest_filter=GraphicsInstances.Layered*:GraphicsInstances.WholeModeAndPlainTemplatesKeepTheGraphicsTimeline`. Expected: FAIL (missing members).
- [ ] **Step 3: Implement** the clock and the dispatch. Add a shared `readAnimations(const Json&, Revision&)` used by both `readRevision` and the update validation.
- [ ] **Step 4: Run** `--gtest_filter=GraphicsInstances.*:GraphicsMotion.*`. Expected: PASS. Every existing animation test is unchanged (`NativeAnimationUsesMonotonicTimeAndReversesWithoutAnOpacityJump`, `HideRetiresAtNativeTransitionEndWhileClearCutsImmediately`, `RepeatedShowAndContentUpdateDoNotRestartNativeEntrance`).
- [ ] **Step 5: Commit** `Graphics native: per-layer motion clock on Program (leave air at the last layer, continuous reversal, layers own|whole)`.

### Task 4: Native raster atlas and per-run compositing on Program/Preview

**Files:**
- Modify: `native/src/compositor/CompositorOverlayRaster.h:106-145` and `.cpp` `prepareRaster` :355-536 and `drawTemplate` :185-260.
  - `OverlayRasterTex` gains `std::shared_ptr<const graphics::GraphicsRunLayout> runs;`. Add `struct GraphicsRunLayout { double width, height; std::vector<GraphicsRun> runs; std::vector<GraphicsAtlasSlice> slices; int atlasWidth, atlasHeight; std::shared_ptr<const GraphicsDrawing> drawing; }` to GraphicsMotion.h.
  - `drawTemplate` gains a `(first, count, sliceOrigin)` overload.
  - Add the public `std::shared_ptr<const graphics::GraphicsRunLayout> activeRuns() const`. It reads from the active lease (cache hit **and** adopted `GraphicsPreparedResource`).
- Modify: `native/src/modules/D3D11CompositorAdapter.cpp` `drawOverlayLayer` :2022-2130.
- Test: `native/tests/GraphicsRpcPixelTest.cpp` (D3D-guarded section, beside `NativeFadeAndReversalReachActualGpuPixels` :481).

**Rules:**
- **Plain templates.** `prepareRaster` runs `planRuns` only for a template whose `motionSummary().layered` is true **and** `!request.thumbnail`. Otherwise it makes today's single full texture (`runs == nullptr`).
- **Layered templates.** `packAtlas(runs, heightPx / drawing.height, …)`. Each run is drawn by `drawTemplate` over its primitive range, with the D2D transform `Scale(scale)·Translate(slice − source·scale)`. A pack failure returns the exact message in Decision 11. The texture is `atlasWidth × atlasHeight` and its budget is charged at that size.
- **`drawOverlayLayer`** with `activeRuns()` non-null:

```cpp
const auto layout = overlayRaster_.activeRuns();                 // local owner; never range-for over a temporary member
const auto state = layer.plan.overlay.graphicsMotion.value_or(graphics::GraphicsMotionState{});
for (size_t i = 0; i < layout->runs.size(); ++i) {
  const auto& run = layout->runs[i]; const auto& slice = layout->slices[i];
  const auto s = graphics::sampleRun(*layout->drawing, run.unit, state);
  if (s.opacity <= 0) continue;
  const auto moved = s.transform.apply(run.source);               // reference px -> normalized inside `animated`
  const auto toRect = [&](const graphics::GraphicsBox& b) { return compositor::LayerRect{animated.x + float(b.x / layout->width) * animated.width,
      animated.y + float(b.y / layout->height) * animated.height, float(b.width / layout->width) * animated.width, float(b.height / layout->height) * animated.height}; };
  auto scissor = clip; if (s.clip) scissor = intersectRect(clip, toRect(*s.clip)); if (scissor.width <= 0 || scissor.height <= 0) continue;
  setScissorFromRect(scissor); setViewportFromRect(toRect(moved));
  writeLayerConstants(layer, renderPlan, 0xffffffffu, alpha * float(s.opacity),
      float(slice.width) / layout->atlasWidth, float(slice.height) / layout->atlasHeight, float(slice.x) / layout->atlasWidth, float(slice.y) / layout->atlasHeight);
  // same overlay pixel shader / premultiplied blend / Draw(3,0) as the single-texture branch
}
```

  - Confirm that the overlay pixel shader applies `uvScale`/`uvOffset` (`CompositorShaders.h:65-66, 123-124`). If it does not, add the two-line UV transform to the overlay shader only.
  - `runs == nullptr` keeps the existing single-texture branch untouched.
  - The cue/Preview bus has no `graphicsMotion`, so it draws every run Settled.

- [ ] **Step 1: Write the failing tests.**
  - The motion clock is the injected `now`; only the existing bounded preparation poll waits.
  - Program 320×180, default placement lower-left (96, −54). Map reference to frame px with `fx = 16 + x/6`, `fy = 143 + y/6`.
  - `column(frame, fx, fy)` reads one BGRA pixel.
  - `blank` is the frame rendered before any instance.

```cpp
TEST(GraphicsCompositionPixel, BarLeadsTheWordsOnProgramPixels) {
  // install motion-bar-first (graphics-template-install with the fixture text); create {name:"Ada Lovelace", title:"Engineer"}; show;
  // poll render() until preparation == "ready"; now = 0; render();           // composition-eligible starts the clock at 0
  now = 100; auto f = render();
  EXPECT_NE(px(blank, 33, 168), px(f, 33, 168));     // plate at 10 % (ref x 106, y 150) revealed: 1060*0.4898 = 519 px wide
  EXPECT_EQ(px(blank, 148, 168), px(f, 148, 168));   // plate at 75 % (ref x 795) still hidden by the wipe
  now = 150; f = render();
  EXPECT_TRUE(uniform(f, /*ref*/ 40, 30, 600, 86)); // name box (inside the 714 px revealed plate): no glyph pixels yet
  now = 600; f = render();
  EXPECT_NE(px(blank, 148, 168), px(f, 148, 168));   // plate complete
  EXPECT_GE(differentFrom(f, px(f, 30, 168), /*ref*/ 40, 30, 600, 86), 20); // name glyphs drawn
  EXPECT_EQ("on-air", row().getString("visibility")); }
TEST(GraphicsCompositionPixel, TemplatesWithoutLayerMotionKeepTheLegacyDrawAndPixels) {
  // corporate-lower-third instance shown settled: frame A (graphicsMotion empty) == frame B (same plan with overlay.graphicsMotion = Settled forced)
  // and NativeFadeAndReversalReachActualGpuPixels / EveryCuratedLookReachesActualNativeProgramPixels stay green unchanged.
}
TEST(GraphicsCompositionPixel, ThumbnailAndPreviewBusShowTheSettledGraphicNotSlices) {
  // thumbnailPng(bar-first overlay, 530, 84): IHDR width 530, height 84; decoded alpha at (398, 75) [plate 75 %, bottom strip] == 255
  // cue the bar-first instance (no show): the Preview render at now = 100 equals the Preview render at now = 600 byte-for-byte (settled, all runs)
  // and its plate pixel at ref (795,150) differs from blank.
}
```

- [ ] **Step 2: Run** `npm run build:native-dev`, then `--gtest_filter=GraphicsCompositionPixel.BarLeadsTheWordsOnProgramPixels:GraphicsCompositionPixel.TemplatesWithoutLayerMotionKeepTheLegacyDrawAndPixels:GraphicsCompositionPixel.ThumbnailAndPreviewBusShowTheSettledGraphicNotSlices`. Expected: FAIL. The bar-first frame at 100 ms shows the whole plate, because the whole texture is drawn.
- [ ] **Step 3: Implement** the atlas preparation, `activeRuns`, and the per-run draw.
- [ ] **Step 4: Run** `--gtest_filter=GraphicsCompositionPixel.*:GraphicsRpcPixel.*:GraphicsKeyPixels.*:D3D11Compositor*:GraphicsMotion.*:GraphicsInstances.*`. Expected: PASS.
- [ ] **Step 5: Commit** `Graphics native: per-layer motion on Program — atlas runs drawn with their own transform, opacity and wipe`.

### Task 5: Native private-preview Play (one round trip per press, 30 fps core clock)

**Files:**
- Modify: `native/src/modules/Interfaces.h:1225-1232`. `GradePreviewDemand` gains `std::string playMode; double playElapsedMs = 0;`.
- Modify: `native/src/core/GradePreviewController.h`:
  - `configure` :30-55 reads `lowerThirdPreview.graphics.play.mode`.
  - `Entry` gains `Clock::time_point playStarted`. It is reset when the demand revision increases.
  - `tick` :101-150 sets `demand.playElapsedMs` and uses the interval `PlayInterval = 33ms` while any entry plays. Otherwise it uses `Interval`.
  - `diagnosticsFor` keeps `requestedIntervalMs` 100 and adds `playIntervalMs` 33.
- Modify: `native/src/graphics/GraphicsInstances.cpp` `authoringPreview` :97-140:
  - Accept key `play` (draft object ≤ 9 keys). `play` is `null`, or exactly `{mode: in|out|cycle|loop, token: integer 0..2147483647}`; anything else returns `{}`.
  - Copy `revision.entrance/exit` into the overlay, so the compositor knows the graphic-level motion: `overlay.graphicsMotion = GraphicsMotionState{Settled, 0, entrance, exit}`.
- Modify: `native/src/modules/D3D11CompositorAdapter.cpp` `renderGradeBatch` :945-990.
  - When `demand.playMode` is non-empty: take `inMs`/`outMs` from the resolved definition's summary and `motionTotal`, then set `key.overlay.graphicsMotion = previewMotion(...)`. A `Hidden` result skips the key layer.
  - While playing, set `observation.sourceFrameId = request.sequence`.
- Test: `native/tests/GradePreviewTest.cpp`, `native/tests/IsolatedMonitorPixelTest.cpp`.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(GradePreviewControl, PlayElapsedComesFromTheDemandClockAndRestartsOnANewRevision) {
  core::GradePreviewController control; PreviewCompositor compositor; const auto t0 = core::GradePreviewController::Clock::now();
  auto request = canvasRequest(1, Json::Object{{"mode","in"},{"token",1}});   // lowerThirdPreview.graphics.play; graphicsDraft supplied as in AuthoredStillCanvas test
  ASSERT_TRUE(control.configure(request, {}, true, t0, overlay()));
  control.tick({}, compositor, 1, t0 + 250ms); EXPECT_EQ("in", compositor.last.gradePreviews[0].playMode); EXPECT_DOUBLE_EQ(250., compositor.last.gradePreviews[0].playElapsedMs);
  ASSERT_TRUE(control.configure(canvasRequest(2, Json::Object{{"mode","in"},{"token",1}}), {}, true, t0 + 300ms, overlay()));
  control.tick({}, compositor, 2, t0 + 400ms); EXPECT_DOUBLE_EQ(100., compositor.last.gradePreviews[0].playElapsedMs); }
TEST(GradePreviewControl, PlayingDemandTicksAtThirtyHertzWithoutRefusedEvents) {
  // configure playing demand at t0; for i in 1..30: tick at t0 + i*34ms, drain() after each -> 30 submits, 30 events, eventsRefused() == 0
  // configure the same id with play null (revision 2): ticks at +34 ms intervals submit only every >= 100 ms (3 submits in 300 ms)
}
TEST(GraphicsInstances, AuthoringPreviewAcceptsPlayAndCarriesTheGraphicMotion) {
  // draft for motion-bar-first + play {mode:"cycle",token:4}: overlay has graphicsMotion with graphicIn fade 250, graphicOut fade 200
  // play {mode:"spin",token:1} -> no overlay; play {mode:"in"} (no token) -> no overlay; snapshot instances stay empty
}
TEST(GradePreviewPixels, PrivatePreviewPlaysLayerMotionWithoutTouchingProgram) {
  // monitor request with the bar-first authoring overlay: playMode "in", playElapsedMs 100 vs 600 -> preview pixels at the plate's 75 % column differ
  // (hidden at 100, drawn at 600); playMode "out", playElapsedMs 700 -> plate column equals the empty sample canvas
  // a Program render of an empty plan before/after is byte-identical; GraphicsInstances snapshot has no instance
}
```

- [ ] **Step 2: Run** `--gtest_filter=GradePreviewControl.Play*:GraphicsInstances.AuthoringPreviewAcceptsPlayAndCarriesTheGraphicMotion:GradePreviewPixels.PrivatePreviewPlaysLayerMotionWithoutTouchingProgram`. Expected: FAIL.
- [ ] **Step 3: Implement.** Also read `native/src/core/MediaCore.cpp` around the `gradePreviews_.tick` call (:6153) and confirm that its events are drained on every loop iteration. If they are drained less often than every 33 ms, report it and stop: the 30 fps rate would refuse events.
- [ ] **Step 4: Run** `--gtest_filter=GradePreviewControl.*:GradePreviewPixels.*:GraphicsInstances.*:IsolatedMonitorPixels.*`. Expected: PASS.
- [ ] **Step 5: Commit** `Graphics native: Play preview on the private preview (in/out/cycle/loop at 30 fps, core clock)`.

### Task 6: MediaCore — motion edits, mirror, presets, strip model, wire models, editor fixture

**Files:**
- Create: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsTemplateEditor.Motion.cs`, `native-shell/CoreVideoPro.MediaCore/Services/GraphicsMotionModel.cs`.
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsTemplateEditor.cs`:
  - `LayerAllows` :148 never allows `animations` through `Set`.
  - `Duplicate` :121-127 refuses a copy beyond 16 animated layers.
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsTemplateEditor.Structure.cs`. `Ungroup` drops the group's motion (children keep their own).
- Modify: `native-shell/CoreVideoPro.MediaCore/Models/NativeGraphicsState.cs:174-178`. `NativeGraphicsAnimations` gains `public string Layers { get; init; } = "own";`.
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsCommandGateway.cs:297-323`. `ValidAnimations` accepts `layers` with `own|whole`.
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsWorkspaceSession.cs:187-197`. `EditTemplateSynced` sets `Animations = GraphicsTemplateEditor.GraphicMotion(edited)` in the same step.
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsDesignWorkflow.cs:183-187`. `Carry` uses `Animations = template.Animations` (the saved manifest's) instead of `typed.Animations`.
- Create (generated by the test below): `native/tests/fixtures/graphics-editor/designer-motion.json`. Add it to the `EditorOutputShapesLoadNatively` list (`native/tests/GraphicsEditorShapesTest.cpp:10`).
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsTemplateEditorMotionTests.cs`, `GraphicsMotionModelTests.cs`, `GraphicsCommandGatewayTests.cs`, `GraphicsWorkspaceSessionTests.cs`, `GraphicsDesignServiceTests.cs`.

**Interfaces:**
- `public sealed record GraphicsLayerMotion(string Type, double StartMs, double DurationMs, string Easing = "ease-out", string? Direction = null, double? Distance = null, bool FromEdge = false);`
- On `GraphicsTemplateEditor`:
  - Common behaviour: these throw `InvalidDataException` (bad value) and `InvalidOperationException` (refusal). `SetLayerMotion` and `ApplyMotionPreset` may run on locked layers (motion is not geometry).
  - `GraphicsLayerMotion? LayerMotion(NativeGraphicsScratch s, string id, string phase)`.
  - `NativeGraphicsScratch SetLayerMotion(NativeGraphicsScratch s, string id, string phase, GraphicsLayerMotion? motion)`.
    - `null` = moves with the graphic. It removes that phase; with no phases left it removes `animations`.
    - When mirror is on, the call is refused for `phase == "out"` ("Turn off Out mirrors In to edit the way out."). Every `in` edit rewrites every layer's `out` and the graphic-level `out` by mirror.
    - Values are written explicitly: always `type, startMs, durationMs, easing`; `direction` for slide/wipe/grow; `distance` (number or `"edge"`) for slide/rise.
  - `bool OutMirrorsIn(NativeGraphicsScratch s)`.
  - `NativeGraphicsScratch SetOutMirrorsIn(NativeGraphicsScratch s, bool on)`. On writes the mirror immediately; off keeps the current outs.
  - `NativeGraphicsScratch ApplyMotionPreset(NativeGraphicsScratch s, string presetId)` (presets below; always turns mirror on).
  - `NativeGraphicsAnimations GraphicMotion(NativeGraphicsScratch s)` (top-level `animations`, `Layers = "own"`).
  - `GraphicsLayerMotion Mirror(GraphicsLayerMotion inMotion, double inTotalMs)`.
- `public sealed record GraphicsMotionBar(string LayerId, string Label, int Depth, bool Follows, string Type, double StartMs, double DurationMs, string Caption, string Tooltip);`
- On `public static class GraphicsMotionModel`:
  - `IReadOnlyList<GraphicsMotionBar> Bars(NativeGraphicsScratch s, string phase)`. Layers come back-to-front in document order, depth-first (a group, then its children). A following layer's bar spans the graphic-level motion, with caption "With the graphic · Fade" and `Follows = true`.
  - `double Total(NativeGraphicsScratch s, string phase)`. Same rule as native `T_in`/`T_out`.
  - Choice lists, each as `IReadOnlyList<(string Id, string Label)>`:
    - `Moves`: (cut "Cut"), (fade "Fade"), (rise "Fade + rise"), (slide "Slide"), (wipe "Wipe"), (grow "Grow"), (pop "Pop").
    - `Feels`: (ease-out "Ease out"), (ease-in "Ease in"), (ease-in-out "Smooth"), (linear "Linear").
    - `Directions(type)`: slide/wipe (left "From left"), (right "From right"), (up "From top"), (down "From bottom"); grow (down "Up"), (up "Down"), (left "Right"), (right "Left").
    - `Distances(type)`: rise (12 "12 px"), (24 "24 px"), (48 "48 px"), (edge "From below the plate"); slide (edge "From outside its area"), (24 "24 px"), (48 "48 px"), (96 "96 px").
    - `Presets`: (build-left-to-right "Build left to right"), (everything-fades "Everything fades"), (bar-first "Bar first, words after").
  - `GraphicsLayerMotion DefaultFor(string type)`. Durations come from the Global Constraints.
  - `string Caption(GraphicsLayerMotion m)`: "Cut", "Fade", "Fade + rise", "Slide from left", "Wipe from left", "Grow up" (direction down), "Grow down" (up), "Grow right" (left), "Grow left" (right), "Pop".
  - `(double StartMs, double DurationMs) Drag(double startMs, double durationMs, string handle, double deltaMs, bool fine)`.
    - `handle` is `body`, `start` or `end`. The grid is 50 ms, or 10 ms when `fine`.
    - `start` keeps the end fixed.
    - Durations clamp to ≥ 50 and start + duration ≤ 3000.
  - `double ParseSeconds(string text)`. Accepts "0.2", "0.20 s", ".2s"; otherwise throws `InvalidDataException("Type a time like 0.2 or 0.20 s.")`.

**Presets (exact):**
- Presets give root-level layers (and groups) their own In, clear every nested layer's own motion, set the graphic-level In to fade 250 ms ease-out, and write the mirror. "Shapes" are rect and image; "words" are text (incl. clock) and groups.
- **Build left to right:** root layers ordered by `x` ascending (ties: document order). The i-th gets wipe left, start min(i·120, 2700), 300 ms, ease-out.
- **Everything fades:** every root layer gets fade, start 0, 250 ms, ease-out.
- **Bar first, words after:**
  - Every root shape: wipe left, start 0, 350 ms, ease-out.
  - Root words ordered by `y` ascending (ties: document order). The j-th gets rise distance 24, start min(200 + j·100, 2700), 300 ms, ease-out.

- [ ] **Step 1: Write the failing tests.** `Seed` = `GraphicsDesignSeeds.Manifest("lower-third","design-test-a1b2c3",1,"Test")` as a scratch.

```csharp
[Fact] public void SettingInWritesTheMirroredOutForEveryLayer()
{   var s = GraphicsTemplateEditor.SetLayerMotion(Seed, "plate", "in", new("wipe", 0, 350, "ease-out", "left"));
    s = GraphicsTemplateEditor.SetLayerMotion(s, "name-label", "in", new("rise", 200, 300, "ease-out", Distance: 24));
    Assert.Equal(new GraphicsLayerMotion("wipe", 150, 350, "ease-in", "left"), GraphicsTemplateEditor.LayerMotion(s, "plate", "out"));
    Assert.Equal(new GraphicsLayerMotion("rise", 0, 300, "ease-in", Distance: 24), GraphicsTemplateEditor.LayerMotion(s, "name-label", "out"));
    Assert.Equal(500, GraphicsMotionModel.Total(s, "in")); Assert.Equal(500, GraphicsMotionModel.Total(s, "out"));
    Assert.Equal("Turn off Out mirrors In to edit the way out.", Assert.Throws<InvalidOperationException>(() => GraphicsTemplateEditor.SetLayerMotion(s, "plate", "out", new("fade", 0, 200))).Message); }
[Fact] public void MotionLimitsUseTheShellMessages()
{   Assert.Equal("Motion must finish within 3.0 s (starts at + takes).", Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetLayerMotion(Seed, "plate", "in", new("fade", 2800, 300))).Message);
    Assert.Equal("Takes must be at least 0.05 s.", Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetLayerMotion(Seed, "plate", "in", new("fade", 0, 40))).Message);
    Assert.Equal("Choose a move from the list.", Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetLayerMotion(Seed, "plate", "in", new("spin", 0, 300))).Message);
    var full = SixteenAnimatedLayers();   // seed + 12 AddLayer shapes, each with fade 0/100
    Assert.Equal("Up to 16 layers can have their own motion. Let this one move with the graphic.", Assert.Throws<InvalidOperationException>(() => GraphicsTemplateEditor.SetLayerMotion(GraphicsTemplateEditor.AddLayer(full, "shape").Scratch, "shape-13", "in", new("fade", 0, 100))).Message);
    Assert.Throws<InvalidOperationException>(() => GraphicsTemplateEditor.Duplicate(full, "plate", out _)); }
[Fact] public void BarFirstPresetLeadsWithShapesThenWordsByHeight()
{   var s = GraphicsTemplateEditor.ApplyMotionPreset(Seed, "bar-first");
    Assert.Equal(new GraphicsLayerMotion("wipe", 0, 350, "ease-out", "left"), GraphicsTemplateEditor.LayerMotion(s, "plate", "in"));
    Assert.Equal(new GraphicsLayerMotion("wipe", 0, 350, "ease-out", "left"), GraphicsTemplateEditor.LayerMotion(s, "accent", "in"));
    Assert.Equal(new GraphicsLayerMotion("rise", 200, 300, "ease-out", Distance: 24), GraphicsTemplateEditor.LayerMotion(s, "name-label", "in"));
    Assert.Equal(new GraphicsLayerMotion("rise", 300, 300, "ease-out", Distance: 24), GraphicsTemplateEditor.LayerMotion(s, "title-label", "in"));
    Assert.Equal(new NativeGraphicsTransition { Type = "fade", DurationMs = 250, Easing = "ease-out" }, GraphicsTemplateEditor.GraphicMotion(s).In);
    Assert.True(GraphicsTemplateEditor.OutMirrorsIn(s)); }
[Fact] public void BuildLeftToRightAndEverythingFadesFillEveryRootLayer() // left-to-right: plate & accent x 0 (doc order) -> 0, 120; name/title x 32 -> 240, 360; all wipe left 300; fades: all fade 0/250
[Fact] public void PresetsClearNestedMotionAndUngroupDropsTheGroupsMotion() // Group(title-label) -> group-1; preset -> group-1 has In, title-label none; SetLayerMotion(group-1) then Ungroup -> no "animations" on any former child that had none
[Fact] public void BarsListBackToFrontWithCaptionsAndFollowers()
{   var s = GraphicsTemplateEditor.SetLayerMotion(Seed, "plate", "in", new("wipe", 0, 350, "ease-out", "left"));
    var bars = GraphicsMotionModel.Bars(s, "in");
    Assert.Equal(new[] { "plate", "accent", "name-label", "title-label" }, bars.Select(b => b.LayerId));
    Assert.Equal(("Wipe from left", false), (bars[0].Caption, bars[0].Follows));
    Assert.Equal(("With the graphic · Fade", true, 0d, 250d), (bars[1].Caption, bars[1].Follows, bars[1].StartMs, bars[1].DurationMs)); }
[Fact] public void DragSnapsToTheGridAndKeepsWithinThreeSeconds()
{   Assert.Equal((200d, 300d), GraphicsMotionModel.Drag(180, 300, "body", 22, fine: false));
    Assert.Equal((210d, 290d), GraphicsMotionModel.Drag(200, 300, "start", 12, fine: true));
    Assert.Equal((2700d, 300d), GraphicsMotionModel.Drag(2600, 300, "body", 400, fine: false));
    Assert.Equal((0d, 50d), GraphicsMotionModel.Drag(0, 300, "end", -400, fine: false));
    Assert.Equal(0.2, GraphicsMotionModel.ParseSeconds("0.20 s")); Assert.Throws<InvalidDataException>(() => GraphicsMotionModel.ParseSeconds("soon")); }
[Fact] public void EditorMotionOutputMatchesTheNativeFixture() // see the op sequence below; JsonNode.DeepEquals with designer-motion.json; set UPDATE_GRAPHICS_FIXTURES=1 to (re)write it
// GraphicsCommandGatewayTests
[Fact] public void AnimationsAcceptLayersOwnOrWholeOnly() // {"in":{...},"layers":"whole"} valid; "layers":"sideways" invalid; "layers":1 invalid
// GraphicsWorkspaceSessionTests
[Fact] public void MotionEditsKeepTheDraftsGraphicMotionInStep() // EditTemplateSynced(ApplyMotionPreset bar-first) -> Draft.Animations.In == fade 250 ease-out, Out == fade 250 ease-in; one Undo restores both scratch and Animations
// GraphicsDesignServiceTests
[Fact] public async Task SaveCarriesTheSavedGraphicMotionNotTheOldDraftsOne() // design seed (fade 250/200) -> preset build-left-to-right -> SaveAsync -> Draft.Animations.Out.DurationMs == 250 and Out.Easing == "ease-in"
```

  The fixture op sequence starts from `GraphicsDesignSeeds.Manifest("lower-third","design-fixture-motion",1,"Motion fixture")`:
  1. `ApplyMotionPreset("bar-first")`.
  2. `SetLayerMotion(accent, in, grow down 0/300 ease-out)`.
  3. `AddLayer shape` → `shape-1`, then `SetLayerMotion(shape-1, in, pop 400/200 linear)`.
  4. `AddLayer shape` → `shape-2`, then `SetLayerMotion(shape-2, in, cut 450/0)`.
  5. `AddLayer text` → `text-1`, then `SetLayerMotion(text-1, in, fade 100/250 ease-out)`.
  6. `Group(title-label)` → `group-1`, then `SetLayerMotion(group-1, in, slide left edge 0/400 ease-in-out)`.

  The result covers all seven moves, a nested composition and mirrored outs.
- [ ] **Step 2: Run** `dotnet test native-shell\CoreVideoPro.MediaCore.Tests --filter "FullyQualifiedName~GraphicsTemplateEditorMotionTests|FullyQualifiedName~GraphicsMotionModelTests|FullyQualifiedName~AnimationsAcceptLayersOwnOrWholeOnly|FullyQualifiedName~MotionEditsKeepTheDraftsGraphicMotionInStep|FullyQualifiedName~SaveCarriesTheSavedGraphicMotionNotTheOldDraftsOne"`. Expected: FAIL (types missing).
- [ ] **Step 3: Implement.**
  - Reuse `Walk`, `Save`, `LabelOf` and `KeepRequiredFieldsUsed`.
  - Mirror recomputation: (1) compute `T_in` with `GraphicsMotionModel.Total(s,"in")`; (2) set every layer with an `in` to `out = Mirror(in, T_in)`; (3) set the graphic-level out to mirror the graphic-level in.
  - Generate the fixture with `UPDATE_GRAPHICS_FIXTURES=1`. Read it back with the repo-root walk (`LifecycleContractTests.FixturesPath` pattern, `native/tests/fixtures/graphics-editor/designer-motion.json`).
- [ ] **Step 4: Run** the filter, then the full `dotnet test native-shell\CoreVideoPro.MediaCore.Tests`. Then `npm run build:native-dev` and `--gtest_filter=GraphicsEditorShapes.EditorOutputShapesLoadNatively`. Expected: PASS. If native refuses the fixture, the editor is wrong: fix the editor, never the fixture by hand.
- [ ] **Step 5: Commit** `Graphics designer: per-layer motion edits, Out mirrors In, presets and the motion strip model`.

### Task 7: Designer UI — motion strip, Motion tab, Play preview, layout probe

**Files:**
- Create: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsDesignerControl.Motion.cs` (the strip) and `GraphicsDesignerControl.MotionPanel.cs` (the Motion tab).
- Modify: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsDesignerControl.cs`:
  - Header :55-80: "Play preview" between Undo and Save.
  - `BuildCentre` :82-104: rows = canvas *, sample bar Auto, motion strip Auto.
  - `UpdatePreviewLease` :140-149: `draft.ForPreview(_session.PreviewPlay)`.
  - `Reflow` :109-114: strip collapsed below 1500 px unless the author expanded it this session.
- Modify: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsDesignerControl.Inspector.cs:14-48`. Add the third tab "Motion" (AutomationName "Motion tab").
- Modify: `native-shell/CoreVideoPro.MediaCore/Models/LowerThirdPreview.cs:25-28`:
  - Add `public sealed record NativeGraphicsPreviewPlay(string Mode, int Token);`.
  - `NativeGraphicsPreview` gains `NativeGraphicsPreviewPlay? Play = null`.
  - `ForPreview(NativeGraphicsPreviewPlay? play = null)`.
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsWorkspaceSession.cs`:
  - Add `public NativeGraphicsPreviewPlay? PreviewPlay { get; }`, `public void PlayPreview(string mode)` (mode `in|out|cycle|loop`; increments the token; raises `PreviewChanged`) and `public void StopPreviewPlay()`.
  - `Edit`, `Undo`, `SetPreviewSample`, `Browse` and `Rebase` clear a one-shot play (`in|out|cycle`) and keep `loop`.
- Modify: `native-shell/CoreVideoPro.WinUI/Services/GraphicsWorkspaceLayoutProbe.cs` (`CheckDesignerAsync`).
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsWorkspaceSessionTests.cs`, the probe.

**Layout (mockups D1 + D4):**
- **Strip header row** (`MOTION`):
  - Toggles "In · {T_in:0.0#} s" / "Out · {T_out} s" (AutomationNames "Show the way in", "Show the way out").
  - Checkbox "Out mirrors In" (AutomationName "Out mirrors In").
  - "Play In", "Play Out", "Loop" (a toggle; AutomationName "Loop preview").
  - A collapse chevron (AutomationName "Show motion" / "Hide motion").
  - Hint "Each bar is one layer's move. Drag to retime." (13 px, muted).
- **Body:** 140 px label column + scale. One 22 px row per bar, max 6 rows visible (scroll beyond).
  - Bar colours by move: wipe `#4A7CC9`, grow `#B5463F`, rise `#2E8A73`, fade/cut `#2E6E8A`, slide `#8A6E2E`, pop `#6B5BAA`. A following bar uses `StudioBrushes.Border` with muted text.
  - Bar AutomationName: "{Label} motion, starts {s:0.00} s, takes {d:0.00} s".
  - Drag the body = move; drag the 8 px ends = retime. Ctrl = 10 ms; the drag is wrapped in `BeginGesture`/`EndGesture`.
  - Keys: Left/Right move ±50 ms; Shift+Left/Right change the length ±50 ms.
  - Clicking a bar selects its layer.
  - Dragging a following bar first turns it into the layer's own motion (copy of the graphic-level motion).
  - In the Out view while mirror is on, bars are disabled and the hint reads "Turn off Out mirrors In to edit the way out."
- **Motion tab** (`{LABEL} · HOW IT COMES IN` / `HOW IT LEAVES`):
  - Checkbox "Moves with the graphic". When checked, the controls below are disabled and `SetLayerMotion(..., null)`.
  - 7 move buttons in a 4-column grid (Cut, Fade, Fade + rise, Slide / Wipe, Grow, Pop). Selecting a move writes `DefaultFor(type)` and keeps the current start.
  - "Starts at" and "Takes" text boxes (commit on Enter or lost focus via `ParseSeconds`; refusals go to the status line).
  - "Feel" combo; "From" combo (slide/wipe/grow; grow labelled "Grows"); "Rises by" / "Travels" combo (rise/slide).
  - "Whole-graphic presets": three buttons, plus the hint "A preset fills every bar; adjust any one afterwards."
  - Every write goes through `Apply(...)` (`EditTemplateSynced`, one undo step).
- **Header "Play preview"** (AutomationName "Play preview") = `PlayPreview("cycle")`. The status line shows "Playing the way in and out on the private preview." Disabled when the draft has no scratch or template (same as Save).

- [ ] **Step 1: Write the failing tests.**

```csharp
// GraphicsWorkspaceSessionTests
[Fact] public void PlayPreviewIsPrivateAndOneShotsStopOnEdit()
{   var session = Browsed(DesignSeedTemplate());
    session.PlayPreview("in"); var first = session.PreviewPlay; Assert.Equal("in", first!.Mode);
    session.PlayPreview("in"); Assert.Equal(first.Token + 1, session.PreviewPlay!.Token);       // a second press replays
    Assert.Equal(session.PreviewPlay, session.Draft!.ForPreview(session.PreviewPlay).Play);
    session.SetField("title", JsonSerializer.SerializeToElement("Host")); Assert.Null(session.PreviewPlay);
    session.PlayPreview("loop"); session.SetField("title", JsonSerializer.SerializeToElement("Guest")); Assert.Equal("loop", session.PreviewPlay!.Mode);
    session.SetPreviewSample("blank"); Assert.Equal("loop", session.PreviewPlay!.Mode);
    session.StopPreviewPlay(); Assert.Null(session.PreviewPlay);
    Assert.Empty(_commands.Where(c => c.Action.StartsWith("graphics.instance", StringComparison.Ordinal))); }
```

  The probe (`CheckDesignerAsync`, after the Slice 2 checks, at 1320×640 and then 1560×772):
  - At 1320×640: the strip is collapsed; "Show motion" is visible and inside the designer; canvas ≥ 640×360.
  - Invoke "Show motion": 4 bar rows are visible, labelled Plate, Accent bar, Name, Title. Canvas ≥ 480×270. The designer `ActualWidth` ≤ the page width.
  - At 1560×772: the strip is expanded; canvas ≥ 640×360.
  - The elements "Play preview", "Play In", "Play Out", "Loop preview", "Out mirrors In" and "Motion tab" have `ActualHeight > 0` and lie inside the designer.
  - Select the Plate row; Invoke "Motion tab"; Invoke "Wipe". The plate bar's AutomationName is "Plate motion, starts 0.00 s, takes 0.35 s", and the session draft's plate In type is `wipe`.
  - Invoke "Bar first, words after". The Name bar starts at 0.20 s.
  - Invoke "Play preview". `session.PreviewPlay.Mode == "cycle"`.
  - Invoke "Undo". The preset is gone; `PreviewPlay` is null.
  - Screenshots: `graphics-designer-motion-1360x768.png`, `graphics-designer-motion-1600x900.png`.
- [ ] **Step 2: Run** `dotnet test native-shell\CoreVideoPro.MediaCore.Tests --filter FullyQualifiedName~PlayPreviewIsPrivateAndOneShotsStopOnEdit`. Expected: FAIL. Build with `dotnet build native-shell\CoreVideoPro.WinUI -c Release -p:Platform=x64`, then run `powershell -File scripts\test-studio-layout.ps1 -ReportDirectory D:\CoreVideo-QA\tmp\gfx-slice3-probe-t7a`. Expected: graphics FAILS at "Show motion".
- [ ] **Step 3: Implement** the session play state, the strip, the Motion tab, the header button and the probe checks.
  - Confirm that `ColorGradeEditorViewModel` redraws the native surface when `texture.frameNumber` changes even if `sourceFrameId` and `revision` are unchanged. If it keys on `sourceFrameId`, Task 5 already sends the request sequence while playing; if it keys on revision, make it key on `frameNumber` for `preview:lower-third` only.
- [ ] **Step 4: Run** the full MediaCore suite and the WinUI tests: `dotnet test native-shell\CoreVideoPro.WinUI.Tests -c Release -p:Platform=x64 -p:WindowsAppSDKBootstrapAutoInitializeOptions_Default=false -p:WindowsAppSDKBootstrapAutoInitializeOptions_None=true`. Expected: the known #1088 failure excepted. Rebuild, then run the probe with `-ReportDirectory …-t7b`. Expected: all kinds pass. Look at both screenshots against D1/D4.
- [ ] **Step 5: Commit** `Graphics designer: motion strip, Motion tab and Play preview on the private preview`.

### Task 8: Operate — the saved version and its motion reach air; row Motion moves the whole graphic; docs

**Files:**
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsPlayoutService.cs` `Ensure` :331-372:
  - Motion choice: `row.Motion is { } m ? m with { Layers = "whole" } : GraphicsDesignIds.IsDesignTemplateId(templateId) ? installed.Animations with { Layers = "own" } : Motion`.
  - Existing instance: when `existing.TemplateVersion != version` or `existing.Animations != motion`, send `graphics.instance.update` `{target:"cue", templateVersion: version, animations: motion, fields}` (plus `binding` when present), even when the fields are the same. Today it returns "Ready." on unchanged fields.
- Docs:
  - `CLAUDE.md:206` (graphics-playout row: add **Motion** (slice 3): `GraphicsMotion` native units/clock/atlas, `GraphicsTemplateEditor.Motion.cs`, `GraphicsMotionModel`, Play preview).
  - `docs/reference/graphics-playout.md`: a new "## Motion (Graphics Designer slice 3)" after :221-308 "The Designer". It covers the moves table, Out mirrors In, presets, strip, Play preview, the row-Motion rule and the leave-air rule. Also update "Where things live" :425-441 and "Rules to keep" :497-508.
  - `docs/reference/volt-native-instances.md:162-168` (`layers:own|whole`; node `animations`; the reversal rule for layered templates).
  - `docs/BACKLOG.md:428` (Slice 3 status, "not live-verified").
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsPlayoutServiceTests.cs`.

- [ ] **Step 1: Write the failing tests.** Reuse this file's fake core (`_commands` log, state builder, template install), as `PlaylistReinstallEvictsUnusedDesignVersionsWhenTheLibraryIsFull` does.

```csharp
[Fact] public async Task ShowAfterSavingANewVersionAdoptsItAndTheDesignsMotion()
{   // design "design-guest-a1b2c3": v1 installed + pinned (seed motion), row on F6, Motion null; PlayInAsync -> instance created at v1
    // the owner saves motion: v2 installed + pinned whose top-level animations are in fade 250 ease-out / out fade 250 ease-in and whose plate has animations.in wipe
    // row re-pinned to v2 (TemplateVersion = 2); the fake core reports the instance at TemplateVersion 1
    var shown = await service.PlayInAsync(entry);
    Assert.True(shown.Accepted, shown.Reason);
    var update = _commands.Single(c => c.Action == "graphics.instance.update");
    Assert.Equal(2, update.Body.GetProperty("templateVersion").GetInt32());
    Assert.Equal("own", update.Body.GetProperty("animations").GetProperty("layers").GetString());
    Assert.Equal(250, update.Body.GetProperty("animations").GetProperty("out").GetProperty("durationMs").GetDouble());
    Assert.True(_commands.IndexOf(update) < _commands.FindIndex(c => c.Action == "graphics.instance.cue")); }
[Fact] public async Task RowMotionMakesTheWholeGraphicMoveAsOne()
{   // design row with Motion = GraphicsBuilder.MotionFor("slide-left", 300)
    await service.PlayInAsync(entry);
    var create = _commands.Single(c => c.Action == "graphics.instance.create");
    Assert.Equal("whole", create.Body.GetProperty("animations").GetProperty("layers").GetString());
    Assert.Equal("slide", create.Body.GetProperty("animations").GetProperty("in").GetProperty("type").GetString()); }
[Fact] public async Task BuiltInRowsKeepTheDefaultFade() // corporate-lower-third row, Motion null -> create animations in fade 250 ease-out, out fade 200 ease-in, layers "own"
[Fact] public async Task UnchangedVersionAndMotionStillSkipTheUpdate() // second PlayIn with nothing changed -> no graphics.instance.update (today's fast path kept)
```

- [ ] **Step 2: Run** `dotnet test native-shell\CoreVideoPro.MediaCore.Tests --filter "FullyQualifiedName~GraphicsPlayoutServiceTests"`. Expected: the first two FAIL. `BuiltInRowsKeepTheDefaultFade` and `UnchangedVersionAndMotionStillSkipTheUpdate` pass, which pins the unchanged paths.
- [ ] **Step 3: Implement** `Ensure`. Write the docs, including:
  - the Acceptance 3 script;
  - "a row's own Motion makes the whole graphic move as one";
  - "per-layer moves are masked by their parent's area";
  - "an instance leaves air when its last layer finishes";
  - "Play preview never reaches Preview/Program".
  - Then run `rg -n "per-layer|Slice 3" docs CLAUDE.md` to check the wording.
- [ ] **Step 4: Run** the full `dotnet test native-shell\CoreVideoPro.MediaCore.Tests` and `dotnet test native-shell\CoreVideoPro.Control.Tests`. Run the WinUI test command from Task 7. Then `npm run build:native-dev` and `--gtest_filter=GraphicsTemplate.*:GraphicsMotion.*:GraphicsInstances.*:GraphicsCompositionPixel.*:GraphicsRpcPixel.*:GradePreview*:GraphicsEditorShapes.*:ContractParity.*`. Expected: PASS (the known pre-existing `AdapterConformanceTests` golden excepted).
- [ ] **Step 5: Commit** `Graphics: a saved design's version and motion reach air; row Motion moves the whole graphic; docs`.

---

## Acceptance hand-off (owner, installed build — spec Acceptance 3)

1. Design → Your designs → open your lower-third design (or New design → Lower third, blank → Open in Designer).
2. In the Layers panel select **Plate**. Open the **Motion** tab, choose **Wipe** (From: Left), and set Takes to **0.35 s**.
3. Select **Name**. Choose **Fade + rise** and set Starts at to **0.20 s**. The motion strip under the canvas (Show motion at 1360×768) shows the plate bar first and the name bar starting at 0.2.
4. Press **Play preview** (header). The private preview plays the way in, holds 1 s and plays the way out. Program and Preview do not change. Optional: press **Loop** and drag a bar; the loop restarts with the new timing.
5. Press **Save to playlist · F6**.
6. Operate: **Show** the row. On air the bar wipes in from the left first, the name rises 0.2 s later, and the title and accent fade with the graphic. After 8 s (or Play Out) the name leaves first and the bar last. The graphic is off air when the bar finishes.
7. Optional: choose **Bar first, words after** for the same look in one click.

---

## Self-review

- **Spec coverage.**
  - §5 node `animations` in/out with the 7 types, 4 easings and ≤ 3000 ms: Task 1 (format) and Task 2 (behaviour).
  - Layers without motion follow the graphic, with unchanged existing templates: Tasks 1, 3 and 4 (Review Focus 2).
  - Out mirrors In: Task 6, proved exact in Task 2.
  - The three presets: Task 6, with UI in Task 7.
  - Motion strip with drag start/ends, and the per-layer panel: Task 7.
  - Play preview on the private preview only: Tasks 5 and 7.
  - Row Motion overrides the whole graphic: Tasks 3 and 8.
  - Acceptance 3: walked in the table above; every step has an owning task, including the Operate half (Task 8).
  - Owner ruling 3 (no free curves): there is no curve editor anywhere.
  - Out of scope respected: Slice 4 content and free curves.
- **Step scan.** Every task has failing tests with exact values, a run command with the expected failure, an implement step, a re-run and a commit. Native runs use exact gtest filters from `native\build-dev` with `COREVIDEO_VCAM_SHM_DIR`. Motion time is always injected. The bounded preparation poll is the only wall-clock wait.
- **Type consistency.**
  - `GraphicsLayerMotion` exists in both native (C++ struct) and shell (C# record). The wire keys are the same, and the editor writes them explicitly.
  - `GraphicsMotionState` and `GraphicsMotionPhase` are used by the instance plan (Task 3), the compositor (Task 4) and the preview (Task 5).
  - `NativeGraphicsAnimations.Layers` (Task 6) is consumed by the gateway (Task 6) and playout (Task 8).
  - `NativeGraphicsPreviewPlay(Mode, Token)` (Task 7) maps to native `play {mode, token}` (Task 5).
- **Review Focus.**
  - Item 1 → Task 8 `ShowAfterSavingANewVersionAdoptsItAndTheDesignsMotion`.
  - Item 2 → Task 4 `TemplatesWithoutLayerMotionKeepTheLegacyDrawAndPixels` and Task 3 `WholeModeAndPlainTemplatesKeepTheGraphicsTimeline`.
  - Item 3 → Task 3 `LayeredHideLeavesAirWhenTheLastLayerFinishes` and `LayeredReversalPlaysBackwardsWithoutAJump`.
  - Item 4 → Task 4 `ThumbnailAndPreviewBusShowTheSettledGraphicNotSlices`.
  - Item 5 → Task 5 `PrivatePreviewPlaysLayerMotionWithoutTouchingProgram` and `PlayingDemandTicksAtThirtyHertzWithoutRefusedEvents`.
- **Fixed inline.**
  - Task 1's `anyFollows` expectation for `nested-motion` corrected to `true`: a root unit missing a phase follows the graphic.
  - `Carry` switched to the saved template's animations. Otherwise the pre-save draft's graphic motion would overwrite a preset's mirrored Out after Save.
- **Proportion.** The four native tasks carry the risk: format, math, clock, and raster + compositor. They come first and each is independently testable. Task 5 (preview Play) is kept apart from Task 4 so that the Program path can be reviewed alone. Task 7 is the only WinUI task. Task 8 is small but load-bearing: it closes the acceptance path's Operate half, the gap Slice 2's final review caught.
