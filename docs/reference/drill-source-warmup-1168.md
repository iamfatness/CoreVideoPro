# Remote source warmup before ISO selection — #1168

Observed: the c9ab4c55 drill joined, assigned eight remotes and immediately started recording. The first seven sources got writers; the eighth did not. Later all eight were present, selected, assigned and delivering 1920x1080, but the recording retained seven writers. Current shell intentionally resolves eligible-present sources at Record. Startup readiness ordering is the working hypothesis, not yet a proven product cause.

Specification: before Record or Stream starts, every declared remote must deliver current 1920x1080 frames. Require the existing 30-second stable interval, reset it on any missing/stale/sub-resolution source, and fail after 300 seconds with source-specific reasons. Save pre-record observations separately. Local-only runs have no remote warmup. Post-start all-ISO checks and exact per-frame judges remain unchanged; never silently omit a writer.

Plan: add a pure remote readiness function and pre-record wait; test missing last participant, lower resolution, stale evidence, missing snapshot and all-ready transition; retain the original failed recording. Rehearse the same frozen package with the revised harness and require eight writer identities before measurement. If the eighth is still absent, retain that result and investigate product selection rather than repeating until green.

Acceptance: no output starts during incomplete remote readiness; the declared writer set is verified after start. Only actual successful eight-writer evidence can close the omission report. No physical webcam/full-workload acceptance is implied by a motion-clip run.
