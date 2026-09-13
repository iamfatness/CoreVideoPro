# Tiles wall: one animator per wall (slice 2, plan 1 of 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Tiles wall's animation belong to the wall instead of to each bus, so a wall taken to Program **mid-animation** is continuous — and register the wall in `SourceRegistry`, making #448 the first real consumer of the #419 foundations.

**Architecture:** Today `MediaCore` holds two `TilesPlanAnimation` objects, one per bus, and hands settled state across on the take tick. This plan replaces them with one `TilesWallSource` per wall, owned by a `TilesWallSources` map keyed by wall id, and deletes the hand-off. The wall also registers in `SourceRegistry` under a new composed kind whose capture-only fields stay `nullopt`. **No drawing changes** — the plan still emits expanded `tile:` layers, so no compositor is touched.

**Tech Stack:** C++20, MSVC; GoogleTest (`native/tests`); CMake target `corevideo-native-tests`.

**Spec:** `docs/superpowers/specs/2026-09-12-tiles-wall-persistent-source-design.md`

## Global Constraints

- **Windows build:** `cmake --build native/build-dev --target corevideo-native-tests` with `ZOOM_SDK_DIR` set to the staged SDK x64 dir. Run the binary the build just wrote: `native/build-dev/corevideo-native-tests.exe`. **Do not** run `native/build-dev/Release/` — nothing updates it.
- **Run one test:** `native/build-dev/corevideo-native-tests.exe --gtest_filter='Suite.Name'`. **Negative filters do not work** in this runner; to skip, run the full suite.
- **Full suite must stay green:** 931 tests as of `c425e3ca`, plus what this plan adds.
- **Escape scanner must pass:** `python scripts/qa/check-string-escapes.py`. MSVC tolerates invalid escapes that GCC/Clang reject; this scanner is what stops a Linux/macOS CI break.
- **`kTilesStaleFrameMs` (1500 ms) must not change.** It is shared with tile admission; moving it changes wall membership for every source.
- **Never do pixel work under `coreMutex` or on a hot tick.** This plan adds no I/O and no allocation on the render tick.
- **Red before green is verified by reverting**, never by assuming. Three tests in the 2026-09-12 session passed without their fix.

---

### Task 1: `SourceRegistry` admits a composed source

**Files:**
- Modify: `native/src/core/SourceRegistry.h` (the `Kind` enum; the `Source` and `Registration` field types)
- Modify: `native/src/core/SourceRegistry.cpp:49-71` (`validRegistration`, `externalConflict`)
- Test: `native/tests/SourceRegistryComposedTest.cpp` (create)
- Modify: `native/CMakeLists.txt` (register the new test file)

**Interfaces:**
- Consumes: nothing.
- Produces: `SourceRegistry::Kind::Composed`; `Registration` accepted with an empty `externalId` when `kind == Kind::Composed`; `Source::personId`, `Source::externalId`, `Source::availability`, `Source::subscriptionRequested`, `Source::subscriptionObserved` are `std::optional` and left `nullopt` for composed sources.

**Background the implementer needs.** `SourceRegistry` lives on the `#419` branch (`origin/codex/production-realtime-architecture`), not on `main`. This plan assumes the minimal carve described in the spec §6 has already been cherry-picked onto the working branch: `SourceRegistry.{h,cpp}` and their existing tests. If it has not, do that first as a separate commit with no behaviour change.

Two current rules reject a wall. `validRegistration` requires a non-empty `externalId`:

```cpp
// SourceRegistry.cpp:56
!retiredProcessEpochs_.contains(r.processEpoch) && !r.externalId.empty() &&
```

and `externalConflict` treats two sources with the same `kind + processEpoch + externalId` as duplicates, so two walls with empty external ids would collide:

```cpp
// SourceRegistry.cpp:68-70
if (entry.first != r.sourceId.value && source.availability != Availability::Departed &&
    source.kind == r.kind && source.token.processEpoch == r.processEpoch && source.externalId == r.externalId)
  return true;
```

A wall has no SDK handle. It is identified by its `sourceId` alone.

- [ ] **Step 1: Write the failing tests**

Create `native/tests/SourceRegistryComposedTest.cpp`:

```cpp
#include "core/SourceRegistry.h"

#include <gtest/gtest.h>

namespace {
using corevideo::core::SourceRegistry;

SourceRegistry::Registration wallRegistration(const std::string& id) {
  SourceRegistry::Registration registration;
  registration.sourceId = {id};
  registration.kind = SourceRegistry::Kind::Composed;
  registration.displayName = "Gallery";
  // A wall lives and dies with the core process, so the core's epoch is its epoch.
  registration.processEpoch = "core-epoch-1";
  // externalId deliberately left EMPTY: a wall has no SDK handle.
  return registration;
}

// A wall has no SDK handle, and validRegistration rejects an empty externalId
// today, so add() answers Invalid and the wall can never be registered.
TEST(SourceRegistryComposed, AWallIsAdmittedWithoutAnExternalId) {
  SourceRegistry registry("registry-epoch-1");
  const auto mutation = registry.add(wallRegistration("tiles:scene-a"));
  EXPECT_EQ(mutation.result, SourceRegistry::Result::Applied);
  ASSERT_TRUE(mutation.token.has_value());
  EXPECT_EQ(mutation.token->sourceId.value, "tiles:scene-a");
}

// externalConflict matches on kind + processEpoch + externalId, so two walls
// that both have an EMPTY externalId look like duplicates of each other.
TEST(SourceRegistryComposed, TwoWallsWithNoExternalIdDoNotCollide) {
  SourceRegistry registry("registry-epoch-1");
  ASSERT_EQ(registry.add(wallRegistration("tiles:scene-a")).result,
            SourceRegistry::Result::Applied);
  const auto second = registry.add(wallRegistration("tiles:scene-b"));
  EXPECT_EQ(second.result, SourceRegistry::Result::Applied);
}

// The load-bearing honesty test. subscriptionObserved is initialised ENGAGED
// with the value false, and its own comment says nullopt means unknown - so a
// registered wall would otherwise ASSERT "subscription observed = false" into
// ShowPlanGenerator, which consumes this snapshot.
TEST(SourceRegistryComposed, AComposedWallNeverClaimsASubscriptionState) {
  SourceRegistry registry("registry-epoch-1");
  ASSERT_EQ(registry.add(wallRegistration("tiles:scene-a")).result,
            SourceRegistry::Result::Applied);

  const auto snapshot = registry.snapshot();
  ASSERT_NE(snapshot, nullptr);
  ASSERT_EQ(snapshot->sources.size(), 1U);
  const auto& wall = snapshot->sources.front();

  EXPECT_EQ(wall.kind, SourceRegistry::Kind::Composed);
  EXPECT_FALSE(wall.personId.has_value());
  EXPECT_FALSE(wall.externalId.has_value());
  EXPECT_FALSE(wall.availability.has_value());
  EXPECT_FALSE(wall.subscriptionRequested.has_value());
  EXPECT_FALSE(wall.subscriptionObserved.has_value());
}

// A capture source is unchanged: it still carries every field it always did.
TEST(SourceRegistryComposed, ACaptureSourceStillCarriesItsCaptureFields) {
  SourceRegistry registry("registry-epoch-1");
  SourceRegistry::Registration camera;
  camera.sourceId = {"camera-alice"};
  camera.kind = SourceRegistry::Kind::ParticipantVideo;
  camera.displayName = "Alice";
  camera.processEpoch = "zoom-process-1";
  camera.externalId = "alice-sdk-id";
  ASSERT_EQ(registry.add(camera).result, SourceRegistry::Result::Applied);

  const auto& source = registry.snapshot()->sources.front();
  ASSERT_TRUE(source.externalId.has_value());
  EXPECT_EQ(*source.externalId, "alice-sdk-id");
  ASSERT_TRUE(source.availability.has_value());
  EXPECT_EQ(*source.availability, SourceRegistry::Availability::Available);
}
}  // namespace
```

Register it in `native/CMakeLists.txt` beside the other core tests:

```cmake
    tests/SourceRegistryComposedTest.cpp
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter='SourceRegistryComposed.*'`

Expected: compile failure — `Kind::Composed` does not exist, and `personId`/`externalId`/`availability`/`subscriptionRequested`/`subscriptionObserved` have no `.has_value()`. That compile failure IS the red for this task; it names every field the implementation must change.

- [ ] **Step 3: Add the composed kind and make the capture-only fields optional**

In `native/src/core/SourceRegistry.h`:

```cpp
  // Composed: a source the CORE renders rather than captures (the Tiles wall
  // today; lower-thirds and graphics in slice 3). It has no SDK handle, no
  // person, and is never subscribed - so the capture-only fields below stay
  // nullopt for it, and "nullopt" means NOT APPLICABLE, never false.
  enum class Kind { ParticipantVideo, ParticipantShare, Device, Media, Browser, Composed };
```

and in `struct Source`, change the five capture-only members:

```cpp
  struct Source {
    Token token;
    Kind kind = Kind::ParticipantVideo;
    std::optional<PersonId> personId;
    uint64_t personGeneration = 0;
    std::string displayName;
    // nullopt = NOT APPLICABLE to this kind. Never read as false.
    std::optional<std::string> externalId;
    std::optional<Availability> availability;
    std::optional<bool> subscriptionRequested;
    std::optional<bool> subscriptionObserved;
    std::optional<Format> format;
    bool hasPublication = false;
    bool hasPublicationWatermark = false;
    uint64_t publicationSequence = 0;
    int64_t lastPublicationNs = 0;
  };
```

In `native/src/core/SourceRegistry.cpp`, teach both rules about composed sources:

```cpp
bool SourceRegistry::validRegistration(const Registration& r) const {
  const bool composed = r.kind == Kind::Composed;
  const bool knownKind = composed || r.kind == Kind::ParticipantVideo ||
      r.kind == Kind::ParticipantShare || r.kind == Kind::Device ||
      r.kind == Kind::Media || r.kind == Kind::Browser;
  // A composed source is identified by its sourceId alone: it has no SDK handle,
  // so requiring an externalId would reject every wall outright.
  const bool externalIdOk = composed
      ? r.externalId.empty()
      : (!r.externalId.empty() && r.externalId.size() <= 512);
  return knownKind && externalIdOk && ...  // rest of the existing expression unchanged
}
```

```cpp
bool SourceRegistry::externalConflict(const Registration& r) const {
  // A composed source has no externalId to collide on; two walls are distinct
  // whenever their sourceIds differ.
  if (r.kind == Kind::Composed) {
    return false;
  }
  ...  // existing body unchanged
}
```

Then fix the assignment sites in `install()` so a composed registration leaves the five fields `nullopt` and a capture registration sets them exactly as before. Every other read of these fields in `ZoomSourceAuthorityAdapter.cpp`, `ZoomRuntimeAuthorityBridge.cpp` and `ZoomEngineRuntime.cpp` must be updated to dereference the optional — those paths only ever handle capture kinds, so `*source.externalId` is correct there; do **not** introduce a `value_or(false)`, which would recreate the false claim this task exists to remove.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter='SourceRegistryComposed.*'`
Expected: 4 tests PASS.

- [ ] **Step 5: Verify the Zoom authority goldens did not move**

Run: `native/build-dev/corevideo-native-tests.exe` (full suite)
Expected: the pre-existing authority/golden tests PASS **unchanged**. Making a registry field optional must not alter what the Zoom observation path emits. If a golden moves, the change has leaked into the capture path — fix that rather than re-recording the golden.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/SourceRegistry.h native/src/core/SourceRegistry.cpp \
        native/tests/SourceRegistryComposedTest.cpp native/CMakeLists.txt
git commit -m "feat(registry): admit composed sources, and never claim a subscription state for them"
```

---

### Task 2: `TilesWallSource` owns one wall's animation

**Files:**
- Create: `native/src/core/TilesWallSource.h`
- Test: `native/tests/TilesWallSourceTest.cpp` (create)
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: `compositor::TilesPlanAnimation` (existing, `native/src/compositor/TilesPlanAnimation.h`).
- Produces:
  - `core::TilesWallSource` with
    `void advance(modules::CompositorRenderPlan&, const std::string& wallId, bool present, bool enabled, double durationMs, double nowMs)`,
    `void applyLatest(modules::CompositorRenderPlan&, const std::string& wallId) const`,
    `uint64_t generation() const`, `void noteReset()`.
  - `core::TilesWallSources` with `TilesWallSource& forWall(const std::string& wallId)`,
    `const TilesWallSource* find(const std::string& wallId) const`,
    `void releaseAllExcept(const std::vector<std::string>& liveWallIds)`, `std::size_t size() const`.
  - `compositor::TilesPlanAnimation::advance` changes return type from `void` to
    `bool` — **true when it reset the animator**. This is how the generation is
    bumped: no `std::function` callback, so nothing is allocated or indirected on
    the render tick.

**Why a generation.** The spec's proof (§5) requires that a take record cannot read `cut` while the wall's animation reset. `generation` is bumped whenever the animator is reset, so "did this wall restart?" is a number rather than an opinion.

- [ ] **Step 1: Write the failing test**

Create `native/tests/TilesWallSourceTest.cpp`:

```cpp
#include "core/TilesWallSource.h"

#include <gtest/gtest.h>

namespace {
using corevideo::core::TilesWallSources;

// One wall id yields ONE source however many buses ask for it. This is the
// whole point of the slice: the animation belongs to the wall, not the bus.
TEST(TilesWallSources, BothBusesAskingForOneWallGetTheSameSource) {
  TilesWallSources sources;
  auto& fromProgram = sources.forWall("tiles:scene-a");
  auto& fromPreview = sources.forWall("tiles:scene-a");
  EXPECT_EQ(&fromProgram, &fromPreview);
  EXPECT_EQ(sources.size(), 1U);
}

TEST(TilesWallSources, DifferentWallsAreDifferentSources) {
  TilesWallSources sources;
  auto& a = sources.forWall("tiles:scene-a");
  auto& b = sources.forWall("tiles:scene-b");
  EXPECT_NE(&a, &b);
  EXPECT_EQ(sources.size(), 2U);
}

// Lifetime is "referenced by a scene", per the parent spec. A wall no scene
// references is released; a wall still referenced survives the sweep.
TEST(TilesWallSources, AWallNoSceneReferencesIsReleased) {
  TilesWallSources sources;
  sources.forWall("tiles:scene-a");
  sources.forWall("tiles:scene-b");

  sources.releaseAllExcept({"tiles:scene-b"});

  EXPECT_EQ(sources.size(), 1U);
  EXPECT_EQ(&sources.forWall("tiles:scene-b"), &sources.forWall("tiles:scene-b"));
}

// A generation that never moves proves nothing. It must move on a reset...
TEST(TilesWallSources, AResetBumpsTheGeneration) {
  TilesWallSources sources;
  auto& wall = sources.forWall("tiles:scene-a");
  const auto before = wall.generation();
  wall.noteReset();
  EXPECT_GT(wall.generation(), before);
}

// ...and a released-then-recreated wall is a NEW wall, so its generation
// must not silently continue the old one's.
TEST(TilesWallSources, ARecreatedWallDoesNotInheritTheOldGeneration) {
  TilesWallSources sources;
  sources.forWall("tiles:scene-a").noteReset();
  const auto retired = sources.forWall("tiles:scene-a").generation();
  sources.releaseAllExcept({});
  EXPECT_EQ(sources.forWall("tiles:scene-a").generation(), 0U);
  EXPECT_NE(sources.forWall("tiles:scene-a").generation(), retired);
}
}  // namespace
```

Register it in `native/CMakeLists.txt`:

```cmake
    tests/TilesWallSourceTest.cpp
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter='TilesWallSources.*'`
Expected: compile failure — `core/TilesWallSource.h` does not exist.

- [ ] **Step 3: Write the implementation**

Create `native/src/core/TilesWallSource.h`:

```cpp
#pragma once

#include "compositor/TilesPlanAnimation.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace corevideo::core {

// One wall's animation, owned by the WALL rather than by a bus.
//
// Before this existed, MediaCore held programTilesAnimation_ and
// previewTilesAnimation_, and a Take handed settled state from one to the other
// (TilesPlanAnimation::adoptSettledFrom). That hand-off REFUSED a wall whose
// tiles were still flying - "mid-flight state belongs to the bus that is flying
// it" - because with two animators there is no correct answer. So a wall taken
// mid-animation re-animated on the cut, which is #448.
//
// With one animator there is nothing to hand over: both buses sample the same
// object, and a cut changes only which bus is looking at it.
class TilesWallSource final {
 public:
  // Wraps the animation so a reset can never happen without the generation
  // moving. TilesPlanAnimation::advance returns true when it reset the
  // animator; a plain bool return keeps this allocation-free on the render tick
  // (a std::function callback would not be).
  void advance(modules::CompositorRenderPlan& plan, const std::string& wallId, bool present,
               bool enabled, double durationMs, double nowMs) {
    if (animation_.advance(plan, wallId, present, enabled, durationMs, nowMs)) {
      noteReset();
    }
  }

  void applyLatest(modules::CompositorRenderPlan& plan, const std::string& wallId) const {
    animation_.applyLatest(plan, wallId);
  }

  // The take record's proof that nothing restarted (spec section 5), so
  // "did this wall restart?" is a number rather than an opinion.
  [[nodiscard]] uint64_t generation() const { return generation_; }
  void noteReset() { ++generation_; }

 private:
  compositor::TilesPlanAnimation animation_;
  uint64_t generation_ = 0;
};

// Wall id -> source. The id is the Tiles layer id, which the shell already
// emits as "tiles:<sceneId>" (TilesLayerPayloadBuilder.cs), so it is unique per
// scene and identical for the same gallery on either bus.
class TilesWallSources final {
 public:
  [[nodiscard]] TilesWallSource& forWall(const std::string& wallId) {
    return sources_[wallId];
  }

  // Lifetime is "referenced by a scene" (parent spec section 2). Anything no
  // live scene names is released; a recreated wall is a NEW wall and starts at
  // generation 0, never continuing a retired one's count.
  void releaseAllExcept(const std::vector<std::string>& liveWallIds) {
    for (auto it = sources_.begin(); it != sources_.end();) {
      bool live = false;
      for (const auto& id : liveWallIds) {
        if (it->first == id) { live = true; break; }
      }
      it = live ? std::next(it) : sources_.erase(it);
    }
  }

  // Const lookup for readers (the take record). An unknown wall has never
  // animated, so its caller reports generation 0 - which is true, not a guess.
  [[nodiscard]] const TilesWallSource* find(const std::string& wallId) const {
    const auto it = sources_.find(wallId);
    return it == sources_.end() ? nullptr : &it->second;
  }

  [[nodiscard]] std::size_t size() const { return sources_.size(); }

 private:
  std::map<std::string, TilesWallSource> sources_;
};

}  // namespace corevideo::core
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter='TilesWallSources.*'`
Expected: 5 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/TilesWallSource.h native/tests/TilesWallSourceTest.cpp native/CMakeLists.txt
git commit -m "feat(tiles): a wall owns its own animation, keyed by wall id"
```

---

### Task 3: MediaCore uses one animator per wall, and the hand-off is deleted

**Files:**
- Modify: `native/src/core/MediaCore.h:522-523` (replace the two animators with the map)
- Modify: `native/src/core/MediaCore.cpp:3634` (program plan `applyLatest`)
- Modify: `native/src/core/MediaCore.cpp:3670` (multiview PVW `applyLatest`)
- Modify: `native/src/core/MediaCore.cpp:6263-6277` (the take-tick hand-off and both `advance` calls)
- Modify: `native/src/core/MediaCore.cpp:6558` (preview-scene `applyLatest`)
- Modify: `native/src/compositor/TilesPlanAnimation.h` (delete `adoptSettledFrom`)
- Test: `native/tests/TilesRenderPlanTest.cpp` (add the headline test)
- Modify: `native/tests/TilesAnimatorTest.cpp:97-163` (rewrite the hand-off tests)

**Interfaces:**
- Consumes: `core::TilesWallSources` from Task 2.
- Produces: `MediaCore::tilesWallSources_`, and `MediaCore::tilesWallGeneration(const std::string& wallId) const` for the take record.

**This is the task that fixes #448.** Everything before it is scaffolding.

- [ ] **Step 1: Write the failing headline test**

Add to `native/tests/TilesRenderPlanTest.cpp`:

```cpp
// #448. adoptSettledFrom refuses a wall whose tiles are still flying, because
// with two animators mid-flight state has no correct owner. So a wall taken
// MID-ANIMATION lost its animation on Program. With one animator per wall there
// is nothing to hand over and the cut is continuous.
//
// CORRECTION (post-implementation): a reset does NOT replay from alpha 0. The
// animator treats a reset's next non-empty sample() as an ADOPTION, so the wall
// SNAPS TO ITS FINAL STATE - alpha pops to 1, mid-spring rects jump to settled.
// Therefore the EXPECT_GE below is NOT a regression test (a snap satisfies it);
// see the as-built test in TilesRenderPlanTest.cpp, which additionally asserts
// the post-take alpha stays BELOW 0.9 and that any tile already at opacity 1
// keeps its mid-spring rect. Those are the assertions verified red.
//
// This test MUST FAIL before Task 3. Verify that by reverting, not by assuming.
TEST(TilesRenderPlan, AWallTakenMidAnimationIsContinuous) {
  TilesRenderPlanHarness harness;
  harness.cueWallInPreview("scene-a", /*members=*/4);

  // Advance only PART WAY through the entry animation: tiles are still flying.
  harness.advanceMs(harness.animationDurationMs() / 3);
  const auto midFlight = harness.sampledPreviewTiles();
  ASSERT_FALSE(midFlight.empty());
  ASSERT_FALSE(std::all_of(midFlight.begin(), midFlight.end(),
                           [](const auto& tile) { return tile.atRest; }))
      << "precondition: the wall must still be animating for this test to mean anything";

  const auto generationBefore = harness.wallGeneration("tiles:scene-a");
  harness.takeToProgram("scene-a");
  harness.renderOneTick();

  // The wall did not restart...
  EXPECT_EQ(harness.wallGeneration("tiles:scene-a"), generationBefore);

  // ...and its tiles continued from where they were, rather than SNAPPING
  // FORWARD to the settled state (see the correction above - this direction is
  // the opposite of what the first draft of this plan assumed).
  const auto afterTake = harness.sampledProgramTiles();
  ASSERT_EQ(afterTake.size(), midFlight.size());
  for (size_t i = 0; i < afterTake.size(); ++i) {
    EXPECT_GE(afterTake[i].alpha, midFlight[i].alpha)
        << "tile " << i << " lost its in-flight animation instead of continuing";
  }
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter='TilesRenderPlan.AWallTakenMidAnimationIsContinuous'`
Expected: FAIL — the program animator resets on the key it has never held, so alpha restarts near 0 and the generation moves.

If the harness helpers (`cueWallInPreview`, `advanceMs`, `sampledPreviewTiles`, `takeToProgram`, `wallGeneration`) do not exist, add them to the existing harness in that file first, in their own commit, with no behaviour change.

- [ ] **Step 3: Replace the two animators with the map**

In `native/src/core/MediaCore.h`, delete:

```cpp
  compositor::TilesPlanAnimation programTilesAnimation_;
  compositor::TilesPlanAnimation previewTilesAnimation_;
```

and add:

```cpp
  // One animation per WALL, not per bus (#448). See core/TilesWallSource.h for
  // why the per-bus pair and its hand-off were wrong.
  core::TilesWallSources tilesWallSources_;
```

In `native/src/core/MediaCore.cpp`, replace the take-tick block at 6263-6277 with:

```cpp
  const std::string programWallId = tilesLayer_.layerId;
  const std::string previewWallId = previewTilesLayer_.layerId;
  if (tilesLayer_.present && tilesLayer_.style.animateLayout) {
    tilesWallSources_.forWall(programWallId).advance(
        renderPlan, programWallId, tilesLayer_.present, tilesLayer_.style.animateLayout,
        tilesLayer_.style.animationDurationMs, animationNowMs);
  }
  // Advance Preview on the SAME render clock even when its wall is empty, so
  // snapshot/prefetch builds cannot change entry/departure animation state.
  // When both buses show the SAME wall this is the same object, advanced once
  // above - guard against double-advancing it on one tick.
  if (previewTilesLayer_.present && previewTilesLayer_.style.animateLayout &&
      hasPreviewScene() && previewWallId != programWallId) {
    auto previewAnimationPlan = buildPreviewCompositorRenderPlan(videoFrames);
    tilesWallSources_.forWall(previewWallId).advance(
        previewAnimationPlan, previewWallId, true, previewTilesLayer_.style.animateLayout,
        previewTilesLayer_.style.animationDurationMs, animationNowMs);
  }
  // Lifetime: release any wall no live scene still names (parent spec section 2).
  std::vector<std::string> liveWallIds;
  if (tilesLayer_.present) liveWallIds.push_back(programWallId);
  if (previewTilesLayer_.present) liveWallIds.push_back(previewWallId);
  tilesWallSources_.releaseAllExcept(liveWallIds);
```

Update the three `applyLatest` sites to read from the map, passing the wall id
(no longer `sceneId + ":" + layerId`):

- `MediaCore.cpp:3634` → `tilesWallSources_.forWall(tilesLayer_.layerId).applyLatest(programPlan, tilesLayer_.layerId);`
- `MediaCore.cpp:3670` and `:6558` → the same shape with `previewTilesLayer_.layerId`.

Add the generation accessor for the take record:

```cpp
uint64_t MediaCore::tilesWallGeneration(const std::string& wallId) const {
  return tilesWallSources_.forWall(wallId).generation();
}
```

Use the const `find()` added in Task 2 rather than making `forWall` const —
`forWall` inserts, and a read must never create a wall:

```cpp
uint64_t MediaCore::tilesWallGeneration(const std::string& wallId) const {
  const auto* wall = tilesWallSources_.find(wallId);
  return wall ? wall->generation() : 0;
}
```

- [ ] **Step 4: Delete the hand-off**

In `native/src/compositor/TilesPlanAnimation.h`, delete `adoptSettledFrom` entirely, along with its comment block. Nothing may call it: with one animator per wall there is no second animator to adopt from, and leaving it would invite a future caller to reintroduce per-bus state.

Change `advance` to REPORT whether it reset, so the generation cannot move
without the animator moving (and vice versa). Return type `void` -> `bool`:

```cpp
  // Returns TRUE when this call reset the animator. TilesWallSource turns that
  // into a generation bump; a bool return keeps the render tick allocation-free
  // where a std::function callback would not.
  bool advance(modules::CompositorRenderPlan& plan, const std::string& wallKey,
      bool present, bool enabled, double durationMs, double nowMs) {
    if (!present || !enabled) { const bool had = !key_.empty(); reset(); return had; }
    bool didReset = false;
    if (key_ != wallKey) { animator_.reset(); key_ = wallKey; sampled_.clear(); didReset = true; }
    ...  // body below unchanged, including the all-stale guard
    return didReset;
  }
```

**The all-stale guard stays exactly as it is** — `if (targets.empty() && !sampled_.empty()) return didReset;`. With one shared animator a wipe would now
affect every bus at once, so this guard matters more than before, not less.

- [ ] **Step 5: Run the headline test to verify it passes**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter='TilesRenderPlan.AWallTakenMidAnimationIsContinuous'`
Expected: PASS.

- [ ] **Step 6: Verify the red was real**

```bash
git stash push -u -m "448-verify-red"
# confirm the test FAILS on the pre-change tree, then restore:
git stash list --format='%H %gs'   # capture YOUR entry's SHA
git stash apply <sha>
```

Expected: FAIL without the change, PASS with it. **A test that passes both ways proves nothing** — this has happened three times in this codebase.

- [ ] **Step 7: Rewrite the tests that pinned the hand-off**

`native/tests/TilesAnimatorTest.cpp:97-163` contains three hand-off tests
(`AWallSettledInPreviewIsAlreadySettledOnItsFirstProgramFrame`,
`AWallTakenWhileItsFramesLapseIsStillCutToNotRedrawn`,
`AWallThatWasNeverInPreviewIsHandedNothing`). **Rewrite, do not delete** — they
pin real behaviour that must survive:

- the first becomes "a wall settled on one bus is settled on the other, because it is the same object";
- the second keeps its all-stale-beat assertion, which is now about the shared animator's retained tiles;
- the third becomes "a wall that was never cued starts cold", asserting generation 0 and a full entry animation.

- [ ] **Step 8: Run the full suite and the escape scanner**

Run: `native/build-dev/corevideo-native-tests.exe`
Expected: all green, 931 + the tests added by this plan.

Run: `python scripts/qa/check-string-escapes.py`
Expected: `no invalid escape sequences found`.

- [ ] **Step 9: Commit**

```bash
git add native/src/core/MediaCore.h native/src/core/MediaCore.cpp \
        native/src/compositor/TilesPlanAnimation.h \
        native/tests/TilesRenderPlanTest.cpp native/tests/TilesAnimatorTest.cpp
git commit -m "fix(tiles): one animator per wall, so a wall taken mid-animation is continuous

Closes #448"
```

---

### Task 4: The wall registers as a composed source, and the take record proves continuity

**Files:**
- Modify: `native/src/core/MediaCore.cpp` (register/release around the `releaseAllExcept` call added in Task 3)
- Modify: `native/src/core/TakeRecordPolicy.h` (carry the wall generation)
- Test: `native/tests/TakeRecordTest.cpp`

**Interfaces:**
- Consumes: `SourceRegistry::Kind::Composed` (Task 1); `TilesWallSources` (Task 2); `MediaCore::tilesWallGeneration` (Task 3).
- Produces: a take record whose `sources[]` includes `tiles:<sceneId>` with `generationBefore`/`generationAfter`.

- [ ] **Step 1: Write the failing test**

Add to `native/tests/TakeRecordTest.cpp`:

```cpp
// The spec's proof (section 5): a take record cannot read "cut" while the wall
// restarted. Before this task the wall was invisible to the ledger entirely, so
// a re-animating wall could be recorded as a clean cut.
TEST(TakeRecord, AWallThatRestartedCannotBeRecordedAsACleanCut) {
  TakeRecordHarness harness;
  harness.cueWallInPreview("scene-a");
  harness.takeToProgram("scene-a");
  harness.forceWallReset("tiles:scene-a");   // a cold start, however caused

  const auto record = harness.completeTakeRecord();

  const auto wall = record.sourceNamed("tiles:scene-a");
  ASSERT_TRUE(wall.has_value());
  EXPECT_NE(wall->generationBefore, wall->generationAfter);
  EXPECT_TRUE(wall->restarted);
  EXPECT_EQ(record.verdict, "rebuilt");
}

TEST(TakeRecord, AContinuousWallIsRecordedAsACut) {
  TakeRecordHarness harness;
  harness.cueWallInPreview("scene-a");
  harness.takeToProgram("scene-a");

  const auto record = harness.completeTakeRecord();

  const auto wall = record.sourceNamed("tiles:scene-a");
  ASSERT_TRUE(wall.has_value());
  EXPECT_EQ(wall->generationBefore, wall->generationAfter);
  EXPECT_FALSE(wall->restarted);
  EXPECT_EQ(record.verdict, "cut");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter='TakeRecord.AWallThatRestartedCannotBeRecordedAsACleanCut:TakeRecord.AContinuousWallIsRecordedAsACut'`
Expected: FAIL — `record.sourceNamed("tiles:scene-a")` is empty; the wall is not in the ledger.

- [ ] **Step 3: Register the wall and feed its generation to the take record**

Where Task 3 added `releaseAllExcept`, also register and release in the registry:

```cpp
  // The wall is a SOURCE (parent spec section 2), so it registers like one.
  // A composed registration carries no externalId and never claims a
  // subscription state - see SourceRegistry::Kind::Composed.
  for (const auto& wallId : liveWallIds) {
    if (registeredWallIds_.insert(wallId).second) {
      SourceRegistry::Registration registration;
      registration.sourceId = {wallId};
      registration.kind = SourceRegistry::Kind::Composed;
      registration.displayName = "Tiles wall";
      registration.processEpoch = coreProcessEpoch_;
      sourceRegistry_.add(std::move(registration));
    }
  }
```

and remove entries from `registeredWallIds_` (and the registry) for walls the
sweep released, so a recreated wall registers afresh.

Then include the wall in the take record's source set, using
`tilesWallGeneration(wallId)` for `generationBefore`/`generationAfter`, exactly as
`SourceContinuityLedger` already does for frame sources.

- [ ] **Step 4: Run to verify it passes**

Run: the same filter as Step 2.
Expected: both PASS.

- [ ] **Step 5: Run the full suite and the escape scanner**

Run: `native/build-dev/corevideo-native-tests.exe` then `python scripts/qa/check-string-escapes.py`
Expected: all green; `no invalid escape sequences found`.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/MediaCore.cpp native/src/core/MediaCore.h \
        native/src/core/TakeRecordPolicy.h native/tests/TakeRecordTest.cpp
git commit -m "feat(tiles): register the wall as a composed source and prove its continuity in the take record"
```

---

## What this plan deliberately does NOT do

These are plan 2, and each is listed in the spec:

- The wall's own offscreen texture, and buses sampling it.
- Collapsing the plan to a single `tiles-wall` layer (and the Metal / CPU-preview parity that forces).
- The multiview PVW cell sampling the Preview bus texture.
- Adopting `AtomicTakeCoordinator` for the cut.
- The **pixel-continuity probe** (`ProgramPixelContinuityTest`, the luma-comb
  technique). It belongs with plan 2 because it proves what is DRAWN, and plan 1
  changes no drawing. Plan 1's proof is the generation counter, which is the
  spec's first proof layer.
- The fault-injection and live-soak gates, and the integrated-GPU number that
  **cannot be produced** here (#425).

**Invariants 1, 2, 6 and 7 of the spec are untouched by this plan** (the wall's
background is still emitted above the admission gate; `wall.present` alone still
gates; the `tiles` snapshot node is unchanged; borders stay multiview-only),
because layer emission is not modified. Invariants 3 and 4 — the asymmetric
stale rules and the all-stale guard — ARE in scope here and are pinned by the
rewritten tests in Task 3, Step 7.

**Nothing in this plan touches how a frame is drawn**, which is why it can land while `beta-2026-09-12-c425e3c` is still being live-checked. Plan 2 changes the render path and should wait for that beta to be shaken out.
