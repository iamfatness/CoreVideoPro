# First-Party Mastering Chain — Master Bus Processor Spec

Status: owner-requested 2026-07-05 ("a Mastering VST built by us that will sit on the
Master Bus, Which should be the same as Left and Right Bus"). Written 2026-07-05
(overnight session). References studied per owner: **sergree/matchering** (GPL-3 —
reference-matching mastering; architecture only, clean reimplementation) and
**Wamphyre/oXygen** (chain topology reference). Same license discipline as
Tracktion/Ardour: read, learn, never copy.

## 0. Topology — RESOLVED (owner-confirmed 2026-07-06)

**Master and program L/R carry the identical signal.** The chain is applied ONCE on
the `master` bus and everything downstream inherits the processed signal: pgm-l,
pgm-r, stream and mon inherit the mastered master when they are not explicitly
routed (`MediaCore.cpp` `renderAudioOutputTick`, mastering block ~:4650 + the
inherit-when-unrouted copies that follow it). An explicitly routed STREAM/MON bus
is a deliberate matrix override and keeps its own mix. This is shipped code, not a
plan — the question this section used to ask is closed.

## 1. What it is

A broadcast mastering processor that sits on the Master Bus and makes the program sound
finished by default — the "sounds professional out of the box" feature that vMix users
buy third-party plugins for.

**SHIPPED chain** (`AudioMastering.h processMasteringChain`, status B1 2026-07-19):

```
input trim → HP/LP filters → 3-band tone (shelves + presence bell)
   → momentary-LUFS loudness ride → glue compressor (single-band | 3-band LR4)
   → M/S stereo width → TRUE-PEAK limiter (4x oversampled) → master
   → pgm-l/pgm-r/stream/mon inherit (§0)
```

- **Loudness ride**: steers momentary-LUFS-averaged loudness toward the target
  (-14 / -16 / -23 presets), asymmetric (~3s down / ~8s up), ±maxRideDb bound,
  200ms gain slew. The limiter catches what the ride can't.
- **Glue compressor**: program-relative threshold (target +6dB). B1 exposed the
  character controls — `glueRatio` / `glueAttackMs` / `glueReleaseMs` /
  `glueMakeupDb` (defaults 2:1, 30/250ms, 0dB = the old fixed values,
  bit-exact). Optional **3-band multiband mode** (`glueMultiband`):
  Linkwitz-Riley LR4 crossovers at ~200Hz/~3kHz (cascaded Butterworth biquads,
  low band phase-aligned through an AP2 at the upper crossover so the neutral
  band sum recombines flat), per-band compressors sharing the character
  controls plus ±6dB per-band trims. Single-band remains the DEFAULT until the
  owner's listening pass; multiband OFF is bit-identical to the single-band
  path. All crossover/compressor state is per-band per-channel persistent.
- **True-peak limiter**: SHIPPED (B1) — no longer just metered. 4x-oversampled
  polyphase windowed-sinc detector (the `computeTruePeakDbfs` kernel shape)
  drives the gain computer, so inter-sample overs are held below `ceilingDbfs`
  (-1.3 dBTP default). 16-sample detector lookahead (0.33ms @48k) delays the
  audio path by the same amount, landing the instant attack exactly on the
  peak; release-smoothed recovery; peak-hold envelope (~10ms); all state
  (gain, envelope, interpolator history, delay line) persists across 20ms
  ticks. Measured cost ≈ well under the 20ms worker budget (unit-test perf
  guard prints the number).
- **EQ / tone**: shelves at 120Hz/10kHz + 3kHz presence bell; flat by default;
  reference-matched curves remain the follow-up below.

Every stage is **bit-identical bypass at its neutral value** — the invariant every
B1 addition preserves (unit-test pinned).

### Rack workflow + persistence — SHIPPED (B2, 2026-07-19)

- **Everything persists** (`ProductionOutputPreferences` schema **v6**): the full
  mastering settings block (incl. the B1 glue dynamics/multiband params), BOTH A/B
  compare slots + which slot is active, the selected loudness target, and
  operator-saved named presets (`MasteringUserPresets`). Restore rides the
  `ApplyProductionOutputPreferences` backing-field pattern (the O1 vcam shape) and
  reaches the core on the initial full sync — no second wire. Pre-v6 files migrate
  with null mastering blocks = in-app defaults; a migration can never flip
  mastering on or invent a rack state.
- **User presets** save/rename/delete beside the 4 built-ins
  (`MasteringPresetLibrary`, pure + unit-tested; built-ins are code-defined in
  `MasteringPresetCatalog` and immutable — their names are reserved).
- **Post-mastering meters on the rack**: integrated LUFS with the -14/-16/-23
  target guide + true peak with the ceiling guide (`MasteringGuideMeter`). The
  core meters the routed master AFTER `processMasteringChain`
  (`updateProgramLoudnessMeter` runs on the post-chain master tap), published as
  `audioMixSession.masterMeter`. The TP detector is **streaming**
  (`streamingTruePeakBlockDbfs`, sinc history persists across 20ms chunks) — the
  finite-buffer `computeTruePeakDbfs` rings at block edges (~+0.4dB over-report
  on ISP content) and must not drive an operator-facing meter.
- **Rack reads as one chain**: stages 01 INPUT → 02 FILTER → 03 TONE → 04 RIDE →
  05 GLUE → 06 WIDTH → 07 CEILING in DSP order, per-stage bright/dim engage
  opacity mirroring the exact neutral-bypass conditions in the chain (the
  DspResponseCurve convention: dim = arithmetically a no-op). Stage reset ids
  follow the same names (old aliases still accepted).

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
