# VST3 Plugin Host — Design Spec (audio overhaul 4.5)

**Date:** 2026-07-04 (P2c delivered 2026-07-12) · **Status:** P1/P2a/P2b/P2c shipped ·
**Parent:** `docs/audio-overhaul-spec.md` §4.5 (root
cause R7). With 4.4 shipped (built-in gate/EQ/comp/limiter genuinely processing), third-party
plugin hosting is **the last fake in the audio chain**: a "VST3 Bridge Slot" insert is stored and
labeled "scan/load bridge required" (`StudioViewModel.cs:1321`), and unrecognized insert names
pass through the channel/bus chains untouched (`AudioDsp.h`, by design).

Design follows the house typed-native-boundary discipline and reuses the proven Zoom-engine
subprocess pattern (per-instance-token IPC names, JSON-line control pipe, seqlock SHM for
real-time payloads, reader/sender threads outside `coreMutex`).

## 1. Goals / non-goals

- Real VST3 processing on channel and bus insert chains, **crash-isolated**: a misbehaving
  plugin must never take down the show or glitch program audio beyond one bypassed tick.
- The built-in 4.4 DSP is the **guaranteed fallback** at every failure point (host absent,
  crashed, plugin failed probe, deadline missed).
- Non-goals (v1): plugin GUI editor embedding (phase P4), CLAP (P4), sidechains, >2ch.

## 2. Architecture

One `corevideo-plugin-host.exe` **child of the media core** (not the shell), spawned on demand
when a probed plugin is first inserted. One host process for all plugin instances (v1);
death of the host = ALL third-party inserts bypass to built-ins + a warning, restart with
capped backoff and state re-injection. (Per-plugin isolation processes are a P4 option for
plugins flagged unstable.)

**Respawn backoff (AS BUILT, round-2 A1):** `PluginHostRespawnPolicy` (pure, unit-tested)
gates `ensurePluginHostServeStarted` — retries back off 5→10→20→40→60s and after 5
consecutive failed respawns the core GIVES UP: the insert stays loudly auto-bypassed
(`serve.lastError` + `serve.respawn{attempts,gaveUp}` + chip BYPASS status) while audio
keeps flowing unprocessed. A run alive ≥30s counts healthy and resets the ladder (the
host's by-design 30s idle exit never pays a backoff). Operator actions reset the ladder:
selecting a different plugin or clicking "Open controls". State re-injection after
respawn is still open (round-2 PR 2, with get/set-state).

- **Control plane** — JSON lines over a token-named pipe (mirrors `engine-ipc.h` naming):
  core → host: `scan`, `probe {pluginId}`, `load {instanceId, pluginId, channels, sampleRate}`,
  `unload {instanceId}`, `set-param {instanceId, paramId, normalized}`, `get-state`/`set-state`
  (persistence blobs, base64).
  host → core: `scan-result {plugins[]}`, `probe-result {pluginId, pass, reasons[]}`,
  `loaded {instanceId, params[], latencySamples}`, `param-changed`, `state`, `error`, periodic
  `health {rssMb, instances}` heartbeat.
- **Audio plane** — per-instance SHM block pair (in/out) with seq numbers, same
  copy-then-recheck seqlock discipline as Zoom frame/PCM regions. The audio worker writes the
  insert point's stereo float block (48 kHz, one tick = ~960 frames), signals an event, and
  waits on the host's completion event **with a hard deadline (default 4 ms)**. Deadline miss →
  that tick BYPASSES the plugin (built-ins still ran), `deadlineMisses` increments, and three
  consecutive misses auto-bypass the instance with a warning until the operator re-enables.
  The audio worker never blocks unboundedly and never takes `coreMutex` while waiting
  (lock order: the exchange happens inside the `audioOutputMutex_` work span, no new locks).

## 3. Safety posture (inspect, but stay blocked)

- **Scan** enumerates `%COMMONPROGRAMFILES%\VST3` read-only — metadata only, no code loaded
  into the core or shell, ever. Scanning happens **in the host process**.
- **Probe** (in the host, still isolated): load the plugin, feed a known test block, verify —
  returns within deadline, output finite (no NaN/Inf), |gain| bounded (< +24 dB on the test
  signal), parameter set enumerable, state round-trips. Fail → plugin listed as `unhostable`
  with reasons; it can never be inserted. Pass → insertable.
- A plugin reaches live audio ONLY after passing probe, and even then every tick is
  deadline-bounded with bypass-on-miss. The built-in chain never leaves the path.

## 4. Protocol / snapshot additions

Snapshot (`audioMixSession.pluginHost`):
`{status: "absent"|"starting"|"ready"|"degraded"|"crashed", plugins: [{id, name, vendor,
probe: "pending"|"pass"|"fail", reasons[]}], instances: [{instanceId, slot, pluginId,
latencyMs, deadlineMisses, bypassed}]}` + warnings on the existing channel. Wire fields ride
the existing snapshot; C# models auto-bind camelCase (B1 lesson: check before writing a parser).

Insert-chain integration (AS BUILT, P2b/P2c): a **bus** insert whose name contains
`vst`/`host` routes the bus block through the host exchange before
`applyBusInsertChain` runs the built-ins. Naming convention (P2c):
- `vst:<class or plugin name>` — selects a REAL scanned plugin (case-insensitive; exact
  class match, then class substring, then plugin/bundle name). WaveShell-style shell
  bundles expose many classes — `vst:<bundle>/<class>` disambiguates explicitly.
- plain `vst` / `Host Test Gain` (no colon) — the host's built-in -6 dB test processor
  (kept so the transport rig drill works without any plugin installed).
The selection rides the SHM block (`pluginBundle`/`pluginClass`); the host loads the
plugin on demand ON ITS SERVE THREAD (the core deadline-bypasses during the load) and
caches it per selection for the process lifetime. Host status
(`activePlugin`/`lastError`/`statusCode`) rides back in the block and is surfaced at
`pluginHost.serve{}`; an unresolvable `vst:` name bypasses loudly
(`serve.lastError` + rate-capped core log), never fakes processing. Channel inserts are
P3.

## 5. Phases (each shippable)

| Phase | Scope | Proof | Status |
|---|---|---|---|
| P1 | Host exe skeleton + **real scan** + snapshot surfacing; shell plugin browser lists real plugins | Installed plugins appear in the snapshot; core untouched by plugin code | **SHIPPED** (`--scan`, `PluginHostScan.h`) |
| P2a | **Probe**: load the module in the host process, enumerate classes via raw COM-ABI factory vtables (no VST3 SDK), pass/fail verdicts incl. crash-on-load | `--probe <bundle>`; a crashing plugin kills only the probe process | **SHIPPED** |
| P2b | Live processing on **bus** inserts: single-slot SHM block exchange + req/done events, 4ms deadline bypass, bypass-on-host-death, serve auto-start | Transport e2e test (kill the host mid-test → bypass, no hang); rig drill with the -6dB test processor | **SHIPPED** |
| P2c | **Real VST3 instantiation + processing**: raw COM-ABI IComponent/IAudioProcessor (vst-abi.h, layout static_asserts), `vst:<name>` insert selection against scan results (shell-bundle classes supported), load-on-demand + per-selection cache in the serve loop, status/error telemetry (`serve.activePlugin`/`lastError`), `--process <bundle> <class>` CLI proof mode, fake-factory ABI unit tests | `--process` against an installed plugin prints rms/changed JSON; failure paths (license refusal, non-stereo, NaN, crash) bypass honestly | **SHIPPED** (see PR; Waves headless verdict recorded there) |
| P3 | **Channel** inserts (SHIPPED — `AudioDsp.h` routes channel + bus chains through the host) + generic parameter surface (slider list from `params[]`) + state persistence in production prefs | Param moves are audible + survive restart | **PARTIAL** (routing shipped; params/state = round-2 PR 2) |
| P4 | Plugin editor GUI, CLAP, per-plugin isolation for flagged plugins | — | **PARTIAL** (round-2 A1): editor opens as a top-level window IN THE HOST (`vst-processor.h showEditor`), centered + raised best-effort (background processes lack foreground rights — topmost pulse + taskbar flash), one editor at a time, clean detach on close (`removed()` before DestroyWindow), user-close republishes idle status. Editor telemetry `pluginHost.serve.editor{StatusCode,ActivePlugin,LastError}` surfaces on the shell chips + rack status line ("This plugin has no editor" for createView-null). Root cause of the original "no UI ever" defect: the shell sent `open-vst-editor` as a top-level RPC and `JsonRpcServer` rejected it unrouted (silently discarded) — now routed + regression-pinned (`JsonRpcServerTest.TopLevelOpenVstEditorRoutesToMediaCore`). Shell-owned child-window embedding + CLAP remain open. |

## 6. Testing

- Protocol: pure serializer/parser tests both sides (mirror ZoomEngineClient tests).
- A **fake plugin host exe** (like `corevideo-zoom-engine-fake.exe`) speaking the real IPC with
  a synthetic "gain -6dB" plugin — drives core e2e tests + the stub CI gate without VST3 SDK.
- DSP determinism: bypass-on-miss leaves output bit-identical to built-ins-only.
- Failure drills as tests: host absent, host killed, probe-fail plugin inserted (must refuse).

## 7. Effort

P1 ≈ 2-3 days (the VST3 SDK module-info scan is self-contained); P2 ≈ 3-4 days (SHM exchange +
worker deadline discipline); P3 ≈ 2-3 days; P4 open-ended. VST3 SDK is GPLv3/proprietary
dual-licensed — vendor under `third_party/vst3sdk` behind a CMake option so stub/CI builds
never require it.
