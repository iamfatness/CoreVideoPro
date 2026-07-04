# VST3 Plugin Host — Design Spec (audio overhaul 4.5)

**Date:** 2026-07-04 · **Status:** proposed · **Parent:** `docs/audio-overhaul-spec.md` §4.5 (root
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

Insert-chain integration: a channel/bus insert named `vst3:<pluginId>` resolves to a host
instance; `applyChannelInsertChain`/`applyBusInsertChain` gain a host hook that exchanges the
block when (and only when) the instance is live, else falls through to pass-through. The
"VST3 Bridge Slot" placeholder name is retired.

## 5. Phases (each shippable)

| Phase | Scope | Proof |
|---|---|---|
| P1 | Host exe skeleton + token IPC + **real scan/probe** + snapshot surfacing; shell plugin browser lists real plugins with probe status; "scan-only" label retired | Rig: installed plugins appear with pass/fail; core untouched by plugin code |
| P2 | Live processing on **bus** inserts: SHM block exchange, deadline bypass, crash restart w/ re-inject | Rig: audible third-party EQ on the MON bus; kill -9 the host mid-show → program audio continues on built-ins + warning |
| P3 | **Channel** inserts + generic parameter surface (slider list from `params[]`) + state persistence in production prefs | Param moves are audible + survive restart |
| P4 | Plugin editor GUI (child-HWND host window), CLAP, per-plugin isolation for flagged plugins | — |

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
