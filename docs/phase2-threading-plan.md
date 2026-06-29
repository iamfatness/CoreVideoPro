# Phase 2 — Core Request / Render / Audio Threading Plan

_Status: 2026-06-29. Design pass (not yet implemented). Origin: under Zoom-joined +
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
</content>
</invoke>
