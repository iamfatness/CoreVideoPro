# Stream backpressure: hold quality, shed frames, never storm the encoder — design

**Date:** 2026-09-23 (revised same day after reviewing OBS Studio's approach)
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

**Owner rulings, 2026-09-23:**
- When the destination cannot sustain the bitrate, **drop frame rate first and
  keep quality.**
- **Backpressure ships on its own slice.** The egress-based health signal and
  the phantom-fault fix (see "Out of scope") follow separately.
- After reviewing OBS Studio's implementation, fold its lessons into this design
  before planning. **Architecture only: OBS is GPL and none of its code may be
  copied into this product** (the standing rule in `EncoderPolicy.h`).

## What OBS teaches, and what we take

OBS Studio has solved this problem for a decade. Four lessons, and one thing
that does not transfer.

1. **Encoded frames ARE droppable, by priority.** OBS raises a minimum-priority
   threshold under congestion so lower-priority packets are discarded before
   entering the send queue: B-frames first, then P-frames, keyframes almost
   never. An earlier draft of this spec asserted compressed frames cannot be
   dropped. That was wrong, and it is corrected here.
2. **Measure buffered TIME, not queue occupancy.** OBS's thresholds are
   milliseconds of queued media. An earlier draft used percent-of-bound, which
   changes meaning with bitrate and resolution. Time is stable across settings
   and is the unit an operator understands.
3. **Throttling inflow does not recover latency already accumulated.** Reducing
   what arrives next stops the queue growing; it does not clear the ~1 second
   already sitting in it. OBS recovers that by discarding. **Two levers are
   needed, not one.**
4. **Never rebuild the encoder for a network problem.** OBS reconnects the
   network layer and leaves the encoder running, asking for a keyframe on
   reconnect. Our incident rebuilt the hardware encoder eight times for what was
   a destination condition.

**What does not transfer:** OBS owns its RTMP client and measures the actual
socket send queue. We hand bytes to FFmpeg, which hides the network behind its
own buffering, so our queue depth is a proxy one buffer removed from the truth
and will UNDERSTATE real end-to-end latency. Everything below is calibrated with
that understatement in mind, and it is the main argument for pulling the
egress-based health signal forward (see "Out of scope").

## What this slice delivers

A struggling stream sheds frame rate, recovers its latency, and stays up. No
encoder rebuild happens for backpressure at all.

## Design

### 1. The signal: age of the oldest queued chunk

`RtmpOutputSenderAdapter::enqueueBitstream` already bounds the queue at **60
chunks or 2 MiB**. At 1080p60 / 10 Mbps an average chunk is ~21 KB, so 60 chunks
≈ 1.25 MB: **the queue holds roughly one second of encoded video.** That second
is the latency budget this policy defends.

The measure is the **wall-clock age of the chunk at the head of the queue**:

```
bufferedMs = now - oldestQueuedChunkEnqueuedAt
```

Direct, needs no frame-rate assumption, and stays correct while the frame rate
is being changed underneath it — which is exactly when a frames-to-milliseconds
conversion would lie. Store one timestamp per queued chunk; publish `bufferedMs`
and `queuedChunks` as `std::atomic` written where the queue is already mutated
under `bitstreamQueueMutex_`, read relaxed from the submit path. No new lock, no
new ordering.

### 2. Lever A — the input divisor (holds quality)

`core/StreamBackpressurePolicy.h`: pure, header-only, no platform types, unit
tested without a GPU, deliberately the same shape as `core/MonitorShedPolicy.h`
(observation struct, transition enum, integer-only comparisons, divisor, enter
fast / recover slowly, hysteresis band). Anyone who has read the monitor policy
can read this one.

At divisor `d`, `submitFrameToGpuEncoder` skips all but every `d`-th program
frame. The encoder's thread is paced by the keyed mutex and only advances when a
new handle is published, so skipping submits genuinely reduces its output rate
rather than merely delaying it.

| divisor | encoder input | intended use |
|---|---|---|
| 1 | 60 fps | healthy |
| 2 | 30 fps | first step |
| 3 | 20 fps | second step |
| 4 | 15 fps | floor |

**Why this satisfies "keep quality".** Under CBR the encoder allocates
approximately `bitrate / declaredFrameRate` bits per frame. The declared frame
rate on the output media type does not change, so bits per frame stay constant
and per-frame quality holds; feeding half the frames emits approximately half
the data per wall-clock second. Frame rate is what gives way, which is the
ruling. **This is the design's load-bearing assumption and must be measured
first** (see Risks).

### 3. Lever B — backlog discard (recovers latency)

Lever A stops the queue growing. It does not clear what is already there: if the
destination drains at exactly the throttled rate, the stream stabilises a full
second behind and stays there. Lever B recovers that.

**Our encoder configuration limits us to two priority tiers, and this is a
consequence of our own earlier decision.** HEVC and AV1 run with B-frames
disabled (NVENC low-latency mode, the #565/#521 work), so there are no
non-reference frames to discard cheaply. The only safe discard is therefore a
**GOP tail**: drop every queued chunk from the head up to, but not including,
the next keyframe. Dropping an arbitrary P-frame would corrupt every frame after
it until the next keyframe; dropping the tail produces a clean skip instead.

`GpuEncodedChunk` already carries `keyframe`, so this is implementable with what
exists. If no keyframe is present in the queue, discard nothing and let Lever A
and the hard bound handle it — never discard blindly.

Lever B costs a visible skip, so it engages only at the higher threshold, after
Lever A has already had its chance.

### 4. Thresholds

Observations arrive once per program frame (~60/s). Every number is justified
against the ~1000 ms hard bound.

| constant | value | why |
|---|---|---|
| `kThrottleAboveBufferedMs` | 250 | a quarter of the budget: act early, while the response is still invisible |
| `kDiscardAboveBufferedMs` | 750 | three quarters: recover latency before the hard bound, and only after Lever A has failed to hold it |
| `kRecoverBelowBufferedMs` | 100 | the 100–250 ms band is the anti-flap hysteresis; throttling drains the queue, so recovering at the throttle threshold would oscillate |
| `kMaxDivisor` | 4 | 15 fps is the lowest frame rate worth putting on air |
| `kEnterAfterOverWaterTicks` | 30 (0.5 s) | a single keyframe spikes the queue; sustained growth is the signal |
| `kRecoverAfterHealthyTicks` | 600 (10 s) | network capacity changes far more slowly than render load, so recovery is 20x slower than entry |

One step at a time in both directions, exactly like the monitor policy.

### 5. Encoder and connection are separate failures

**Design goal, stated so the plan cannot lose it: a destination or network fault
restarts the connection and muxer; it must never rebuild the encoder.** The
encoder keeps running across a reconnect and is asked for a keyframe so the new
connection has an entry point (`CODECAPI_AVEncVideoForceKeyFrame`, the same
`ICodecAPI` surface the B-frame work already uses).

If the queue still breaches its hard bound at divisor 4 with discard engaged,
the destination cannot carry even 15 fps at this bitrate. The existing overflow
path then fires **once** and the sender fails to its supervisor — a genuine
terminal condition rather than the first step of a storm.

### 6. The restart floor

The storm's restarts were **2.5 to 3.5 seconds apart. The supervisor's own
backoff ladder starts at 5 seconds** (`OutputDestinationSupervisorPolicy.h`,
5→10→20→40→60 s, give up after 5), and `kHealthyRunMs` is 30 s so a healthy-run
budget reset cannot explain it either. Restarts closer together than the
ladder's first rung are proof the ladder was not being applied to this
destination.

This slice must establish why and enforce a hard minimum spacing between encoder
rebuilds for a destination, regardless of which path requests them. The likely
mechanism to check first is the supervisor's per-destination record and policy
being reconstructed (and therefore `reset()`) as the sender list is re-synced,
which would restart the ladder at rung one every time. Whatever the cause, the
acceptance criterion is behavioural: **no two encoder rebuilds for the same
destination closer together than the ladder's current rung.**

### 7. Observability

`sessionState().outputSenderSession.senders[].backpressure`, published unconditionally
(the multiviewer-node rule — a node that vanishes in the case worth detecting is
useless): `{divisor, level, bufferedMs, queuedChunks, enteredCount, shedFrames,
discardedChunks, discardEvents, lastReason, lastTransitionBufferedMs}`. One
`[stream-backpressure] enter|step-up|step-down|exit|discard divisor=N
buffered=Nms` line per state change, never per frame.

The operator-facing readout must say the stream is degraded and at what frame
rate. A stream quietly running at 15 fps is the same class of defect as a silent
codec downgrade.

### 8. Scope of application

Both levers apply to the GPU-direct path, which is where compressed frames
queue. The raw CPU path already drops stale frames by design and is unchanged.
The policy is per destination: one struggling destination must not throttle or
discard for a healthy sibling.

## Testing

- **Pure:** `StreamBackpressurePolicyTest` — the buffered-ms measure at both
  queue bounds, one step at a time up and down, the 100–250 ms band does not
  flap, entry needs 30 sustained ticks and a single spike does not trigger,
  recovery needs 600, the divisor never exceeds `kMaxDivisor`, discard engages
  only above 750 ms and only after throttling, and a sustained overload holds at
  the floor rather than oscillating.
- **Discard correctness (pure):** a queue with no keyframe discards nothing; a
  queue with a keyframe discards exactly the chunks before it and never the
  keyframe itself.
- **Sender-level, no GPU:** the divisor actually gates `submitFrameToGpuEncoder`
  (the #481 rule — test the whole decision, not the leaf; deleting the gate must
  fail a test).
- **Real GPU, the acceptance gate:** extend `scripts/validate-gpu-encode.mjs`
  with a deliberately slow sink (a rate-limited SRT reader). Assert, over several
  minutes: the stream never stops, the divisor steps down and later recovers,
  buffered latency returns below the recovery threshold rather than sitting at
  the bound, **zero encoder rebuilds**, Program holds 60 fps, and the shell's
  `perf.log` sample cadence stays within its normal 5–6 s band. That last
  assertion is the one that would have caught #597: it is measured against the
  sample COUNTER, not wall time alone, so a producer slowdown is distinguishable
  from a UI freeze.
- **Regression:** the existing `--codec h264` and `--codec hevc` gates must still
  pass unchanged against a healthy sink.

## Risks, named

- **The CBR bits-per-frame assumption (load-bearing, measure first).** If the
  MFT's rate control uses a wall-clock leaky bucket rather than per-frame
  allocation, it will inflate per-frame bits to chase the target and egress will
  NOT fall with the divisor, defeating Lever A. Run the encoder at divisor 2 and
  compare bytes emitted per wall-clock second against divisor 1. If egress does
  not roughly halve, the fallback is dynamic bitrate via
  `CODECAPI_AVEncCommonMeanBitRate`, which contradicts the "keep quality" ruling
  and goes back to the owner rather than being adopted silently. (OBS offers
  dynamic bitrate as an opt-in setting, which is the precedent if both are ever
  wanted.)
- **Our latency measure understates the truth.** FFmpeg buffers behind our queue,
  so `bufferedMs` is a lower bound on what the viewer experiences. Thresholds are
  set against a budget we can see, not the one that matters.
- **Keyframe cadence in wall-clock time** stretches with the divisor if the
  interval is configured in frames. A 2 s GOP at 60 fps becomes 8 s at divisor 4,
  which lengthens both a viewer's join time and the worst-case wait for a
  discard point. Check how the interval is configured and say what it does.
- **One rig, one destination.** Everything is calibrated from a single incident
  against YouTube on an RTX 4090. The thresholds are starting points; the gate is
  what re-tunes them.

## Out of scope (the second slice)

- **Egress-based health**, made more urgent by the OBS comparison: the supervisor
  trusts `framesSent`, which the comment at its own increment site already flags
  as proving local FFmpeg input acceptance, not destination receipt. Because
  FFmpeg hides the socket from us, this is the only way to see the real
  destination state. FFmpeg's `time=` and `speed=` are that evidence and we
  already capture them.
- **The phantom-fault hole.** At 21:07:50, 21:08:15 and 21:08:50 the supervisor
  logged `Destination never accepted output within 15s of starting` — each line
  twice, ~15 ms apart — while FFmpeg was demonstrably muxing at 1x, and no
  encoder restart followed. `everProduced_` only becomes true inside
  `if (o.observed)`, and `OutputDestinationSupervisor.cpp`'s admissibility guard
  `continue`s on a generation-stamp mismatch, leaving `observedThisCycle` false.
  A rejected observation and an absent one are different states and must stop
  being indistinguishable; `staleEventsRejected` exists but is published only in
  the snapshot, which the incident capture did not preserve.
- Adaptive bitrate as an operator setting, macOS/VideoToolbox, and the raw-path
  throughput work (its own sub-project).
