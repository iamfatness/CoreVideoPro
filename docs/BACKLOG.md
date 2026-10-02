# CoreVideo Pro — ranked backlog

**This is the only ordered list of work.** Status and detailed evidence live on
linked GitHub issues. Rules: [AGENTS.md](../AGENTS.md).

Owner-requested order review 2026-09-27 against `main` `0ff22af4` and linked
issues: #659, #661, #663 and the #674 implementation slices are merged. The
Now/Next tables below replace the exhausted 2026-09-26 Now queue.
Earlier #616, #601, #617, #622, #530, #621, and #540 are closed.
The owner paused the remaining private-SDK CI work in #618 pending research.
Owner promoted and completed source-bus ingest extraction (#540). On 2026-09-26 the
owner approved executing the #657 control-state plan before SRT/NDI edge I/O (#538);
its three implementation slices are now merged, while parent qualification remains.
Owner accepted live lip sync (#579) and first Takes (#555) on beta `db9e703`.
#513 validation remains deferred; #582 range correction is in live validation.
#668 speaker-floor director implementation is merged, with installed acceptance open.

Architecture review pin: commit `6f4f025` (2026-09-24). Issues #616–#622 are that
review. Live incidents (#615, #608, #624) stay unranked and may
preempt this list on a show night.

## How to use this file

1. Work the first unblocked **Now** item. Maximum 3 in flight.
2. New findings get a GitHub issue and an unranked row; the owner ranks them.
3. Close only after a merged fix meets acceptance, or an explicit owner disposition.
4. A fix awaiting live acceptance is not Done. A non-reproducing report is not a confirmed current defect.
5. Build/run guidance lives in `CLAUDE.md`; design documents are not work queues.

Priority remains show survival, diagnosability, real-show usability, then external
installation. Build one source bus before adding more ingest paths. MXL stays parked.

## Now — revised priority order

| Order | Issue | Remaining work |
|---|---|---|
| 1 | [#538](https://github.com/iamfatness/CoreVideoPro/issues/538) | Owner made streaming encode/mux the top priority on 2026-09-29: one shared hardware Program encode for compatible RTMP/SRT/HLS destinations, compositor-clock video and sample-counted audio, independent bounded mux/transport queues, decoded sync and bitrate evidence. Existing edge I/O slices remain; #703 owns the current YouTube A/V incident and #605 owns hardware GOP/IDR. |
| 2 | [#657](https://github.com/iamfatness/CoreVideoPro/issues/657) | Qualify the merged control-state slices in an installed live meeting: Zoom mute/unmute converges in shell strip and PCM without Preview, GoXLR monitor control is audible, and gap/restart recovery stays within refresh budgets. If real guest transitions are unavailable, record `MISSING_EVIDENCE` and work the next unblocked row. |
| 3 | [#536](https://github.com/iamfatness/CoreVideoPro/issues/536) | Listener/caller SRT ingest and a 30-minute 1080p30 contribution soak passed in #687. Finish honest RTT and codec/packet decode-error health, then validate failure/recovery; the issue remains open. |

## Deferred by owner

| Issue | Status |
|---|---|
| [#513](https://github.com/iamfatness/CoreVideoPro/issues/513) | Owner reports an overnight run with no recurrence and will validate later. Pooling/teardown fixes and the earlier 151-minute soak remain evidence of reduced exposure, not root-cause closure. Not a blocker for #535. |
| [#618](https://github.com/iamfatness/CoreVideoPro/issues/618) | Owner paused private Zoom SDK CI provisioning pending research. The non-stub Windows adapter compile lane passed in #643; real Zoom CI compile still reports `MISSING_EVIDENCE`. Keep open. |

## Next — external install and cold media Take

| Order | Issue | Remaining work |
|---|---|
| 1 | [#423](https://github.com/iamfatness/CoreVideoPro/issues/423) | Signing + first external install. Current beta is still unsigned. |
| 2 | [#449](https://github.com/iamfatness/CoreVideoPro/issues/449) | **Cold Take acceptance.** Media half closed by #535 slice 3b. Step 1 open: a never-cued clip cut to Program still cold-starts into the warming slate. `scripts/qa/media-take-ab.py --skip-cue` is the falsification control. |

## Unranked — show survival and operator findings

These can preempt Next on a show night. They do not jump the ranked Now item until the owner says so.

| Issue | Remaining work |
|---|---|
| [#735](https://github.com/iamfatness/CoreVideoPro/issues/735) | Fixed: the CPU-fallback stream path delivered nothing from 2026-09-30 because shared-AAC routing withheld PCM from a sender that was not on the GPU path. Remaining: RTMP was not tested separately from SRT; owner acceptance on a machine without a hardware encoder. |
| [#732](https://github.com/iamfatness/CoreVideoPro/issues/732) | Recording resumes in a new folder and the stream reconnects after the media core restarts (owner rulings 2026-10-01 and 2026-10-02; PR #746). Remaining: owner acceptance on an installed beta; the stream reconnect was proven against a local SRT sink only, not RTMP or a real platform. |
| [#728](https://github.com/iamfatness/CoreVideoPro/issues/728) | Frame-sized allocations drop a frame instead of terminating the core, proven under a real per-process commit cap at 600 and 300 MB. At 400 to 500 MB the core still dies in Windows thread start-up, which nothing in the core can catch. The core now publishes `systemMemory` and logs when the machine runs low. Remaining: show that warning to the operator; what exhausted memory on 2026-10-01 is unknown. Owner has not assigned a rank. |
| [#724](https://github.com/iamfatness/CoreVideoPro/issues/724) | Program lost two slots whenever a source, stream or recording was first exported: the render thread created a D3D device (13 to 21 ms). Every export (participant, encoder, multiview, preview) is now created off that thread, and the recorded harness fails on any missed Program slot. Remaining: owner acceptance on an installed beta with a real media Take; the 4-6 slot stall first reported did not reproduce on any build (0 of 42 sessions). |
| [#750](https://github.com/iamfatness/CoreVideoPro/issues/750) | Recorded A/V harness: media audio lands 13 to 18 ms after video, straddling its one-frame limit, so its A/V verdict fails about one run in four. Two sessions in 14 read 34.5 ms. Needs a ruling on whether the steady offset is expected. Owner has not assigned a rank. |
| [#725](https://github.com/iamfatness/CoreVideoPro/issues/725) | Root cause fixed in #727: the shell rejected a respawned core's roster. Owner acceptance on an installed beta remains. |
| [#705](https://github.com/iamfatness/CoreVideoPro/issues/705) | Headless media A/V pattern reaches Program video but not audio. The media transport demand clock mismatches the render timeline; fix and verify PCM in a real Release recording. Owner has not assigned a rank. |
| [#668](https://github.com/iamfatness/CoreVideoPro/issues/668) | Slices A–D merged (#670–#673): unique guest bindings, host exclusion, talk ledger and Set & Forget holds. Installed host-plus-three-guests, interview, solo and share-priority acceptance remains. |
| [#674](https://github.com/iamfatness/CoreVideoPro/issues/674) | Slices A–F merged (#675–#680), including direct negotiated-format facts and existing Multiview texture crop. Installed no-click video-off, ISO arm, Engine-off transitions and ten-row GPU load remain acceptance; reopened after an early close. |
| [#651](https://github.com/iamfatness/CoreVideoPro/issues/651) | Virtual-camera DLL registration pointed to a removed beta. Re-registering restored the live camera; durable install-path registration, an actionable error and installed-beta proof remain. |
| [#473](https://github.com/iamfatness/CoreVideoPro/issues/473) | Installed beta ProRes media decoder shows a placeholder while the same code works in a dev launch; reproduce and fix the installed path. |
| [#652](https://github.com/iamfatness/CoreVideoPro/issues/652) | Matched A/B found GoXLR live skew near −240 ms on both accepted beta and prior main, while recorded Program stayed near −20 ms. #655 reduced live skew to −46.2 ms with zero underruns/lost samples over 24 s; real-human and long-show drift acceptance remain. |
| [#703](https://github.com/iamfatness/CoreVideoPro/issues/703) | Saved YouTube replay has about 0.7 s audio lag. Same-run Program/RTMP local pattern is near one frame with #705's media PCM fix; the sender reports video and audio on the saved YouTube test destination. Fresh YouTube playback measurement and frame-delivery acceptance remain missing. |
| [#649](https://github.com/iamfatness/CoreVideoPro/issues/649) | RTMP demuxer test compares `const char*` literal addresses; use content comparison so Debug native gate is deterministic. |
| [#615](https://github.com/iamfatness/CoreVideoPro/issues/615) | RTMP packet amplification is measured and coalesced. Encoder frames that never reached the shed counter are retained (main `61e83ed1`). The original PC-wide outage and full-show continuity still need fleet acceptance. |
| [#627](https://github.com/iamfatness/CoreVideoPro/issues/627) | Per-participant audio delay. Capture devices already have `setAudioSyncOffset`; a Zoom guest does not. Headless clap on the Zoom-audio mailbox build measured one pipeline skew (median -39.0 ms, audio lags video), not a per-person offset. |
| [#608](https://github.com/iamfatness/CoreVideoPro/issues/608) | Root cause isolated: Zoom source mute was copied into durable core mixer mute; #656 fixed that copy. Installed recurrence and natural guest mute/unmute convergence remain unverified; track with #657. |
| [#624](https://github.com/iamfatness/CoreVideoPro/issues/624) | Recover a subscribed Zoom video feed that stops advancing in Tiles. |
| [#599](https://github.com/iamfatness/CoreVideoPro/issues/599) | After the #597 stream stall, Zoom Meeting panel stayed hidden on display 2. Targeted restore worked; need a reliable operator path. Causality with #597 unproven. |
| [#610](https://github.com/iamfatness/CoreVideoPro/issues/610) | Operator-facing degraded-stream readout. Consume the generated observation model; do not hand-write a fourth snapshot. |
| [#590](https://github.com/iamfatness/CoreVideoPro/issues/590) | Simplify and prune Sources for live operation. Coordinate with #505 and #588. |
| [#681](https://github.com/iamfatness/CoreVideoPro/issues/681) | Global Zoom camera resolution request and selectable local maximum frame rate (15/24/25/30/60 fps); preserve actual input Format and verify both choices, churn, and persistence in a real meeting. |
| [#682](https://github.com/iamfatness/CoreVideoPro/issues/682) | Local Windows `test:gate` App integration worker intermittently stalls; PR #683 CI integration passed, so reproduce and bound before calling this a CI blocker. |
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

## Unranked — half-wired features (2026-10-02 audit)

Found by reading code on main; none reproduced in the running app unless its issue says so. Order is a suggestion (most likely to hit a normal show first); the owner ranks.

| Issue | Remaining work |
|---|---|
| [#758](https://github.com/iamfatness/CoreVideoPro/issues/758) | A disconnected bridged capture device stays on Program as a frozen frame: the shell never sends `unregister-capture-shm` and the adapter re-emits its last frame. Blackmagic-named UVC devices are forced onto this path. |
| [#759](https://github.com/iamfatness/CoreVideoPro/issues/759) | Output status reads "Live" while the sender is still `starting`. Stream-only sessions on a current core run the shell's "legacy core" branches because the core omits `recording.lifecycle`. |
| [#760](https://github.com/iamfatness/CoreVideoPro/issues/760) | SRT and RTMP ingest status never reaches the UI on a real core: `captureDevices` is not bound on the wire path, so a working feed reads as not connected. |
| [#762](https://github.com/iamfatness/CoreVideoPro/issues/762) | The shell accepts any core. A stub encoder (Record shows live, no file written) or no-op compositor in a real build is silent; `NativeMediaCoreProfileValidator` has no caller. Pair with #741. |
| [#763](https://github.com/iamfatness/CoreVideoPro/issues/763) | NDI group is accepted and discarded; Program is advertised to every receiver on the LAN. |
| [#761](https://github.com/iamfatness/CoreVideoPro/issues/761) | Set & Forget and Magic Scene never act on a real core: `autoProduction` is not bound. |
| [#764](https://github.com/iamfatness/CoreVideoPro/issues/764) | Operator settings the core drops: colour-grade LUT presets, brand-kit styles, and other payload fields. |
| [#765](https://github.com/iamfatness/CoreVideoPro/issues/765) | Invented values shown as measurements: guest network quality always "good", the LIVE clock, caption latency/confidence, per-participant meters. |
| [#766](https://github.com/iamfatness/CoreVideoPro/issues/766) | Diagnostics that over-claim or are empty: first-frame evidence, support-bundle sections, the stale-roster counter (roster epoch/revision unbound). |
| [#767](https://github.com/iamfatness/CoreVideoPro/issues/767) | Mock UI: licensing panel and caption controls with nothing behind them. Depends on the #743 ruling. |
| [#768](https://github.com/iamfatness/CoreVideoPro/issues/768) | Dead code: test-only fail/recover/simulate commands in the production protocol, breakout detection wired only to its simulator, an unused 935-line Zoom SDK adapter, unreferenced C# types. |

## Unranked — mock code in the shipping path and repo cleanup (2026-10-01 audit)

Found by the 2026-10-01 cleanup audit. Order within this table is a suggestion; the owner ranks.

| Issue | Remaining work |
|---|---|
| [#741](https://github.com/iamfatness/CoreVideoPro/issues/741) | The core serves a fake two-person Zoom roster whenever `COREVIDEO_ZOOM_ENGINE_PATH` is unset, at runtime. Report Zoom unavailable instead; make `simulate-breakout-room-change` stub-only. |
| [#738](https://github.com/iamfatness/CoreVideoPro/issues/738) | Remove the orphaned React prototype (`src/`) and Node simulator (`native-core/`): PR #745. Follow-ups: move the two Zoom config JSONs out of `src/`; remove `studio/` once `scripts/app.ps1` no longer builds the core through `build-studio.ps1`. |
| [#754](https://github.com/iamfatness/CoreVideoPro/issues/754) | Two timing-based shell tests each failed CI once and passed on rerun (`IncompatibleRequestOnlyChildIsTerminallyRejected`, `AStartDuringCrashRecoveryStillRejoinsZoom`). The second is in the crash-recovery rejoin path: rule out a real double-rejoin. |
| [#743](https://github.com/iamfatness/CoreVideoPro/issues/743) | `caption-broker` and `licensing-api` are stub services with no native client. Owner ruling: build for real or remove, including deployed workers. |

## Unranked — open issue inventory restored in this audit

These linked issues already existed with the `backlog` label but had no row in
the sole work list. Their order in this table is not a priority assignment.

| Issue | Remaining work |
|---|---|
| [#602](https://github.com/iamfatness/CoreVideoPro/issues/602), [#604](https://github.com/iamfatness/CoreVideoPro/issues/604), [#612](https://github.com/iamfatness/CoreVideoPro/issues/612) | Sender supervision: destination isolation, stop terminating the FFmpeg child, and deterministic give-up during backoff. |
| [#605](https://github.com/iamfatness/CoreVideoPro/issues/605), [#606](https://github.com/iamfatness/CoreVideoPro/issues/606), [#607](https://github.com/iamfatness/CoreVideoPro/issues/607) | Encoder/sender correctness: keyframe interval, codec selection, and a measured queue latency bound. |
| [#611](https://github.com/iamfatness/CoreVideoPro/issues/611) | Move test-only virtual methods out of `IOutputSender` when touching that seam. |
| [#524](https://github.com/iamfatness/CoreVideoPro/issues/524), [#525](https://github.com/iamfatness/CoreVideoPro/issues/525) | GPU encode writer isolation and later recording/ISO/macOS adapter slices; separate from current edge I/O. |
| [#505](https://github.com/iamfatness/CoreVideoPro/issues/505), [#512](https://github.com/iamfatness/CoreVideoPro/issues/512) | Vestigial Sources border controls and residual Tiles-wall polish. |
| [#457](https://github.com/iamfatness/CoreVideoPro/issues/457), [#444](https://github.com/iamfatness/CoreVideoPro/issues/444), [#446](https://github.com/iamfatness/CoreVideoPro/issues/446) | Shell crash dump triage, churn/resize soak, and live Take soak evidence. |
| [#443](https://github.com/iamfatness/CoreVideoPro/issues/443), [#445](https://github.com/iamfatness/CoreVideoPro/issues/445) | Combined record/stream/vcam drill and engine-off teardown audit. |
| [#453](https://github.com/iamfatness/CoreVideoPro/issues/453), [#447](https://github.com/iamfatness/CoreVideoPro/issues/447) | Worktree housekeeping and persistent-source migration reconciliation. |
| [#450](https://github.com/iamfatness/CoreVideoPro/issues/450), [#451](https://github.com/iamfatness/CoreVideoPro/issues/451) | Deferred OHG screen integration and live scene-canvas editor design. |
| [#439](https://github.com/iamfatness/CoreVideoPro/issues/439), [#442](https://github.com/iamfatness/CoreVideoPro/issues/442) | GPU-tier defaults and reference-hardware sweeps. |
| [#433](https://github.com/iamfatness/CoreVideoPro/issues/433), [#434](https://github.com/iamfatness/CoreVideoPro/issues/434), [#435](https://github.com/iamfatness/CoreVideoPro/issues/435), [#436](https://github.com/iamfatness/CoreVideoPro/issues/436), [#437](https://github.com/iamfatness/CoreVideoPro/issues/437) | Installer, production telemetry endpoint, beta access, OAuth broker, and signing integration gates. |
| [#424](https://github.com/iamfatness/CoreVideoPro/issues/424), [#425](https://github.com/iamfatness/CoreVideoPro/issues/425), [#426](https://github.com/iamfatness/CoreVideoPro/issues/426), [#427](https://github.com/iamfatness/CoreVideoPro/issues/427) | Zoom SDK redistribution answer, reference machines, tester feedback channel, and handoff decisions. |
| [#441](https://github.com/iamfatness/CoreVideoPro/issues/441) | First-run wizard design after external-install path is known. |

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
| [#665](https://github.com/iamfatness/CoreVideoPro/issues/665) | Live monitor control snapshot fix merged in #666 and shipped in beta `975076b8`; installed Off/On applied on GoXLR Game, native monitor muted/playing, and the owner confirmed the button works. |
| [#659](https://github.com/iamfatness/CoreVideoPro/issues/659), [#661](https://github.com/iamfatness/CoreVideoPro/issues/661), [#663](https://github.com/iamfatness/CoreVideoPro/issues/663) | Versioned roster facts, revisioned audio crosspoint edits, and bounded recovery merged in #683/#684/#685; child issues closed. Parent #657 retains combined installed qualification. |
| [#632](https://github.com/iamfatness/CoreVideoPro/issues/632), [#634](https://github.com/iamfatness/CoreVideoPro/issues/634), [#637](https://github.com/iamfatness/CoreVideoPro/issues/637), [#640](https://github.com/iamfatness/CoreVideoPro/issues/640) | Native slate assertion, Zoom passcode parsing, live raw-capture validator, and JSONL handshake test fixes merged; issues closed. |
| [#597](https://github.com/iamfatness/CoreVideoPro/issues/597) | Backpressure issue closed by owner on 2026-09-24. Remaining sender defects (#602/#604–#607/#612), degraded-stream readout (#610), and full-show outage acceptance (#615) have separate open issues. |
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
