# Phase 2 — Core Request / Render / Audio Threading Plan

_Status: 2026-07-02. **Fully implemented** — increments 1, 2, 4, 5 shipped earlier;
increments 3 (engine send off the lock) and 6 (sub-ms hold guardrails; the TSan CI
gate was already wired) shipped on `agent/phase2-increments-3-6` (see §6
implementation log). The one remaining gate is validation, not code: the ≥10-min
audio-routed + recording soak is being executed separately by the alpha validation
pass (`docs/alpha-plan.md`). Original design pass 2026-06-29. Origin: under Zoom-joined +
Capture-On, command round-trips time out (`[bridge] TIMEOUT type=media-core-sync after
4000ms` + `zoom-media-spine-sync`), blocking GPU multiview layout delivery and degrading
scene/routing/audio syncs — while the render is healthy (56fps, lockWait 0ms, 2.3ms) and
responses are already prioritized. So the >4s lives in the command-serialization path, not
the render. A parallel hands-on agent instruments `JsonRpcServer` to pin the exact span and
ship the first targeted fix; this is the durable architecture that fix slots into._

## 0. Scope correction

The documented "Phase 2" ("decouple audio onto its own thread") is a subset. The real
problem is **command latency under load**, and the durable fix is to make command latency
bounded independent of render. Audio decoupling is one increment inside that.

## 1. Current locking / threading model (as built)

Core process threads in `JsonRpcServer::run` (`native/src/rpc/JsonRpcServer.cpp:192`):

| Thread | Loop | Touches `coreMutex`? |
|---|---|---|
| reader (`:274`) | `getline(stdin)`→`inQ` | no |
| writer (`:245`) | drains `outHi` then `outLo`→stdout | no (responses jump ahead of frames) |
| main/command (`:381`) | drains `inQ`; each `{lock; handle();}` (`:413`); every 33ms `{lock; pumpHeavyFrameEvents();}` (`:440`) | **yes — write path** |
| render (`:308`) | `{lock; renderDisplayTick();}` then pace 60fps | yes, ~1-2ms (light `videoOnly` tick) |
| zoomPump (`:371`) | `drainZoomVideoFrameEvents()` every 8ms | no (engine's own mutex) |

`coreMutex` is the single big lock over all of `MediaCore`. Only **command** and **render**
contend on it.

**What holds the lock, and the key problem — blocking I/O is held under `coreMutex`:**
- Render `renderDisplayTick()`→`renderSyntheticTick(videoOnly=true)` (`MediaCore.cpp:3052`): ~1-2ms, skips audio/encoder/output/readback. This is why render stays fast.
- Command `media-core-sync`/poll → `applyCommands()` (`:766`) → N×`applyCommand()` **then the FULL `renderSyntheticTick(videoOnly=false)`** (`:3132-3418`), all under the lock: audio mix (`:3169`), routed-bus matrix + insert chains (`:3205/3222`), **`monitorOutput->render()`** real audio device I/O (`:3249`), BS.1770 loudness (`:3270`), `encoder->submit()` (`:3358`), **`outputSender->sync()` blocking RTMP/SRT network I/O** (`:3381`), recording mux (`:3412`).
- Command `zoom-media-spine-sync` → `syncSpine()` (`:523` / `ZoomEngineRuntime.cpp:238`) → **`process_->sendLine()` blocking named-pipe write** per subscribe, inside `coreMutex`.

Bridge request sources over one stdin pipe (`MediaCoreSupervisor.cs`/`MediaCoreBridgeService.cs`):

| Source | Cadence | Type |
|---|---|---|
| FrameDrain ping | **every 16ms (~62/s)** | `ping` |
| Poll timer | 250ms | `media-core-sync` (empty) |
| Spine timer | 500ms | `zoom-media-spine-sync` |
| Production syncs | bursty | `media-core-sync` (real) |

Serialization: `_stdinGate` (write+flush), `_syncInFlight` (one media-core-sync), `_spineSyncInFlight`, 4000ms `RequestTimeoutMs`.

**Where the latency comes from (ranked):**
1. **The poll IS a full heavy tick.** Empty `media-core-sync` every 250ms still runs the full audio + `outputSender->sync()` + recording under the lock. A "read" pays for blocking output I/O. Dominant load-coupled hold.
2. **Single command thread = convoy.** ping (62/s) + poll + spine + production all serialize; one multi-hundred-ms op stalls everything queued in `inQ`. Timeout when convoy_depth × per-req_latency > 4s.
3. **Blocking I/O under the lock** (`outputSender->sync`, `monitorOutput->render`, `sendLine`).
4. **62 ping/s flood** — pure contention, zero product value once a real poll exists.
5. **Reads serialized behind writes** (`snapshot`, health/session queries take `coreMutex`).

## 2. Target architecture

Principle: **`coreMutex` protects only fast in-memory state mutation + snapshot (<1ms holds). No blocking I/O or heavy DSP under the lock.** Expensive work runs on dedicated worker threads via double-buffered / bounded-queue handoff. Adopt all of:

- **(c) Coalesce bridge traffic** — drop/slow the 16ms ping; debounce production syncs (trailing ~16-33ms) into one batch; one "latest pending" slot. *C# only, zero core risk, ship first.*
- **(b) Read/poll de-serialization** — publish an immutable `shared_ptr<const SessionState>` snapshot swapped atomically per tick; route `snapshot`/health/session/**empty-poll** to it lock-free and **without running a tick**. Removes the 250ms heavy poll + de-serializes reads.
- **(d) Engine send off the lock** — `ZoomEngineRuntime` gets an outbound command queue + dedicated sender thread owning `sendLine`; `syncSpine` enqueues (keeping `sentSubscriptions_` dedup) and returns the in-memory snapshot.
- **(a) Move audio mix / encoder-submit / output-sender / monitor / readback off the command thread** — split the full tick: copy per-tick inputs under the lock into work items, run DSP + device render + network sync + encoder on worker thread(s) with no lock, publish results back atomically. Output/encoder first (already exception-isolated), audio second (needs glitch-free double buffer).

**Keep (non-negotiable):** GPU 60fps light render tick; spine subscription dedup; multiview content-signature dedup; prioritized responses (`outHi`/`outLo`); skip-present + skip-readback; `timeBeginPeriod(1)` pacing; lock-free zoom-frame pump. **Do NOT** reintroduce frame-rate x:Bound collection rebuilds or per-tile swap chains (CoreMessagingXP class).

## 3. Staged increments (smallest-safe-first; each ships + validated alone)

Repro each with the fake engine + Capture-On (see CLAUDE.md). **Success bar: `[bridge] TIMEOUT` ≈ 0, bounded sync latency, no new `held core lock` warnings, zero CoreMessagingXP fail-fast.**

0. **Instrumentation seam** — structured per-request timing (inQ-wait / lock-acquire / handle / write) keyed by id+type in `JsonRpcServer` + `applyCommands`. The socket the parallel fix writes into; regression telemetry thereafter.
1. **Coalesce bridge traffic (C# only)** — drop the 16ms ping to ~1s (or remove); debounce production syncs; single latest-pending. Request rate ~70/s → <10/s; TIMEOUTs drop sharply with zero core risk.
2. **Read/poll de-serialization** — published snapshot; route reads + empty poll lock-free, no tick. Poll round-trip <5ms even during a write.
3. **Engine send off the lock** — outbound queue + sender thread in `ZoomEngineRuntime`; `syncSpine` enqueues. A slow engine no longer extends spine latency.
4. **Output sender + encoder submit off the command thread** — `OutputWorkItem` → dedicated output thread (drop-to-latest); health/session published back. Lowest-risk part of (a).
5. **Audio mix/route/monitor/loudness off the command thread** (the original "Phase 2") — audio worker at fixed cadence, double-buffered `AudioWorkItem`, atomic result publish. Fixes "audio rides the render lock" + unblocks locked 60fps.
6. **Cleanup & guardrails** — assert all `coreMutex` holds sub-ms; document the threading contract.

## 4. Avoid the CoreMessagingXP 0xc000027b regression class

- Do NOT increase snapshot/frame-event rate to the UI — keep snapshots timer-paced; faster snapshots → more `RefreshSurfaceBindings` → the rebuild storm that's throttled to ~12.5/s.
- Keep shared-texture events structural-change-only; don't let decoupled workers emit textures per audio/output tick.
- Verify any debounce continuation that sets x:Bound props resumes on the UI thread (the Engine-On `ConfigureAwait(true)` pattern).
- Regression gate: every increment's repro runs Engine-On + Capture-On + roster churn ≥60s with zero fail-fast, plus TIMEOUT≈0.

## 5. Three riskiest parts

1. **Audio glitching if mis-threaded (Increment 5)** — real-time PCM producer→consumer; a blocking queue underruns the monitor device. Mitigation: fixed-cadence worker + double buffer drop-newest-on-overrun; atomic meter/bus publish. (Why audio is staged after output.)
2. **Data races on shared `MediaCore` state** — `audioChannels_`, `audioRoutingSends_`, `routedBusPcm_`, `lastProgramFrame_`, `multiviewSources_`, `sentSubscriptions_`. Rule: workers only read copies / `shared_ptr<const>` published under the lock; never touch members directly. Add TSan in CI.
3. **Ordering between commands and render/output** — after decoupling, a response returns once state mutates + snapshot publishes, while output/audio land a tick later. Keep control-plane transitions (start/stop/fail/recover encoder/recording/output) synchronous on the command thread; only per-frame submit/sync/mix go to workers, sequenced by monotonic tick id (drop, never reorder).

## Critical files
- `native/src/rpc/JsonRpcServer.cpp`
- `native/src/core/MediaCore.cpp`
- `native/src/modules/ZoomEngineRuntime.cpp`
- `native-shell/CoreVideoPro.MediaCore/Services/MediaCoreSupervisor.cs`
- `native-shell/CoreVideoPro.MediaCore/Services/MediaCoreBridgeService.cs`

## 6. Implementation log (2026-06-29, branch `worktree-agent-af20fe26631f34348`)

### Increment 1 — SHIPPED (C# bridge traffic)
`MediaCoreSupervisorOptions.FrameDrainIntervalMs` 16ms → **1000ms**
(`MediaCoreSupervisor.cs`). The "frame drain" timer only sends a `ping`; all
video/preview/texture frames pump autonomously on the core's own threads (render
thread + zoom pump), so the timer never moves a frame — it was ~62 `ping`/s, and
every `ping` still took `coreMutex` in the command loop (serialized behind real
syncs, contending the render thread). 1s keeps a liveness heartbeat and removes the
contention. Production syncs were already coalesced (`_syncInFlight` /
`_spineSyncInFlight` single-in-flight gates) — left as-is. Validated: C# MediaCore
builds clean (0/0); MediaCore.Tests 203/203; native tests 233 pass / 6 known
pre-existing fails (ZoomEngineClient I420 thumbnail set).

### Increments 2 + 4 + 5 — SHIPPED (2026-06-29, commit `243f605`)

The dedicated-lock design below was implemented as specified, superseding the
deferral note that follows (kept for the rationale/design record):

- `MediaCore::renderAudioOutputTick(coreMutex)` (`MediaCore.cpp:4095`) runs on a
  dedicated ~50Hz `audioOutputThread` in `JsonRpcServer::run` (`JsonRpcServer.cpp:447`)
  with the exact gather (brief `coreMutex`) → work (`audioOutputMutex_` only) →
  publish (brief `coreMutex`) split. The work span carries `mixRoutedBuses`, insert
  chains, `monitorOutput->render`, BS.1770 loudness, `encoder->submit`,
  `outputSender->sync`, and recording `submitAudio` — so **increment 4** (output/
  encoder off the command thread) landed inside the same worker.
- The render thread is video-only (`renderDisplayTick` → `renderSyntheticTick(
  videoOnly=true)`); `enableAudioOutputWorker()` flips the core off the legacy
  in-tick audio path.
- **Increment 2** landed in `MediaCore::applyCommands` (`MediaCore.cpp:906-914`):
  when the worker is active, an EMPTY `media-core-sync` poll renders zero synthetic
  ticks and returns the published snapshot; command-carrying syncs still render
  (capped catch-up) so effects are reflected immediately.

**Still open:** validation only. The ≥10-min audio-routed + recording soak with ears
on the monitor output has NOT been executed — it is being run separately by the alpha
validation pass (`docs/alpha-plan.md`), not by the increment 3+6 implementation
session (which was code/build/tests only, no devices). The increment 6 TSan gate IS
wired: CI runs the native stub tests under ThreadSanitizer (`native-stub-tsan` job),
clean as of 2026-07-02 — it exercises the audio worker's gather/work/publish handoff
AND (as of the increments 3+6 landing below) the engine sender-thread handoff:
enqueue-from-caller-thread while the sender drains, wedged-pipe non-blocking, restart
purge, shutdown-join races (`ZoomEngineRuntimeTest`), plus the guardrail registry
under the full multi-threaded server (`JsonRpcServerTest`).

### Increments 3 + 6 — SHIPPED (2026-07-02, branch `agent/phase2-increments-3-6`)

**Increment 3 — engine send off the lock (`ZoomEngineRuntime`).** No engine pipe
I/O ever happens under `coreMutex` (or the runtime's own `mutex_`) anymore:

- All `process_->sendLine(...)` call sites (init/join/leave/start_media/subscribe/
  subscribe_audio/unsubscribe) now go through `enqueueEngineSendLocked` → a bounded
  FIFO `sendQueue_` drained by ONE dedicated sender thread (`senderLoop`), the only
  place engine stdin is written. Single-thread FIFO drain preserves ordering to the
  engine exactly as the old synchronous path did.
- `sentSubscriptions_` dedup is keyed at ENQUEUE time (under `mutex_`, which
  serializes `syncSpine` calls): "marked sent" now means "queued exactly once, will
  reach the engine in order" — semantics unchanged for the engine.
- Each queued line carries the `processGeneration_` it was built for. On engine
  restart (`ensureStartedLocked` creating a new process) the generation bumps, lines
  queued for the dead process are purged (dropped + logged, never replayed into the
  new pipe), and `sentSubscriptions_`/`mediaStarted_` reset so the new engine is
  re-initialized/re-subscribed from scratch. Same on shutdown: queued lines are
  dropped + logged. A 1024-line cap (drop-oldest + log) is a safety valve for a
  wedged pipe; dedup keeps the steady-state queue tiny.
- Send failures are applied by the sender (under `mutex_`, generation-checked) as
  Error events carrying the original stage ("init"/"join"/...), which the join/auth
  wait loops already observe — so `join()` still returns an error snapshot, just via
  the event path instead of a synchronous return.
- Lifecycle: one sender for the runtime's lifetime (survives engine restarts).
  Destructor order: signal sender stop + drop queue → `stopReader()` (terminates the
  process, breaking the pipe and unblocking any in-flight write) → join sender.
  `process_` became `shared_ptr` so the sender's in-flight reference outlives a
  concurrent replace; the sender's unlocked write racing teardown's `stop()` is the
  same benign class as the reader thread's long-standing unlocked `readEvent()`.
- `ZoomEngineProcessClient` I/O entry points are now virtual, and
  `ZoomEngineRuntime::installEngineProcessForTest` exists, so the sender path is
  unit-testable with a fake client (blocking/dead-process control). New tests:
  ordering+dedup, wedged-pipe non-blocking (syncSpine/snapshot return while a send
  is blocked), restart purge + resubscribe, destructor join-without-hang, and a
  concurrent churn/snapshot/drain test that TSan exercises in CI.

**LOCK ORDER (final, after increment 3)** — documented also at the `coreMutex`
declaration in `JsonRpcServer.cpp`:

```
coreMutex  →  audioOutputMutex_                                  (audio/output plane)
coreMutex  →  ZoomEngineRuntime::mutex_  →  ZoomEngineRuntime::sendMutex_
```

- `audioOutputMutex_` and the ZoomEngineRuntime locks are never held together.
- The audio worker holds `coreMutex` only for gather/publish and NEVER nested with
  `audioOutputMutex_`; the engine sender thread holds `mutex_`/`sendMutex_` only
  briefly and strictly sequentially (never nested in the reverse order), and holds
  NO lock across the blocking pipe write; the reader thread takes only `mutex_`.
- The increment 6 guardrail registry mutex is a leaf: taken under `coreMutex`,
  takes nothing.

**Increment 6 — sub-ms `coreMutex`-hold guardrails (`core/LockHoldGuardrail`).**
Every instrumented `coreMutex` hold is timed (RAII `ScopedLockHoldTimer`) against a
per-site budget. Semantics: telemetry counters per site (holds / over-budget /
worst) + a rate-capped `[lock-guardrail]` stderr warning on over-budget holds
(first occurrence, then ≤1/s per site with the suppressed count) in ALL builds;
never a hard failure by default — timing asserts are too flaky, especially under
TSan's 5-20x slowdown. Opt-in strict mode (`COREVIDEO_LOCK_GUARDRAIL_STRICT=1`)
aborts on any over-budget hold as a debugging tool. Budgets: sub-ms default
(1ms) for the audio worker's `audio.gather`/`audio.publish` spans; sanctioned
long-hold sites declare their own — `cmd.handle` 50ms (command-carrying syncs
render a capped catch-up under the lock, per increment 2), `render.display-tick`
8ms (typ 1-2ms), `cmd.frame-pump` 8ms. Unit-tested (`LockHoldGuardrailTest`) and
wired-into-the-live-server tested (`JsonRpcServerTest`).

Validated (this landing): native stub suite 285/285 (incl. 11 new sender/guardrail
tests), MediaCore C# 221/221, WinUI 427/427; the `native-stub-tsan` CI job runs the
new concurrency tests under ThreadSanitizer. The ≥10-min audio soak gate runs in
the alpha validation pass, as noted above.

### Increments 2 + 5 — original deferral note (2026-06-29, superseded same day)
**Why deferred:** the headline goal (locked 60fps under load) provably requires
Increment 5 — any design that keeps the audio mix / routed-bus matrix / monitor
render / BS.1770 / `encoder->submit` / `outputSender->sync` under `coreMutex` at
*any* cadence starves the render thread (e.g. 10Hz × ~60ms hold ≈ 600ms/s
contention), and Increment 2 (de-heavy the empty poll) starves audio unless the
audio cadence has already moved off the poll (i.e. unless 5 lands). So 2 cannot ship
without 5, and 5 is the riskiest change of the session. Its two hard gates —
**no audio glitches** and **no data races** — are validated by the plan's own
≥10-min audio-routed + recording soak. That gate could not be executed in the
implementing (headless, automated) session: audio glitch-freedom needs ears (no
programmatic glitch signal exists yet) and race-freedom needs TSan (not wired into
CI yet — see §5.2). Per the risk section, shipped the safe subset (Inc 1) rather
than land an unvalidatable real-time-audio lock restructuring.

**Design to execute (dedicated-lock variant — preferred over lock-free for
race-auditability):**
1. Add `mutable std::mutex audioOutputMutex_` to `MediaCore`. Lock hierarchy:
   `coreMutex` is OUTER, `audioOutputMutex_` is INNER. The render thread takes
   ONLY `coreMutex` (so it is never blocked by audio/output work). The audio worker
   takes `coreMutex` *briefly* to gather/publish and `audioOutputMutex_` for the
   long DSP/IO span — never both at once (sequential, no nesting on the worker).
2. Split `renderSyntheticTick(videoOnly=false)` (`MediaCore.cpp:~3132-3418`):
   - keep the video half (ingest/poll video, build plan, `compositor->render` →
     `lastProgramFrame_`, multiview composite, texture-event enqueue) on the render
     thread under `coreMutex` exactly as the `videoOnly=true` path does today;
   - move the audio/output half into a new `MediaCore::renderAudioOutputTick()`.
3. `renderAudioOutputTick()` runs on a new dedicated worker thread in
   `JsonRpcServer::run` at a fixed cadence (~20–50ms, finer & steadier than today's
   bursty 250ms poll — strictly better for the monitor device, NOT a new glitch
   source: it reuses the existing source-drain model, no new PCM ring buffer):
   - **Gather (brief `coreMutex`):** `pollAudioFrames()` from zoom/engine/capture/
     media; copy plain-data inputs (`audioChannels_`, `audioRoutingSends_`, monitor
     flags+volume, recording/output flags, `lastProgramFrame_` copy for encoder/
     output, encoder session destinations, `outputDestinationSettings_`).
   - **Work (`audioOutputMutex_` only, NO `coreMutex`):** `mixer->mix`,
     `mixRoutedBuses`+insert chains (into a LOCAL bus map), `monitorOutput->render`,
     `updateProgramLoudnessMeter` (into LOCAL meter accumulators), `encoder->submit`,
     `outputSender->sync` (the blocking network I/O), recording `submitAudio`.
   - **Publish (brief `coreMutex`):** swap LOCAL results into the plain-data members
     `sessionState()` reads (`routedBusPcm_`, `mixedAudioFrameCount_`,
     `audioMonitorStatus_/Warning_/FramesPlayed_`, `programLufs*`, recording
     counters). Drop-newest-on-overrun + monotonic tick id (never reorder).
4. Guard EVERY audio/output control-plane command with `audioOutputMutex_`
   (`startProgramOutput`, prepare/start/stop`EncoderSession`, fail/recover
   `OutputSender`, start/stop/fail/recover`RecordingSession`, `syncAudioMonitor`
   which calls `monitorOutput->start/stop`, `setRecordingTargets`,
   `configureEncoderRecordingRequest`) and guard the module reads inside
   `sessionState()` helpers (`encoderSessionState`, `outputSenderSessionState`,
   `audioMixSessionState`, `masterMeterState`, `recordingState`) — these run under
   `coreMutex`, so they take `coreMutex`→`audioOutputMutex_` in that fixed order.
   A single missed guard = data race; this is why the change needs TSan + soak.
5. THEN Increment 2: route the empty `media-core-sync` poll (and `snapshot`/
   `get-output-health`/`get-output-session`) to a `coreMutex`-brief read of the
   already-published plain-data snapshot, with NO tick (audio now ticks on the
   worker). Keep snapshots timer-paced — do NOT raise the UI snapshot/frame-event
   rate (CoreMessagingXP 0xc000027b guardrail, §4).
6. Validation gate before merge: build + native/WinUI suites green (minus the 6
   known fails); ≥10-min fake-engine soak (4 participants + Capture-On) showing
   `[render]` ~60fps / lockWait ~0ms, `[cmd] ... held core lock` warnings gone,
   audio meters live + glitch-free, 0 `media-core-sync` TIMEOUTs, 0 CoreMessagingXP
   fail-fast; ideally a TSan run over the gather/work/publish handoff.
</content>
</invoke>
