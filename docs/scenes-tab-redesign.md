# Scenes Tab Redesign

**Date:** 2026-07-04 · **Status:** proposed design, pre-implementation · **Owner report:**
"scene building (creating/editing layouts — sources, positions) doesn't work well; full
layout/UX redesign." Code audit anchors verified at `8c8fee4`.

## 1. Headline finding

**The wiring is fine; the interaction model is the problem.** Everything the UI exposes reaches
the core scene graph correctly (source picker, rect drag, fit, borders, zoom/pan/tilt, grades,
presets → `SourceRoute` → `load-scene-graph`/`set-preview-scene` → `computeSourceFraming`). What
breaks building is what's MISSING or DANGEROUS:

| # | Pain (ranked) | Evidence |
|---|---|---|
| 1 | **Cannot delete a source from a scene** — add is append-only; no remove command exists anywhere | `SourcesPage.xaml:199-360` (card has no delete), `StudioViewModel.cs:10924` |
| 2 | **Cannot reorder layers** — z-order is hardwired to add-order (`ZIndex = LayerIndex`), so overlap/PiP compositions are unbuildable predictably | `SceneCanvasLayerViewModel.cs:347` |
| 3 | **Presets are destructive** — they overwrite every rect/framing AND re-point every route's source assignment; no undo | `SceneCanvasLayoutService.cs:39-52`, `SceneRoutingService.cs:135-164` |
| 4 | **Editing a live scene mutates program directly** — cueing the program scene to preview and editing pushes straight to air (`SyncLiveSceneEditIfNeeded`); no working copy | `StudioViewModel.cs:11043-11049` |
| 5 | **Preview edits don't reliably re-composite the core preview bus** — canvas WYSIWYG can diverge from the downstream preview monitor until the next full sync | `StudioViewModel.cs:10379-10399` |
| 6 | **Drag-only placement** — no numeric X/Y/W/H, no snapping/guides/nudge/aspect-lock, single corner grip | `SceneCanvasEditorControl.xaml.cs:396-440,192-203` |
| 7 | **Building is split across two tabs** — scene list/create/remove live on the Studio rail; the canvas lives on the Scenes tab; "Open Scene builder" merely switches tabs | `StudioWorkspace.xaml:268-423`, `StudioViewModel.cs:3421` |
| 8 | **New/Save/Update overlap and clobber by name** (Save silently overwrites a same-named scene; Save with empty name becomes Update); no Duplicate | `StudioViewModel.cs:2413-2524` |
| 9 | **Scenes are process-memory only** — `_scenes`/`_sceneRoutes` dictionaries, never persisted; custom scenes die with the app | `StudioViewModel.cs:419,2470-2473` |
| 10 | **Per-layer opacity is stranded** — the compositor supports it (`Interfaces.h:253`) but `SceneRouteState`/the wire/the UI never carry it | `MediaCore.h:170-197` |
| 11 | Canvas box ↔ property card unlinked (index-only); framing sliders in opaque units; border color is a raw hex TextBox | `SceneCanvasEditorControl.xaml.cs:21`, `SceneCanvasLayerViewModel.cs:69`, `SourcesPage.xaml:245` |

Also kept (works, don't rebuild): the live-preview canvas itself (`VideoSurfaceHost` per layer),
drag/resize/pan-in-box interaction, per-layer color grade, preset geometry math, preview-cue and
Take flow.

## 2. Design principles

- **One surface owns building.** The Scenes tab gets the scene LIST (create/duplicate/rename/
  delete) next to the canvas; the Studio rail keeps only select/cue (its actual show-time job).
- **Editing is safe by default.** Edits apply to a working copy; "Update" commits. Editing a
  scene that is live on program NEVER changes air until committed (fixes #4). Committing pushes
  the preview composite immediately (fixes #5).
- **The three missing primitives come first**: delete layer, reorder layer, opacity (#1, #2,
  #10). Nothing else matters until a source can be removed and layered.
- **Presets are starting points, not resets**: apply-geometry-only (assignments preserved) with
  a one-step undo (fixes #3).
- **Precision is available, never required**: numeric rect fields, snap-to-guides, arrow-key
  nudge, aspect lock — alongside the existing freehand drag (fixes #6).

## 3. Implementation phases (each shippable)

| Phase | Scope | Size | Status |
|---|---|---|---|
| S1 | **Missing primitives**: per-layer Delete + Move Up/Down (real zIndex through the wire) + opacity (new field on `SourceRoute` → wire → `SceneRouteState` → render plan; slider on the card). Native + C# + tests | M | **SHIPPED** #168 |
| S2 | **Safety**: non-destructive presets (geometry-only + keep assignments; snapshot-undo), Duplicate scene, single Save model (Save commits the working copy; Save As creates; no silent same-name overwrite), working-copy editing for live scenes, commit re-pushes preview | M | **SHIPPED** — S2a #169 (presets/undo/duplicate/no-clobber + persistence, pulled forward from S4) + S2b #171 (live-scene draft editing) |
| S3 | **Precision**: numeric X/Y/W/H on the card, snapping (edges/centers/thirds), arrow-key nudge, aspect-lock resize, edge handles, shared canvas↔card selection + highlight | M | **PARTIAL** — S3a #173 (numeric fields, snap guides, arrow-key nudge). S3b open: aspect-lock, edge handles, canvas↔card selection sync |
| S4 | **Consolidation/polish**: scene list moves onto the Scenes tab, preset thumbnails, border color picker, cover/contain fit options, scene persistence to disk (with the show/production profile) | M-L | open — persistence already shipped early in S2a |

Also shipped beyond the original plan: **R1 production roles** #174 (section below).

## 4. Acceptance

- A source can be removed from a scene in one click, and layers can be reordered; a PiP built
  over a full-screen source stays on top after save/reload.
- Applying a preset to a hand-built scene keeps every source assignment and can be undone.
- Editing the on-air scene changes nothing on program until Update; the preview monitor shows
  the edit as it is made.
- A box can be placed at exactly x=0.25 w=0.25 numerically, nudged by keyboard, and snapped to
  another box's edge.
- Custom scenes survive an app restart (S4).

## R1 — Production roles (shipped)

Owner ask (2026-07-04): identify the Host / Question Reader / etc. so scenes can
be built against ROLES and stay valid no matter who joins ("Host + Active
Speaker", "Host + Reader"), and so automated mixing can reason in roles.

Decisions (owner): fixed role set (Host, Co-host, Reader, Panelist, Guest 1-4);
assignments are session-only by design (rosters change every show — the scene
persists the ROLE target, never the person).

Shipped shape:
- Assign: role dropdown on each Zoom feed-health row (Inputs tab). One holder
  per role — assigning Host to B strips it from A.
- Target: "Role: X" entries in Add source and in every layer's source picker.
  A role route stores ProductionRoleId (persists with saved scenes); the
  participant is resolved at sync time by ResolveRouteFromShowInput. Unassigned
  role -> placeholder slate, exactly like a fixed route to an absent person.
- Automation: the assigned role REPLACES the Zoom-derived role on the
  participant wire, so the core director/automation sees show roles.

Later: role-based scene templates in the rail ("Host + Reader" one-click),
automation rules keyed on roles ("when Reader speaks..."), custom role names.
