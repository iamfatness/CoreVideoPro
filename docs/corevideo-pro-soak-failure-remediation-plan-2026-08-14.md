# CoreVideo Pro soak-failure remediation plan

Date: 2026-08-14
Input evidence: `artifacts/soak-20260814-125948/`
Current launch decision: **NO-GO**
Primary objective: make Program + eight Zoom ISO A/V recording trustworthy for a live producer before adding more launch surface.

## Definition of success

CoreVideo Pro is launchable when one 60-minute real Zoom rehearsal can record Program plus eight ISOs through normal production actions without recording segmentation, unbounded A/V drift, hidden drops, resource escalation, or contradictory UI/support evidence. A forced termination during a separate take must leave every committed recording stream recoverable.

The implementation must preserve the positive soak results:

- independent audio in every ISO;
- full-range BT.709 metadata and conversion;
- bounded async encoder queues;
- quick normal finalization;
- clean Stop/Leave resource teardown;
- good Zoom ingest latency.

## Engineering rules

1. **Transport commands are edges, not desired-state refresh commands.** Scene synchronization may update routes and plans, but must not reopen or finalize writers.
2. **One recording session owns one immutable epoch and one folder.** Scene changes do not create a new recording generation.
3. **Capture/submission time is authoritative.** Writer-thread scheduling time must never become media PTS.
4. **Sink facts beat estimates.** UI and support bundles report actual stream counters, bytes, timestamps, paths, encoder selection, and drops.
5. **Program is priority one.** Under pressure, ISO work degrades explicitly before Program A/V or render control stalls.
6. **Every release claim has a destructive or long-running proof.** A manifest flag is not proof of crash safety; a unit counter is not proof of operator visibility.

## Work order

### Milestone 0 — Preserve the baseline and turn failures into automated red tests

**Purpose:** prevent the current dirty worktree and successful ISO-audio work from being obscured by broad fixes.

Actions:

- Preserve the exact soak report, support bundle, recording manifests, and process samples as the failure fixture.
- Record the current worktree SHA/diff as the baseline; do not discard the existing ISO audio, queue fairness, color, OAuth, and warm-Preview changes.
- Add failing harness cases before changing behavior:
  - one recording start followed by repeated scene sync, Preview queue, Take, and unrelated settings sync must retain one writer generation, folder, and session ID;
  - 30-minute synthetic Program + eight-ISO timestamp run;
  - slow-encoder/drop accounting reconciliation;
  - repeated Zoom resolution promotion/demotion policy;
  - support-bundle counters reconciled against sink/file counters.
- Demote the existing “crash-safe fragmented MP4 — Proven” claim in `docs/obs-real-meeting-parity-audit-2026-08-13.md` until the forced-kill rig gate passes.
- Remove the double-start assumption from `docs/iso-record-spec.md`; it currently documents the behavior that caused the shard failure.

**Exit:** each launch blocker has a deterministic failing test or rig script and the successful ISO-audio/color tests remain green.

### Milestone 1 — Make recording lifecycle idempotent

**Failure addressed:** Preview and Take created new recording folders inside one take.

Implementation:

- Split scene/render synchronization from transport lifecycle synchronization in `MediaCoreCommandBuilder`.
- Emit `start-program-output`, `start-recording-session`, `stop-recording-session`, and output teardown only on explicit state transitions.
- Carry an operator-generated recording session ID from the Record action through every sync until Stop completes.
- Make `MediaCore::startProgramOutput()` compare the requested output configuration with the active encoder session. A same-session/same-config command is a no-op, not `encoder->start()`.
- Reject or explicitly segment incompatible recording changes while a take is active; never silently reopen writers.
- Expose `Idle → Arming → Recording → Finalizing → Idle/Failed` as the one transport state machine consumed by the button, bottom status, diagnostics, and support bundle.
- Remove/update native tests whose expected behavior is “start-program-output arms generation 1, then start-recording-session creates generation 2.”

Primary files:

- `native-shell/CoreVideoPro.MediaCore/Services/MediaCoreCommandBuilder.cs`
- `native-shell/CoreVideoPro.WinUI/ViewModels/Transport/TransportCoordinator.cs`
- `native/src/core/MediaCore.cpp`
- `native/src/modules/MediaFoundationEncoderAdapter.cpp`
- corresponding C# and native command/recording tests

Acceptance:

- 100 state syncs, 20 Preview selections, and 20 Takes during one recording create exactly one folder and one Program/ISO file set.
- Writer generation, session ID, file paths, and A/V track presence remain unchanged until Stop.
- Repeated Start is idempotent; repeated Stop is idempotent; rapid Stop/Start cannot overlap finalization.

### Milestone 2 — Prove real crash durability

**Failure addressed:** files stayed zero/tiny during the live take despite a 1-second fragment declaration.

Implementation:

- Add live writer telemetry for committed fragment count, committed duration, committed bytes, last commit age, and byte-stream flush result per stream.
- Verify the actual Media Foundation fragmented sink behavior with Program + eight simultaneous writers. Do not infer durability from `MF_MPEG4SINK_MIN_FRAGMENT_DURATION` alone.
- If periodic byte-stream flushing makes completed fragments durable, perform it off the realtime/render path on a bounded cadence.
- If the Media Foundation sink cannot guarantee bounded recovery for this topology, use deterministic short segment rollover with a session manifest and seamless post-import ordering. Do not retain the `crashSafe` label without the forced-kill proof.
- Ensure Stop/finalize remains an async control barrier and that a failed ISO finalize cannot block Program finalization.

Tests:

- Force-kill the native process at 10 s, 2 min, and 10 min with Program + eight ISOs; no destructor or Stop call.
- Reopen every surviving file/segment through both `IMFSourceReader` and `ffprobe`.
- Assert recovered duration is within one commit interval plus one GOP of the kill point.
- Repeat while one writer is slow and while the target volume is nearly full.

Acceptance:

- All nine streams have advancing committed bytes/duration during the take.
- A forced kill loses no more than the documented recovery window.
- Manifest `crashSafe`, fragment duration, and recovery telemetry describe measured behavior.

### Milestone 3 — Replace writer-time PTS with one session clock

**Failure addressed:** Program had an 18.951-second A/V duration mismatch in a 20-minute shard; ISOs differed by as much as 5.261 seconds.

Implementation:

- Define a `RecordingTimeline` owned by the recording session, anchored once on arm.
- Timestamp Program video, ISO video, Program audio, and ISO audio at capture/compositor submission boundaries in the same steady-clock domain.
- Keep audio duration sample-counted at 48 kHz, but map its first sample and resync checks to the session epoch.
- Pass Program video submission timestamps into the encoder instead of sampling `now100ns()` on the writer thread.
- Continue per-source frame-ID dedup, but do not let frame arrival rate or queue latency stretch the timeline.
- Add bounded correction policy for genuine source-clock divergence; log insert/drop corrections by stream.
- Treat stream duration divergence as a recording warning during the take, not only a post-process discovery.

Tests:

- Extend `RecordingPtsClock` tests to 60 simulated minutes with queue jitter, writer delay, frame duplicates, gaps, and ISO audio gating.
- Extend `validate-av-clap.mjs` and `validate-record-audio.mjs` to measure content sync at head, middle, and tail.
- Probe every output stream for monotonic PTS/DTS and compare audio/video end times.

Acceptance:

- Head/middle/tail content A/V offset stays within ±40 ms for Program and each ISO in a 60-minute run.
- Audio/video duration difference is ≤ one video frame plus one AAC packet.
- No stream PTS is derived from encoder queue service time.

### Milestone 4 — Make drops, recording health, and support evidence truthful

**Failures addressed:** 1,499 logged render drops appeared as zero; bundle bytes/frames/audio did not match files.

Implementation:

- Define distinct monotonic counters for:
  - compositor deadline misses;
  - Program video/audio queue drops;
  - per-ISO video/audio queue drops;
  - source stale/late frames;
  - encoder write failures and backlog.
- Wire `AsyncEncoderSink` counters into `OutputSession`, native snapshot, the three protocol mirrors, the C# models, Studio transport, diagnostics, manifests, and support bundles.
- Replace `recordingDroppedFrames_` with aggregated real counters; remove dead counters.
- Remove synthetic byte estimates and the hard-coded 4.99 MB/s rate from `MediaCore::recordingState()`.
- Report actual `Mp4Writer` bytes, frames, audio samples, committed duration, selected encoder path, and warnings per stream. After Stop, reconcile against filesystem size and container metadata.
- Populate operator actions/event history for record start/stop, scene sync, fallback, warning, and failure transitions.
- Add a visible warning threshold before Program quality is affected; ISO-only degradation must identify the affected track.

Acceptance:

- UI, manifest, snapshot, and support bundle agree exactly on session ID, paths, stream count, frames, audio presence, drops, and final file bytes.
- A deterministic slow-writer test creates known ISO drops; all four evidence surfaces show the same number and Program remains unaffected.
- A zero-drop test reports zero everywhere without relying on log parsing.

### Milestone 5 — Add an active capacity governor and remove lock starvation

**Failures addressed:** all eight Zoom feeds ratcheted to 1080p; memory/GPU stayed at peak until Leave; command sync stalled render for up to 142 ms.

Implementation — Zoom subscriptions:

- Replace upgrade-only behavior with a desired-resolution planner using Program, Preview, active-speaker, Tiles, multiview, and ISO priorities.
- Apply both upgrades and downgrades with hysteresis and minimum hold times to avoid subscription thrash.
- Publish requested vs actual resolution, reason, decoder cost, last change, and downgrade failures per source.
- Add a session budget for total decoded pixels/second and GPU memory, not just source count.

Implementation — ISO encoding:

- Wire the existing `IsoEncoderPlacement` policy into actual Media Foundation writer creation.
- Reserve hardware capacity for Program/stream first, then assign ISOs to hardware; overflow uses the Media Foundation software H.264 transform.
- Make hardware preference a real per-writer MF attribute and expose selected path/fallback reason.
- Never switch encoders silently inside a file; startup failure may demote before first sample, runtime failure rolls a manifest segment.

Implementation — locks:

- Move writer open/close/configure and all file/codec work outside `coreMutex`.
- Build immutable command/config snapshots under the lock, execute slow work asynchronously, then publish the result with generation checks.
- Reduce work in `render.display-tick` and `audio.gather`; preserve the documented lock order.

Acceptance:

- Rotating active-speaker importance through eight guests causes old feeds to downgrade after hysteresis; helper memory/GPU plateaus instead of ratcheting to eight 1080p feeds.
- Forced hardware-session exhaustion produces the planned GPU/CPU distribution and a playable ISO for every accepted track.
- Scene sync, Take, Record, and Stop perform no command-lock hold over one frame budget; no operator action produces a >50 ms render frame.
- A 30-minute Program + eight-ISO run has zero unexplained compositor drops; under forced pressure, only identified ISO tracks drop.

### Milestone 6 — Repair operator health, logs, and identity handling

**Failures addressed:** optional silent GoXLR input poisoned global health; “Show error” appeared during healthy recording; logs were unbounded; UTF-8 names were corrupted.

Implementation:

- Evaluate audio health per required path. A silent optional local source warns on its row but does not invalidate Zoom meeting mix, Program, or recording proof.
- Derive recording proof from actual Program and ISO audio packets/tracks, not generic local capture status.
- Replace “Show error” with state-appropriate language and ensure every recording readout consumes the same transport state.
- Add one shared rotating writer for `launch.log`, `media-core.log`, and `perf.log` with bounded total size, atomic rollover, and support-bundle awareness.
- Rate-limit unchanged steady-state telemetry; always preserve state transitions, first failures, recovery, and periodic summaries.
- Normalize UTF-8 once at the Zoom IPC boundary. Preserve the exact display name in roster, manifest, diagnostics, and support bundle; sanitize filenames without mojibake.

Acceptance:

- A silent optional mic yields one scoped warning while Zoom/PGM/ISO recording health remains Good when those paths are proven.
- The UI never simultaneously displays Idle and Recording, or error affordances for a normal state.
- Continuous logging remains within the configured cap through a 24-hour synthetic run and exported bundles contain the relevant transition history.
- `Elena Kovač` survives Zoom → native core → WinUI → manifest/support bundle exactly; its filename is deterministic and valid.

### Milestone 7 — Correct documentation and run the launch gate

Update these contracts to match the fixed architecture and measured evidence:

- `docs/obs-real-meeting-parity-audit-2026-08-13.md`
- `docs/iso-record-spec.md`
- `docs/audio-overhaul-spec.md`
- `docs/corevideo-tiles-iso-scaling-plan.md`
- operator validation and show-drill runbooks

Run two qualification passes:

#### Pass A — 60-minute non-destructive production rehearsal

- Eight Zoom participants, Program + eight ISOs, Preview and Program visible.
- At least 20 Takes, active-speaker churn, mute/unmute, camera off/on, one leave/rejoin, screen-share start/stop, and one Zoom helper restart.
- Hardware capacity forced low enough to exercise at least one CPU ISO.
- Capture metrics every five seconds and export the final support bundle.

Required result:

- one logical recording folder/session;
- nine playable independent A/V outputs;
- Program and every ISO within the A/V limits above;
- no hidden Program drop or lock stall;
- truthful UI/support evidence;
- memory reaches a plateau and recovers after Stop/Leave;
- no warning persists after its cause recovers.

#### Pass B — destructive recovery rehearsal

- Start a fresh Program + eight-ISO recording.
- Force-terminate the native engine at a randomized point after at least 10 minutes.
- Restart/recover, then inspect every artifact without running a normal finalizer.

Required result:

- every committed stream opens;
- loss is within the documented recovery window;
- recovery creates an explicit continuation segment/session event;
- the support/crash bundle identifies the last committed duration and affected streams.

Only after both passes are retained as release evidence can the launch decision move from NO-GO to GO.

## Proposed PR sequence and rough effort

| PR | Deliverable | Dependency | Rough effort |
|---|---|---|---:|
| 1 | Failure harnesses + contract corrections | none | 0.5–1 day |
| 2 | Edge-triggered/idempotent recording lifecycle | PR 1 | 1–2 days |
| 3 | Durable fragmented output or bounded segment fallback | PR 2 | 2–3 days |
| 4 | Unified capture-time recording timeline | PR 2 | 2–3 days |
| 5 | Real drop/stream counters + support/manifest reconciliation | PRs 2–4 | 2–3 days |
| 6 | Resolution governor + real GPU/CPU ISO placement | PR 2; can overlap PRs 3–5 | 3–5 days |
| 7 | Lock reduction, audio-health UX, rotation, UTF-8 | PRs 2 and 5 | 2–3 days |
| 8 | 60-minute and destructive launch qualification | all | 1–2 days plus soak time |

Expected total for one focused engineering lane: roughly **13–22 engineering days**, including test/harness work but excluding unrelated Tiles UI implementation. Two lanes can overlap durability/timeline work with the capacity governor after PR 2.

## Product sequencing

- Do not expand CoreVideo Tiles UI while Milestones 1–4 are red; a premium gallery cannot compensate for untrustworthy recordings.
- Continue the ISO GPU/CPU placement work in Milestone 5 because the mixed-path soak is part of recording correctness, not optional polish.
- Resume Tiles implementation after recording lifecycle, durability, A/V, and telemetry gates are green. Tiles then uses the same capacity governor, render telemetry, media decoder, and launch rehearsal rather than creating parallel infrastructure.
