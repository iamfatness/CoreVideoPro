# CoreVideo Pro render isolation and delivery specification

Implementation specification, October 3, 2026. Parent: [#517](https://github.com/iamfatness/CoreVideoPro/issues/517). The owner authorized starting after the show while remaining in the meeting. Work ranking is recorded in [BACKLOG](../BACKLOG.md). This specification supersedes the architectural alternatives in the incident plan retained in the owner's local workspace. Development and isolated tests do not require replacing the running installation.

Revision October 6, 2026: installed baseline `77bac0c5c8ddc7b05723b70c946749926e890b38`. This revision adds current incident evidence and defines how to qualify and enable the existing isolation implementation. The [completion plan](render-delivery-completion-plan.md) describes execution; BACKLOG remains the only ranked queue.

## October 6 evidence and limits

The live run had monitor isolation disabled. Source export remained inside
Program rendering, including sources outside its single visible layer. A
sampled call took 31.367 ms, of which 29.857 ms was source export, against a
16.667 ms frame period. A fresh 30.084-second interval contained 22 Program
buffer underruns, 144 render deadline misses and 953 monitor shed ticks.
No additional camera publication replacements occurred; this does not prove
loss-free upstream production or camera receiver playback.

A matched experiment used the exact installed core binary (SHA256
`a0f1e91d9d3041f01975cc6de72a69dbea7bee8bc3916e961f58764771f7f5f0`),
eight synthetic Zoom inputs requested at 1080p30, two BGRA captures at 1080p60
and 2560x1440p60, 1080p60 Program/Preview/multiview, a two-frame buffer and local
Program recording. Approximately 20 seconds per path produced:

| Observation | Inline monitors | Isolated monitors |
|---|---:|---:|
| Buffer underruns | 2 | 0 |
| Render deadline misses | 68 | 33 |
| Monitor shed ticks | 1,201 | 0 |
| Mean Program-thread work | 9.727 ms | 6.029 ms |
| Audio lost samples | 0 | 0 |

The isolated worker completed 1,209 requests without failure or pending
replacement. Seven focused native regressions passed, including Program
composing while the monitor worker was blocked, actual Preview/multiview
pixels across resize, and delivered camera NV12 identity. Zoom-only controls
had no buffer underruns on either path: source count alone did not reproduce
the incident. [Evidence and exclusions](https://github.com/iamfatness/CoreVideoPro/issues/517#issuecomment-6026674449) remain on #517.

This establishes a serialized monitor/source-export bottleneck and supports
isolation as the repair direction. It does not attribute every underrun or
individual driver/preemption wait, or qualify real SDK/WGC inputs, presentation,
camera receivers, decoded A/V or long runs. The 33 remaining render misses
remain visible even when the buffer absorbs them.

## Launch behavior and implementation seams

The existing `COREVIDEO_ISOLATE_MONITORS` switch is sampled at compositor
creation. Explicit `1` selects isolation; explicit `0` selects the legacy path
for matched QA or rollback. Unset retains the shipping default until installed
gates pass. A default change preserves these explicit overrides. No automatic
live switch occurs during a show. Isolation and GPU capture ingress retain
separate gates; passing one does not silently enable the other.

Publish requested/effective mode, selection source (default or override),
worker readiness, fallback reason and delivery epoch through existing generated
observations and the support bundle. Older peers report unknown. Initialization
refusal produces a named degraded state, never successful isolation. Any
legacy fallback explicitly reports that optional work again shares Program's
deadline; it preserves format and buffer settings.

| Seam | Required final behavior |
|---|---|
| `D3D11CompositorAdapter` | No optional per-source conversion/copy/export in Program. Preserve decoupled shell publication. Monitor initialization, resize, joins and destruction stay off Program. |
| `MonitorRenderWorker` and admission | One active job and at most one pending latest job. Replacement drops monitor work only. Immutable identities and ready images cross the boundary; unused/missing monitor inputs cannot retain Program leases or refuse unrelated valid work. |
| Consumer demand | Preview/multiview use monitor-local textures. Individual exports require actual fallback/inspector/popout demand. Hidden/destroyed consumers release their demand without removing Program/ISO demand. A configured Preview composite retires its legacy individual-source fallback. |
| Capture and CPU fallback | Prepare BGRA/I420 fallback views outside Program. Program consumes ready images; no uploads solely for monitors. Preserve qualified GPU ingress and the independent CPU/ISO branch; unsupported paths have explicit reasons. |
| Identity and presentation | Carry completed image sequence, source epoch, Take/layout revision and capture identity through publication, shell acquire and presentation submission. Program tiles show actual buffered delivery. Never stamp old pixels with the latest render-loop sequence. |
| Health | Report monitor degradation separately from production failure. Actual monitor completion cadence, optional drops, Program underruns and camera reader/receiver evidence are separate facts. A loop-frame label cannot prove Preview FPS. |

Use existing compositor/worker/source-bus seams. Missing observation fields go
through `contracts/observation.schema.json` and generated readers, with their
first real UI/bundle consumer. No new ownership in `StudioViewModel`, per-frame
file I/O, quality reduction or additional configured buffering belongs here.

## Outcome and scope

Deliver continuous 1920x1080 Program at 60 fps, continuous virtual-camera delivery to a qualified receiver, and smooth 60 fps multiview on the reference rig with the October 3 workload. Source cameras may have lower native cadence; their held frames must not be misclassified as a production failure. Preserve source resolution, color, audio synchronization and the existing two-frame Program buffer. Do not buy continuity by adding steady-state buffering or reducing quality.

The implementation must remove per-source monitor conversion/export from Program's render call, eliminate the normal Display capture GPU-to-CPU-to-GPU round trip on compatible hardware, and make delivery measurable through the virtual-camera reader. Monitor work must have independent scheduling and resource ownership. Work ranking remains in the repository BACKLOG; this document defines behavior and engineering dependencies, not another Now/Next queue. Implementation and deployment begin after the show, separately from this specification request.

Do not refactor unrelated production state, replace the Zoom SDK transport, change the encoder architecture, implement new ingest products, or retire CPU fallback. A distinct virtual-camera failure discovered through tracing gets a focused repair and its own acceptance evidence; it is not automatically attributed to #517.

## October 3 baseline evidence (historical)

Evidence is pinned to installed commit `75c38387578e19a9b01533f7a95ae1e03d2f049d`, not current main. Implementation must reconcile subsequent changes before editing. The saved read-only snapshots and logs are in `preserved-local-evidence/show-20261003-render/` in the owner's workspace, outside this checkout; they are not redistributed with the repository.

| Finding | Strength and consequence |
|---|---|
| Monitor-shed transitions at 10:24:04 and 10:24:31 reduce monitor cadence to divisor 2 | Confirmed by both transition logs and approximately 60 multiview calls per two seconds. Explains a period of 30 fps monitoring. |
| Source exports consumed 8.181 ms of a 9.004 ms slow compositor call at 10:33:45 | Measured CPU wall time. This work runs inside Program rendering and includes sources outside Program. |
| Display upload repeatedly takes 4–6 ms at 2560x1440 | Measured copy-plus-Unmap duration; it does not separate memory copy, driver work or thread preemption. |
| WGC reads GPU pixels into CPU BGRA and the compositor uploads them again | Confirmed in exact-build source. A full frame is 14,745,600 bytes. Unchanged-frame caching already exists. |
| The operator later saw virtual-camera hits | Real reported symptom, but its first failing pipeline boundary has not been established. |
| Existing snapshots explicitly do not verify destination completion or display presentation | Published-frame totals and average fps cannot close the output incident. |

The existing `D3DDecoupledExport` already separates shell-facing keyed-mutex publication onto an export device/thread. The remaining problem includes conversion, source upload and submission still performed on Program's context. This design must preserve that consumer isolation, not recreate the old shell-to-Program dependency. A zero CPU timeout on `AcquireSync` is not proof of zero GPU-side wait.

## Mandatory invariants

1. Only the Program worker submits commands to the Program immediate context. Monitor, capture, telemetry, UI and virtual-camera consumers never submit to it.
2. Program does not wait for monitor completion, a UI keyed mutex, an external consumer, resource creation, worker joins, file logging or a texture resize. It never uploads pixels solely for a monitor/inspector consumer.
3. Queues and GPU residency are bounded. A stalled monitor loses its own updates; it cannot retain capture slots needed by Program or create an unbounded backlog.
4. No source slot is reused while any admitted GPU read is in flight. CPU reference release alone is insufficient. Reuse requires completion evidence for every admitted copy.
5. Source epoch, frame identity and capture time survive every handoff. Program sequence advances only for a newly composed frame; monitor and camera metadata report the frame actually copied, not the current render-loop counter.
6. Production audio and ISO timelines remain independent of monitor cadence. New GPU payloads must not silently remove CPU pixels from an ISO or other existing consumer.
7. A device/format failure produces measured status and an explicit fallback reason. Unknown receiver delivery remains unknown.

## Architecture and ownership

| Component | Owns | Contract |
|---|---|---|
| Capture adapter | Capture device/context and capture callback | Copies a WGC surface into an owned bounded pool before releasing the frame-pool surface. No CPU readback on the supported same-adapter GPU path. |
| Source GPU ingress worker | Transfer device/context, import cache and readiness queries | Makes completed immutable source images available to Program. Texture opening, allocation and CPU fallback upload happen here. |
| Program worker | Program device/context, selected source views and Program buffer | Samples ready images, composes Program, hands off bounded production output. No participant-monitor export loop. |
| Monitor worker | Separate same-adapter device/context, source cache and monitor render targets | Renders Preview, the complete multiview and demanded source inspectors from immutable source/scene snapshots. Publishes through consumer-isolated exports. |
| Resource lifecycle worker | Pool construction, retirement and device rebuild orchestration | Performs allocation, handle opening, worker retirement and joins away from Program. |
| Camera publisher and reader | Existing production transport and Frame Server boundary | Carry/report exact Program identity; reader pacing remains independently measured. |

The workers share neither immediate contexts nor mutable scene objects. Publish scene intent as revisioned immutable plans under the existing core-state authority. Commands and Take remain authoritative in the core; moving monitor work does not create a second scene controller. Worker completion posts facts through the existing bounded event mechanism.

Program gives the monitor a bounded copy of an already composed Program image for the Program tile. That handoff is optional for the monitor and never awaited. The monitor must not recompose Program from a newer scene plan and label it as the delivered frame. Preview uses its own plan revision. Tally and tile labels carry the matching layout revision to avoid presenting old pixels under new source labels during a Take.

Use the existing `D3DProgramBuffer`, `D3DDecoupledExport`, source bus and typed command/snapshot seams where their contracts fit. Do not introduce a parallel media pipeline in WinUI. Shells continue presenting GPU surfaces and intent, not composing real-time media.

## GPU frame contract

Extend native source frames with an optional platform-neutral GPU payload interface. Windows handles and COM types belong in the Windows adapter implementation, not public portable headers. The existing BGRA/I420 alternatives remain supported. Frame validity checks, arbitration, compositor input, capture events, ISO and stub round trips must all understand the new alternative.

| Field | Meaning |
|---|---|
| sourceId and sourceEpoch | Canonical source identity and generation; reconnect, resize and device replacement start a new epoch. |
| frameId and captureQpc | Monotonic source frame identity within epoch and host monotonic capture timestamp. |
| width, height, format, color metadata | Actual pixel dimensions, format, matrix/range and applicable transfer function. No implicit color reinterpretation. |
| adapterIdentity | Adapter LUID on Windows, hidden behind the payload interface. |
| resourceGeneration and slotId | Select an already imported resource in a prebuilt pool. |
| readiness and lease | Nonblocking readiness observation plus lifetime tracking that includes GPU completion. |

Hand off frame metadata and leases, not serializable COM pointers. The payload must be recognized as real content so the core does not substitute a slate. Adapters advertise the GPU path only after successful resource construction and first completed frame. A null GPU payload is not a successful GPU frame.

Use D3D11 completion queries on the owning worker to establish readiness before publication. Poll with `DONOTFLUSH`; command submission/flush belongs to that worker, never the Program reader. No spin-until-ready or blocking GPU readback on Program. A lease is not published as ready until the producer copy is complete. Resource setup opens the textures on the Program device before admission; Program issues only reads of immutable ready pixels and reports read completion before retirement/reuse. Submit its read-completion query on Program's owning context, poll it nonblockingly on later ticks, and send only completion facts to lifecycle management. Never poll or operate another worker's immediate context from the lifecycle worker. Validate this ordering with an independent consumer device; do not rely on comments or successful CPU API return values.

## Buffering and admission

Each source ingress generation has three slots. Slot transitions are `Free -> Writing -> Ready -> Reading -> Retiring -> Free`. `Ready` can be superseded by a newer ready frame only when no reader owns it. A slot in Writing or Reading is never overwritten. Retiring waits for GPU completion on the lifecycle/ingress worker, never on Program.

Program takes the newest completed admissible image and retains its last valid private/view lease when no new image is ready. Count holds with a reason: source has no new frame, ingress copy pending, no free slot, source disconnected or device failure. For a source naturally below 60 fps, repeating its image while composing new Program frames is expected.

Monitor delivery uses a separate three-slot pool and a single replaceable pending job per source. Monitor workers may not hold Program ingress slots while waiting for UI consumption. The ingress worker makes a monitor-private copy only when monitor capacity is available; a slow branch is refused and counted. Conversion and per-source shell exports happen on the monitor context. One monitor worker serves the scene, rather than introducing an unbounded worker per source.

Initial new-allocation limits are 512 MiB for source ingress pools and 256 MiB for monitor pools/caches, counting retiring generations and staging allocations. These are proposed implementation limits, not measurements of current usage. Account existing Program/encoder pools separately and report total adapter budget pressure. Allocate by active consumer demand, evict unused monitor resources, and deny new optional monitor residency before taking capacity from production. Do not silently downscale. A denied production source uses the explicit CPU fallback if it fits; otherwise report capacity unavailable. Never silently claim all sources are GPU-backed.

Queue depth is a capacity limit, not intentional latency: consume newest ready content, not a three-frame FIFO. Existing Program-buffer depth remains two. Monitor supersession cannot discard Program, recording or ISO frames.

## Consumer demand and fallback

Introduce a typed source-consumer demand table with source ID/epoch, consumer kind, consumer instance and required representation. Program routes, Preview routes, multiview tiles, inspector/popout surfaces and ISO register explicitly. Destroyed/hidden UI consumers release their instance registrations; core process/session teardown clears the epoch. Snapshot the registrations immutably for workers. Missing demand does not unsubscribe a production source that another registered consumer still needs.

The composited multiview uses monitor-local source textures, not a per-participant shell export for every source. Create individual shell exports only for actual inspector/popout consumers. Cache unchanged frame-and-grade pairs; scene-grade changes still invalidate correctly. Source resolution changes must not synchronously join or recreate exporter devices on Program.

Same-adapter WGC uses the GPU payload. Unsupported sharing, adapter mismatch, incompatible format, resource failure or downstream CPU-only consumer needs select a named fallback. Preserve the current pixel quality and color behavior. CPU readback/upload for fallback is performed on workers using bounded latest-frame storage; Program sees only completed images. CPU-only ISO requests are serviced as a separate bounded conversion branch with their own loss counters and established recording semantics; never borrow monitor-drop semantics for recording.

Rollout gates are internal configuration options sampled at launch for GPU capture ingress and monitor isolation. They default off until hardware qualification and are recorded in the build evidence. No live automatic toggling during a show. A runtime source failure may fall back within its defined adapter contract and must record the reason and epoch transition.

## Resize and failure behavior

Resize starts construction of a new generation off-thread; keep the previous valid image until the first new image is complete. Atomically publish new dimensions, handles and epoch together. Bound retiring generations to one per source; coalesce further resize requests to the newest size instead of accumulating pools. If retirement cannot complete, deny/retry allocation and expose the fault. Never free textures whose GPU work has not completed.

Capture disconnect follows the configured hold/slate policy and marks input liveness separately from frame availability. A held picture is not live input. Monitor device loss restarts only the monitor path and reports stale/unavailable status; it must not restart Program. Program-device loss follows the existing production recovery policy and starts a new delivery epoch. Pool generations and counters must prevent stale completions from being applied to a replacement device.

Worker stop is requested asynchronously. Joins and resource destruction happen off the render/UI thread. Failure to stop within two seconds raises a diagnostic and leaves resources owned until completion; there is no unsafe forced destruction. Normal process shutdown may wait through the established shutdown coordinator. Test repeated start/stop and device loss for leaks and deadlocks.

## Delivery tracing and honest status

Add a versioned `deliveryEvidence` snapshot section, covered by generated contracts and native/C#/Swift fixtures. Names below define semantics; align spelling with the repository schema during implementation. Fields absent on older peers are unknown, never zero or healthy by default.

The common record is `(sessionEpoch, sourceEpoch where applicable, programSequence, sourceFrameId where applicable, stage, qpcTimestamp, reason)`. Program sequences identify compositions; source identities identify their ingredients. Trace Take/layout revisions where they affect image attribution. Wall-clock timestamps are for correlation with an operator report; all durations use QPC and the recorded frequency on this host.

Stages distinguish source arrival, source GPU ready, Program scheduled/start/complete, buffer delivery, camera publish accepted/complete, camera reader request/read/sample emission, monitor compose complete and shell presentation submission. An emitted Media Foundation sample is not proof of receiver display; only the receiver harness can establish that last boundary.

Preserve the shipping virtual-camera pixel transport ABI. Put optional tracing in a separate versioned diagnostics channel with the same least-privilege cross-session access pattern; do not widen permissions. Reader records are keyed by reader instance and request sequence. The diagnostics channel uses its own seqlock and carries the producer epoch, pixel-payload seqlock value, frameNumber and completion time. Read diagnostics before and after the pixel read; accept correlation only if both diagnostics identities agree and match the stable pixel identity. Restart/mapping replacement invalidates cached associations. A wrap or race that cannot establish identity is explicitly uncorrelated, not guessed from a repeated frame number. Old DLLs remain functional and explicitly report reader tracing unavailable. Readers never acknowledge through a synchronization primitive that can block publication.

For each boundary report observed/enabled, last identity, last progress age, accepted/completed counts, refusal/drop/repeat counts by reason, queue occupancy/capacity, and interval/age histograms. Program frame-number metadata must advance only with the corresponding submitted image. Do not infer monitor fps from a label stamped with the current Program loop counter.

The proposed production trace is a preallocated 16 MiB circular buffer of fixed-size events with no strings or file I/O on media threads, an aggregate snapshot at most once a second, and a bounded background export on explicit diagnostic capture. Overflow increments a counter and drops trace events rather than media. Timing instrumentation should add less than 1% p95 render-work time in matched on/off tests; failure of that target blocks enabling it by default. Include malformed/partial capture detection so missing events cannot produce a false PASS.

Do not record meeting secrets, stream URLs, media contents or participant names in the trace. Stable per-session source IDs and frame numbers suffice. The receiver test pattern is an explicitly selected synthetic moving frame counter, never an overlay injected into a live show.

## Timing and overload policy

Keep the shipping monitor-shed thresholds during the baseline and initial repairs. Once monitors run independently, the monitor controller measures its own wall/GPU completion and queue pressure. It must not call aggregate Program-plus-monitor cost a Program overload. A 60 fps monitor is the reference target; temporary monitor cadence reduction is a reported degraded state and fails the healthy-workload acceptance below.

For the reference workload, target Program CPU render work at p99 <= 8 ms and p99.9 <= 12.5 ms. Report scheduler lateness, lock wait and GPU completion separately. These are engineering acceptance targets, not claims about current performance. No texture creation, per-source monitor conversion, shell waits or worker join is permitted in that budget. Shared GPU contention can remain despite separate devices; matched stress testing must establish actual output continuity. Isolation alone is not a performance proof.

## Implementation boundaries

| Slice | Deliverable and dependency | Required proof |
|---|---|---|
| Delivery evidence | Add identity-correct stage counters, reader diagnostics and receiver harness to existing consumers | Detect injected duplicate, gap, stale epoch and trace loss; uninstrumented peers remain unknown. |
| Consumer demand | Replace unconditional per-source shell exports with explicit demand and preserve all existing consumers | Hidden/closed monitors stop exports; Program, Preview, inspector and ISO behavior remain correct. |
| GPU capture ingress | Add native GPU payload plus WGC same-adapter worker pools and bounded fallback | Real pixels and color from an independent device; no CPU round trip on normal path; correct resize/disconnect. |
| Monitor isolation | Move source conversion/export, Preview and multiview to the independent worker using the preceding contracts | Stalled UI and injected monitor delay cannot stall Program; correct Program-tile identity and Take labels. |
| Camera repair if required | Focused change at the first proven divergent downstream boundary | Receiver trace demonstrates the failing boundary and corrected delivery under identical load. |
| Installed qualification | One packaged candidate containing qualified slices | Meets the end-to-end gates and retains rollback. |

Each slice must include its first real consumer and regression tests; do not merge unused foundation types. These dependencies do not change BACKLOG rank. Create scoped child issues under #517 when scheduling code work; only a proven distinct downstream failure merits a separate incident claim. Current evidence and this revision are tracked on #517.

## Tests and acceptance

Unit/contract coverage must exercise slot ownership and retirement, concurrent completion, demand registration, epochs, capacity refusal, unchanged-frame/grade invalidation, trace wrap and mixed-version peers. Deterministic fault injection must delay the monitor by 25 ms, stall a UI consumer, exhaust its slots, and repeat resize/device loss without creating a wait on Program. The expected monitor degradation is acceptable only in these fault tests; production loss is not.

Windows hardware tests must prove GPU completion and actual pixel contents, not just nonzero handles or frame counters. Cover same-adapter WGC, CPU fallback, the available adapter-mismatch path, BGRA/I420/color-range parity, 1080p/1440p inputs, inspector/popout demand and ISO recording. A hardware path not exercised is marked MISSING_EVIDENCE and cannot be advertised as qualified.

The baseline reproduces the incident workload on `75c3838` with recording/streaming off and the actual receiver. A/B trials disconnect Display, restore it outside Program, put it on Program, and vary monitor visibility one factor at a time. Existing evidence is sufficient to start the render repair; these trials validate attribution and benefit, not defer the work.

Use a synthetic 1080p60 input with a unique visible counter for receiver continuity, alongside the real mixed-source workload. Warm-up lasts at most 30 seconds, is reported separately and may not restart to hide a failure. The negotiated receiver rate must be verified as 60/1; run 60000/1001 as a separately scored mode if supported. Compare identity sequences using rational cadence mapping, not an assumption that every callback arrives exactly 16.667 ms apart.

| Gate | Passing requirement after warm-up |
|---|---|
| Program schedule and buffer | Zero new skipped production slots, buffer underruns or output sequence gaps in the controlled reference test; all new deadline misses investigated. |
| Virtual-camera synthetic continuity | Every expected frame identity delivered once at the qualified 60/1 receiver, without tears or reordering; all observed gaps/repeats attributed to a specific boundary. An unexplained receiver hit fails. |
| Receiver timing | Report full arrival-interval distribution and maximum; no unexplained interarrival gap above 33.4 ms. This supplements, not replaces, the identity gate. |
| Multiview | Verified fresh monitor frames at the configured 60 fps cadence, no shedding in the reference workload and no unexplained stale Program tile. Display/vsync mismatch is measured separately. |
| Latency and audio | No added configured buffering; median content latency regression <= 5 ms and p95 regression <= one 60 fps frame against the matched baseline. No lost audio samples; paired synthetic flash/beep absolute skew <= 50 ms and first-versus-last five-minute median skew change <= 5 ms. Measure measurement uncertainty and fail an inconclusive result rather than silently accepting it. These are proposed gates for this change, not a new disposition of the separate #750 recording-offset issue. |
| Resources | No unbounded queue, retiring generation, memory, handle or thread growth; counters stay inside declared limits. |
| Rehearsal | 30-minute controlled soak then 90-minute installed full-workload rehearsal, including Takes, guest churn, capture changes and recovery; operator confirms receiver and monitor smoothness. |

A receiver that independently drops samples cannot certify the camera. Validate the harness with a known-good reference source and do not weaken gates to accept an unexplained loss. The trace must identify whether Program missed, publication failed, the reader repeated, or the receiver dropped. Repeat the real-show workload with normal logging after profiling is disabled. Average fps and a green native suite never substitute for these gates.

Run a separate combined record/stream/virtual-camera regression drill because source representation and output ownership are shared. Run repository-required production-native and shell gates. Qualify one lower-tier reference machine before broad hardware claims; preserve an explicit supported-workload envelope if it differs from the RTX 4090 rig.

## Rollout and definition of done

Build one candidate with commit, flags, adapter/driver, source formats, receiver version, test results and evidence hashes recorded. Keep the original installer and configuration backup. Do not install during the show. Roll back on delivery, audio, device-compatibility or latency regression; do not compensate by silently lowering quality.

#517 is not closed merely because monitor work moved threads. Closure for this scope requires the declared workload to pass on an installed candidate, end-to-end frame evidence, no consumer regressions and operator acceptance. The separate webcam symptom remains open if receiver tracing does not explain it. Any acceptance exception must be explicit in the issue and release evidence rather than buried in a passing aggregate.

## Sources

- [Exact-build WGC CPU readback](https://github.com/iamfatness/CoreVideoPro/blob/75c3838/native/src/modules/WgcScreenCaptureAdapter.cpp#L267)
- [Exact-build source export](https://github.com/iamfatness/CoreVideoPro/blob/75c3838/native/src/modules/D3D11CompositorAdapter.cpp#L1839)
- [Existing export isolation](https://github.com/iamfatness/CoreVideoPro/blob/75c3838/native/src/modules/D3DDecoupledExport.h)
- [Shedding policy](https://github.com/iamfatness/CoreVideoPro/blob/75c3838/native/src/core/MonitorShedPolicy.h#L98)
- [Existing render-budget issue and earlier measurements](https://github.com/iamfatness/CoreVideoPro/issues/517)

## Windows buffered optional publication ownership (#804)

The native delivery packet and optional shell/multiview snapshots have distinct
publication boundaries. Preparation holds input key 1, converts the exact image
to owned NV12 when requested, and copies it into a per-slot immutable BGRA
snapshot. It releases the input to key 0 and verifies the preparation GPU event
before marking the slot Ready. Scheduled delivery makes no D3D calls; it advances
the native sequence/PTS and queues the NV12 packet. Its sharedTexture is empty.

Shell and multiview each own a separate device/context, completion query and
stable keyed output texture. Each accepts at most one source read lease, including
running work; an occupied or failed branch refuses subsequent offers instead of
queuing them. A free branch cannot be held behind the other reader. The original
slot pool remains depth + 3 (five slots for the two-frame setting); slots with
outstanding readers cannot return to the producer. The additional immutable BGRA
snapshot costs width * height * 4 bytes per slot, approximately 39.6 MiB at 1080p
with two-frame buffering, excluding existing input/output/readback allocations.
There is no per-frame texture creation or growing handle cache.

Each export worker verifies its actual GPU copy event before dropping its source
lease. Metadata retains that same frame number, delivery sequence, generation
and render-plan evidence, and advances before key 1 exposes the completed copy.
latestDeliveredProgramFrame peeks the completed shell snapshot; the multiview PGM
cell separately peeks the completed multiview snapshot. Native output consumers
continue using takeDeliveredProgramFrame. A delayed optional snapshot must not
advance native delivery counters or claim display presentation. Existing stable
handle/key consumer semantics are preserved; physical receiver/display frame
identity still needs the parent qualification evidence.

Busy output ownership preserves the previous published snapshot. A GPU-query
error/timeout or uncertain exception disables that branch and quarantines its
source slot rather than permitting a future frame to overwrite an unfinished
read. A two-second query limit bounds polling; a driver API that never returns
can still prevent joining its worker during teardown. Device recreation and
that driver-hang shutdown case remain separate lifecycle qualification limits.

The bounded asynchronous [program-buffer-export] trace reports branch, frame,
generation, completed/refused/busy/unconsumed counts and maximum worker API wall
time at most once per second per active branch. Its GPU read completion flag is
not display presentation proof. Refusals also contribute to the existing
aggregate displayBusy diagnostic. Native deadline misses/underruns remain
independent; optional refusal never becomes a successful native frame claim.
The fault test holds either branch for roughly one second while native NV12
continues at 60 Hz, reads the other branch's actual GPU pixels and then checks
that the delayed image still contains its original tagged pixels.
