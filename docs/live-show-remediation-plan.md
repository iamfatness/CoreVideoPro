# CoreVideo Pro live-show remediation plan

Status: planning complete; implementation begins after the active show ends and plan review.

Planning baseline: `origin/main` at `9fb739d` (`beta-2026-09-08-9fb739d`). The local
`codex/rename-capture-engine` branch adds the approved Capture-to-Engine wording and
must be reviewed and integrated without losing newer fixes.

## Outcome

CoreVideo Pro has one authoritative, revisioned description of the meeting and the
show. Scenes, Program, Preview, Tiles, Multiview, audio routing, ISO selection,
lower thirds, and automation all consume the same committed revision. A Take is one
atomic render operation, including its selected transition. Departed or replaced
participants cannot leave an old frame or inherit another person's identity.

The beta gate requires stable 60 fps output on the designated rigs, successful
participant churn and scene-change soaks, usable support evidence, and a signed
Windows installer.

## Operating constraints

- Do not build, launch, restart, exercise the API, or manipulate the current meeting
  while the show is active.
- Preserve support evidence from the show before clearing logs or state.
- Implement in small `codex/` branches and merge only after review and the phase's
  acceptance gate passes.
- Keep wire changes additive until the shell and native core both consume the new
  contract. A legacy client may use the compatibility path, but the UI must identify
  that mode.
- Use stable, scoped source identities such as `zoom:<meeting-generation>:<user-id>`,
  `capture:<device-id>`, and `media:<asset-id>`. Never route by display name or row
  position.

## Root cause and target model

The current shell derives overlapping collections for the room roster, video-capable
participants, show inputs, scene routes, Tiles membership, Multiview, audio, and ISO.
It then sends multiple commands such as roster, scene graph, Preview, and Multiview in
sequence. During participant churn or a Take, consumers can observe different points
in time. Some UI controls, including Take effects, do not have a corresponding native
operation at all.

Introduce a canonical `ShowSessionSnapshot` and a serialized reducer:

- `session`: meeting ID hash, engine epoch, meeting generation, connection state.
- `participants`: stable identity, presence, camera/audio/share state, role, and the
  latest valid video-frame generation.
- `inputs[1..10]`: source identity, assignment intent, availability, subscription,
  delivered media health, audio route, and ISO intent.
- `scenes`: immutable scene documents containing stable source references, Tiles
  policy, background, graphics, and lower-third intent.
- `transport`: Program revision, Preview draft revision, pending/active transition,
  output lifecycle, and frame-buffer policy.
- `diagnostics`: bounded counters and state transitions rather than per-frame debug
  text unless enhanced diagnostics is enabled.

Meeting callbacks and operator commands become typed events applied by one reducer.
Each accepted change creates a monotonically increasing revision. The core applies a
complete render commit for that revision at a frame boundary and acknowledges applied
or rejected with a reason. UI screens project the last acknowledged snapshot instead
of maintaining independent routing truth.

## Delivery sequence

### PR 1 — canonical state contract and observability

Add versioned contracts for `ShowSessionSnapshot`, typed state events, render commits,
and acknowledgements. Add `revision`, `engineEpoch`, `meetingGeneration`, and
`operationId` to relevant messages. Build the reducer beside the current path and
compare its projections without controlling Program yet.

Instrument state changes with bounded counters: latest produced/applied revision,
rejected and superseded commits, source generations, queue depth, Take latency, render
deadline misses, and stale-frame prevention. Normal logging records lifecycle changes
and failures; the Health switch enables time-bounded verbose tracing.

Acceptance:

- Golden C#/C++ fixtures serialize identically and reject unsupported major versions.
- Replaying the same meeting/operator event stream produces the same snapshot.
- No display name, participant list index, UI row, or scene slot is used as identity.
- Diagnostics comparison shows where the legacy projections disagree without changing
  the live output.

### PR 2 — participant lifecycle and source ownership

Make the canonical participant registry the sole authority for who is in the meeting
and which media generations are valid. Treat camera off, leave, rejoin, SDK user-ID
reuse, and engine restart as explicit lifecycle transitions. Bind video and audio to
the same source identity.

When a source becomes unavailable, invalidate its texture generation immediately and
render a deterministic placeholder/transparent result according to scene policy.
Keep the operator's assignment intent so the same stable participant can recover, but
never keep the departed participant's last frame. A new participant or slot occupant
must receive a new source generation.

Acceptance:

- Selecting any source in Scenes retains that exact stable ID; it cannot resolve to
  Jamal or another participant by matching a stale index/name.
- Leave, camera-off/on, rejoin, rename, duplicate display-name, and ID-reuse fixtures
  cannot display a frame from the wrong generation.
- Video routing, audio routing, and ISO selection resolve the same source identity and
  report unavailable state consistently.
- Engine restart invalidates old callbacks and textures.

### PR 3 — unified show-input, Multiview, audio, and ISO projections

Remove screen-local roster/routing copies. Derive Sources, Scenes pickers, Multiview,
Audio, ISO, and diagnostics from `ShowSessionSnapshot`. Preserve pending operator edits
as a draft until Apply; live callbacks cannot overwrite an open selection.

Adopt one ten-input capacity contract end to end. Generate Multiview layouts for ten
source cells plus Program and Preview, update the explicit eight-source UI text, and
make limits capability-driven. Provide Tiles membership modes for `Routed sources
only` and `Selected plus eligible meeting participants`; manual includes/excludes take
precedence over autofill.

Acceptance:

- Ten assigned inputs appear and update in Multiview, with Program and Preview still
  visible.
- Every screen shows the same assignments, availability, audio role, and ISO state for
  a revision.
- Roster changes do not close pickers, replace pending choices, jump rows, or silently
  reroute sources.
- Tiles membership exactly follows the selected policy and contains no departed source.

### PR 4 — atomic scene publish and reliable Take

Replace the sequential `reason=take` synchronization batch with one immutable
`PrepareRenderCommit` followed by `CommitTake`. The core validates and preloads the
incoming scene, sources, Tiles members, background/media assets, graphics, lower third,
audio graph, ISO graph, and Multiview metadata. At a render-frame boundary it atomically
promotes the prepared revision to Program. Preview is rebuilt from the acknowledged
outgoing Program revision.

Do not optimistically swap local Program. The UI shows preparing, ready, taking,
completed, failed, or reconciling. Edge-triggered Take uses an operation ID and is never
blindly replayed after a timeout. Allow only one Take at a time; newer Preview edits
create a later draft rather than mutating the in-flight commit.

Acceptance:

- Pressing Take always produces a visible completed/rejected result within the stated
  timeout and cannot execute twice.
- Program never combines the new scene with the previous scene's background, Tiles,
  sources, audio, ISO, graphics, or lower third.
- Moving a Zoom feed or background into Program does not cause independent refreshes.
- Core restart or lost acknowledgement reconciles from the applied core revision.

### PR 5 — native Take transition engine

Replace the cosmetic selector with a typed `TransitionSpec`: cut, fade, dip, or wipe;
duration; wipe direction; and dip color. Carry it on `CommitTake`. Retain the outgoing
and incoming render graphs for the transition and animate them from the core render
clock. Release the outgoing graph only after completion. Publish queued, active,
progress, completed, and failed state.

Acceptance:

- Cut swaps at one frame boundary.
- Fade blends both complete scenes; Dip passes through the configured color; Wipe has
  a spatial reveal in the selected direction.
- Captured frame sequences prove the selected label matches the native effect and
  duration within one frame.
- Transition progress never depends on UI timers and cannot leave Program between
  scenes.

### PR 6 — Tiles, lower thirds, and Magic Scene correctness

Move Tiles membership/layout decisions into the canonical commit and keep the C++
compositor responsible for per-frame geometry. Bounds-check all members and overrides;
missing sources render policy-defined placeholders. Make background changes part of
the prepared graph. Add focused crash containment so a bad scene is rejected without
terminating the app or meeting engine.

Route lower-thirds through the same graphic lifecycle: build-in, live, build-out, and
cleared, keyed by source generation and render revision. Make Magic Scene produce a
Preview draft through a deterministic planner. It may never edit Program, start a Take,
or repeatedly react to its own changes. Present its proposed assignments before Take.

Acceptance:

- Repeated entry to and Take from a Tiles scene survives empty, 1, 8, 10, and maximum
  configured membership, plus rapid join/leave churn.
- Tiles backgrounds and feed positions remain attached to the committed revision.
- Lower thirds appear, animate, update, and clear on Program; source departure clears
  or substitutes according to policy.
- Magic Scene is idempotent for the same snapshot, changes Preview only, and never
  causes an event loop or uncontrolled routing.

### PR 7 — render pacing, flashing prevention, and selectable buffer

Trace frames end to end by source generation, timestamp, shared-texture fence, and
Program revision. The compositor may sample only a fully published texture matching
the expected generation. Hold the last valid frame only for a short camera jitter
window; a lifecycle departure still invalidates it immediately. Eliminate texture
reuse before consumer completion and mixed-generation Multiview/Program sampling.

Add an operator-selectable two- or three-frame input jitter buffer. At 60 fps these are
approximately 33.3 ms and 50.0 ms of buffering. Use timestamp ordering and a bounded
late-frame policy; never allow an unbounded queue. Keep the render clock independent
of Task Manager, diagnostics panels, and logging work. Move expensive diagnostics off
the render path and leave detailed profiling disabled by default.

Acceptance on each designated rig:

- Program and Multiview show no black/foreign/old-generation flashes during source
  churn, scene changes, and Take effects.
- The selected two- or three-frame buffer is reported accurately and remains bounded.
- A 60-minute 1080p60 rehearsal has zero compositor deadline misses attributable to
  CoreVideo under the agreed production workload; record GPU/CPU utilization, ingest
  late frames, render p50/p95/p99/max, queue depth, and A/V drift.
- Opening Task Manager and Health does not cause a sustained frame-drop burst.
- If the hardware or SDK input cannot deliver 60 unique frames, diagnostics distinguish
  ingest lateness from compositor deadline failure instead of hiding it.

### PR 8 — Engine lifecycle and operator recovery

Complete the approved rename to **Engine** everywhere. Define it as Zoom media ingest
and subscription control; it does not depend on Zoom cloud/local recording rights.
Expose a compact state machine: Off, Starting, On, Degraded, Stopping, Failed. The
media-core and Zoom SDK windows remain intentionally hidden in production, while
Health provides process state, PID, last exit, restart, and recovery guidance.

Acceptance:

- Turning Engine on/off changes media ingestion and surfaces without implying Zoom
  recording permission.
- A hidden SDK helper is distinguishable from a missing/crashed helper in Health.
- Restart invalidates the old epoch, restores desired subscriptions from the canonical
  snapshot, and cannot accept stale callbacks.

### PR 9 — beta qualification, signing, and release evidence

Sign the application executables and installer with one trusted code-signing identity,
timestamp signatures, and verify the chain on a clean Windows machine. Confirm that
the installer contains only the intended signed binaries. Keep verbose diagnostics off
by default and verify that the Health switch and support bundle still provide enough
evidence to diagnose a failure.

Run the release candidate through automated contract/reducer/compositor tests, packaged
API smoke tests, a participant-churn matrix, scene and effect drills, and a two-hour
record/stream/ISO soak. Test both two- and three-frame buffering. Include the 11th-gen
i7-11370H/RTX A2000 system as a lower-tier reference and a stronger designated 60 fps
rig. Archive exact commit, installer hash, signatures, versions, GPU/driver, settings,
and performance results.

Acceptance:

- Clean install, upgrade, uninstall, launch, engine recovery, and support-bundle tests
  pass on a machine without development tools.
- SmartScreen identifies the configured publisher; all shipped executables validate
  against the same publisher certificate.
- The two-hour run has no crash, UI hang, stale/wrong-person frame, state divergence,
  duplicate Take, or unbounded memory/queue growth.
- Every advertised 60 fps configuration meets the PR 7 deadline and A/V-sync gates.
- GitHub beta assets and release notes identify the exact tested commit and installer
  SHA-256.

## Required scenario matrix

Every affected PR adds deterministic fixtures where possible. The final packaged pass
must exercise:

1. Ten participants assigned; duplicate display names; rename; camera off/on.
2. Participant leaves and another joins; old frame never appears in the replacement.
3. Video source, audio bus, and ISO selections compared on every screen.
4. Static scene and Tiles scene moved Preview to Program with every Take effect.
5. Tiles manual-only, routed-only, and selected-plus-eligible membership modes.
6. Background change, media playback, lower third, and Magic Scene proposal during
   participant churn.
7. Engine off/on, helper crash/restart, media-core restart, and lost Take acknowledgement.
8. 1080p60 with two-frame and three-frame buffers; Health opened; Task Manager opened;
   recording, streaming, and ISOs active.

## Merge and rollback policy

PRs 1 through 4 are the critical path and merge in order. PR 5 depends on atomic Take.
PR 6 depends on canonical scene commits. PRs 7 and 8 can proceed after the state
contract stabilizes, then PR 9 qualifies the combined candidate.

Each migration keeps an additive compatibility adapter for one release. Rollback is by
whole acknowledged revision or feature flag, never by partially replaying a Take batch.
A failed prepared commit leaves current Program untouched. No phase is complete based
only on a successful build or unit suite; the relevant scenario and packaged acceptance
evidence are required.
