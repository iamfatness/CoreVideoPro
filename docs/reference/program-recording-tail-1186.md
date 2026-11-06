# Program recording final picture

Issue: [#1186](https://github.com/iamfatness/CoreVideoPro/issues/1186). Ordering lives in BACKLOG.

The retained November 6 recording contains one duplicate final Program identity, matching the writer's explicit tail sample. The anchored measured interval is continuous, but the unchanged whole-file judge fails. This is separate from intentional post-output clock cessation (#941).

## Contract

Every submitted Program picture appears once in the finalized file. Reconciliation preserves all accepted audio and video, the timeline of genuine gaps and the common A/V endpoint. Do not cut audio, invent another picture, downgrade quality or relax the judge. Existing source-stall ISO holds and their counted exception remain unchanged.

Program already enables variable sample durations and keeps its last real sample pending. At close, if audio outlasts that picture, extend this pending sample's duration to the audio endpoint before submitting it. Validate its timestamp and positive duration; propagate any failure through existing finalization warnings. Do not add a queue, delay ordinary submission further, or change earlier sample timestamps. Restrict this policy to Program; leave ISO reconciliation untouched in this slice. If the pending sample is unexpectedly absent, report an explicit inability to reconcile rather than silently dropping audio or claiming a pass.

## Execution and evidence

First reproduce using the actual MF writer with identifiable NV12 pictures and an audio tail. Decode the finalized picture count/identity and audio sample endpoint. The old implementation must fail the no-extra-picture assertion. Cover audio-longer, equal and video-longer endpoints; retain the existing Program-gap regression and ISO source-hold regressions. Test the production hardware path and a supported software path without mistaking an unavailable encoder for a pass.

After focused and native checks, produce a short packaged recording with deterministic pictures, finalize and run the unchanged whole-file judge. Retain failed artifacts and independent track-end evidence. A finite short check qualifies this close-time behavior only; it cannot close the broader 30-minute OS-camera and receiver-latency gates. Hosted checks must pass on the reviewed commit before merge. Reforecast after at most 90 active minutes.
