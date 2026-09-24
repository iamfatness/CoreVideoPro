# CoreVideo Pro — ranked backlog

**This is the only ordered list of work.** Status and detailed evidence live on
linked GitHub issues. Rules: [AGENTS.md](../AGENTS.md).

Owner-approved order updated 2026-09-22: finish #535's remaining media/capture
audio and adapter lifecycle work, then SRT/NDI hardening (#538). Owner accepted
live lip sync (#579) and first Takes (#555) on beta `db9e703`. #513 validation
remains deferred; #582's content-aware range correction is in live validation.

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
| 1 | [#535](https://github.com/iamfatness/CoreVideoPro/issues/535) | **Finish the source bus.** Video and media transport slices are shipped. **Slice 3b (media TRANSPORT into the core) SHIPPED — merged 2026-09-21 in [#567](https://github.com/iamfatness/CoreVideoPro/pull/567)**, spec `docs/superpowers/specs/2026-09-20-source-bus-slice3b-media-source-state-design.md`: the `layers` argument is gone, a media asset is ONE core source (`media:<assetId>` / `background:<assetId>`) owning its decoder, clock and transport state (cued/live/paused/ended), fed a DESIRED SET computed at COMMAND time from both scene graphs (`MediaCore::syncMediaTransportsDesired`), with `core::MediaTransports` as the owner and the pure `MediaTransportPolicy.h` as the transition table. Enters Program → roll from 0 with audio; cued in Preview → poster at 0; a cued clip entering Program RESUMES the same decoder (this DELETED the #449 cue hand-off, `MediaCueHandoff`/`adoptCuedDecoders`, along with `OwnedMediaFrameSource`, `MediaGoLiveLedger`, the `media:<id>:live:<n>` go-live generation, `BuildSceneMediaPlaybackKey` and the `preview:media:<id>` poster namespace); staying on Program across a Take does nothing; loops never pause or restart. `Ended` is decoder evidence (`IMediaVideoPrefetch::mediaEnded`, MF-only, never for a loop) with a 6 s backstop and in-place recovery; `Release` carries a 750 ms grace, and a source inside it publishes `onProgram`/`onPreview` false (read-side only). Wire: one-shot `set-media-transport {mediaAssetId, action}` (a GESTURE, never re-sent per sync), `set-media-playback` is selection only, new `mediaSources[]` snapshot node and the `idle\|cued\|live\|paused\|ended\|unavailable` vocabulary on `mediaPlayback`. Branch gates at merge: full Windows dev suite 1113/0, stub gate 100%, `dotnet test` MediaCore 2232/0 + Control 74/0 + WinUI 1519/0 + Release x64 0 errors, `validate-multiview.mjs` and `validate-tiles.mjs` PASS, `zoom-gap-hold-ab.py --label s3b-hold` 248 frames luma 187.5–204.7 with take verdict `cut`, show drill PASSED (100% delivery, source→render p50 26.9 ms / p99 37.7 ms), and the new `scripts/qa/media-take-ab.py` PASS with its `--skip-cue` control correctly FAILING. **Not gated live:** no run against a real Zoom meeting or the real WinUI app; the 750 ms grace is reasoned, not measured against the real spine cadence; only the MF adapter implements `mediaEnded()`, so stub/mac decoders wait for the 6 s backstop. Zoom PCM integration shipped in #577 and live sync is owner-accepted. Media/capture PCM migration shipped in #583 / beta `6782b09` (1,167 native tests and real media recording checked) — that is what moved media audio off 3b's direct `popAudio` pop and onto the bus via `SourceAudioIngress`. Remaining afterward: adapter lifecycle and retirement of the old interfaces. Capture dropout liveness is tracked by #562. |
| 2 | [#601](https://github.com/iamfatness/CoreVideoPro/issues/601) | Owner requested 2026-09-24: preserve the configured frame rate. Explicit encoder rate control and measured conformance at 4.5/6/10 Mbps, with 2 Mbps as a stress check. Full-range noise reaches QP 51; the original claim that bitrate never reaches the encoder is not established. Hard saturation remains distinct from normal bitrate control. Evidence: [investigation](issue-601-rate-control-investigation.md). Await review and live acceptance. |

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
| 4 | [#449](https://github.com/iamfatness/CoreVideoPro/issues/449) | **Cold Take acceptance, reopened.** The **MEDIA half of #449 is CLOSED by #535 slice 3b** (merged 2026-09-21, [#567](https://github.com/iamfatness/CoreVideoPro/pull/567)): the core owns the transport, go-live is decided from scene membership at command time ("enters Program → roll from 0", "stays on Program across a Take → nothing"), and a cued clip entering Program resumes its own warm decoder in place — which is why the cue→Program hand-off (#492) could be deleted rather than moved. Restart-from-zero semantics are decided. **Step 1 remains OPEN:** holding the outgoing picture on a COLD cut — a clip cut to Program that was NEVER cued still cold-starts into the warming slate — is not established by the cued-path oracle or the late-frame playback test. `scripts/qa/media-take-ab.py --skip-cue` is the falsification control that measures exactly that gap (it fails by design; a run that passes with `--skip-cue` is judging nothing). Verify/fix this remaining step; no new owner ruling is required for the restart semantics. |

## Unranked — fixes awaiting acceptance / new findings

| Issue | Remaining work |
|---|---|
| [#599](https://github.com/iamfatness/CoreVideoPro/issues/599) | After the #597 stream stall, the Zoom SDK helper and video frames recovered but the Zoom Meeting panel remained hidden on display 2. A targeted window restore made the existing window visible without restarting the meeting; explain the hide/mini-window transition and provide a reliable operator recovery path. Causality with #597 is unproven. |
| [#597](https://github.com/iamfatness/CoreVideoPro/issues/597) | **Backpressure slice 1 shipped on `feat/stream-backpressure`; owner acceptance and merge remain.** The ~20 s stall was the CORE starving, not the UI blocking (the `perf.log` gap carried a normal sample-COUNTER delta), amplified by eight hardware-encoder rebuilds in 20 s. Shipped: a buffered-age signal on the compressed-video queue; Lever A (an input divisor at the compositor's encoder-texture export, measured to halve egress) and Lever B (GOP-tail discard, safe because parameter sets are in band); a restart floor over BOTH restart authorities (the adapter re-opened its own transport on a 1 s rung, never consulting the supervisor ladder); the queue-overflow path now discards instead of failing the sender into a restart storm; a per-sender `backpressure` snapshot node; and a live slow-sink/burst acceptance gate. Not proven: a finite soak only, ladder rung 1 pinned by unit tests alone, and the HEVC overflow branch has never fired (#607). Slice 2 remains: the egress-based health signal and the phantom-fault fix. Defects found and deferred: #601 (the encoder ignores its configured bitrate — possibly the real trigger, which backpressure now masks), #602, #603, #604, #605, #606, #607. |
| [#590](https://github.com/iamfatness/CoreVideoPro/issues/590) | Simplify and prune Sources for live operation. Audit accumulated controls, remove or relocate redundant and misleading UI, and verify source assignment with realistic inputs. Coordinate with #505 and #588. |
| [#591](https://github.com/iamfatness/CoreVideoPro/issues/591) | Rework starter Scenes and scene creation. Owner sees the same source repeated in default scenes; reproduce and fix that behavior, make common looks easy to build, and preserve saved custom scenes. Related: #451. |
| [#592](https://github.com/iamfatness/CoreVideoPro/issues/592) | Lower-third second line visibly refreshes as “Guest” and sometimes briefly shows lowercase “g”. Trace metadata, key-state, and rendering updates; fix the cause and verify stable installed output. |
| [#593](https://github.com/iamfatness/CoreVideoPro/issues/593) | Add per-source lower-third name and editable secondary line (title, organization, website, or other text) in Sources, reusing the existing DisplayName override and persisting the new field. |
| [#594](https://github.com/iamfatness/CoreVideoPro/issues/594) | Put Health and support tools inside Settings; replace the top-level Health button with Settings and remove the Diagnose label without losing diagnostic access. |
| [#595](https://github.com/iamfatness/CoreVideoPro/issues/595) | Hide OHG Show from default navigation until explicitly enabled in Settings. Preserve existing OHG configuration; #450 remains the broader redesign. |
| [#565](https://github.com/iamfatness/CoreVideoPro/issues/565) | AV1 streaming remains explicitly refused: the GPU encoder emitted near-empty 1080p60 access units. Fix and verify a playable packaged receiver-side stream before enabling AV1; never silently substitute a codec. |
| [#588](https://github.com/iamfatness/CoreVideoPro/issues/588) | Make local display/window capture an obvious, reliable source-to-Preview/Program workflow. WGC capture and generic device routing exist, but the operator path is fragmented and lacks current installed-beta end-to-end acceptance. Distinguish this from incoming Zoom screen share; verify lifecycle, failure status, frame cadence, and audio labeling. |
| [#582](https://github.com/iamfatness/CoreVideoPro/issues/582) | **Confirmed by owner in Preview and multiview** on beta `4dfe476`, including a non-active participant. Live callback evidence found the SDK's limited-range flag stayed true across range-like frame changes while raw out-of-range pixels collapsed and returned. Default-on correction in #586 yielded 0 isolated luma jumps in 88,159 frames over five minutes across 8 feeds, with zero subscription churn; the owner reports the corrected picture stable so far. CI, longer visual acceptance, and shipping remain. |
| [#587](https://github.com/iamfatness/CoreVideoPro/issues/587) | First Join after changing the Zoom meeting URL used the stale placeholder meeting number; a second click used the visible URL. Fix the URL binding/command boundary and verify first-click Join uses the visible meeting. |
| [#568](https://github.com/iamfatness/CoreVideoPro/issues/568) | Stale retry fencing is merged in #566 and regression-tested. Still needs the installed operator sequence: AV1 refusal → explicit HEVC retry without app restart, with genuine new failures visible and sibling outputs uninterrupted. |
| [#581](https://github.com/iamfatness/CoreVideoPro/issues/581) | Zoom window appears slower than Program. Producer/Windows serving cadence was approximately 60 fps; Zoom self-view versus receive statistics and real presentation timing await owner validation. No confirmed fix; source-bus work continues. |
| [#569](https://github.com/iamfatness/CoreVideoPro/issues/569) | Owner reports H.265 streaming still fails in current testing; exact installed build and symptom await capture. Earlier Preparing/offline symptom was repaired in #566, with YouTube LIVE/Excellent and moving public playback verified through a 30m16s run. Reproduce the new report and establish repeatable receiver-side video/audio acceptance; distinguish sending from verified playback. Do not assume the old symptom recurred. |
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
