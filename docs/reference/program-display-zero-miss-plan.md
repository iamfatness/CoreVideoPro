# Program-to-Multiview delivery: cause analysis and zero-miss repair plan

Owner request: October 14, 2026. Issue: [#904](https://github.com/iamfatness/CoreVideoPro/issues/904).
Parent acceptance: [#517](https://github.com/iamfatness/CoreVideoPro/issues/517).
Related fixture evidence: [#898](https://github.com/iamfatness/CoreVideoPro/issues/898);
a common initiating cause has not been established. Work order lives only in
[BACKLOG](../BACKLOG.md). This document describes the repair and its gates.

## Finding and confidence

The loss boundary is established: three Program delivery identities did not enter
the Multiview reader's private GPU copy. Program delivery, webcam publication and
monitor CPU jobs continued. This is downstream of the Program buffer, inside the
optional monitor's Program-image admission/retirement path.

The strongest explanation is the reader's two-outstanding-read admission limit:
it polls GPU completions once, refuses a new identity if both remain pending,
draws its previous image, then normally selects a newer Program identity on the
next job. There is no deadline-aware retry of that refused identity.

**The original three refusal reasons are not proven.** Capacity, import and
allocation failures share one counter. Sparse observations cannot retrospectively
recover the missing branch/HRESULT. Do not rename this hypothesis a confirmed
GPU stall, driver fault, or regression from this beta. The first implementation
gate is attribution, not an assumed capacity fix.

An instrumented real-meeting reproduction later established `read-capacity` as
the actual branch for that trial. This does not retrospectively attribute the
historical three or establish a GPU driver fault. See the reproduction limits
below; the original evidence and its uncertainty remain intact.

### Preserved measured evidence

Baseline: installed `beta-2026-10-14-4366d5b`, RTX 4090, isolated monitors,
two-frame Program buffer, recording/streaming off. Eight producing 1920x1080 Zoom
sources and a 2560x1440 screen source were observed during the real-meeting run.
The owner used the PC; control was through the API, with no desktop input.

| Boundary | Delta over 59.625 seconds / 118 API samples |
|---|---|
| Program | 3,570 delivered; zero new underruns, deadline misses, gaps, overflows or GPU-not-ready counts |
| Webcam publisher | 3,570 accepted/published; zero pending replacement or exception counts |
| Monitor CPU jobs | 3,570 submitted/completed; zero failure, supersession or retention refusal counts |
| Multiview Program reader | 3,567 copies; 3,566 completions; **3 refusals and 3 skipped identities**; zero new reuse or reordering counts |
| Optional resource lane | Two announcements admitted; zero refusals |

All three new refusals/skips land between samples at 58.688 and 59.203 seconds:
Program and monitor completions advance 29; accepted Program-reader copies advance
26; copy completions advance 27; outstanding reads change from two to one.
The one-copy difference between submitted and completed deltas is one additional
in-flight read, not a fourth lost identity. Every sample satisfies
`copies - completedCopies == gpuReads`.

Copied generation remains 1, allocation charge remains 66,355,200 bytes, and
sampled state remains `copied`. These are completed observations, not records of
each failed request. They cannot exclude an unseen request/import transition.

The baseline already contained one Program-buffer underrun/overflow, three render
deadline misses, 25 monitor supersessions, four webcam replacements and 16 display
skips. Retain them. This run is not whole-session zero-miss evidence. The API
observes counters and identities, not physical display pixels or decoded audio.

Local evidence, relative to `release-bundle-20261014/artifacts/bundle-release/`:
`api-live-60s/report.json`, `api-live-60s/samples.jsonl`,
`api-live-workload.json` and `api-prevalidation-baseline.json`.
Preserved SHA-256 values: report
`797319dbb2f142208832f48c116258a7c69f68f75732f9d3e582476312ec02b0`;
sample stream
`43cd68d01df842789279ea25e01e7da8a081490a5143e6b4aa54167d372249ac`.
The independent OS-camera counter test is separate; it does not qualify this
real-meeting monitor path.

### Instrumented reproduction

Native source `8a9f433`, Release/D3D11 on the reference RTX 4090, reproduced the
failure with eight live Zoom feeds, a synthetic 2560x1440 BGRA screen and
`pgmPvwTop` Multiview. CPU-source preparation was explicitly **0**. Webcam and a
shell consumer were absent from the owned headless candidate; the operator's
separate app/webcam publisher remained running. This is attribution evidence,
not matching-all-output qualification.

**Workload correction:** the first harness revision bound Program to synthetic
participant `101` during real SDK runs. The real feeds appeared in Multiview,
but Program selected a missing source. That includes the reproduction below and
the earlier clean retry/priority/readiness controls: their reader mechanics
remain evidence, but they do not qualify real-source Program composition.
The corrected harness binds actual roster IDs and checks every requested
Program source's GPU admission, dimensions and renderer. Its first actual
nine-source grid pilot retained initial unavailable-source admission; a clean
reader count alone does not turn that trial into a full-workload pass.

The corrected grid/camera depth-two trace (`b40a3a4`) isolates a second cause:
CPU checkpoint 11 (Program copy submitted) to 12 (participant exports done)
took 64.9321 ms for identity 262. The log records eviction of the screen's
participant export in that interval. Erasing `ParticipantTex` destroys its
`D3DDecoupledExport`, whose destructor joins its deferred D3D initialization
worker. The next selected Program identity is 266: three skipped identities.
The reader has no refusal, import or allocation error. This is a synchronous
resource-retirement stall on the monitor owner, independent of the earlier
capacity-refusal branch. Observed completion ages include this CPU delay and
are not GPU execution durations.

The repair moves participant exporter eviction, failed creation and replacement
to a separate bounded retirement owner. Its four-item / 256 MiB texture-payload
charge includes the executing destructor. On saturation the original caller
retains ownership and defers the optional replacement; it never joins the
worker or allocates an overflow queue. Driver/device overhead is outside that
texture charge. Shutdown still drains owned resources. Snapshot evidence
reports pending count, charged bytes and refusals.
The participant-export map also has a 64-entry admission bound, so an indefinitely
stalled destructor cannot accumulate inactive caller-owned entries without limit.
Preview and Multiview resize/failure retirement use the same owner; the existing
encoder's separate readiness policy is unchanged.

The first repaired 60-second depth-two repeat (`aea0ba8`) had 3,931 completed
copies, zero refusals/skips and four observed readiness deadline misses. Its
registered OS receiver decoded 3,600 identities without loss/repetition, with
serving-module identity verified and no discarded warmup.
The subsequent critical-copy progress change (`e2d58d6`) flushes and observes
copy readiness before optional work, bounded by the same absolute deadline and
cancellation state. Its 60-second depth-two trial completed 3,920 copies with
zero refusals, skips, late submissions or observed readiness deadline misses.
However its OS receiver found 41 repeats and 41 missing identities. This does
not establish whether that variation was introduced by the critical-copy change;
the prior longer combined trial had camera losses too. Do not promote either
trial to full-output zero-miss acceptance.

Both corrected real-source trials also retained unavailable source admissions
(`preparation-stopped`) during SDK epoch changes, tracked in
[#908](https://github.com/iamfatness/CoreVideoPro/issues/908). Their actual Zoom
inputs were observed at 640x360, not eight 1080p feeds. Reader copy mechanics,
source content, OS-camera pixels, physical presentation and audio have separate
verdicts. The original app's camera state was restored after each owned test;
the registered DLL and installed beta were not replaced.

At sample elapsed 77.360 seconds, requested Program identity 4598 was rejected
after both real `GetData(DONOTFLUSH)` calls returned `S_FALSE`. Outstanding
identities 4596/4597 had observation ages 33.374/16.784 ms. The import was cached,
generation was 1, and `GetDeviceRemovedReason` returned `S_OK`. The original
absolute deadline still had **15.368 ms** remaining. Abandoning that identity
before spending its available admission budget is the demonstrated loss
mechanism. Query age does not identify GPU execution duration or explain driver
scheduling; that requires the progress/ordering experiment.

The last retained sample had 591 capacity refusals, zero resource/query failure
counts and 654 skipped identities. Monitor supersessions also occurred (72
lifetime), so reader repair alone cannot waive the complete-path requirement.
Program underruns, deadline misses, sequence gaps and GPU-not-ready counts stayed
zero. The trial stopped at its 64-incident artifact budget after 164.875 seconds;
it is explicitly incomplete/failed, never a performance pass. Its first incident
and prehistory remain intact under
`artifacts/program-display-904/real-program-preview-attribution/` in the #905
worktree. Later probes have a bounded 512-file / 256 MiB incident-artifact budget,
separate from the reader's unchanged 512-record / 128 KiB memory bound.

## Source explanation

Source baseline: commit `4366d5b4e376b4e40bb2a876a93f48e648620366`.

1. `D3DProgramBuffer::multiview` selects an immutable publication using the
   scheduled production slot, with the existing one-frame display offset.
2. `D3DProgramDisplayReader::copy` polls its old completion queries once using
   `GetData(..., D3D11_ASYNC_GETDATA_DONOTFLUSH)`. Only two read queries exist.
3. If both remain outstanding, it increments `refused`, returns the previous SRV,
   and submits no copy for the requested Program identity. Import/allocation
   errors can produce the same refusal count.
4. A later accepted identity advances `copiedSequence`; a jump increments
   `skipped`. `MonitorRenderWorker::completed` still advances when the CPU batch
   returns. It does not establish GPU completion or successful presentation.

The producer's immutable pool has eight slots; the reader has two in-flight
queries. The current `capacity = 8` observation describes the former, not the
reader's admission limit. Expose both explicitly.

Ownership and flush ordering were independently audited. The immutable source is
retained until copy completion; old generations retain their own leases;
unretired reads are quarantined on teardown. Multiview and the batch both flush.
No missing-flush or use-after-free defect was established. Source exports precede
Program copy admission, and previous Multiview/Preview commands share its monitor
GPU stream; short queue/progress delays are therefore possible even when CPU
work is fast and aggregate GPU utilization is low.

[Microsoft documents Flush as asynchronous](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-flush).
[GetData S_FALSE means results are not yet available](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-getdata),
not that the query failed. `D3DProgramBuffer::completePreparation` already records
a separate NVIDIA preparation-context observation involving DONOTFLUSH versus
flags 0. That is a discriminating experiment to consider, not proof of this cause
and not permission to add unconditional flushes or a spin loop.

Source anchors:
`native/src/modules/D3DProgramDisplaySnapshots.h` (copy/retire/observation),
`D3DProgramBuffer.h` (scheduled selection), `D3D11CompositorAdapter.cpp`
(submitMonitors/renderMonitorBatch/renderMultiview), and
`MonitorRenderWorker.h` (CPU completion accounting).

## Repair contract

### Refusal evidence contract

`program-display-refusal-v1` is additive to the existing display observation.
`latestReader` carries live reader evidence even when the exported canvas is old
or unavailable. The outer observation retains exported-image attribution;
never relabel those older pixels with the newest requested/copied identity.
`readerEpoch` separates reader lifetimes; `readerReadLimit` describes the reader,
not the producer's `capacity`. `refusalReasons` separates admission refusal from
query creation/read, import, texture creation and SRV creation failures. A query
read failure is a failure event; subsequent refused requests are separately
counted as `failed-reader`, so summing every failure category is not the same as
counting refused requests. `notReadyPolls` includes normal unavailable results
and is not a dropped-frame count.

`capacityPressures` counts admission pressure; `admittedAfterPressure` distinguishes
absorbed pressure from a refused identity. `deadline` means the original budget
expired; `cancelled` is explicit monitor-disable/shutdown cancellation and does
not increment `refused`. `lateSubmissions` records a real copy whose CPU submission
returned past its deadline; copied pixels cannot waive that failure. The paired
poll flags and `flushingPollRetirements` describe observed progress when a
pressure-only flags-0 poll was used, not proof that a driver is defective.

The fixed numeric incident history has 512 records and remains below 128 KiB.
An incident snapshot retains prehistory and refreshes after eight further copy
submissions. Stage IDs are 1 admission, 2 copy submitted, 3 completion observed,
4 failure/refusal, 5 completion unavailable and 6 CPU flush returned.
Stage 7 records pressure, stage 8 recovered admission capacity, and stage 9
readiness not observed by the read's original deadline. Query records identify
their `subjectSequence`/generation; their target/deadline refers to that read,
not the newest request being admitted. Pending
identities and ages describe outstanding source leases at that observation.
`traceOverwritten` describes bounded prehistory retention; `traceCaptureFailures`
invalidates complete incident evidence. Diagnostic allocation failure must not
throw into rendering. Timestamps and exact identities use decimal strings;
the clock is steady-clock nanoseconds. Flush and completion-observation times
are CPU observations, never GPU execution duration or physical presentation.
No free-text SDK errors, names, credentials or image payloads enter the records.

Each retained GPU read carries its original target/deadline. An outstanding read
is counted once in `gpuReadinessDeadlineMisses` if readiness is not observed by
that deadline; later recovery cannot erase the failure. This is a conservative
readiness-observation failure, not a measurement of GPU execution time or proof
that physical display dropped a frame. `gpuDeadlineCompletedReads` establishes
how many completed reads actually had a declared deadline. Zero misses with
unknown-deadline reads is not timing qualification.

An idle completion pump uses the existing monitor owner, polling at most once
per millisecond while reads remain outstanding. With no pending retirement it
returns to the normal mailbox wait. Device-failed readers stop the idle pump;
shutdown interrupts software waits. Callback resources retire on that owner
before its COM apartment closes. Completion updates publish immutable reader
metadata without inventing a new monitor job or changing exported-image identity.

The headless `scripts/qa/program-display-copy-probe.py` retains startup and the
synthetic workload's snapshots. It labels all-output and presentation acceptance
unverified, even if copy counters happen to be zero. It never controls desktop
input or opens the operator's camera/stream/recording destinations.
Real-meeting mode uses the full authorized URL from a local input file; optional
SDK credentials pass through anonymous stdin and never enter capture artifacts.
The probe requires UTF-8 mode for native RPC (#877). Explicit pre-demand and
demand-ack phases retain startup counters; they do not reset or discard them.

Preserve 1080p60 quality, the selected two/three-frame Program buffer, existing
display offset, audio alignment, enabled outputs and producer isolation. Do not
increase the pool, read limit or output delay to conceal misses. No CPU readback
or speculative current Program may replace the actual delivered Program image.

Every required monitor identity must either reach its target within the declared
deadline or create an explicit failed delivery record. Holding a last-good image
is safe fallback, not successful delivery of the new identity. Separate selected,
copy-submitted, GPU-ready, monitor-exported, shell-imported and presented facts.
API/CPU completion cannot stand in for an unobserved stage.

GPU completion retains/releases ownership on its existing device owner. No D3D
call, GPU wait, optional formatting or retirement may run under the Program/core
lock or delay camera/audio production. Any retry is bounded by the original
identity's absolute deadline; it must not become an accumulating replay queue.
An unbounded GPU/driver stall cannot be guaranteed lossless: record and fail the
tested configuration honestly rather than relabeling overload as zero loss.

## Implementation sequence and decision gates

### 1. Attribute each refusal before choosing the repair

Add cumulative reason counters for `read-capacity`, `query-create`,
`query-read-failed`, `import`, `texture-create` and `srv-create`. Distinguish
`S_FALSE` progress delay, device removal, and an absent/repeated selected identity.
Expose producer pool capacity and reader in-flight limit separately, with a
reader epoch so recreation never looks like a silent counter reset.

Extend the existing delivery-trace infrastructure where it fits this contract.
Retain a fixed-size numeric incident trace, capped at 512 records / 128 KiB,
with explicit overwrite/truncation counts. Records carry reader/producer epoch,
job and requested/copied/completed identity, requested dimensions and pool slot,
cache hit, reason/HRESULT/device HRESULT, outstanding read identities and ages,
copy-submit/flush/completion-observed monotonic timestamps, and the absolute
scheduled target/deadline. Include post-retirement read count at admission.
QPC-to-poll age is an observation bound, not GPU execution duration; label it so.

The monitor owner writes preallocated records; an off-lock diagnostics owner
serializes immutable batches. No per-frame file logging, strings, synchronous
JSON assembly or GPU queries from API requests. Publish cached bounded evidence
through the existing snapshot/support contract; any trace control is a typed
`POST /invoke` action, not a generic command tunnel. Capture pre/post incident
history and evidence-overflow failures without changing the tested deadline.
Exclude meeting passcodes, credentials, participant names and video payloads.

Gate: independently force each reason and verify counters, identity records,
ownership and recovery. Then capture an equivalent real incident in the
instrumented candidate. The historical three remain branch-unattributed unless
additional original evidence actually establishes them.

### 2. Repair the demonstrated admission/progress failure

For confirmed read-capacity/progress loss, prioritize delivered Program admission
ahead of optional source exports and conversion work. Prewarm imports and the
local image per generation on the monitor owner. Retire signaled reads between
jobs as well as at admission, without depending on the next frame to notice
completion. Preserve the existing read limit and frame offset for comparison.

Derive and carry an absolute monitor target/deadline from the rational Program
timeline. `MonitorRenderRequest` currently has no such explicit deadline. Do not
start a fresh relative timeout on each retry. If a read becomes available within
the remaining budget, retry the same selected identity before exporting the
monitor image. Never block producer delivery or silently skip straight to the
newest identity while claiming full success.
Cancel pending retries when the monitor is disabled or the core stops. Bound
software waits and shutdown work; preserve or quarantine GPU-owned leases rather
than recycling them to make shutdown appear complete.

Measure each candidate change separately. If the GPU actually completes after
the required deadline, a completion pump or different poll flag alone cannot
repair it. Trace queue scheduling and reduce/isolate the Program-copy critical
path from auxiliary work. A dedicated transfer context/device is a conditional
design decision only if that contention is demonstrated; it needs its real
Multiview consumer, explicit budget and lifetime tests in the same change.

For confirmed import/allocation failure, repair the failing resource transition
and preserve its HRESULT/epoch. Retry within the same remaining budget when
recoverable; otherwise publish an explicit degraded state and fail acceptance.
Do not treat changing the two-read limit as a fix for resource failure.

### 3. Prove behavior with causal controls

Use the real reader with narrow test-only completion/resource fault seams:

- Keep two GPU completions unresolved, then release before the target deadline.
  Require the same Program identity and independently expected pixels, with no
  refusal/skip after repair. The old path must lose that identity under this
  control; a passing test that merely increases capacity is insufficient.
- Hold completion beyond the deadline. Require explicit failure and no early
  source-slot reuse, false readiness, hidden duplicate or unbounded wait.
- Force first-use import, genuine resize allocation and query failures separately;
  prove correct attribution, bounded recovery and no stale-generation pixels.
- Cover generation replacement, duplicate selection, hide/reopen, disable/enable,
  device loss and teardown with reads outstanding. Preserve old-generation leases
  and quarantine failed reads rather than recycling in-use textures.
- Disable the monitor and stop the core while a retry awaits its deadline;
  require cancellation, bounded software shutdown and safe outstanding ownership.
- Verify cached API diagnostics under delayed formatting and incident pressure;
  saturation must remain bounded and must not stall Program/audio/camera.

Fault controls are unavailable to shipping API callers. Timing-sensitive hardware
tests run in an appropriate validation window, not as a competing stress load
while the owner works. Existing phase, pixel, buffer and real RPC gates remain.

### 4. Qualify the complete path without resetting away failures

First run a controlled 1080p60 counter/pixel sequence through the actual Program
to Multiview chain, using independently decoded output identities and readiness
timestamps. Validate both supported Program buffer selections and real source
changes. A native copy test is not shell presentation proof: record the actual
consumer's import/present identity/deadline, or label that stage unverified.
API automation drives actions; independent pixel observers must not steal the
operator's shared-texture ownership or perturb its delivery.
The current API does not expose Sources-page navigation or every editor action.
Those matrix cells require a typed API action backed by the existing application
command, or an operator-assisted review at a suitable time. They must remain
unverified until covered; do not substitute desktop automation while the owner
uses the PC.

Then repeat the eight-source real Zoom + screen workload on the installed
candidate: steady state, ordinary desktop use, source resolution ramps,
remove/reassign, Preview/Take, visible-page changes, grading/scopes on/off, and
the supported recording/streaming combinations. Use local/safe test destinations,
never the operator's public stream without its own authorization. Add a sustained
meeting bake and additional supported hardware before broad qualification.

Retain startup and transition failures from the first required/advertised-ready
slot; define readiness explicitly, retain pre-ready evidence separately, and do
not discard arbitrary warmup intervals to make a pass. Keep all failed trials,
source/reader epochs, exact flags, workload, artifact hashes and duration.

For every required stage, zero unexpected gaps/repeats/reorders, copy refusals,
unabsorbed deadline misses or supersessions are required in its qualified
interval; expected source cadence holds must be distinguished from lost Program
identities. Audio, Program and webcam must not regress while fixing Multiview.
Average FPS or a smooth visual impression cannot waive a failed stage. Missing
receiver/presentation evidence remains unverified. A finite pass is scoped to
its measured workload and duration, never an unlimited zero-miss guarantee.

## Delivery and closure

The October 14 combined depth-three trace retained a further cold-import
boundary: identity 251 entered reader selection with about 12.5 ms remaining,
no pending reads and no cached import, then refused after 17.1 ms inside that
selection before any copy submission. This bounds the stall to cold import /
private-resource setup or scheduling within that call, not GPU execution.
Import entry/return stages 24/25 distinguish that boundary on a recurrence.
Prepare all eight shared-slot imports for already-registered monitor devices
during pool initialization, using thread-safe device calls without touching a
foreign immediate context. Private surfaces/queries are already prewarmed.
Late/unsupported consumers keep the explicit attributed fallback; preparation
does not fabricate delivered pixels or enlarge the pool/read limits. The
hardware test reads distinct pixels from every slot on an independent consumer
while refusing any attempt to perform a cold import inside reader admission.

Land attribution with its actual API/support consumer, the causal repair and its
negative control, then installed qualification evidence under #904. Preserve
rollback and do not change the installed app during plan review. Close #904 only
after the issue's target path meets the identity/deadline criteria. Keep #517,
#898 and the separate monitor counter issue #903 open until their own acceptance
criteria are met; this plan closes none of them.


## Deadline owner separation

The retained `boundaries-d5a-depth3` run still failed one original GPU-readiness
deadline (identity 251). Immediately before it, the same monitor owner spent
5.056 ms mapping/copying/unmapping the actual 2560x1440 BGRA screen, 3.624 ms
in Multiview export, and another 2.513 ms in Preview. The next request was
submitted at target +1.453 ms but its copy was not submitted until +5.306 ms.
The event query was still S_FALSE at +16.791 ms, beyond the original
16.667 ms boundary. This is a real missed readiness deadline, not merely a late
observation of a known completed copy. These CPU boundaries do not identify
an internal driver/GPU scheduling cause; they establish the avoidable shared
owner/context dependency. The earlier `combined-538-depth3` failure had the
same dependency, with 17.475 ms inside the preceding Multiview pass.

The isolated path now uses a dedicated `D3DProgramCopyOwner` device, context,
and mailbox for scheduled copies. It refreshes only the exact publication and
polls against the original deadline, with the existing two-query read limit.
A separate monitor owner performs source uploads, exports, Multiview and
Preview drawing. Neither monitor input-cache work nor those optional GPU
commands runs on the critical copy context.

The copy destination is a three-image immutable shared output pool. It is
storage for in-flight ownership, not an additional timeline delay: the selected
Program depth remains two or three frames. Each completed image carries its
original delivery sequence, frame number and timeline observation. The monitor
renders that exact image and retains it through its own GPU read query.
Occupied output images cannot be overwritten; exhaustion is an attributed
read-capacity refusal, never silent reuse. The output storage shares the existing
256 MiB monitor residency ceiling. At 1080p it adds 24,883,200 bytes compared
with the previous single 8,294,400-byte private image (a net 16,588,800 bytes).
Reported allocation includes both the eight input publications and the private
output images; selected depth, input capacity eight, and read limit two are
unchanged. Unresolved writes quarantine their output lease as well as their
input lease on shutdown.

Both owners and every output image are prepared before anchoring Program's
cadence. The initial two/three production slots have no delivered Program
identity yet; they publish the monitor's actual empty startup state without
waiting on or counting a nonexistent negative timeline slot. No admitted
identity or failed trial is discarded.

The regression test blocks the actual optional consumer for 24 consecutive
60 Hz copies, then reads the first retained and latest images through a separate
D3D device. It requires all original copy deadlines and identities, plus exact
first/latest pixel values. Reader readiness remains distinct from monitor
render/export health and physical shell presentation. A clean copy count cannot
waive superseded monitor work, bad pixels, camera loss, or unmeasured display/audio
continuity.
