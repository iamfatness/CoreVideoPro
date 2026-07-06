# First-Party Mastering Chain — Master Bus Processor Spec

Status: owner-requested 2026-07-05 ("a Mastering VST built by us that will sit on the
Master Bus, Which should be the same as Left and Right Bus"). Written 2026-07-05
(overnight session). References studied per owner: **sergree/matchering** (GPL-3 —
reference-matching mastering; architecture only, clean reimplementation) and
**Wamphyre/oXygen** (chain topology reference). Same license discipline as
Tracktion/Ardour: read, learn, never copy.

## 0. OPEN QUESTION FOR THE OWNER (blocks topology, not design)

"Master ... should be the same as Left and Right Bus" — reading this as: **master and
program L/R must carry the identical signal**, i.e. the mastering chain is applied ONCE
and everything downstream of it (master, pgm-l, pgm-r, stream, recording) inherits the
processed signal. If instead you meant the chain should be *insertable on L and R
separately with linked settings*, say so — the DSP is identical either way; only the
bus wiring differs. **Please confirm the intended topology before Phase M2 wires it.**

## 1. What it is

A broadcast mastering processor that sits on the Master Bus and makes the program sound
finished by default — the "sounds professional out of the box" feature that vMix users
buy third-party plugins for:

```
input gain → EQ (flat | reference-matched curve) → glue compressor
   → true-peak limiter → LUFS loudness normalization → master
```

- **Loudness normalization**: continuously steers integrated loudness toward the
  configured target (-14 LUFS streaming / -16 / -23 EBU R128 presets — the targets
  already in the master rail's meter). Slow gain rides (minutes-scale integrator),
  never pumping; the limiter catches what the ride can't.
- **Glue compressor**: gentle 2:1-ish, slow attack, program-dependent release — the
  existing `applySmoothedCompressor` kernel with a mastering parameter map.
- **True-peak limiter**: the shipped `applySmoothedPeakLimiter` (peak-hold detector,
  LimiterState) extended with 4x oversampled true-peak detection (ITU BS.1770 TP) —
  ceiling -1.0 dBTP default.
- **EQ**: the shipped 8-section biquad chain; flat by default; can load a
  **reference-matched curve** (below).

## 2. The matchering idea, adapted to LIVE (the differentiator)

matchering matches a finished track to a reference OFFLINE (spectrum, RMS, peak,
stereo width). Live adaptation:

1. Operator drops a reference file (or points at a recording of a show they like).
2. **Offline analysis pass** (seconds, off the audio worker): windowed spectrum →
   1/3-octave average curve; integrated LUFS; crest factor; stereo width.
3. The analysis becomes a **mastering preset**: EQ match curve (difference between our
   recent program average spectrum and the reference's, capped ±6dB/band, smoothed),
   loudness target, width hint.
4. The live chain applies that static preset — deterministic, CPU-trivial, no
   adaptation feedback loops on the hot path (session law: control-plane transients
   never puncture the data plane; the preset only changes when the operator applies it,
   with slewed parameter transitions).

"Make my show sound like X" — no competitor has it.

## 3. Where it runs (boundary shape)

- **Phase M1 — built-in master insert** (the fast path to value): a `MasteringChain`
  DSP module in the core (`native/src/modules/AudioMastering.h`, pure, stateful,
  unit-tested like every kernel), exposed as a fixed insert slot on the master bus with
  a WinUI editor panel (targets, ceiling, amount, preset picker, reference-load).
  No plugin host dependency; ships on the existing insert plumbing.
- **Phase M2 — packaged as a real VST3** (`CoreVideo Mastering.vst3`) via the plugin
  host work (audio-overhaul 4.5 / P2c): same DSP core compiled into a VST3 shell, so
  it's usable in DAWs and becomes a marketing asset. The in-app chain keeps running
  built-in (no out-of-process hop for our own trusted DSP).
- Analysis pass runs in the SHELL or a worker thread core-side — never on the audio
  worker (instrument-first law: the analysis emits its derived preset as DATA, applied
  like any parameter set).

## 4. Verification (the soak rig earns its keep)

- Unit: each stage pure-tested (existing kernel tests extended); loudness integrator
  convergence test (synthetic -10 LUFS input → target within N minutes, no overshoot
  oscillation); true-peak detector vs known inter-sample-peak vectors.
- Soak: fake-engine tones through the mastering chain → tone-scan PASS (no clicks from
  parameter rides) + measured integrated LUFS lands within ±0.5 LU of target — a
  MACHINE-verifiable acceptance for a mastering processor.
- Ear (owner): A/B toggle on real program material; the chain defaults must survive
  "turn it on and forget it" for a full show.

## 5. Sequencing

M1 (built-in chain + editor + soak verification) ~1-2 sessions; reference-analysis
preset generator +1; M2 VST3 packaging rides the plugin-host epic when it lands.
