# Program deadline during eight-ISO recording — #1177

## Observed failure

The November 6 packaged eight-ISO run retained one Program recording missing frame. At 08:45:43 Eastern, render.display-tick held coreMutex for 14.590 ms, including 11.635 ms attributed to ISO ingress. The adjacent Program-buffer deadline reported target 27798 still preparing. The writer then reported a missed scheduled frame, with no recording write-queue drops. Eight actual Zoom feeds remained 1920x1080. This correlation identifies a boundary to investigate, not a proven initiating operation.

Native 88e97fea (content-identical to rebased #1174 f436f57b), shell af9f983a, RTX 4090/i7-14700K, two-frame Program buffer, hardware Program plus eight ISO writers, real meeting. Camera activation was refused by the existing registration mismatch; it is not qualified. Evidence lives under D:/CoreVideo-QA/resume-20261106; retain the original failed recording and log context.

A second retained miss occurred at 09:10:21, beside a 14.756 ms render hold with 10.799 ms in ISO ingress. Multiple other long holds concentrate in the same phase with zero engine-mutex wait. The first instrumentation slice separates prefetched arrival transfer, selection preparation and ingress leaf-lock wait from admission/retirement, using the existing fixed-size render timing recorder. It changes no queue or delivery behavior. Do not identify pixel destruction or lock contention as the live cause until the narrower evidence distinguishes them.

## Contract

ISO arrival transfer/admission must not hold Program rendering behind pixel copying, last-owner pixel destruction, device work or an encoder operation. Keep bounded ownership and preserve admitted ISO arrival identities/order. No source-quality reduction, buffer-depth increase, hidden drop, or startup/held-frame waiver. A moved operation must have a real production consumer and explicit shutdown/generation behavior.

## Investigation and execution

Trace the interval currently labelled ISO ingress into capture/Zoom arrival transfer, leaf-lock wait, admission and retirement. Inspect existing bounded queues and ownership first. Use a controlled blocked operation or payload-retirement fixture to prove any suspected dependency before changing scheduling. If the existing phase attribution cannot distinguish candidates, extend the existing bounded timing evidence and use it in the comparable live workload.

Implement the smallest demonstrated ownership/ordering repair in a focused component. Regress normal ordered arrivals, bounded overflow accounting, deselection/stop, session changes, and any slow-operation independence the fix claims. Keep a negative control that fails without the fix. Run focused and full native production checks, then package with exact component identity and repeat the same eight-ISO workload without overlapping local builds. Inspect finalized Program/ISO continuity, upstream arrival counters and original geometry; decode or use existing pixel judges where the source identity prerequisites actually exist.

## Acceptance and limits

A causal reproduction must show the old path blocking the required owner and the corrected path preserving both its deadline independence and source identity/quality. The repeated live run must retain every observed miss rather than report only a favorable average. Stable geometry alone does not pass Program delivery. Any remaining missed frame keeps release qualification failed. OS-camera and full-workload acceptance remain separate, explicit prerequisites; do not infer them from an eight-ISO comparison.

## Narrowed ownership repair

The first ten-minute diagnostic retained eight actual 1080p feeds and zero reported writer drops, but reproduced admission holds up to 11.34 ms with negligible transfer/lock wait. The next five-minute diagnostic isolates retained-view replacement: at 10:00:31 Eastern, 9.355 ms of a 10.723 ms render hold was `iso-latest`, versus 18 microseconds for queue work. Its finalized Program wrote all 18,024 scheduled frames; this shorter pass does not erase the original two misses. ISO holds/tails remain unjudged until pixel comparison.

`latest()` is only consumed when the live output worker is absent. The live path nevertheless retained that extra CPU-frame owner, so replacing an already-encoded frame could free its multi-megabyte buffer on the render caller. The repair distinguishes direct fallback from worker delivery. Worker delivery omits the unused retained view and moves admitted CPU arrivals into the existing drain queue; the direct fallback retains its previous view behavior. No additional thread, queue capacity, pixel copy or source policy is introduced.

A controlled payload deleter blocks until explicitly released. The regression drains on the consumer and attempts the next producer admission while retirement is blocked. Restoring the old retained-view ownership must fail producer independence; corrected ownership must retire on the consumer and admit the next ordered frame. Preserve tests for duplicate rejection, source epochs, capture timestamps, the four-per-source bound, explicit overflow counts, deselection and clear. Overflow is still an output failure, not a newly excused delivery path. This change does not claim all render stalls or overload/shutdown retirement are eliminated.
