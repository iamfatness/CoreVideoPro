# OHG Show Engine — Design Spec

**Date:** 2026-08-04
**Status:** Approved design, pre-implementation
**Companion doc:** `2026-08-04-ohg-isadora-actor-reference.md` — the full algorithm-level
extraction of the Isadora patch (`Experimental_OH-2.1_iz3.2.6_v0.78.1` / patch v0.78).
That document is the authoritative reference for every ported algorithm and wire format;
this spec defines the architecture, module boundaries, protocol, and platform contract.

## 1. What this is

Office Hours Global (OHG) runs on an Isadora patch that orchestrates a rack of external
gear: ZoomOSC/ZoomISO bot instances for Zoom video extraction, an ATEM (driven via the
MixEffect app's OSC API) for switching and SuperSource layouts, a Blackmagic router/
multiview for the gallery wall, SPX for graphics, the Mukana REST backend for panelist
registration/questions/hands, oh.tally.video for panelist tally, and the "Universe"
web control surface over OSC.

**This project replaces that stack with CoreVideo Pro** (and, at equal priority, the
CoreVideo OBS plugin): CVP's native Zoom engine replaces the ZoomOSC/ZoomISO bots,
CVP's GPU compositor + scene system replaces the ATEM/SuperSource/multiview, and a new
**show engine** — a host-agnostic TypeScript package — replaces the Isadora logic. The
external *show data* ecosystem stays: Mukana, SPX graphics, and tally are kept in v1.

### Goals
- All Isadora show logic reimplemented as tested, host-agnostic TS modules.
- One control protocol (the existing CVP control manifest stack) serving **native
  operator panels in the shells** (the Universe replacement — our own design, not a
  web port of it), the Bitfocus Companion module, and OSC clients.
- Identical behavior on Windows CVP, macOS CVP, and the OBS plugin, enforced by a
  shared conformance suite. **No show logic in Swift, C#, or C++.**
- Safe migration: shadow mode against the live rig before any cutover.

### Non-goals (v1)
- Replacing Mukana, SPX, or the tally service.
- A `universeBridge` driving the legacy rig from the new surface (explicitly deferred;
  see §8 migration).
- Auth beyond LAN-trust + shared token (control room network assumption, same posture
  as the existing control API).
- ASL-specific compositing features beyond role handling (skip-lists, role assignment).

## 2. Architecture

```
                 ┌─────────────────────────────────────────────┐
                 │        show-engine (TypeScript, Node)        │
                 │  zoomIngest · identity · mukanaSync ·        │
                 │  overrideDb · panelistDb · liveSlots ·       │
                 │  speakerRecency · galleryDirector ·          │
                 │  handsQueue · lookDirector · programBus ·    │
                 │  tallyPublisher · gfxDirector · config       │
                 │                                              │
                 │        HostAdapter interface (port)          │
                 └───────┬──────────────────────┬───────────────┘
                         │                      │
              CVP adapter│                      │OBS-plugin adapter
                         ▼                      ▼
        ┌────────────────────────┐   ┌────────────────────────┐
        │ CoreVideo Pro          │   │ CoreVideo OBS plugin   │
        │ Zoom engine + D3D11/   │   │ Zoom engine + OBS      │
        │ Metal compositor       │   │ scenes/sources         │
        └────────────────────────┘   └────────────────────────┘
```

- **Placement:** a new package in the CVP monorepo (sibling to `native-core/`),
  `show-engine/`, pure logic + vitest, following the `src/engine/` idiom: no I/O in
  core modules; I/O lives in thin client/adapter files.
- **Process model:** the engine runs as a Node subprocess launched by whichever shell
  hosts it (WinUI shell on Windows, mac-shell on macOS, the OBS plugin's supervisor).
  CVP is already a multi-process app (shell + C++ media core + Zoom engine subprocess);
  this is one more supervised subprocess using the same JSON-line pipe idiom.
- **Event source:** the host's Zoom engine roster events replace ZoomOSC. Slot/pin
  routing becomes host slot/source assignment through the `HostAdapter`. There are no
  bots, no OSC-to-Zoom hops.
- **The control surface is a client, not the brain.** All state lives in the engine;
  surfaces render state snapshots and send actions.
- **Platform parity by construction:** the same engine package runs everywhere; only
  process-launch glue and host command wiring differ per shell. The spec (not the code)
  is the contract: data model (§5), protocol (§4), adapter command set (§6). A native
  reimplementation, if ever needed, implements this spec.

## 3. Module breakdown

State flows one direction; each module consumes typed events/state and publishes typed
state. All algorithms cite their source actor in the companion reference.

**Ingest & identity**
1. **`zoomIngest`** — host roster events → participant map (working + published
   snapshot double-buffer, per `Zoom_Cached_Data`; prevents half-applied bulk updates
   from being observed).
2. **`identity`** — PIN extraction (`\b\d{4}\b`, first match) and fallback
   `name | location` display-name splitting. Pure functions
   (`ZoomData_Pin_Extractor`, fallback extractor).
3. **`mukanaSync`** — polls `?req=panelists|question|hands`; rejects the off-hours
   `"status"` error body; re-keys by PIN; persistent merge
   (`Mukana_Data_Parser`/`Merger` + health gate).
4. **`overrideDb`** — operator role overrides with host/reader exclusivity via the
   demote-then-assign algorithm; roles `panelist | host | reader | aslpanelist |
   aslinterpreter` (`Javascript__93`).
5. **`panelistDb`** — joins Zoom participants against Mukana+overrides on PIN → master
   DB keyed by zoomID; single source of truth for identity + editorial role
   (`DataBase_Aggregator`).

**Slots & direction**
6. **`liveSlots`** — fixed slot array sized to host capacity; add-to-first-EMPTY hole;
   remove-leaves-hole; replace-inherits-slot; single-host/single-reader invariant;
   PIN≥9000 utility-participants pinned to the tail block (`pin − 9000` = offset from
   end); JSON backup/restore (`Panelists_Append_Remove_Replace_v4`). The heart of the
   engine; most exhaustive tests.
7. **`speakerRecency`** — three strategies behind one interface
   (`onActiveSpeaker(id) → decisions`): FILO eviction (`FILO_Speaker_Router`),
   speaking-score bucket sort (`Smart_Gallery_Brain`), visible-set swapping
   (`MV16_Router`). The role skip-list lives here: a speaker whose role is in
   `skipRoles` (default `["aslinterpreter"]`) never triggers a change
   (`Active_Speaker_Search_v6`).
8. **`galleryDirector`** — 16-cell gallery state (reset-from-slots, replace, remove,
   empty), smart-gallery variant, grid transposition, emits cell-routing diffs
   (`Gallery_Remove_Replace_v3`, `MV16_Routing_Generator`, `List_Transposer`).
9. **`handsQueue` + `lookDirector`** — hands API (prev/current/next PINs) → ordered
   guest queue with host/reader stripped; named looks (the SuperSource-state
   successors: HR/H ±Q, Banter, Teatime, Panel Checks → CVP scene presets) and the
   box-window algorithm choosing visible queued guests
   (`SuperSourceBrain`/`Javascript__13`, `SuperSource_Search_v3`).

**Outputs**
10. **`programBus`** — software PGM/PVW (preview/cut/auto/direct-cut per
    `onProgram_-_onPreview`); active-speaker follow (the ME2 replacement) with gating
    and direct-cut override.
11. **`tallyPublisher`** — derives on-air PIN lists from `programBus` + `lookDirector`
    + gallery state (the `MixEffect_Info_v15` derivation minus ATEM parsing — the
    engine *is* the switcher state now); publishes to oh.tally.video.
12. **`gfxDirector`** — SPX client: six template functions with exact param encodings,
    layouts A1–D2 → `f0..f12` field building, rundown/item control, change-detection
    so overlays fire only on real changes.
13. **`config`** — typed show config replacing `infraestructure-*.js`: service
    addresses, slot capacity, skip-roles, look definitions, poll intervals. JSON +
    schema; one-shot importer from the legacy files.

**Boundary**
14. **`hostAdapter`** — interface, §6.
15. **`controlIntegration`** — registers `ohg.*` actions/state with the host control
    server, §4.

**Deliberate divergences from the patch:** the shipped quirks are *not* ported —
`some()`-as-find retaining the last element on miss, mixed live-DB key formats
(`"1"`/`"1-"`/`"1-<zoomID>"` — we use plain numeric slot keys), 1-based JSON with
0-based wire emission (the adapter owns index translation), `" "`/`"####"` sentinel
pins (we use `null`), and the `.lenght`/`udpate`/`dirtyData` typos. The reference doc's
"Reimplementation gotchas" section is the canonical divergence list; behavior changes
beyond cleanups require a spec update.

## 4. Control protocol & surfaces

Builds on the shipped `CoreVideoPro.Control` stack: one server per host — HTTP
(`:8011`: `GET /manifest`, `GET /state`, `POST /invoke {action, args[]}`), WebSocket
state push, and the OSC mirror (`transport.take` ⇄ `/cvp/transport/take`, feedback
under `/cvp/state/…`). Manifest derives from the action registry and cannot drift;
Companion builds actions from it dynamically. Params are positional
(string/int/double/bool).

1. **One control endpoint per host.** The engine registers `ohg.*` actions with the
   host's control server; the host proxies `ohg.*` invokes to the engine subprocess and
   merges engine state into `ControlState` under an `ohg` node. Companion, the native
   panels, and OSC clients keep a single integration point. Every action gets an OSC
   address for free (`/cvp/ohg/panelist/add`) — also a migration path for legacy OSC
   gear.
2. **Actions** (positional params; defaults shown):
   - `ohg.panelist.add (int zoomID, int slot = 0 /* 0 = first empty */)`
   - `ohg.panelist.remove (int slot)`
   - `ohg.panelist.replace (int slot, int zoomID)`
   - `ohg.panelist.role.set (int pin, string role)`
   - `ohg.panelist.syncAll ()`
   - `ohg.program.preview (string source)` · `ohg.program.cut ()` ·
     `ohg.program.auto ()` · `ohg.program.directCut (string source)` ·
     `ohg.program.asFollow.set (bool on)`
   - `ohg.look.set (string name)` · `ohg.look.nextGuest ()` · `ohg.look.prevGuest ()`
   - `ohg.gallery.resetFromSlots ()` · `ohg.gallery.replace (int cell, int slot)` ·
     `ohg.gallery.remove (int cell)` · `ohg.gallery.empty ()` ·
     `ohg.gallery.smart.set (bool on)`
   - `ohg.gfx.headline.in ()` / `.out ()` / `.change (string name, string location)`
   - `ohg.gfx.question.in ()` / `.out ()`
   - `ohg.gfx.rundown (string op /* play|continue|stop|next|prev */)`
   - `ohg.mukana.sync ()` · `ohg.mukana.override.set (int pin, string name,
     string location, string role)` · `ohg.mukana.override.delete (int pin)`
3. **State.** Rich JSON on the `ohg` node of `/state` + WS push: `panelists` (master
   DB), `slots` (live array with holes), `gallery`, `queue`, `program`
   (pgm/pvw/mode/asFollow), `tally` (PIN lists + per-slot on-air), `health` (mukana /
   spx / tally / host). Flattened OSC/Companion feedback fields join `StateFields`:
   `ohg/slot/{n}/name`, `ohg/slot/{n}/role`, `ohg/slot/{n}/tally`,
   `ohg/program/mode`, `ohg/queue/current`, `ohg/health/mukana`, etc. (the
   `input/{slot}/…` convention).
4. **Native operator surface** (the Universe replacement) — an "OHG Show" workspace in
   each shell (WinUI on Windows, SwiftUI on macOS), designed as our own product UI
   rather than a Universe clone. The panels are **thin state renderers**: every
   behavior is an `ohg.*` action and every displayed value comes from the `ohg` state
   node, so the WinUI/SwiftUI duplication is view-only — no show logic in either.
   Four working areas:
   1. **Panelist board** — master list (name, location, PIN badge, Mukana/video/online
      status, role chips) beside the live slot grid; tap/drag-to-assign, hole-aware;
      live host/reader conflict warnings (`Search_Multiple_Host_Reader` behavior).
   2. **Program panel** — PGM/PVW state, cut/auto/direct-cut, active-speaker-follow
      toggle with current speaker + lower-third preview, look selector, hands-queue
      pager. (Thumbnails are state-only in v1.)
   3. **Gallery panel** — 16-cell grid, tap-to-replace/remove, smart toggle,
      reset-from-slots.
   4. **GFX & data panel** — current question, headline in/out, rundown transport,
      Mukana sync status, override editor.

   In-app panels talk to the engine through the same action/state contract (via the
   shell's existing engine plumbing — no localhost round-trip required in-process),
   which keeps the contract honest: anything the native UI can do, Companion/OSC can
   do.
5. **Remote/secondary operation** is Companion + OSC in v1 (both first-class via the
   manifest). A browser surface for remote operators is a possible later addition —
   the protocol already supports it — but is explicitly **not** in v1. **Companion**
   needs zero action-side changes (manifest-driven); we add preset pages (slot buttons
   with tally-colored feedback, look buttons, cut/auto, queue paging) + the flattened
   feedback fields.
6. **Auth:** LAN-trust + shared token from config, stated honestly as such.

## 5. Data model (normative shapes)

Full JSON examples in the companion reference §0. Normative TS shapes:

```ts
type Role = "panelist" | "host" | "reader" | "aslpanelist" | "aslinterpreter";

interface Participant {           // zoomIngest output, keyed by participantId
  participantId: string;          // host's id (CVP participant id / zoomID)
  rawName: string;
  online: boolean; videoOn: boolean; audioOn: boolean; handRaised: boolean;
  zoomRole: number;               // display only
}

interface Panelist extends Participant {   // panelistDb output
  displayName: string; location: string;
  pin: string | null;             // 4-digit Mukana PIN
  hasMukana: boolean;
  role: Role;                     // editorial role (Mukana < override)
}

interface Slot {                  // liveSlots output; index 1..capacity
  slot: number;
  panelist: Panelist | null;      // null = EMPTY hole
}

interface GalleryCell {           // galleryDirector output; cell 1..16
  cell: number;
  slot: number | 0;               // 0 = blank
}

interface Look {                  // config-defined
  id: string;                     // e.g. "hr-q", "banter", "teatime", "panel-checks"
  scenePreset: string;            // host scene/preset id
  boxes: number;                  // guest boxes
  includesReader: boolean;        // the "2-series" layouts
  spxLayout: "A1"|"B1"|"C1"|"D1"|"A2"|"B2"|"C2"|"D2";
}

interface QueueState { prev: string[]; current: string | null; next: string[] } // PINs
```

Persistence: `liveSlots` + `galleryDirector` + `overrideDb` snapshot to an
atomic-write JSON state file on change (debounced); restored on start (generalizes
`zISO-routing.json` recovery).

## 6. Host adapter contract

*Events in (host → engine):* `participants` (join/leave/name/role/video/audio/hand),
`activeSpeaker(participantId)`, `capacity` (max concurrent participant slots),
`connectionState` (in-meeting, engine on/off).

*Commands out (engine → host):*
- `assignSlot(slot, participantId | null)` — bind a participant to a stable video slot
  (CVP: Show Input assignment / spine subscription; OBS: plugin source binding).
- `applyLook(lookId, boxes: Map<box, slot | null>)` — program composition (CVP: scene
  preset + source routes; OBS: scene switch + item visibility/transform).
- `setPreview(source)` / `cut()` / `auto(transitionId?)`.
- `setGallery(cells: Map<cell, slot | 0>)` — multiview/gallery composition.
- `setOverlay(id, visible, fields?)` — host-rendered overlays only (SPX goes direct).

**Capability declaration:** adapters report `hasPreviewBus`, `maxGalleryCells`,
`transitions[]`; the engine degrades gracefully (no preview bus → direct-cut only).
**Conformance suite:** a shared scenario test suite runs against every adapter (mock
host in CI; scripted obs-websocket for OBS) — the mechanism that keeps CVP-Windows,
CVP-Mac, and OBS semantically identical.

**ATEM → CVP semantic map:**

| Legacy | Show engine |
|---|---|
| ME1 PGM/PVW, cut/auto | `programBus` → host transport |
| ME2 active-speaker cut | AS-follow retargeting a dedicated "speaker" slot |
| SuperSource + boxes, named states | `Look` presets with per-box routes |
| DSK/USK | CVP overlay layers / SPX |
| Aux 10030 smart gallery | gallery scene/multiview |
| FTB | black look preset (v1) |

## 7. External integrations

- **`mukanaClient`** — poll intervals from config (defaults: panelists 5s,
  hands/question 2s); off-hours `"status"` body → keep last-good DB + "registry
  dormant" health, never an error; exponential backoff on network failure.
- **`spxClient`** — six template functions with exact param encodings (normalized to
  URI-encoded; the patch's unencoded `animateHeadlineIn` is a template-side check
  during migration), layouts A1–D2 → `f0..f12`, rundown/item control, primary + backup
  address failover, change-detection.
- **`tallyClient`** — publishes derived on-air PIN lists on change.
- **Error philosophy** (house rule: loud, never silent): every external failure is
  visible in `ohg.health` + surfaces; show logic never blocks on an external service —
  GFX/tally/Mukana are fire-and-observe.

## 8. Testing & migration

**Testing:**
1. **Unit/characterization per module** — tests written from Isadora behavior using
   the real data captured in the reference doc; divergence tests for the intended
   fixes; property-style invariants on `liveSlots` (unique zoomID per slot, ≤1 host,
   ≤1 reader, PIN≥9000 in the tail block) and `speakerRecency` (never evict the
   current speaker).
2. **Engine integration** — full pipeline against a scripted host-adapter mock +
   mocked Mukana/SPX HTTP; golden scenario files for real show flows (pre-show fill,
   host handoff, question segment, ASL interpreter speaking).
3. **E2E per host** — CVP: the fake Zoom engine drives the real app + show engine
   headlessly (the `validate-iso-record.mjs` harness pattern); OBS: adapter-level
   integration against obs-websocket; conformance suite in CI for every adapter.

**Migration (no big-bang; OHG is a live daily show — the bar for cutover safety is
correspondingly high, and shadow mode gets many reps quickly):**
1. **Shadow mode** — engine runs beside the Isadora rig on the same meeting via CVP,
   driving nothing; operators compare its state panel against reality; divergences
   logged. Validates the brain in production with zero risk. The comparator is kept
   afterward as a regression tool.
2. **Surface-first (optional, default out)** — a `universeBridge` driving the legacy
   rig from the new surface is explicitly deferred unless transition needs it.
3. **Pilot show** on CVP end-to-end (rehearsal/low-stakes), Isadora on hot standby.
4. **Cutover + retirement.**

Config migration: one-shot importer from `infraestructure-*.js` + `mukana-*.js`
(Mukana URLs, SPX addresses, tally URL, capacity; the bot fleet ceases to exist).

## 9. Per-platform work (delegable checklists)

- **Windows shell (`native-shell`)**: launch/supervise the engine subprocess; proxy
  `ohg.*` through `CoreVideoPro.Control`; merge `ohg` state node; native WinUI "OHG
  Show" workspace (§4.4).
- **mac-shell**: implement the `CoreVideoPro.Control` contract (manifest/state/invoke/
  WS/OSC) — the C# implementation is the reference, the manifest JSON the conformance
  artifact, `HttpControlServerTests`/`OscControlRoutingTests` define behavior; then the
  same subprocess supervision + native SwiftUI "OHG Show" workspace (§4.4).
- **OBS plugin**: engine subprocess supervision; control server (same contract);
  adapter mapping to OBS scenes/sources via the plugin's internals or obs-websocket.

## 10. Decisions log

- CVP replaces the ZoomISO/ATEM stack (not control-only, not hybrid) — jwallace,
  2026-08-04.
- Operator surface is **native in the shells** (our own design replacing what Universe
  did) on the shared protocol; no web surface in v1 (remote = Companion/OSC; browser
  surface possible later) — jwallace, 2026-08-04. Companion rides the manifest.
- OBS plugin is equal priority; enforced via host-agnostic engine + conformance suite.
- Mukana, SPX, tally all kept in v1.
- One control endpoint per host (engine registers actions; no second public server).
- Shipped Isadora quirks are fixed, not ported (§3 divergences).
- Auth stays LAN-trust + shared token in v1.
