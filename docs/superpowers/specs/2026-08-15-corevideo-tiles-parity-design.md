# CoreVideo Tiles — parity design

Date: 2026-08-15
Charter: `docs/obs-plugin-parity-charter.md` (group T)
Status: design approved, not yet planned

## Purpose

Bring CoreVideo Pro's Tiles to and past the OBS plugin's CoreVideo Tiles: a
participant wall that fills the canvas, styles every tile (border, corner radius,
glow), crops rather than letterboxes, reflows with animation as people join and
leave, and can be hand-arranged by the operator without losing any of that.

Pro shipped a Tiles *layout* in #401 — `DynamicGalleryLayoutService` solves a grid
and emits ordinary scene routes. This spec keeps the product behaviour that shipped
and replaces the mechanism underneath it, because the missing features (glow,
radius, crop, animation) cannot be expressed as independent scene routes.

## Scope

**In:** the tiles render-plan layer, the core-side solver and animator, the GPU
draw (fill-crop, border, radius, glow, background), membership policy in the shell
(auto/manual fill, never-show, max tiles), per-tile overrides (rect, crop, z),
the canvas editor over the core's wall, persistence, and a pixel-level verification
oracle.

**Out:** the paused source-level border work from 2026-08-11 — borders and glow on
non-tile sources stay out of this spec and will reuse these shaders when unpaused.
Group Z and group O of the charter. Metal is in scope as a tracked deliverable but
sequenced behind Windows (§9).

## Architecture

Tiles becomes a new render-plan layer kind, `tiles`, alongside the existing
`participant-video`, `screen-share`, `media-video`, `media-background`, `overlay`,
and `chroma-key` (validated set at `native/src/modules/StubModules.cpp:79`).

The division of ownership:

- **The shell owns policy.** Who is on the wall, what it looks like, what the
  operator has overridden. The roster, the operator's intent, and persistence
  already live there, and that logic stays unit-testable in C#.
- **The core owns time and pixels.** The layout solve, the animation clock, and the
  draw. This is forced, not chosen: animated reflow needs a 60Hz clock, and the
  only way to drive that from the shell is a frame-rate command path — the exact
  churn class that fail-fasts WinUI with `CoreMessagingXP 0xc000027b` (CLAUDE.md,
  "the crash class you WILL hit"). The core already has a 60Hz render tick.

Two alternatives were rejected. A *scene layout mode* (the core recognises
`layout: "dynamic-gallery"` and expands it) makes the wall a property of the whole
scene, so it cannot sit in a corner next to a camera and there cannot be two walls —
both of which the plugin supports. A *`configure-tiles` command* in the shape of
`configure-multiviewer` would be silently lost whenever the supervisor respawns the
core; that failure already shipped once as "the multiviewer is broken" (CLAUDE.md,
"a one-shot command must be re-applied on every core generation").

Because the wall is a render-plan layer, program, preview, recording, RTMP/SRT and
the virtual camera inherit it from one implementation — they all consume the
composed program.

## Wire contract

One layer object, sent on the existing sync channel at its current cadence. No new
command, no new frequency.

```
{
  "kind": "tiles",
  "layerId": "tiles:<sceneId>",
  "order": <int>,
  "rect": { x, y, w, h },            // the wall's region of the canvas
  "members": [ "zoom:<pid>" | "capture:<id>", ... ],   // ordered
  "style": {
    "tileAspect": "16:9"|"4:3"|"5:4"|"1:1"|"3:4"|"9:16"|"custom",
    "customAspectRatio": <double>,
    "gutterPercent": <double>,        // of wall height
    "marginPercent": <double>,        // of wall height
    "backgroundColor": "#RRGGBB",
    "backgroundSourceId": "<sourceId>|null",
    "borderShape": "square"|"rounded",
    "borderColor": "#RRGGBB",
    "borderThickness": <double>,
    "cornerRadius": <double>,
    "glowColor": "#RRGGBB",
    "glowSize": <double>,
    "glowIntensity": <double>,        // 0..100
    "glowSoftness": <double>,         // 0..100
    "animateLayout": <bool>,
    "animationDurationMs": <int>      // 100..2000
  },
  "overrides": {                      // sparse; absent for a normal wall
    "<memberId>": { "rect": {x,y,w,h}|null, "cropLeftPercent": <double>,
                    "cropRightPercent": <double>, "z": <int>|null }
  }
}
```

`members` is ordered and authoritative for *eligibility*, not for what is drawn —
see the frame-reality veto below. The style block is `DynamicGallerySettings`
serialised, plus the two background fields it does not yet carry.

Protocol mirrors that must move in lockstep, per the ISO-1 precedent: `Protocol.h`
(capability), `native-core/src/protocol.ts` (types), and the core parser.

## Core

### Solve

`DynamicGalleryLayoutService`'s grid search ports to C++ substantially as-is. Its
50 existing tests in `DynamicGalleryLayoutServiceTests` become the port's
acceptance criteria: the C++ solver must produce the same rects for the same
inputs, so the port is verified against a known-good implementation rather than
against itself.

Overrides change the solve in one place. A member carrying an override rect is
placed at that rect and its area removed from what is available; the remaining
members solve into the leftover space. "Pinned host tile beside a reflowing
gallery" is therefore the ordinary code path, not a special case.

Once the core solves, the shell stops solving. `DynamicGalleryLayoutService` is
retained only as the reference implementation the C++ port is tested against, and
is no longer consulted at runtime — two live solvers would drift, and the day they
disagree is the day the editor draws boxes where the program is not rendering.

### Frame-reality veto

Before solving, the core drops any member with no fresh frame. The shell's
membership filter is its *belief* about video (`Health != VideoOff`); the core is
the process actually receiving frames, and it already detects exactly this
condition — `warnUnmatchedCaptureLayer` exists because a layer resolving to no
frame used to render silently pink. The same signal drives a reflow instead of a
warning.

A guest whose feed dies leaves the wall on the next solve and rejoins when frames
return. The plugin cannot do this; its docs accept a wall that briefly holds a dead
tile as "the deliberate trade for the wall never being behind reality."

Hysteresis is a design requirement, not an inherited constant. A member is dropped
once its most recent frame is older than a stale threshold, and re-admitted on the
first fresh frame. The threshold must be long enough that an ordinary frame gap or
a resolution ramp never reflows the wall, and short enough that a dead feed does
not hold a slot through a visible pause; the plan sets the value and pins it with a
test at both edges. Note that `warnUnmatchedCaptureLayer`'s existing 5s interval is
a *warning rate limit*, not a staleness threshold — it must not be borrowed as one
without confirming what per-source frame-age signal the core actually exposes.

### Animate

No new thread and no new timer. The render tick is already a 60Hz loop on a
fixed-anchor pacer with bounded catch-up. The layer holds `startLayout`,
`targetLayout`, `startTimeMs`, `durationMs`; each tick interpolates between them.

A new solve arriving mid-animation retargets from where the tiles currently *are*,
not from the previous target, so a join during a reflow bends the motion rather
than snapping it.

Two behaviours ported from the plugin: a newcomer fades in at its final slot rather
than travelling across the wall, and a departure reflows immediately rather than
holding the leaving tile on screen.

At rest the interpolator is bypassed and tiles draw through the pixel-exact,
even-snapped path — a stationary wall must be byte-identical to the un-animated
path, and only a tile actually in motion takes the sub-pixel path, returning to the
exact one as soon as it settles. The precise snapping rule comes from the
behavioural contract (§6 of `docs/obs-plugin-tiles-behavioral-contract.md`).

With `animateLayout` off, the animator never runs. As in the plugin, this is a
different render path, not a cosmetic setting.

### Draw

Three passes per wall, per frame:

1. **Background** — the background colour fills the wall rect; if
   `backgroundSourceId` is set, that source draws over the colour and under the
   tiles. A background source that is missing, deleted, or would render itself
   (the wall, or a scene containing it) falls back to colour.
2. **Glow** — drawn behind the tiles, so adjacent tiles' glows do not paint over
   each other's content. Falloff is defined by the behavioural contract (§5) and
   must match the plugin's shipped `.effect`, not merely resemble it.
3. **Tiles** — each tile's source rect computed for fill-never-letterbox (crop the
   sides rather than bar the edges), composed with any per-tile crop percentages;
   then the border drawn inset from the tile edge, clamped so a width past half the
   shorter side degrades gracefully instead of inverting the tile. Corner radius
   masks tile content, border, and glow consistently.

The solved rects are published in the snapshot so the shell can position editor
handles over the pixels.

## Shell

### Controls

`DynamicGallerySettings` already carries aspect, custom ratio, gutter, margin,
border shape/colour/thickness, corner radius, glow colour/size/intensity/softness,
animate, and duration. Four capabilities are added:

- **Manual fill mode** — a per-slot participant picker. A slot whose assigned
  participant is absent holds empty; it never silently promotes someone else.
- **Never-show list** — participants auto-fill skips.
- **Background colour and background source** — new fields on the settings model.
- **Per-tile crop** — left/right percentages, carried in the override map.

### Editor

The canvas editor renders the core's wall texture and positions drag handles,
labels, and selection chrome from the solved rects in the snapshot. Dragging a tile
writes an override; clearing the override returns it to the grid on the next solve.

This also means the wall shows live video in the editor by construction — one
surface with one key, rather than N per-layer surfaces under synthesised keys. That
is the same single-texture pattern that fixed the multiview when per-tile swap
chains were fail-fasting. It fixes the wall only; non-tile scene layers keep
today's path and are being investigated separately.

A drag that begins while the wall is animating either snaps to the animation's end
state or cancels the animation — it does not track a moving target.

### Persistence

Prefs schema v9 → v10, carrying membership policy (fill mode, manual assignments,
never-show, max tiles), the background fields, and the override map. A v9 file
migrates to a wall with no overrides and no never-show list, which is exactly
today's behaviour. Restore rides the established backing-field pattern — a property
setter would sync a core that is not up yet.

### Audio

The plugin creates one Zoom participant audio source per tile into an
operator-managed scene or group, and documents three failure modes for doing that
on two walls at once. Pro needs none of it: per-guest strips, ISO stems, and THE
FADER LAW (no audio source reaches a bus without a strip) already exist. Parity
here is only that wall membership guarantees a strip exists for each member —
the law the codebase already enforces. No second audio model and no multi-wall stem
numbering hazard.

## Failure modes

Every one of these is loud, never silent — the house rule.

| Condition | Behaviour |
|---|---|
| Glow or tile effect fails to load | The tile degrades to a clean, unstyled tile. A failed effect must never make a source disappear or take Program down. Rate-limited warning. |
| A member has no frame | Excluded from the solve (frame-reality veto); the wall reflows without it. Not an error. |
| Background source missing / self-referential | Falls back to background colour, with a warning. |
| Override rect outside the wall rect | Clamped into the wall, with a warning; never drawn off-canvas. |
| Border width or radius past the tile's usable interior | Clamped, per the plugin's rule. Not an error. |
| Members exceed max tiles | Truncated in shell policy before the wire; the count that was dropped is surfaced, never silently discarded. |

## Verification

The wall is judged on pixels. This codebase has been burned twice by validators
that checked a proxy that survived the bug — a program recording that muxed a
320×180 UI thumbnail for months while every validator checked stream presence and
none looked at a pixel, and a sender whose container FPS read healthy while it was
structurally capped at 50. `validate-multiview.mjs` is explicit that it proves
structure, never pixels.

So Tiles gets a pixel oracle, built on the fake engine, which already emits
distinct animated I420 per participant. It must assert:

1. Each solved tile rect contains its assigned participant's content — not merely
   that N rects exist.
2. Gutters, margin, and uncovered canvas carry the background colour.
3. Borders appear at the expected inset, thickness, and colour.
4. Glow is present behind tiles when enabled and absent when disabled.
5. A tile at rest is byte-identical to the same wall rendered with
   `animateLayout` off.
6. A departure reflows and settles within `animationDurationMs`.
7. An extreme border width or corner radius degrades rather than inverting a tile.

Unit-level, alongside: the C++ solver reproduces `DynamicGalleryLayoutServiceTests`
case-for-case; override placement leaves the remaining tiles solving into the
correct residual space; the frame-reality veto admits and drops on the intended
thresholds.

## Sequencing and the Metal cost

Windows (`D3D11CompositorAdapter`, HLSL) leads. Every draw pass in §Draw must then
be written a second time in `MetalCompositorAdapter` or the Mac port ships without
Tiles. That work is part of this spec's definition of done, tracked explicitly and
sequenced after Windows — not deferred silently and discovered later.

## Dependencies

- `docs/obs-plugin-tiles-behavioral-contract.md` — the extracted contract from the
  plugin's shipped implementation. This spec depends on it for two formulas it does
  not restate: the glow falloff (§5 of that document) and the at-rest pixel-snapping
  rule (§6). Both must match the shipped plugin shader, and the implementation plan
  cannot close those items without it.
