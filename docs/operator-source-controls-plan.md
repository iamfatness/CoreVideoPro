# Operator timing and source controls — execution plan

Behavior contract: [operator-source-controls-spec.md](operator-source-controls-spec.md).
Issues: [#627](https://github.com/iamfatness/CoreVideoPro/issues/627),
[#592](https://github.com/iamfatness/CoreVideoPro/issues/592),
[#681](https://github.com/iamfatness/CoreVideoPro/issues/681),
[#456](https://github.com/iamfatness/CoreVideoPro/issues/456),
[#668](https://github.com/iamfatness/CoreVideoPro/issues/668).
This document describes implementation and proof, not priority or status.
[BACKLOG.md](BACKLOG.md) alone determines work order. Each PR names one issue;
no shared foundation PR without the first shipping consumer.

## Design gates before implementation

1. Review the [#456 Clip range layout and on-air staging rule](operator-source-controls-spec.md#456--per-asset-clip-inout) with the owner. #456 explicitly requires UI design approval before code. Confirm whether the compact timecode group is the desired operator control.
2. Confirm the [#627 sign convention and bounds](operator-source-controls-spec.md#627--manual-zoom-guest-lip-sync-trim): positive delays guest audio; negative delays that guest's video. The user has already rejected automatic Zoom delivery correction; no further decision is needed on that point. The range is a proposed engineering bound until queue/latency tests support it.
3. Reconcile #668's previous unconditional-host sentence with the owner’s new per-source choice. Preserve the default for saved scenes. Do not implement a global host toggle.
4. Pin a Release SHA and an installed beta SHA for each hardware comparison. Record device, Zoom meeting epoch, enabled outputs, Program buffer already in effect, and audio routing. A file-only clap is insufficient evidence for live monitor sync.

## Implementation seams and falsification

| Issue | Smallest shipping slice and owner | Test that must go red if behavior is removed | Installed evidence |
|---|---|---|---|
| #627 | A focused core per-source timing policy/delay adapter, typed command and applied fact; shell control consumes that fact. Zoom engine continues to deliver raw media. | Two guests with timed transients: only selected guest changes by both signs; 0 is identical to baseline; epoch flush and mix-mode refusal. Delete the adapter or one sign and test fails. | Same-run Preview/Program/record/stream/monitor clap where available; applied/requested value; lost samples and underruns. |
| #592 | Instrument text lineage, then fix only the demonstrated metadata, key, or raster stage. | Rapid same-source facts and real secondary-line edit produce complete versions; remove atomic publish/stable guard and a captured text sequence fails. | Installed Preview/Program capture and recorded output during roster churn, with no `g` fragment or restart animation. |
| #681 | Persist global preference in focused settings type; pure resolution policy supplies Zoom subscription command at join; Sources reads actual fact. | Existing installs default 1080; 360/720/1080 requests; mid-meeting save does not re-key, next join does; Preview/Take cannot cause subscription churn. | One meeting across a preference save and next join, with current/pending labels and actual WxH@fps. |
| #456 | Per-asset trim model and editor plus a core playout range adapter used by actual decoder transport. | Decode starts at In, stops/loops at Out with video and PCM; delete core range application and test fails. Include FFmpeg fallback. | Installed cold/cued Take, loop and one-shot, on-air edit staged to next cue, record/stream clip ends. |
| #668 | Per-directed-source eligibility policy and independent holds; migrate old route mode to guest-only. | Two variants coexist; host can win only include-host; explicit exclude and person uniqueness win in both. Remove policy isolation and test fails. | Host UVC + Zoom host + guests; Preview source variants and Program Take, epoch reset. |

No new methods in `StudioViewModel.cs` beyond a minimal existing-property bridge if binding requires one. Put state machines and validation in focused types. Shell does not store frame or PCM queues. `MediaCore.cpp` gains no new ingest path. The renderer and outputs consume the same timed source facts and transport; projection repairs dropped facts without polling as the primary update mechanism.

## Proof sequence for each PR

1. Add a failing policy or transport test at the actual seam. For visual #592, add trace/capture instrumentation first and attach the frame sequence that localizes the fault.
2. Implement the smallest end-to-end consumer, including operator affordance and applied-state readout. One issue per PR, with its number in the title.
3. Run focused tests, stub-core and native Release gates, and relevant installed integration. A self-skipped hardware test is `MISSING_EVIDENCE`.
4. Report command lines, SHA, artifacts, median/spread/sign for timing, frame deadline misses, underruns and lost samples. Distinguish simulation from installed real-meeting results.
5. Check unrelated paths that share the source: Zoom mute versus mixer mute, Preview versus Program, record/stream, source departure/epoch, and old saved scene or asset migration. Revert a buffering change if continuity regresses.
6. Put evidence in the PR’s first comment and update its linked issue. Merge only after gates pass; the issue stays open for any stated installed acceptance still missing.

## Integration order and dependency notes

The owner’s [BACKLOG](BACKLOG.md) rank governs selection and the three-item WIP cap. Within an issue, tests precede implementation. #592 tracing can be performed independently of #627 and #681. #681 preference must feed the existing Zoom subscription policy, not the input-row projection alone. #627 timing must be before destination fan-out; #652 live monitor latency remains separately measurable. #456 needs its design gate before its code, and must not be used to mask #449 cold Take. #668 changes directed sources while preserving the already shipped speaker-floor ledger. After each issue is merged, rerun the cross-feature installed scenario in the spec before claiming the batch accepted.

## Operator review script

Use a real test meeting and an installed build. Start with host UVC, host Zoom tile, and two guests; open recording and a test stream if available. Confirm that guest-only Active Speaker holds a guest while the host talks, while include-host can choose the host, and no person appears twice. Adjust one guest’s lip-sync trim in each direction and reset it; the other guest must not shift. Edit a participant’s secondary lower-third line during Program and watch for fragments or animation restart. Change the camera ceiling during the meeting: the current request and actual Format remain honest; after rejoining, the new ceiling becomes active. Cue a clip with In/Out, Take it, loop it, edit its range while it is on Program, and verify the current play stays unchanged until the next cue. Save the Program recording and report any visible or audible mismatch with its exact time. If a guest, stream, or hardware endpoint is unavailable, record the missing observation rather than treating the scenario as passed.
