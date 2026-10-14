# Real-time isolation: cross-subsystem audit and repair contract

Audit date: October 12, 2026. Source baseline: main
`cf0ac11b8917fb4ccaa3961e9e2fe2ace2be228f`. Parent delivery contract:
[#517](https://github.com/iamfatness/CoreVideoPro/issues/517). First repair consumer:
[#870](https://github.com/iamfatness/CoreVideoPro/issues/870), with its
[specification](status-snapshot-isolation-spec.md) and
[implementation plan](status-snapshot-isolation-plan.md).

This is an architectural dependency audit, not a ranked work queue or evidence
that every incident has one cause. Work order remains in [BACKLOG](../BACKLOG.md).
Measured findings below retain their own workloads and acceptance criteria.
Static dependencies identify possible interference; they do not prove which
operation initiated the original #870 stall. The pending #868 Program handoff
repair is separate from this main baseline.

## Audio monitor owner after the audit

The live path now wraps the monitor adapter in `AsyncAudioMonitorOutput` when
`MediaCore::enableAudioOutputWorker` runs. Commands submit endpoint intent without
taking the DSP work lock. Start, stop, PCM delivery and backend retirement run on
one device owner; status reads lease cached observations. The synchronous path
remains for direct single-owner callers. This supersedes the audio-monitor
dependency described in the historical baseline table below.

Each endpoint/rate/channel change or mute advances a generation and clears the
pending PCM queue. The owner revalidates that generation after a potentially
blocked stop, before opening the captured endpoint. A mute, retarget or shutdown
that arrived during stop therefore prevents the superseded endpoint from opening.
A start already in flight cannot be cancelled, but its late completion cannot
become the readiness or endpoint observation for newer intent.

PCM admission holds at most four pending packets plus one in flight, with at most
4096 sample-frames and eight channels per packet. Surviving packets keep order;
queued packets older than 80 ms are discarded and counted. Accepted-frame counts
advance only after backend acceptance, not queue admission. Wrapper destruction
waits up to 200 ms; an uninterruptible driver keeps its own shared worker/backend
state until it returns rather than delaying core shutdown indefinitely.

`AsyncAudioMonitorOutput` regressions gate activation, PCM rendering and stop
independently. They cover retarget/mute during blocked stop, superseded readiness,
bounded packet pressure and owner retirement during blocked stop. The MediaCore
test also renders Program and polls status while activation is held. These
deterministic dependency tests do not prove GPU/webcam deadlines or physical
device audio continuity; installed device-switch qualification remains separate.

## Where the pattern recurs

### Resource handoff after the audit

The optional resource pump remains the formatting/mailbox owner. Its core-side
`ResourceEventStaging` now admits at most 64 Program, participant, Multiview and
Preview announcements, with a conservative 2 MiB allocation/serialization bound.
The fixed event schemas reserve 8 KiB for root/texture nodes and keys, 4 KiB per
Multiview tile and eight bytes per variable string character (including worst
JSON escaping). These bounds are computed from source facts before constructing
JSON. An exhausted lane never constructs the rejected payload, evicts an
accepted announcement or releases a retained payload under `coreMutex`.

A drain moves at most 64 owned immutable JSON leases to the pump. Admission and
accepted FIFO order are preserved across kinds. Final payload retirement is
outside the core lock. A blocked pump can retain one additional drained batch,
so pre-pump ownership is at most two 64-event/2 MiB batches, in addition to the
separate bounded serialized mailbox. A refused Program/Multiview/Preview
announcement does not advance its emitted identity/signature; the next render
retries the current structure. Participant facts retry each tick. Counters at
`realtimeEvidence.resourceEventStaging` distinguish admission from refusal and
report pending items and the resident bound; neither counter proves receiver
delivery. Resource descriptors do not introduce GPU release/ownership barriers.
The serialized mailbox retains its existing logical wire-byte accounting;
allocator/string capacity overhead is not a measured resident-memory guarantee.

Grade drain captures the existing maximum sixteen JSON leases and matching
`appliedRevision` scalars under the lock. Adding that field and copying object
maps happens after unlock, using the captured revision even if a newer grade is
applied meanwhile. The synchronous drain remains for isolated direct callers.
Its owner queue also refuses before JSON construction at sixteen events or a
512 KiB conservative fixed-schema byte bound, without retiring accepted roots.
`gradePreview.eventsRefused` and `eventOwnedByteBound` report this pressure.
Active editor ticks retry the current surface on the next 100 ms tick after
space returns. A one-shot unsupported/capacity response returns a failed control
ACK when its event cannot be admitted; repeating that control can retry after
drain. This avoids acknowledging a diagnostic event that was silently lost.
The RPC classifies that valid-request pressure as `grade-preview-overloaded`;
malformed identity/revision remains `protocol-error`. Nested commands record
the corresponding overload failure rather than mislabelling it invalid input.

Serialized mailbox capacity wait belongs only to the pump. On terminal stop,
saturated admission is cancelled rather than waiting for a blocked pipe writer;
the summary log records `resource terminal-cancelled` and `oversized` separately.
The writer's stop signal follows producer joins and final bounded drains, so a
writable mailbox can finish accepted metadata in order. An unavailable writer
can lose metadata through explicitly counted terminal cancellation. The existing
uninterruptible `std::ostream` write/join limitation is not repaired here; this
change adds no new shutdown wait on mailbox capacity after stop. All mailbox
condition-variable notifications wake both writer and capacity waiters.
`[resource-pump]` shutdown summaries attribute formatting and mailbox acquisition/
capacity wait separately (count, total and maximum microseconds). Core capture
retains `rpc.resource-drain` lock-guardrail timing; the existing writer reports
queue sojourn and blocking flush separately. These timings do not establish GPU
execution cost or receiver presentation.

Deterministic tests stall formatting for 20/40/100/500 ms while the MediaCore
stub keeps producing Program, saturate staging, verify accepted order and current
structure retry, check payload leases survive drain, and cancel saturated
mailbox admission on stop. These establish dependency and ownership behavior,
not physical GPU/webcam deadlines, installed continuity or a zero-miss result.

| Boundary | Evidence and mechanism | Repair boundary / tracking |
|---|---|---|
| Status to audio/output | **Measured interference, source-confirmed nesting.** RPC holds `coreMutex`; `sessionState()` acquires `audioOutputMutex_`. Installed snapshots held the core lock 35–43 ms while Program lost frames. The exact expensive initiating operation is still unproven. | Owner-published immutable observations; bounded core capture and off-lock assembly. [#870](https://github.com/iamfatness/CoreVideoPro/issues/870). |
| Commands to device lifecycle | **Source-confirmed exposure, not a measured incident.** `MediaCore::syncAudioMonitor` holds both locks while monitor start/stop performs WASAPI endpoint enumeration, activation and initialization. Recording/sender commands also acquire the audio lock, although their backend start/control methods already enqueue asynchronously. | Audio owner applies monitor intent; observed readiness and revision are published separately. Audit every command's nested lock, rather than moving just its JSON response. [#872](https://github.com/iamfatness/CoreVideoPro/issues/872). |
| Render to event formatting | **Source-confirmed exposure.** `JsonRpcServer::run` stringifies shared-texture/grade events and enters the frame mailbox under `coreMutex`. Pipe writes are already separate; they are not the remaining crossing. | Drain an owned bounded batch, then format/enqueue/retire outside the render lock. Preserve resource lifecycle barriers. [#873](https://github.com/iamfatness/CoreVideoPro/issues/873), attribution [#823](https://github.com/iamfatness/CoreVideoPro/issues/823). |
| Zoom frames to optional thumbnails | **Measured mutex occupancy.** #832 recorded roughly 12–13 ms publication bodies, including 6–7 ms thumbnail encoding, with Program fetch waiting on the runtime mutex. `ZoomEngineRuntime::publishVideoFrameLocked` constructs/base64-encodes optional events under that mutex. | Prepare owned events off-lock; commit pointers only after generation validation. [#832](https://github.com/iamfatness/CoreVideoPro/issues/832). |
| Program to display/export work | **Measured delivery failures, attribution incomplete.** #804 retained expired Program exports around 16.6–19.4 ms. Wall time does not prove GPU execution time. Display resource ownership and export scheduling are distinct from status locking. | Independent ownership and optional export scheduling; trace queue wait, GPU completion, copying and publication separately. [#804](https://github.com/iamfatness/CoreVideoPro/issues/804), [#868](https://github.com/iamfatness/CoreVideoPro/pull/868), [#823](https://github.com/iamfatness/CoreVideoPro/issues/823). |
| Recording/streaming pressure | **Existing isolation plus qualification gaps.** Async encoder/sender workers and per-track recording workers exist. Historical #814 disk-pressure losses cannot be assumed to describe every current queue. #607 separately measured codec/rate-dependent latency hidden by chunk/byte caps. | Verify per-writer isolation with slow/failing sinks; bound latency by age, memory by bytes, and preserve audio chronology. [#814](https://github.com/iamfatness/CoreVideoPro/issues/814), [#607](https://github.com/iamfatness/CoreVideoPro/issues/607). |
| Cold Take / media admission | **Measured ~100 ms stalls, exact initiating call unresolved.** #724 retained 4–6 missing slots near cold Take/start. Current `MediaTransports` already opens decoders on owner workers and reaps finished workers separately; do not claim decoder open still runs inline. | Trace preparation, first upload, admission and retirement; make readiness explicit before live admission. [#724](https://github.com/iamfatness/CoreVideoPro/issues/724). |
| Operator UI / grading | **Separate presentation weakness.** The earlier WinUI apply incident had zero native losses but structural refresh up to 197 ms; #509 tracks it. `GradePreviewController::tick` already submits previews and reads completed results rather than calculating all scopes inline. These facts do not qualify every worker/mailbox boundary. | Coalesce optional UI updates, preserve editing state, bound preview work and test UI responsiveness separately from Program. [#509](https://github.com/iamfatness/CoreVideoPro/issues/509), [#835](https://github.com/iamfatness/CoreVideoPro/issues/835), [UI evidence](operator-stutter-snapshot-apply.md). |
| SDK callbacks / recovery | **Different confirmed mechanism.** #790's symbolized crash showed an SDK callback reentering a held share mutex. #856's later helper disappearance has no established same cause. | No external SDK calls under callback/state locks; generation-fenced lifecycle transitions and independent exit evidence. [#790](https://github.com/iamfatness/CoreVideoPro/issues/790), [#856](https://github.com/iamfatness/CoreVideoPro/issues/856). |

## Shared isolation contract

**Keep the dependency graph acyclic at the media deadline.** Render may consume
prepared media and immutable owner observations. It must not wait for DSP,
device opening, disk/network writes, SDK callbacks, JSON formatting, UI refresh
or optional scopes. Moving an operation onto a worker is insufficient if a
status read or command subsequently acquires that worker's work lock under
`coreMutex`. Audit both directions and destructor paths.

**Separate intent from completion.** Commands validate and capture bounded intent
under the core lock, then the responsible owner performs lifecycle work. ACKs
identify the applied configuration revision; observed readiness/failure carries
owner generation, sequence and time. Obsolete start/stop/retarget results cannot
restore old state after a newer command or restart. Recording must still be
recording by the time the UI reports it; an enqueued start is not that boundary.
Keep cancellation and shutdown bounded, without detaching unbounded new workers.

**Bound every handoff by its semantics.** Preview/status work can coalesce to
latest; structural resource handoffs and command barriers require ordered,
explicit admission and failure semantics. PCM must retain compatible chronology;
do not apply video's latest-wins policy to audio. Define item, byte and age bounds,
in-flight ownership, exhaustion, restart and old-generation retirement. Move
expensive construction and final payload destruction outside shared locks.
Owner observation metadata uses #870's explicit capacity/freshness contract;
media queues require their own measured bounds, not that metadata budget.

**Reuse working owners.** AsyncEncoderSink and AsyncOutputSender already enqueue
backend work. AsyncVirtualCameraPublisher swaps owned frames and retires old
payloads after its state lock is released. RPC's base64 frame pump already
formats drained events outside the core lock. Media transport workers and grade
preview submission provide additional existing seams. Extend these patterns
through their remaining consumers; do not introduce a generic replacement engine
or ship unused infrastructure.

**Make stale observations honest.** A frozen observer cannot claim healthy media
from old values or turn missing telemetry into zeros. Health/support reports must
separate production, each delivery boundary, SDK process state and UI freshness.
[#766](https://github.com/iamfatness/CoreVideoPro/issues/766) retains broader health
acceptance; this audit does not claim all its historical field defects remain
unchanged on the current head. No aggregate FPS counter proves unique pixels or
continuous PCM at a receiver.

## Guardrails that detect the actual weakness

`LockHoldGuardrail::kCommandHandleBudgetUs` currently allows 50 ms of core-lock
hold. A two-frame 60 Hz Program buffer contains about 33.3 ms, so the measured
35–43 ms snapshots can harm delivery without exceeding that guardrail. Three
frames provide 50 ms with no remaining scheduling margin. A sanctioned warning
budget is therefore not an acceptable render-isolation contract.

The repair separates command mutation/capture from asynchronous work and makes
the proposed core capture target less than 1 ms under declared fault/load tests.
Record acquisition wait, held work, nested owner wait, event assembly, payload
retirement and queue age independently. Render work has a separate whole-frame
deadline; no claim that every render tick should finish within the command
capture budget. Deterministic CI tests establish forbidden dependencies and
ordering; reference-machine performance tests measure timing. Do not turn noisy
wall-clock CI thresholds into the only protection, or merely raise warning limits.

## Integrated verification and delivery

Each focused repair ships with its first production consumer and tests, linked
to its existing issue. #870's observation/capture consumer establishes the first
shared contract. #872 and #873 specify additional concrete crossings; their
ranking belongs to the owner. Shared types can grow only with a real subsequent
consumer. This document does not replace issue-specific acceptance or require
one large rewrite before any useful repair lands.

Fault tests delay one owner at a time for 20/40/100 ms and a sustained interval:
audio work/device activation, thumbnail packaging, JSON assembly/mailbox/pipe,
disk writer, network destination, scope worker and SDK lifecycle callback.
Assert that unrelated Program production/delivery continues, including lock
waits, pixel identity and PCM chronology. Then combine faults with polling,
roster changes, Takes, resize, grade/ROI/framer edits and record/stream/webcam.
Test exhaustion, old generations, shutdown and delayed completion, not just
healthy steady state. Retain a negative control that exposes the old dependency.

Installed qualification starts with the reported eight live Zoom feeds plus one
capture, Tiles, 1080p60, two-frame Program depth and independent webcam receiver.
Expand to the reference two-capture workload, both supported buffer depths and
enabled production outputs, then a 30–60 minute real-meeting run. Record actual
source cadences/resolutions, binary identity, startup and sustained results, and
per-boundary unique pixel/decoded audio evidence. UI smoothness is a separate
observation. Any miss fails that workload; missing evidence stays unverified.
No quality reduction, larger Program buffers, disabled monitoring or slower
polling may substitute for isolation. These are proposed gates, not completed
validation from this documentation audit.
