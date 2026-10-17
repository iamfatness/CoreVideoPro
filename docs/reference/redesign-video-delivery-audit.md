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
`scripts/qa/collect-redesign-workload.ps1 -AppProcessId PID -OutputDirectory DIR`
collects the workload observations into a fresh directory. It deliberately leaves
uncollected pixel/display evidence absent; its report cannot pass by itself.

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
Slow preparation logs split GPU completion from CPU readback for correlation
with a missing frame; these logs alone are not GPU deadline acceptance.

The full-size NV12 regression requires all 180 submitted identities, in order,
with zero underruns/overflows. It drains the submitted tail instead of counting
intentional idle time after the producer stops. It is still a short regression,
not full-workload qualification.

The independent receiver retains bounded observation records in memory during
capture and writes NDJSON afterward. Synchronous diagnostic disk/pipe writes
must not delay its next ReadSample. Original monotonic arrival times, sample
timestamps and decoded pixel identities remain the evidence; buffering logs
does not smooth timestamps or relax the continuity/interval judge. An interrupted
capture without its completion record cannot pass.

The receiver's named stop event (`Local\CoreVideoPixelProbeStop-PID`) ends a
diagnostic trial gracefully and preserves the buffered observations. Early
completion does not satisfy the 30-minute coverage requirement.

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
