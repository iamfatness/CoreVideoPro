# Remote source warmup before ISO selection — #1168

Observed: the c9ab4c55 drill joined, assigned eight remotes and immediately started recording. The first seven sources got writers; the eighth did not. Later all eight were present, selected, assigned and delivering 1920x1080, but the recording retained seven writers. Current shell intentionally resolves eligible-present sources at Record. Startup readiness ordering is the working hypothesis, not yet a proven product cause.

Specification: before Record or Stream starts, every declared remote must deliver current 1920x1080 frames. Require the existing 30-second stable interval, reset it on any missing/stale/sub-resolution source, and fail after 300 seconds with source-specific reasons. Save pre-record observations separately. Local-only runs have no remote warmup. Post-start all-ISO checks and exact per-frame judges remain unchanged; never silently omit a writer.

Plan: add a pure remote readiness function and pre-record wait; test missing last participant, lower resolution, stale evidence, missing snapshot and all-ready transition; retain the original failed recording. Rehearse the same frozen package with the revised harness and require eight writer identities before measurement. If the eighth is still absent, retain that result and investigate product selection rather than repeating until green.

Acceptance: no output starts during incomplete remote readiness; the declared writer set is verified after start. Only actual successful eight-writer evidence can close the omission report. No physical webcam/full-workload acceptance is implied by a motion-clip run.

## Reassignment investigation after failed prewarm

The 96ec361b live trial reached 1080p on all eight sources for 30 seconds before Record and still omitted the eighth writer. Native isoSourceIds itself contains seven, so the first missing boundary is at or before shell target delivery. The saved slot 9 referenced an unavailable old participant. Control API input.assign sets Kind and ParticipantId; same-kind changes skip Kind refresh and ParticipantId does not recompute cached IsSourceMissing. The unified source picker does refresh it. This is a specific source lead requiring a red regression, not yet proof from a patched live build.

Revised implementation plan: construct eight live participants with the eighth slot initially saved-unavailable, refresh once, then perform the same Kind/ParticipantId assignments as the API without roster churn. Assert all eight intended ISO targets and current availability; retain the old failure. Refresh availability for explicit participant/capture assignment using retained roster/device/media lists, without a per-snapshot collection rebuild. Cover missing-to-present and present-to-missing transitions, capture and media kinds, and unchanged saved intent. Rebuild the shell against the frozen native candidate and repeat the actual API assignment plus recording workload. Warmup remains useful defense but is not claimed as the causal fix.

Done requires all eight actual writers in the packaged regression. Full 30-minute output delivery and physical webcam acceptance remain separate.

Deterministic evidence: the eight-source coordinator regression fails on the old code because the replacement retains IsSourceMissing=true. Direct assignment now batches the model change, refreshes availability from retained source lists, then publishes the changed callback. The regression verifies all eight ISO targets at that callback, without any later roster/UI action. A capture replacement test covers missing→present→missing and forbids silent substitution. Fifty adjacent assignment/coordinator tests pass. Packaged eight-writer verification remains pending.
