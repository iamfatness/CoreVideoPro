# Sources Redesign, Direct Positioning & Browser Sources — Plan & Spec

Status: owner-requested 2026-07-11 ("Sources screen is very brittle… uvc and screen
selection management seems odd… not intuitive and clunky"; "move and resize sources on
the preview panel and possibly the program feed"; "bring in browser sources").
Companion docs: `capture-sources-spec.md` (browser/screen architecture — Phase BR is the
build target of §C), `scenes-tab-redesign.md` (the canvas-editing machinery §B reuses),
`operator-performance-plan.md` (capture-path stability context).

---

## A. Sources page — evaluation and redesign

### A1. What's wrong today (evaluated against the code, not vibes)

The top workflow strip (1 Assign inputs → 2 Route outputs → 3 Compose scenes) is good
and stays. The slot table below it is where the brittleness lives:

1. **Type-switch wiped the source list** *(fixed 2026-07-11, `5bb3c70`)*: switching a
   slot's TYPE rebuilt options without the media-asset/audio lists
   (`ShowInputSlotViewModel.Kind` setter passed 2 of 4 lists), so "Media asset" showed
   an empty dropdown that still *displayed* the previous device's text and a stale NAME.
   The owner's screenshot (media slot showing "Game Capture HD60 S+" / "Steven Smith")
   is this bug. Fixed, but it exposes the structural problem: **TYPE and SOURCE are two
   coupled dropdowns that must be kept consistent by hand.**
2. **Silent auto-assignment** (`ShowInputSlotViewModel.RefreshSourceOptions`, the
   fallback at the "not in options → FirstOrDefault" branches): when a slot's source
   isn't in the rebuilt list, the slot silently grabs the FIRST available source. A
   device appearing/disappearing (or a type switch) can re-point a slot at something
   the operator never chose — mid-show. A production tool must never reassign an input
   by itself.
3. **No empty states.** An empty option list renders as a blank dropdown (which WinUI
   fills with stale text). "No media assets — add them on the Media tab" or "No signal"
   never appears anywhere.
4. **Two competing selection surfaces.** The slot table assigns capture devices, AND
   the "Capture devices" panel below has its own Primary/Secondary "Dual capture"
   pickers + per-device cards. Two id spaces, two mental models, no indication which
   one wins. (The dual-capture panel predates the 10-slot table; it is now redundant.)
5. **TYPE dropdown is operator burden.** The operator must know a Game Capture is
   "UVC webcam" vs "Screen" vs "Blackmagic" before the SOURCE list is even populated.
   The type is derivable from the source — it should never be asked.

### A2. Redesign (Phase SRC-1..3)

**Model: pick a SOURCE, not a type-then-source.** One flat, grouped source picker per
slot; the slot's kind is inferred from what was picked.

- **SRC-1 — one picker + explicit empty states + no silent reassign** (the de-brittle
  core, shippable alone) — **SHIPPED 2026-07-11** (`BuildUnifiedSourceOptions` +
  `SelectedUnifiedSourceId`/`IsSourceMissing` + single SOURCE column w/ kind chip and
  MISSING badge; auto-pick removed; 8 new tests):
  - Replace TYPE+SOURCE with a single grouped ComboBox per slot: `Cameras` (UVC +
    Blackmagic/AJA), `Screens`, `Zoom guests`, `Media`, `SRT`, later `Browser`.
    Groups with zero entries render a disabled row: "No media assets — add on the
    Media tab" (with the tab as a hyperlink), "No Zoom guests — join a meeting".
  - Kill the auto-pick: a slot whose source disappears goes to an explicit
    `Missing — was <name>` state (amber chip, keeps the binding so the source
    re-attaches when the device returns — the vMix behavior). Never substitute.
  - Placeholder "Select a source" whenever unbound; a bound slot always renders
    name + kind chip + connect state (`live / connecting / no signal / missing`).
- **SRC-2 — retire the dual-capture panel** — **SHIPPED 2026-07-11**: fold per-device rows (resolution/fps,
  signal, paired-audio picker, Connect toggle, Grade) into a compact "Devices" drawer
  under the table. One selection surface. The Primary/Secondary pickers go away —
  slots ARE the selection.
- **SRC-3 — polish**: drag a device row onto a slot to assign; per-slot preview
  thumbnail on hover (reuse the existing per-source preview pixels); inline rename kept
  (it feeds lower thirds — see the display-name override path, which already works).

Guardrail carried over from the capture work: any state the UI shows must come from
snapshot data, and a missing/failed source must be LOUD (chip + warning), never a
stale-text dropdown.

---

## B. Direct positioning on Preview/Program (PIP, bugs, exact placement)

### B1. What already exists (don't rebuild it)

The Scenes tab canvas editor (S1–S3b, shipped) already has: drag, 4-corner resize
grips, Shift aspect-lock, snap guides (edges/centers/thirds), numeric X/Y/W/H fields,
arrow-key nudge, z-order, per-layer opacity — and S2b's **draft editing** (editing the
on-air scene mutates a draft shown on the PREVIEW bus; program is untouched until
Update/Take). The core compositor honors per-route rects end-to-end. **The machinery is
done; the gap is that it lives on the Scenes tab instead of under the operator's cursor.**

### B2. Spec (Phase POS-1..3)

- **POS-1 — edit-in-place on the Studio PREVIEW panel**: overlay the existing
  `SceneCanvasEditorControl` interaction layer directly on the preview panel (same
  hit-testing, snapping, grips), editing the S2b draft. A small toolbar toggle on the
  preview header: `Edit layout` (pencil). Click a source → grips appear; drag/resize/
  nudge exactly as on Scenes; Take/Update commits as today. No new wire — it drives
  the same route rects.
- **POS-2 — "bug" / undersized-asset placement**: an `Add overlay layer` action
  (preview toolbar + Scenes) that places a media asset (or, later, browser source) at
  its native size scaled to a chosen fraction, NOT canvas-filling; presets for the
  classic spots (corners w/ safe-area margin) plus free placement; aspect always locked
  for logos; persists in the scene like any route (it IS a route with a small rect).
- **POS-3 — Program-side editing, settings-gated**: same overlay on the PROGRAM panel
  behind Settings → Output → "Allow live program layout editing" (default OFF — moving
  live pixels on air is an expert move). When enabled, edits go straight to program
  routes with a red border on the panel while in edit mode.
- **Settings** (POS-1 ships these): snap on/off + grid size, safe-area guide overlay
  (action/title safe), default bug margin %.

Effort: POS-1 is wiring + hit-test plumbing (~1 session, the control exists). POS-2 is
small (route rect + presets). POS-3 is trivial after POS-1 but gated by the toggle.

---

## C. Browser sources (competitive necessity)

Architecture is already specified in `capture-sources-spec.md` §4 (Phase BR) and stands:
**WebView2 in a dedicated `corevideo-browser-host.exe` per source** (windowless
composition, transparent BGRA, keyed-mutex shared texture into the core — the
Zoom-engine isolation pattern), page audio later via process-loopback (BR-A). Screen
capture (Phase SC) from that spec already shipped, proving the ingest pattern.

What this unlocks (the owner's ask): URL sources for **third-party graphics tools —
Singular.live, H2R Graphics, uno, Flowics** — i.e. professional lower thirds/scorebugs
rendered by the tool, keyed over program with real alpha; plus dashboards, chat
widgets, countdowns. Combined with §B (place/resize anywhere) this equals the
OBS/vMix browser-source workflow.

Delivery order (BR-1..3):
- **BR-1 — render-only browser source**: `browser.add {url,w,h,fps}` → host process →
  transparent texture → appears in Sources (as a `Browser` group entry per §A) and in
  scenes/slots like any source. Reload/health/slate-on-crash included.
- **BR-2 — operator UX**: add/edit URL dialog (with size presets: full-canvas overlay
  1920x1080\@alpha for graphics tools; custom for widgets), per-source zoom/custom-CSS.
- **BR-3 — page audio** into the mixer (process loopback, standard channel strip).

---

## Sequencing across A/B/C (recommended)

| Order | Phase | Why first |
|---|---|---|
| 1 | SRC-1 | The brittleness is live-show risk today; small, shell-only |
| 2 | POS-1 (+settings) | Machinery exists; highest perceived-pro gain per effort |
| 3 | BR-1 | The substantial native build; unlocks the third-party-graphics story |
| 4 | POS-2, SRC-2 | Bug placement + one selection surface |
| 5 | BR-2/3, POS-3, SRC-3 | Polish + expert modes |

Already landed with this spec: the type-switch wipe fix (`5bb3c70`).
