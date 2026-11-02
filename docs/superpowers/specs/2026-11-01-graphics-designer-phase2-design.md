# Graphics Designer (titler spec Phase 2) — design

**Status:** proposed 2026-11-01. **Inputs:** `docs/titler-service-spec.md` Phase 2; the owner-reviewed
mockups (canvas "Graphics Tab Mockups", page *Designer (Phase 2)*: D1 designer, D2 start a graphic,
D3 variables, D4 animate); the gap analysis of main a6b9739f; owner rulings 2026-11-01.

## Owner rulings (binding)

1. The **Designer replaces the Advanced editor**. The five-step builder stays as the quick path
   ("Build a graphic"); Design gets a second entry, "Open in Designer".
2. **Designs live in the show file** (`GraphicsShowDocument`), survive restarts, belong to that show.
3. **Animation is per-layer in/out**: start offset, duration, easing, a small set of moves, and
   whole-graphic presets. No free-form keyframe curves this phase.
4. **Import PNG and SVG now** (SVG rasterised to PNG at import), PSD later.

## Problem (from the gap analysis)

An author can restyle an installed look but cannot build one:

- No **blank start**; Save always makes a new version of the *same* template id, so family, bounds and
  fields are frozen. No way to add a variable (e.g. `location`).
- Layer editing exists only in the Advanced tool pane (flat list, no rename/drag, image layers need a
  pre-declared asset); the builder is fill-in only.
- **Saved versions vanish on restart** unless a playlist row pins them; the native registry is in
  memory, capped at 16 definitions (incl. the 6 curated templates; the 3 compatibility looks are outside the registry) and 256 KiB, with no remove.
- Native animation is whole-graphic only (`GraphicsTimeline`: one fade/slide in/out).
- Only `rect`; no outline/shadow; width/height fixed (no grow-to-text); image import makes a separate
  "bug" template instead of an asset in the design; `.svg` is offered by the logo picker and then
  rejected (bug).

## Design

### 1. Designs: identity, storage, lifetime

- A **design** is an authored template the show owns: `GraphicsDesign { DesignId, Name, Family,
  TemplateId, TemplateVersion, Definition (manifest JSON), Assets[] (id, sha256, png bytes in the
  show's graphics folder), UpdatedAt }`, stored in `GraphicsShowDocument.Designs` (schema 2 → 3,
  additive; v2 documents load with no designs).
- `TemplateId` for a design is `design-<slug>-<6 hex>` (unique, ≤64, `[a-z0-9-]`), never a built-in id.
  Every save writes a **new template version** under the design's own id and replaces the design's
  stored definition; older versions stay pinned only while a playlist row or cue uses them.
- **Install on demand.** The shell installs a design's current version into the core when it is
  opened in the Designer, when a playlist row that pins it is cued/shown (existing pin path), and on
  core restart for designs in use. A design nobody uses is not installed, so the 16-definition cap
  bites only when more than ~10 distinct designs are on air or open at once.
- **Native `graphics-template-remove`** (new): removes a non-built-in definition version that no
  instance references; refused otherwise (`template-in-use`). The shell removes superseded design
  versions after a save when nothing pins them, and a design's versions when the design is deleted.
- **Delete design** (Designer list): refused while a playlist row uses it (message names the row).
- **Duplicate design**: new design id, same definition.

### 2. Start a graphic (mockup D2)

- **Start blank**: Lower third, Title, Bug or corner, Full screen. Each is a shell-side seed manifest
  (family, canvas 1920×1080, bounds, default placement, a plate rect, and for lower third `name`
  and `title` string fields bound to two text layers; title: `heading`/`subheading`; bug: an image
  slot; full screen: a full-canvas rect).
- **Start from a look**: any design or installed template; makes a new design (copy) — built-ins are
  never edited in place.
- Name field; "Open in the designer".

### 3. The Designer surface (mockup D1) — replaces the Advanced editor

- **Layers panel**: list front-to-back; add Text, Shape, Image…, Clock; drag to reorder; double-click to
  rename (`metadata.label`); lock/hide; duplicate; delete; group/ungroup.
- **Canvas**: existing `GraphicsAuthoringCanvas` (select, move, resize, nudge) + snap to safe area and
  centre guides; sample-text buttons (Typical / Longest name / Empty title) that feed the preview.
- **Inspector** for the selected layer: Shows (variable or fixed text), typeface (brand + safe
  families), size, weight, colour (brand tokens or hex), lines/shrink/ellipsis, **outline**, **drop
  shadow**, X/Y/W/H, opacity; shape kind, fill, stroke, radius; image fit/align; **Size to content**
  (plate grows with the longest bound text between min and max width).
- **Header**: Undo, Play preview (runs the in/out on the private preview), Save, Save to playlist (F-key).
- Everything edits the private draft; nothing reaches Preview/Program until Show/Cue in Operate.

### 4. Variables (mockup D3)

- Variables panel: add / rename / delete; kind (Text with max length, Number, Colour, On/off, Image);
  sample value; "If empty: hide its layer / show sample"; "Used by" (layers bound to it).
  Native field limits stay (≤32 fields).
- A text layer's "Shows" binds it to a variable or fixed text; an image layer to an image variable or a
  design asset; a shape's fill to a colour variable.
- **Where values come from** stays on the playlist row (shipped): Sources (whoever is on Program), a
  names list, or typed. New: a design variable other than name/title can map to a **Sources column**;
  adding a variable offers to add that column on the Sources page. Names-list CSV gains one column per
  extra variable.

### 5. Motion (mockup D4)

- Native: each layer gets optional `animations.in` / `animations.out` `{ type, startMs, durationMs,
  easing, direction?, distance? }` with types `cut`, `fade`, `slide`, `rise`, `wipe`, `grow`, `pop`;
  easing `linear`/`ease-in`/`ease-out`/`ease-in-out`; total ≤ 3000 ms. Layers without their own motion
  follow the graphic's in/out (today's behaviour, so every existing template renders unchanged).
- "Out mirrors In" (default on) reverses In.
- Presets fill every layer: *Build left to right*, *Everything fades*, *Bar first, words after*.
- UI: the motion strip under the canvas (one bar per layer, drag start / ends) and the per-layer panel.
- A playlist row's own `Motion` (shipped) still overrides the whole graphic when set.

### 6. Content additions

- Shapes: `rect` (existing), `ellipse`, `line`; stroke colour/width on rect/ellipse.
- Text: outline (colour, width ≤ 8 px), drop shadow (colour, offset, blur ≤ 16 px).
- Size to content on a group/plate: `width: "auto"` with `minWidth`/`maxWidth`; text shrinks to its
  minimum size before ellipsis.
- Images: PNG/JPEG import adds an **asset to the open design** (not a separate template); SVG is
  rasterised to PNG at import at 2× the layer's size (cap 4096 px), transparency kept. Fix the logo
  picker's `.svg` filter.

## Slices (each its own plan and PR, in order)

1. **Designs foundation** — `GraphicsShowDocument.Designs` (schema 3), design ids, start blank (4 seeds),
   start from a look (copy), save (new version under the design id), duplicate, delete, install on
   demand, native `graphics-template-remove`, superseded-version cleanup, Designer list + "Open in
   Designer" entry. Variables add/rename/delete/kind and layer binding (§4, without the Sources-column
   mapping).
2. **Designer surface** — layers panel (reorder, rename, group), inspector expansion, canvas guides and
   sample text, replace the Advanced editor (retire its pane; keep automation where it is surfaced
   today).
3. **Per-layer motion** — native per-node in/out, preset fills, motion strip + panel, preview play.
4. **Content** — ellipse/line/stroke, outline/shadow, size to content, image asset import + SVG.
5. **Sources columns** — extra variables mapped to Sources columns and names-list CSV columns.

## Out of scope

PSD import; free keyframe curves; multi-select; rotation; rulers; templates shared across shows; HTML
templates; data adapters beyond Sources/names list (Phase 3).

## Acceptance (owner at the keyboard, installed build)

1. Start a blank lower third, add a `location` variable and a third text line bound to it, save to the
   playlist on F6, Show it from Operate with a typed location — under 2 minutes, no Advanced pane.
2. Restart the app: the design is still in the Designer list and the playlist row still shows.
3. Give the plate "Wipe from left", the name "Fade + rise" 0.2 s later; Play preview; Show on air:
   the bar leads the words.
4. Import an SVG logo into the design; it appears crisp at 1080p with transparency.
5. Delete a design used by the playlist: refused with the row named.
