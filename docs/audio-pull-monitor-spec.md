# Pull-Model Audio Output — Architecture Spec

Status: **approved by owner 2026-07-05** ("take the new architecture and fix this and build
a scalable solution"). Supersedes the push-model monitor internals of
`WasapiMonitorOutputAdapter.cpp`. Informed by the 2026-07-05 click-hunt (see §2) and by the
architectural convergence of Tracktion Engine and Ardour (§3).

## 1. The bar this serves

Owner doctrine (`audio-overhaul-spec.md` §0): A/V sync paramount; clean sound is THE bar;
latency serves both. The monitor path must be artifact-free by construction, not by tuning.

## 2. Why the push model cannot be saved (evidence)

The current adapter pushes one ~20ms block per audio-worker tick at the worker's cadence and
then manages the endpoint's fill level. Every artifact class found in the 2026-07-05 hunt is
a symptom of that decision:

| Artifact (owner-heard) | Mechanism (instrument-proven) |
|---|---|
| Continuous grain | Endpoint underran EVERY tick (50/s) — zero standing slack, phase raced the device |
| Good-then-bad @ ~27min | One-shot 40ms cushion drained at ~25ppm clock drift, then edge-riding |
| Way-worse periodic holes | Servo v1 subtracted reserve per emission → zero-emission ticks |
| Clicks @ fixed intra-tick offset | Servo v1 hunted at ±8 frames/block; nearest-index resample = splice per correction |
| Warble/robotic | Servo v2 ±1-frame lerp = 0.1% time-warp flutter, invisible to click scanners |
| Loud-speech clicks | Limiter instant attack (unrelated to transport, fixed separately) |

Three servo generations failed because the **padding measurement itself phase-wobbles by up
to one device period**: any control loop keyed on it either hunts (audible) or must be so
slow it cannot also provide slack. The class is unfixable; the model is wrong.

## 3. The architecture (Tracktion/Ardour convergence)

**The device pulls; the engine fills.** Both reference engines execute inside the audio
device's callback. Our equivalent for the shared-mode WASAPI monitor:

```
audio worker (50Hz)                     render thread (device-paced)
  mixRoutedBuses → MON bus                 WaitForSingleObject(renderEvent)
  render(pcm) = PUSH into ─── SPSC ring ──→ PULL exactly what the device asks
  lock-free ring, never blocks             GetBuffer/ReleaseBuffer, format convert
                                           ring dry → silence + counter
```

- **SPSC lock-free ring** (single producer = audio worker; single consumer = render
  thread). Capacity ~340ms; target standing depth ~60ms (3 worker ticks).
- **Event-driven render thread**: `AUDCLNT_STREAMFLAGS_EVENTCALLBACK`; the device signals
  when it wants data; the thread fills ALL available space from the ring every wake.
  The device's own buffer (200ms) plus the ring absorb worker jitter; there is no padding
  to manage and no phase race — the consumer always keeps the endpoint full.
- **Clock drift**: still real (worker produces on its clock; device consumes on its own),
  but now measured as **slow ring-depth trend** — a phase-stable signal, unlike padding.
  A ~5s loop nudges `IAudioClockAdjustment::SetSampleRate` a few ppm (≤±500) so the
  WASAPI engine's high-quality resampler absorbs the offset. Sample data is never touched
  by our code (servo v2's lesson).
- **Overflow policy**: ring full → drop the incoming tail + rate-capped log (bounded
  latency beats blocking the worker; with the depth trim active this should never fire in
  steady state).
- **Underflow policy**: ring dry at pull → fill silence + count `ringDryFrames_`
  (telemetry replaces the padding-based underrun counter, same snapshot field).

## 4. Invariants (the week's laws, now structural)

1. Never per-block/hand-rolled resampling on a live path — the platform resampler does
   rate matching (servo v1/v2 lessons).
2. All DSP state persists across blocks (C7c/C7d/limiter lessons) — unchanged; upstream
   of this adapter.
3. Control-plane transients never puncture the data plane (hold-last guards) — unchanged.
4. No file I/O, locks shared with control plane, or unbounded work on the render thread
   or inside the worker's render() push (debug-tap Heisenberg lesson).
5. Every deliberate buffer sits OUTSIDE the synced program path (doctrine §0) — the ring
   is monitor-only; program/recording taps are upstream and unaffected.

## 5. Scalability (why this is the pattern, not a patch)

The same `SpscRingBuffer` + device-paced consumer shape serves: future ASIO/exclusive-mode
monitor outputs, multi-output monitoring (per-operator cue buses), and the virtual-camera /
NDI audio sends. The ring becomes a shared primitive (`native/src/modules/SpscRing.h`),
unit-tested pure (no device dependency).

## 6. Implementation phases

- **P1 (this change)**: `SpscRing.h` (pure, tested) + rewrite `WasapiMonitorOutputAdapter`
  internals: EVENTCALLBACK init, render thread, ring push/pull, ring-depth rate trim,
  delete primeCushion/padding servo/drop counters. `render()` keeps its signature
  (push-only, non-blocking). stop() joins the thread before teardown (engine-off deadlock
  rules apply: no joins under locks the thread needs — the adapter is called under
  audioOutputMutex_, the render thread takes NO app locks, join is safe).
- **P2**: per-block ramped control changes (gain/pan steps → ramps) in mixRoutedBuses —
  Ardour's every-change-is-a-ramp rule.
- **P3**: tone-soak + latency-pulse rig (spectral analysis in CI-adjacent tooling) so
  flutter-class artifacts are machine-caught; wire `ringDryFrames_`/depth into telemetry.

## 7. Verification

- Unit: SpscRing push/pull/wrap/full/dry properties; adapter format conversion pure parts.
- Rig: 30-minute soak — zero ring-dry events after warmup, ring depth flat (trim locked),
  loopback capture spectrally clean at quiet AND loud speech (both artifact classes).
- The existing 341-test native suite must stay green (the adapter is dev-gated; suite
  exercises DSP, not the device).
