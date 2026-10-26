# Render gather: stop taking the Zoom engine mutex under coreMutex — design

**Date:** 2026-10-23
**Status:** implemented on `codex/rc-1037` (Outcome below); owner ruling D17 recorded under Risks. Follow-ups #1048/#1049 implemented on `codex/rc-1049` (addendum at the end)
**Issue:** [#1037](https://github.com/iamfatness/CoreVideoPro/issues/1037)
**Related:** #1030 / PR #1035 (the audio-gather fix and the `EngineMutexGuardrail` this design reuses), #517 / #870 (realtime isolation), `docs/reference/realtime-isolation-audit.md`
**Line numbers:** `main` at `1879ccb7` (the #1035 merge). Re-grep before editing; they move.

## Why

### The defect

The render thread holds `coreMutex` for the whole display tick
(`native/src/rpc/JsonRpcServer.cpp:691-698`, site `render.display-tick`). Inside
it, `MediaCore::renderSyntheticTick` reads the Zoom roster with
`zoomEngineRuntime_->floorParticipants()` (`native/src/core/MediaCore.cpp:5233`).
That call takes `ZoomEngineRuntime::mutex_` and copies every participant struct.

`mutex_` is not a short leaf lock. It is held:

- across `ZoomEngineRuntime::applyEvent` (`ZoomEngineRuntime.cpp:963-997`): on every
  roster event it builds **two full `state_.snapshot()` copies** (participants,
  subscriptions, events, warnings) and, when the revision moved, a
  **`rawCaptureSnapshotLocked()` roster-fact JSON** of every participant;
- across the video-ingest thread's poll and the frame-sync slot accounting in
  `latestDecodedVideoFrames` (`ZoomEngineRuntime.cpp:757-`).

So on roster churn (people joining, leaving, muting, starting video) the render
tick blocks on `mutex_` while holding `coreMutex`, and every other `coreMutex`
waiter waits behind it: the command loop (`cmd.handle`, 1,000 us budget), the
audio/output worker (`audio.gather`, 1,000 us), the resource pump and the
preview pump. This is the class #1035 removed from the audio gather. The comment
above `MediaCore::pollZoomAudioUnlocked` (`MediaCore.cpp:6174-6186`) records the
measurement that motivated it: taking the engine mutex under `coreMutex` measured
**8.5 ms worst against a sub-ms budget** while the rest of the gather stayed
under 0.03 ms, and the 0.16 ms mean hid it.

**A correction to the issue text.** The render tick's own budget is
`kRenderTickBudgetUs = 8,000 us` (`LockHoldGuardrail.h:46`), not sub-ms. The
sub-ms budgets belong to the sites that wait behind it. The harm is a long
render hold multiplied across every other `coreMutex` client, plus the render
thread's own deadline.

### Evidence from the 2026-10-23 live show (read-only)

`D:\CoreVideo-QA\soak-20261023-01\media-core-final.log`, build
`1.0.0+1072a50a` (before #1035: no engine-mutex guardrail, so no line attributes a
hold to the engine mutex). The log rolled over, so the retained window is
10:54:28 to 11:40:49 (46 min). It has 33 `[lock-guardrail]` lines. The counters
are cumulative for the process:

| site | budget | over budget / holds (first line in window, then last) | worst in process |
|---|---|---|---|
| `render.display-tick` | 8,000 us | 152 / 408,517, then 156 / 428,839 (0.04 %) | **26,168 us** |
| `cmd.handle` | 1,000 us | 4,143 / 47,877, then 5,533 / 64,529 (8.6 %) | **10,111 us** (first logged 10:56:35) |
| `audio.gather` | 1,000 us | 312 / 340,545, then 327 / 402,462 | 6,098 us (pre-#1035 build) |
| `rpc.resource-drain` | 1,000 us | 21 / 677,585 | 4,822 us |

What this does and does not establish:

- Holds on the render tick are rare but long: a 26 ms worst is more than a
  60 fps frame and a half. While the render tick holds `coreMutex`, every
  `cmd.handle` and `audio.gather` acquisition waits. That wait is lock **wait**,
  which those sites' timers do not count, because they start after acquisition.
- `cmd.handle`'s own 8.6 % over-budget rate and its 10,111 us worst happen
  **inside** command handling. This slice will probably not fix them. The most
  likely in-command engine-mutex hold is `zoom-media-spine-sync` (inventory row
  C1). It is periodic and runs all of `ZoomEngineRuntime::syncSpine` under
  `mutex_`. That is a hypothesis: Task 1 of the plan attributes it, and its fix
  is a separate slice.
- No line attributes any hold to `floorParticipants`. The issue says this
  plainly. The design is justified by the lock structure plus a reproducible
  churn measurement (Acceptance 2), not by the soak alone.

## Inventory: every engine-mutex acquisition reachable under coreMutex

The method: grep every `zoomEngineRuntime_->` call in `native/src` (MediaCore.cpp,
MediaCoreMonitors.cpp and CoreStateCapture.cpp are the only callers). Map each
call to its enclosing function, find that function's callers, and classify the
lock context. The `coreMutex` holders are `render.display-tick` (render thread),
`cmd.handle` (every command except `zoom-join` with an engine and
`register-grade-document`; `JsonRpcServer.cpp:1171-1188`), `audio.gather` /
`audio.publish`, `video.gather`, `cmd.frame-pump`, `rpc.resource-drain` and
`rpc.metadata-capture`. Only the first two reach the engine.

**Count: 14 production call sites take the engine mutex while holding coreMutex**
(9 on the render tick, 5 on the command lane). Another 11 entries are listed
below so that the classification is complete: they are lock-free, off-lock,
unreachable with an engine, or test-only. Every entry has a decision.

### Render tick (`render.display-tick`, coreMutex held by `JsonRpcServer.cpp:691`)

| # | Site | Engine call (what it holds `mutex_` for) | Decision |
|---|---|---|---|
| R1 | `MediaCore.cpp:5095` | `pollCompositorVideoFrames` (subscription stats walk) | **Justify this slice, follow-up issue.** The frame-selection group. See "Why the frame group stays". |
| R2 | `MediaCore.cpp:5098` | `latestDecodedVideoFrames` (frame-sync `takeDueZoomVideo(nowTp)` + slot accounting) | **Justify this slice, follow-up issue.** Same group. |
| R3 | `MediaCore.cpp:5161` | `setVideoConsumerDemand` (rebuilds a set, may enqueue `iso_video` sends) at 60 Hz | **Move out.** It is applied before the lock from the demand published on the previous tick. |
| R4 | `MediaCore.cpp:5233` | `floorParticipants` (copies every participant) | **Move out.** This is the issue. Pre-read before the lock. |
| R5 | `MediaCore.cpp:5292` | `appendCpuVideoFrames` (moves ISO CPU arrival frames) | **Justify this slice, follow-up issue.** Same group (a frame transfer). |
| R6 | `MediaCore.cpp:652` via `followSpeakerForRoutes`, called from `buildRenderPlanForScene` (`:4567`) | `directedSpeakerId` | **Move out.** It reads the pre-read speaker. It is also reached from `cmd.handle` through `armTakeRecord` (`:1919`), and that path is fixed by the same change. |
| R7 | `MediaCore.cpp:3764` in `buildMultiviewRenderPlan`, called from `:5849` and `MediaCoreMonitors.cpp:42` | `zoomSnapshot()` → `ZoomEngineRuntime::snapshot()`: a **full roster JSON build** for one string | **Move out.** It reads the pre-read speaker. |
| R8 | `MediaCore.cpp:5865` (multiview tiles) | `zoomSnapshot()`: the same full JSON build | **Move out.** It reads the pre-read speaker. |
| R9 | `MediaCoreMonitors.cpp:44` (isolated-monitor tiles) | `zoomSnapshot()`: the same full JSON build | **Move out.** It reads the pre-read speaker. |

R7 to R9 are worse than the issue's target. `kMultiviewTickDivisor = 1`
(`MediaCore.cpp:5831`), so with multiview active and the D3D11 isolated monitor
worker present, **every tick builds two or three complete roster JSON documents
under the engine mutex while holding coreMutex**, only to read
`activeSpeakerId`.

### Command lane (`cmd.handle`, coreMutex held by `JsonRpcServer.cpp:1171`)

| # | Site | Engine call | Decision |
|---|---|---|---|
| C1 | `MediaCore.cpp:762` from `zoom-media-spine-sync` (`JsonRpcServer.cpp:107`); the shell sends it repeatedly | `syncSpine` (subscription diff + spine snapshot JSON, all under `mutex_`) | **Move out, in a follow-up slice (command lane).** It is the leading suspect for the `cmd.handle` 10,111 us worst. The fix is a two-phase handler: apply multiview and preview under coreMutex, then call `syncSpine` after release, the way `zoom-join` already runs lock-free. Command-ordering semantics need their own design. Plan Task 1 measures it, and Task 9 files the issue. |
| C2 | `MediaCore.cpp:668` from `zoom-snapshot` (`JsonRpcServer.cpp:153`); the shell polls it (`MediaCoreBridgeService.cs:725`) | `snapshot()` (full JSON) | **Move out, in the same follow-up slice.** With an engine configured it is a pure runtime read and could be handled without coreMutex. Kept out of this slice to keep #1037 on the render lane. |
| C3 | `MediaCore.cpp:2729` `resolveIsoDisplayName`, from `configureEncoderRecordingRequest` (`:2687`; start-program-output / start-recording-session) | `zoomSnapshot()`, **once per ISO source** | **Move out (this slice).** It reads the display name from the tick-cached pre-read roster. The cold-start fallback is described under "Design". |
| C4 | `MediaCore.cpp:620` `leaveZoom` | `leave()` (closes streams, empty-roster barrier) | **Justify.** It runs at operator rate, once per meeting, and the empty-roster barrier must serialize with core state. |
| C5 | `MediaCore.cpp:631` `stopZoomCapture` | `stopCapture()` | **Justify.** It runs at operator rate and only enqueues. Its snapshot is the same full JSON as C2. That cost is recorded in the C1/C2 follow-up. |

### No engine mutex under coreMutex (complete-inventory rows)

| # | Site | Why it needs no action |
|---|---|---|
| N1 | `configured()` (everywhere) | It has been lock-free since #1035 (`configuredMirror_`). |
| N2 | `speakerEpoch()` `:496, :651, :5232/5234, :5286`; `subscriptionChurnTotal()` `:1947, :1982`; `setThumbnailDemand` `:1194` | These are atomics. |
| N3 | `MediaCore.cpp:5262` `zoomSnapshot()` in the `!engineLive` branch | The stub path, where `configured()` is false. It builds no engine call. |
| N4 | `MediaCore.cpp:1510, :1558` `zoomReadinessState` / `zoomEvidenceState` | They are reached only from the stub branch of `zoomSnapshot()` (`:675, :710`), so they are unreachable with an engine. |
| N5 | `MediaCore.cpp:1441` `deriveDirectorSignals` (→ `zoomSnapshot()`) | Only `DirectorTest.cpp` calls it. Production uses `CoreStateCapture`'s copy (N7). |
| N6 | `MediaCore.cpp:605` `joinZoom` → `join()` | It runs on the join worker without coreMutex (`JsonRpcServer.cpp:1019-1040`). The `handle()` path is reached only without an engine. |
| N7 | `CoreStateCapture.cpp:2333, 2354, 2369, 2417, 2600` | These run in `assemble()` on the response assembly lane, after coreMutex is released (`cmd.handle` always installs the snapshot deferrer). |
| N8 | `MediaCore.cpp:899` `drainFrameEvents` | It runs on the Zoom pump thread with no coreMutex (`JsonRpcServer.cpp:841-848`). |
| N9 | `MediaCore.cpp:6199, :6225` audio poll + roster ids | They run before the lock on the audio worker (#1035). The inline call at `:6090` runs only without a worker (test/direct path), where there is no contention. |
| N10 | `renderSyntheticTick()` from command handlers (`:1097, :2472, :2619, :2919`) | Only when `!audioWorkerActive_` (test/direct callers). These callers use the same pre-read, computed inline. |
| N11 | `renderVideoOutputTick` (`video.gather`), `publishAudioOutputResults`, resource/preview pumps | These make no engine calls. |

### Why the frame group (R1, R2, R5) stays this slice

These three calls are the frame-selection point. `latestDecodedVideoFrames`
runs frame sync against `steady_clock::now()`. Pre-reading it before the render
thread waits for `coreMutex` would age every Zoom frame by that lock wait (0 in
the common case, up to a command hold, measured to 10 ms) and could select an
older frame. That is a latency change on the north-star path and needs its own
A/B, not a lock refactor. They stay inside an explicit, greppable sanction
(`AllowEngineMutexScope`, below), so the guardrail covers the rest of the tick.

Their exposure is the producer's hold, and this slice shrinks it: `applyEvent`
stops building two full `snapshot()` copies per roster event (Task 7). The
follow-up issue for the group carries the per-call wait numbers from Task 1.

## What the render gather actually needs from the roster

| Consumer (render tick) | Fields read |
|---|---|
| `graphicsInstances_.observeSources` → `GraphicsSourceCatalog::observeSources` | `id`, `sourceGeneration`, `displayName` (identity, size check, label) |
| guest A/V offset pruning (`:5237-5251`) | `id` (presence set) |
| `speakerFloor_` observations (`:5252-5258`) | `id`, `isHost`, `isMe`, `directorExcluded`, `isTalking` |
| graphics epoch confirmation (`:5232, :5234`) | `speakerEpoch()` read before and after the roster |
| follow-speaker routes (R6), multiview and monitor tallies (R7 to R9) | the directed speaker id, with the epoch read first (L3, `:649-652`) |
| C3 (`resolveIsoDisplayName`) | `id`, `displayName` (with the `"Zoom User <id>"` fallback the snapshot applies) |

`persistentId`, `hasVideo`, `isMuted`, `isSharingScreen` and `networkQuality`
are not read.

**The cheapest pre-read shape: one engine-mutex acquisition per tick, off coreMutex.**

```cpp
// modules/ZoomEngineRuntime.h — new
struct ZoomRenderView {
  std::vector<ZoomEngineParticipant> floorParticipants;  // == state_.floorParticipants()
  std::string directedSpeakerId;                          // after advanceActiveSpeaker(now)
};
// One mutex_ acquisition. Call it OUTSIDE coreMutex (#1037). nullopt = not configured.
[[nodiscard]] std::optional<ZoomRenderView> renderViewIfConfigured();

// core/MediaCore.h — new
struct RenderEngineInputs {
  bool engineConfigured = false;
  bool valid = false;                      // a pre-read happened (false = never read)
  std::uint64_t speakerEpoch = 0;          // read BEFORE the view (L3 order)
  std::uint64_t rosterSpeakerEpoch = 0;    // speakerEpoch if unchanged across the view, else 0
  std::vector<modules::ZoomEngineParticipant> floorRoster;
  std::string directedSpeakerId;
};
[[nodiscard]] RenderEngineInputs prepareRenderEngineInputsUnlocked();
```

It keeps `std::vector<ZoomEngineParticipant>` and does not use a slimmer struct.
`GraphicsInstances::observeSources` takes that type. The copy now happens off
coreMutex, so slimming it would buy almost nothing and would change a graphics
API.

**Rejected: an atomically published immutable roster from the engine.** The
render side would wait for nothing. The engine would have to republish at every
mutation of `participants_` and of the speaker director (`apply`,
`setSpeakerExclusions`, `setSpeakerSources`, `leave`, `reset`,
`advanceActiveSpeaker`), and missing one leaves the render on a roster that is
stale **indefinitely**. That failure is worse than the one-tick staleness of a
pre-read. The pre-read still waits on `mutex_`, but on the render thread with
coreMutex free, so commands and audio proceed. Revisit the published snapshot
only if Task 8 shows the render thread's own deadline is still hurt.

**The directed speaker is read with the director advanced.** The three snapshot
callers (R7 to R9) relied on `rawCaptureSnapshotLocked()` calling
`state_.advanceActiveSpeaker(now)`. `renderViewIfConfigured` keeps that tick and
reads `directedSpeakerIdString()` after it. `ZoomActiveSpeakerDirector::tick` is
purely time-driven (`ZoomActiveSpeakerDirector.h:82-96`): repeated calls only
make promotions on schedule. Two side effects change:

1. The director is now ticked at 60 Hz even with multiview off. Before, it was
   ticked by every snapshot: spine sync, command snapshots, roster facts, and
   multiview ticks. Follow-speaker promotion becomes consistently on time
   instead of depending on whether a multiview happened to be open.
2. The render no longer bumps `fallbackTick_`, so the snapshot `"tick"` field
   advances more slowly. No shell production code reads `"tick"`. Only
   `CoreProtocolParserTests.cs` fixtures contain it.

## Design

1. **Guardrail sanction.** `EngineMutexGuardrail.h` gains `AllowEngineMutexScope(const char* reason)`.
   It saves the thread's forbidden depth, sets it to 0, and restores it in its
   destructor. It is the only way to take the engine mutex inside a forbidden
   scope, so `grep AllowEngineMutexScope` lists every sanctioned exception.
2. **Scope placement.** The 3-argument `renderDisplayTick(slot, anchor, RenderEngineInputs)`
   opens `ForbidEngineMutexScope("render.display-tick")` around its call to
   `renderSyntheticTick(true, …)`, which covers the whole production render tick.
   R1, R2 and R5 sit inside `AllowEngineMutexScope("render.frame-group #1037-followup")`.
   The command-lane `renderSyntheticTick()` calls (N10) are not covered. They are
   the test/direct path.
3. **Pre-read before the lock.** The render loop in `JsonRpcServer.cpp` calls
   `mediaCore_.prepareRenderEngineInputsUnlocked()` **before** taking `coreMutex`
   and passes the result in. The function touches only `zoomEngineRuntime_`,
   which is created once and never reset, so it is the same justification as
   `pollZoomAudioUnlocked`. It also applies the demand published on the
   previous tick (R3, below). The 2-argument `renderDisplayTick()` remains for
   existing tests: it prepares inline, then forwards.
4. **One home for the tick's inputs.** `renderSyntheticTick` moves the inputs
   into the member `renderEngineInputs_` (it is coreMutex state) at the top of
   the tick. Every consumer reads that member: the floor block, `followSpeakerForRoutes`,
   `buildMultiviewRenderPlan`, the multiview tiles, `submitIsolatedMonitorTick`
   and `resolveIsoDisplayName`. The `const` builders need no new parameter, and
   the command lane (R6 through Take, and C3) reads the last tick's values with
   no engine call.
5. **R3: demand published, applied before the lock.** The tick still computes
   `captureDemands` under coreMutex, and it still calls
   `captureDevice->setVideoConsumerDemand` (not an engine call). It then
   publishes the engine half as `std::atomic_store(&engineVideoDemand_, make_shared<const vector<…>>(captureDemands))`.
   `prepareRenderEngineInputsUnlocked` loads that pointer and calls
   `zoomEngineRuntime_->setVideoConsumerDemand(*demand)` off-lock. The demand
   reaches the engine one tick (16.7 ms) later. Re-arming after a leave or
   restart is preserved, because the call still runs every tick. A dedupe was
   rejected: `leave()` clears `isoArrivalQueues_`, and keying a dedupe correctly
   on session boundaries is a trap.
6. **C3: display names from the tick cache.** With an engine configured and
   `renderEngineInputs_.valid`, look the id up in `floorRoster` and apply
   `rawCaptureSnapshotLocked`'s exact fallback (`"Zoom User " + std::to_string(id)`, which is what the file-local `participantIdString` returns for a non-zero id).
   Without a pre-read yet (record started before the first tick), fall back to
   the existing `zoomSnapshot()` path. That path is rare, cold and justified. The
   stub path is unchanged.
7. **Producer hold shrink (supports the frame-group justification).** In
   `applyEvent`, replace the two `state_.snapshot()` copies with two new O(1)
   accessors, `state_.meetingGeneration()` and `state_.rosterRevision()`, read
   before and after `apply`. The roster-fact JSON is still built only when one
   of them moved. Behaviour is identical. Building the fact JSON off-lock is out
   of scope.

## Acceptance

1. **Guardrail counter 0 on the render gather (CI, deterministic).** A new test
   in `native/tests/MediaCoreCommandTest.cpp`:
   `MediaCoreRenderGather.RosterChurnReachesTheRenderGatherWithoutTheEngineMutex`.
   It sets up a configured engine (the same env-var setup as the #1035 test),
   a roster of 2, multiview active, an isolated-monitor probe compositor (the
   `SourcePreviewRequestProbe` pattern from `MediaCoreBatchSnapshotTest.cpp:110-131`),
   a follow-speaker route, and a guest A/V offset for participant 2. Each tick
   runs `renderDisplayTick(-1, 0, core->prepareRenderEngineInputsUnlocked())`.
   The test asserts that `EngineMutexGuardrail::forbiddenAcquisitions()` is
   unchanged across all ticks, and that the roster change took effect through
   the gather: after participant 2 leaves, `sessionState()`'s `zoomGuestAvSync`
   no longer lists `"2"`. It ends with a live-guardrail sanity block, as the
   #1035 test does. This test is RED on `main` once the scope is in place.
2. **Measured before/after worst coreMutex hold during roster churn (QA, not CI).**
   `RenderEngineMutexChurnQa.WorstRenderHoldUnderRosterChurn`, gated by
   `COREVIDEO_QA_LOCK_CHURN=1` (it self-skips otherwise; timing asserts are too
   flaky for CI, per `LockHoldGuardrail.h`). It uses 50 participants and a churn
   thread that applies a `participants` event with an incremented
   `roster_revision` and alternating membership at maximum rate. The render
   loop runs 1,200 ticks paced at 16.7 ms, each under a test-owned coreMutex,
   with multiview active. It reports worst, p99 and mean hold, and the per-call
   engine-mutex wait for every R-row (from `ZoomHandoffCpuScope`'s existing
   `COREVIDEO_QA_ZOOM_HANDOFF_TIMING` instrumentation plus the new
   `renderViewIfConfigured` scope). It is run on `main` (before) and on the
   branch (after), both **Release** builds with the binary size checked. The
   pass criterion: the worst and p99 render-tick hold fall, and the residual
   engine wait inside the tick is attributable only to R1, R2 and R5. Both
   tables go in this spec's Outcome section. A drop smaller than expected is
   reported as measured, never rounded up.
3. Native suite green on a confirmed Release core. `ZoomEngineRuntimeTest`'s
   roster-fact tests are unchanged and green after Task 7.

## Risks

- **Stale roster by one tick: acceptable.** The pre-read happens before the
  lock wait, so the roster can be older than the frames by lockWait (usually 0,
  up to a command hold) plus at most one tick for command-lane readers. Per
  consumer:
  - **Graphics binding:** the epoch confirmation is preserved (before and after
    the view). A join binds one tick later. A departure unbinds one tick later.
    The two reads have never been atomic: frames (R2) and roster (R4) are
    already separate acquisitions within one tick.
  - **Floor ledger and follow-speaker:** the director's sensitivity and hold
    times are hundreds of milliseconds, so 16.7 ms is invisible. L3 order (epoch
    before id) is preserved.
  - **Multiview and monitor tally:** cosmetic, one tick.
  - **ISO display names:** a name at record start that is at most one tick old.
  - **Guest A/V offset pruning: the one real race, judged acceptable.** If the
    render thread's lock wait outlasted a full shell round trip (roster fact →
    pump → shell → `set-zoom-guest-av-sync` command), a pre-read taken before a
    participant joined could prune a trim that was set for them in that window.
    That needs render starvation of tens of milliseconds, which is itself a P0
    that `[render] STALL` reports, and the result is a visible trim reset the
    operator can re-apply. Review Focus lists it. If the owner wants it closed,
    the fix is to stamp offsets with the time they were applied and prune only
    offsets older than the pre-read.
    **Owner ruling D17 (2026-10-23): accepted for this slice.** A guest A/V
    trim set during a tens-of-milliseconds render-thread stall may be dropped.
    The fix (applied-at stamp) rides with the command-lane follow-up issue.
- **The render thread still waits on `mutex_`,** before the lock instead of
  under it. Its own deadline exposure is unchanged in kind. Only the other
  coreMutex clients are freed. Task 8 measures it.
- **R3 lands one tick late.** ISO CPU arrival for a Zoom source starts 16.7 ms
  after record start. This is unverified. Task 5 adds a test that a recording's
  ISO demand reaches the engine, and must check that the first ISO frame's
  timing is no worse than today's, where demand is applied mid-tick and frames
  arrive from the ingest thread later anyway.
- **Speaker director cadence** changes as described above. It is time-driven,
  and Task 4 re-checks the tick's purity before relying on it.
- **Inline preparation hides a missed wiring.** The production render loop must
  call the 3-argument overload. Review checks the single call site. The 2-argument
  overload is commented "tests/direct only".

## Out of scope (follow-ups to file)

- **Command lane, C1 and C2:** spine sync and `zoom-snapshot` off coreMutex.
  This is the likely `cmd.handle` 10,111 us worst.
- **The frame group, R1, R2 and R5:** a fetch-before-lock A/B against frame age.
  Optionally, a lock-free "no ISO arrival queues" skip for R5.
- **Roster-fact JSON** built off `mutex_`.
- **The `zoom-stop-capture` / `zoom-leave` responses'** full snapshots (C4, C5).

## Outcome (2026-10-24)

Branch `codex/rc-1037`, implementation head `c7b36bdd` (plus the measurement
and docs commits). Machine: the dev box, Intel Core i7-14700K, Windows 11 Home
10.0.26200. Both builds are Release from `npm run build:native-dev`
(`corevideo-native.exe` 4,075,008 bytes before, 4,077,056 after; the tests
binary 12.3 MB both times). No `CoreVideoPro.WinUI`, `corevideo-native` or
`corevideo-zoom-engine` process was running for either measurement. One
AFTER attempt was discarded because another worktree's test binary was
running; the AFTER runs below were taken after it exited.

### Lock-hold churn measurement (QA harness, not a CI gate)

`RenderEngineMutexChurnQa.WorstRenderHoldUnderRosterChurn`,
`COREVIDEO_QA_LOCK_CHURN=1`: 50 participants, a churn thread applying roster
events at maximum rate, multiview active (8 tiles), 1,200 ticks paced at
16.7 ms, each under a test-owned mutex. Hold = the time inside the mutex.

BEFORE (`main` 1879ccb7 + the harness, commit 3c39c7f6; the 2-argument
`renderDisplayTick`, which is today's production shape on main):

| run | churn events | worst | p99 | p50 | mean |
|---|---|---|---|---|---|
| 1 | 19,437 | 47,806 us | 17,686 us | 5,148 us | 5,545 us |
| 2 | 19,496 | 17,372 us | 10,783 us | 5,290 us | 5,444 us |
| 3 | 19,567 | 14,411 us | 8,905 us | 5,161 us | 5,211 us |
| 4 (handoff timing on) | 19,743 | 10,746 us | 8,887 us | 5,061 us | 5,131 us |

AFTER (branch; production shape: pre-read outside the mutex, then the
3-argument `renderDisplayTick` inside it):

| run | churn events | worst | p99 | p50 | mean | pre-read worst / p99 / p50 (off the mutex) |
|---|---|---|---|---|---|---|
| 1 | 34,861 | 4,412 us | 2,779 us | 1,588 us | 1,682 us | 1,819 / 1,062 / 283 us |
| 2 | 34,560 | 11,208 us | 3,085 us | 1,591 us | 1,708 us | 6,820 / 1,323 / 302 us |
| 3 | 34,760 | 4,736 us | 3,140 us | 1,655 us | 1,753 us | 3,943 / 1,142 / 299 us |
| 4 (handoff timing on) | 34,242 | 19,191 us | 3,394 us | 1,702 us | 1,775 us | 46,466 / 2,122 / 303 us |

Reading it:

- p99 fell from 8.9-17.7 ms to 2.8-3.4 ms and the mean from 5.1-5.5 ms to
  1.7-1.8 ms, while the churn thread applied about 1.8x as many roster events
  (Task 7 made each event cheaper, so the AFTER load is harsher per tick).
- The worst hold is noisy in both columns. AFTER's worst was 4.4-11.2 ms in
  three runs and 19.2 ms in the fourth. In that fourth run the off-lock
  pre-read also saw a 46 ms worst, and `COREVIDEO_QA_ZOOM_HANDOFF_TIMING`
  logged no frame-group (`poll-compositor`, `latest-decoded`) wait over its
  8 ms threshold, in either column. So no single worst-case hold in either
  column is attributed to an engine-mutex wait by the existing
  instrumentation; scheduler preemption of the test process is not ruled
  out. The worst-case drop is reported as measured, not claimed.
- Residual engine waits inside the tick are, by construction and by the CI
  guardrail, only R1, R2 and R5. Their individual wait distribution was not
  measured below the 8 ms logging threshold (no new production
  instrumentation, per the plan).
- The pre-read's own p99 of 1.1-2.1 ms is now paid by the render thread
  before the lock, not by every coreMutex waiter. It is not yet shown to be
  material to the render deadline; the published-roster alternative is not
  filed.

### Guardrail (CI)

`MediaCoreRenderGather.RosterChurnReachesTheRenderGatherWithoutTheEngineMutex`
was RED with 10 forbidden acquisitions for two ticks on both the stub
compositor (R3, R4, R6, R7, R8 per tick) and the isolated-monitor probe (R3,
R4, R6, R7, R9 per tick). After the pre-read it saw 2 (R3 only), and after
R3 moved, 0. `IsoVideoDemandReachesTheEngineFromBeforeTheLock` and
`IsoDisplayNamesComeFromTheTickRosterNotAnEngineSnapshot` were each RED on
the guardrail before their change.

### Changes beyond the plan

- The speaker-floor ledger now observes the roster under the epoch read
  BEFORE the pre-read (`renderEngineInputs_.speakerEpoch`) instead of a fresh
  `speakerEpoch()` read after it, so roster and epoch come from the same
  read (L3).
- Test seam `ZoomEngineRuntime::isoArrivalDemandIdsForTest()` (the friend
  access struct lives in another test file).
- The C3 test defers the response snapshot (as `cmd.handle` does) and runs
  with the audio worker enabled, so its forbidden scope covers only the
  command.

### Native suite

Full native suite on the Release core at `c7b36bdd` (+ the harness change), `COREVIDEO_CAPTURE_TESTS=1`: **1,895 passed, 0 failed** (the churn harness self-skips without its env var; other tests' own skip lines are counted as passes by the gtest shim).

### Not established

- No live-meeting soak or installed-app run. The 26 ms `render.display-tick`
  worst from the 2026-10-23 show is not re-measured.
- The `cmd.handle` 10,111 us worst (C1 spine sync, C2 `zoom-snapshot`) is not
  addressed; it is the command-lane follow-up.
- The frame group's (R1, R2, R5) residual waits are unmoved.
- R3 lands one tick later. The first ISO frame's timing after record start
  was not measured against main.
- The guest A/V prune race (D17) remains, accepted.

## Follow-up outcome: #1048, #1049 and #1056 evidence (2026-10-25)

Branch `codex/rc-1049` from `main` `ede5d493`. Machine: the dev box (Intel Core
i7-14700K, Windows 11 Home 10.0.26200). Release build from
`npm run build:native-dev` (`corevideo-native.exe` 4,092,928 bytes before the
A/B commit; tests binary about 12.4 MB). Stub compositor
(`createStubModules`), so absolute hold numbers are lower than a GPU core's.
Only the difference between the two orders is the result. Nothing touched the
owner's installed app.

### Attribution first (#1056)

The October 25 soak lost one Program frame in a 15,127 us `render.display-tick`
hold, and nothing in the log said where that time went. Every production
display tick now records a per-phase breakdown (`core/RenderTickPhases.h`).
The phases are: setup, the frame-group calls R1, R2 and R5 (each with its
engine-mutex wait), bus tap, capture poll, gather, stills and floor, ISO
ingress, plan, program, monitors and emit. The breakdown also records the
pre-read and the coreMutex wait, which fall outside the hold. An over-budget
hold logs one line:

```
[lock-hold-attribution] site=render.display-tick held_us=N budget_us=8000 slot=S
  phases_us=[setup=.. program=.. monitors=.. ...] other_us=N
  frame_group_engine_wait_us=N outside_hold_us=[pre-read=N lock-wait=N]
  unattributed_since_last=N
```

The line is capped at one per second per site. The `[lock-guardrail]` warning
is capped at one per minute, and that cap hid most of the soak's overruns,
which came about once a minute. The warning line carries the same text.
`cmd.handle` holds name their command (`command=<type>`). `slot` matches
`[program-buffer-miss] target=`. The next soak can therefore say which phase
consumed a long tick. No stack is needed.

### #1048 command lane (C1, C2, D17)

- **C1 `zoom-media-spine-sync`.** The live loop applies the core half
  (multiview layout and preview scene) under coreMutex. It runs
  `ZoomEngineRuntime::syncSpine` after it releases the lock. That call happens
  on the same command thread, before the next command is dequeued, so command
  order does not change. A render tick may run between the two halves, just as
  one may run between two commands. `handle()` called directly still runs both
  halves inline.
- **C2 `zoom-snapshot`.** With an engine configured, the loop answers it before
  taking coreMutex, through `MediaCore::zoomEngineSnapshotUnlocked()`. Without
  an engine it takes the locked stub path.
- **Gate.** The loop opens `ForbidEngineMutexScope("cmd.handle")` for both
  commands.
  `JsonRpcServer.SpineSyncAndZoomSnapshotRunTheirEngineWorkWithoutCoreMutex`
  counts engine-mutex acquisitions over a real `server.run`. The test was RED
  before the move. It also checks that the core half still applies, and that a
  snapshot sent after the spine sync sees the spine sync's director exclusion
  (ordering).
- **D17.** Each guest A/V trim is stamped when it is applied. The pre-read
  records when its roster read started, and a trim is pruned only if it was
  applied before that time.
  `MediaCoreRenderGather.GuestAvTrimSetAfterThePreReadSurvivesThatTick` was
  RED before the change.

### #1049 frame group: fetch-before-lock A/B

`COREVIDEO_RENDER_FRAME_FETCH=before-lock|under-lock` selects where R1, R2 and
R5 run. In `before-lock` they run last in `prepareRenderEngineInputsUnlocked`
and their frames are restamped inside the tick. A session change between the
fetch and the tick (leave, Engine off or a new engine process, any of which
moves `speakerEpoch`) drops those frames. In `under-lock`, the #1037 order,
they run inside the tick under `AllowEngineMutexScope("render.frame-group
under-lock order #1049")`.

Frame age is measured at the tick's consume point, which is the same point in
both orders: consume time minus ingest `observedAt`, once per new frame per
participant (`MediaCore::observeZoomFrameAgeAtConsume`). The engine's own
`[zoom-latency]` line samples at selection. In the before-lock order that
point precedes the coreMutex wait, so the line under-reports by that wait.

`RenderFrameFetchAbQa.FrameAgeAndHoldForEachFetchOrder`
(`COREVIDEO_QA_FRAME_FETCH_AB=1`) uses the production loop shape: pre-read,
then coreMutex, then the 3-argument tick, at 60 Hz. It runs this load:

- an ingest thread publishing every camera into the frame-sync queue;
- roster events;
- the audio worker's pre-lock engine poll at 50 Hz, plus a 100 us coreMutex
  hold;
- `drainFrameEvents` at 100 Hz;
- a command lane that takes coreMutex every 5 ms. Hold times: 90 % between
  50 and 300 us, 9 % between 0.3 and 1 ms, and 1 % between 1 and 8 ms.

The orders alternate in 6 blocks of 300 ticks each (15 warm-up ticks are
dropped per block). Frame-age percentiles are in ms. "Delta" is before-lock
minus under-lock at the median.

| workload | run | frame age p50 under / before | p99 under / before | delta p50 | hold p99 under / before | hold worst under / before |
|---|---|---|---|---|---|---|
| 10 participants @30 fps, roster 10 Hz | 1 | 66.687 / 66.562 | 68.396 / 68.945 | -0.125 | 2.844 / 2.556 | 5.960 / 5.788 |
| same | 2 | 66.664 / 66.700 | 68.381 / 68.951 | +0.036 | 2.949 / 3.034 | 10.895 / 12.919 |
| same | 3 | 66.621 / 66.642 | 68.741 / 69.436 | +0.022 | 2.361 / 2.345 | 4.034 / 3.734 |
| 10 participants @60 fps, roster max rate | 1 | 66.972 / 66.834 | 71.845 / 70.721 | -0.137 | 6.668 / 1.596 | 13.448 / 2.346 |
| same | 2 | 66.946 / 66.853 | 71.487 / 73.800 | -0.093 | 5.948 / 2.239 | 11.072 / 2.752 |
| 50 participants @60 fps, roster max rate | 1 | 66.925 / 66.877 | 72.088 / 72.105 | -0.048 | 7.723 / 3.535 | 12.340 / 6.498 |
| same | 2 | 66.853 / 66.885 | 72.396 / 71.288 | +0.032 | 7.726 / 3.662 | 41.128 / 5.599 |

How to read the table:

- The roughly 66.7 ms median is the 60 ms frame-sync reserve plus selection
  quantization, and it is the same in both orders. The median delta is within
  ±0.14 ms in every run. That is about 1 % of the 16.7 ms rule, so
  **before-lock is the default**. The p99 frame age moved by -1.1 to +2.3 ms,
  depending on the run, with no consistent sign.
- When the engine mutex is contended, the hold drops: p99 fell from 6-8 ms to
  1.6-3.7 ms, and the worst from 11-41 ms to 2.3-6.5 ms. Under the realistic
  10 Hz roster load the hold is unchanged within noise, because there is
  little engine contention to remove.
- The render thread still pays the engine wait, now in its pre-read with
  coreMutex free. The pre-read p99 rose from about 0.05 ms to 0.1 ms
  (realistic load) and stayed at 4-9 ms either way under stress.
- The #1037 churn harness (`RenderEngineMutexChurnQa`, 50 participants, max
  rate) ran two times per order. under-lock: hold p99 2,856 / 2,983 us, worst
  3,917 / 5,288 us. before-lock: p99 1,751 / 1,583 us, worst 1,931 / 2,416 us.

### Guardrail (CI)

`MediaCoreRenderGather.BeforeLockOrderNeedsNoEngineMutexInsideTheTick` counts
every engine-mutex acquisition on the render thread during a before-lock tick,
including sanctioned ones (`EngineMutexGuardrail::threadAcquisitions`). The
count is 0. The under-lock control takes at least 3.
`BothFetchOrdersDeliverADueZoomFrameToTheTick` and
`PrefetchedFramesFromAnEndedSessionAreDropped` cover delivery and the
session-change drop. `grep AllowEngineMutexScope native/src` now lists only
the under-lock rollback path. Deleting it, once a live soak confirms the
default, is a one-line follow-up.

### Not established

- **No live soak.** The owner's app was in a live meeting. Whether the
  October 25 overruns (8-16 ms) came from the engine mutex at all is unknown.
  The stub-core A/B shows that the frame group can cost up to about 6 ms of
  hold under contention. It does not show that the frame group caused that
  soak's ticks. The new attribution line answers that question.
- The GPU compositor was not used. The phases `program` and `monitors`
  (D3D11 submission, keyed-mutex exports) are the other candidates for the
  15 ms tick, and the stub cannot measure them.

### Acceptance procedure (owner, live)

1. Install the build from this branch and run the October 25 workload for 30
   minutes: 10 participants; Program plus 8 Zoom ISOs on hardware H.264; the
   SRT loopback stream; the virtual camera. Use the default order.
2. `grep -E "lock-hold-attribution|program-buffer-miss|lock-guardrail" media-core.log`.
   Pass when there are zero `program-buffer-miss` lines and the Program file
   holds exactly the scheduled packet count. For every `render.display-tick`
   attribution line, record which phase dominates. If `frame_group_engine_wait_us`
   is zero and `program` or `monitors` dominates, the remaining overruns are GPU
   work, which is outside the scope of this lock change.
3. Optional A/B on the same rig: repeat with `COREVIDEO_RENDER_FRAME_FETCH=under-lock`
   set in the core's environment, and compare the over-budget counts in the
   final `[lock-guardrail]` line.
