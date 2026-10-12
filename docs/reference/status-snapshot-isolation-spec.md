# Status snapshot isolation specification

Issue: [#870](https://github.com/iamfatness/CoreVideoPro/issues/870), under
[#517](https://github.com/iamfatness/CoreVideoPro/issues/517).
Implementation sequence: [repair plan](status-snapshot-isolation-plan.md).
Work order and status remain in [BACKLOG](../BACKLOG.md) and the issue.
This is a proposed contract, not an implementation or acceptance claim.

## Problem and evidence boundary

`JsonRpcServer` holds `coreMutex` while `MediaCore::applyCommands` constructs
`sessionState()`. That construction acquires `audioOutputMutex_` for encoder
state and reacquires it through health, profile, mixer and sender projections.
The audio worker holds that mutex across DSP/output work. A status reader can
therefore block the render worker while waiting for another media owner.
Response string serialization is already outside `coreMutex`; moving only
`stringify()` does not repair this dependency.

The October 12 installed `690fe1e` run recorded empty status syncs holding the
render lock for 35–43 ms alongside Program source-stage misses. The first failing
187.64-second observation had three underruns, two overflows and four reused/skipped
monitor identities. Render work maximum was 8.2923 ms; render lock-wait maximum
rose to 31.8028 ms. At 60 Hz, two frames provide approximately 33.333 ms of
buffering. Machine commit availability remained approximately 85 GB.

A separate private component probe measured 16.7337 ms acquiring the initial
audio-state lock and 18.8707 ms total snapshot construction. Its 61,211-frame
diagnostic process remained free of Program underruns/overflows; it initially
used Speaker+Slides and later the eight-source Tiles wall. It did not reproduce
the original 35–43 ms interval. The original audio-worker wall maximum of
48.8528 ms includes core-lock waits and does not establish DSP execution time.
The long interval's initiating operation remains an attribution gate.

The Program display ownership repair in [#868](https://github.com/iamfatness/CoreVideoPro/pull/868)
addresses a different failure. [#832](https://github.com/iamfatness/CoreVideoPro/issues/832)
tracks the separate Zoom thumbnail/runtime mutex. Neither is closed by this spec.
The earlier [WinUI snapshot-apply incident](operator-stutter-snapshot-apply.md)
had zero native delivery losses and expensive UI refresh. This incident has
measured native underruns; those two snapshot paths must not be conflated.

## Required behavior

Status polling, health UI refresh and support-bundle projection must not acquire
the audio DSP/output lock while holding the render lock. A slow audio owner,
snapshot builder, device query, encoder status read or response writer must not
hold up Program production through the status path.

Keep 1080p60 output, configured two/three-frame Program buffering, matching audio
delay, grading/ROI/overlays, source quality and enabled outputs. Do not hide the
failure through slower operator polling, reduced scopes, longer waits, additional
buffering, disabled monitors or silent substitution of stale frames.

## Ownership and publication

Extract focused native types rather than adding another subsystem to
`MediaCore.cpp`. Proposed seams are `AudioOutputObservation`,
`PublishedObservationStore`, `CoreStateCapture` and `SessionSnapshotAssembler`;
names may follow existing repository conventions.

| Owner | Publishes | Readers must not do |
|---|---|---|
| Audio worker | Mixer/channel meters, program loudness, monitor counters/status and audio-owned output facts | Run DSP, scan meter history, inspect a device or wait on `audioOutputMutex_` |
| Async encoder/sender owner | Recording/stream lifecycle, completion/progress counters and failures | Turn desired state into observed completion or issue blocking backend queries |
| Zoom, capture/discovery, browser and camera owners | Their cached roster/device/status facts with identities and freshness | Enumerate hardware, query a backend or acquire its work mutex under the render lock |
| Command/render owner | Applied intent/revisions, scene/roster references and already-produced render/delivery facts | Build full JSON, fetch another worker's live mutable state or copy pixels/PCM |
| Snapshot assembler | JSON projection of one captured core state and leased immutable owner observations | Read mutable `MediaCore` members off-lock or combine unlabelled generations |

Capture the audio observation while its owner already owns the relevant state,
then release the DSP/output mutex before publishing it. Collect immutable cached
encoder/sender observations through their existing ownership seams; audit each
accessor rather than assuming `session()` is nonblocking. Status publication may
coalesce; it must not introduce a queue on the media worker.

Publication replaces a small immutable reference. Allocation, formatting and
retirement of the old payload happen outside publication locks. A short pointer
lock or the platform's atomic shared-pointer implementation is allowed; this is
not a claim that shared-pointer atomics are lock-free. No publication lock spans
DSP, GPU work, device calls, file/network I/O or a reader's JSON projection.

Keep at most one pending assembly request and one executing assembly job. Reserve
fixed metadata capacity before copying: four slots per owner/store, covering
current, construction, assembler and dispatcher leases; excess consumers share
those immutable leases rather than allocate new versions. Proposed payload budgets
are 512 KiB per owner and 2 MiB per aggregate, excluding existing separately
bounded response transport buffers. Implement counters for bytes, busy slots,
coalescing and refusals. Exhaustion retains the previous observation, advances its
age and reports the refusal; it never waits on production. Required identities,
errors and applied revisions cannot be silently truncated. Oversize optional
detail is explicitly omitted and counted. Preserve existing supported source/bus
capacities; do not lower them to meet these budgets.

These stores contain metadata only: no pixels, PCM, GPU/read leases, encoded
payloads or thumbnail base64. Existing media/thumbnail event paths keep their own
ownership and bounds. Payload leases must not retain a backend or device object.
Preserve existing cold-start thumbnail fields through their separately bounded,
immutable event publication and off-lock response projection, not a new GPU
readback or raw-image lease in `CoreStateCapture`. Account encoded thumbnail
bytes separately in the existing response transport budget. Collect system-memory
samples outside the render lock and publish the sample time alongside their values.

## Core capture and RPC semantics

Split command mutation from expensive snapshot construction. Under `coreMutex`,
apply a command and capture its applied revision plus a bounded `CoreStateCapture`.
Capture immutable scene/roster references and small already-published facts;
move required array construction to its owner outside the lock. Release the lock
before assembly. The capture/lease portion must stay below 1 ms maximum in the
declared fault/load tests; any over-budget occurrence is retained as a failure.
Audit all RPC handlers, cold-start/handshake paths and direct health/profile reads,
not just empty `media-core-sync` requests.

A mutation response acknowledges the command actually applied. It must not wait
for a later audio tick, claim an asynchronous effect completed, or attach an older
applied-control revision that reverses the shell's settings. Asynchronous operations
remain accepted/starting until their owner reports completion or failure. Recording
must not be shown as recording before the existing observed writer-start boundary.

Attach worker observation metadata separately from applied intent. Every observation
carries process epoch, owner generation, monotonic sequence, publication time and
the applied configuration revision it actually observed. Acquire each owner payload
once per response and reuse that lease for all its projections. Reject publications
from a retired epoch/generation. A response is not a globally simultaneous snapshot:
its component generations/revisions and observation times disclose the boundaries.

At startup or after recovery, an owner without an observation is `unobserved`.
Do not return healthy zero counters. Commands may update desired fields immediately
while observed fields remain pending; older worker samples must not restore old
gain, routing, recording, streaming or monitor settings. Preserve existing schema
fields and add versioned observation metadata; native, managed and support-bundle
consumers must explicitly handle missing/new metadata.

## Freshness and failure reporting

Compute age at response serialization from the original monotonic publication
time, never the time the assembler copied the payload. Proposed defaults for
continuously running audio telemetry are fresh through 250 ms, aging from 250 ms
through 1 s, and stale above 1 s. These are reporting limits, not permission to
delay media. Existing destination-specific lifecycle/completion rules remain
authoritative; the observation envelope cannot relabel an already-failed output
healthy. An idle owner publishes an explicit idle observation rather than becoming
permanently stale. Shutdown publishes a final terminal observation and retires
the generation; a restart begins unobserved.

Stalled publication keeps last measurements visible with their age and reason.
Meters freeze with a visible stale indication instead of dropping to silence.
Observed recording/streaming health cannot remain confidently live indefinitely.
The shell must not silently stop/restart outputs merely because metadata is stale.
Output failure, upstream acquisition loss, stale diagnostics and Program delivery
failure remain distinguishable.

## Attribution and diagnostics

Use an opt-in, fixed-capacity timing collector for core capture, every snapshot
component, nested lock wait versus body, audio gather/lock/work/publish, assembly,
response serialization and queueing. Record monotonic interval endpoints, thread
role, epoch/generation/revision and command kind; omit credentials and raw media.
Do not infer a mutex owner from one slow wait or infer DSP/GPU duration from a
whole-worker wall timer. Preserve normal-logging matched trials alongside probes.

Expose publisher progress/age, pool bounds/refusals, dropped/coalesced observation
counts and snapshot core-lock duration in native and managed support evidence.
Instrumentation must not synchronously format/write logs under media locks.
Collection is bounded on disk and in memory and does not sample every frame into
an unbounded managed list.

## Verification and acceptance

Use barriers/latches, not sleep-based races, to stall an audio/status owner while
actual Program GPU pixels and an independent camera receiver continue. Cover
20/40/100 ms stalls, a sustained stalled owner, assembler/writer backpressure,
publication exhaustion, retirement/restart, concurrent routing/gain/output changes
and delayed old-generation publication. Test all supported synchronous/test paths
as well as live workers. Negative controls must show the baseline dependency and
detect held/gapped/reordered pixels; counters alone cannot pass the pixel gate.

Health tests must prove initial unobserved state, advancing age without publication,
aging/stale thresholds, recovery, terminal output errors and command acknowledgment
ordering. Specifically reject an older audio observation reverting a new setting,
an accepted recording start appearing as observed recording, and stale counters
being replaced with healthy zeros. Verify fixed memory/lease limits during slow
readers and fault tests; no media object may be retained by an observation store.

Installed comparison starts with the reported eight-Zoom-feed plus one live-capture
Tiles workload. Broaden to eight Zoom feeds plus two capture feeds, including the
existing 1080p/1440p capture reference, with declared requested/delivered source
resolutions and cadences. Program/Preview/multiview and webcam remain enabled;
exercise both buffer depths, Takes, resize, grade/ROI/framer, recording and configured
stream outputs. Production output remains 1080p60; source arrivals and negotiated
SDK limits are reported separately. Missing raw rights/feeds invalidate that workload.

Run matched baseline/candidate Release builds on the same hardware and normal
logging, then a 30–60 minute installed meeting run. Retain exact commits, installed
and loaded-camera identities, flags, hardware/driver, actions, all raw captures,
startup and every failure. Use a fixed disclosed startup interval for identity
measurement; never reset counters or retry away startup/output failures.

Acceptance requires zero Program underruns, scheduled delivery misses, output
identity gaps and unaccounted Program-cell repeats/skips/reordering in the declared
release workload, with independent webcam continuity and decoded A/V intact.
Render/GPU readiness, native canvas pixels, physical display and receiver pixels
are distinct boundaries; mark unavailable evidence unverified. Average FPS and
operator smoothness do not waive a missed frame. Any remaining startup/delivery
failure stays open under #517/#823.

The original 35–43 ms interval's expensive component must be attributed through
captured nested timing or a causal reproduction before #870 is closed. Removing
the confirmed lock dependency may be reviewed separately, but a clean probe is not
proof of that attribution or full release acceptance. Exact-head required CI must
pass; known failures such as #754/#869 remain visible rather than weakened.

## Rollout and rollback

Ship the store with its real status/RPC consumers in one focused #870 repair;
there is no unused foundation release. Qualify against #868's repaired Program
display handoff so its known pixel-starvation failure does not obscure this test.
Keep issue closure and beta release decisions in the issue, not this document.

An implementation-only comparison switch may support controlled baselines, but
must not silently fall back to locking live audio state if publication stalls.
Rollback uses the previous verified package/installer and its matching machine
camera registration. Do not replace individual installed binaries or deregister a
working camera as a diagnosis shortcut. Preserve the running session during document
work; installation/restart belongs to scheduled installed validation.
