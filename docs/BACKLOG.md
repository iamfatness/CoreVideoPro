# CoreVideo Pro — ranked backlog

**This is the only ordered list of work.** Status and detailed evidence live on
linked GitHub issues. Rules: [AGENTS.md](../AGENTS.md).

Owner-approved order updated 2026-09-26: #616, #601, #617, #622, #530, #621, and #540 are closed.
The owner paused the remaining private-SDK CI work in #618 pending research.
Owner promoted and completed source-bus ingest extraction (#540). Next: SRT/NDI edge I/O (#538).
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

No issue is currently in Now. The first unblocked ranked item is #538 below.

## Deferred by owner

| Issue | Status |
|---|---|
| [#513](https://github.com/iamfatness/CoreVideoPro/issues/513) | Owner reports an overnight run with no recurrence and will validate later. Pooling/teardown fixes and the earlier 151-minute soak remain evidence of reduced exposure, not root-cause closure. Not a blocker for #535. |
| [#618](https://github.com/iamfatness/CoreVideoPro/issues/618) | Owner paused private Zoom SDK CI provisioning pending research. The non-stub Windows adapter compile lane passed in #643; real Zoom CI compile still reports `MISSING_EVIDENCE`. Keep open. |

## Next — control plane, then edge I/O

| Order | Issue | Remaining work |
|---|---|---|
| 1 | [#538](https://github.com/iamfatness/CoreVideoPro/issues/538) | SRT send + NDI send hardening and real endpoint acceptance. NDI stays in-process this cycle; classify it as process-fatal in #617. |
| 2 | [#536](https://github.com/iamfatness/CoreVideoPro/issues/536) | SRT ingest decoding to real pixels/PCM on the source bus, then the 30+ minute contribution soak. Depends on #535 and honest #617 capabilities. |
| 3 | [#423](https://github.com/iamfatness/CoreVideoPro/issues/423) | Signing + first external install. Current beta is still unsigned. |
| 4 | [#449](https://github.com/iamfatness/CoreVideoPro/issues/449) | **Cold Take acceptance.** Media half closed by #535 slice 3b. Step 1 open: a never-cued clip cut to Program still cold-starts into the warming slate. `scripts/qa/media-take-ab.py --skip-cue` is the falsification control. |

## Unranked — show survival and operator findings

These can preempt Next on a show night. They do not jump the ranked Now item until the owner says so.

| Issue | Remaining work |
|---|---|
| [#649](https://github.com/iamfatness/CoreVideoPro/issues/649) | RTMP demuxer test compares `const char*` literal addresses; use content comparison so Debug native gate is deterministic. |
| [#637](https://github.com/iamfatness/CoreVideoPro/issues/637) | Live Zoom validator requests subscriptions but never starts raw capture after the explicit Engine On gate. Set `startCapture` in the dedicated harness and repeat meeting media validation. |
| [#634](https://github.com/iamfatness/CoreVideoPro/issues/634) | Real test meeting join failed because the Zoom runtime appended passcode-query digits to the meeting number. Repair parsing and repeat the live join. |
| [#640](https://github.com/iamfatness/CoreVideoPro/issues/640) | Handshake CI test parses the entire JSONL output as one object; assert the first complete message is the handshake. |
| [#632](https://github.com/iamfatness/CoreVideoPro/issues/632) | Restore native CI after the constructed Zoom slate made the old empty-source snapshot assertion stale. |
| [#615](https://github.com/iamfatness/CoreVideoPro/issues/615) | RTMP packet amplification is measured and coalesced. Encoder frames that never reached the shed counter are retained (main `61e83ed1`). The original PC-wide outage and full-show continuity still need fleet acceptance. |
| [#627](https://github.com/iamfatness/CoreVideoPro/issues/627) | Per-participant audio delay. Capture devices already have `setAudioSyncOffset`; a Zoom guest does not. Headless clap on the Zoom-audio mailbox build measured one pipeline skew (median -39.0 ms, audio lags video), not a per-person offset. |
| [#608](https://github.com/iamfatness/CoreVideoPro/issues/608) | Audio drops out for remaining participants when others disconnect; selecting their source restores it. |
| [#624](https://github.com/iamfatness/CoreVideoPro/issues/624) | Recover a subscribed Zoom video feed that stops advancing in Tiles. |
| [#597](https://github.com/iamfatness/CoreVideoPro/issues/597) | **Backpressure slice 1 shipped on `feat/stream-backpressure`; owner acceptance and merge remain.** Slice 2: egress-based health signal and phantom-fault fix. Deferred from that slice: #601 (encoder ignores configured bitrate), #602, #603, #604 (`stopFfmpegProcess` never kills the child), #605, #606, #607. |
| [#599](https://github.com/iamfatness/CoreVideoPro/issues/599) | After the #597 stream stall, Zoom Meeting panel stayed hidden on display 2. Targeted restore worked; need a reliable operator path. Causality with #597 unproven. |
| [#610](https://github.com/iamfatness/CoreVideoPro/issues/610) | Operator-facing degraded-stream readout. Consume the generated observation model; do not hand-write a fourth snapshot. |
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
| [#521](https://github.com/iamfatness/CoreVideoPro/issues/521) | GPU-direct HEVC shipped in #566. AV1 refused pending #565. Raw fallback and recording/ISO on the encoder seam remain. |
| [#517](https://github.com/iamfatness/CoreVideoPro/issues/517) | Full 16-guest / 1080p-input render-budget acceptance remains. |
| [#519](https://github.com/iamfatness/CoreVideoPro/issues/519) | RTMP `bytesSent` still estimated; `latencyMs` hard-coded to 2100. |
| [#509](https://github.com/iamfatness/CoreVideoPro/issues/509) | 197 ms participant-rebuild stutter. Command-drop on the same apply graph is #622. |
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
| [#540](https://github.com/iamfatness/CoreVideoPro/issues/540) | Source video merge and per-source health projection extracted from MediaCore.cpp into SourceVideoIngress; frame order, Zoom roster, capture end, and still behavior covered by native tests. |
| [#621](https://github.com/iamfatness/CoreVideoPro/issues/621) | Generated observation model and per-view field policies in #648; typed, qualification, and ControlState views covered by contract and shell tests. |
| [#622](https://github.com/iamfatness/CoreVideoPro/issues/622) | Command syncs queue behind one bridge slot while empty polls coalesce; overlap/cancellation tests and live meeting Control API check passed in #645. |
| [#530](https://github.com/iamfatness/CoreVideoPro/issues/530) | Canonical and legacy dual-view modes now select ProgramPreview; invalid values are rejected. Live meeting Control API check passed in #646. |
| [#616](https://github.com/iamfatness/CoreVideoPro/issues/616) | Live dispatcher command admission, protocol failure handling, parity coverage, Program event drain, and bounded response lane merged in #631 and #639. |
| [#601](https://github.com/iamfatness/CoreVideoPro/issues/601) | Live meeting Program 1080p60 recording and local SRT receiver bitrate acceptance merged in #641. |
| [#617](https://github.com/iamfatness/CoreVideoPro/issues/617) | Constructed adapter capability reporting merged in #635. |
| [#579](https://github.com/iamfatness/CoreVideoPro/issues/579), [#578](https://github.com/iamfatness/CoreVideoPro/issues/578) | Clap harness and source-video timing fixes shipped in #577; 32 recorded paired events within 41 ms. Owner reports good live lip sync in their overnight test (2026-09-22). |
| [#555](https://github.com/iamfatness/CoreVideoPro/issues/555) | #554 fix, first-Take checks, and resolution-ramp coverage in #577; owner reports no first-Take oddness in their test (2026-09-22). |
| [#570](https://github.com/iamfatness/CoreVideoPro/issues/570) | Async MFT drain/shutdown in #566. Hardware cycles, installed restarts, sustained run and normal drain/stop passed. |
| [#571](https://github.com/iamfatness/CoreVideoPro/issues/571) | COM lifetime through WIC/module teardown in #566. Reproducing regression and installed orderly shutdown checks passed. |
| [#572](https://github.com/iamfatness/CoreVideoPro/issues/572) | Unobserved startup remains `starting` in #566. Lifecycle regressions and repeated installed starts passed. |
| [#573](https://github.com/iamfatness/CoreVideoPro/issues/573) | Phase-stable two-clip summing fixture in #566; assertion retained. Repeated local runs and merged-main TSan passed. |
| [#574](https://github.com/iamfatness/CoreVideoPro/issues/574) | GPU readiness moved before publication in #566. Hardware pixel tests and 30m16s installed candidate soak passed with zero new CVP delivery failures. Workload limits remain under #517/#569. |
| [#529](https://github.com/iamfatness/CoreVideoPro/issues/529), [#533](https://github.com/iamfatness/CoreVideoPro/issues/533) | ISO drops/timeline fixes merged and validated in the earlier 24-minute six-ISO recording; shipped September 18. |
| [#516](https://github.com/iamfatness/CoreVideoPro/issues/516), [#526](https://github.com/iamfatness/CoreVideoPro/issues/526), [#518](https://github.com/iamfatness/CoreVideoPro/issues/518) | Earlier render-stall, Program-buffer startup/busy-loop, and audio-log fixes merged and validated; shipped September 18. Later distinct defects retain their own issues. |
