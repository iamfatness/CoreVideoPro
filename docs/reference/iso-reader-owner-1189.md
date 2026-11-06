# ISO reader source ownership

Issue: [#1189](https://github.com/iamfatness/CoreVideoPro/issues/1189). Ordering lives in BACKLOG.

The failed local rehearsal contains two epochs and roughly twice the requested ISO arrival cadence for every participant. The runtime captures one job per video stream reference but obtains its ring and cursor by participant only. An old auto source or a screen-share reference can schedule the same camera ring again, changing only its claimed source lifetime.

## Contract

An ISO demand names the participant's camera UUID. Only that UUID may schedule, map, consume and publish its ISO ring. Exactly one job uses its cursor per capture phase. Auto/share references must neither steal the ring nor duplicate its arrivals. Genuine camera replacement still invalidates old in-flight work, and camera absence must not fall back to share/latest pictures. Keep worker assignment, bounded capacity, Program independence, source quality and all picture judges unchanged.

## Execution and acceptance

Use the actual three-phase runtime consumer with a camera plus auto/share references for the same participant. Publish known pictures to its ring. The old consumer must fail the single-sequence/owner assertion; the corrected consumer admits each picture once. Cover camera absent with an alias present, camera return, and existing replacement fencing and independent worker tests. Update older synthetic test UUIDs to the actual production camera convention so they exercise this contract.

After native checks, run synthetic sources with auto-subscription left enabled, preserving the original triggering condition. Require one camera epoch per participant, exact offered-arrival/file identity correspondence and explicit recording finalization. Retain original traces and failures; never filter out a second owner in the judge. A shorter causal run validates this repair; full RC requires the separate 30-minute output workload with registered camera and bounded receiver latency.
