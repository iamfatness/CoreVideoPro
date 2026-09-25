# CoreVideo Pro — ranked backlog

**This is the only ordered list of work.** Status and detailed evidence live on
linked GitHub issues. Rules: [AGENTS.md](../AGENTS.md).

Owner-approved order updated 2026-09-24: finish #535 adapter lifecycle, then make
the wire honest (#616 with #619/#620), then constructed capabilities (#617), then
a Windows production-native CI compile lane (#618), then SRT/NDI edge I/O (#538).
Owner accepted live lip sync (#579) and first Takes (#555) on beta `db9e703`.
#513 validation remains deferred; #582 range correction is in live validation.

Architecture review pin: commit `6f4f025` (2026-09-24). Issues #616–#622 are that
review. Live incidents (#615, #608, #624, #597 acceptance) stay unranked and may
preempt this list on a show night.

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
| 1 | [#535](https://github.com/iamfatness/CoreVideoPro/issues/535) | **Finish the source bus.** Video and media transport slices are shipped. **Slice 3b (media TRANSPORT into the core) SHIPPED — merged 2026-09-21 in [#567](https://github.com/iamfatness/CoreVideoPro/pull/567)**. Capture session control now lives on `ICaptureDeviceLifecycle`. Capture adapters push frames and embedded PCM into a mailbox (`postVideo` / `postAudio`). `deliverVideo` and `deliverAudio` only flush that mailbox. `snapshotVideo` and `snapshotAudio` are gone. Remaining: retire `IZoomCaptureSource` and the media decoder face of `IMediaFrameSource`. Capture dropout liveness is #562. Do not start #616 while this is open unless the owner re-ranks. |
| 2 | [#601](https://github.com/iamfatness/CoreVideoPro/issues/601) | Owner requested 2026-09-24: preserve the configured frame rate. Explicit encoder rate control and measured conformance at 4.5/6/10 Mbps, with 2 Mbps as a stress check. Full-range noise reaches QP 51; the original claim that bitrate never reaches the encoder is not established. Hard saturation remains distinct from normal bitrate control. Evidence: [investigation](issue-601-rate-control-investigation.md). Await live acceptance. |

## Deferred by owner

| Issue | Status |
|---|---|
| [#513](https://github.com/iamfatness/CoreVideoPro/issues/513) | Owner reports an overnight run with no recurrence and will validate later. Pooling/teardown fixes and the earlier 151-minute soak remain evidence of reduced exposure, not root-cause closure. Not a blocker for #535. |

## Next — control plane, then edge I/O

| Order | Issue | Remaining work |
|---|---|---|
| 1 | [#616](https://github.com/iamfatness/CoreVideoPro/issues/616) | **Make the wire honest.** One command manifest from the live C++ dispatcher; reject unknown batched commands; implement or delete the three production-sync no-ops. Ride [#619](https://github.com/iamfatness/CoreVideoPro/issues/619) (Program texture events on the preview queue) and [#620](https://github.com/iamfatness/CoreVideoPro/issues/620) (bound the high-priority response lane) in the same change. Not #530. |
| 2 | [#617](https://github.com/iamfatness/CoreVideoPro/issues/617) | Capabilities describe constructed adapters, not compile flags. Required before #538/#536 admission. Do not advertise DeckLink/AJA until [#537](https://github.com/iamfatness/CoreVideoPro/issues/537) has pixels. |
| 3 | [#618](https://github.com/iamfatness/CoreVideoPro/issues/618) | Windows CI production-native compile lane (`COREVIDEO_STUB=OFF` + the flags the beta ships). Compile-only; no 60 fps soak on the runner. Not #575. |
| 4 | [#622](https://github.com/iamfatness/CoreVideoPro/issues/622) | Single-slot sync plus UI-thread snapshot apply can drop a fire-and-forget command. After the wire is honest. Not #509. |
| 5 | [#621](https://github.com/iamfatness/CoreVideoPro/issues/621) | One generated observation model; typed snapshot, redacted qualification JSON, and ControlState are views of it. Lets #610/#519/#551 stop growing a fourth projection. |
| 6 | [#538](https://github.com/iamfatness/CoreVideoPro/issues/538) | SRT send + NDI send hardening and real endpoint acceptance. NDI stays in-process this cycle; classify it as process-fatal in #617. |
| 7 | [#536](https://github.com/iamfatness/CoreVideoPro/issues/536) | SRT ingest decoding to real pixels/PCM on the source bus, then the 30+ minute contribution soak. Depends on #535 and honest #617 capabilities. |
| 8 | [#423](https://github.com/iamfatness/CoreVideoPro/issues/423) | Signing + first external install. Current beta is still unsigned. |
| 9 | [#449](https://github.com/iamfatness/CoreVideoPro/issues/449) | **Cold Take acceptance.** Media half closed by #535 slice 3b. Step 1 open: a never-cued clip cut to Program still cold-starts into the warming slate. `scripts/qa/media-take-ab.py --skip-cue` is the falsification control. |

## Unranked — show survival and operator findings

These can preempt Next on a show night. They do not replace #535 as Now until the owner says so.

| Issue | Remaining work |
|---|---|
| [#615](https://github.com/iamfatness/CoreVideoPro/issues/615) | RTMP packet amplification is measured and coalesced. Encoder frames that never reached the shed counter are retained. The original PC-wide outage and full-show continuity still need fleet acceptance. |
| [#608](https://github.com/iamfatness/CoreVideoPro/issues/608) | Audio drops out for remaining participants when others disconnect; selecting their source restores it. |
| [#624](https://github.com/iamfatness/CoreVideoPro/issues/624) | Recover a subscribed Zoom video feed that stops advancing in Tiles. |
| [#597](https://github.com/iamfatness/CoreVideoPro/issues/597) | **Backpressure slice 1 shipped on `feat/stream-backpressure`; owner acceptance and merge remain.** Slice 2: egress-based health signal and phantom-fault fix. Deferred from that slice: #601 (encoder ignores configured bitrate), #602, #603, #604 (`stopFfmpegProcess` never kills the child), #605, #606, #607. |
| [#599](https://github.com/iamfatness/CoreVideoPro/issues/599) | After the #597 stream stall, Zoom Meeting panel stayed hidden on display 2. Targeted restore worked; need a reliable operator path. Causality with #597 unproven. |
| [#610](https://github.com/iamfatness/CoreVideoPro/issues/610) | Operator-facing degraded-stream readout. Implement on the #621 model; do not hand-write a fourth snapshot. |
| [#590](https://github.com/iamfatness/CoreVideoPro/issues/590) | Simplify and prune Sources for live operation. Coordinate with #505 and #588. |
| [#591](https://github.com/iamfatness/CoreVideoPro/issues/591) | Rework starter Scenes and scene creation; owner sees the same source repeated in defaults. |
| [#592](https://github.com/iamfatness/CoreVideoPro/issues/592) | Lower-third second line refreshes as “Guest” / brief lowercase “g”. |
| [#593](https://github.com/iamfatness/CoreVideoPro/issues/593) | Persist per-source lower-third name and editable secondary line. |
| [#594](https://github.com/iamfatness/CoreVideoPro/issues/594) | Move Health into Settings; replace Diagnose label. |
| [#595](https://github.com/iamfatness/CoreVideoPro/issues/595) | Hide OHG Show until explicitly enabled in Settings. |
| [#565](https://github.com/iamfatness/CoreVideoPro/issues/565) | AV1 streaming explicitly refused until a playable packaged receiver-side stream exists. |
| [#588](https://github.com/iamfatness/CoreVideoPro/issues/588) | Local display/window capture as a first-class Program source. |
| [#582](https://github.com/iamfatness/CoreVideoPro/issues/582) | Speaker-change brightness flashes. Content-aware range correction in live validation; CI and shipping remain. |
| [#587](https://github.com/iamfatness/CoreVideoPro/issues/587) | First Join after editing the meeting URL used the stale placeholder number. |
| [#568](https://github.com/iamfatness/CoreVideoPro/issues/568) | Installed operator sequence: AV1 refusal → explicit HEVC retry without app restart. |
| [#581](https://github.com/iamfatness/CoreVideoPro/issues/581) | Zoom window appears slower than Program; owner validation pending. |
| [#569](https://github.com/iamfatness/CoreVideoPro/issues/569) | H.265 streaming needs repeatable installed receiver acceptance. |
| [#575](https://github.com/iamfatness/CoreVideoPro/issues/575) | Scheduled staging smoke blocked on missing Actions secrets. Not #618. |

## Papercuts and deferred workload gates — existing order

| Issue | Remaining work |
|---|---|
| [#562](https://github.com/iamfatness/CoreVideoPro/issues/562) | Capture dropout policy needs adapter liveness via `signalPresent`. |
| [#551](https://github.com/iamfatness/CoreVideoPro/issues/551) | Async ISO writer status still omits live `framesWritten` / `bytesWritten`. |
| [#456](https://github.com/iamfatness/CoreVideoPro/issues/456) | Media In/Out points; UI design still needed. |
| [#530](https://github.com/iamfatness/CoreVideoPro/issues/530) | Control manifest advertises `programPreview`; implementation spelling is `program-preview`. |
| [#521](https://github.com/iamfatness/CoreVideoPro/issues/521) | GPU-direct HEVC shipped in #566. AV1 refused pending #565. Raw fallback and recording/ISO on the encoder seam remain. |
| [#517](https://github.com/iamfatness/CoreVideoPro/issues/517) | Full 16-guest / 1080p-input render-budget acceptance remains. |
| [#519](https://github.com/iamfatness/CoreVideoPro/issues/519) | RTMP `bytesSent` still estimated; `latencyMs` hard-coded to 2100. |
| [#509](https://github.com/iamfatness/CoreVideoPro/issues/509) | 197 ms participant-rebuild stutter. Command-drop on the same apply graph is #622. |
| [#540](https://github.com/iamfatness/CoreVideoPro/issues/540) | Split ingest off MediaCore.cpp after F1 has one consumer. Not a god-object epic. |
| [#537](https://github.com/iamfatness/CoreVideoPro/issues/537) | DeckLink/AJA: live frames on the source bus (not probe-only). Parked for beta. |
| [#508](https://github.com/iamfatness/CoreVideoPro/issues/508) | Watch item: multiview label/click mismatch after unassign has not recurred. |
| [#507](https://github.com/iamfatness/CoreVideoPro/issues/507) | Debug text on operator surfaces shifts transport buttons. |

## Later — parked

| Issue | Item |
|---|---|
| [#539](https://github.com/iamfatness/CoreVideoPro/issues/539) | MXL / ZoomISO Cloud / Kubernetes; parked until the source bus and plant I/O exist. |

Parked from the 2026-09-24 architecture review (do not start from that document):
MediaCore/StudioViewModel extraction beyond #540, Program/Preview/Multiview thread
split, atomic native Take, NDI out of process, retiring React/`native-core` from CI,
enabling DeckLink/AJA before #537 has pixels.

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
