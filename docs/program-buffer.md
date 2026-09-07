# Program delivery buffer

The Windows D3D11 production path supports a startup-selected two- or three-frame
Program buffer. Three is the default. The saved preference is passed as
`COREVIDEO_PROGRAM_BUFFER_FRAMES` before launching the native child and remains
fixed across child recovery in the same app session. Changing it requires an app
restart; the Engine button only toggles Zoom capture.

At 60 Hz the added content delay is 33 1/3 or 50 ms. This is not total camera-to-
screen latency. Unsupported compositors report no active buffer and must not
apply the matching audio delay.

## Ownership and timing

Rendering, frame preparation, and scheduled delivery use separate D3D devices
and contexts. A bounded pool retains actual Program pixels; queuing handles to
the old overwritten render target would not buffer frames. Keyed mutexes guard
cross-device ownership. NV12 preparation carries the identity of its GPU frame.
The delivery packet owns its pixels, scene/overlay attribution, and scheduled
timestamp; output consumers must not attach an untagged latest NV12 sample.

The Program display export and the multiview Program cell have separate shared
textures. Preview and source monitoring remain live. A display consumer must not
block recording or network delivery. Display export contention and unconsumed
frames are recorded separately from queued output packets.

Early GPU readiness work uses a private delivery texture, leaving display
exports available to their readers. At the scheduled time the delivery worker
submits short copies to those exports. Submission that outlives its slot is
discarded before queuing the output packet; the timeline records the missed
slots instead of moving the anchor or delivering stale content later.

Commands publish immutable output configuration. The buffered video-output
worker does not acquire the render lock. RTMP, SRT, and NDI have independent
bounded workers; recording retains its separate encoder worker.

Program/master and stream PCM receive a sample-accurate delay of 1,600 or 2,400
sample frames at 48 kHz, crossing the existing 960-sample block boundaries.
Cue monitoring, metering, and ISO audio retain their original timing. Delayed
content keeps the continuous output audio clock; video uses its scheduled
delivery timestamp.

Recording retains the request time across settings updates and repeated start
commands. After its writer is ready, the first eligible scheduled Program frame
sets the shared mux epoch and starts at PTS zero. Startup audio is bounded and
trimmed to that same boundary; ISO tracks share it. Request, writer readiness,
and mux epoch timestamps remain separate diagnostics.
Program audio timestamps travel through the asynchronous writer queue instead
of being replaced with writer-thread arrival time. This prevents negative
opening video timestamps from discarding the first encoded keyframe.

File video is decoded ahead into a bounded queue. The render consumer selects
pixels using its scheduled content timestamp, without waiting for a decoder
worker to wake at the presentation deadline. Audio uses the same media epoch.
Recording proof reports real frames, synthetic preroll/tail frames, and audio
padding separately; padding never counts toward unique-frame acceptance.

## Acceptance

CPU render overruns are diagnostic when buffered delivery still succeeds.
Acceptance requires zero buffer underruns and zero missed scheduled output
deadlines, with GPU readiness and every enabled destination measured separately.
A repeated image is emergency continuity, not a unique delivered frame.

`session.programBuffer` and `[program-buffer]` logs report buffer state and failure
counters. `deadlineMisses` measures retained Program GPU readiness against the
scheduled delivery deadline. Display-export work and actual presentation are
separate observations; a ready GPU frame does not prove on-time scanout.
`generation` changes when a replacement buffer is created, invalidating a
measurement interval that spans the replacement. These are internal observations, not proof of physical display or
destination completion. The strict QA validator fails closed when that evidence
is absent. Preserve source telemetry and exact hardware, workload, enabled
destinations, process/session identity, and interval alongside test results.

Required validation covers both depths: initial prefill, retained pixel/metadata
identity, audio delay across block boundaries, short absorbed stalls, longer
underruns, recovery without accumulating latency, source/Take/key transitions,
slow destination isolation, resolution-generation ownership, and shutdown.
Live acceptance and independent latency/A-V measurements remain necessary after
unit tests and builds pass.

## Current validation boundary

The native suite covers retained GPU/NV12 pixel identity, resource replacement,
independent draining, and actual multiview pixel progress at two fixed consumer
phases. The internal smoke runner measures each depth in a separate native
process and rejects generation changes, missing evidence, underruns, overflows,
readiness misses, and output sequence gaps.

Neither test establishes physical presentation. The WinUI surface currently
counts `Present` submissions; it does not correlate native Program identity
with displayed-frame completion. Strict display acceptance requires that
correlation and presentation evidence. A successful internal smoke report must
continue to set `framePerformancePassed` to false.

## Validation snapshot — 2026-09-07

The offline Windows candidate passed 684 native tests, 856 WinUI tests, and 68
JavaScript evidence tests. The opt-in real 1080p60 H.264 source test was also run
explicitly: all eight flash onsets and widths matched the source cadence.

Finalized recordings at both depths decoded from PTS zero with matching native,
packet, and decoded-frame counts. Worst source-corrected A/V offsets were
13.44 ms (two frames) and 11.34 ms (three frames). Neither recording interval
increased buffer underruns, overflows, GPU readiness misses, or output sequence
gaps. The longest interior recorded frame interval was 16.667 ms. No synthetic
video padding was needed in these two files; audio padding remains explicit.
The same binary also passed 30-second internal smoke intervals at each depth:
1,800 and 1,801 delivered packets respectively, with zero failure-counter
increments (`smoke-06.json`).

These measurements used an RTX 4090 with an older app instance still running
on the user's PC. They are bounded recording/internal checks, not an isolated
GPU benchmark, a live-meeting soak, or physical display/network acceptance.
The artifact report is `recorded-av-1788757375777-19b9af0c/report.json` under the
ignored `artifacts/program-buffer-final` directory. Native SHA-256:
`A7A24BD7C46907201326B4795C31B75DAB48B61F8D20CD0A9018AC1E3AD7110C`.
Earlier failing reports are retained. Merge acceptance remains pending.
