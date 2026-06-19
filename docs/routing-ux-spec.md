# Sources / Inputs / Routing / Scenes — UX & Data-Model Spec

_Design-first spec. No app code lands from this document; it is the blueprint the
implementation PRs will follow. Companion to `docs/alpha-plan.md` §4._

## Decisions locked (2026-06-19)

1. **Three tabs**, where the **Sources tab _is_ the Inputs 1–10 mapping** (no
   separate Sources/Inputs split).
2. **Audio routing is a gain matrix** — every crosspoint carries on/off **and** a
   level (dB), i.e. a DSP-style crosspoint mixer, not just Dante on/off.
3. **Design-first**: lock this before writing routing code.

## Signal flow

```
 Sources / Inputs (1–10) ──▶ Routing (video + audio matrices) ──▶ Scenes (canvas) ──▶ Outputs
   what's plugged in           Dante-style crosspoint grids         compositor /        record /
   and which input it feeds     (audio cells carry gain)            16:9 canvas         stream
```

A signal enters as an **Input** (1–10), is **routed** to destinations/buses in the
matrix, gets **composed** on the Scenes canvas, and leaves through **Outputs**.
Each tab owns exactly one stage. Today the Scenes screen owns three of them at
once; this spec splits them.

---

## Tab 1 — Sources (= Inputs 1–10)

**Purpose:** define what feeds each of the ten input slots, and show source health.
This is the home of "what's plugged in."

**Backed by existing model:** `ShowInputSlot` (`Models/ShowInputModels.cs`) already
has `SlotNumber`, `Kind` (Unassigned / ZoomParticipant / Blackmagic / Aja /
UvcWebcam), `ParticipantId | CaptureDeviceId`, and `InShow`. No model change
needed for this tab — it is a relocation + visual upgrade of today's "Show inputs
(multiview)" panel, with capture discovery and Zoom feed health moved alongside.

```
┌ Sources ───────────────────────────────────────────────────────────────────┐
│  #   Input name        Type          Source                 Signal     Show  │
│ ─────────────────────────────────────────────────────────────────────────── │
│ 01   Host cam          Zoom      ▾   Jane Doe          ▾    ● 1080p60   [on]  │
│ 02   Guest cam         Zoom      ▾   Bob Smith         ▾    ● 1080p30   [on]  │
│ 03   Stage A           Blackmagic▾   DeckLink 8K Pro   ▾    ● 2160p30   [on]  │
│ 04   Slides            Screen sh.▾   (Zoom share)      ▾    ○ idle      [ ]   │
│ 05   Intro bumper      Media     ▾   intro_bumper.mp4  ▾    ● ready     [ ]   │
│ 06–10  + assign…                                                              │
│ ─────────────────────────────────────────────────────────────────────────── │
│  Capture devices  [Refresh]        Zoom feed health                          │
│   DeckLink 8K Pro · 2160p30 · live   Jane Doe   ● good                        │
│   UVC Webcam      · 720p30  · live   Bob Smith  ▲ recovering                  │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Interactions**
- Each row binds a Kind + concrete source (existing `SelectedSourceId` logic).
- "Show" toggle = `InShow` (puts the input on the Studio multiview strip).
- Optional editable **Input name** (new, cosmetic — defaults to `Input 01`).
- Capture/feed-health panels are reference data, read-only here.

---

## Tab 2 — Routing (new) — Dante-style crosspoint matrices

Two matrices, switchable by a **Video / Audio** segmented control. Rows are
**sources**, columns are **destinations/buses** (the Dante Controller convention is
TX × RX; we use sources × destinations and label clearly).

### 2a. Video matrix — on/off crosspoints

Sources route to **non-program** video destinations. Program/Preview are driven by
the Scenes canvas, **not** the matrix (a scene composes several inputs, so it
cannot be a single crosspoint). The matrix governs ISO recording, multiview, and
aux/stream sends.

```
 VIDEO            │ ISO-A │ ISO-B │ ISO-C │ ISO-D │ MV-1 │ MV-2 │ AUX-1 │
 ────────────────┼───────┼───────┼───────┼───────┼──────┼──────┼───────┤
 Input 01 Host   │   ●   │       │       │       │  ●   │      │       │
 Input 02 Guest  │       │   ●   │       │       │      │  ●   │       │
 Input 03 Stage  │       │       │   ●   │       │      │      │   ●   │
 Active Speaker  │       │       │       │       │      │      │       │
 Screen Share    │       │       │       │   ●   │      │      │       │
 Media           │       │       │       │       │      │      │       │
```

- Click a cell to toggle the route. A source may fan out to many destinations; a
  destination takes exactly one source (radio behavior down a column for ISO/AUX;
  multiview tiles MV-n are 1:1).
- **Active Speaker / Screen Share / Media** are dynamic pseudo-sources (see Open
  Questions) so the matrix can route "whoever is talking" to an ISO/aux.

### 2b. Audio matrix — gain crosspoints (the locked decision)

Every cell is a **send with level**. Empty = unrouted (−∞). A value = routed at
that gain. This is a crosspoint mixer: one source can sit in the Program mix at
0 dB, in a monitor bus at −6 dB, and ISO-record dry at unity, simultaneously.

```
 AUDIO            │ PGM-L │ PGM-R │ ISO-1 │ ISO-2 │  MON  │ STREAM │
 ────────────────┼───────┼───────┼───────┼───────┼───────┼────────┤
 Input 01 Host   │ -3.0  │ -3.0  │  0.0  │       │ -6.0  │  -3.0  │
 Input 02 Guest  │  0.0  │  0.0  │       │  0.0  │ -6.0  │   0.0  │
 Zoom mix        │ -6.0  │ -6.0  │       │       │       │  -6.0  │
 Media           │ -4.5  │ -4.5  │       │       │ -12.0 │  -4.5  │
                 │       │       │       │       │       │        │
 (cell = dB; click toggles route, scroll / type / drag sets gain; blank = -∞)
```

- **Click** toggles route on (defaults to 0 dB) / off (−∞).
- **Scroll, drag, or type** in a routed cell sets gain; range −∞…+10 dB, 0 dB unity.
- **Column header** shows bus master meter + master trim.
- **Row header** shows source meter, plus **Mute** and **Solo** affecting all of
  that source's sends.
- Right-click a cell → quick set (Unity / −6 / −∞), copy/paste a row's sends.

### Routing tab interactions (both matrices)
- Keyboard: arrows to move the focused cell, Space/Enter to toggle, +/− to nudge
  gain (audio), Esc clears focus. Fully operable without a mouse for live use.
- Crosspoint state persists per show preset (extends the existing preset engine).
- Hovering a cell highlights its row + column for legibility at 10×7+.

---

## Tab 3 — Scenes (canvas only)

The 16:9 OBS canvas (now always fully visible — shipped in PR #126). The **"Add
source"** affordance lists **Inputs 1–10** (only those assigned), plus the dynamic
pseudo-sources **Active Speaker / Screen Share / Media**.

```
┌ Scenes ──────────────────────────────────────────────┬ Sources in scene ─────┐
│  [ Full ] [ 2-up ] [ PIP ] [ Grid ] [ Speaker+slides ]│  Add ▾                 │
│ ┌──────────────────────────────────────────────────┐ │   Input 01 Host        │
│ │                  16:9 canvas                      │ │   Input 02 Guest       │
│ │   ┌────────────┐        ┌────────────┐            │ │   Active Speaker       │
│ │   │ Input 01   │        │ Input 02   │            │ │   Screen Share         │
│ │   └────────────┘        └────────────┘            │ │ ───────────────────    │
│ │            (drag / resize layers)                 │ │  Layer order ▲▼        │
│ └──────────────────────────────────────────────────┘ │  (no route/audio here) │
└───────────────────────────────────────────────────────┴───────────────────────┘
```

- A scene **layer = Input reference + `NormalizedCanvasRect` + z-index**. The
  per-layer **Route mode** and **Audio role** dropdowns are **removed** from this
  tab — source resolution and audio now come from Inputs + the Routing matrix.
- Dragging/resizing still drives `NormalizedCanvasRect` exactly as today.

---

## Data-model changes

| Concern | Today | Change |
|---|---|---|
| Input slot | `ShowInputSlot` (1–10 → kind + source + inShow) | **Keep.** Becomes the canonical source list. Add optional `Name`. |
| Scene layer | `SourceRoute` (`Mode`, `ParticipantId`, `AudioRole`, `CanvasRect`, `ZIndex`) | Add `InputId` (which Input 1–10 this layer shows). `Mode`/`AudioRole` deprecated on the layer (resolution + audio move to Inputs + matrix). `CanvasRect`/`ZIndex` unchanged. |
| Video routing | implicit (Mode = active-speaker / screen-share, ISO via `isoParticipantIds`) | New **`VideoRoutingMatrix`**: `crosspoints: { sourceId, destinationId }`. ISO assignment becomes matrix-driven. |
| Audio routing | `MediaCoreAudioMixChannelWire` (flat per-participant gain/mute/NS) | New **`AudioRoutingMatrix`**: `sends: { sourceId, busId, enabled, gainDb }`. Existing per-source NS/mute stays on the source (row header). |
| Buses / destinations | none explicit | New small enums/registries: video destinations (ISO-A…D, MV-n, AUX-n) and audio buses (PGM, ISO-1…n, MON, STREAM). |

`Scene`, `NormalizedCanvasRect`, and the preset engine are reused as-is (presets
gain matrix state).

## Native command wiring

The matrix maps onto the existing JSON-RPC boundary (`MediaCoreCommandBuilder`).
Use the repo skills **`typed-native-boundary`** (protocol design) and
**`native-core-capability`** (the file-by-file slice with a test at each layer)
when implementing.

- **Scenes → `load-scene-graph`:** the route payload already carries
  `routeId / mode / audioRole / participantId / rect / zIndex`
  (`BuildSceneGraphCommand`). Add **`inputId`**; resolve the concrete source
  (participant / device / media) from the Input registry at build time so the
  native core keeps receiving a resolved `participantId`/source id (backward
  compatible). `mode`/`audioRole` become derived/optional.
- **Video matrix → ISO/aux:** today ISO is `isoParticipantIds` on
  `start-program-output`. Extend to a **`set-video-routing`** command (or enrich
  the existing output/ISO commands) carrying `{ sourceId, destinationId }`
  crosspoints; `isoParticipantIds` becomes a projection of the ISO columns.
- **Audio matrix → `sync-audio-matrix` (new):** replaces/extends
  `sync-participant-audio-mix`. Payload:
  `{ sends: [{ sourceId, busId, enabled, gainDb }], sources: [{ sourceId, muted, noiseSuppression }] }`.
  Keep `sync-participant-audio-mix` as a thin compatibility projection of the
  PGM column until the native mixer consumes the matrix natively.
- Mirror every new wire type in `src/engine/*` and keep the **TS↔C# contract
  parity** gate green (it already guards command/snapshot shape).

## Incremental rollout (build order — each step ships behind mock data)

1. **Sources tab**: relocate Input 01–10 editor + capture/health into a dedicated
   tab; add optional Input name. No behavior change. _(Windows smoke)_
2. **Scenes add-source = Inputs**: layer references an `InputId`; "Add source"
   lists assigned Inputs + pseudo-sources. Keep old dropdowns temporarily.
3. **Routing tab — video matrix**: read-only projection of current routing first,
   then interactive ISO/MV/AUX crosspoints.
4. **Routing tab — audio gain matrix**: crosspoints + gain; wire to
   `sync-audio-matrix`.
5. **Native wiring**: `inputId` on `load-scene-graph`, `set-video-routing`,
   `sync-audio-matrix`; contract-parity tests.
6. **Remove** per-layer Route/Audio dropdowns from Scenes once matrix is the
   source of truth.

> Every step is WinUI and cannot be rendered in Linux CI — each needs a Windows
> smoke pass (`npm run run:studio`).

## Open questions (confirm before step 3–4)

1. **Video destinations:** how many ISO record channels (A–D?), how many multiview
   tiles are matrix-addressable, and how many aux/stream sends?
2. **Program/Preview:** confirm these stay **scene-driven** (not matrix
   destinations). Recommendation: yes.
3. **Audio buses:** confirm the bus list — PGM (stereo), N ISO tracks, how many
   monitor/aux buses, and a single Stream bus?
4. **Dynamic sources:** model **Active Speaker / Screen Share / Media** as
   first-class pseudo-inputs in the matrix (recommended) vs. only as scene layer
   kinds?
5. **Gain law:** confirm range/units (−∞…+10 dB, 0 dB unity, mute = −∞) and
   whether crosspoint gain is pre- or post- the source's channel trim.
