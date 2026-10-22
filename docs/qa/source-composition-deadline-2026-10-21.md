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
