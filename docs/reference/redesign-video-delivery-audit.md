# Video delivery across the redesigned shell

The incident and acceptance evidence belong to [#930](https://github.com/iamfatness/CoreVideoPro/issues/930).

Moving D3D calls to a worker is insufficient if that worker consumes one request
per `CompositionTarget.Rendering` callback. A busy XAML thread then still owns
the video clock. The presenter must retain the current immutable handle/geometry
and consume new producer pixels independently until suspended or disposed.
The UI owns binding, geometry, visibility and swap-chain attachment; it does not
grant permission for each frame.

| Boundary | Contract |
| --- | --- |
| Native Zoom, UVC and screen ingestion | Native producer callbacks/workers continue without XAML callbacks. |
| Managed capture fallback | Publish capture shared memory before dispatching UI telemetry. |
| Program, Preview, multiview and source monitors | Stable handles remain usable while the UI is blocked. Each swap chain has its own latency credit. Present does not wait for vertical sync on a shared worker. |
| Core snapshots | Keep one pending UI callback and the latest snapshot. Do not replay a backlog after a UI stall. |
| CPU thumbnail events | Visible CPU consumers request them; GPU-ready or hidden hosts release demand. Disabling the optional base64 stream leaves full-resolution I420 ingestion, GPU textures and outputs running. |
| Surface handle notifications | Structural binding/retirement remains on the UI thread. Stable-handle pixel progression does not require those notifications. |
| Recording and virtual camera | Observe actual writer/receiver progress. An accepted command only establishes requested intent. |

A swap-chain readiness signal is held until `Present` commits. A deferred
`DXGI_ERROR_WAS_STILL_DRAWING` result or occlusion does not submit a frame, so
the retry must not consume another readiness signal. Chain retirement clears
the held credit. The credit regression exercises a signal available exactly
once followed by a deferred present and a successful retry.

The old functional probe could accept roughly 25 presentations/second and gaps
up to 250 ms. That establishes neither smooth 60 Hz delivery nor display scanout.
Pixel checks, UI responsiveness and unit-test success answer different questions.

Persistent managed render workers still stop during process-wide GC. Unused
base64 thumbnail traffic can therefore stall GPU monitors even when no render
operation waits on XAML. `CpuPreviewDemand` aggregates visible CPU consumers,
reapplies demand after core restart and restores thumbnails on GPU fallback.
Legacy clients retain CPU thumbnails by default. The native regression checks
that decoded media continues with demand off and CPU images resume on demand.

`--verify-inspect-video REPORT.json` exercises stable-handle pixel changes and
a 750 ms blocked UI with an independent GPU producer. `--verify-inspect-native
CORE.exe REPORT.json` exercises the complete workspace, ten synthetic sources,
source/Inspect aliases, hidden-view suspension, restoration, deliberate driver
stalls and the operator monitor. The native probe explicitly requests 1080p60
for its synthetic Zoom phase. Preserve each surface identity in the report:
selecting a view-mode property alone does not prove a distinct Preview surface
was displayed; the Studio monitor can contain Program/Preview within one native
multiview texture.

These offscreen probes report `functionalPassed` separately. Their root `passed`
and frame-performance verdict remain false without physical delivery evidence;
their nonzero exit is intentional. Long gaps reject submission cadence, while
even perfectly spaced submissions cannot certify scanout. A completed maximum
gap and the elapsed time since the last successful commit are combined with
`max`, never added together.

## Combined workload qualification

Run `npm run qualify:redesign -- REPORT.json`. The report declares
`requiredInputIds` (eight remote sources, webcam and screen) and `requiredIsoIds`
(the selected ISO recording outputs). `samples` contain `processGeneration`,
monotonic `collectedAtMs`, and the observed native `snapshot`. Collect at least
every 10 seconds, after every required output is actually producing, for at
least 30 minutes. Include the independent `cameraPixelVerdict`, the blocked-UI
`monitorProbeVerdict`, and a `framePerformanceReport` following
[the strict evidence schema](../../scripts/qa/frame-performance-evidence.md).
`scripts/qa/collect-redesign-workload.ps1 -AppProcessId PID -OutputDirectory DIR
-ExpectedZoomParticipantId ID1,ID2,ID3,ID4,ID5,ID6,ID7,ID8
-ExpectedWebcamSourceId capture:CAMERA -ExpectedScreenSourceId capture:screen:DISPLAY
-ExpectedIsoSourceId zoom:ID1,zoom:ID2,zoom:ID3,zoom:ID4,zoom:ID5,zoom:ID6,zoom:ID7,capture:CAMERA`
collects the workload observations into a fresh directory. Supply the eight real
remote participant IDs from the intended show, including any unavailable feed;
the collector must not derive expectations from whichever sources currently
deliver frames. Background media cannot substitute for a remote participant.
It deliberately leaves
uncollected pixel/display evidence absent; its report cannot pass by itself.

Wait for actual Zoom video to reach 1080p before starting the recording: an ISO
writer retains the dimensions of its first source frame. A subsequent 1080p
input does not prove a 1080p ISO file. After finalization, supply `isoFileEvidence`
for every selected ISO, with `measurement: "ffprobe-count-packets"`, the measured
track's `sourceId` and `path`, actual encoded `width`/`height`, observed `packets`,
and the final writer's `muxVideoFrameCount`. The gate requires 1920x1080 and matching
positive packet counts. This file accounting supplements source-arrival and
pixel/deadline proof; it does not replace them.

For direct ISO arrival-to-file proof, explicitly enable `COREVIDEO_QA_ISO_COUNTER=1`
and `COREVIDEO_DELIVERY_TRACE_STAGES=program-and-iso` together with a fresh
`COREVIDEO_DELIVERY_TRACE_PATH`. This QA mode changes visible picture bands.
Zoom stamps its privately copied ISO buffer; UVC stamps its mutable capture
buffer before publication/preparation. Shared buffer owners are preserved.
Normal launches leave this flag unset and do not paint or export ISO identities.
The completed bounded binary trace includes stage 11, `iso-arrival-offered`,
tagged by `iso:SOURCE_ID`, with source owner, actual frame identity, observed
timestamp and actual geometry. It observes offers, so ring/FIFO losses still
fail separately. The existing ten live cadence aggregates remain unchanged;
ISO identity evidence is file-only, not an inferred aggregate success.

After recording and capture finalize, run `scripts/qa/iso-pixel-verdict.py` with
the ISO path, trace, source ID, explicit measured QPC bounds and the ffmpeg/
ffprobe paths. Its default minimum is 1,800 seconds. It compares the decoded
pictures with the exact ordered source-arrival identities, preserving source
cadence rather than inventing 60-fps ISO slots. Missing/repeated/reordered/wrong
identities, trace loss, source retirement or non-1080p payloads fail this proof.
Shorter diagnostic intervals must be labeled partial. Decode after the live
interval so verification does not contend with the workload.

Each `recording:iso:SOURCE_ID` performance path uses
`measurement: "source-arrival-fidelity"` with the unmodified ISO pixel verdict
in `proof`. The top-level `performanceEvidence.intervalTicks` declares string
QPC `start`/`end` and numeric `frequency`; those bounds must match the workload's
first/last observed `qpcTicks` samples and the pixel verdict exactly. The
collector records the corresponding UTC pair for human-readable coverage.
The pixel proof must refer to the same finalized track path inspected for
dimensions/packet counts. Synthetic 60-fps slot counts cannot replace ISO proof.
Program, GPU readiness, presentation and fixed-rate outputs retain their
anchored 60-fps completion/deadline checks.

The frame report must inventory `recording:program`, `virtual-camera:program`,
and `recording:iso:SOURCE_ID` for each selected ISO, plus
`preview-presentation` and `multiview-presentation` in its required paths.
The camera receiver must cover the measured workload duration. Do not count
only its duration: `cameraPixelCoverage` must contain `complete: true` and
the actual post-warmup `startTime`/`endTime`, correlated from receiver timestamps
to the workload clock. A successful receiver run from another interval is not
evidence for this workload. Do not count
startup, failed recording preparation, a process restart, missing inputs, or
disconnected outputs as simultaneous workload coverage. Preserve failed trials.
The report must also supply `candidateCameraSha256` and the administrator
inspection as `cameraInstallation`. Registration alone does not qualify a new
DLL: Frame Server can retain a previously loaded image until its consumers close
and the hosting service unloads it. Require observed loaded-module identity to
match the candidate as well as the registered protected file.
Each sample also records `displayState.visible` and `displayState.minimized`
from the tested process's actual window. A hidden or minimized window cannot
qualify a visible-monitor soak. This check does not establish absence of
occlusion or actual scanout; those still require downstream evidence.
Every required source, recording track and camera publication counter must
advance between samples. Status labels cannot establish liveness. The frame
evidence interval must cover the same wall-clock interval as the workload.

The gate rejects recording loss, Program-buffer underruns, virtual-camera
publication replacement/errors, incomplete coverage and missing downstream
evidence. Counter comparisons use interval deltas and reject resets. Historical
startup losses are not silently relabeled as losses inside a later interval.
The gate validates supplied evidence; retain raw traces, pixel receiver records,
recorded files, hardware/build identity and collector output with the report.
Correlate display traces with application presentation counters. Missing trace
events can resemble long display stalls; a disagreement makes the display
evidence inconclusive, not a pass and not a measured freeze duration. Exclude
elevation prompts and deliberate window changes from steady-state intervals.

## Recording preparation is outside the take

Zoom can change source geometry while selected ISO writers are preparing.
Reopening an empty preparation writer must discard that unused container:
Media Foundation reports `MF_E_SINK_NO_SAMPLES_PROCESSED` (`0xC00D4A44`) when
asked to finalize it. Discard is forbidden once any media or pending video
sample exists. Normal recording finalization retains all failure checks.

## Preparation polling and receiver measurement

Program's preparation worker uses its own high-resolution timer for short GPU
Map/GetData polls. Condition-variable waits nominally lasting 100 microseconds
are rounded by the Windows scheduler; repeated waits consume buffer lead. The
timer is separate from the delivery clock, and no application lock spans its
wait. NV12 staging and immutable snapshot copies are submitted together, with
one GPU completion query before CPU readback. The approved Program buffer depth
and pixel quality remain unchanged.
Each retained frame owns its staging textures and completion query. While an
earlier query is pending, the preparation worker can submit subsequent retained
frames on the same ordered context. Readback and retirement remain ordered;
in-flight resources cannot be recycled just because their deadline expired.
The regression defers the first completion until the second frame is submitted,
then checks both frames' distinct NV12 pixels and delivery order. This prevents
an accidental return to serial submit/wait preparation, but does not prove a
given live workload meets every deadline.
Slow preparation logs split GPU completion from CPU readback for correlation
with a missing frame; these logs alone are not GPU deadline acceptance.

The full-size NV12 regression requires all 180 submitted identities, in order,
with zero underruns/overflows. It drains the submitted tail instead of counting
intentional idle time after the producer stops. It is still a short regression,
not full-workload qualification.

Program rendering, preparation, scheduled delivery and asynchronous camera publication
register their own threads with Windows MMCSS under `Playback`. Audio retains
its existing `Pro Audio` registration. The video registration is scoped to the
worker lifetime and logs success/failure; it does not change process priority,
registry policy or optional display-worker priority. A high-resolution timer
alone does not guarantee that an ordinary-priority delivery worker runs when
its timer fires. Keep readiness failures separate from late publication of an
already GPU-ready frame when interpreting a trace.

The independent receiver retains bounded observation records in memory during
capture and writes NDJSON afterward. Synchronous diagnostic disk/pipe writes
must not delay its next ReadSample. Original monotonic arrival times, sample
timestamps and decoded pixel identities remain the evidence; buffering logs
does not smooth timestamps or relax the continuity/interval judge. An interrupted
capture without its completion record cannot pass.

A visible HWND does not prove an available interactive desktop. Qualification
requires an explicitly observed unlocked Windows session throughout the run.
The collector refuses to start when locked and terminates measurement on lock
or any required input whose identity stops advancing between observations.
WGC can retain its last frame and report connected/ready on a locked desktop;
that is not an active screen workload. Preserve such a run as partial evidence.

The camera DLL likewise queues serve diagnostics outside `RequestSample`.
A fixed 128-message queue uses a nonblocking producer lock; a blocked file sink
drops diagnostics when the queue fills rather than delaying camera samples or
growing memory. Drop/write-failure counters remain visible in delivery records.
Windows thread-pool callbacks retain the DLL before submission and release it
only after returning, including when the last COM source closes with logs queued.
The file sink permits concurrent diagnostic readers. A blocked-sink regression
checks bounded memory and continued producer progress, while real-DLL tests
still check actual fresh, held and slate pixels and wait separately for logs.
These tests establish isolation from diagnostic I/O, not live cadence acceptance.

The receiver's named stop event (`Local\CoreVideoPixelProbeStop-PID`) ends a
diagnostic trial gracefully and preserves the buffered observations. Early
completion does not satisfy the 30-minute coverage requirement.

For receiver scheduling comparisons, `--mmcss` registers the capture caller
under Windows `Playback` and records that mode and thread ID in the header.
The default remains ordinary scheduling; report which was tested. A scheduled
reference receiver does not qualify every third-party consumer. The identity,
cadence, duration and maximum-arrival-interval gates are identical in both modes.
`--fail-fast` preserves the first failing observation after the fixed warmup,
then stops so a circular scheduling trace can be saved promptly. A short capture
cannot pass the full-workload duration requirement. Do not discard a receiver
failure merely because source emission counters remained clean.

## Meter work belongs within the audio budget

Program loudness uses continuous K-weighting with rolling 400 ms and 3 s energy
sums. Re-filtering the preceding three seconds on every 20 ms audio tick is
display-derived work that can consume the audio deadline. The same continuous
filter supplies integrated gating blocks. Integrated loudness is recomputed
only when a gating block changes, using equivalent energy-domain thresholds.
Tests compare both rolling windows against independently filtered full-history
references across ring wraps, channel differences, level changes and rates.

Full-workload qualification also rejects interval increases in audio loss,
audio-clock reanchors and dropped recording audio packets. Good camera pixels
cannot qualify a recording with lost audio. Concurrent applications explicitly
included by the operator are declared with `-AdditionalProcessName` and must
remain running with the same process identity throughout the measured interval.

## Finalized Program pixels

After the live interval, `scripts/qa/recording-pixel-verdict.py` decodes every
encoded Program frame with FFmpeg's passthrough frame mode and reads both QA
counter guard rows. It rejects corrupt, repeated, missing and reordered pixel
identities. Optional first/last identities must come from the measured receiver
interval; both endpoints and every identity between them must be present.
Keep that interval explicit rather than dropping known failures from a result.
This is independent confirmation of recorded content, not evidence of physical
display or real-time delivery deadlines. Run decoding after the live capture.
Supply its result as `recordingPixelVerdict` in the combined report. Its first
and last identities and sample count must match the camera receiver's measured
interval; an unrelated clean recording cannot qualify the run.

## Camera request phase and ownership (#991)

The MF stream establishes its rational cadence once at the first successfully
queued fresh sample. Cold allocation and mapping work can finish after the first
request's read deadline; retaining that pre-allocation phase left following read
windows near the publisher boundary. The original issued deadline remains
unchanged and its diagnostic miss remains visible. Later fresh samples cannot
rebase the clock. The existing two-millisecond delivery reserve, retry bound and
two-payload-copy limit remain in force.

One request owns pacing, the shared timer, read deadline, copy and event delivery
under a separate request mutex. Stream-state operations retain their own mutex.
Shutdown takes request ownership before stream state so it cannot close a timer
being waited on. A request reserved in an earlier Start generation cannot emit
into a later Start. This avoids treating queued MF tokens as independent clocks.

The retained October 20 trial decoded every recorded Program identity while the
registered OS receiver observed one held duplicate, confirmed by FrameServer's
held/unrecovered counters. Program file continuity therefore cannot substitute
for camera pixel acceptance. The scheduling regression and direct-DLL MF tests
are necessary checks; only a fresh installed OS-camera workload can qualify it.

## Dispatch the monitor payload before current Program work (#995)

An early buffered Program copy alone does not isolate the complete multiview
canvas. The auxiliary monitor owner also needs its scene, current source refs,
Preview plan and bounded inspector export demand. Dispatching that payload after
the current Program render leaves a hidden dependency: a late render can consume
almost the entire monitor slot before the independent worker receives any job.

MediaCore now submits the same tick's immutable monitor payload before calling
the current Program renderer. A focused assembly method retains the existing
source selection, Preview and inspector demand rules. The buffered Program
image still comes from its earlier scheduled slot and copy owner; no output
deadline is extended. The caller-order regression first fails against the old
ordering. The real D3D pixel test uses the new ordering while injecting a current
Program stall and retains the original absolute canvas-publication deadline.
Correct pixels published after that boundary remain a failure.

For live qualification, enabling virtual camera or launching a receiver process
is insufficient. The registered OS consumer must negotiate 1920x1080 60/1 and
receive an actual decodable first frame before measurement starts. The shared
receiver flushes its setup header and reports its first frame once; per-frame
evidence stays buffered to avoid file I/O on the measured cadence. A receiver
startup error, failed exit or missing finalized evidence invalidates the run.
Final pixel continuity, whole-interval coverage and every other enabled output
must still pass independently.

## Managed monitor readiness and diagnostic projection (#997)

A nonblocking keyed-mutex or swap-chain readiness poll means only that the image
cannot be consumed at that instant. Waiting another entire 60 Hz period can
miss a producer that finishes immediately after every poll. Persistent workers
now retry only pending hosts, at most once per millisecond inside the original
slot. The next periodic slot takes precedence; stalls do not create a backlog
or extend an earlier deadline. A committed host does not participate in further
retries for that slot. Unsuccessful retries do not allocate UI snapshots.

The shell's managed render workers remain subject to runtime GC pauses. In the
controlled live diagnostic, native Program/ISO/OS-camera pixel proofs passed
while UI commits were below 60/sec and a matched 1080p output-enabled trace
recorded all-managed-stopped windows up to 50 ms. Sampled allocation stacks
identified repeated full diagnostic JSON projection on the UI feedback timer,
plus a typed snapshot deserialization that the native-wire path discarded.
These runtime observations are separate from physical display evidence.

Control feedback now captures the immutable snapshot and defers complete,
redacted observation projection until it is actually read or serialized. Record
copies share a thread-safe single evaluation; replacing or clearing the source
snapshot cannot retain an older observation. The native wire mapper no longer
first parses a typed stub snapshot. Typed projection borrows the parsed response
only during its synchronous projection, omitting diagnostic fields before
copying; the original raw observation remains available. Egress projection
applies the existing field policy and recursive redaction in one pass. Unknown
fields and secret-name filtering retain their prior behavior.

Regressions require low allocation when ordinary feedback does not request the
large observation, exact diagnostic facts and redaction when HTTP does request
it, and preservation of raw diagnostic payloads omitted by the typed view.
These tests cannot qualify display delivery. Fresh simultaneous local recording,
registered OS-camera pixels, eight actual live remotes, physical monitor timing
and installed desktop-shortcut acceptance remain release gates.

### Presentation ownership after the controlled pause experiment

Reducing control-plane allocation did not remove the runtime dependency: a
fresh 1080p recording/camera comparison still observed a 52 ms all-managed
pause. A compiled WinUI experiment deliberately paused all managed render
control workers for 750 ms while the independent GPU producer continued.
Both monitor aliases stopped. This proves an ownership defect independently
of attributing any individual production delay to garbage collection.

Persistent monitor hosts now lease their device, immediate context, chain,
back buffer and duplicated frame-credit handle to a native presentation owner.
There is one native worker per immediate context, one keyed-mutex ingest per
shared handle, and a private latest GPU texture shared by aliases. The worker
releases the producer key before presentation. UI and managed workers configure
leases and read bounded observations; neither drives steady-state GPU delivery.
Retirement queues resource release to the native owner without a UI join.
Suspension, handle replacement, chain revision and device failure retain their
existing managed lifecycle boundaries. No managed callback crosses the ABI.

The same compiled pause experiment now observes 46 inspector and 45 Preview
commits in 750 ms, with actual changing-pixel and alias checks passing. Native
tests also exercise no-control-call progress, suspension/resume, alias retirement
and cross-device rejection. The offscreen/compiled probes explicitly report
physical display delivery as unverified; accepted Present calls cannot qualify
scanout. The production package requires corevideo-monitor-presenter.dll, and
the package validator rejects its omission. A missing DLL retains the legacy
managed fallback and logs it as unqualified.

Native observations also count accepted-commit intervals over 25, 33 and 50 ms.
Qualification compares cumulative endpoint deltas inside the measured interval,
so a near-60 average cannot hide occasional long gaps. Missing counter fields
are unknown evidence. These counters do not establish physical scanout.

### Match the regular desktop launch during qualification

Task Scheduler defaults to priority 7, which starts actions and their children
at Below Normal priority, with background I/O and memory policy. A normal-SID
task alone therefore does not reproduce the owner's desktop launch. The local
reproduction also confirmed inheritance through the shortcut's `cmd → start`
pattern. Explicit task priority 5 produces Normal child priority; this is a
launch-environment correction, not a high-priority product workaround.

Qualification must record the installed shortcut launch, process generation,
elevation state, actual app priority and actual Windows-camera consumer priority.
`judgeRedesignWorkload` requires the matching process generation and Normal
priority for both processes. Missing observations and background/boosted runs
cannot establish acceptance of the regular installed launch. Preserve prior
diagnostics with their original launch conditions. Normal priority alone does
not qualify presentation: all source-picture and display-delivery gates remain.
