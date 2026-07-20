# Master Processing + VST Round 2 — Design Spec

Status: designed 2026-07-19 with the owner ("master processing and VST processing
still needs lots of work"), grounded in a same-day code audit of main (cf31a43).
Owner decisions captured: **both tracks in parallel**, **full plugin-latency
compensation** (sync is the north star), **multiband in scope now**. Reference-
matched mastering ("sound like X") explicitly deprioritized this round.

Companion specs: `docs/mastering-chain-spec.md` and `docs/vst-host-spec.md` — both
lag shipped reality (topology question is resolved in code, P3 channel routing
shipped); each track's PRs refresh the relevant status tables in the same change.

## 0. Audit baseline (what is real today)

- Mastering chain (`native/src/modules/AudioMastering.h:120-255`): input trim →
  HP/LP filters → 3-band tone → momentary-LUFS ride → fixed 2:1 glue (params not
  exposed) → M/S width → **sample-peak** ceiling. True-peak is metered
  (`computeTruePeakDbfs`, 4× oversampled, `MediaCore.cpp:3067`) but never limited.
  Topology: mastering applied ONCE on `master`; pgm-l/r/stream/mon inherit
  (`MediaCore.cpp:4641-4678`, owner-confirmed 2026-07-06). Presets are 4 hardcoded
  C# records; A/B slots are session-only (`StudioViewModel.cs:2428-2520`).
- VST: out-of-process host (raw COM-ABI, no SDK), 4 ms deadline bypass, channel +
  bus inserts both route (`AudioDsp.h:921-931`). #291 added editor hosting
  (`vst-processor.h:678-768` `showEditor()`: top-level window in the host process,
  "Open controls" per slot chip) — **owner reports no UI ever appears** (live
  defect). **No param bridge, no getState/setState, no state persistence** — spec
  §get-state/set-state unimplemented. Host respawns after death
  (`MediaCore.cpp:4577-4578`) but with **no backoff** and no state re-injection.
  Plugin latency is reported (`getLatencySamples`) but not carried over the SHM
  block (`host-transport.h` has no field) and **never compensated**.

## 1. Track A — VST

### A1. Editor launch fix + host reliability (PR 1) — **SHIPPED 2026-07-19**

Status: Phase 0 diagnosis PROVED the root cause — the shell sends
`open-vst-editor` as a TOP-LEVEL RPC and `JsonRpcServer::handle` had no route
for it (only `media-core-sync`/`commands` batches reached `handleCommand`), so
the core answered `protocol-error "Unsupported native media-core command"` and
`MediaCoreSupervisor.OpenVstEditorAsync` disposed the response unread: a fully
silent no-op. The editor machinery itself worked when driven correctly
(headless probe: Waves Curves AQ Stereo editor opened, attached, plugin-resized
via IPlugFrame). Shipped: top-level routing + regression test, supervisor
ok-check (rejections now surface as status text), editor window centered +
raised best-effort (background-process foreground denial was real — topmost
pulse + taskbar flash), single-editor invariant, clean detach on WM_CLOSE
(`removed()` before DestroyWindow), user-close republishes idle status,
`serve.editor*` and `serve.respawn{attempts,gaveUp}` surfaced on chips + rack
status line ("This plugin has no editor" for createView-null), and the respawn
backoff ladder (`PluginHostRespawnPolicy`, 5→10→20→40→60s, give up after 5).
Editor embedding stays out (P4 follow-up).

- **Phase 0 (diagnosis, first):** reproduce the editor failure live via
  systematic-debugging — drive `open-vst-editor` against a real WaveShell, read
  `pluginHost.serve.editor{Status,LastError}` + host stderr, and identify why the
  window never appears (candidate causes: message pump not running/blocked on the
  serve thread, window created without activation/foreground rights from a
  background process, plugin rejecting `attached()` on that thread, editorStatus
  error swallowed). The fix follows the root cause, not a guess.
- Editor lifecycle hardening regardless of root cause: editor window must appear
  on the operator's desktop, foregrounded; closing it detaches cleanly
  (`removed()` before destroy); a second "Open controls" focuses the existing
  window instead of double-attaching; editor errors surface on the slot chip
  (status text), never silently.
- **Respawn backoff + auto-bypass** (spec `vst-host-spec.md` §42, unimplemented):
  host respawn follows the house backoff pattern (5→10→20→40→60 s, give up after
  5 consecutive failures → insert auto-bypasses LOUDLY with a chip status; manual
  re-enable resets). A crash-on-load plugin must not hot-loop respawn during a
  show.
- Editor embedding as a shell-owned child window is explicitly OUT of this round
  (P4 follow-up); a reliable top-level window is acceptable for beta.

### A2. Params + state — the real P3 (PR 2)

- **Param bridge:** extend the host protocol (SHM block + host-transport.h,
  version-bump the magic like CVP2 did) with param enumeration
  (`IEditController` param count/info/normalized values) and set-param requests.
  Generic param surface in the insert flyout: slider per param (name, normalized
  0-1 mapped display), following the existing InsertParamSpecs flyout pattern.
  Params apply live (host applies via `setParamNormalized` +
  `performEdit`-equivalent on the processor where required).
- **State persistence:** `getState`/`setState` (IBStream over the block or a
  side-channel file if component state exceeds the block; chunked if needed).
  State blobs (base64) persist in ProductionOutputPreferences per insert slot
  (bus/channel id + insert name), schema-bumped with migration. Restore applies
  on selection load; **re-injection after host respawn** (closes the
  respawn-loses-state gap from A1's backoff work).
- Editor tweaks therefore survive restart: editor edits mutate controller state →
  captured by getState on a debounce/timer and at shutdown.

### A3. Latency compensation (in PR 2, decided: COMPENSATE)

- Carry `latencySamples` per active instance through the SHM block →
  `pluginHost.serve` telemetry → the mix.
- Compensation model: delay-align at the mix so a plugin-hosting chain is not
  late vs the rest — global alignment to the largest active reported latency,
  applied as compensating delay lines on non-delayed paths (implementation
  detail for the plan: reuse the steady-feed/delay-line primitives; the delay
  changes only on plugin selection change, with a declick ramp). A/V PTS: the
  added audio latency must ride the existing shared-epoch PTS clock so
  recordings stay in sync (audio is delayed, timestamps must reflect it).
- Surface total added latency in the audio telemetry + a per-insert latency
  badge; 0-latency plugins add nothing.

## 2. Track B — Master

### B1. DSP quality (PR 3)

- **True-peak limiter:** replace the sample-peak ceiling with a true-peak
  limiter using the existing 4× oversampled detector shape (`computeTruePeakDbfs`)
  — inter-sample overs held below `ceilingDbfs`. Continuity rules apply
  (persistent state across ticks — the C7c/C7d lesson). A/B'd by unit tests:
  ISP-heavy synthetic signal exceeds ceiling pre-fix, holds post-fix.
- **Exposed glue dynamics:** ratio / attack / release / makeup operator-exposed
  (currently fixed 2:1, 30/250 ms), with the current values as defaults.
- **Multiband glue (owner: in scope now):** 3-band mode (Linkwitz-Riley-style
  crossovers ~200 Hz / ~3 kHz, per-band glue with shared character controls +
  per-band gain), switchable single-band ↔ multiband; single-band remains the
  default until the owner's listening pass. Neutral settings = bit-identical
  bypass, matching the house rule every mastering stage already obeys.
- All stages keep persistent DSP state across ticks (the block-boundary
  distortion class is a solved lesson — every new kernel carries state).

### B2. Rack workflow + persistence (PR 4)

- Presets, A/B slots, and current mastering settings persist in
  ProductionOutputPreferences (schema bump + migration); user-saved named
  presets (save/rename/delete) alongside the 4 built-ins.
- Mastered-signal metering on the master rack: integrated LUFS + true-peak of
  the POST-mastering signal with target guides (meters already exist in the
  core; expose them on the rack, not just program-wide).
- Rack UI pass: the mastering panel reads as one rack (stage order visible,
  per-stage engage state bright/dim like the DspResponseCurve convention),
  A/B + preset controls adjacent to the meters they affect.
- Spec refresh: `mastering-chain-spec.md` topology question closed, TP status
  honest; `vst-host-spec.md` phase table corrected (P3 channel routing shipped,
  P3 params/state = PR 2, P4 editor partial).

## 3. Constraints and invariants

- Audio worker discipline unchanged: 4 ms host deadline bypass stays; no new
  work under `coreMutex`; lock order preserved; all new DSP stateful across
  ticks.
- Never fake processing: bypass is always loud (chip status, telemetry).
- 0xc000027b rules: param sliders/flyouts are rebuilt-on-open surfaces (the
  established insert-flyout pattern), no new snapshot-rate bound collections.
- Prefs changes stack on the S4/O1 schema chain (v5) — coordinate version bumps
  with open PRs #297/#298; if they merge first, bump to v6, else rebase.
- Every PR: unit tests at each layer, WinUI + native suites green, "Morning rig
  test" section, docs refreshed in the same change.

## 4. Sequencing

PR 1 (A1 editor+reliability) and PR 3 (B1 DSP) start in parallel — independent
code. PR 2 (A2/A3 params+state+latency) follows PR 1 (protocol version bump
builds on the editor/status plumbing). PR 4 (B2 rack+persistence) follows PR 3
(meters/stages it exposes) and coordinates the prefs bump with A2's state blobs.
Owner listening pass gates the multiband default, not the merge.
