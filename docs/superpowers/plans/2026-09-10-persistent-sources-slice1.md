# Persistent Sources — Slice 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A media background, loop or clip that is on both Preview and Program is served by ONE decoder with ONE clock, a Take never restarts it, and the Take record can prove that per source instead of guessing.

**Architecture:** Three changes. (1) A pure `SourceContinuityLedger` watches every frame source the render gather sees and assigns a generation that bumps only when a source restarts (frameId regression or reappearance); the Take record captures generations of every source on both sides of a cut and refuses to call it `cut` if a shared source restarted. (2) The core stops renaming Preview media layers to `preview:*` except for the one case that needs it (a paused clip cue), and still images drop their `preview:media:` key, so Preview and Program address the same source. (3) The shell replaces the per-Take playback key with a per-asset **go-live generation** that advances only when a clip ENTERS Program; loops never carry a generation.

**Tech Stack:** C++20 core (`native/`, gtest-style custom runner `corevideo-native-tests.exe`), C# .NET 9 shell (`native-shell/`, xUnit), Node QA scripts (`scripts/qa/`).

**Spec:** `docs/superpowers/specs/2026-09-10-persistent-sources-design.md` (sections 2, 3 tier 2, 4, 5 slice 1).

## Global Constraints

- Windows-first; Metal/CPU-preview parity is slice 4, do not touch `MetalCompositorAdapter.mm`.
- No pixel work under `coreMutex`; the ledger does string/int bookkeeping only (spec §3 "everything stays on the single render thread under coreMutex").
- A `preview:` prefix may survive ONLY for a paused, non-still `media-video` cue layer (spec §2: "The `preview:` namespace and per-Take playback keys are removed" — the cue poster is the one place two playback positions of the same asset legitimately coexist).
- Tests that pin today's per-bus behaviour are REWRITTEN to assert the new contract, never deleted (spec §4).
- The media owner's 16-decoder cap must warn LOUDLY naming the asset, never drop silently (spec §5 risks).
- Every commit ends with: `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>` and `Claude-Session: https://claude.ai/code/session_01SAB8v62BEV8ihmaeGhhtY9`.
- Build the core with `npm run build:native-dev` (`ZOOM_SDK_DIR` set) — never a hand-typed cmake line; run the suite from `native/build-dev/corevideo-native-tests.exe` (no `Release/`). The custom runner ignores `--gtest_filter`, run the whole suite.
- Worktree: `C:\Users\walla\OneDrive\Documents\ChatGPT\CoreVideo Pro\persistent-sources`, branch `codex/persistent-sources`.

---

### Task 1: `SourceContinuityLedger` — a pure generation counter per frame source

**Files:**
- Create: `native/src/core/SourceContinuityLedger.h`
- Create: `native/tests/SourceContinuityLedgerTest.cpp`
- Modify: `native/CMakeLists.txt` (add the test to the `corevideo-native-tests` source list next to `tests/RenderedSceneAttributionTest.cpp`)

**Interfaces:**
- Produces:
  ```cpp
  namespace corevideo::core {
  struct SourceContinuity { std::uint64_t generation = 0; std::int64_t lastFrameId = -1; std::int64_t lastSeenTick = -1; };
  class SourceContinuityLedger {
   public:
    // Called once per render tick with the frame ids the gather saw. A source is
    // "restarted" when its frameId went backwards, or it reappears after being
    // absent for more than `absentTicksBeforeRestart` ticks.
    void observe(const std::string& sourceId, std::int64_t frameId, std::int64_t tick);
    void endTick(std::int64_t tick);  // no-op today; reserved for eviction
    [[nodiscard]] std::optional<SourceContinuity> lookup(const std::string& sourceId) const;
    [[nodiscard]] std::map<std::string, SourceContinuity> snapshot(const std::vector<std::string>& sourceIds) const;
    static constexpr std::int64_t absentTicksBeforeRestart = 30;  // 500 ms at 60 Hz
   private:
    std::map<std::string, SourceContinuity> entries_;
  };
  }
  ```

- [ ] **Step 1: Write the failing tests**

```cpp
// native/tests/SourceContinuityLedgerTest.cpp
#include "core/SourceContinuityLedger.h"
#include <gtest/gtest.h>

using corevideo::core::SourceContinuityLedger;

TEST(SourceContinuityLedger, AFirstSightingIsGenerationOne) {
  SourceContinuityLedger ledger;
  ledger.observe("media:bg", 1, 0);
  const auto c = ledger.lookup("media:bg");
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->generation, 1u);
  EXPECT_EQ(c->lastFrameId, 1);
}

TEST(SourceContinuityLedger, AdvancingFrameIdsKeepTheGeneration) {
  SourceContinuityLedger ledger;
  for (std::int64_t tick = 0; tick < 100; ++tick) ledger.observe("media:bg", tick + 1, tick);
  EXPECT_EQ(ledger.lookup("media:bg")->generation, 1u);
  EXPECT_EQ(ledger.lookup("media:bg")->lastFrameId, 100);
}

TEST(SourceContinuityLedger, AFrameIdRegressionIsARestart) {
  SourceContinuityLedger ledger;
  ledger.observe("media:clip", 40, 0);
  ledger.observe("media:clip", 41, 1);
  ledger.observe("media:clip", 1, 2);  // decoder reopened
  EXPECT_EQ(ledger.lookup("media:clip")->generation, 2u);
}

TEST(SourceContinuityLedger, AHeldFrameIsNotARestart) {
  SourceContinuityLedger ledger;
  ledger.observe("capture:cam", 7, 0);
  ledger.observe("capture:cam", 7, 1);
  ledger.observe("capture:cam", 7, 2);
  EXPECT_EQ(ledger.lookup("capture:cam")->generation, 1u);
}

TEST(SourceContinuityLedger, ReappearingAfterALongAbsenceIsARestart) {
  SourceContinuityLedger ledger;
  ledger.observe("media:bg", 5, 0);
  ledger.observe("media:bg", 6, SourceContinuityLedger::absentTicksBeforeRestart + 2);
  EXPECT_EQ(ledger.lookup("media:bg")->generation, 2u);
}

TEST(SourceContinuityLedger, ABriefGapIsNotARestart) {
  SourceContinuityLedger ledger;
  ledger.observe("media:bg", 5, 0);
  ledger.observe("media:bg", 6, 3);
  EXPECT_EQ(ledger.lookup("media:bg")->generation, 1u);
}

TEST(SourceContinuityLedger, SnapshotOnlyReturnsKnownSources) {
  SourceContinuityLedger ledger;
  ledger.observe("a", 1, 0);
  const auto snap = ledger.snapshot({"a", "b"});
  EXPECT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap.count("a"), 1u);
}
```

- [ ] **Step 2: Add the test to CMake and run it to verify it fails**

Add `tests/SourceContinuityLedgerTest.cpp` after `tests/RenderedSceneAttributionTest.cpp` in `native/CMakeLists.txt`.

Run: `npm run build:native-dev`
Expected: compile error — `core/SourceContinuityLedger.h` not found.

- [ ] **Step 3: Write the header**

```cpp
// native/src/core/SourceContinuityLedger.h
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace corevideo::core {

// WHICH SOURCES RESTARTED, PER SOURCE, ON EVIDENCE.
//
// A persistent source (spec 2026-09-10-persistent-sources-design §2) has one
// clock: its frame ids only ever advance. A frame id that goes BACKWARDS is a
// decoder that reopened; a source that vanishes for longer than a beat and
// comes back cold-started. Both are "restarts", and the Take record uses this
// ledger to refuse the word "cut" when any source present on both sides of a
// Take restarted across it.
//
// Pure bookkeeping (strings and ints), safe to run under coreMutex on the
// render gather. No pixels, no allocation beyond the map.
struct SourceContinuity {
  std::uint64_t generation = 0;
  std::int64_t lastFrameId = -1;
  std::int64_t lastSeenTick = -1;
};

class SourceContinuityLedger {
 public:
  static constexpr std::int64_t absentTicksBeforeRestart = 30;  // 500 ms at 60 Hz

  void observe(const std::string& sourceId, std::int64_t frameId, std::int64_t tick) {
    auto& entry = entries_[sourceId];
    const bool first = entry.generation == 0;
    const bool regressed = !first && frameId < entry.lastFrameId;
    const bool reappeared = !first && (tick - entry.lastSeenTick) > absentTicksBeforeRestart;
    if (first || regressed || reappeared) ++entry.generation;
    entry.lastFrameId = frameId;
    entry.lastSeenTick = tick;
  }

  void endTick(std::int64_t /*tick*/) {}

  [[nodiscard]] std::optional<SourceContinuity> lookup(const std::string& sourceId) const {
    const auto found = entries_.find(sourceId);
    if (found == entries_.end()) return std::nullopt;
    return found->second;
  }

  [[nodiscard]] std::map<std::string, SourceContinuity> snapshot(
      const std::vector<std::string>& sourceIds) const {
    std::map<std::string, SourceContinuity> result;
    for (const auto& id : sourceIds) {
      if (const auto found = entries_.find(id); found != entries_.end()) result.emplace(id, found->second);
    }
    return result;
  }

 private:
  std::map<std::string, SourceContinuity> entries_;
};

}  // namespace corevideo::core
```

- [ ] **Step 4: Build and run the suite**

Run: `npm run build:native-dev && ./native/build-dev/corevideo-native-tests.exe 2>/dev/null | tail -1`
Expected: the 7 `SourceContinuityLedger.*` tests appear as PASSED and the summary is `N tests passed, 0 failed` with N = previous count + 7.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/SourceContinuityLedger.h native/tests/SourceContinuityLedgerTest.cpp native/CMakeLists.txt
git commit -m "Count source restarts per frame source on evidence"
```

---

### Task 2: The Take record refuses `cut` when a shared source restarted

**Files:**
- Modify: `native/src/core/TakeRecordPolicy.h`
- Modify: `native/src/core/MediaCore.h` (TakeRecord struct near line 690; add `SourceContinuityLedger sourceContinuity_;` and `std::int64_t renderTickCounter_ = 0;`)
- Modify: `native/src/core/MediaCore.cpp` (`armTakeRecord` ~1921, `completeTakeRecord` ~1941, `takeRecordsState` ~1871, the render gather in `renderSyntheticTick` right after `videoFrames` is final — just before the program plan is built, ~line 6007)
- Test: `native/tests/RenderedSceneAttributionTest.cpp`

**Interfaces:**
- Consumes: `SourceContinuityLedger` (Task 1).
- Produces:
  ```cpp
  // TakeRecordPolicy.h additions
  struct SharedSourceContinuity {
    std::string sourceId;
    std::uint64_t generationBefore = 0, generationAfter = 0;
    std::int64_t frameIdBefore = -1, frameIdAfter = -1;
  };
  // Observation gains:
  std::vector<SharedSourceContinuity> sharedSources;   // sources present in BOTH plans
  // Verdict gains:
  std::vector<std::string> restartedSources;           // sourceIds whose generation changed
  bool sharedSourceRestarted = false;
  ```
  Snapshot `takeRecords.records[].sources[]` = `{sourceId, generationBefore, generationAfter, frameIdBefore, frameIdAfter, restarted}`; `records[].restartedSources` (array of ids); `records[].sharedSourceRestarted` (bool). Log line gains `restarted=[a,b]`.

- [ ] **Step 1: Write the failing policy test**

Append to `native/tests/RenderedSceneAttributionTest.cpp`:

```cpp
TEST(TakeRecordPolicyRules, ASharedSourceThatRestartedDeniesTheCut) {
  TakeRecordPolicy::Observation observation;
  observation.hasWallAfter = false;
  observation.sharedSources.push_back({"background:bg", 1, 2, 40, 1});
  observation.sharedSources.push_back({"zoom:7", 3, 3, 500, 501});
  const auto verdict = TakeRecordPolicy::evaluate(observation);
  EXPECT_TRUE(verdict.sharedSourceRestarted);
  ASSERT_EQ(verdict.restartedSources.size(), 1u);
  EXPECT_EQ(verdict.restartedSources[0], "background:bg");
  EXPECT_STREQ(verdict.verdict, "rebuilt");
}

TEST(TakeRecordPolicyRules, SharedSourcesThatKeptTheirGenerationAllowACut) {
  TakeRecordPolicy::Observation observation;
  observation.hasWallAfter = true;
  observation.wallAdoptedSettled = true;
  observation.sharedSources.push_back({"background:bg", 1, 1, 40, 45});
  const auto verdict = TakeRecordPolicy::evaluate(observation);
  EXPECT_FALSE(verdict.sharedSourceRestarted);
  EXPECT_STREQ(verdict.verdict, "cut");
}

TEST(TakeRecordPolicyRules, ANoWallTakeWithNoRestartIsACutNotNoWall) {
  // A plain scene-to-scene cut that shares a background: the verdict must be
  // about the sources, not only about the wall.
  TakeRecordPolicy::Observation observation;
  observation.hasWallAfter = false;
  observation.sharedSources.push_back({"background:bg", 2, 2, 10, 11});
  EXPECT_STREQ(TakeRecordPolicy::evaluate(observation).verdict, "cut");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `npm run build:native-dev`
Expected: compile error — `sharedSources` is not a member of `Observation`.

- [ ] **Step 3: Extend the policy**

Replace the body of `native/src/core/TakeRecordPolicy.h` from `struct TakeRecordPolicy {` to the end with:

```cpp
struct TakeRecordPolicy {
  struct SharedSourceContinuity {
    std::string sourceId;
    std::uint64_t generationBefore = 0, generationAfter = 0;
    std::int64_t frameIdBefore = -1, frameIdAfter = -1;
  };

  struct Observation {
    bool hasWallAfter = false;         // the taken scene carries a Tiles wall
    bool wallAdoptedSettled = false;   // adoptSettledFrom() returned true
    bool liveBackgroundExpected = false;   // the wall declares a live source background
    bool liveBackgroundEmitted = false;    // ...and it was in the first program frame
    std::uint64_t subscriptionChurnDelta = 0;  // real re-subscribes across the take
    // Every frame source present in BOTH the outgoing and incoming plans, with
    // its SourceContinuityLedger generation on each side of the take.
    std::vector<SharedSourceContinuity> sharedSources;
  };

  struct Verdict {
    const char* wall = "none";        // none | adopted-settled | reset
    const char* verdict = "no-wall";  // cut | rebuilt | no-wall
    bool backgroundDropped = false;
    bool subscriptionsChurned = false;
    bool sharedSourceRestarted = false;
    std::vector<std::string> restartedSources;
  };

  [[nodiscard]] static Verdict evaluate(const Observation& observation) {
    Verdict verdict;
    verdict.backgroundDropped =
        observation.liveBackgroundExpected && !observation.liveBackgroundEmitted;
    verdict.subscriptionsChurned = observation.subscriptionChurnDelta > 0;
    for (const auto& source : observation.sharedSources) {
      if (source.generationAfter != source.generationBefore) {
        verdict.restartedSources.push_back(source.sourceId);
      }
    }
    verdict.sharedSourceRestarted = !verdict.restartedSources.empty();

    if (!observation.hasWallAfter) {
      verdict.wall = "none";
      // No wall: the take is about its shared sources. Nothing shared and
      // nothing restarted is the old "no-wall"; a restart is a rebuild on air.
      if (verdict.sharedSourceRestarted) verdict.verdict = "rebuilt";
      else verdict.verdict = observation.sharedSources.empty() ? "no-wall" : "cut";
      return verdict;
    }
    if (!observation.wallAdoptedSettled) {
      verdict.wall = "reset";
      verdict.verdict = "rebuilt";
      return verdict;
    }
    verdict.wall = "adopted-settled";
    verdict.verdict = (verdict.backgroundDropped || verdict.subscriptionsChurned ||
                       verdict.sharedSourceRestarted)
                          ? "rebuilt"
                          : "cut";
    return verdict;
  }
};
```

Add `#include <string>` and `#include <vector>` at the top of the header.

- [ ] **Step 4: Build, run, confirm the three policy tests pass and the existing `TheWallVerdictSeparatesACutFromARebuild` still passes**

Run: `npm run build:native-dev && ./native/build-dev/corevideo-native-tests.exe 2>/dev/null | grep -E 'TakeRecordPolicyRules|tests passed'`
Expected: all `TakeRecordPolicyRules.*` PASSED; total `0 failed`.

- [ ] **Step 5: Write the failing end-to-end test**

Append to `native/tests/RenderedSceneAttributionTest.cpp` (uses `SolidMediaFrameSource`-style fake — copy the class from `MediaCoreCommandTest.cpp` into this file's anonymous namespace as `CountingMediaFrameSource` with one change: `frame.frameId = ++frameIds[frame.participantId];` per source, and a public `void restart(const std::string& sourceId) { frameIds[sourceId] = 0; }`):

```cpp
namespace {
corevideo::rpc::Json backgroundScene(const char* sceneId, const char* assetId) {
  return corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"},
      {"sceneId", sceneId},
      {"background", corevideo::rpc::Json::Object{
          {"mediaAssetId", assetId}, {"mediaAssetName", "bg"}, {"mediaAssetKind", "video"},
          {"mediaAssetPath", "C:\\media\\bg.mp4"}, {"playing", true}}},
      {"routes", corevideo::rpc::Json::Array{}}};
}
}  // namespace

TEST(TakeRecord, ASharedBackgroundThatKeptItsGenerationIsACut) {
  auto modules = corevideo::modules::createStubModules();
  modules.compositor = std::make_unique<DeliveringCompositor>();
  auto media = std::make_unique<CountingMediaFrameSource>();
  modules.mediaFrames = std::move(media);
  MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  (void)core.applyCommands(corevideo::rpc::Json::Array{backgroundScene("scene-a", "bg-1")});
  for (int i = 0; i < 5; ++i) core.renderDisplayTick();
  (void)core.applyCommands(corevideo::rpc::Json::Array{backgroundScene("scene-b", "bg-1")});
  core.renderDisplayTick();

  const auto& records = core.sessionState().get("takeRecords")->get("records")->asArray();
  ASSERT_EQ(records.size(), 2u);
  const auto& take = records[1];
  EXPECT_EQ(take.getString("verdict"), "cut");
  EXPECT_FALSE(take.get("sharedSourceRestarted")->asBool(true));
  const auto& sources = take.get("sources")->asArray();
  ASSERT_EQ(sources.size(), 1u);
  EXPECT_EQ(sources[0].getString("sourceId"), "background:bg-1");
  EXPECT_EQ(sources[0].getNumber("generationBefore"), 1);
  EXPECT_EQ(sources[0].getNumber("generationAfter"), 1);
  EXPECT_GT(sources[0].getNumber("frameIdAfter"), sources[0].getNumber("frameIdBefore"));
}

TEST(TakeRecord, ASharedBackgroundThatRestartedAcrossTheTakeIsRebuilt) {
  auto modules = corevideo::modules::createStubModules();
  modules.compositor = std::make_unique<DeliveringCompositor>();
  auto media = std::make_unique<CountingMediaFrameSource>();
  auto* mediaPtr = media.get();
  modules.mediaFrames = std::move(media);
  MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  (void)core.applyCommands(corevideo::rpc::Json::Array{backgroundScene("scene-a", "bg-1")});
  for (int i = 0; i < 5; ++i) core.renderDisplayTick();
  (void)core.applyCommands(corevideo::rpc::Json::Array{backgroundScene("scene-b", "bg-1")});
  mediaPtr->restart("background:bg-1");  // the decoder reopened on the take
  core.renderDisplayTick();

  const auto& records = core.sessionState().get("takeRecords")->get("records")->asArray();
  const auto& take = records[1];
  EXPECT_EQ(take.getString("verdict"), "rebuilt");
  EXPECT_TRUE(take.get("sharedSourceRestarted")->asBool(false));
  ASSERT_EQ(take.get("restartedSources")->asArray().size(), 1u);
  EXPECT_EQ(take.get("restartedSources")->asArray()[0].asString(), "background:bg-1");
}
```

- [ ] **Step 6: Run to verify they fail**

Run: `npm run build:native-dev && ./native/build-dev/corevideo-native-tests.exe 2>/dev/null | grep -E 'TakeRecord\.|tests passed'`
Expected: the two new tests FAIL (no `sources` node / verdict `no-wall`).

- [ ] **Step 7: Wire the ledger into MediaCore**

In `native/src/core/MediaCore.h`:
- add `#include "core/SourceContinuityLedger.h"`;
- in `TakeRecord` add `std::map<std::string, SourceContinuity> continuityBefore;` and `std::vector<std::string> fromSourceIds;` (these are the FRAME source ids, i.e. `layer.sourceId` for layers with a non-empty one, else `participantId`);
- add members `SourceContinuityLedger sourceContinuity_;` and `std::int64_t renderTickCounter_ = 0;` next to `takeRecords_`;
- add a private helper declaration `static std::vector<std::string> renderPlanSourceIds(const modules::CompositorRenderPlan& plan);`.

In `native/src/core/MediaCore.cpp`:

1. Next to `renderPlanLayerIds`, add:
```cpp
std::vector<std::string> MediaCore::renderPlanSourceIds(const modules::CompositorRenderPlan& plan) {
  std::vector<std::string> ids;
  for (const auto& layer : plan.layers) {
    const auto& id = !layer.sourceId.empty() ? layer.sourceId : layer.participantId;
    if (!id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
  }
  return ids;
}
```
2. In `renderSyntheticTick`, immediately after the media frames are appended to `videoFrames` (the block at ~6093 ending `videoFrames.insert(videoFrames.end(), mediaFrames.begin(), mediaFrames.end());`) and BEFORE the program plan is built, add:
```cpp
  ++renderTickCounter_;
  for (const auto& frame : videoFrames) {
    sourceContinuity_.observe(frame.participantId, frame.frameId, renderTickCounter_);
  }
  sourceContinuity_.endTick(renderTickCounter_);
```
   If the media poll happens AFTER the program plan build in this function, move the observe loop to after the last `videoFrames.insert(...)` in the tick and read the plan's source ids there; the requirement is that `completeTakeRecord` sees this tick's generations.
3. In `armTakeRecord`, after `record.fromLayerIds = renderPlanLayerIds(outgoing);` add:
```cpp
  record.fromSourceIds = renderPlanSourceIds(outgoing);
  record.continuityBefore = sourceContinuity_.snapshot(record.fromSourceIds);
```
4. In `completeTakeRecord`, before `record.verdict = TakeRecordPolicy::evaluate(...)`, add:
```cpp
  const auto toSourceIds = renderPlanSourceIds(programPlan);
  const auto continuityAfter = sourceContinuity_.snapshot(toSourceIds);
  for (const auto& id : toSourceIds) {
    const auto before = record.continuityBefore.find(id);
    const auto after = continuityAfter.find(id);
    if (before == record.continuityBefore.end() || after == continuityAfter.end()) continue;
    record.observation.sharedSources.push_back({id, before->second.generation, after->second.generation,
                                                before->second.lastFrameId, after->second.lastFrameId});
  }
```
5. In `takeRecordsState`, add to each record object:
```cpp
        {"sources", [&] {
           rpc::Json::Array sources;
           for (const auto& s : record.observation.sharedSources) {
             const bool restarted = s.generationAfter != s.generationBefore;
             sources.emplace_back(rpc::Json::Object{
                 {"sourceId", s.sourceId},
                 {"generationBefore", static_cast<double>(s.generationBefore)},
                 {"generationAfter", static_cast<double>(s.generationAfter)},
                 {"frameIdBefore", static_cast<double>(s.frameIdBefore)},
                 {"frameIdAfter", static_cast<double>(s.frameIdAfter)},
                 {"restarted", restarted}});
           }
           return sources;
         }()},
        {"restartedSources", stringArray(record.verdict.restartedSources)},
        {"sharedSourceRestarted", record.verdict.sharedSourceRestarted},
```
6. In the `[take]` log line, append ` restarted=[%s]` with `joinLayerIds(record.verdict.restartedSources).c_str()`.

Note: `completeTakeRecord` is called from the render tick with the program plan; confirm the observe loop runs BEFORE that call in the same tick (search for the `completeTakeRecord(` call site and place the loop above it if needed).

- [ ] **Step 8: Build and run; confirm both new TakeRecord tests and the whole suite pass**

Run: `npm run build:native-dev && ./native/build-dev/corevideo-native-tests.exe 2>/dev/null | tail -1`
Expected: `0 failed`. Also `ATakeIsRecordedWithBothSidesOfTheWall` still expects `rebuilt` (wall reset) — unchanged.

- [ ] **Step 9: Commit**

```bash
git add native/src/core/TakeRecordPolicy.h native/src/core/MediaCore.h native/src/core/MediaCore.cpp native/tests/RenderedSceneAttributionTest.cpp
git commit -m "Deny a clean-cut verdict when a shared source restarted across the take"
```

---

### Task 3: One frame-source identity per media asset across Preview and Program

**Files:**
- Modify: `native/src/core/MediaCore.cpp` — `buildPreviewCompositorRenderPlan` (~5126-5141) and `syncStillMediaDesired` (~2113-2132)
- Modify: `native/tests/MediaCoreCommandTest.cpp` (`DecodesFirstFrameForPausedPreviewCue` ~2850, `KeepsSceneBackgroundAndProgramMediaRouteFrameSourcesDistinct` ~4095)
- Modify: `native/tests/StillMediaFrameCacheTest.cpp` (~350-380, the `preview:media:` namespace test)
- Test (new cases): `native/tests/MediaCoreCommandTest.cpp`

**Interfaces:**
- Produces: the rule `previewMediaSourceIdFor(layer)`: a Preview layer keeps its Program source id (`background:<asset>` / `media:<asset>`) UNLESS `layer.kind == "media-video" && !layer.mediaAssetPlaying && !isStillImageMediaAsset(kind, path)`, in which case it becomes `preview:media:<asset>` (a paused clip cue poster). Still routes are keyed `media:<asset>` on both buses.

- [ ] **Step 1: Write the failing tests**

Append to `native/tests/MediaCoreCommandTest.cpp` (reuse `SolidMediaFrameSource`, which records `seenSourceIds` per poll):

```cpp
TEST(MediaCoreCommand, ALoopingBackgroundHasOneFrameSourceIdOnBothBuses) {
  auto modules = corevideo::modules::createStubModules();
  auto mediaFrames = std::make_unique<SolidMediaFrameSource>();
  auto* mediaFramesPtr = mediaFrames.get();
  modules.mediaFrames = std::move(mediaFrames);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto background = corevideo::rpc::Json::Object{
      {"mediaAssetId", "bg-loop"}, {"mediaAssetName", "Loop"}, {"mediaAssetKind", "video"},
      {"mediaAssetPath", "C:\\media\\loop.mp4"}, {"playing", true}};
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "pgm"},
                                   {"background", background}, {"routes", corevideo::rpc::Json::Array{}}},
      corevideo::rpc::Json::Object{{"type", "set-preview-scene"}, {"sceneId", "pvw"},
                                   {"background", background}, {"routes", corevideo::rpc::Json::Array{}}},
  });
  mediaCore.renderDisplayTick();

  // Program and Preview both asked for the SAME source id: one decoder, one clock.
  std::vector<std::string> ids = mediaFramesPtr->seenSourceIds;
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0], "background:bg-loop");
  EXPECT_EQ(ids[1], "background:bg-loop");
}

TEST(MediaCoreCommand, APausedClipCueInPreviewKeepsItsOwnPosterSource) {
  auto modules = corevideo::modules::createStubModules();
  auto mediaFrames = std::make_unique<SolidMediaFrameSource>();
  auto* mediaFramesPtr = mediaFrames.get();
  modules.mediaFrames = std::move(mediaFrames);
  corevideo::core::MediaCore mediaCore(std::move(modules));

  const auto clipRoute = [](bool playing) {
    return corevideo::rpc::Json::Object{
        {"routeId", "clip"}, {"mode", "fixed"}, {"mediaAssetId", "clip-1"}, {"mediaAssetName", "Clip"},
        {"mediaAssetKind", "video"}, {"mediaAssetPath", "C:\\media\\clip.mp4"},
        {"mediaPlaybackKey", "media:clip-1"}, {"mediaAssetPlaying", playing},
        {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}};
  };
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "pgm"},
                                   {"routes", corevideo::rpc::Json::Array{clipRoute(true)}}},
      corevideo::rpc::Json::Object{{"type", "set-preview-scene"}, {"sceneId", "pvw"},
                                   {"routes", corevideo::rpc::Json::Array{clipRoute(false)}}},
  });
  mediaCore.renderDisplayTick();

  std::vector<std::string> ids = mediaFramesPtr->seenSourceIds;
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0], "media:clip-1");
  EXPECT_EQ(ids[1], "preview:media:clip-1");  // the ONE case a second position is legitimate
}
```

Then update the two existing tests:
- `KeepsSceneBackgroundAndProgramMediaRouteFrameSourcesDistinct`: change the route's `mediaPlaybackKey` from `"program-take:8:media:clip-intro"` to `"media:clip-intro"` and the matching `EXPECT_EQ(seenPlaybackKeys[1], "media:clip-intro")`. (Background and route stay distinct: a loop source and a clip source over the same file are two sources by kind, spec §2.)
- `StillMediaFrameCacheTest.cpp:350-380`: the test that expects `frames[0].participantId == "preview:media:logo-9"` must now expect `"media:logo-9"` and assert that a preview-only still route produces a `media:` key (rename the test `APreviewStillRouteSharesTheProgramKey`).
- `DecodesFirstFrameForPausedPreviewCue` (~2850): leave as is — the cue path is preserved; only confirm it still uses `layer.sourceId = "preview:media:diagnostic"` and passes.

- [ ] **Step 2: Run to verify the two new tests fail**

Run: `npm run build:native-dev && ./native/build-dev/corevideo-native-tests.exe 2>/dev/null | grep -E 'ALoopingBackground|APausedClipCue|tests passed'`
Expected: both FAIL (`preview:background:bg-loop` observed).

- [ ] **Step 3: Implement the rule in `buildPreviewCompositorRenderPlan`**

Replace the loop at the end of `MediaCore::buildPreviewCompositorRenderPlan` with:

```cpp
  // PERSISTENT SOURCES (spec 2026-09-10 §2): Preview and Program address the
  // SAME media source. A looping background or a still that is on both buses is
  // one decoder with one clock, so a Take cannot restart it. The single case
  // that keeps a Preview-only namespace is a PAUSED CLIP CUE: its poster frame
  // is a different playback position from Program's rolling copy, and the two
  // must not replace each other in the frame set.
  for (auto& layer : plan.layers) {
    if (layer.mediaAssetId.empty()) continue;
    const bool pausedClipCue = layer.kind == "media-video" && !layer.mediaAssetPlaying &&
                               !modules::isStillImageMediaAsset(layer.mediaAssetKind, layer.mediaAssetPath);
    if (!pausedClipCue) continue;
    const auto sourceId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
    layer.sourceId = "preview:" + sourceId;
  }
```

And in `syncStillMediaDesired`, change `addRoutes(previewSceneRoutes_, "preview:media:");` to `addRoutes(previewSceneRoutes_, "media:");` (the cache dedups identical `sourceKey` + path, so a still on both buses is one entry).

- [ ] **Step 4: Build, run the whole suite**

Run: `npm run build:native-dev && ./native/build-dev/corevideo-native-tests.exe 2>/dev/null | tail -1`
Expected: `0 failed`. If `StillMediaFrameCache` tests fail on a duplicate `sourceKey`, dedup in `syncStillMediaDesired` before `setDesired` (keep the first request per `sourceKey`).

- [ ] **Step 5: Commit**

```bash
git add native/src/core/MediaCore.cpp native/tests/MediaCoreCommandTest.cpp native/tests/StillMediaFrameCacheTest.cpp
git commit -m "Address the same media source from Preview and Program"
```

---

### Task 4: One decoder per shared media request, and a loud cap

**Files:**
- Modify: `native/src/modules/OwnedMediaFrameSource.h` (`manage()` cap warning ~line 211)
- Test: `native/tests/MediaPlaybackTimelineTest.cpp` (where the existing `OwnedMediaFrameSource.*` tests live)

**Interfaces:**
- Consumes: `OwnedMediaFrameSource::requests()` key = `sourceId|path|assetId|playbackKey|playing|loop` (unchanged).
- Produces: cap warning text `Media decoder capacity reached (16 active/retiring assets); not starting <sourceId>.`

- [ ] **Step 1: Write the failing tests**

Append to `native/tests/MediaPlaybackTimelineTest.cpp`:

```cpp
TEST(OwnedMediaFrameSource, TheSameRequestFromTwoBusesStartsOneDecoder) {
  auto gate = std::make_shared<DecodeGate>();
  std::atomic<int> created{0};
  OwnedMediaFrameSource source([gate, &created] { ++created; return std::make_unique<TestDecoder>(gate); });
  auto program = workerLayer();      // sourceId media:test, playing, same path/key
  auto preview = workerLayer();
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  (void)source.pollMediaFrames({program, preview}, now);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (created.load() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(created.load(), 1);
}

TEST(OwnedMediaFrameSource, TheCapWarningNamesTheAssetItRefused) {
  auto gate = std::make_shared<DecodeGate>();
  OwnedMediaFrameSource source([gate] { return std::make_unique<TestDecoder>(gate); });
  std::vector<CompositorRenderPlanLayer> layers;
  for (int i = 0; i < 17; ++i) {
    auto layer = workerLayer();
    layer.mediaAssetId = "asset-" + std::to_string(i);
    layer.sourceId = "media:" + layer.mediaAssetId;
    layers.push_back(layer);
  }
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  (void)source.pollMediaFrames(layers, now);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto warnings = source.warnings();
  const bool named = std::any_of(warnings.begin(), warnings.end(), [](const std::string& w) {
    return w.find("Media decoder capacity reached") != std::string::npos && w.find("media:asset-16") != std::string::npos;
  });
  EXPECT_TRUE(named);
}
```

(`#include <algorithm>` and `<atomic>` at the top of the file if missing.)

- [ ] **Step 2: Run to verify**

Run: `npm run build:native-dev && ./native/build-dev/corevideo-native-tests.exe 2>/dev/null | grep -E 'OwnedMediaFrameSource|tests passed'`
Expected: `TheSameRequestFromTwoBusesStartsOneDecoder` PASSES already (the request map keys identical layers together — this test pins the guarantee); `TheCapWarningNamesTheAssetItRefused` FAILS (warning does not name the asset).

- [ ] **Step 3: Name the asset in the cap warning**

In `OwnedMediaFrameSource::manage()`, replace
```cpp
if (entries_.size() + retired.size() >= 16) { warnings_.push_back("Media decoder capacity reached (16 active/retiring assets)."); continue; }
```
with
```cpp
if (entries_.size() + retired.size() >= 16) {
  warnings_.push_back("Media decoder capacity reached (16 active/retiring assets); not starting " +
                      (layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId) + ".");
  continue;
}
```

- [ ] **Step 4: Build and run the suite**

Run: `npm run build:native-dev && ./native/build-dev/corevideo-native-tests.exe 2>/dev/null | tail -1`
Expected: `0 failed`.

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/OwnedMediaFrameSource.h native/tests/MediaPlaybackTimelineTest.cpp
git commit -m "Pin one decoder per shared media request and name the asset a full cap refuses"
```

---

### Task 5: Shell go-live policy replaces the per-Take playback key

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/Services/MediaRoutePlaybackService.cs`
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/Transport/ITransportHost.cs:78` (`IncrementProgramMediaPlaybackTakeVersion` → `RecordProgramMediaGoLive(IReadOnlyList<SourceRoute> previousProgramRoutes)`)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/Transport/TransportCoordinator.cs:145-156`
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs` (field `_programMediaPlaybackTakeVersion` :785; uses at :4068-4073, :6102-6108, :8735-8740, :8906-8914, :13243-13249)
- Test: `native-shell/CoreVideoPro.WinUI.Tests/MediaRoutePlaybackServiceTests.cs`, `native-shell/CoreVideoPro.WinUI.Tests/TransportCoordinatorTests.cs`

**Interfaces:**
- Produces (`MediaRoutePlaybackService`):
  ```csharp
  public sealed class MediaGoLiveLedger
  {
      // asset id -> generation; a generation advances ONLY when the asset ENTERS Program.
      public int GenerationOf(string mediaAssetId);
      // Returns the asset ids that went live (were not routed on Program before, are now).
      public IReadOnlyList<string> RecordTake(IReadOnlyList<SourceRoute> previousProgramRoutes, IReadOnlyList<SourceRoute> programRoutes);
      // Operator pressed Play on a program-routed clip: roll from 0.
      public void RecordRestart(string mediaAssetId);
  }
  public static string BuildSceneMediaPlaybackKey(string mediaAssetId, bool loop, int goLiveGeneration)
      => loop ? $"media:{id}" : $"media:{id}:live:{goLiveGeneration}";
  public static SceneRoutePlayback ResolveSceneRoutePlayback(string mediaAssetId, bool isProgramScene, bool loop,
      string? selectedMediaAssetId, bool selectedMediaAssetPlaying, IReadOnlyList<SourceRoute> programRoutes, int goLiveGeneration);
  ```
  Rules: loop → `Playing: true` on BOTH buses, key `media:<id>`. Clip → Program: `ShouldPlaySceneMediaRoute(...)` (unchanged semantics); Preview: `Playing: false`; key `media:<id>:live:<gen>`.
- `ITransportHost.RecordProgramMediaGoLive(IReadOnlyList<SourceRoute> previousProgramRoutes)` replaces `IncrementProgramMediaPlaybackTakeVersion()`.
- Route loop-ness: `SourceRoute` has no loop flag today; the route media asset's kind decides: `MediaAsset.Kind == "background"` or the asset id is a scene background → loop. Add `public static bool IsLoopingAsset(MediaAsset asset) => string.Equals(asset.Kind, "background", StringComparison.OrdinalIgnoreCase) || asset.Loop;` — check `MediaAsset` in `Models/ProductionModels.cs` for an existing `Loop` property; if none exists, treat only scene backgrounds as loops (they are built by `MediaCoreCommandBuilder` with `playing = background.Playing` and never carry a playback key, so they are unaffected by this task) and clips as non-loop.

- [ ] **Step 1: Rewrite the pinned tests to the new contract**

In `MediaRoutePlaybackServiceTests.cs` replace the four key/take tests (`BuildSceneMediaPlaybackKey_KeepsPreviewKeyStableAndPaused`, `BuildSceneMediaPlaybackKey_ChangesProgramKeyPerTake`, `BuildSceneMediaPlaybackKey_UsesProgramTakeKeyWhenProgramMediaIsPaused`, `ShouldAdvanceProgramPlaybackKey_OnlyAdvancesWhenStartingProgramRoutedMedia`, `ResolveSceneRoutePlayback_KeepsPreviewMediaPausedWithStablePreviewKey`, `ResolveSceneRoutePlayback_AutoplaysProgramMediaWithTakeVersionKey`, `ResolveSceneRoutePlayback_KeepsPausedSelectedProgramMediaOnProgramKey`) with:

```csharp
[Fact]
public void BuildSceneMediaPlaybackKey_ALoopHasNoGeneration()
{
    Assert.Equal("media:bg", MediaRoutePlaybackService.BuildSceneMediaPlaybackKey("bg", loop: true, goLiveGeneration: 7));
}

[Fact]
public void BuildSceneMediaPlaybackKey_AClipCarriesItsGoLiveGeneration()
{
    Assert.Equal("media:clip:live:3", MediaRoutePlaybackService.BuildSceneMediaPlaybackKey("clip", loop: false, goLiveGeneration: 3));
}

[Fact]
public void GoLiveLedger_AdvancesOnlyWhenAnAssetEntersProgram()
{
    var ledger = new MediaGoLiveLedger();
    var clip = FixedMediaRoute("clip");
    var none = Array.Empty<SourceRoute>();
    Assert.Equal(new[] { "clip" }, ledger.RecordTake(none, new[] { clip }));
    Assert.Equal(1, ledger.GenerationOf("clip"));
    // A second Take with the clip STILL on Program does not restart it.
    Assert.Empty(ledger.RecordTake(new[] { clip }, new[] { clip }));
    Assert.Equal(1, ledger.GenerationOf("clip"));
    // Leaving and re-entering Program rolls it again.
    Assert.Empty(ledger.RecordTake(new[] { clip }, none));
    Assert.Equal(new[] { "clip" }, ledger.RecordTake(none, new[] { clip }));
    Assert.Equal(2, ledger.GenerationOf("clip"));
}

[Fact]
public void GoLiveLedger_OperatorRestartAdvancesTheGeneration()
{
    var ledger = new MediaGoLiveLedger();
    ledger.RecordTake(Array.Empty<SourceRoute>(), new[] { FixedMediaRoute("clip") });
    ledger.RecordRestart("clip");
    Assert.Equal(2, ledger.GenerationOf("clip"));
}

[Fact]
public void ResolveSceneRoutePlayback_ALoopPlaysOnBothBusesWithOneKey()
{
    var routes = new[] { FixedMediaRoute("bg") };
    var program = MediaRoutePlaybackService.ResolveSceneRoutePlayback("bg", isProgramScene: true, loop: true, null, false, routes, 0);
    var preview = MediaRoutePlaybackService.ResolveSceneRoutePlayback("bg", isProgramScene: false, loop: true, null, false, routes, 0);
    Assert.True(program.Playing);
    Assert.True(preview.Playing);
    Assert.Equal(program.MediaPlaybackKey, preview.MediaPlaybackKey);
}

[Fact]
public void ResolveSceneRoutePlayback_AClipIsPausedInPreviewAndRollsOnProgram()
{
    var routes = new[] { FixedMediaRoute("clip") };
    var preview = MediaRoutePlaybackService.ResolveSceneRoutePlayback("clip", isProgramScene: false, loop: false, null, false, routes, 1);
    var program = MediaRoutePlaybackService.ResolveSceneRoutePlayback("clip", isProgramScene: true, loop: false, null, false, routes, 1);
    Assert.False(preview.Playing);
    Assert.True(program.Playing);
    Assert.Equal("media:clip:live:1", program.MediaPlaybackKey);
    Assert.Equal("media:clip:live:1", preview.MediaPlaybackKey);
}
```

`FixedMediaRoute(string assetId)` — reuse the existing helper in that test file that builds a `SourceRoute` with `Mode = SourceRouteMode.Fixed` and `ParticipantId = ShowInputRosterService.ToMediaSourceId(assetId)`; if it has another name, use that name.

In `TransportCoordinatorTests.cs`: rename the fake host's `IncrementProgramMediaPlaybackTakeVersion()` to `RecordProgramMediaGoLive(IReadOnlyList<SourceRoute> previousProgramRoutes) => GoLiveRecords++;`, rename `TakeVersionIncrements` → `GoLiveRecords`, and keep the three assertions (`Assert.Equal(1, host.GoLiveRecords)` at the former :321 and :461; `:308` unchanged).

- [ ] **Step 2: Run the shell tests to verify they fail**

Run: `dotnet test native-shell/CoreVideoPro.WinUI.Tests/CoreVideoPro.WinUI.Tests.csproj -v q --nologo 2>&1 | tail -3`
Expected: build errors — `MediaGoLiveLedger` does not exist; `RecordProgramMediaGoLive` not on `ITransportHost`.

- [ ] **Step 3: Implement the service**

In `MediaRoutePlaybackService.cs` add:

```csharp
public sealed class MediaGoLiveLedger
{
    private readonly Dictionary<string, int> _generations = new(StringComparer.Ordinal);

    public int GenerationOf(string mediaAssetId) =>
        _generations.TryGetValue(mediaAssetId, out var generation) ? generation : 0;

    public IReadOnlyList<string> RecordTake(IReadOnlyList<SourceRoute> previousProgramRoutes, IReadOnlyList<SourceRoute> programRoutes)
    {
        var before = ProgramMediaAssetIds(previousProgramRoutes);
        var wentLive = new List<string>();
        foreach (var assetId in ProgramMediaAssetIds(programRoutes))
        {
            if (before.Contains(assetId)) continue;
            _generations[assetId] = GenerationOf(assetId) + 1;
            wentLive.Add(assetId);
        }
        return wentLive;
    }

    public void RecordRestart(string mediaAssetId)
    {
        if (string.IsNullOrWhiteSpace(mediaAssetId)) return;
        _generations[mediaAssetId] = GenerationOf(mediaAssetId) + 1;
    }

    private static HashSet<string> ProgramMediaAssetIds(IReadOnlyList<SourceRoute> routes)
    {
        var ids = new HashSet<string>(StringComparer.Ordinal);
        foreach (var route in routes)
        {
            if (route.Mode == SourceRouteMode.Fixed &&
                ShowInputRosterService.TryGetMediaAssetId(route.ParticipantId, out var assetId) &&
                !string.IsNullOrWhiteSpace(assetId))
            {
                ids.Add(assetId);
            }
        }
        return ids;
    }
}
```

Replace `BuildSceneMediaPlaybackKey(string mediaAssetId, bool isProgramScene, int programTakeVersion)` with:

```csharp
public static string BuildSceneMediaPlaybackKey(string mediaAssetId, bool loop, int goLiveGeneration)
{
    var id = string.IsNullOrWhiteSpace(mediaAssetId) ? "unknown" : mediaAssetId.Trim();
    return loop ? $"media:{id}" : $"media:{id}:live:{Math.Max(0, goLiveGeneration)}";
}
```

Delete `ShouldAdvanceProgramPlaybackKey`. Replace `ResolveSceneRoutePlayback` with:

```csharp
public static SceneRoutePlayback ResolveSceneRoutePlayback(
    string mediaAssetId, bool isProgramScene, bool loop,
    string? selectedMediaAssetId, bool selectedMediaAssetPlaying,
    IReadOnlyList<SourceRoute> programRoutes, int goLiveGeneration)
{
    // A loop is a persistent source: live on every bus, never restarted by a cut.
    // A clip rolls when it goes live and shows its first frame while cued.
    var playing = loop || ShouldPlaySceneMediaRoute(mediaAssetId, isProgramScene, selectedMediaAssetId, selectedMediaAssetPlaying, programRoutes);
    return new SceneRoutePlayback(BuildSceneMediaPlaybackKey(mediaAssetId, loop, goLiveGeneration), playing);
}
```

- [ ] **Step 4: Rewire the host**

- `ITransportHost.cs:78`: replace `void IncrementProgramMediaPlaybackTakeVersion();` with `void RecordProgramMediaGoLive(IReadOnlyList<SourceRoute> previousProgramRoutes);`.
- `TransportCoordinator.cs:145-156`: capture `var previousProgramRoutes = _host.GetResolvedProgramRoutes();` BEFORE the scene ids are swapped (add `IReadOnlyList<SourceRoute> GetResolvedProgramRoutes();` to `ITransportHost` if it is not already there — `StudioViewModel.GetResolvedProgramRoutes()` exists), then replace `_host.IncrementProgramMediaPlaybackTakeVersion();` with `_host.RecordProgramMediaGoLive(previousProgramRoutes);`.
- `StudioViewModel.cs`:
  - `:785` replace `private int _programMediaPlaybackTakeVersion;` with `private readonly MediaGoLiveLedger _mediaGoLive = new();`.
  - Add `public void RecordProgramMediaGoLive(IReadOnlyList<SourceRoute> previousProgramRoutes) => _mediaGoLive.RecordTake(previousProgramRoutes, GetResolvedProgramRoutes());`.
  - `:4068-4073` (scene save updating the Program scene): replace `_programMediaPlaybackTakeVersion++;` with `_mediaGoLive.RecordTake(previousRoutesSnapshot, GetMutableRoutes(scene.Id));` where `previousRoutesSnapshot` is captured before `CopyPreviewRoutesToScene` (the existing `previousProgramMediaRoutes` signature string is not enough — capture the route list).
  - `:6102-6108` (operator Play/Pause): replace the `ShouldAdvanceProgramPlaybackKey` block with `if (SelectedMediaAssetPlaying && MediaRoutePlaybackService.IsMediaAssetRoutedOnProgram(asset.Id, GetResolvedProgramRoutes())) _mediaGoLive.RecordRestart(asset.Id);`.
  - `:8735-8740`: `BuildSceneMediaPlaybackKey(selectedMediaAsset.Id, loop: IsLoopingAsset(selectedMediaAsset), _mediaGoLive.GenerationOf(selectedMediaAsset.Id))`.
  - `:8906-8914` and `:13243-13249`: pass `loop: IsLoopingAsset(mediaAsset)` and `_mediaGoLive.GenerationOf(mediaAsset.Id)` to the new `ResolveSceneRoutePlayback` signature.
  - Add `private static bool IsLoopingAsset(MediaAsset asset) => string.Equals(asset.Kind, "background", StringComparison.OrdinalIgnoreCase);` (extend with `asset.Loop` if the model has it).

- [ ] **Step 5: Run all shell suites**

Run: `dotnet test native-shell/CoreVideoPro.WinUI.Tests/CoreVideoPro.WinUI.Tests.csproj -v q --nologo 2>&1 | tail -2 && dotnet test native-shell/CoreVideoPro.MediaCore.Tests/CoreVideoPro.MediaCore.Tests.csproj -v q --nologo 2>&1 | tail -2`
Expected: both `Failed: 0`. `MediaCoreCommandBuilderTests.cs:41-64` asserts the wire carries `mediaPlaybackKey`/`mediaAssetPlaying` verbatim — it should still pass; if it asserts a `program-take:` literal, change the literal to `media:<id>:live:1`.

- [ ] **Step 6: Commit**

```bash
git add native-shell/CoreVideoPro.WinUI/Services/MediaRoutePlaybackService.cs native-shell/CoreVideoPro.WinUI/ViewModels/Transport/ITransportHost.cs native-shell/CoreVideoPro.WinUI/ViewModels/Transport/TransportCoordinator.cs native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs native-shell/CoreVideoPro.WinUI.Tests/MediaRoutePlaybackServiceTests.cs native-shell/CoreVideoPro.WinUI.Tests/TransportCoordinatorTests.cs
git commit -m "Roll a clip when it goes live instead of on every Take"
```

---

### Task 6: Pixel continuity across a Take (the probe that cannot be fooled by counters)

**Files:**
- Modify: `native/src/core/MediaCore.h` — add `[[nodiscard]] const modules::ProgramFrame& lastProgramFrameForTest() const { return lastProgramFrame_; }` next to `setStillImageDecoderForTest` (test seam, same law: nothing outside `native/tests/` calls it).
- Create: `native/tests/ProgramPixelContinuityTest.cpp`
- Modify: `native/CMakeLists.txt` (add the test)

**Interfaces:**
- Consumes: `ProgramFrame::preview` (the 320x180 BGRA CPU thumbnail `ProgramFramePreview` builds for the stub compositor — fields `previewWidth`, `previewHeight`, `preview` bytes; confirm the exact names in `Interfaces.h` `struct ProgramFrame`).
- Produces: helper `double meanLuma(const modules::ProgramFrame&)`.

- [ ] **Step 1: Write the failing test**

```cpp
// native/tests/ProgramPixelContinuityTest.cpp
#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "modules/StubModules.h"
#include "rpc/Json.h"
#include <gtest/gtest.h>
#include <cmath>

namespace {
// A media source whose "video" is a flat mid-grey; if a bus ever draws the
// placeholder colour (the cold-start slab) or nothing, the luma moves.
class GreyMediaFrameSource final : public corevideo::modules::IMediaFrameSource {
 public:
  std::vector<corevideo::modules::VideoFrame> pollMediaFrames(
      const std::vector<corevideo::modules::CompositorRenderPlanLayer>& layers, int64_t timestampMs) override {
    std::vector<corevideo::modules::VideoFrame> frames;
    for (const auto& layer : layers) {
      if (layer.mediaAssetId.empty()) continue;
      corevideo::modules::VideoFrame f;
      f.participantId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
      f.width = f.pixelWidth = f.naturalWidth = 64; f.height = f.pixelHeight = f.naturalHeight = 36;
      f.pixelStride = 64 * 4; f.timestampMs = timestampMs; f.frameId = ++frameIds_[f.participantId];
      f.pixels = std::make_shared<std::vector<uint8_t>>(64 * 36 * 4, 0x80);
      frames.push_back(std::move(f));
    }
    return frames;
  }
  std::vector<corevideo::modules::AudioFrame> pollMediaAudioFrames(
      const std::vector<corevideo::modules::CompositorRenderPlanLayer>&, int64_t) override { return {}; }
  std::vector<std::string> warnings() const override { return {}; }
 private:
  std::map<std::string, int64_t> frameIds_;
};

double meanLuma(const corevideo::modules::ProgramFrame& frame) {
  // Adjust the field names to ProgramFrame's preview thumbnail members.
  const auto& px = frame.preview;
  if (px.empty()) return -1.0;
  double sum = 0; size_t n = 0;
  for (size_t i = 0; i + 3 < px.size(); i += 4) { sum += 0.114 * px[i] + 0.587 * px[i + 1] + 0.299 * px[i + 2]; ++n; }
  return n ? sum / n : -1.0;
}

corevideo::rpc::Json backgroundScene(const char* sceneId) {
  return corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", sceneId},
      {"background", corevideo::rpc::Json::Object{{"mediaAssetId", "bg"}, {"mediaAssetName", "bg"},
          {"mediaAssetKind", "video"}, {"mediaAssetPath", "C:\\media\\bg.mp4"}, {"playing", true}}},
      {"routes", corevideo::rpc::Json::Array{}}};
}
}  // namespace

TEST(ProgramPixelContinuity, ASharedBackgroundDoesNotFlickerAcrossATake) {
  auto modules = corevideo::modules::createStubModules();
  modules.mediaFrames = std::make_unique<GreyMediaFrameSource>();
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  (void)core.applyCommands(corevideo::rpc::Json::Array{backgroundScene("scene-a")});
  for (int i = 0; i < 10; ++i) core.renderDisplayTick();
  const double before = meanLuma(core.lastProgramFrameForTest());
  ASSERT_GT(before, 0.0);

  (void)core.applyCommands(corevideo::rpc::Json::Array{backgroundScene("scene-b")});
  for (int tick = 0; tick < 10; ++tick) {
    core.renderDisplayTick();
    const double luma = meanLuma(core.lastProgramFrameForTest());
    EXPECT_NEAR(luma, before, 2.0) << "tick " << tick << " after the take";
  }
}
```

- [ ] **Step 2: Add to CMake, build, run**

Run: `npm run build:native-dev && ./native/build-dev/corevideo-native-tests.exe 2>/dev/null | grep -E 'ProgramPixelContinuity|tests passed'`
Expected: PASS if Task 3 is correct (the same source id is served on both sides). If it FAILS with a luma dip on tick 0, that is a real defect in the tick ordering (media polled after the plan is built) — fix in `renderSyntheticTick`, do not widen the tolerance. If `meanLuma` returns -1 on every tick, the stub compositor is not filling `preview`; switch the test to `modules.compositor = std::make_unique<...>` using the `ProgramFramePreview` CPU path (`buildProgramFramePreview` in `modules/ProgramFramePreview.h`) and recompute.

- [ ] **Step 3: Commit**

```bash
git add native/tests/ProgramPixelContinuityTest.cpp native/src/core/MediaCore.h native/CMakeLists.txt
git commit -m "Prove a shared background holds its pixels across a take"
```

---

### Task 7: The live soak judges Takes by the record, not by eye

**Files:**
- Create: `scripts/qa/live-meeting-soak.mjs` — copy from `C:\Users\walla\OneDrive\Documents\ChatGPT\CoreVideo Pro\beta-release\scripts\qa\live-meeting-soak.mjs` (untracked there), then extend.
- Create: `scripts/qa/take-verdict-judge.mjs` (pure) and `scripts/qa/take-verdict-judge.test.mjs`
- Modify: `package.json` — add `"test:take-verdict-judge": "node --test scripts/qa/take-verdict-judge.test.mjs"`.

**Interfaces:**
- Produces: `judgeTakeRecords(records, {expectedTakes})` → `{ ok, cuts, rebuilt, reasons: [{fromSceneId,toSceneId,verdict,restartedSources,backgroundDropped,subscriptionsChurned}] }`; `ok` is false if any record's `verdict === "rebuilt"` or fewer than `expectedTakes` records completed.
- Soak flags: `--takes N` (default 0) alternates `load-scene-graph` between the two scene ids given by `--scene-a`/`--scene-b` every 3 s over the wire the soak already speaks, then reads `sessionState().takeRecords` at the end and runs the judge.

- [ ] **Step 1: Write the failing judge test**

```js
// scripts/qa/take-verdict-judge.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { judgeTakeRecords } from './take-verdict-judge.mjs';

test('every completed take must be a cut', () => {
  const result = judgeTakeRecords([
    { fromSceneId: 'a', toSceneId: 'b', verdict: 'cut', restartedSources: [], backgroundDropped: false, subscriptionsChurned: false },
    { fromSceneId: 'b', toSceneId: 'a', verdict: 'rebuilt', restartedSources: ['background:bg'], backgroundDropped: false, subscriptionsChurned: false },
  ], { expectedTakes: 2 });
  assert.equal(result.ok, false);
  assert.equal(result.cuts, 1);
  assert.equal(result.rebuilt, 1);
  assert.deepEqual(result.reasons[0].restartedSources, ['background:bg']);
});

test('fewer records than takes is a failure, never a quiet pass', () => {
  const result = judgeTakeRecords([{ verdict: 'cut', restartedSources: [] }], { expectedTakes: 3 });
  assert.equal(result.ok, false);
});

test('a no-wall take with nothing shared is not a failure', () => {
  const result = judgeTakeRecords([{ verdict: 'no-wall', restartedSources: [] }], { expectedTakes: 1 });
  assert.equal(result.ok, true);
});
```

- [ ] **Step 2: Run to verify it fails**

Run: `node --test scripts/qa/take-verdict-judge.test.mjs`
Expected: FAIL — module not found.

- [ ] **Step 3: Write the judge**

```js
// scripts/qa/take-verdict-judge.mjs
export function judgeTakeRecords(records, { expectedTakes = 0 } = {}) {
  const list = Array.isArray(records) ? records : [];
  const reasons = [];
  let cuts = 0, rebuilt = 0;
  for (const r of list) {
    if (r.verdict === 'rebuilt') {
      rebuilt++;
      reasons.push({
        fromSceneId: r.fromSceneId, toSceneId: r.toSceneId, verdict: r.verdict,
        restartedSources: r.restartedSources ?? [],
        backgroundDropped: Boolean(r.backgroundDropped),
        subscriptionsChurned: Boolean(r.subscriptionsChurned),
      });
    } else if (r.verdict === 'cut') {
      cuts++;
    }
  }
  const ok = rebuilt === 0 && list.length >= expectedTakes;
  return { ok, cuts, rebuilt, total: list.length, expectedTakes, reasons };
}
```

- [ ] **Step 4: Extend the soak**

In `scripts/qa/live-meeting-soak.mjs`: parse `--takes`, `--scene-a` (default `speaker-slides`), `--scene-b` (default `panel`); after the join settles, if `takes > 0`, every 3000 ms send `{"type":"load-scene-graph","sceneId": <alternating>, ...}` using the same command shape the soak already uses for scene sync (search the file for `load-scene-graph`; if the soak drives scenes through the shell's `/invoke scene.select`, use that instead and note it). At the end read the snapshot's `takeRecords.records`, call `judgeTakeRecords(records, { expectedTakes: takes })`, print one line per reason, include the judge result in the evidence JSON, and make the process exit non-zero when `ok` is false.

- [ ] **Step 5: Run the judge tests, then a 2-minute live soak with 4 takes against the test meeting**

Run: `node --test scripts/qa/take-verdict-judge.test.mjs`
Expected: 3 passed.

Run (app stopped, real meeting): `COREVIDEO_TEST_MEETING_URL="<full pwd link>" node scripts/qa/live-meeting-soak.mjs --minutes 2 --takes 4 --scene-a speaker-slides --scene-b panel`
Expected: `takes: 4 cut, 0 rebuilt` in the summary. If any is `rebuilt`, the reasons name the source — that is a finding for the report, not a reason to change the judge.

- [ ] **Step 6: Commit**

```bash
git add scripts/qa/live-meeting-soak.mjs scripts/qa/take-verdict-judge.mjs scripts/qa/take-verdict-judge.test.mjs package.json
git commit -m "Judge live takes by the take record"
```

---

### Task 8: Documentation

**Files:**
- Modify: `CLAUDE.md` — in the section "A Take is traceable…" add a bullet **PER-SOURCE CONTINUITY IS PART OF THE VERDICT** describing `SourceContinuityLedger`, the `sources[]`/`restartedSources` nodes, and the rule "a peek is not an observation" now extended to "a counter that only counts submits is not continuity". Add a new short section **Media is a persistent source (slice 1, 2026-09-10)**: same source id on both buses, the one `preview:media:` exception (paused clip cue), loop vs clip go-live policy, the `MediaGoLiveLedger`, and the 16-decoder cap warning naming the asset.
- Modify: `docs/superpowers/specs/2026-09-10-persistent-sources-design.md` — under §5 mark slice 1 "shipped <date>, commits <range>" and list anything found by the live soak.

- [ ] **Step 1: Write the docs**
- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md docs/superpowers/specs/2026-09-10-persistent-sources-design.md
git commit -m "Document persistent media sources and per-source take continuity"
```

---

## Self-review

**Spec coverage (slice 1 items, §5):** generation counters in the Take record → Tasks 1-2; pixel probe → Task 6; one player per media asset → Tasks 3-4; go-live policy → Task 5; remove `preview:` namespace and per-Take keys → Tasks 3 and 5; still-image keys unified → Task 3; live soak asserts on the record → Task 7; loud 16-decoder cap → Task 4; tests rewritten not deleted → Tasks 3, 5. Not in slice 1 by design: Tiles animator, transitions, Metal.

**Known gap, stated:** pausing a scene BACKGROUND (the "Pause" label on `SceneBackground.IsPlaying`) still flips `playing` and resets its decoder via `MediaPlaybackTimeline::configure`. That is an operator transport action, not a cut, and is out of slice 1; it is recorded as the first item for slice 3.

**Type consistency:** `SharedSourceContinuity{sourceId, generationBefore, generationAfter, frameIdBefore, frameIdAfter}` is used identically in Task 2 policy, MediaCore wiring, JSON and tests; `MediaGoLiveLedger.RecordTake/RecordRestart/GenerationOf` and `BuildSceneMediaPlaybackKey(id, loop, generation)` are the same in Task 5 tests and implementation; `ITransportHost.RecordProgramMediaGoLive(IReadOnlyList<SourceRoute>)` matches the coordinator call and the test fake.
