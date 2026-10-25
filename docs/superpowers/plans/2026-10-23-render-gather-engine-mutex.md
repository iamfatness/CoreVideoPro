# Render Gather Engine-Mutex Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The production render tick (`render.display-tick`, under `coreMutex`) takes `ZoomEngineRuntime::mutex_` only at the three sanctioned frame-selection calls, and the guardrail proves it in CI. The worst `coreMutex` hold under roster churn is measured before and after the change.

**Architecture:** This is the #1035 pattern applied to the render lane. Before the render thread takes `coreMutex`, `MediaCore::prepareRenderEngineInputsUnlocked()` makes one engine-mutex acquisition, `ZoomEngineRuntime::renderViewIfConfigured()`. It returns the floor roster and the director-advanced speaker id, bracketed by the lock-free speaker epoch, and it applies the ISO video demand the previous tick published. The tick stores those inputs in `renderEngineInputs_`, and every roster or speaker consumer reads that member. `renderDisplayTick` opens `ForbidEngineMutexScope("render.display-tick")`. The frame group (R1, R2, R5) sits inside the new `AllowEngineMutexScope`. A small producer-side change stops `applyEvent` from building two full snapshots per roster event.

**Tech Stack:** C++20 core (`native/`), the in-house gtest shim (`native/tests/gtest/gtest.h`), the `EngineMutexGuardrail` from PR #1035.

**Spec:** `docs/superpowers/specs/2026-10-23-render-gather-engine-mutex-design.md`. Its inventory gives the R*/C*/N* row ids used below.

## Global Constraints

- **No engine-mutex acquisition under coreMutex outside an `AllowEngineMutexScope`** on the render tick. The only sanctioned sites are R1 (`pollCompositorVideoFrames`), R2 (`latestDecodedVideoFrames`) and R5 (`appendCpuVideoFrames`), under the reason string `"render.frame-group #1037-followup"`. Do not add others. If a new acquisition appears, move it.
- **Do not move the frame group.** Fetching frames before the lock ages them by the lock wait. That is a latency change and gets its own A/B (the follow-up issue).
- **No behaviour change beyond the spec's named ones:** the director is ticked at 60 Hz; demand reaches the engine one tick later; the snapshot `"tick"` field advances more slowly. Preserve the L3 order (epoch before speaker id) and the graphics epoch confirmation (epoch before and after the roster).
- `zoomEngineRuntime_` is created once and never reset. That is the only reason calling it without coreMutex is safe. Do not add any unlocked access to coreMutex-domain state in `prepareRenderEngineInputsUnlocked`. Hand it data only through the atomic `engineVideoDemand_` pointer.
- Lock order is unchanged: `coreMutex → ZoomEngineRuntime::mutex_ → sendMutex_`. The pre-read takes `mutex_` with **no** coreMutex held, which is always legal.
- Timing assertions never go into CI. The churn harness self-skips unless `COREVIDEO_QA_LOCK_CHURN=1` and prints numbers. The CI gate is the guardrail counter.
- Build Release: `cmake --build native\build-dev --config Release --target corevideo-native-tests corevideo-native`. Debug and Release write the same exe path. Check the size (about 2.3 MB Release, about 8 MB Debug) before believing any timing number.
- The gtest shim's `--gtest_filter` takes ONE positive wildcard: no `:` lists and no `-` negation. Run suites separately.
- Never run the native suite while the owner's installed app is streaming or has the virtual camera on.
- Other agents write specs in this worktree. Stage only the files each task names.

## Review Focus

1. **The single production call site.** `JsonRpcServer.cpp`'s render loop calls `prepareRenderEngineInputsUnlocked()` **before** `std::unique_lock<std::mutex> lock(coreMutex)` and calls the 3-argument `renderDisplayTick`. If it calls the 2-argument overload, the pre-read happens under the lock and the guardrail does not see it, because the inline preparation runs before the scope opens.
2. **`AllowEngineMutexScope` usage.** Exactly three sites, all in the ingest section of `renderSyntheticTick`. `grep -n AllowEngineMutexScope native/src` must list three call sites (the class definition in `EngineMutexGuardrail.h` also matches).
3. **Ordering inside `prepareRenderEngineInputsUnlocked`:** epoch, then view, then epoch. `rosterSpeakerEpoch` is 0 when the two epoch reads differ, and `speakerEpoch` is the first read.
4. **`renderViewIfConfigured` keeps `advanceActiveSpeaker(monotonicMs())`.** The multiview/monitor tally used to depend on the snapshot call ticking the director.
5. **The guest A/V prune race** (spec Risks) is accepted, not fixed. Confirm the owner agrees, or add the applied-at stamp.
6. **R3 re-arm.** After `leave()` clears `isoArrivalQueues_`, the next tick's demand must re-create them. The demand is applied every tick, not deduplicated.
7. **C3 fallback.** The `"Zoom User " + std::to_string(id)` name matches `rawCaptureSnapshotLocked` byte for byte. The cold path (no pre-read yet) still uses `zoomSnapshot()`.

## Recommended executor

| Task | What | Model | Effort | Est. AI-assisted hours |
|---|---|---|---|---|
| 1 | Churn harness and the BEFORE measurement on `main` | sonnet | medium | 1.25 |
| 2 | `AllowEngineMutexScope` and its test | haiku | low | 0.5 |
| 3 | Render-gather guardrail test (RED) and scope placement | opus | high | 1.25 |
| 4 | Pre-read: `renderViewIfConfigured`, `RenderEngineInputs`, render-loop wiring, consumers R4/R6-R9 (GREEN) | opus | high | 2.5 |
| 5 | R3: publish the demand, apply it before the lock | opus | medium | 1.25 |
| 6 | C3: ISO display names from the tick cache | sonnet | medium | 0.75 |
| 7 | Producer hold shrink in `applyEvent` | sonnet | medium | 0.75 |
| 8 | AFTER measurement, outcome tables, native suite | sonnet | medium | 1.0 |
| 9 | Docs and follow-up issues | sonnet | low | 0.75 |
| | **Total** | | | **10.0** |

The opus/high tasks are the ones where a wrong order (epoch/roster, pre-read/lock) is silent and only review catches it.

---

### Task 1: Churn harness and the BEFORE measurement

**Files:**
- Create: `native/tests/RenderEngineMutexChurnQaTest.cpp`
- Modify: `native/CMakeLists.txt`. Register the file in the unconditional test list next to `tests/MediaCoreCommandTest.cpp` (around line 880).

**Interfaces:**
- Consumes: `MediaCore::zoomEngineRuntimeForTest()`, `ZoomEngineRuntime::applyEngineEventForTest`, `parseZoomEngineEvent`, `MediaCore::renderDisplayTick()`, `ScopedLockHoldTimer` and `LockHoldGuardrail::statsForSite`.
- Produces: printed numbers only. No production code.

- [ ] **Step 1: Write the harness**

```cpp
#include <gtest/gtest.h>
#include "core/LockHoldGuardrail.h"
#include "core/MediaCore.h"
#include "modules/ZoomEngineClient.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {
void setEnv(const char* name, const char* value) {
#if defined(_WIN32)
  _putenv_s(name, value ? value : "");
#else
  if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}
std::string rosterJson(int revision, int count, bool dropOdd) {
  std::string json = R"({"cmd":"participants","meeting_generation":1,"roster_revision":)" +
                     std::to_string(revision) + R"(,"participants":[)";
  bool first = true;
  for (int id = 1; id <= count; ++id) {
    if (dropOdd && (id % 2)) continue;
    if (!first) json += ",";
    first = false;
    json += R"({"id":)" + std::to_string(id) + R"(,"name":"Guest )" + std::to_string(id) + R"("})";
  }
  return json + "]}";
}
}  // namespace

// #1037 QA harness (NOT a CI gate: timing). Run with COREVIDEO_QA_LOCK_CHURN=1
// on a RELEASE build, once on main (before) and once on the branch (after).
TEST(RenderEngineMutexChurnQa, WorstRenderHoldUnderRosterChurn) {
  const char* gate = std::getenv("COREVIDEO_QA_LOCK_CHURN");
  if (!gate || std::string(gate) != "1") {
    std::fprintf(stderr, "[  SKIPPED ] RenderEngineMutexChurnQa (set COREVIDEO_QA_LOCK_CHURN=1)\n");
    return;
  }
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto core = std::make_unique<corevideo::core::MediaCore>(corevideo::modules::createStubModules());
  setEnv("COREVIDEO_ZOOM_ENGINE_PATH", nullptr);
  auto* runtime = core->zoomEngineRuntimeForTest();
  ASSERT_TRUE(runtime && runtime->configured());
  constexpr int kParticipants = 50;
  runtime->applyEngineEventForTest(*corevideo::modules::parseZoomEngineEvent(rosterJson(1, kParticipants, false)));
  // Multiview active so R7/R8 run every tick (kMultiviewTickDivisor = 1).
  corevideo::rpc::Json::Array sources;
  for (int i = 0; i < 8; ++i)
    sources.push_back(corevideo::rpc::Json::Object{{"kind", "participant"},
        {"participantId", std::to_string(i + 1)}, {"slot", i}, {"label", "Tile"}});
  (void)core->applyCommand(corevideo::rpc::Json::Object{{"type", "set-multiview-layout"},
      {"canvasWidth", 1920}, {"canvasHeight", 1080}, {"sources", sources}});

  std::atomic<bool> stop{false};
  std::thread churn([&] {
    for (int revision = 2; !stop.load(); ++revision)
      runtime->applyEngineEventForTest(*corevideo::modules::parseZoomEngineEvent(
          rosterJson(revision, kParticipants, (revision % 2) != 0)));
  });

  std::mutex coreMutex;
  std::vector<long long> holdsUs;
  holdsUs.reserve(1200);
  auto next = std::chrono::steady_clock::now();
  for (int tick = 0; tick < 1200; ++tick) {
    next += std::chrono::microseconds(16'667);
    {
      std::lock_guard<std::mutex> lock(coreMutex);
      const auto t0 = std::chrono::steady_clock::now();
      core->renderDisplayTick();  // AFTER Task 4: see Step 4 below
      holdsUs.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count());
    }
    std::this_thread::sleep_until(next);
  }
  stop = true;
  churn.join();
  std::sort(holdsUs.begin(), holdsUs.end());
  long long sum = 0;
  for (auto v : holdsUs) sum += v;
  std::fprintf(stderr, "[render-churn] participants=%d ticks=%zu worst=%lldus p99=%lldus p50=%lldus mean=%lldus\n",
               kParticipants, holdsUs.size(), holdsUs.back(),
               holdsUs[holdsUs.size() * 99 / 100], holdsUs[holdsUs.size() / 2],
               sum / static_cast<long long>(holdsUs.size()));
}
```

If `set-multiview-layout` needs fields this sketch lacks, copy the shape from `MediaCoreCommandTest.cpp:7160-7179`. Report what changed.

- [ ] **Step 2: Per-call attribution**

Also run with `COREVIDEO_QA_ZOOM_HANDOFF_TIMING=1`. That turns on the existing `ZoomHandoffCpuScope` lines for `poll-compositor`, `latest-decoded` and `configured`, which log waits over 8 ms. Record which R-rows show waits. Do not add new instrumentation to production code for this step.

- [ ] **Step 3: Build Release on `main` and record BEFORE**

```powershell
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
cmd /c "call `"$vs`" -arch=amd64 >nul 2>&1 && cmake -S native -B native\build-dev >nul && cmake --build native\build-dev --config Release --target corevideo-native-tests 2>&1" | Select-String " error |tests.vcxproj -> "
(Get-Item native\build-dev\corevideo-native-tests.exe).Length
$env:COREVIDEO_QA_LOCK_CHURN = "1"; native\build-dev\corevideo-native-tests.exe --gtest_filter='RenderEngineMutexChurnQa.*'
```

Run it three times and record all three `[render-churn]` lines. Also record the machine, the build size and the commit. This is the BEFORE table.

- [ ] **Step 4: Note for later**

After Task 4, Step 1's loop must call `core->renderDisplayTick(-1, 0, core->prepareRenderEngineInputsUnlocked())` with the preparation done **outside** the `lock_guard`, to match production. Task 8 makes that edit. On `main` the harness uses the 2-argument call, which is today's behaviour.

- [ ] **Step 5: Commit**

```bash
git add native/tests/RenderEngineMutexChurnQaTest.cpp native/CMakeLists.txt
git commit -m "QA harness: render-tick coreMutex hold under Zoom roster churn (#1037, before)"
```

---

### Task 2: `AllowEngineMutexScope`

**Files:**
- Modify: `native/src/modules/EngineMutexGuardrail.h`
- Modify: `native/tests/MediaCoreCommandTest.cpp`. Add the test after the #1035 test at about line 9590.

**Interfaces:**
- Produces: `corevideo::modules::AllowEngineMutexScope(const char* reason)`, which Task 3 uses.

- [ ] **Step 1: Write the failing test**

```cpp
// #1037: a sanctioned exception inside a forbidden scope is NOT counted, and
// the forbidden scope is restored on exit.
TEST(EngineMutexGuardrail, AllowScopeSanctionsOneSiteAndRestoresTheForbiddenScope) {
  corevideo::modules::EngineMutex mutex;
  const auto before = corevideo::modules::EngineMutexGuardrail::forbiddenAcquisitions().load();
  {
    corevideo::modules::ForbidEngineMutexScope forbidden("test.outer");
    {
      corevideo::modules::AllowEngineMutexScope allowed("test.sanctioned");
      std::lock_guard<corevideo::modules::EngineMutex> lock(mutex);
    }
    EXPECT_EQ(corevideo::modules::EngineMutexGuardrail::forbiddenAcquisitions().load(), before);
    std::lock_guard<corevideo::modules::EngineMutex> lock(mutex);  // forbidden again
  }
  EXPECT_EQ(corevideo::modules::EngineMutexGuardrail::forbiddenAcquisitions().load(), before + 1);
  EXPECT_EQ(corevideo::modules::EngineMutexGuardrail::forbiddenDepth(), 0);
}
```

Build. The test fails to compile because `AllowEngineMutexScope` does not exist yet. That is the RED.

- [ ] **Step 2: Implement**

Add the class below `ForbidEngineMutexScope` in `EngineMutexGuardrail.h`:

```cpp
// #1037: the ONLY way to take the engine mutex inside a forbidden scope. Each
// use is a reviewed, named exception; `grep AllowEngineMutexScope` lists them.
class AllowEngineMutexScope {
 public:
  explicit AllowEngineMutexScope(const char* reason)
      : savedDepth_(EngineMutexGuardrail::forbiddenDepth()), savedSite_(EngineMutexGuardrail::forbiddenSite()) {
    EngineMutexGuardrail::forbiddenDepth() = 0;
    EngineMutexGuardrail::forbiddenSite() = reason;
  }
  ~AllowEngineMutexScope() {
    EngineMutexGuardrail::forbiddenDepth() = savedDepth_;
    EngineMutexGuardrail::forbiddenSite() = savedSite_;
  }
  AllowEngineMutexScope(const AllowEngineMutexScope&) = delete;
  AllowEngineMutexScope& operator=(const AllowEngineMutexScope&) = delete;

 private:
  int savedDepth_;
  const char* savedSite_;
};
```

- [ ] **Step 3: Build Release and run**

`native\build-dev\corevideo-native-tests.exe --gtest_filter='EngineMutexGuardrail.*'` should be green. Re-run `--gtest_filter='MediaCoreIsoDeparture.*'`, which should still be green.

- [ ] **Step 4: Commit**

```bash
git add native/src/modules/EngineMutexGuardrail.h native/tests/MediaCoreCommandTest.cpp
git commit -m "Guardrail: AllowEngineMutexScope, the one named exception inside a forbidden scope (#1037)"
```

---

### Task 3: Render-gather guardrail test (RED) and scope placement

**Files:**
- Modify: `native/tests/MediaCoreCommandTest.cpp`
- Modify: `native/src/core/MediaCore.cpp` (`renderDisplayTick` at about line 5014, and the ingest section at lines 5094-5099 and 5292)

**Interfaces:**
- Consumes: `AllowEngineMutexScope` (Task 2).
- Produces: a test that Task 4 turns GREEN, and that Tasks 5 and 6 extend.

- [ ] **Step 1: Write the test**

```cpp
// #1037: the production render tick reads the roster and the directed speaker
// through a pre-read made BEFORE coreMutex; nothing inside it takes the Zoom
// engine mutex except the three sanctioned frame-group calls.
namespace {
class IsolatedMonitorProbe final : public corevideo::modules::ICompositor {
  // Copy SourcePreviewRequestProbe from MediaCoreBatchSnapshotTest.cpp:110-131
  // (wraps the stub compositor; hasIsolatedMonitors() == true so the
  // MediaCoreMonitors.cpp:44 path (R9) runs; counts submitMonitors calls).
};
}  // namespace

TEST(MediaCoreRenderGather, RosterChurnReachesTheRenderGatherWithoutTheEngineMutex) {
  setTestEnv("COREVIDEO_ZOOM_ENGINE_PATH", "C:/fake/corevideo-zoom-engine.exe");
  auto modules = corevideo::modules::createStubModules();
  auto probe = std::make_unique<IsolatedMonitorProbe>(std::move(modules.compositor));
  auto* monitors = probe.get();
  modules.compositor = std::move(probe);
  auto core = std::make_unique<corevideo::core::MediaCore>(std::move(modules));
  setTestEnv("COREVIDEO_ZOOM_ENGINE_PATH", nullptr);
  auto* runtime = core->zoomEngineRuntimeForTest();
  ASSERT_TRUE(runtime && runtime->configured());
  const auto roster = [&](const char* json) {
    const auto event = corevideo::modules::parseZoomEngineEvent(json);
    ASSERT_TRUE(event.has_value());
    runtime->applyEngineEventForTest(*event);
  };
  roster(R"({"cmd":"participants","meeting_generation":1,"roster_revision":1,"participants":[{"id":1,"name":"Stays"},{"id":2,"name":"Leaves"}]})");

  // Multiview (R7/R8), a follow-speaker route (R6) with frames in the gather,
  // and a guest A/V trim for participant 2 (the observable roster consumer).
  // Use the set-multiview-layout shape from :7160-7179, followSpeakerScene()
  // from TilesRenderPlanTest.cpp:458, and useZoomSourcesForTest so videoFrames
  // is non-empty (followSpeakerForRoutes is skipped with no frames).
  // ... setup commands ...
  (void)core->setZoomGuestAvSyncOffset("2", 120);

  const auto tick = [&] {
    std::mutex coreMutex;
    auto inputs = core->prepareRenderEngineInputsUnlocked();   // BEFORE the lock
    std::lock_guard<std::mutex> lock(coreMutex);
    core->renderDisplayTick(-1, 0, std::move(inputs));
  };
  const auto trimmed = [&](const std::string& id) {
    const auto state = core->sessionState();
    const auto* sync = state.get("zoomGuestAvSync");
    if (!sync || !sync->isArray()) return false;
    return std::any_of(sync->asArray().begin(), sync->asArray().end(),
        [&](const auto& row) { return row.getString("participantId") == id; });
  };

  const auto guardBefore = corevideo::modules::EngineMutexGuardrail::forbiddenAcquisitions().load();
  tick();
  EXPECT_TRUE(trimmed("2")) << "participant 2 is present: the trim must survive the gather";
  EXPECT_GT(monitors->monitorCalls, 0) << "the isolated-monitor path (R9) must have run";

  roster(R"({"cmd":"participants","meeting_generation":1,"roster_revision":2,"participants":[{"id":1,"name":"Stays"}]})");
  tick();
  EXPECT_FALSE(trimmed("2")) << "a roster drop must reach the render gather (guest A/V prune)";

  EXPECT_EQ(corevideo::modules::EngineMutexGuardrail::forbiddenAcquisitions().load(), guardBefore)
      << "the render tick (under coreMutex) took the Zoom engine mutex outside the frame group";
  core.reset();
}
```

Check the `zoomGuestAvSync` row field name against `CoreStateCapture.cpp:681-699` before relying on it. If `sessionState()` needs the snapshot deferrer, call `core->sessionState()` exactly the way other tests in this file do. Report the seam you used.

- [ ] **Step 2: Place the scopes (do not move any call yet)**

In `MediaCore.h`, add the 3-argument overload `void renderDisplayTick(int64_t productionSlot, int64_t productionAnchorNs, RenderEngineInputs inputs);`. For this task, declare an empty `struct RenderEngineInputs {};` and stub `prepareRenderEngineInputsUnlocked()` to return `{}`, so the test compiles. Task 4 fills both in.

In `MediaCore.cpp`:
- In the 3-argument `renderDisplayTick`, wrap the `renderSyntheticTick(/*videoOnly=*/true, …)` call in `modules::ForbidEngineMutexScope noEngineMutex("render.display-tick");`.
- Make the 2-argument overload forward: `renderDisplayTick(slot, anchor, prepareRenderEngineInputsUnlocked());`, with a comment that it is for tests and direct callers only.
- Wrap each of the three frame-group calls in `{ modules::AllowEngineMutexScope frameGroup("render.frame-group #1037-followup"); … }`: `pollCompositorVideoFrames` (5095), `latestDecodedVideoFrames` (5098) and `appendCpuVideoFrames` (5292). Keep each scope tight around its call, so that the work on the returned frames is still covered.

- [ ] **Step 3: Build Release and run. Confirm the RED and count it.**

`--gtest_filter='MediaCoreRenderGather.*'` should fail on the guardrail assertion only. Print the delta (`forbiddenAcquisitions() - guardBefore`) in the failure message and record it. The expected count is 2 ticks × (R3 + R4 + R6 + R7 ×2 for the multiview plus monitor plans + R8 + R9), roughly 14. Explain any difference: for example, R6 is not reached if the follow route resolves no frames. **If the count is 0, the scope is not on the path the test drives. STOP and fix the test before Task 4.**

- [ ] **Step 4: Do not commit the RED alone.** Task 4 commits the test together with the fix that greens it.

---

### Task 4: Pre-read the roster and speaker (GREEN)

**Files:**
- Modify: `native/src/modules/ZoomEngineRuntime.h`, `native/src/modules/ZoomEngineRuntime.cpp`
- Modify: `native/src/core/MediaCore.h`, `native/src/core/MediaCore.cpp`, `native/src/core/MediaCoreMonitors.cpp`
- Modify: `native/src/rpc/JsonRpcServer.cpp` (render loop at about lines 686-698)
- Modify: `native/tests/ZoomEngineRuntimeTest.cpp` (one accessor test)

**Interfaces:**
- Produces: `ZoomRenderView`, `ZoomEngineRuntime::renderViewIfConfigured()`, `MediaCore::RenderEngineInputs`, `MediaCore::prepareRenderEngineInputsUnlocked()`, and the member `renderEngineInputs_`. Tasks 5 and 6 use them.

- [ ] **Step 1: Re-verify the director tick is pure-by-time**

Read `ZoomActiveSpeakerDirector::tick`, `enforceIncumbent`, `fillVacancy` and `promote` (`native/src/modules/ZoomActiveSpeakerDirector.h`). Confirm they hold no per-call counters and that repeated calls at the same `nowMs` are idempotent. If any of that is false, STOP. Report it, and use `directedSpeakerId()` (no tick) for R6 plus an advancing read only for R7-R9.

- [ ] **Step 2: Engine accessor test (RED)**

In `ZoomEngineRuntimeTest.cpp`, use a configured runtime, apply a roster of 2 with participant 1 talking, and assert that `renderViewIfConfigured()` returns both participants (`directorExcluded` agrees with `floorParticipants()`) and a non-empty `directedSpeakerId` equal to `snapshot().getString("activeSpeakerId")`. An unconfigured runtime returns `std::nullopt`.

- [ ] **Step 3: Implement the accessor**

```cpp
// ZoomEngineRuntime.h
struct ZoomRenderView {
  std::vector<ZoomEngineParticipant> floorParticipants;
  std::string directedSpeakerId;  // after the director is advanced, like a snapshot
};
// #1037: ONE mutex_ acquisition for everything the render tick needs from the
// roster. Call it OUTSIDE coreMutex (MediaCore::prepareRenderEngineInputsUnlocked).
[[nodiscard]] std::optional<ZoomRenderView> renderViewIfConfigured();

// ZoomEngineRuntime.cpp
std::optional<ZoomRenderView> ZoomEngineRuntime::renderViewIfConfigured() {
  if (!configured()) return std::nullopt;
  std::lock_guard<EngineMutex> lock(mutex_);
  state_.advanceActiveSpeaker(monotonicMs());  // what rawCaptureSnapshotLocked did for R7-R9
  return ZoomRenderView{state_.floorParticipants(), state_.directedSpeakerIdString()};
}
```

- [ ] **Step 4: MediaCore inputs and preparation**

```cpp
// MediaCore.h (public, next to renderDisplayTick)
struct RenderEngineInputs {
  bool engineConfigured = false;
  bool valid = false;
  std::uint64_t speakerEpoch = 0;
  std::uint64_t rosterSpeakerEpoch = 0;
  std::vector<modules::ZoomEngineParticipant> floorRoster;
  std::string directedSpeakerId;
};
// #1037: read BEFORE coreMutex, for the same reason as pollZoomAudioUnlocked.
// Touches only zoomEngineRuntime_ (created once, never reset).
[[nodiscard]] RenderEngineInputs prepareRenderEngineInputsUnlocked();
// private:
RenderEngineInputs renderEngineInputs_;  // coreMutex state; the last tick's inputs

// MediaCore.cpp
MediaCore::RenderEngineInputs MediaCore::prepareRenderEngineInputsUnlocked() {
  RenderEngineInputs inputs;
  inputs.valid = true;
  if (!zoomEngineRuntime_ || !zoomEngineRuntime_->configured()) return inputs;
  inputs.engineConfigured = true;
  const auto before = zoomEngineRuntime_->speakerEpoch();   // L3: epoch FIRST
  auto view = zoomEngineRuntime_->renderViewIfConfigured();
  const auto after = zoomEngineRuntime_->speakerEpoch();
  inputs.speakerEpoch = before;
  inputs.rosterSpeakerEpoch = before == after ? before : 0;  // graphics confirmation
  if (view) {
    inputs.floorRoster = std::move(view->floorParticipants);
    inputs.directedSpeakerId = std::move(view->directedSpeakerId);
  }
  return inputs;
}
```

Change `renderSyntheticTick` to take the inputs, `void renderSyntheticTick(bool videoOnly, int64_t mpt, RenderEngineInputs inputs)`, and assign `renderEngineInputs_ = std::move(inputs);` at the top of the tick. The command-lane callers (N10: `:1097, :2472, :2619, :2919`) pass `prepareRenderEngineInputsUnlocked()` inline. Keep a default-argument overload only if it reduces churn, and document it as the test/direct path.

- [ ] **Step 5: Rewire the consumers**

- **R4 (floor block, `:5231-5259`):** replace the three engine calls with `const auto& roster = renderEngineInputs_.floorRoster;` and `graphicsInstances_.observeSources(roster, renderEngineInputs_.rosterSpeakerEpoch, videoFrames, captureLabel);`. Keep the `engineLive` gate as is.
- **R6 (`followSpeakerForRoutes`, `:648-653`):** in the engine branch, use `epoch = renderEngineInputs_.speakerEpoch; current = renderEngineInputs_.directedSpeakerId;`. Keep the stub branch unchanged.
- **R7 (`buildMultiviewRenderPlan`, `:3764`), R8 (`:5865`), R9 (`MediaCoreMonitors.cpp:44`):** add a private helper.

  ```cpp
  // #1037: the multiview/monitor tally speaker. Engine: the tick's pre-read
  // (never a full snapshot under coreMutex). Stub: unchanged.
  std::string MediaCore::tallySpeakerId() const {
    if (renderEngineInputs_.engineConfigured) return renderEngineInputs_.directedSpeakerId;
    return zoomSnapshot().getString("activeSpeakerId");
  }
  ```

  Replace each `zoomSnapshot().getString("activeSpeakerId")` at those three sites with `tallySpeakerId()`. Grep for any other `zoomSnapshot().getString("activeSpeakerId")` on the render path and report it.

- [ ] **Step 6: Wire the production render loop**

In `JsonRpcServer.cpp`, before `std::unique_lock<std::mutex> lock(coreMutex);` (about line 687):

```cpp
      // #1037: everything the render tick needs from the Zoom engine is read
      // HERE, with coreMutex free. Taking ZoomEngineRuntime::mutex_ under
      // coreMutex stalls every command and the audio gather behind it.
      auto engineInputs = mediaCore_.prepareRenderEngineInputsUnlocked();
```

Inside the lock, call `mediaCore_.renderDisplayTick(cadence.slotIndex(), …anchor…, std::move(engineInputs));`. Leave `tickLockWaitNs` as it is. It still measures only the lock wait. Do not fold the pre-read time into it.

- [ ] **Step 7: Build Release and run**

These should be green: `MediaCoreRenderGather.*`, `ZoomEngineRuntime*` (the suite name the accessor test uses), `MediaCoreIsoDeparture.*`, `TilesRenderPlan.*`, `MediaCoreBatchSnapshot*` and `JsonRpcServer*`. The guardrail delta in the Task 3 test must now be exactly 0 **unless** R3 is the only remainder. In that case, record the residual count (expected 2, one per tick) and change the assertion to `EXPECT_EQ(delta, 2) << "R3 only; Task 5 removes it"`. Task 5 restores `EXPECT_EQ(…, guardBefore)`.

- [ ] **Step 8: Commit**

```bash
git add native/src/modules/ZoomEngineRuntime.h native/src/modules/ZoomEngineRuntime.cpp native/src/core/MediaCore.h native/src/core/MediaCore.cpp native/src/core/MediaCoreMonitors.cpp native/src/rpc/JsonRpcServer.cpp native/tests/ZoomEngineRuntimeTest.cpp native/tests/MediaCoreCommandTest.cpp
git commit -m "Render gather reads the Zoom roster and speaker before coreMutex (#1037)"
```

---

### Task 5: R3, the ISO video demand published and applied before the lock

**Files:**
- Modify: `native/src/core/MediaCore.h`, `native/src/core/MediaCore.cpp` (`:5153-5161` and `prepareRenderEngineInputsUnlocked`)
- Modify: `native/tests/MediaCoreCommandTest.cpp`

**Interfaces:**
- Produces: `std::shared_ptr<const std::vector<modules::SourceVideoDemand>> engineVideoDemand_`, accessed only through `std::atomic_load_explicit`/`std::atomic_store_explicit`, the same pattern as `programOutputConfiguration_`.

- [ ] **Step 1: Test (RED for the demand reaching the engine)**

Extend `MediaCoreRenderGather`, or add a sibling test `IsoVideoDemandReachesTheEngineFromBeforeTheLock`. Start a recording with `isoSourceIds = ["zoom:1"]` (the #1035 test's command sequence), run two `tick()`s, and assert that the engine holds an ISO arrival queue for `"1"`. Use `subscriptionChurnState()` → `sources[].isoCpuArrivals.demanded`, or friend `ZoomEngineRuntimeTestAccess` if it is reachable from this file; report the seam. Then stop the recording, run two ticks, and assert the queue is gone. Then `leave()`, start recording again, run two ticks, and assert the queue is **re-created** (Review Focus 6). Assert the guardrail delta is 0 across all ticks.

- [ ] **Step 2: Implement**

In `renderSyntheticTick`, keep computing `captureDemands` and calling `modules_.captureDevice->setVideoConsumerDemand(captureDemands)` (not an engine call). Replace `if (engineLive) zoomEngineRuntime_->setVideoConsumerDemand(captureDemands);` with:

```cpp
  // #1037 R3: the engine half is applied by the NEXT tick's pre-read, with
  // coreMutex free. One tick (16.7 ms) late; re-arms every tick (no dedupe:
  // leave() clears the engine's arrival queues).
  std::atomic_store_explicit(&engineVideoDemand_,
      std::make_shared<const std::vector<modules::SourceVideoDemand>>(captureDemands),
      std::memory_order_release);
```

In `prepareRenderEngineInputsUnlocked`, after the configured check and before the view:

```cpp
  if (const auto demand = std::atomic_load_explicit(&engineVideoDemand_, std::memory_order_acquire))
    zoomEngineRuntime_->setVideoConsumerDemand(*demand);
```

- [ ] **Step 3: Build Release and run** `MediaCoreRenderGather.*` and `MediaCoreIsoDeparture.*`, then `--gtest_filter='IsoVideoIngress*'`. Restore the Task 3 assertion to `EXPECT_EQ(…, guardBefore)`.

- [ ] **Step 4: Commit**

```bash
git add native/src/core/MediaCore.h native/src/core/MediaCore.cpp native/tests/MediaCoreCommandTest.cpp
git commit -m "Render gather: apply Zoom ISO video demand before coreMutex (#1037 R3)"
```

---

### Task 6: C3, ISO display names from the tick cache

**Files:**
- Modify: `native/src/core/MediaCore.cpp` (`resolveIsoDisplayName`, `:2722-`)
- Modify: `native/tests/MediaCoreCommandTest.cpp`

- [ ] **Step 1: Test (RED)**

Use a configured engine, a roster of `{1, "Alice"}` and `{2, ""}`, and one `tick()`. Then call `start-recording-session` with `isoSourceIds ["zoom:1","zoom:2"]` inside a test-opened `ForbidEngineMutexScope forbid("test.cmd")`. Assert that the guardrail delta is 0 for the command, and that the recording request carries the names `"Alice"` and `"Zoom User 2"`. Capture the request with an `IEncoderSink` probe's `configureRecording`, in the style of the #1035 test's `DepartureCapturingEncoder`. The test is RED on the guardrail, because today `resolveIsoDisplayName` calls `zoomSnapshot()`. If the synchronous command-path render (N10) runs inside that scope and counts R1/R2/R5, move the forbid scope tighter: around `configureEncoderRecordingRequest` through a test seam, or start the scope after the command's own render. Report which.

- [ ] **Step 2: Implement**

At the top of the `scheme == "zoom" || scheme.empty()` branch:

```cpp
    if (renderEngineInputs_.engineConfigured && renderEngineInputs_.valid) {
      // #1037 C3: the tick's pre-read roster, never a full snapshot under coreMutex.
      for (const auto& participant : renderEngineInputs_.floorRoster) {
        if (participant.id == 0 || std::to_string(participant.id) != bareId) continue;
        return participant.displayName.empty() ? "Zoom User " + bareId : participant.displayName;
      }
      // not in the roster: fall through to the existing not-found behaviour below
    }
```

Keep the existing `zoomSnapshot()` loop as the cold path, for an engine configured with no tick yet, and for the stub path. Make sure the not-found fall-through does **not** re-enter the snapshot loop when the cache is valid. Restructure the branch so the snapshot loop runs only when `!(engineConfigured && valid)`.

- [ ] **Step 3: Build Release, run `MediaCore*` suites one wildcard at a time, and commit**

```bash
git add native/src/core/MediaCore.cpp native/tests/MediaCoreCommandTest.cpp
git commit -m "ISO display names from the render tick's roster, not a snapshot under coreMutex (#1037 C3)"
```

---

### Task 7: Producer hold shrink in `applyEvent`

**Files:**
- Modify: `native/src/modules/ZoomEngineState.h` (two accessors)
- Modify: `native/src/modules/ZoomEngineRuntime.cpp` (`applyEvent`, `:974-983`)
- Modify: `native/tests/ZoomEngineRuntimeTest.cpp`

- [ ] **Step 1: Test first**

If there is no existing test that does the following, add one: a roster event that bumps `roster_revision` produces exactly one `zoom-roster-fact` in `drainFrameEvents()`; a second event with the **same** revision and generation produces none; a new `meeting_generation` produces one. Run it on the current code. It must pass before the change; this is a characterization test.

- [ ] **Step 2: Implement**

```cpp
// ZoomEngineState.h
[[nodiscard]] std::uint64_t meetingGeneration() const { return meetingGeneration_; }
[[nodiscard]] std::uint64_t rosterRevision() const { return rosterRevision_; }

// ZoomEngineRuntime::applyEvent — replaces the two full snapshot() copies
  const auto generationBefore = state_.meetingGeneration();
  const auto revisionBefore = state_.rosterRevision();
  state_.apply(event);
  if (rosterEvent && (state_.meetingGeneration() != generationBefore ||
                      state_.rosterRevision() != revisionBefore)) {
    pendingRosterFact_ = rpc::Json::Object{
        {"type", "zoom-roster-fact"}, {"roster", rawCaptureSnapshotLocked()}};
  }
```

Confirm that `snapshot().meetingGeneration` and `snapshot().rosterRevision` are those two members copied verbatim (`ZoomEngineState.cpp`, `snapshot()`). If either is derived, STOP and report.

- [ ] **Step 3: Build Release, run `ZoomEngineRuntime*`, `ZoomEngineState*` and `MediaCoreRenderGather.*`, and commit**

```bash
git add native/src/modules/ZoomEngineState.h native/src/modules/ZoomEngineRuntime.cpp native/tests/ZoomEngineRuntimeTest.cpp
git commit -m "Zoom engine: detect roster change by revision, not two full snapshots under mutex_ (#1037)"
```

---

### Task 8: AFTER measurement and outcome

**Files:**
- Modify: `native/tests/RenderEngineMutexChurnQaTest.cpp` (Task 1 Step 4)
- Modify: `docs/superpowers/specs/2026-10-23-render-gather-engine-mutex-design.md` (append an `## Outcome` section)

- [ ] **Step 1:** Switch the harness loop to production shape: `auto inputs = core->prepareRenderEngineInputsUnlocked();` **outside** the `lock_guard`, then `renderDisplayTick(-1, 0, std::move(inputs))` inside, with the hold timed inside the lock. Also print the pre-read's duration distribution separately. That is the render thread's own residual engine wait, which is not a coreMutex hold.
- [ ] **Step 2:** Run a Release build three times with `COREVIDEO_QA_LOCK_CHURN=1`, and once more with `COREVIDEO_QA_ZOOM_HANDOFF_TIMING=1`, on the same machine as Task 1. Check the binary size.
- [ ] **Step 3:** Run the full native suite on the confirmed Release core and record pass/fail counts.
- [ ] **Step 4:** Append `## Outcome` to the spec. It gets a BEFORE/AFTER table (worst, p99, p50 and mean render hold; pre-read p99 and worst; which R-rows still show waits), the suite count, the machine, both commit SHAs, and an explicit **Not established** list. That list must name at least these: no live-meeting soak; the `cmd.handle` 10,111 us worst is not addressed (C1/C2); the frame group's residual waits are unmoved. If AFTER is not better than BEFORE, say so, and do not claim the slice met its acceptance.
- [ ] **Step 5: Commit**

```bash
git add native/tests/RenderEngineMutexChurnQaTest.cpp docs/superpowers/specs/2026-10-23-render-gather-engine-mutex-design.md
git commit -m "Render gather #1037: measured before/after coreMutex hold under roster churn"
```

---

### Task 9: Docs and follow-up issues

**Files:**
- Modify: `docs/reference/gotchas.md`. Extend the #1030 entry's last sentence: the render tick enforces this too, and `AllowEngineMutexScope` lists the sanctioned frame-group exceptions.
- Modify: `docs/reference/realtime-isolation-audit.md`. Add a short "engine mutex under coreMutex" table, the inventory's 14 rows with their decisions, linking the spec.
- Modify: `docs/BACKLOG.md`. Add a row for each new issue in the tier the owner's ranking implies. Do not re-rank.

- [ ] **Step 1:** File these issues (label `backlog`), each linking the spec's inventory:
  1. **Command lane, C1/C2:** take `zoom-media-spine-sync`'s engine half and `zoom-snapshot` off coreMutex. Cite `cmd.handle` 8.6 % over budget and its 10,111 us worst from the 2026-10-23 soak, and Task 8's numbers.
  2. **Frame group, R1/R2/R5:** a fetch-before-lock A/B against frame age (the ingest-to-render handoff distribution plus the render lock wait). Optionally, a lock-free skip for `appendCpuVideoFrames` when there are no ISO arrival queues.
  3. *(Only if Task 8 shows the render thread's own pre-read wait is still material)* a published immutable roster from the engine (the spec's rejected alternative).
- [ ] **Step 2: Commit**

```bash
git add docs/reference/gotchas.md docs/reference/realtime-isolation-audit.md docs/BACKLOG.md
git commit -m "Docs: render tick engine-mutex guardrail and the #1037 inventory"
```

---

## Self-review

- **Spec coverage:** Design §1 (Allow scope) → Task 2. §2 (scope placement) → Task 3. §3 (pre-read and render-loop wiring) → Task 4 Steps 4 and 6. §4 (one home for inputs; R4, R6-R9) → Task 4 Step 5. §5 (R3) → Task 5. §6 (C3) → Task 6. §7 (producer shrink) → Task 7. Acceptance 1 → Tasks 3-6. Acceptance 2 → Tasks 1 and 8. Acceptance 3 → Tasks 7 and 8. Risks: director cadence → Task 4 Step 1; R3 lateness → Task 5 Step 1; the guest A/V race → Review Focus 5; the hidden 2-argument path → Review Focus 1. Out of scope → Task 9 issues.
- **Placeholder scan:** the Task 3 test leaves its setup commands described rather than written: multiview layout, follow route and frame source. The exact JSON shapes live in two existing tests, and the test names them with line numbers. Tasks 5 and 6 name a choice between two seams and ask the executor to report which one they used. Every production-code step carries its real code.
- **Type consistency:** `ZoomRenderView{floorParticipants, directedSpeakerId}` (Task 4 Step 3) is what `prepareRenderEngineInputsUnlocked` unpacks into `RenderEngineInputs{engineConfigured, valid, speakerEpoch, rosterSpeakerEpoch, floorRoster, directedSpeakerId}`. Tasks 4-6 read exactly those fields through `renderEngineInputs_`. `engineVideoDemand_` is written in `renderSyntheticTick` and read only in `prepareRenderEngineInputsUnlocked` (Task 5).
