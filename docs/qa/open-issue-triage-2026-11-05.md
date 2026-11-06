# Open-issue triage snapshot — November 5, 2026

This is an **unranked inventory and evidence audit**, not a second work queue.
The proposed eight-hour order is only in [BACKLOG](../BACKLOG.md#active-eight-hour-session--november-5-into-november-6-2026).

## Coverage and limits

- 134 open issues were enumerated from GitHub (limit 200; the returned set was not capped), including titles, labels and update dates.
- Bodies and comments were retrieved for 18 high-value candidates: #1143, #1157, #1158, #1074, #1056, #1133, #1137, #970, #1136, #1146, #993, #758, #513, #608, #989, #1118, #1117 and #1076. Long historical threads were read selectively; this is not an exhaustive audit of every old issue or every comment.
- The remaining issues received title-level scope screening. Their classification is provisional; **not selected does not mean low severity, resolved or safe**.
- Current source examined at `a40e90bbda63caef7c2715a019c5619d1b1ad74d`: Tiles membership/hold policy, UVC asynchronous read path, Graphics cache and private-preview tests, and the monitor retirement fixture. PR merge evidence was checked for #1132, #990 and #656.
- Main CI and CodeQL were still running when this plan was assembled. The published beta was `beta-2026-11-05-3330c56`; do not confuse it with the newer Twitch/capture-fixture main source.
- The owner explicitly permits local app and device rehearsals for the planned window. A running WinUI process was observed; no process was stopped or operated for this audit.
- No eight-hour execution was started, no code was changed, and no issue was closed by the planning task.

## Findings that change the plan

| Evidence | Planning consequence |
|---|---|
| [#1143](https://github.com/iamfatness/CoreVideoPro/issues/1143) retained a real dropped ISO frame and held-count mismatch; isolated reruns pass | Highest-value bounded investigation. Preserve actual samples and strict loss accounting; do not call it counter noise or proven fixture-only failure. |
| [#1158](https://github.com/iamfatness/CoreVideoPro/issues/1158) misses a second prepared asset after lease release; [#1157](https://github.com/iamfatness/CoreVideoPro/issues/1157) rejects a valid preview without retaining the response | Capture missing admission/RPC evidence first. Same Graphics area does not establish a common root cause. |
| [#1137](https://github.com/iamfatness/CoreVideoPro/issues/1137) fixture reads `gpuDone` twice when constructing one observation | A small deterministic test-interleaving candidate; the race explanation remains a hypothesis until reproduced. |
| [#1074](https://github.com/iamfatness/CoreVideoPro/issues/1074) still describes tile loss at 1.5 s, but main contains a separate 4 s hold window (`427e7ce6`, `45d37414`) | Rehearse existing behavior; do not implement the old proposal again. Four seconds remains a working assumption under #427. Correlated incoming/outgoing stalls are not conclusive network attribution. |
| [#1133](https://github.com/iamfatness/CoreVideoPro/issues/1133) comments say the timing fix and exact-head checks passed; supporting #1132 merged November 5 | Strong closure-evidence candidate. Verify the final linked acceptance, then close with evidence rather than schedule another rewrite. |
| [#608](https://github.com/iamfatness/CoreVideoPro/issues/608) later comment supersedes its PCM-silence hypothesis with stale mixer mute; #656 is merged | Recheck remaining audio after participant mute/departure during rehearsal. Do not start a speculative Zoom PCM rewrite from the original report. |
| [#989](https://github.com/iamfatness/CoreVideoPro/issues/989) has merged #990; partial workload previously lacked the eighth remote feed | Full simultaneous output evidence is still necessary; a partial trial or zero drop counter does not close it. |
| [#993](https://github.com/iamfatness/CoreVideoPro/issues/993) described synchronous no-reply reads; main now installs `MF_SOURCE_READER_ASYNC_CALLBACK` and uses a bounded callback wait (`37fa18c0`) | Exercise actual unplug/replug and command responsiveness before considering another capture redesign. |
| [#758](https://github.com/iamfatness/CoreVideoPro/issues/758) capture work landed in merged #1132 | Verify actual bridge dropout/reconnect rather than assume the open issue means the old code remains. |
| [#1076](https://github.com/iamfatness/CoreVideoPro/issues/1076) comments record merged Operate/Design, Cue/Show/Hide and On Air work | Evaluate the four user tasks on the current app. Technical probe success is not the owner's usability acceptance. |
| [#1146](https://github.com/iamfatness/CoreVideoPro/issues/1146) Auto fixes are merged, but its done-when includes installed host/three-guest behavior | Give this a real rehearsal window; code and local test completion alone do not close it. #668 is already closed. |
| [#1056](https://github.com/iamfatness/CoreVideoPro/issues/1056) has actual Program frame-loss evidence correlated with a long render lock hold | Include precise Program identity and lock attribution in the output run. Do not promise a broad lock-ownership refactor overnight. |
| [#1136](https://github.com/iamfatness/CoreVideoPro/issues/1136) requires an approved private SDK download configuration | Credential/source dependency; local packaging already works. Do not put private SDK material in git or let this block all reliability work. |
| [#1117](https://github.com/iamfatness/CoreVideoPro/issues/1117) and [#1118](https://github.com/iamfatness/CoreVideoPro/issues/1118) require controlled provider events | Conditional qualification only when the channel/accounts/authorization are available. No unapproved public broadcasts or synthetic chat sent to others. |

The hour-scale #513 crash and old capture/performance reports remain open evidence, not disproven by a short clean rehearsal. The known #1141 camera wall-clock test issue is now closed; it is absent from the 134-open snapshot.

## Inventory buckets

- Bounded investigation: 4
- Closure-evidence review: 3
- Current-state review needed; not selected: 102
- External access or owner decision: 10
- Feature expansion deferred: 5
- Rehearsal/output evidence: 10

## Unranked open inventory

Sorted by issue number only. “Body/comments” indicates deeper evidence retrieval; “Title screen” requires further validation before execution.

| Issue | Title | Triage bucket | Review depth |
|---|---|---|---|
| [#1158](https://github.com/iamfatness/CoreVideoPro/issues/1158) | Graphics cache test misses the second asset after releasing its first lease in full run | Bounded investigation | Body/comments |
| [#1157](https://github.com/iamfatness/CoreVideoPro/issues/1157) | Valid private Graphics preview request rejected during full native run | Bounded investigation | Body/comments |
| [#1146](https://github.com/iamfatness/CoreVideoPro/issues/1146) | Auto director policy conflict discards native bindings and leaves a non-executable fallback | Rehearsal/output evidence | Body/comments |
| [#1143](https://github.com/iamfatness/CoreVideoPro/issues/1143) | Full native run exposes ISO late-frame drop and held-frame count mismatch | Bounded investigation | Body/comments |
| [#1139](https://github.com/iamfatness/CoreVideoPro/issues/1139) | Graphics follow-on: authoring efficiency and presenter workflow specification | Feature expansion deferred | Title screen |
| [#1137](https://github.com/iamfatness/CoreVideoPro/issues/1137) | Investigate monitor reader retirement progress assertion under TSAN | Bounded investigation | Body/comments |
| [#1136](https://github.com/iamfatness/CoreVideoPro/issues/1136) | Restore private Zoom SDK configuration for hosted beta builds | External access or owner decision | Body/comments |
| [#1133](https://github.com/iamfatness/CoreVideoPro/issues/1133) | RPC contention test relies on more than five frames during a fixed 300 ms delay | Closure-evidence review | Body/comments |
| [#1120](https://github.com/iamfatness/CoreVideoPro/issues/1120) | Graphics social feed follow-up: moderated LinkedIn organization comments | Feature expansion deferred | Title screen |
| [#1119](https://github.com/iamfatness/CoreVideoPro/issues/1119) | Graphics social feed follow-up: moderated X posts | Feature expansion deferred | Title screen |
| [#1118](https://github.com/iamfatness/CoreVideoPro/issues/1118) | Graphics social feed follow-up: moderated Twitch chat | External access or owner decision | Body/comments |
| [#1117](https://github.com/iamfatness/CoreVideoPro/issues/1117) | Qualify moderated YouTube live chat with a controlled public stream | External access or owner decision | Body/comments |
| [#1113](https://github.com/iamfatness/CoreVideoPro/issues/1113) | Graphics Phase 3 Slice 6: individually approved YouTube live chat graphics | Current-state review needed; not selected | Title screen |
| [#1110](https://github.com/iamfatness/CoreVideoPro/issues/1110) | Graphics Phase 3 Slice 5: custom APIs and social feeds | Current-state review needed; not selected | Title screen |
| [#1106](https://github.com/iamfatness/CoreVideoPro/issues/1106) | Graphics Phase 3 Slice 4: data-aware Shot Layouts | Current-state review needed; not selected | Title screen |
| [#1103](https://github.com/iamfatness/CoreVideoPro/issues/1103) | Graphics Phase 3 Slice 3: HTTPS data feeds for graphics | Current-state review needed; not selected | Title screen |
| [#1098](https://github.com/iamfatness/CoreVideoPro/issues/1098) | Graphics Phase 3 Slice 2: native crawl and safe publication | Current-state review needed; not selected | Title screen |
| [#1096](https://github.com/iamfatness/CoreVideoPro/issues/1096) | Graphics Phase 3 Slice 1: local data tables and leaderboards | Current-state review needed; not selected | Title screen |
| [#1076](https://github.com/iamfatness/CoreVideoPro/issues/1076) | Graphics tab is not usable for the operator: the four everyday tasks are not findable | Rehearsal/output evidence | Body/comments |
| [#1074](https://github.com/iamfatness/CoreVideoPro/issues/1074) | Tiles drop participants during 1-2 s network stalls that also back-pressure the stream (live show 2026-10-31) | Rehearsal/output evidence | Body/comments |
| [#1056](https://github.com/iamfatness/CoreVideoPro/issues/1056) | Program recording missed one frame in the 10-25 soak: 15 ms coreMutex hold on render.display-tick (#1049 evidence) | Rehearsal/output evidence | Body/comments |
| [#1049](https://github.com/iamfatness/CoreVideoPro/issues/1049) | Render frame group: fetch-before-lock A/B for the three sanctioned engine-mutex calls (#1037 follow-up) | Current-state review needed; not selected | Title screen |
| [#1048](https://github.com/iamfatness/CoreVideoPro/issues/1048) | Command lane: zoom-media-spine-sync and zoom-snapshot take the Zoom engine mutex under coreMutex (#1037 follow-up) | Current-state review needed; not selected | Title screen |
| [#1041](https://github.com/iamfatness/CoreVideoPro/issues/1041) | Multiview wall beyond 10 slots on the 8-16 GB and 16 GB+ GPU tiers (owner-scheduled after RC) | Feature expansion deferred | Title screen |
| [#1003](https://github.com/iamfatness/CoreVideoPro/issues/1003) | Gray Program sources from blocked GPU preparation retirement escape counter-band qualification | Current-state review needed; not selected | Title screen |
| [#1002](https://github.com/iamfatness/CoreVideoPro/issues/1002) | Screen capture freezes while the selected display video continues moving | Current-state review needed; not selected | Title screen |
| [#993](https://github.com/iamfatness/CoreVideoPro/issues/993) | Camera disconnect can block the native command lane on a no-reply synchronous ReadSample | Rehearsal/output evidence | Body/comments |
| [#989](https://github.com/iamfatness/CoreVideoPro/issues/989) | Preserve Zoom ISO arrivals before latest-picture playout and account upstream loss | Rehearsal/output evidence | Body/comments |
| [#971](https://github.com/iamfatness/CoreVideoPro/issues/971) | Beta installer rolls back camera setup while direct packaged helper succeeds (#857) | Current-state review needed; not selected | Title screen |
| [#970](https://github.com/iamfatness/CoreVideoPro/issues/970) | Compiled Graphics search probe checks asynchronous filtering after a fixed 50 ms | Current-state review needed; not selected | Body/comments |
| [#963](https://github.com/iamfatness/CoreVideoPro/issues/963) | Qualify and package UI-independent production without orphaning live engines | Current-state review needed; not selected | Title screen |
| [#962](https://github.com/iamfatness/CoreVideoPro/issues/962) | Remove WinUI-owned camera fallback from production capture | Current-state review needed; not selected | Title screen |
| [#961](https://github.com/iamfatness/CoreVideoPro/issues/961) | Move live show synchronization and recovery authority out of WinUI | Current-state review needed; not selected | Title screen |
| [#959](https://github.com/iamfatness/CoreVideoPro/issues/959) | Keep the committed show running across a WinUI crash: implementation and acceptance | Current-state review needed; not selected | Title screen |
| [#941](https://github.com/iamfatness/CoreVideoPro/issues/941) | Classify intentional Program shutdown separately from required delivery misses | Current-state review needed; not selected | Title screen |
| [#933](https://github.com/iamfatness/CoreVideoPro/issues/933) | Focused AVFoundation encoder CI test segfaults on macOS paravirtual runner | Current-state review needed; not selected | Title screen |
| [#930](https://github.com/iamfatness/CoreVideoPro/issues/930) | Redesign audit: remove UI-paced video dependencies and require full-workload frame evidence | Current-state review needed; not selected | Title screen |
| [#929](https://github.com/iamfatness/CoreVideoPro/issues/929) | Volt Graphics: scoped participant automation, rehearsal and manual override | Current-state review needed; not selected | Title screen |
| [#928](https://github.com/iamfatness/CoreVideoPro/issues/928) | Volt Graphics: native title, logo bug and clock/countdown consumers | Current-state review needed; not selected | Title screen |
| [#927](https://github.com/iamfatness/CoreVideoPro/issues/927) | Volt Graphics: template library, cue workspace and compatibility migration | Current-state review needed; not selected | Title screen |
| [#926](https://github.com/iamfatness/CoreVideoPro/issues/926) | Volt Graphics: native instances and bounded preparation through compatibility lower thirds | Current-state review needed; not selected | Title screen |
| [#919](https://github.com/iamfatness/CoreVideoPro/issues/919) | Guard deferred participant-list callbacks against window teardown | Current-state review needed; not selected | Title screen |
| [#916](https://github.com/iamfatness/CoreVideoPro/issues/916) | Measure delayed source video when opening Sources Inspect | Current-state review needed; not selected | Title screen |
| [#911](https://github.com/iamfatness/CoreVideoPro/issues/911) | Camera cold consumer skips an identity while recovering a torn latest-slot copy | Current-state review needed; not selected | Title screen |
| [#908](https://github.com/iamfatness/CoreVideoPro/issues/908) | Preserve prepared Program source pixels across SDK epoch transitions | Current-state review needed; not selected | Title screen |
| [#907](https://github.com/iamfatness/CoreVideoPro/issues/907) | Attribute native stub TSan gate hang after recording-session tests | Current-state review needed; not selected | Title screen |
| [#906](https://github.com/iamfatness/CoreVideoPro/issues/906) | Investigate Windows GradePreviewCoordinator disposal gate timeout | Current-state review needed; not selected | Title screen |
| [#904](https://github.com/iamfatness/CoreVideoPro/issues/904) | Multiview Program reader refuses and skips copies during real-meeting API validation | Current-state review needed; not selected | Title screen |
| [#903](https://github.com/iamfatness/CoreVideoPro/issues/903) | Monitor underrun counter resets in muted snapshots and resurfaces after reenable | Current-state review needed; not selected | Title screen |
| [#898](https://github.com/iamfatness/CoreVideoPro/issues/898) | Isolated Program monitor misses completion bound in fixed-phase GPU fixture | Current-state review needed; not selected | Title screen |
| [#893](https://github.com/iamfatness/CoreVideoPro/issues/893) | Intermittent small white-box flash in webcam output viewed in Zoom SDK | Current-state review needed; not selected | Title screen |
| [#889](https://github.com/iamfatness/CoreVideoPro/issues/889) | Scene Builder inspector tab clear crashes during scene/color binding refresh | Current-state review needed; not selected | Title screen |
| [#882](https://github.com/iamfatness/CoreVideoPro/issues/882) | Sources thumbnails shift and show adjacent feeds after removing an input | Current-state review needed; not selected | Title screen |
| [#879](https://github.com/iamfatness/CoreVideoPro/issues/879) | Scene Builder workspace and CoreVideo Tiles controls | Current-state review needed; not selected | Title screen |
| [#876](https://github.com/iamfatness/CoreVideoPro/issues/876) | Program delivery clock can continue scheduling while the core retires owners | Current-state review needed; not selected | Title screen |
| [#872](https://github.com/iamfatness/CoreVideoPro/issues/872) | Audio monitor device lifecycle runs under the Program render lock | Current-state review needed; not selected | Title screen |
| [#867](https://github.com/iamfatness/CoreVideoPro/issues/867) | Windows native stub gate intermittently fails background-only media transport test | Current-state review needed; not selected | Title screen |
| [#856](https://github.com/iamfatness/CoreVideoPro/issues/856) | Zoom SDK helper disappears during real meeting; exit cause missing from diagnostics | Current-state review needed; not selected | Title screen |
| [#853](https://github.com/iamfatness/CoreVideoPro/issues/853) | Draw-first rectangle and circle scope ROI controls | Current-state review needed; not selected | Title screen |
| [#851](https://github.com/iamfatness/CoreVideoPro/issues/851) | 1080P flip live acceptance (mac): owner-meeting soak before any tagged beta | Current-state review needed; not selected | Title screen |
| [#850](https://github.com/iamfatness/CoreVideoPro/issues/850) | Intermittent macOS AsyncEncoderSink freshness recovery test failure | Current-state review needed; not selected | Title screen |
| [#848](https://github.com/iamfatness/CoreVideoPro/issues/848) | Make scene, audio and grading controls understandable and direct to manipulate | Current-state review needed; not selected | Title screen |
| [#847](https://github.com/iamfatness/CoreVideoPro/issues/847) | Improve lower-third appearance and add a usable style designer | Current-state review needed; not selected | Title screen |
| [#839](https://github.com/iamfatness/CoreVideoPro/issues/839) | Built-in OH framer overlay for webcam output only | Current-state review needed; not selected | Title screen |
| [#835](https://github.com/iamfatness/CoreVideoPro/issues/835) | Advanced per-source grading workspace with histogram, waveform and vectorscope | Current-state review needed; not selected | Title screen |
| [#823](https://github.com/iamfatness/CoreVideoPro/issues/823) | Complete bounded per-frame delivery evidence and latency attribution (#517) | Rehearsal/output evidence | Title screen |
| [#814](https://github.com/iamfatness/CoreVideoPro/issues/814) | Recording write queues are shared with the live path and drop ISO under disk pressure | Current-state review needed; not selected | Title screen |
| [#811](https://github.com/iamfatness/CoreVideoPro/issues/811) | Native MF capture fails after immediate HD60 S+ reconnect (0x8007001F) | Current-state review needed; not selected | Title screen |
| [#804](https://github.com/iamfatness/CoreVideoPro/issues/804) | Program frame expires during scheduled shell/multiview publication (#517) | Current-state review needed; not selected | Title screen |
| [#798](https://github.com/iamfatness/CoreVideoPro/issues/798) | Investigate hosted fake-core initial roster request timeout | Current-state review needed; not selected | Title screen |
| [#790](https://github.com/iamfatness/CoreVideoPro/issues/790) | SDK helper crashes on screen-share end: callback reenters share mutex | Current-state review needed; not selected | Title screen |
| [#784](https://github.com/iamfatness/CoreVideoPro/issues/784) | Screen capture flashes: asynchronous connecting state triggers repeated session restarts | Current-state review needed; not selected | Title screen |
| [#782](https://github.com/iamfatness/CoreVideoPro/issues/782) | Windows supervisor disposal can query an already-disposed Process during crash-lock test | Current-state review needed; not selected | Title screen |
| [#768](https://github.com/iamfatness/CoreVideoPro/issues/768) | Dead code in the shipping core and shell | Current-state review needed; not selected | Title screen |
| [#759](https://github.com/iamfatness/CoreVideoPro/issues/759) | Output status reads Live while the sender is still connecting | Current-state review needed; not selected | Title screen |
| [#758](https://github.com/iamfatness/CoreVideoPro/issues/758) | A disconnected bridged capture device stays on Program as a frozen frame | Rehearsal/output evidence | Body/comments |
| [#750](https://github.com/iamfatness/CoreVideoPro/issues/750) | Recorded A/V harness: media audio lands 13-18 ms after video, straddling the one-frame gate | Current-state review needed; not selected | Title screen |
| [#728](https://github.com/iamfatness/CoreVideoPro/issues/728) | Core dies on std::bad_alloc in the FFmpeg media decoder when Windows is low on virtual memory | Current-state review needed; not selected | Title screen |
| [#724](https://github.com/iamfatness/CoreVideoPro/issues/724) | Program render stalls ~100 ms when a take opens a media clip (4-6 delivery slots with no frame) | Current-state review needed; not selected | Title screen |
| [#701](https://github.com/iamfatness/CoreVideoPro/issues/701) | Media playout video can stutter or freeze during a live show; identify decoder versus presentation path | Current-state review needed; not selected | Title screen |
| [#681](https://github.com/iamfatness/CoreVideoPro/issues/681) | Global preferred maximum Zoom camera resolution | Current-state review needed; not selected | Title screen |
| [#674](https://github.com/iamfatness/CoreVideoPro/issues/674) | Multiview input inspector: live Zoom format, ISO, and picture without tickle refresh | Current-state review needed; not selected | Title screen |
| [#657](https://github.com/iamfatness/CoreVideoPro/issues/657) | Define cross-component state authority and event propagation, then prove first consumer | Current-state review needed; not selected | Title screen |
| [#652](https://github.com/iamfatness/CoreVideoPro/issues/652) | Live validation: perceived A/V sync worse than accepted beta; measure monitor and display path | Current-state review needed; not selected | Title screen |
| [#627](https://github.com/iamfatness/CoreVideoPro/issues/627) | Sync audio delay per participant | Current-state review needed; not selected | Title screen |
| [#624](https://github.com/iamfatness/CoreVideoPro/issues/624) | Recover a subscribed Zoom video feed that stops advancing in Tiles | Current-state review needed; not selected | Title screen |
| [#618](https://github.com/iamfatness/CoreVideoPro/issues/618) | CI Windows job compiles the stub core; production D3D11/MF/WASAPI adapters can merge unbuilt | Closure-evidence review | Title screen |
| [#615](https://github.com/iamfatness/CoreVideoPro/issues/615) | RTMP streaming amplifies packet traffic; investigate PC-wide connectivity loss | Current-state review needed; not selected | Title screen |
| [#608](https://github.com/iamfatness/CoreVideoPro/issues/608) | Audio drops out for remaining participants when others disconnect; selecting their source restores it | Rehearsal/output evidence | Body/comments |
| [#607](https://github.com/iamfatness/CoreVideoPro/issues/607) | The bitstream queue's caps are memory bounds used as a latency bound | Current-state review needed; not selected | Title screen |
| [#599](https://github.com/iamfatness/CoreVideoPro/issues/599) | Zoom Meeting window stays hidden after SDK media recovers | Current-state review needed; not selected | Title screen |
| [#595](https://github.com/iamfatness/CoreVideoPro/issues/595) | Hide OHG Show until explicitly enabled in Settings | Current-state review needed; not selected | Title screen |
| [#594](https://github.com/iamfatness/CoreVideoPro/issues/594) | Move Health into Settings and replace the Diagnose navigation label | Current-state review needed; not selected | Title screen |
| [#593](https://github.com/iamfatness/CoreVideoPro/issues/593) | Edit and persist each source lower-third name and secondary line in Sources | Current-state review needed; not selected | Title screen |
| [#592](https://github.com/iamfatness/CoreVideoPro/issues/592) | Stop lower-third secondary line refreshing to inconsistent Guest text | Current-state review needed; not selected | Title screen |
| [#591](https://github.com/iamfatness/CoreVideoPro/issues/591) | Make starter scenes and scene creation useful without repeated sources | Current-state review needed; not selected | Title screen |
| [#590](https://github.com/iamfatness/CoreVideoPro/issues/590) | Simplify and prune the Sources screen for live operators | Current-state review needed; not selected | Title screen |
| [#588](https://github.com/iamfatness/CoreVideoPro/issues/588) | Make local screen/window capture a first-class Program source | Current-state review needed; not selected | Title screen |
| [#582](https://github.com/iamfatness/CoreVideoPro/issues/582) | Video source brightness flashes during speaker changes | Current-state review needed; not selected | Title screen |
| [#581](https://github.com/iamfatness/CoreVideoPro/issues/581) | Investigate reported low webcam output frame rate on beta 793c452 | Current-state review needed; not selected | Title screen |
| [#575](https://github.com/iamfatness/CoreVideoPro/issues/575) | Scheduled staging smoke cannot run because required Actions secrets are missing | Current-state review needed; not selected | Title screen |
| [#569](https://github.com/iamfatness/CoreVideoPro/issues/569) | HEVC streaming needs repeatable installed receiver acceptance | Current-state review needed; not selected | Title screen |
| [#568](https://github.com/iamfatness/CoreVideoPro/issues/568) | Stream retry keeps stale AV1 terminal failure after switching to H.265 | Current-state review needed; not selected | Title screen |
| [#565](https://github.com/iamfatness/CoreVideoPro/issues/565) | GPU-direct AV1 emits near-empty access units (~54 bytes/frame at 1080p60) | Current-state review needed; not selected | Title screen |
| [#562](https://github.com/iamfatness/CoreVideoPro/issues/562) | Capture-device "On dropout" policy needs adapter liveness (signalPresent), not frameId cadence | Current-state review needed; not selected | Title screen |
| [#551](https://github.com/iamfatness/CoreVideoPro/issues/551) | ISO writer framesWritten/bytesWritten read 0 mid-recording for async independent writers (status-only) | Closure-evidence review | Title screen |
| [#539](https://github.com/iamfatness/CoreVideoPro/issues/539) | Parked: MXL receive for ZoomISO Cloud (not a Now item) | Feature expansion deferred | Title screen |
| [#538](https://github.com/iamfatness/CoreVideoPro/issues/538) | Core edge I/O: receive and send SRT, NDI, RTMP, HLS as adapters | Current-state review needed; not selected | Title screen |
| [#537](https://github.com/iamfatness/CoreVideoPro/issues/537) | DeckLink/AJA: live frames on the source bus (not probe-only) | Current-state review needed; not selected | Title screen |
| [#536](https://github.com/iamfatness/CoreVideoPro/issues/536) | SRT ingest decoder: packets must become VideoFrame pixels + PCM on the source bus | Current-state review needed; not selected | Title screen |
| [#525](https://github.com/iamfatness/CoreVideoPro/issues/525) | GPU-direct encode slices 2-4: recording/ISO on the GPU encoder + macOS VideoToolbox behind the seam | Current-state review needed; not selected | Title screen |
| [#524](https://github.com/iamfatness/CoreVideoPro/issues/524) | GPU-direct encode: decouple the bitstream write from the encoder thread (one slow destination throttles the encoder) | Current-state review needed; not selected | Title screen |
| [#517](https://github.com/iamfatness/CoreVideoPro/issues/517) | perf: Program render hits the one-frame budget at 16 participants with the 1080p wall | Rehearsal/output evidence | Title screen |
| [#513](https://github.com/iamfatness/CoreVideoPro/issues/513) | WinUI fail-fast 0xc000027b: the GC finalizer thread releases XAML objects (idle crash after ~1h) | Current-state review needed; not selected | Body/comments |
| [#512](https://github.com/iamfatness/CoreVideoPro/issues/512) | Residual minors from the Tiles-wall one-animator slice (#448 / PR #511) | Current-state review needed; not selected | Title screen |
| [#509](https://github.com/iamfatness/CoreVideoPro/issues/509) | Operator stutter: what flips the participant structure signature, and which rebuild step costs 197ms | Current-state review needed; not selected | Title screen |
| [#505](https://github.com/iamfatness/CoreVideoPro/issues/505) | The per-layer border colour/width controls in the Sources layer editor are vestigial | Current-state review needed; not selected | Title screen |
| [#473](https://github.com/iamfatness/CoreVideoPro/issues/473) | Installed beta: a ProRes media source never starts its FFmpeg decoder (placeholder on multiview), while a dev launch of the same code does | Current-state review needed; not selected | Title screen |
| [#456](https://github.com/iamfatness/CoreVideoPro/issues/456) | [T5.6] In and Out points for media playback | Current-state review needed; not selected | Title screen |
| [#451](https://github.com/iamfatness/CoreVideoPro/issues/451) | [T5.5] The scene canvas editor shows live GPU video (redesign, not a whitelist) | Current-state review needed; not selected | Title screen |
| [#450](https://github.com/iamfatness/CoreVideoPro/issues/450) | [T5.4] OHG: redesign on screens first, integrate into existing tabs; cheap parity gaps (preview tally, gallery order, | Current-state review needed; not selected | Title screen |
| [#447](https://github.com/iamfatness/CoreVideoPro/issues/447) | [T5.1] Reconcile persistent sources with the rearch: which #419 foundations slices 2-3 build on (likely SourceRegistr | Current-state review needed; not selected | Title screen |
| [#446](https://github.com/iamfatness/CoreVideoPro/issues/446) | [T4.4] Run scripts/qa/live-meeting-soak.mjs --takes N for #422 (needs the app stopped) | Current-state review needed; not selected | Title screen |
| [#444](https://github.com/iamfatness/CoreVideoPro/issues/444) | [T4.2] Scripted churn/resize soak (the 0xc000027b class; resize mitigation never soak-verified) | Current-state review needed; not selected | Title screen |
| [#443](https://github.com/iamfatness/CoreVideoPro/issues/443) | [T4.1] The record + stream + virtual camera drill as one command | Current-state review needed; not selected | Title screen |
| [#442](https://github.com/iamfatness/CoreVideoPro/issues/442) | [T3.4] Hardware sweeps on T0.3 machines: integrated GPU, audio devices (USB/Bluetooth, unplug mid-show), cheap UVC we | Current-state review needed; not selected | Title screen |
| [#441](https://github.com/iamfatness/CoreVideoPro/issues/441) | [T3.3] First-run wizard ^W screens first, owner markup, then code (the OHG lesson) | Current-state review needed; not selected | Title screen |
| [#437](https://github.com/iamfatness/CoreVideoPro/issues/437) | [T2.5] Wire the certificate into the release pipeline when T0.1 lands | External access or owner decision | Title screen |
| [#435](https://github.com/iamfatness/CoreVideoPro/issues/435) | [T2.3] Closed-beta access gate (signed key or services/licensing-api ^W owner picks) | External access or owner decision | Title screen |
| [#427](https://github.com/iamfatness/CoreVideoPro/issues/427) | [T0.5] Rule on the open handoff decisions: 1500 ms tile staleness; monitor isolation mitigation; the unexplained 18.6 | External access or owner decision | Title screen |
| [#426](https://github.com/iamfatness/CoreVideoPro/issues/426) | [T0.4] Pick the tester feedback channel | External access or owner decision | Title screen |
| [#425](https://github.com/iamfatness/CoreVideoPro/issues/425) | [T0.3] Acquire 2-3 reference machines (mid-tier GPU, hybrid-graphics laptop, AMD GPU) | External access or owner decision | Title screen |
| [#424](https://github.com/iamfatness/CoreVideoPro/issues/424) | [T0.2] Get the Zoom SDK redistribution answer | External access or owner decision | Title screen |
| [#423](https://github.com/iamfatness/CoreVideoPro/issues/423) | [T0.1] Buy the code-signing certificate (Authenticode / Trusted Signing) | External access or owner decision | Title screen |
