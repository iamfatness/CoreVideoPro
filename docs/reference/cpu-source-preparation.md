# CPU source preparation

`CpuSourcePreparation` owns bounded D3D11 preparation for immutable CPU I420
and BGRA arrivals. `I420SourcePreparation` remains a compatibility alias.
Normal Windows D3D11 module composition creates one shared
owner by default for Zoom, native UVC, WGC CPU fallback, media playback,
stills, browser, NDI, SRT and RTMP input. `COREVIDEO_CPU_SOURCE_PREPARATION=0` explicitly selects the
legacy CPU path for comparisons; `1` enables preparation on direct adapter
fixtures too. Unsupported platforms do not publish GPU views. Direct factory
fixtures retain their legacy default unless production preparation is requested.
Production preparation also defaults optional source exports, Preview and
multiview to their isolated monitor owner. Explicit monitor overrides remain
honored; forcing inline with preparation enabled reports unavailable source
exports instead of silently uploading on Program.

Media decoding calls `CpuVideoArrival` on its decoder thread before pushing
the existing CPU presentation queue. Still decoding calls it on the cache
worker, once per source alias and decoded image. Both keep the original CPU
buffer and playback timestamp. A held poll retains its arrival observation;
rewind, new pixels with a repeated identity, size or format changes fence the
old generation. Aliases may share CPU pixels but have distinct GPU identities.

Program selects an arrival token and resolves only its exact completed view.
It performs no upload, resource creation or GPU query for these sources.
The resource owner creates and imports three slots; the GPU owner uploads and
checks the event query before publishing an immutable attributed wrapper.
An upload may prepare one future arrival ahead of CPU selection. Preparing
unbounded future arrivals can evict a frame before CPU playout selects it.

Limits remain 64 registered sources, 16 active generations, 28 pending tokens
per source and 512 MiB shared production storage. A source may keep one charged
retiring generation. A third generation is refused until that lease releases.
CPU and ISO references do not pin GPU slots; submitted GPU reads do.
Valid offers refused by source, pending-token or retiring-generation limits
return an identity-correct stopped token with an explicit capacity reason. The
token owns no queue entry, pool or CPU pixels. Producer held-frame refresh can
retry it without changing the original descriptor. Active-generation and
quarantine limits report their reason on the waiting demand, and clear it when
resource construction becomes admissible. A shared residency-budget refusal
releases partial allocation and reports a stopped retryable token rather than
a terminal device failure. Program surfaces these reasons in admission evidence
and its existing degraded-source warnings; it does not allocate on refusal.
BGRA row stride and straight alpha are preserved. I420 range/matrix metadata
stays on the original CPU descriptor and is applied by the consumer shader.

Idle GPU retirement does not invalidate the CPU frame. Still and decoder
workers refresh stopped tokens for exact held descriptors without advancing
playout or changing observation time. Failed GPU owners publish attributable
failed tokens for subsequent valid arrivals; they cannot silently resurrect
an old prepared image. The GPU owner watches production consumer registration.
A changed production consumer set fences prepared tokens; producer arrivals
and held-frame refresh create new immutable views on the resource owner.
Old GPU reads remain charged until they finish, with one retiring generation
per source. Monitor-only registration does not fence production preparation.
Same-adapter consumer recreation has real BGRA/I420 pixel tests; preparation
device loss and adapter migration remain separate qualification.
After owner failure, a changed production consumer set requests one setup
attempt. Monitor-only registration and repeated CPU arrivals do not request
rebuilds. Existing pools must first complete their writes and release retained
GPU read leases on their old context owner; resource builds must also finish.
Storage stays charged until that drain completes. Only then does the GPU owner
release its old device/context and create the replacement. Failed tokens are
stopped so producer-held-frame refresh can replace them after setup succeeds.
Initial setup has no GPU resources to drain. An irrecoverable pending query
remains stopped/charged and follows existing shutdown quarantine; it is not
freed to force recovery. An injected device-status failure with real textures
and retained reads exercises drain/rebuild; physical device reset and adapter
migration remain unqualified.

WGC CPU fallback transfers OS frames to the bounded capture worker and offers
BGRA before publishing its descriptor. The descriptor snapshots its logical
epoch and calibrated WGC observation time with those exact pixels. Geometry
changes fence the generation on the capture owner. Its idle callback refreshes
stopped held tokens without new CPU bytes or a new observation time. CPU demand
preserves pending capture order; capacity refusals remain reported as drops.
Direct GPU capture and its separate CPU/ISO branch retain their own payloads.
Windows WGC now defaults to direct GPU capture; explicit
`COREVIDEO_GPU_CAPTURE=0` retains the CPU comparison/rollback path. Its capture
owner records exact GPU completion before publishing a source descriptor.
Unsupported sharing retains the existing CPU fallback and measured counters.
The scheduling-only experiment did not remove exact webcam-selection holds.
Buffered Program instead reserves one existing pool credit for an immutable
source/Take choice, demands those exact preparation tokens, and composes that
choice on the next production tick. Composition and preparation retain the
original production slot and deadline. With a two-frame buffer this divides the
existing budget between source preparation and composed-frame readiness; it does
not add a third frame, expand the pool, or retain CPU/ISO or optional-monitor
pixels. Expired or cancelled reservations cannot publish into reused credits.
The normal prepared-source path enables this behavior; explicit
`COREVIDEO_PREPARE_PROGRAM_WITHIN_BUDGET=0` retains the old immediate-composition
control for diagnostic comparisons. Preparation that misses the composition
portion still reports a held/unavailable source and fails exact-source
qualification. It is not relabeled as ready or given a later deadline.

The #1016 causal fixture delays a selected GPU image until after the original
CPU choice. The old path delivers older pixels; the corrected path delivers the
selected pixels at the same original deadline and capacity. This fixture is not
full-workload acceptance. Installed source pixels, source cadence, recording,
OS camera and physical display remain separate measured gates.

The installed #1016 diagnostic exposed a separate selection defect (#1019):
sampling UVC's latest arrival at variable ingest times repeated and omitted
already prepared pictures. `CapturePresentationWindow` retains at most three
original UVC arrivals without pinning prepared GPU slots. For negotiated 60 or
60000/1001 output, the reserved Program choice names the source-frame sequence
from its native cadence, including the required rational-rate repeat.
Fractional-rate conversion samples that clock with floor, placing the repeat
at the beginning of the conversion cycle. Rounding forward can request the
next capture near the cycle boundary, spending the preparation margin on a
future observation. The installed 59.94 Hz camera comparison exposed this:
15 selected images completed 1–3 ms after composition had already held the
preceding image. The original output deadline and 8.333 ms composition reserve
remain unchanged; the phase-sensitive fixture includes late capture arrival
and GPU preparation rather than only counting rational-rate repeats. Installed
picture and full-workload qualification remain separate requirements.
The capture owner can publish that arrival during the existing preparation interval;
composition resolves only the requested identity in that bounded history.
Resolution preserves capture observation, epoch and pixels. It never copies the
latest image into a missing identity. Missing/evicted/replaced choices mark that
layer unavailable and reset the cadence anchor for recovery. Program still
composes the other layers at its original deadline; a source failure cannot
cancel the whole output slot. The source-picture gate still fails that interval.
A new production anchor starts a new choice
timeline. Other negotiated capture rates retain the previous selection path.

The capture owner holds these CPU references; the pending Program job holds an
owner/choice descriptor and no CPU or optional-monitor pixels. The existing
preparation pool, Program pool capacity and two-frame output deadline do not
grow. ISO keeps its independent ordered arrival consumer. The clocked-source
fixture checks actual NV12 content at the unchanged deadline. This is not a
claim about long-run device-clock drift, other native capture rates, monitor
source cadence, or the installed full-workload result.

Direct-GPU WGC frames use `ReadyCapturePublication`, a capture-owner publication
of one already completed image. The Program reservation freezes source identity,
epoch and geometry, but selects this image immediately before composition;
there is no CPU upload to begin a tick earlier. Its pending descriptor does not
retain the earlier GPU surface. Publication owns no additional pool or queue,
and optional monitors discard the production publication handle. Resolution
cannot cross source replacement, dimensions or shutdown, and records the actual
selected image in the requested/drawn trace. This addresses the measured stale
screen choices without increasing the existing Program deadline or capacity.

The overall redesign workload judge now also requires actual camera and screen
picture proofs in both recorded Program and the registered camera receiver,
over its exact measured clock interval. Output-counter continuity cannot waive
source repeats, omissions, stale screen content or frozen ordinary camera
pixels. Native-rate conversion allowance is derived from the declared rate.

Browser input uses one bounded reader thread with at most 64 sources when
preparation is enabled. OS mapping, seqlock BGRA copying and preparation run
outside the adapter's metadata lock. Program polls immutable descriptors.
Each actual host spawn receives a unique SHM name and reader generation;
retained old readers cannot collide with a restarted host or publish into a
removed/replaced source. Reader observations are stamped on exact copied
pixels; the browser SHM ABI does not carry host acquisition time. Held tokens
refresh on the reader. Original CPU/ISO bytes and legacy preparation-off
polling remain available. SHM BGRA uses its existing separate owner.

NDI offers the original decoded BGRA buffer on its receive worker before
publishing the descriptor. Its timed receive loop refreshes stopped held tokens.
SRT and RTMP offer each original decoded BGRA buffer on the decoder reader;
each decoder spawn starts a new preparation epoch while CPU frame counts remain
cumulative. Program polls descriptors only. Audio decoding and original CPU/ISO
buffers are preserved. When preparation is enabled, one metadata worker per
network adapter refreshes stopped held descriptors independently of the blocking
decoder read. It snapshots at most 64 channels per 50 ms pass, rotating larger
configured sets. Offers occur outside adapter/channel locks; commit checks the
same CPU buffer, epoch, frame ID, observation time and old token so a fresh decode
wins. Refresh changes only the prepared GPU token: it does not copy pixels,
advance decode/audio counters, renew observation time or mark a stale feed live.
Shutdown wakes and joins that worker before retiring decoder channels. The
preparation-off path starts no refresh worker. Real local SRT/RTMP publishers
exercise stopped-input consumer recreation against an independent CPU pixel
reference; this does not qualify physical device reset or output continuity.
Transport acquisition cadence and codec conversion are independent
of byte-exact preparation of the decoded CPU pixels.
Installed output continuity, receiver/display identities,
latency, A/V and hardware qualification remain governed by #802/#517 and the
[render delivery specification](render-delivery-spec.md).

The pending-token queue charges only live weak entries. Retaining one old CPU descriptor does not keep expired entries behind it charged against the 28-token limit: a bounded admission scan removes expired entries anywhere in the queue while preserving live tokens and the configured cap. This changes producer admission only; it does not allocate GPU resources or supersede the retained old descriptor.
