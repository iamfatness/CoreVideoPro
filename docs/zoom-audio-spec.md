# Zoom Audio — Architecture Spec & Fix Plan

Status: owner-requested 2026-07-05 ("write a spec and create a plan to fix the zoom audio,
use all the lessons learned from the two repos"). Reference engines: **Tracktion Engine**
and **Ardour** (GPL — architecture references only; read, learn, reimplement clean).
Companion: `audio-pull-monitor-spec.md` (the pull-model output side, shipped),
`audio-overhaul-spec.md` §0 (the bar: sync paramount, clean sound the standard).

## 1. Where we are (instrument-proven, 2026-07-05 session)

Fixed and verified on the rig:

| # | Defect | Fix | Verification |
|---|--------|-----|--------------|
| RC1 | Single-slot SHM snapshot lost/duplicated packets under any reader lag | 32-slot sequenced ring (`engine-ipc.h`), ~320ms lossless catch-up | zero lost-packet logs live |
| RC2 | Analyzer `noiseSuppressionActive` heuristic echoed into the channel *setting* | suppression is operator-only | ns=0 in wire dumps |
| RC3 | New Zoom rows defaulted to NS=true + EQ+Compressor inserts | clean-by-default | ins= empty in wire dumps |
| RC4 | Active-speaker ring recreated on speaker change; reader cursor stranded | reader detects writer restart, resyncs | entrance scans clean |
| RC5 | Steady-feed overflow drop not frame-aligned → L/R flip on stereo | frame-aligned drops | (music retest pending) |

Open (the reasons this spec exists):

- **O1 — Internal double-path**: `zoom-mix` (meeting mixed audio) AND per-participant ISO
  streams BOTH auto-route to the same buses. The same remote voice traverses our mix twice
  at ~110ms skew (measured: autocorrelation 0.334 @ 110ms on the monitor endpoint) —
  heard as echo/garble/warble. The owner correctly called this "in our engine."
- **O2 — No inter-stream/inter-source time discipline**: Zoom ISO and mixed streams have
  different inherent latencies; no per-source clock-drift handling exists between Zoom's
  packet clock and our 50Hz worker (the feed FIFO absorbs phase jitter but not sustained
  rate offset); nothing declares or compensates latency anywhere.
- **O3 — ISO streams are server-gated by Zoom** (silence suppression between talk bursts):
  resumptions arrive as packet-flow restarts. We render them faithfully — including any
  hard onset. Needs deliberate declick treatment and honest UX labeling.
- **O4 — Monitor-endpoint contention**: any other process (the Zoom client itself when the
  operator is also joined interactively) rendering meeting audio to the operator's monitor
  endpoint superimposes a second copy we cannot see in our PCM taps.
- **O5 — Verification is manual**: every regression so far was found by the owner's ears
  or ad-hoc captures; nothing runs continuously.

## 2. The reference-engine lessons, mapped

From **Tracktion Engine** (device-as-clock, engine-quality resampling) and **Ardour**
(process-callback graph, PDC latency compensation, every-change-is-a-ramp, explicit port
routing):

1. **One master sample clock; every external stream is a "device" that gets rate-matched
   to it.** Tracktion never lets two clocks meet without a resampling client between them.
   → Zoom's packet stream is an external clock. It needs the same treatment the monitor
   got: a depth-trimmed buffer, not an ad-hoc FIFO with a drop cap. (Phase Z2)
2. **Latency is declared, then compensated (Ardour PDC).** Every node/source reports its
   latency; the graph delays faster paths so summed signals align.
   → ISO vs zoom-mix skew, and monitor-vs-program-video alignment, are PDC problems.
   (Phase Z3)
3. **Explicit routing; nothing is implicitly summed twice (Ardour's port model).**
   → zoom-mix and ISO must have exclusive default routing; duplicate paths to one bus
   require an explicit operator act. (Phase Z1 — the P0)
4. **Every discontinuity gets a ramp (Ardour region fades / declick).**
   → ISO gating resumptions and source add/remove get short fade-in/out ramps at the
   mix ingest, exactly like Ardour declicks region boundaries. (Phase Z2)
5. **Observability per node.** Both engines meter and count xruns per node, not globally.
   → per-source continuity counters (packets, gaps, resyncs, fades applied) surfaced in
   telemetry permanently. (Phase Z4)

## 3. Target architecture (Zoom ingest side)

```
Zoom SDK callbacks (engine process, SDK clock)
  └─ per-stream SHM ring (RC1/RC4, shipped)                 [lossless transport]
core reader (event-driven drain, cursor per stream)          [shipped]
  └─ per-source IngestBuffer  ← NEW (Phase Z2)
       • depth-trimmed like the monitor ring (target ~3 ticks)
       • declick ramps on flow start/stop (ISO gating, joins/leaves)
       • per-source drift trim: slow ppm resample keyed on buffer depth
         (platform-quality resampler; never hand-warp — servo lessons)
       • declares its latency  ─┐
mix graph (50Hz worker)          │
  • routing: zoom-mix DEFAULT → program buses; ISO DEFAULT → unrouted (Z1)
  • PDC-lite: per-source delay lines align declared latencies before sum (Z3)
  • DSP chain (stateful, ramped — shipped)
buses → pull-model monitor / program taps (shipped)
```

## 4. The plan

### Phase Z1 — Exclusive default routing (the P0; kills the audible echo)
- New Zoom participant ISO rows default to **no sends** (visible in the mixer, unrouted,
  labeled "ISO — route deliberately"). `zoom-mix` defaults to the standard 5-bus routing
  and is labeled "Meeting Mix (program default)".
- Migration: on snapshot apply, if BOTH zoom-mix and ISO rows carry live sends to the same
  bus, warn once in the master rail ("Zoom double-routing: meeting mix and ISO both feed
  MON") — never silently unroute operator choices.
- Owner-visible acceptance: join a meeting, monitor zoom-mix only → no echo; autocorr of a
  monitor loopback capture shows no secondary peak ≥0.15 between 20–400ms.
- Files: `StudioViewModel` routing-matrix defaults + row labels; warning plumbing already
  exists (audioRoutingWarnings_).

### Phase Z2 — Per-source ingest discipline (Tracktion device treatment + Ardour declick)
- Replace the raw `pendingAudio_` → steady-feed hop for Zoom sources with an
  `IngestBuffer` (reuses `SpscRing` + the monitor's depth-trim pattern; target depth
  ~3 ticks; drift trimmed by slow platform-resampler ratio, ≤±500ppm, 5s windows).
- Declick: 5ms fade-in when a stream starts/resumes after ≥1 missing tick; 5ms fade-out
  applied retroactively is impossible, so keep a 1-tick lookback in the buffer and fade
  the tail when flow stops (Ardour-style region-edge ramps).
- Frame-alignment invariant asserted in one place (the RC5 lesson becomes structural).
- Tests: continuity across simulated gating gaps (fade applied, no step); drift soak
  (±200ppm synthetic clock, buffer depth stays bounded, zero drops); stereo alignment
  property test.
- Files: `native/src/modules/AudioIngestBuffer.h` (new, pure, tested),
  `ZoomEngineRuntime` drain path, `MediaCore` gather.

### Phase Z3 — Latency declaration + alignment (Ardour PDC-lite)
- Each source declares latency: zoom-mix (Zoom mixer delay, measured), ISO (~0),
  capture sources (driver period), VST inserts (`getLatencySamples` via the host, when
  P2c lands). Sum alignment: per-source delay line pads faster sources up to the max
  declared latency of the sources sharing a bus (bounded, e.g. ≤200ms).
- Monitor A/V: operator program display delayed by the measured monitor chain latency
  (owner accepted monitor latency; sync is the bar). Latency measured by the Tracktion-
  style pulse loop test in Z4, stored per configuration.
- Tests: two synthetic sources with declared skew sum in-phase after alignment.

### Phase Z4 — Continuous verification (the rig stops needing ears)
- Fake-engine audio mode: `corevideo-zoom-engine-fake` emits a deterministic tone +
  timestamp pattern over the real ring transport → CI-adjacent soak: spectral flatness,
  click scan, autocorr echo scan, gap census — machine-verdict on every audio PR.
- Promote the session toolkit (loopback-rec, click/timeline/autocorr scans) into
  `tools/audio/` in-repo with a README; mono/stereo-aware.
- Per-source telemetry: packetsIngested / gapsResynced / fadesApplied / bufferDepth /
  trimPpm in `audioMixSession`, surfaced in the Diagnostics drawer.
- Endpoint-contention guard (O4): enumerate render sessions on the monitor endpoint
  (IAudioSessionManager2); if a foreign session is active while our monitor plays, show
  the master-rail warning with the offender's process name.

### Sequencing & effort
Z1 is small and lands first (one session, kills the echo). Z2 and Z4's fake-engine mode
are the substantial pair (1–2 sessions each) and unlock ear-free iteration. Z3 rides on
Z2's buffer. Each phase ships with its tests green and a rig verification note appended
to this spec.

## 5. Invariants carried forward (the week's laws)

1. Streaming DSP state persists across blocks. 2. Control-plane transients never puncture
the data plane. 3. Never hand-roll sample-rate conversion on a live path; platform
resamplers, slow trims, depth-keyed signals only. 4. Every interleaved-buffer erase is
frame-aligned. 5. Telemetry and settings are separate fields, always. 6. Every
discontinuity gets a ramp. 7. Explicit routing — nothing summed twice implicitly.
8. Instrument first, fix second; a fix without a measurement is a guess.
