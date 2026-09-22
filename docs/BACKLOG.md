# CoreVideo Pro — ranked backlog

**This is the only ordered list of work.** Status and detailed evidence live on
linked GitHub issues. Rules: [AGENTS.md](../AGENTS.md).

Owner-approved order updated 2026-09-22: finish #535's remaining media/capture
audio and adapter lifecycle work, then SRT/NDI hardening (#538). Owner accepted
live lip sync (#579) and first Takes (#555) on beta `db9e703`. #513 validation
remains deferred; #582 now has a Zoom-recommended post-processing setting to validate.

## How to use this file

1. Work the first unblocked **Now** item. Maximum 3 in flight.
2. New findings get a GitHub issue and an unranked row; the owner ranks them.
3. Close only after a merged fix meets acceptance, or an explicit owner disposition.
4. A fix awaiting live acceptance is not Done. A non-reproducing report is not a confirmed current defect.
5. Build/run guidance lives in `CLAUDE.md`; design documents are not work queues.

Priority remains show survival, diagnosability, real-show usability, then external
installation. Build one source bus before adding more ingest paths. MXL stays parked.

## Now — existing priority order

| Order | Issue | Remaining work / next evidence |
|---|---|---|
| 1 | [#535](https://github.com/iamfatness/CoreVideoPro/issues/535) | **Finish the source bus.** Video and media transport slices are shipped; Zoom PCM integration shipped in #577 and live sync is owner-accepted. Media/capture PCM migration shipped in #583 / beta `6782b09` (1,167 native tests and real media recording checked). Remaining afterward: adapter lifecycle and retirement of the old interfaces. Capture dropout liveness is tracked by #562. |

## Deferred by owner

| Issue | Status |
|---|---|
| [#513](https://github.com/iamfatness/CoreVideoPro/issues/513) | Owner reports an overnight run with no recurrence and will validate later. Pooling/teardown fixes and the earlier 151-minute soak remain evidence of reduced exposure, not root-cause closure. Not a blocker for #535/#555. |

## Next — existing priority order

| Order | Issue | Remaining work |
|---|---|---|
| 1 | [#538](https://github.com/iamfatness/CoreVideoPro/issues/538) | SRT send + NDI send hardening and real endpoint acceptance. Existing transport code does not establish the full edge-I/O matrix. |
| 2 | [#536](https://github.com/iamfatness/CoreVideoPro/issues/536) | SRT ingest decoding to real pixels/PCM on the source bus, followed by the 30+ minute contribution soak. Depends on #535. |
| 3 | [#423](https://github.com/iamfatness/CoreVideoPro/issues/423) | Signing + first external install. The current beta is still unsigned; certificate/service onboarding remains. |
| 4 | [#449](https://github.com/iamfatness/CoreVideoPro/issues/449) | **Cold Take acceptance, reopened.** Warm cue transport shipped (#492/#567); restart-from-zero semantics are decided. Holding the outgoing picture until a never-cued clip's first frame (bounded by timeout) is not established by the cued-path oracle or late-frame playback test. Verify/fix this remaining step; no new owner ruling is required for the restart semantics. |

## Unranked — fixes awaiting acceptance / new findings

| Issue | Remaining work |
|---|---|
| [#582](https://github.com/iamfatness/CoreVideoPro/issues/582) | **Confirmed by owner in Preview and multiview** on beta `4dfe476`, including a participant who was not active speaker. Disabling SDK render post-processing in #585 did not eliminate the flash. Measured isolated incoming frames shift in either mean-luma direction with a similar range-like transform. Next: correlate the SDK's per-frame range flag with these excursions. Root cause remains unproven. |
| [#568](https://github.com/iamfatness/CoreVideoPro/issues/568) | Stale retry fencing is merged in #566 and regression-tested. Still needs the installed operator sequence: AV1 refusal → explicit HEVC retry without app restart, with genuine new failures visible and sibling outputs uninterrupted. |
| [#581](https://github.com/iamfatness/CoreVideoPro/issues/581) | Zoom window appears slower than Program. Producer/Windows serving cadence was approximately 60 fps; Zoom self-view versus receive statistics and real presentation timing await owner validation. No confirmed fix; source-bus work continues. |
| [#569](https://github.com/iamfatness/CoreVideoPro/issues/569) | Preparing/offline symptom repaired in #566; YouTube LIVE/Excellent and moving public playback verified, including restart and a 30m16s run. Retain for the remaining receiver acceptance and UI distinction between sending and verified playback. Player counted 104 dropped / 108,323 frames; cause not attributed to CVP. Do not describe this as still stuck Preparing, or as lossless end-to-end. |
| [#575](https://github.com/iamfatness/CoreVideoPro/issues/575) | Scheduled staging smoke cannot execute because required Actions secrets are missing. Existing configuration gap; application CI passed. Supply secrets through the appropriate secure configuration path, then rerun staging checks. |

## Papercuts and deferred workload gates — existing order

| Issue | Remaining work |
|---|---|
| [#562](https://github.com/iamfatness/CoreVideoPro/issues/562) | Capture dropout policy needs adapter liveness via `signalPresent`; frame cadence alone is not a valid signal for static content. |
| [#551](https://github.com/iamfatness/CoreVideoPro/issues/551) | Async ISO writer status still omits live `framesWritten` / `bytesWritten` updates; files record correctly. Confirmed in `refreshIsoStreams` on main. |
| [#456](https://github.com/iamfatness/CoreVideoPro/issues/456) | Media In/Out points; UI design still needed. |
| [#530](https://github.com/iamfatness/CoreVideoPro/issues/530) | Control manifest still advertises `programPreview`; implementation spelling is `program-preview`. Fix alias/validation and reject invalid modes. |
| [#521](https://github.com/iamfatness/CoreVideoPro/issues/521) | GPU-direct HEVC is shipped in #566. AV1 remains explicitly refused pending [#565](https://github.com/iamfatness/CoreVideoPro/issues/565). Raw fallback at real time and recording/ISO on the encoder seam remain; neither is covered by HEVC stream acceptance. |
| [#517](https://github.com/iamfatness/CoreVideoPro/issues/517) | Participant export-device reuse and the Program readiness fix shipped (#566/#574). Full 16-guest / 1080p-input render-budget acceptance remains. The clean eight-feed soak used adaptive 320x180 inputs, not eight 1080p feeds. |
| [#519](https://github.com/iamfatness/CoreVideoPro/issues/519) | RTMP `bytesSent` still uses `estimatedFrameBytes`; `latencyMs` remains hard-coded to 2100 on main. Replace with measured/explicitly unavailable values. |
| [#509](https://github.com/iamfatness/CoreVideoPro/issues/509) | Roster-change UI work exceeded 16.7 ms in measured sessions; several projections contribute. Earlier throttling reduced cost, but acceptance for the remaining rebuild work is not documented. |
| [#508](https://github.com/iamfatness/CoreVideoPro/issues/508) | **Watch item per owner report:** multiview label/click mismatch after unassign has not recurred. Capture synchronized overlay/texture geometry if a persistent mismatch returns; no speculative geometry fix. |
| [#507](https://github.com/iamfatness/CoreVideoPro/issues/507) | Debug text on operator surfaces and transport layout movement: no closing fix/acceptance identified in this audit. |

## Later — parked

| Issue | Item |
|---|---|
| [#539](https://github.com/iamfatness/CoreVideoPro/issues/539) | MXL / ZoomISO Cloud / Kubernetes; remains parked until the source bus and plant I/O exist. |

## Done — reconciled merged fixes

These rows record specific fixes, not blanket production or fleet reliability.

| Issue | Merged fix / acceptance |
|---|---|
| [#579](https://github.com/iamfatness/CoreVideoPro/issues/579), [#578](https://github.com/iamfatness/CoreVideoPro/issues/578) | Clap harness and source-video timing fixes shipped in #577; 32 recorded paired events within 41 ms. Owner reports good live lip sync in their overnight test (2026-09-22). |
| [#555](https://github.com/iamfatness/CoreVideoPro/issues/555) | #554 fix, first-Take checks, and resolution-ramp coverage in #577; owner reports no first-Take oddness in their test (2026-09-22). |
| [#570](https://github.com/iamfatness/CoreVideoPro/issues/570) | Async MFT drain/shutdown in #566. Hardware cycles, installed restarts, sustained run and normal drain/stop passed. |
| [#571](https://github.com/iamfatness/CoreVideoPro/issues/571) | COM lifetime through WIC/module teardown in #566. Reproducing regression and installed orderly shutdown checks passed. |
| [#572](https://github.com/iamfatness/CoreVideoPro/issues/572) | Unobserved startup remains `starting` in #566. Lifecycle regressions and repeated installed starts passed. |
| [#573](https://github.com/iamfatness/CoreVideoPro/issues/573) | Phase-stable two-clip summing fixture in #566; assertion retained. Repeated local runs and merged-main TSan passed. |
| [#574](https://github.com/iamfatness/CoreVideoPro/issues/574) | GPU readiness moved before publication in #566. Hardware pixel tests and 30m16s installed candidate soak passed with zero new CVP delivery failures. Workload limits remain under #517/#569. |
| [#529](https://github.com/iamfatness/CoreVideoPro/issues/529), [#533](https://github.com/iamfatness/CoreVideoPro/issues/533) | ISO drops/timeline fixes merged and validated in the earlier 24-minute six-ISO recording; shipped September 18. |
| [#516](https://github.com/iamfatness/CoreVideoPro/issues/516), [#526](https://github.com/iamfatness/CoreVideoPro/issues/526), [#518](https://github.com/iamfatness/CoreVideoPro/issues/518) | Earlier render-stall, Program-buffer startup/busy-loop, and audio-log fixes merged and validated; shipped September 18. Later distinct defects retain their own issues. |
