# Source preparation inside the Program deadline

Issue: [#1016](https://github.com/iamfatness/CoreVideoPro/issues/1016).
This is a qualification dependency of #974/#1007 and PR #1008. Publication and
merge remain blocked until installed full-workload evidence passes.

## Causal evidence

The retained qualification-1008-03 interval contained 88 held webcam draws out
of 2,077 requests. Every selected webcam image completed GPU preparation before
its original Program output deadline, with at least 25.396 ms remaining. The
compositor had already chosen older pixels. A subsequent scheduling-only
candidate still held 142 of 2,610 webcam requests. These failed captures are
retained; ordinary Program delivery counters did not detect the stale pictures.

## Ownership and correction

The capture/preparation owners retain responsibility for pixel conversion,
upload and GPU completion. Program selects immutable source descriptors and
the resolved Take plan into one reservation from its existing bounded output
pool. On the next production tick, it composes that selection before requesting
newer source tokens. The original output slot, anchor and deadline are preserved.
At depth two, capacity remains five and total Program latency remains two frames.
No additional queue, texture pool or render thread is introduced. CPU and ISO
buffers and optional monitor leases are not retained by the pending composition.

Reservation nonces prevent cancelled or expired work from claiming a recycled
slot. Cancellation after GPU copy begins cannot release an in-flight texture.
Missing/stopped preparation remains a real source-admission failure. A source
that completes after the composition portion of the budget is not guaranteed to
meet the output deadline; workload measurements must establish that margin.

## Evidence boundaries

The first Release native run passed 1,841 tests, including real capture tests,
before the shipping-default switch and additional source-picture probe. That
run is retained under `artifacts/qualification-1008-05/native-tests.log`; it is
not final-candidate qualification. Focused real-GPU controls reproduce the old
substitution, verify corrected pixels at the unchanged deadline, and reject
incomplete/stopped producers. Reservation tests cover capacity, cancellation,
recycled tickets and expiration.

The picture probe reads actual NV12 bytes through the registered Windows camera.
Camera source identities are painted into the original capture before GPU
preparation, independently of the Program output marker. Interior marks survive
the Program marker. Ordinary body samples provide additional picture evidence.
An independently logged desktop stimulus tests screen-capture progression.
Missing, torn, stale, blank or mismatched samples fail the picture judge even
when Program identities continue. Its five negative/positive fixture checks
pass; installed picture qualification is still pending.

Source observation time is not sensor exposure time. Native source cadence and
actual source pictures must be evaluated separately from output cadence; a
59.94 Hz camera cannot be judged as a native 60/1 source merely because the API
rounds its rate to 60. The UVC log now preserves the negotiated rational rate.
Physical multiview/Preview presentation requires the exact native swapchain,
process, declared interval and actual 60/1 display. No average-FPS waiver applies.

## Installed diagnostic and remaining cadence defect

Candidate `91b9fb02` ran eight actual 1080p Zoom remotes, the 1080p webcam,
2560x1440 screen, local Program plus eight ISO recordings, and the registered
Windows camera consumer. Overwatch was also running. The declared interval was
65.792 seconds; this short diagnostic cannot qualify the required 30 minutes.
The finalized trace, all eight decoded ISO arrival sequences, Program output
markers and independent OS-camera output markers passed their component checks.
Exact source preparation substituted no pictures in that interval.

Actual source pixels still failed: both recorded Program and the OS camera had
147 repeated webcam pictures and 142 skipped webcam pictures. The negotiated
60000/1001 rate allowed six repeats. All 142 omitted arrivals were already GPU
ready before the previous composition. Source-selection intervals varied from
12.7461 to 21.0584 ms. Latest-arrival polling preserved this upstream selection
error even after exact GPU preparation was corrected. #1019 tracks the fix.

A counterfactual cadence replay selected 3,948 exact arrivals available before
the original compositions, with at least 4.3608 ms of readiness slack. This is
feasibility evidence only. The new bounded capture selection has a real-GPU
fixture that delivers the expected NV12 picture at the original two-frame
deadline; installed and sustained qualification remain required.

Screen stimulus age also reached 217.7325 ms against the predeclared 200 ms
bound. Two camera body samples fell below the declared contrast bound; those
observations require picture inspection rather than automatically attributing
them to capture failure. The previous failed verdict is retained. No beta is
qualified by these component successes or by a replay.

## Installed cadence comparison, October 22

Installed `1bbe0266` was measured for 65.8500867 seconds with eight actual 1080p
remotes, both local captures, nine hardware recording writers and the registered
Windows camera consumer. Actual camera pixels in recording and the OS receiver
had zero missing or reordered pictures and four repeats, within the six-picture
allowance for the negotiated 60000/1001 capture rate. All source marker checks
were attributable and ordinary camera pixels advanced. All eight ISO arrival
pixel sequences and original Program GPU deadlines passed their component checks.
This short interval is not the required 30-minute qualification.

Screen freshness still failed in three pictures, at 208.8711–209.3667 ms against
the unchanged 200 ms bound. In every case, a newer GPU-complete screen image
existed before composition but the reservation retained the image chosen a tick
earlier. Those newer images were independently observed in the receiver and
would have reduced the three ages to 109.3395–109.7316 ms. This is causal replay
evidence, not an installed pass for the correction. `ReadyCapturePublication`
selects the ready GPU image at composition while retaining the frozen source
identity, epoch and geometry. It also avoids pinning the older capture surface
throughout the CPU preparation interval.

The whole receiver capture contained one repeated output frame before the
predeclared workload interval. That failed report remains retained. The receiver
judge now also accepts the workload's declared host-clock bounds and includes
samples bracketing both endpoints; neither an interior nor a boundary repeat can
pass. Raw capture completeness and timestamp-order checks remain global. No
post-hoc selection of a convenient passing interval is permitted.
### Sanitizer functional-test boundary

The e0ac356 CI run retained a failure in
`MediaCoreCommand.ARouteLoopFlagReachesTheMediaSourceOnBothBuses`: its initial
wait depended on both sources reading `producing`, which includes the bus's
200 ms wall-clock freshness rule. That is an inappropriate speed condition for
a loop-command propagation test under ThreadSanitizer. The test now requires
real ingested frames from both sources, and additional preview frames after the
loop-only update, alongside the decoder's observed loop flag. Failure includes
the core snapshot. The focused Windows test passed; hosted sanitizer validation
is still required. No production timing bound or full-workload criterion changes.

### Installed e0ac356 comparison: retained failure

The next 65.71-second installed run preserved all eight ISO arrival streams,
Program output counters, registered OS-camera counters, and original Program
GPU deadlines. Screen pictures passed the unchanged 200 ms limit (maximum
186.44 ms). It still failed exact camera pictures: one held source caused one
omitted camera arrival in both Program recording and the OS receiver. The
requested frame's GPU-ready event followed composition by 0.2362 ms.

An isolated 1,000-sample wait comparison on this Windows host measured the
preparation worker's requested 100 us condition-variable wait at 1,163 us
median / 2,507 us p95. The existing high-resolution GPU-owner wait measured
520 us median / 576 us p95. Preparation now uses that bounded yield while GPU
queries are pending; idle arrival notifications remain interruptible. This is
a measured wait correction, not proof of installed acceptance. No Program
deadline, buffering, source-picture bound or quality setting changes.

Independent 10 Hz desktop samples also found three Program-camera observations
over the 100 ms bound (maximum 102.38 ms); these remain failures. Physical
display timing still requires the finalized ETW capture. A remote source had
a 240.54 ms offered-arrival gap, already 239.65 ms at the original observation
with consecutive shared-memory identities; this is not an ISO writer omission.
It does not establish whether the upstream pause arose in Zoom or its sender.
No full-workload pass or beta publication is claimed.

### Native viewer presentation policy (#997)

The retained installed e0ac356 interval has a failed PresentMon verdict on the
exact named multiview swap chain: 3,047 captured presents, 2,001 not displayed,
and 1,046 displayed versus 3,943 expected. Native submit counts and the 10 Hz
desktop pictures disagree with a literal interpretation of the longest ETW gap;
that discrepancy remains under investigation, not a display pass.

The native presenter used flip-model `Present(0, DO_NOT_WAIT)`. Microsoft
[documents](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)
that interval zero permits queued frames to be discarded. The comparison
candidate uses interval one with the existing nonblocking flag and one-credit
per-chain latency limit. Five actual-GPU presenter ownership/pixel tests pass;
they use an offscreen HWND and do not establish physical display cadence. The
installed comparison must prove whether this policy corrects the display
result without harming latency, source pictures, recording or OS-camera output.

### Installed bf04b6b comparison: ownership and pacing failures

The retained 65.41-second interval kept eight actual 1080p remotes, both local
captures, nine hardware writers and the registered OS camera. All eight ISO
arrival streams and Program/OS-camera output markers passed. Actual source
pictures failed: two unavailable camera draws, one held camera draw, and three
screen observations above 200 ms (maximum 203.51 ms). Desktop Program-camera
freshness failed 54 observations, maximum 115.57 ms. The surrounding 70.01-second
native presentation bracket had 29 commit gaps over 25 ms; physical ETW remains
pending. Interval-one presentation alone did not correct the viewer.

Both unavailable camera identities had completed GPU preparation before the
draw: one 22.15 ms earlier and one 9.67 ms earlier. Capture's try-lock publication
can refuse a valid choice/resolution while its writer publishes. Completed CPU
and direct-GPU publication had the same failure pattern. Readers now acquire
immutable publications; capture retains three arrival identities and weak CPU
preparation descriptors still cannot pin a reusable GPU slot.

The held camera completed 0.593 ms after composition, despite the original
Program deadline remaining ahead. Composition now checks exact readiness until
the original deadline minus half an output period, reserved for composition and
GPU completion. The measured draw-to-ready maximum in this failed run was
5.946 ms; that observation does not guarantee the reserved 8.333 ms is sufficient
under the full workload. Deadline and capacity gates remain unchanged and must
reject any resulting overrun. Temporary readiness probes preserve source cadence
and never relabel an older frame or extend its deadline.

The native viewer now includes available DXGI frame-latency handles in its wait
set. A display credit can trigger its bounded owner immediately, independently
of the periodic source retry clock. Credits remain one per chain, producer keys
are released before Present, and no display queue capacity is added. Concurrency,
late-completion and actual-pixel credit tests are required, followed by installed
comparison and the full workload. No merge or publication is authorized by these
changes alone.
