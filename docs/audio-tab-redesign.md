# Audio Tab Redesign (spec 4.4b)

**Date:** 2026-07-03 · **Status:** proposed design, pre-implementation · **Parent:**
`docs/audio-overhaul-spec.md` §4.4b. Code audit anchors verified against the tree at `b0c2473`.

## 1. What exists today (why it feels broken)

The "Audio tab" is **three disconnected surfaces**, none complete:

| Surface | Has | Missing |
|---|---|---|
| Audio tab (`Views/AudioPage.xaml`) | ~10 stacked telemetry strings, read-only source strips (gain/pan/bus as text), display-only bus chips, device pickers, processing panel | Any editing of gain/pan; any actual routing (the chips aren't interactive, `AudioPage.xaml:281-309`) |
| Routing tab (`Views/RoutingPage.xaml:55-234`) | The real crosspoint grid + a single shared gain NumberBox | Meters, context, discoverability (it's a different tab entirely) |
| AudioMixerWindow (pop-out) | The only faders/mutes in the product | Routing; it duplicates the tab's processing panel + master readouts |

Root defects, each verified in code:

1. **The routing model never reflects reality.** `AudioRoutingMatrixViewModel` is built
   client-side from UI source lists and pushed one-way to the core
   (`StudioViewModel.cs:3250-3310`, `:7099-7107`); the core's actual routing state
   (`snapshot.AudioRoutingMatrix`) is consumed ONLY as a flattened telemetry string
   (`:815-818`). Engine-side defaults/overrides never appear in the grid. The UI's own
   defaults even disagree with the engine's: `Build()` routes new sources to
   master/pgm-l/pgm-r/mon but **omits `stream`** (`AudioRoutingMatrixViewModel.cs:219-223`)
   — the engine default (incl. the new Zoom participant sends) includes it, so the grid
   under-reports what is actually routed.
2. **Toggle-and-lose crosspoints.** Clicking a routed cell UNROUTES it
   (`AudioRoutingMatrixViewModel.cs:152-161`); there is no select-without-destroying, so
   adjusting a gain risks deleting the route. Gain editing is one shared NumberBox at the
   bottom of the page for whichever cell was last selected.
3. **The tab's mixer strips are read-only** (`AudioPage.xaml:237-275` — row = one big select
   Button; GAIN/PAN/BUS/INSERTS are labels). `BusLabel` shows only the FIRST routed bus and
   hard-codes "MASTER" for unrouted sources (`StudioViewModel.cs:9064-9074`).
4. **Device selection**: the monitor device list has no "System default" entry and no
   `IsDefault` flag at all (`AudioRenderDeviceDiscoveryService.cs:20-31`,
   `ProductionModels.cs:164-169`); first-run selection is alphabetical noise
   (`StudioViewModel.cs:4947-4951`). Labels are inconsistent (monitor shows `Name`, local
   shows the verbose `DisplayLabel`). Persisted selection is keyed on a hashed id that
   silently resets when the native id path changes.
5. **Two id spaces for the same audio** (show-input/capture ids in the matrix vs
   participant/native ids in the strips), reconciled ad hoc (`:9076-9097`).
6. Placeholder "waiting for PCM" rows are visually indistinguishable from live-but-silent
   sources (`:8809-8818`); no per-participant sync-offset exists anywhere (capture-only,
   Sources page).

## 2. Design principles

- **One surface, two modes.** The Audio tab becomes the single audio home with a
  SHOW / SETUP mode switch. The pop-out window hosts the same mixer component (no forked
  layouts or duplicated panels).
- **Core-authoritative state.** The routing grid and strips RENDER the snapshot
  (`AudioMixSession` + `AudioRoutingMatrix`) and send edits as deltas. If the engine
  defaults, clamps, or overrides something, the operator sees it. No client-side ghost
  model.
- **Select is never destroy.** Editing affordances are explicit; destructive ones are
  distinct.
- **One canonical source id** end-to-end (the `zoom:` / `capture:` / `media:` /
  `local-machine-audio` convention already used by SourceDisplayNames), with display names
  from the nameable-sources resolver.
- **Meters everywhere, with ballistics** (spec 4.4: fast attack, ~300ms release, peak-hold).

## 3. SHOW mode (what an operator needs mid-show)

```
+--------------------------------------------------------------------------+
| [SHOW | setup]                                   MASTER  -16.0 LUFS  Lim |
|                                                  [====meter L====]       |
|  +-------+ +-------+ +-------+ +-------+ +-----+ [====meter R====]       |
|  | Ada   | | John  | | Cam 1 | | Media | | Sys | Monitor [on] [vol--]    |
|  | meter | | meter | | meter | | meter | |meter| device: Headphones (Gx) |
|  | fader | | fader | | fader | | fader | |fader|                         |
|  | pan   | | pan   | | pan   | | pan   | | pan | [Pop out mixer]         |
|  | M  S  | | M  S  | | M  S  | | M  S  | | M S |                         |
|  | PGM.. | | PGM.. | | pgm-l | | MST   | | ... | <- routed-bus badges    |
|  +-------+ +-------+ +-------+ +-------+ +-----+    (click -> setup)     |
+--------------------------------------------------------------------------+
```

- Channel strips: name (display-name resolver), live meter (ballistics), fader (dB),
  pan, Mute, **Solo** (backend already supports it; no UI anywhere today), routed-bus
  badges showing ALL sends (click jumps to Setup with the source focused).
- Strip states: live / silent (meter at floor, normal chrome) / **waiting-for-PCM**
  (distinct dashed chrome + tooltip) / error (device warning inline).
- Master section: BS.1770 loudness, L/R meters, limiter state, monitor enable + volume +
  device (with "System default").
- The AudioMixerWindow becomes this same strip panel hosted in a window (one
  implementation; rotary/DAW fork retired or kept as a strip-density toggle).

## 4. SETUP mode (devices, routing, processing)

```
| [show | SETUP]                                                           |
|  Devices                    Routing matrix                Processing     |
|  Local source [combo][on]   src\bus MST PGM-L PGM-R STR MON ISO1..      |
|   ^ System default entry    Ada      [0] [0]  [0]  [0] [0] [ ]          |
|  Monitor     [combo][on]    John     [0] [0]  [0]  [0] [ ] [0]          |
|   ^ System default entry    Cam 1    [-3] ...                            |
|  Sync offsets (per src)     Media    ...                                 |
|                             + Add aux bus   [rename] [delete]            |
```

- **Matrix interaction**: click empty cell = route at 0dB; click routed cell = SELECT
  (opens an inline gain flyout: slider + numeric + "remove route" button); drag
  vertically on a routed cell = trim gain. ISO exclusivity preserved with a visible
  "exclusive" column badge. Bus columns grouped and labeled by role (Program, Master,
  Stream, Monitor, ISO, Aux) — never raw ids.
- **Reconcile-from-core**: rows/cells hydrate from `snapshot.AudioRoutingMatrix` (which
  must be extended to carry per-send `{sourceId, busId, gainDb}` — today it publishes
  taps/counts only); operator edits optimistically update and send; the snapshot confirms.
  Conflict rule: snapshot wins, with a brief "engine adjusted" toast when it differs.
- **Devices**: both lists get a "System default" pseudo-entry + `IsDefault` markers +
  consistent `Name · kind` labels; persistence by native id with fallback-to-default (not
  fallback-to-first-alphabetical); the monitor==loopback feedback warning surfaces here
  (once 4.2's resolved-endpoint guard lands).
- **Sync offsets**: per-source ±ms nudges, including Zoom participants (new; capture-only
  today, and only on the Sources page).
- **Processing**: per-target insert list stays, but only offers inserts that actually
  process (bus compressor/limiter now; EQ/gate once wired — spec 4.4; VST slots hidden
  behind a dev flag until the host exists).

## 5. Data-flow changes required (core + bridge)

1. Extend the core's published routing matrix with the full send list
   (`{sourceId, busId, gainDb}` per send) so the UI can hydrate. (Core already holds it;
   it just doesn't publish per-send detail.)
2. C# `NativeMediaCoreAudioRoutingMatrix` model + parser gains `Sends`.
3. `AudioRoutingMatrixViewModel` becomes a projection of the snapshot + pending local
   edits (delta queue), not a source of truth. The `Build()` defaults move out entirely
   (the engine + `EnsureDefault*RoutingSends` own defaults; fix the `stream` omission as
   part of the removal).
4. Unify source ids: matrix rows keyed by canonical ids; strips and matrix share one
   roster provider.

## 6. Implementation phases (each shippable)

| Phase | Scope | Size | Status |
|---|---|---|---|
| B1 | Core publishes per-send routing detail; UI hydrates the EXISTING grid from it (reality fix without redesign); fix select-vs-unroute + per-cell gain flyout | S-M | **SHIPPED** #165 — core already published `sends`; shell hydrates with a 2s local-edit quiet period; select-never-destroys + explicit Remove |
| B2 | Device pickers: System-default entries, IsDefault, consistent labels, native-id persistence | S | **SHIPPED** #166 — System-default pseudo-entry (empty native id → core resolves OS default); default-first sort |
| B3 | SHOW mode: editable strips on the tab (reuse mixer-window strip component), Solo button, meter ballistics (with 4.4), state chrome for waiting-for-PCM | M | **SHIPPED** #170 — gain/pan sliders + M/S per row on the tab; Solo added everywhere (DSP supported it all along); ballistics shipped with #166 |
| B4 | SETUP mode: matrix moves onto the Audio tab (Routing tab keeps video), bus grouping/labels, aux bus rename/delete, per-source sync offsets | M | **SHIPPED (core scope)** #176 — shared `AudioRoutingMatrixPanel` on both tabs, same VM instance. Bus rename/delete + per-source sync offsets deferred |
| B5 | Pop-out window rehosts the shared strip panel; delete duplicated XAML | S | open |

## 7. Acceptance

- A send created by engine defaults (e.g. a Zoom participant's `stream` send) is VISIBLE
  in the grid within one snapshot cycle of joining.
- Adjusting a routed crosspoint's gain never deletes the route.
- Gain/pan/mute/solo are editable on the Audio tab itself; the pop-out shows the same
  component.
- Fresh install: monitor device defaults to the OS default output; both pickers offer
  "System default".
- Every routed bus for a source is visible on its strip.

## 8. C-phases — console rework (owner-directed, 2026-07-04)

Owner feedback after using B1–B4 live: **"The Audio tab needs a big rework. The
UI really doesn't make sense."** Reference supplied: Waves eMotion LV1 — a
CONSOLE, not a settings page. The B-phases made the tab honest and editable;
the C-phases make it a mixing surface. Layout language to adopt from the
reference:

- The mixer IS the page: a horizontal bank of VERTICAL channel strips anchored
  to the bottom, one strip per source. Everything else docks around it.
- Strip anatomy (bottom-up): channel name/number chip → cue/solo → vertical
  fader WITH the segmented meter beside it (fader grabs, meter dances — one
  glance) → dB readout → MUTE (red when engaged) → pan/rotary.
- Selected-channel processing rack ACROSS THE TOP: input/trim, gate, dynamics
  (curve + GR meter), EQ (graph), sends, routing — click a strip to load it.
- Master section pinned RIGHT: L/R fader + meters, **big BS.1770 LUFS readout**
  (now real — PR #180), limiter state, monitor volume/device.
- Telemetry text walls demoted: the ~10 stacked status strings move to a
  collapsible diagnostics drawer / support bundle. A console shows state with
  meters and lit buttons, not paragraphs.

| Phase | Scope | Size |
|---|---|---|
| C1 | Console strip bank: vertical fader + adjacent live meter per source (reuse AudioLevelMeter ballistics), dB readout, MUTE/SOLO/pan, name chip, routed-bus badges; master strip right with L/R meters + big LUFS + limiter + monitor controls. Replaces the horizontal row list on the Audio tab; pop-out hosts the same bank (subsumes B5) | L |
| C2 | Selected-channel rack: gate / EQ / compressor cells with enable toggles + parameters bound to the REAL 4.4 DSP (insert chain), GR/gate activity indication | M-L |
| C3 | SHOW/SETUP split finalized: console = SHOW; matrix, devices, sync offsets = SETUP; diagnostics drawer replaces the telemetry wall | M |

Acceptance (C1): an operator can mix a show from the tab alone — grab any
fader while watching its meter, mute/solo without hunting, read program
loudness at a glance; nothing on the surface is a static text readout of
something a meter or lit button could show.
