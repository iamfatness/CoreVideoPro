# Stream backpressure: degrade the frame rate, never storm the encoder — design

**Date:** 2026-09-23
**Status:** approved design (owner, 2026-09-23), slice 1 of the #597 response
**Incident:** [#597](https://github.com/iamfatness/CoreVideoPro/issues/597)
**Related:** #524 (bitstream write isolation — the queue this slice reads), #521 (GPU-direct streaming), #569 (receiver acceptance)

## Why

On 2026-09-22 a 10 Mbps H.264 stream to YouTube on `beta-2026-09-22-58d4fab`
produced a ~20 second operator stall. The logs (preserved under
`artifacts/incident-20260922-h264-10mb-stall/`) show what happened, and it was
not what it looked like.

The stream ran healthily for 96 seconds: FFmpeg's own progress read
`time=00:01:36 bitrate=10.18Mbps speed=1x` at 60 fps. Then its speed slipped
just under 1x, the compressed-video queue hit its bound, and the sender was
failed to its supervisor. From 21:09:09 to 21:09:29 the core tore down and
rebuilt the hardware H.264 encoder **eight times** — 21:09:09, :12, :15, :18,
:20, :23, :27 — each rebuild emitting a fresh ~75 KB keyframe into a pipe that
was already backed up.

**The stall was the core starving, not the UI blocking.** `perf.log` (written by
the shell's `MediaCoreSupervisor`) carries a Zoom transport sample every ~5.6 s
all evening, with exactly one 21.01 s gap: 21:09:12.06 → 21:09:33.07, inside the
storm. Across that gap the sample COUNTER advanced normally (10860 → 10890, the
usual 30) and per-sample `dispatchQueue=0.0ms subscriber=0.1ms` on both sides.
The shell's UI thread was never blocked — the operator's Stop at 21:09:29.55 was
recorded while the gap was still open. The core's snapshot emission fell to
about 26% of normal while it did eight encoder rebuilds.

**The product gap.** CoreVideo Pro has no way to absorb a destination that
cannot sustain the configured bitrate. Its only response is teardown and
restart, and restart makes it worse, because a rebuild's first act is to push
the largest single object in the stream — a keyframe — into the backed-up pipe.

**Why it fails instead of degrading, which shapes the fix.** On the pre-#521 raw
path, video was STATE: `AsyncOutputSender` coalesces, newest wins, stale frames
are dropped and the stream keeps going. On the GPU-direct path the queue holds
COMPRESSED frames, and those cannot be dropped: discarding a P-frame corrupts
every frame until the next keyframe. So the queue can only fill and fail. The
throttle therefore has to act **before** the encoder, on its input, where frames
can still be skipped safely.

**Owner rulings, 2026-09-23:**
- When the destination cannot sustain the bitrate, **drop frame rate first and
  keep quality.**
- **Backpressure ships on its own slice.** The egress-based health signal and
  the phantom-fault fix (below, "Out of scope") follow separately.

## What this slice delivers

A struggling stream steps its frame rate down and stays up. No encoder rebuild
happens for backpressure at all.

## Design

### 1. The signal: queue utilisation, already measured

`RtmpOutputSenderAdapter::enqueueBitstream` already bounds the queue at **60
chunks or 2 MiB**, whichever binds first, and today a breach sets
`bitstreamWriterFailed_` and fails the sender. At 1080p60 / 10 Mbps an average
chunk is ~21 KB, so 60 chunks ≈ 1.25 MB: **the queue holds roughly one second of
encoded video.** That is the latency budget this policy defends.

Utilisation is the worse of the two bounds, as an integer percentage:

```
utilisationPercent = max(chunks * 100 / 60, queuedBytes * 100 / (2 << 20))
```

The queue depth must be readable from the submit path without taking
`bitstreamQueueMutex_` (the submit runs on the sender's sync path, the queue is
serviced by the writer thread). Publish `bitstreamQueuedChunks_` and
`bitstreamQueuedBytes_` as `std::atomic`, written where the queue is already
mutated under its lock, read relaxed by the policy. No new lock, no new
ordering.

### 2. The mechanism: an input divisor, applied before the encoder

`core/StreamBackpressurePolicy.h` — pure, header-only, no platform types, unit
tested without a GPU, deliberately the same shape as `core/MonitorShedPolicy.h`
(observation struct, transition enum, integer-only comparisons, divisor,
enter-fast / recover-slow with a hysteresis band). Anyone who has read the
monitor policy can read this one.

At divisor `d`, `submitFrameToGpuEncoder` skips all but every `d`-th program
frame. The encoder's own thread is paced by the keyed mutex and only advances
when a new handle is published, so skipping submits genuinely reduces the
encoder's output rate rather than merely delaying it.

| divisor | encoder input | intended use |
|---|---|---|
| 1 | 60 fps | healthy |
| 2 | 30 fps | first step |
| 3 | 20 fps | second step |
| 4 | 15 fps | floor |

**Why this satisfies "keep quality".** Under CBR the encoder allocates
approximately `bitrate / declaredFrameRate` bits to each frame. The declared
frame rate on the output media type does not change, so **bits per frame stay
constant and per-frame quality holds**; feeding half the frames therefore emits
approximately half the data per wall-clock second. Frame rate is what gives way,
which is the ruling.

**This is the design's load-bearing assumption and it must be measured, not
assumed** (see Risks).

### 3. Thresholds

Observations arrive once per program frame (~60/s).

| constant | value | why |
|---|---|---|
| `kMaxDivisor` | 4 | 15 fps is the lowest frame rate worth putting on air |
| `kShedAboveUtilisationPercent` | 50 | leave half the queue as headroom to absorb a keyframe burst before the hard bound |
| `kRecoveryHeadroomPercent` | 25 | the 25–50% band is the anti-flap hysteresis; shedding itself drains the queue, so recovering at the shed threshold would oscillate |
| `kEnterAfterOverWaterTicks` | 30 (0.5 s) | a single keyframe spikes depth; sustained growth is the signal |
| `kRecoverAfterHealthyTicks` | 600 (10 s) | network capacity changes far more slowly than render load, so recovery is 20x slower than entry (the monitor policy's ratio, scaled to this domain) |

One step at a time in both directions, exactly like the monitor policy.

### 4. What happens at the floor

If the queue still breaches its hard bound at divisor 4, the destination cannot
carry even 15 fps at this bitrate. The existing overflow path then fires **once**
and the sender fails to its supervisor as it does today. The difference is that
this is now a genuine terminal condition rather than the first step of a storm.

### 5. The restart floor

The storm's restarts were **2.5 to 3.5 seconds apart. The supervisor's own
backoff ladder starts at 5 seconds** (`OutputDestinationSupervisorPolicy.h`,
5→10→20→40→60 s, give up after 5). Restarts closer together than the ladder's
first rung are proof the ladder was not being applied to this destination.
`kHealthyRunMs` is 30 s, so a healthy-run budget reset cannot explain it either.

This slice must establish why, and enforce a hard minimum spacing between
encoder rebuilds for a destination regardless of which path requests them. The
likely mechanism to check first is the supervisor's per-destination record and
policy being reconstructed (and therefore `reset()`) as the sender list is
re-synced, which would restart the ladder at rung one every time. Whatever the
cause, the acceptance criterion is behavioural: **no two encoder rebuilds for
the same destination closer together than the ladder's current rung.**

### 6. Observability

`sessionState().outputSenders.senders[].backpressure`, published
unconditionally (the multiviewer-node rule — a node that vanishes in the case
worth detecting is useless): `{divisor, level, utilisationPercent,
queuedChunks, queuedBytes, enteredCount, shedFrames, lastReason,
lastTransitionUtilisationPercent}`. One `[stream-backpressure] enter|step-up|
step-down|exit divisor=N utilisation=P%` line per state change, never per frame.

The operator-facing readout must say the stream is degraded and at what frame
rate. A stream quietly running at 15 fps is the same class of defect as a silent
codec downgrade.

### 7. Scope of application

The throttle applies to the GPU-direct path, which is where compressed frames
queue. The raw CPU path already drops stale frames by design and is unchanged.
The policy is per destination: one struggling destination must not throttle a
healthy sibling.

## Testing

- **Pure:** `StreamBackpressurePolicyTest` — utilisation maths at both bounds,
  one step at a time up and down, the 25–50% hysteresis band does not flap, entry
  needs 30 sustained ticks and a single spike does not trigger, recovery needs
  600, the divisor never exceeds `kMaxDivisor`, and a sustained overload holds at
  the floor rather than oscillating.
- **Sender-level, no GPU:** the divisor actually gates `submitFrameToGpuEncoder`
  (the #481 rule — test the whole decision, not the leaf; deleting the gate must
  fail a test).
- **Real GPU, the acceptance gate:** extend `scripts/validate-gpu-encode.mjs`
  with a deliberately slow sink (a rate-limited SRT reader). Assert, over several
  minutes: the stream never stops, the divisor steps down and later recovers,
  **zero encoder rebuilds**, Program holds 60 fps, and the shell's `perf.log`
  sample cadence stays within its normal 5–6 s band. That last assertion is the
  one that would have caught #597: it is measured against the sample COUNTER, not
  wall time alone, so a producer slowdown is distinguishable from a UI freeze.
- **Regression:** the existing `--codec h264` and `--codec hevc` gates must still
  pass unchanged against a healthy sink.

## Risks, named

- **The CBR bits-per-frame assumption (load-bearing).** If the MFT's rate control
  uses a wall-clock leaky bucket rather than per-frame allocation, it will inflate
  per-frame bits to chase 10 Mbps and egress will NOT fall with the divisor,
  defeating the whole design. **Measure before building the rest:** run the
  encoder at divisor 2 and compare bytes emitted per wall-clock second against
  divisor 1. If egress does not roughly halve, the fallback is dynamic bitrate via
  `CODECAPI_AVEncCommonMeanBitRate`, which contradicts the owner's "keep quality"
  ruling and must go back to them rather than be adopted silently.
- **Keyframe cadence in wall-clock time** stretches with the divisor if the
  interval is configured in frames. A 2 s GOP at 60 fps becomes 8 s at divisor 4,
  which lengthens a viewer's join time. Check how the interval is configured and
  say what it does.
- **One rig, one destination.** Everything here is calibrated from a single
  incident against YouTube on an RTX 4090. The thresholds are starting points and
  the gate is what re-tunes them.

## Out of scope (the second slice)

- **Egress-based health.** The supervisor trusts `framesSent`, which the code
  comment at its own increment site already flags as proving local FFmpeg input
  acceptance, not destination receipt. FFmpeg's `time=` and `speed=` are the real
  evidence and we already capture them.
- **The phantom-fault hole.** At 21:07:50, 21:08:15 and 21:08:50 the supervisor
  logged `Destination never accepted output within 15s of starting` — each line
  twice, ~15 ms apart — while FFmpeg was demonstrably muxing at 1x, and no encoder
  restart followed. `everProduced_` only becomes true inside `if (o.observed)`,
  and `OutputDestinationSupervisor.cpp`'s admissibility guard `continue`s on a
  generation-stamp mismatch, leaving `observedThisCycle` false. A rejected
  observation and an absent one are different states and must stop being
  indistinguishable; `staleEventsRejected` exists but is published only in the
  snapshot, which the incident capture did not preserve.
- Adaptive bitrate, macOS/VideoToolbox, and the raw-path throughput work
  (its own sub-project).
