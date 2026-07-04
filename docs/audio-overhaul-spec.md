# Audio Overhaul — Evaluation & Fix Spec

**Date:** 2026-07-03 · **Status:** IN DELIVERY (updated 2026-07-04) · **Owner symptom:** "No audio from Zoom or local
sources, the mixer doesn't do anything, VST / home-built plugins don't work at all. All outputs,
always, even idle." (Also raised: no virtual-camera return feed into Zoom — §8.)

> **Delivery status (2026-07-04):**
> - **4.1 Zoom PCM transport — SHIPPED** (ISO PCM ingest engine→SHM→core→mixer; rig-verified master bus tracking speech).
> - **4.2 Clock + pacer — SHIPPED** (absolute-deadline 50Hz pacer, underrun telemetry). **Feedback guard SHIPPED** PR #172 (monitor resolved-endpoint vs loopback endpoints → `monitorFeedbackRisk` + warning).
> - **4.3 A/V clock — SHIPPED** PR #163 (`RecordingPtsClock`, shared epoch; rig head/tail check still owed).
> - **4.4 Mixer completion — PARTIAL**: meter ballistics ✓ (#166), AUDIO PROOF de-countered ✓ (#166), Solo everywhere ✓ (#170), editable strips on tab ✓ (#170). **Remaining:** channel insert chains actually processing, EQ+gate kernels into the dispatcher, per-channel true LUFS/dBTP (or relabel), master-limiter toggle reconcile, silent-PCM row badge.
> - **4.4b Audio tab overhaul — B1–B4 SHIPPED** (#165, #166, #170, #176); B5 open — statuses in `audio-tab-redesign.md`.
> - **4.5 plugin host — NOT STARTED.** §8 virtual camera — NOT STARTED.

This spec is the output of a live-rig evaluation (real Zoom join, GoXLR endpoint,
2026-07-03 session) plus a three-track code audit (pipeline/clocking, mixer-UI wiring,
documented evidence/gaps). Every load-bearing claim carries a `file:line` anchor.

---

## 1. Executive summary

The audio subsystem is **not** a broken mixer on top of working audio — it is a **working mixer
on top of missing audio**. The control plane (faders, pan, mute, routing, bus inserts, meters,
BS.1770 master loudness) is genuinely wired end-to-end: UI → bridge command → core → real DSP on
real PCM. It reads as "dead" because in the shipped configuration **no PCM ever reaches it**:

1. **Zoom audio PCM never crosses the engine→core boundary.** The engine subprocess receives and
   counts 48 kHz packets (`audio_frame_received` with `byte_len` in the live log), but
   `ZoomEngineState.cpp:186-195` emits **metadata-only** `AudioFrame`s (empty `pcm`). Video got a
   shared-memory transport; audio never did. Every downstream consumer skips empty-PCM frames.
2. **The only wired local source captures silence.** Default local audio = WASAPI loopback of the
   default render endpoint (`AudioCaptureDeviceDiscoveryService.cs:130-139`), which resolved to
   GoXLR "Game" on the rig and delivered `signal=False peak=-120dB` with the app's own warning
   ("Audio capture is receiving silent PCM frames").
3. **The monitor was muted** (`monitorEnabled=False, monitorFrames=0`) — even present signal
   would have been inaudible.

Beneath the silence sit **structural clocking defects** that will surface the moment PCM flows
(glitching, drift, desynced recordings), plus **feature-absence** the UI currently fakes (VST,
channel inserts, EQ/gate). The live session self-reported it:
`fullChain=Full audio chain incomplete: capture, PGM, MON, stream, recording not yet proven.`

**Fix order that maximizes user-visible progress:** transport Zoom PCM (§4.1) → master audio
clock + pacer (§4.2) → recording A/V clock (§4.3) → mixer completion (§4.4) → plugin host (§4.5)
→ validation harness (§7). §4.1 alone makes meters move, the monitor audible, and Zoom audio
recordable.

---

## 2. Evidence (live rig, 2026-07-03)

- Engine receives Zoom audio: `[zoom-engine] {"cmd":"debug","stage":"audio_frame_received",
  "source_uuid":"participant-audio-16778240-mix","count":36000,"sample_rate":48000,"channels":1,...}`
- Shell audio telemetry (launch.log): `monitorEnabled=False monitorStatus=muted monitorFrames=0`,
  sole source `local-machine-audio … signal=False peak=-120.0dB rms=-120.0dB`, every bus tap
  (`master, mon, pgm-l, pgm-r, stream`) at `-120.0dB`, warnings include *"VST inserts are
  configured but live third-party plugin processing requires the dev VST bridge"* and *"No native
  PCM has been mixed for media, meters are held at silence for that channel."*
- Audio worker: `[audioOut] 47.3 ticks/s work=5.5ms` steady — 6% below the 50 Hz target while
  93% idle (pacing defect, not overload).
- `audio.gather` coreMutex holds 2–9 ms against a 1 ms budget (recurring guardrail warnings).
- Historical: the Phase 2 §6 audio soak FAILED under GPU co-load (worker collapse 47→0.6 ticks/s,
  14,473 capture underruns, `docs/alpha-evidence-2026-07-02.md:120-128`); the ears-on clean-rig
  soak has never been run.

## 3. Root causes, ranked

| # | Root cause | Effect | Anchor |
|---|---|---|---|
| R1 | No engine→core PCM transport for Zoom audio | Zoom silent everywhere; meters at −120 | `native/src/modules/ZoomEngineState.cpp:186-195` |
| R2 | No audio clock: worker drains-what's-queued; relative-sleep pacer lands 47.3 Hz and structurally cannot reach 50 | Under/over-fill of every real-time consumer once PCM flows | `native/src/rpc/JsonRpcServer.cpp:480,499-501`; `native/src/core/MediaCore.cpp:3899-3937` |
| R3 | Mixer bus overlap-sum bug: bus length = `max` of source packets, all summed at offset 0 | Multiple packets per tick collapse into one packet's worth → audio loss at slow ticks | `native/src/modules/StubModules.cpp:228,239-246` |
| R4 | Recording PTS: audio = sum of fed samples, video = tick-count × 1/60 s at 47.3 Hz, no shared clock, no resync, no frame dedup | Monotonic A/V drift in every recording | `native/src/modules/MediaFoundationEncoderAdapter.cpp:255-263,299-316,472-475` |
| R5 | Monitor underruns invisible: no counter, no log; shared-mode WASAPI plays silence into gaps | Audible glitching with zero telemetry | `native/src/modules/WasapiMonitorOutputAdapter.cpp:207-228` |
| R6 | Default config loopback-captures the same default render endpoint the monitor plays to; no guard | Feedback/echo loop out of the box | `WasapiAudioCaptureSourceAdapter.cpp:299-343`; `WasapiMonitorOutputAdapter.cpp:253-291` |
| R7 | VST host absent (UI mockup: `status:"scan-only"`); channel inserts stored but never processed; EQ/gate kernels exist but pass-through | "Plugins don't work at all" | `native/src/core/MediaCore.cpp:2550-2552`; `native/src/modules/AudioDsp.h:531-553` (EQ pass-through at `:550`) |
| R8 | No drift compensation anywhere internal (only ffmpeg `aresample=async=1` on RTMP) | Capture/render/tick clock skew accumulates | `native/src/modules/RtmpFfmpegArgs.h:77` |
| R9 | UX: monitor default-muted; silent-endpoint warning buried in a log line; telemetry log throttle defeated (signature embeds counters → ~4 lines/s) | Operator can't tell why audio is dead | `StudioViewModel.cs:5611-5625` |

## 4. Fix plan

### 4.1 P0 — Zoom audio PCM transport (engine → core → mixer)
Mirror the video path. The engine already holds the samples (raw-audio subscription in
`native/zoom-engine/.../engine-audio.cpp` — it logs `byte_len` per packet).
- Add an audio SHM ring (or length-prefixed pipe stream) per instance-token alongside the video
  regions (`engine-ipc.h` helpers already token-scope names). 48 kHz mono per participant +
  the mixed bus; seq-numbered 10–20 ms chunks with engine-clock timestamps.
- Core: `ZoomEngineRuntime` reader fills per-participant ring buffers;
  `pollCompositorAudioFrames` returns **real PCM** `AudioFrame`s (keep the metadata fallback for
  stub builds so tests stay deterministic).
- Acceptance: mixer channel for a live Zoom participant shows a moving meter; monitor renders
  their voice; recording MP4 carries it; `busTaps` leave −120 dB.

### 4.2 P0 — Master audio clock + pacer
- Replace the relative `sleep_for(budget − work)` with an absolute-deadline pacer
  (`sleep_until(start + n·20ms)` + the same spin-guard pattern the video render thread uses at
  `JsonRpcServer.cpp:443-449`). Target met = 50.0 ± 0.1 ticks/s.
- Introduce a **48 kHz sample-counter master clock** in the audio worker: each tick computes
  elapsed samples and produces exactly that much bus audio (sum-append sources sequentially —
  fixes R3's overlap-sum; carry per-source remainder buffers).
- Add monitor underrun/starvation counters (`WasapiMonitorOutputAdapter`: track padding==0 gaps +
  frames-silence-filled) surfaced in snapshot + telemetry line.
- Fix the defeated telemetry throttle (drop volatile counters from the signature).
- Guard R6: warn (and default-prevent) monitor endpoint == loopback-captured endpoint.

### 4.3 P1 — Recording/stream A/V clock unification
- Drive both PTS lanes from the master clock: video PTS = wall-anchored frame timestamps with
  frameNumber dedup (no double-mux of the same program frame); audio PTS = sample count (already)
  but resync-checked against video each second; insert/drop policy bounded (±1 frame).
- Acceptance: 10-min recording, measured A/V offset at head and tail < 40 ms (automated ffprobe
  check in `test:native-recording-proof`).

### 4.4 P1 — Mixer completion (make the UI honest)
- **Meter ballistics** (rig-observed 2026-07-03 once audio flowed): the UI renders the core's
  instantaneous per-tick levels raw, so meters strobe instead of dance. Apply standard console
  ballistics — fast attack, ~300ms exponential release, optional peak-hold — in the meter
  controls (`AudioLevelMeter`, master L/R). The variable per-tick sample quanta (480/960/1440)
  amplify the jumpiness; 4.2's fixed-cadence pacer helps but ballistics are needed regardless.
- **Replace the AUDIO PROOF counter string** (rig-observed): the transport bar binds
  `AudioProofSummary` — a raw diagnostic (`sources 1/1 | PCM 3672480 | …`) with an
  ever-incrementing frame counter. Show a clean status (e.g. "Audio: live · 1 source") and move
  the counters to the support bundle / log line where they belong.
- Process channel insert chains (feed per-source PCM through `applyBusInsertChain` before the
  crosspoint sends) — today they're stored, never run.
- Wire EQ + gate kernels (they exist: biquad stages, `applyNoiseGate`) into the dispatcher.
- Per-channel LUFS/true-peak: export real BS.1770 momentary + `computeTruePeakDbfs` per channel,
  or relabel the readouts (today "LUFS" = RMS dBFS, "dBTP" = sample peak — `StudioViewModel.cs:6930-6931`).
- Add the missing Solo button to `AudioMixerWindow.xaml` (model/bridge/core already support solo).
- Reconcile the master-limiter toggle (today it gates *reporting* only; buses are always
  brickwall-limited at `AudioDsp.h:521`).
- Monitor default: unmuted at modest volume when a monitor device is explicitly selected; surface
  the "silent PCM" warning as a visible UI badge on the source row, not a log line.

### 4.4b P1 — Audio tab overhaul (owner-reported backlog, 2026-07-03)
Now that audio is real (4.1/4.2 landed), the Audio tab needs to catch up. Owner-reported, all
four confirmed:
- **Doesn't reflect reality**: controls/status shown don't match what the engine actually does
  (pre-dates real PCM; every state readout needs re-verification against the live chain).
- **Routing matrix unusable**: the crosspoint UI needs a usable model — sources × buses with
  the defaults visible (Zoom participants now get auto-sends; the operator must be able to SEE
  and override them per crosspoint).
- **Device selection broken**: capture/monitor device pickers misbehave; must also surface the
  silent-endpoint warning and (once 4.2's guard lands) the monitor==loopback feedback risk.
- **Layout/UX overhaul**: redesign the tab, not spot fixes. Design pass first: what an operator
  needs during a show (monitor control, quick mutes, routing at a glance) vs setup time
  (devices, matrix, processing).

### 4.5 P2 — Real plugin host
- Out-of-process VST3 (then CLAP) host — a separate `corevideo-plugin-host.exe` bridged over
  shared memory (crash isolation; a misbehaving plugin must not take down the show). Scan,
  parameter surface, per-bus insert slots first; channel slots after 4.4.
- The current "VST3 Bridge Slot / scan-only" UI becomes real or is hidden behind a dev flag until
  it is. **The UI must stop advertising processing that doesn't exist.**

## 5. What is already good (don't rebuild)
Control-plane wiring (fader/pan/mute → `sync-participant-audio-mix` → real DSP), routed-bus
matrix + solo/mute/pan math, bus compressor/limiter, BS.1770 master metering, true-peak kernel,
roster-driven channel list, meter rendering (zero fabrication), device enumeration. The DSP unit
suite (50+ tests) is solid.

## 6. Explicit non-causes (ruled out in this eval)
- The mixer UI is not a mockup (fully bound; verified control-by-control).
- The audio worker is not overloaded (5.5 ms work in a 20 ms budget); the 47.3 Hz is pacing.
- No duplicate C# audio path fights the core (shell only enumerates devices).

## 7. Validation harness (closes the "never proven" gap)
- Unit: pacer cadence test (mock clock), sum-append mixer test (N packets → N packets out),
  underrun counter tests, PTS drift test (simulated 47 Hz ticks → assert bounded drift).
- Integration: tone-through-chain proof — inject a 1 kHz tone as a capture source, assert
  continuity (no gaps > 1 ms) at the monitor tap and in the recorded AAC (extend
  `native-recording-proof`).
- Rig: the ≥10-min ears-on soak (Phase 2 §6 gate) after 4.1+4.2 land, with the new underrun
  counters at zero as the pass criterion.

## 8. Separate epic — virtual camera return feed to Zoom
Out of scope for the audio overhaul but recorded per owner request: expose the program output as
a Windows virtual camera (MediaFoundation Virtual Camera framework, Win11 — `MFCreateVirtualCamera`,
same approach as OBS's Win11 path) so Zoom can select "CoreVideo Pro Program" as its webcam;
pair with a virtual audio return later. Needs its own spec (packaging/signing implications:
virtual cameras require a signed, registered component).

## 9. Sequencing & effort (rough)
| Phase | Scope | Effort | Unblocks |
|---|---|---|---|
| 4.1 | Zoom PCM transport | 2–4 days | Everything user-facing |
| 4.2 | Clock + pacer + counters + feedback guard | 1–2 days | Glitch-free monitor |
| 4.3 | A/V clock | 1–2 days | Trustworthy recordings |
| 4.4 | Mixer completion | 2–3 days | Honest, complete mixer |
| 4.5 | Plugin host | 1–2 weeks | VST |
| §7 | Harness + soak | 1–2 days | Alpha gate |
