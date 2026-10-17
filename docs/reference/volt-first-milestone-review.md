# Volt first-milestone review candidate

The approved first milestone is implemented on the Volt review branch. This reference describes the review workflow and qualified evidence; the issue/PR owns acceptance status. The installed application, desktop shortcuts, webcam registration and production preferences have not been changed.

## Operator workflow

Browse Graphics in Cards or Compact list. The nine catalog entries include the three compatibility lower thirds, logo-accent, transparent-name, centered title, corner logo, clock/countdown and corporate reference. Their thumbnails are real native reference renders. Choose a global or scene scope, create a hidden cue, then edit privately on the native canvas and layer inspector. Typed adjustments, direct geometry, sample copy, compare and undo do not change accepted Cue/Program.

Cue prepares Preview; Show explicitly reveals the accepted cue. Update is a separate on-air action. Hide removes the current graphic; Clear removes managed Program graphics and inhibits automation. Rack labels and ordering are native metadata, not a shell-only cosmetic order. Global and scene graphics preserve the existing transition boundary, and captions retain their separate owner above graphics.

Save cues and styles in the separate graphics document. Exact template/image pins and source overrides are retained. Restore creates hidden instances; a source-bound cue requires an explicit current source generation. Rules restore only after their target cues have been restored, and remain unarmed. Missing packages/fonts/required fields, changed versions and capacity limits have named refusals.

The existing-look import copies appearance, text, timings, verified logo, presets and source overrides without overwriting production preferences. Showing a managed lower third selects its family authority; old lower-third controls/API use that authority. Hide/Clear never reveals a retained legacy key. The explicit return-to-legacy action clears managed lower thirds and inhibits their automation.

## Introduction modes

Manual leaves the operator in control. Suggest offers a current native-qualified guest and an explicit Cue-to-Preview action. Rehearsal cues only. Automatic needs an explicit Arm after mode/configuration selection, and observes an exact subject draw rather than a roster/recommendation. Rules expose target, scene/source scope, dwell, auto-hide, cooldown and screen-share suppression. Multiple visible guests need a source filter. Show fires once per guest incarnation; unchanged metadata does not replay an entrance. Auto-hide starts at the first matching native draw.

Manual Show/Hide/Update/Cue holds the corresponding rule. Resume is explicit. Disarm cancels pending automatic reveals and offers Hide automatic graphics separately. Clear inhibits; reconnect disarms. Choosing graphics automation never arms scene auto-take. Native trace distinguishes observed, accepted/refused and draw-submitted states, with a bounded dropped-history count.

## Evidence and limits

Windows Release local suites at the integrated native feature revision passed 175 native graphics/monitor/D3D/contract tests, 2,536 MediaCore tests, 1,677 WinUI tests and 84 Control tests. Compatibility-scope policy tests qualify legacy controls against scene-scoped/custom templates. Exact source and binary identities are retained in the local evidence bundle; CI remains a separate required gate.

The packaged native payload SHA-256 `bcf661ea402e41582c28ec83e2744c04ebb4af5c737004bbcfdb1cfb66c77a20` was exercised with eight synthetic 1080p60 inputs, configured multiview, an automatic guest introduction, logo, live clock and real H.264/AAC recording. Its 60.249-second interval delivered 3,615 Program-buffer slots. Interval underrun/overflow/deadline-miss/GPU-not-ready/sequence-gap deltas were all zero; render deadline/drop/degraded deltas were also zero. The earlier development trial had two render overruns with zero buffer-miss deltas; that remains recorded rather than erased.

The finalized file decoded 3,804 video frames at 1920×1080, with interior timestamp intervals of 16.666–16.667 ms and no non-advancing timestamps or missed encoded slot. The harness checks decoded timestamps, not only the container average-rate field; a terminal partial flush frame is reported separately. Clear and a synthetic leave/rejoin verified inhibition, no replay and disarmed state.

This is a short synthetic native/recording qualification on the local RTX 4090 machine. There was no OS-camera publisher/receiver, physical-display consumer or real Zoom SDK meeting in this run. It is not a sustained bake, a fleet qualification, audio-listening/sync acceptance or a claim of zero misses at unmeasured outputs. The installed candidate and real-meeting/output bake retain their own acceptance evidence. #761 director acceptance remains separate; no director recommendation is used as a graphics trigger.

Runtime work is in PR #931 under primary issue #926, with #927–#929 consumers integrated for complete review. macOS AVF crash intake #933 and intermittent telemetry watermark-test intake #934 are separately tracked findings awaiting owner rank; their causes are not declared by these tests and required CI is not waived.
