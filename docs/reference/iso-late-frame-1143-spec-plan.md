# ISO late-frame loss: specification and investigation plan

Issue: [#1143](https://github.com/iamfatness/CoreVideoPro/issues/1143). Work order/time limit: [BACKLOG](../BACKLOG.md). This document defines behavior and method, not a second queue.

## Evidence and required behavior

A retained full native run recorded accepted=62, written=60, dropped_video=1, held=28, behind_hold=0, 88 decoded samples and a full-writer-queue warning. Isolated current/baseline reruns passed. Neither establishes the initiating cause. Preserve `D:/CoreVideo-QA/tmp/vcam-fixture-1131/full-native.log`, its failed media, and comparison logs.

An admitted ISO frame returning behind the last synthetic hold must be represented once at the established monotonic placement boundary, without collapsing the real gap. Subsequent real frames retain their capture timeline. Synthetic holds are separately counted; finalized decoded samples, real frame counts and held/tail counts agree. Genuine capacity overflow remains bounded and visible. Producer submission must not block the real-time path or conceal loss by growing queues.

The test must distinguish an intended late-timestamp scenario from unintended capacity saturation. Both are valid tests, but queue overflow must not be mistaken for evidence about late-frame placement.

## Investigation and implementation sequence

1. Pin source/executable hashes, read the recorded failing section and retained file, and inspect `MediaFoundationIsoFrameStampedBehindTheLastHeldFrame`, `driveDeparture`, `drainedIsoFileBytes`, queue admission, and writer placement. Record the number/timestamps of submitted real frames independently of implementation counters.
2. Establish exact-fixture shape: one selected ISO, matching video geometry, explicit timeline base, 50 initial real frames, one late frame, ten resumed frames; distinguish arming submissions from media-frame counts. Determine which real source identity was lost. Inspect admission/dequeue milestones, not only final counters.
3. Reproduce controlled capacity/worker scheduling separately from timestamp placement. Prefer existing observable writer progress; if a seam is needed, scope it to the tested owner and make failure cleanup release all gates. Preserve the original suite failure. Do not run unrelated GPU/UI workloads simultaneously.
4. If the fixture unintentionally overruns its bounded writer, make its intended preconditions explicit and retain a separate saturation test proving bounded admission and honest loss. If a production admission/placement defect is reproduced, fix only that owner and cover the counterexample. No rendering/encoder rewrite from an unproven hypothesis.
5. Run the exact red/green case, adjacent late/held/tail/departure cases, and full production Release suite after integration. Retain decoded media and diagnostics for failures. An unchanged passing rerun is comparison evidence only.

## Acceptance and discriminatory checks

At intended load: 61 real media pictures, zero dropped real video, one late placement at the held boundary, strictly increasing output PTS, resumed timestamps within the existing one-frame tolerance, and exact held-frame accounting against decoded samples. Stop/finalize stays within the existing bound without abandonment. In the explicit overload case, capacity and error accounting remain enforced.

A mutation that drops the late frame, collapses the gap, counts a held frame as real, or disables overflow accounting must fail a relevant assertion. Capture the evidence before deleting only test-owned files. No new timing tolerance, queue expansion or public claim of full workload acceptance is permitted on a test-only fix.

If causality remains unknown at the backlog limit, deliver the instrumented failure and the smallest repeatable schedule, keep #1143 open, and proceed to the next bounded item.
