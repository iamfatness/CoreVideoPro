# Source Bus — Slice 0 (contract + bus + test-pattern source) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the `ISource` ingest contract and a core-owned `SourceBus` with a test-pattern source flowing real pixels into a captured `ProgramFrame`, alongside the three existing pipes, with per-source `framesIngested`/`droppedFrames` in the snapshot — the #535 foundation, at zero risk to live kinds.

**Architecture:** One header-only contract (`ISource` producing a `SourceTick`) and a header-only `SourceBus` that MediaCore owns. The bus is ingested every render tick (empty in production); a test seam registers the test-pattern source so unit tests prove the bus composites and counts. No live kind (Zoom/capture/media) moves in this slice — those are slices 1-3.

**Tech Stack:** C++17, MediaCore (`native/`), GoogleTest (`corevideo-native-tests`), CMake multi-config (VS 2026), the existing CPU-compositor preview path (`ProgramFramePreview`).

**Spec:** `docs/superpowers/specs/2026-09-18-source-bus-design.md`

## Global Constraints

- **Stub build stays green.** New source/bus code compiles and passes in the `COREVIDEO_STUB` build (`scripts/test-native.ps1`), not only the Windows dev core.
- **New production code is HEADER-ONLY** (`SourceBus.h`, `TestPattern.h`). The only CMake edit is registering the new test file — avoids adding a `.cpp` to both the stub and dev target lists.
- **The test-pattern source is NEVER on-air in production.** It is registered only through a test seam (`MediaCore::addSourceForTest`), exactly like `setStillImageDecoderForTest`. Production ingests an empty bus.
- **Zero-copy under `coreMutex`.** `SourceBus::ingest` copies `shared_ptr` refs only — no pixel work, no allocation on the hot path (the standing law; violating it reintroduces a fixed regression).
- **`source_id` IS `VideoFrame::participantId`** — the existing `scheme:id` key. The bus does not introduce a second key space.
- **A source node is present even at zero** (the multiviewer-node rule): `sources[]` is emitted whenever the bus exists, empty in production, never absent in the case worth detecting.
- **Build/run the dev core with `--config Release`** and check the exe size before believing any result (Debug and Release write the same path; a Debug core inflates every number — `CLAUDE.md` "Build & run").

---

### Task 1: The source contract + a shared SMPTE generator + the test-pattern source

**Files:**
- Create: `native/src/core/SourceBus.h` (contract types only in this task; the `SourceBus` class is Task 2)
- Create: `native/src/core/TestPattern.h` (shared SMPTE-bar generator)
- Create: `native/src/core/TestPatternSource.h` (`ISource` impl)
- Create: `native/tests/SourceBusTest.cpp`
- Modify: `native/CMakeLists.txt:602-632` (register the test file)
- Modify: `native/src/modules/StubModules.cpp:752-773` (call the shared generator instead of its private copy — keep one source of truth)

**Interfaces:**
- Consumes: `corevideo::modules::VideoFrame` / `AudioFrame` (`native/src/modules/Interfaces.h:14,110`).
- Produces (later tasks rely on these EXACT names/types):
  - `enum class corevideo::core::SourceHealth { Producing, Warming, Stalled, Failed, Idle };`
  - `struct corevideo::core::SourceDescriptor { std::string sourceId; std::string kind; int width=0, height=0; std::optional<int> fpsNumerator, fpsDenominator; std::string pixelFormat; bool hasVideo=false, hasAudio=false, composed=false; };`
  - `struct corevideo::core::SourceIngestCounters { uint64_t framesIngested=0; uint64_t droppedFrames=0; int64_t lastFrameId=0; int64_t lastNewFrameNs=0; };`
  - `struct corevideo::core::SourceTick { std::vector<modules::VideoFrame> video; std::vector<modules::AudioFrame> audio; SourceHealth health=SourceHealth::Idle; int64_t clockOffset100ns=0; };`
  - `class corevideo::core::ISource { public: virtual ~ISource()=default; virtual const SourceDescriptor& descriptor() const = 0; virtual SourceTick poll(int64_t programTime100ns) = 0; virtual SourceIngestCounters counters() const = 0; };`
  - `std::shared_ptr<const std::vector<uint8_t>> corevideo::core::makeSmpteBarsBgra(int width, int height);` (7-bar SMPTE, center bar green, alpha 255 — byte-identical to `StubModules.cpp:756-773`)
  - `class corevideo::core::TestPatternSource : public ISource` — ctor `TestPatternSource(std::string sourceId="test:pattern", int width=640, int height=360)`; each `poll()` returns one `VideoFrame` (`participantId==sourceId`, `pixels` = the shared bars, `pixelWidth/Height`, `pixelStride=width*4`, `frameId` incrementing from 1, `timestampMs = programTime100ns/10000`), `health=Producing`, `clockOffset100ns=0`; `counters()` reflects frames produced.

- [ ] **Step 1: Write the failing test**

```cpp
// native/tests/SourceBusTest.cpp
#include "core/TestPatternSource.h"
#include "compositor/CompositorLayout.h"

#include <gtest/gtest.h>

using corevideo::core::TestPatternSource;

TEST(SourceContract, TestPatternSourceProducesSmpteBars) {
  TestPatternSource src("test:pattern", 640, 360);
  EXPECT_EQ(src.descriptor().sourceId, "test:pattern");
  EXPECT_EQ(src.descriptor().width, 640);
  EXPECT_TRUE(src.descriptor().hasVideo);

  const auto tick = src.poll(/*programTime100ns=*/10'000'000);
  ASSERT_EQ(tick.video.size(), 1u);
  const auto& frame = tick.video.front();
  EXPECT_EQ(frame.participantId, "test:pattern");
  EXPECT_TRUE(frame.hasPixels());
  EXPECT_EQ(frame.pixelWidth, 640);
  EXPECT_EQ(frame.pixelHeight, 360);
  EXPECT_EQ(frame.pixelStride, 640 * 4);
  EXPECT_GE(frame.frameId, 1);

  // Center bar is green (BGRA) — real pattern, not a slate.
  const auto& px = *frame.pixels;
  const size_t row = static_cast<size_t>(360 / 2) * static_cast<size_t>(640 * 4);
  const size_t center = row + static_cast<size_t>(640 / 2) * 4;
  EXPECT_EQ(px[center + 0], 0);    // B
  EXPECT_EQ(px[center + 1], 255);  // G
  EXPECT_EQ(px[center + 2], 0);    // R

  // frameId advances each poll; counters follow.
  const auto tick2 = src.poll(20'000'000);
  EXPECT_GT(tick2.video.front().frameId, frame.frameId);
  EXPECT_EQ(src.counters().framesIngested, 2u);
}
```

- [ ] **Step 2: Register the test in CMake and run it to verify it fails to build**

Add `tests/SourceBusTest.cpp` to the `corevideo-native-tests` source list at `native/CMakeLists.txt` (alongside `tests/CaptureIngestTest.cpp`, line ~627).

Run (dev core): `cmake --build native/build-dev --config Release --target corevideo-native-tests`
Expected: FAIL — `core/TestPatternSource.h` not found.

- [ ] **Step 3: Write `native/src/core/TestPattern.h`**

```cpp
#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace corevideo::core {
// Deterministic 7-bar SMPTE-style BGRA test pattern, immutable/shared so each
// frame is a cheap shared_ptr copy. Center bar (index 3) is green. Byte-identical
// to the former StubModules private copy — the one source of truth now.
inline std::shared_ptr<const std::vector<uint8_t>> makeSmpteBarsBgra(int width, int height) {
  static const uint8_t bars[7][3] = {
      {255, 255, 255}, {0, 255, 255}, {255, 255, 0}, {0, 255, 0},
      {255, 0, 255},   {0, 0, 255},   {255, 0, 0},
  };
  auto pixels = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int bar = (x * 7) / (width > 0 ? width : 1);
      const int b = bar < 7 ? bar : 6;
      const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
      (*pixels)[offset + 0] = bars[b][0];
      (*pixels)[offset + 1] = bars[b][1];
      (*pixels)[offset + 2] = bars[b][2];
      (*pixels)[offset + 3] = 255;
    }
  }
  return pixels;
}
}  // namespace corevideo::core
```

- [ ] **Step 4: Write `native/src/core/SourceBus.h` (contract types only)**

```cpp
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "modules/Interfaces.h"

namespace corevideo::core {

enum class SourceHealth { Producing, Warming, Stalled, Failed, Idle };

struct SourceDescriptor {
  std::string sourceId;   // canonical scheme:id == VideoFrame::participantId
  std::string kind;       // "zoom" | "capture" | "media" | "composed" | "test"
  int width = 0;
  int height = 0;
  std::optional<int> fpsNumerator, fpsDenominator;
  std::string pixelFormat;  // "bgra" | "i420"
  bool hasVideo = false;
  bool hasAudio = false;
  bool composed = false;
};

struct SourceIngestCounters {
  uint64_t framesIngested = 0;   // a poll() that returned a NEW frameId
  uint64_t droppedFrames = 0;    // pool refusal or source-reported drop
  int64_t lastFrameId = 0;
  int64_t lastNewFrameNs = 0;    // caller monotonic clock; never UTC
};

struct SourceTick {
  std::vector<modules::VideoFrame> video;  // 0..1 normal; N for share+cam
  std::vector<modules::AudioFrame> audio;  // 0..1 program-rate PCM chunk
  SourceHealth health = SourceHealth::Idle;
  int64_t clockOffset100ns = 0;            // source clock - program epoch
};

class ISource {
 public:
  virtual ~ISource() = default;
  virtual const SourceDescriptor& descriptor() const = 0;
  virtual SourceTick poll(int64_t programTime100ns) = 0;
  virtual SourceIngestCounters counters() const = 0;
};

}  // namespace corevideo::core
```

- [ ] **Step 5: Write `native/src/core/TestPatternSource.h`**

```cpp
#pragma once
#include "core/SourceBus.h"
#include "core/TestPattern.h"

namespace corevideo::core {

class TestPatternSource final : public ISource {
 public:
  explicit TestPatternSource(std::string sourceId = "test:pattern",
                             int width = 640, int height = 360)
      : bars_(makeSmpteBarsBgra(width, height)) {
    descriptor_.sourceId = std::move(sourceId);
    descriptor_.kind = "test";
    descriptor_.width = width;
    descriptor_.height = height;
    descriptor_.fpsNumerator = 60;
    descriptor_.fpsDenominator = 1;
    descriptor_.pixelFormat = "bgra";
    descriptor_.hasVideo = true;
  }

  const SourceDescriptor& descriptor() const override { return descriptor_; }

  SourceTick poll(int64_t programTime100ns) override {
    modules::VideoFrame frame;
    frame.participantId = descriptor_.sourceId;
    frame.width = frame.pixelWidth = frame.naturalWidth = descriptor_.width;
    frame.height = frame.pixelHeight = frame.naturalHeight = descriptor_.height;
    frame.pixelStride = descriptor_.width * 4;
    frame.pixels = bars_;
    frame.frameId = ++frameId_;
    frame.timestampMs = programTime100ns / 10000;
    counters_.framesIngested = static_cast<uint64_t>(frameId_);
    counters_.lastFrameId = frameId_;
    SourceTick tick;
    tick.video.push_back(std::move(frame));
    tick.health = SourceHealth::Producing;
    return tick;
  }

  SourceIngestCounters counters() const override { return counters_; }

 private:
  SourceDescriptor descriptor_;
  std::shared_ptr<const std::vector<uint8_t>> bars_;
  int64_t frameId_ = 0;
  SourceIngestCounters counters_;
};

}  // namespace corevideo::core
```

- [ ] **Step 6: Point `StubModules.cpp` at the shared generator**

In `native/src/modules/StubModules.cpp`, include `"core/TestPattern.h"` and replace the private `makeTestPatternBgra` body (`:752-773`) with a call to `corevideo::core::makeSmpteBarsBgra(width, height)` (or delete the local function and its callsite at `:847` in favor of the shared one). The emitted pixels must be byte-identical — `CaptureIngest.ConnectedDeviceEmitsTestPatternPixels` still passes unchanged.

- [ ] **Step 7: Build and run to verify PASS**

Run: `cmake --build native/build-dev --config Release --target corevideo-native-tests` then `native/build-dev/corevideo-native-tests.exe --gtest_filter=SourceContract.*:CaptureIngest.*`
Expected: PASS (new contract test + the unchanged capture-ingest tests).

- [ ] **Step 8: Commit**

```bash
git add native/src/core/SourceBus.h native/src/core/TestPattern.h native/src/core/TestPatternSource.h native/tests/SourceBusTest.cpp native/CMakeLists.txt native/src/modules/StubModules.cpp
git commit -m "feat(#535): source contract + test-pattern source (slice 0)"
```

---

### Task 2: The SourceBus aggregator + counters + health

**Files:**
- Modify: `native/src/core/SourceBus.h` (add the `SourceBus` class below the contract types)
- Test: `native/tests/SourceBusTest.cpp` (add cases)

**Interfaces:**
- Consumes: `ISource`, `SourceTick`, `SourceDescriptor`, `SourceIngestCounters` (Task 1).
- Produces (Task 3/4 rely on these EXACT names/types):
  - `class corevideo::core::SourceBus` with:
    - `void add(std::shared_ptr<ISource> source);` (keyed by `descriptor().sourceId`; replaces an existing id)
    - `void remove(const std::string& sourceId);`
    - `struct IngestResult { std::vector<modules::VideoFrame> video; std::vector<modules::AudioFrame> audio; };`
    - `IngestResult ingest(int64_t programTime100ns, int64_t nowNs);` — polls every source once, appends frames, bumps `framesIngested` only when a source returns a NEW `frameId`, sets `lastNewFrameNs=nowNs` then.
    - `struct SourceStatus { SourceDescriptor descriptor; SourceIngestCounters counters; SourceHealth health; };`
    - `std::vector<SourceStatus> snapshot(int64_t nowNs) const;` — health derived: `Producing` if `nowNs - lastNewFrameNs <= kStaleAfterNs`, else `Stalled` (a source that never produced stays `Warming`; `kStaleAfterNs = 200'000'000` = 200ms, the take-record jitter window).
    - `bool empty() const;`

- [ ] **Step 1: Write the failing tests**

```cpp
// append to native/tests/SourceBusTest.cpp
#include "core/SourceBus.h"

using corevideo::core::SourceBus;
using corevideo::core::SourceHealth;

TEST(SourceBus, IngestMergesFramesAndCountsNewFrameIds) {
  SourceBus bus;
  bus.add(std::make_shared<TestPatternSource>("test:pattern"));
  ASSERT_FALSE(bus.empty());

  const auto r1 = bus.ingest(/*programTime100ns=*/10'000'000, /*nowNs=*/1'000);
  ASSERT_EQ(r1.video.size(), 1u);
  EXPECT_EQ(r1.video.front().participantId, "test:pattern");

  const auto r2 = bus.ingest(20'000'000, /*nowNs=*/2'000);
  ASSERT_EQ(r2.video.size(), 1u);

  const auto snap = bus.snapshot(/*nowNs=*/2'000);
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap.front().counters.framesIngested, 2u);  // two NEW frameIds
  EXPECT_EQ(snap.front().health, SourceHealth::Producing);
}

TEST(SourceBus, AStaleSourceDecaysToStalled) {
  SourceBus bus;
  bus.add(std::make_shared<TestPatternSource>("test:pattern"));
  bus.ingest(10'000'000, /*nowNs=*/1'000);
  // No further ingest; read far in the future (> 200ms).
  const auto snap = bus.snapshot(/*nowNs=*/1'000 + 300'000'000);
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap.front().health, SourceHealth::Stalled);
}

TEST(SourceBus, RemoveDropsTheSource) {
  SourceBus bus;
  bus.add(std::make_shared<TestPatternSource>("test:pattern"));
  bus.remove("test:pattern");
  EXPECT_TRUE(bus.empty());
  EXPECT_TRUE(bus.ingest(10'000'000, 1'000).video.empty());
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build native/build-dev --config Release --target corevideo-native-tests`
Expected: FAIL — `SourceBus` has no `add`/`ingest`/`snapshot`.

- [ ] **Step 3: Implement the `SourceBus` class in `SourceBus.h`**

```cpp
// add inside namespace corevideo::core, after class ISource
class SourceBus {
 public:
  struct IngestResult {
    std::vector<modules::VideoFrame> video;
    std::vector<modules::AudioFrame> audio;
  };
  struct SourceStatus {
    SourceDescriptor descriptor;
    SourceIngestCounters counters;
    SourceHealth health = SourceHealth::Idle;
  };

  void add(std::shared_ptr<ISource> source) {
    if (!source) return;
    const std::string id = source->descriptor().sourceId;
    entries_[id] = Entry{std::move(source), {}, 0, false};
  }
  void remove(const std::string& sourceId) { entries_.erase(sourceId); }
  bool empty() const { return entries_.empty(); }

  IngestResult ingest(int64_t programTime100ns, int64_t nowNs) {
    IngestResult out;
    for (auto& [id, e] : entries_) {
      SourceTick tick = e.source->poll(programTime100ns);
      for (auto& v : tick.video) {
        const bool isNew = !e.everProduced || v.frameId != e.counters.lastFrameId;
        if (isNew) {
          e.counters.framesIngested += 1;
          e.counters.lastFrameId = v.frameId;
          e.counters.lastNewFrameNs = nowNs;
          e.everProduced = true;
        }
        out.video.push_back(std::move(v));
      }
      for (auto& a : tick.audio) out.audio.push_back(std::move(a));
    }
    return out;
  }

  std::vector<SourceStatus> snapshot(int64_t nowNs) const {
    std::vector<SourceStatus> out;
    out.reserve(entries_.size());
    for (const auto& [id, e] : entries_) {
      SourceHealth h;
      if (!e.everProduced) {
        h = SourceHealth::Warming;
      } else if (nowNs - e.counters.lastNewFrameNs <= kStaleAfterNs) {
        h = SourceHealth::Producing;
      } else {
        h = SourceHealth::Stalled;
      }
      out.push_back({e.source->descriptor(), e.counters, h});
    }
    return out;
  }

 private:
  static constexpr int64_t kStaleAfterNs = 200'000'000;  // 200ms
  struct Entry {
    std::shared_ptr<ISource> source;
    SourceIngestCounters counters;
    int64_t reserved = 0;
    bool everProduced = false;
  };
  std::map<std::string, Entry> entries_;  // stable id order for the snapshot
};
```

Add `#include <map>` and `#include <memory>` to `SourceBus.h`.

- [ ] **Step 4: Run to verify PASS**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter=SourceBus.*:SourceContract.*`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/SourceBus.h native/tests/SourceBusTest.cpp
git commit -m "feat(#535): SourceBus ingest + per-source counters + health (slice 0)"
```

---

### Task 3: MediaCore owns the bus, ingests it each tick, and composites a bus source (F1 gate)

**Files:**
- Modify: `native/src/core/MediaCore.h` (add member + test seam near `setStillImageDecoderForTest:241`)
- Modify: `native/src/core/MediaCore.cpp` (construct the bus; ingest+merge in `renderSyntheticTick` near the capture merge, `:6043`)
- Test: `native/tests/SourceBusTest.cpp` or `native/tests/MediaCoreCommandTest.cpp` (drive a tick through MediaCore)

**Interfaces:**
- Consumes: `SourceBus`, `ISource`, `TestPatternSource` (Tasks 1-2).
- Produces (Task 4 relies on these):
  - `MediaCore` private member `std::unique_ptr<core::SourceBus> sourceBus_;` (constructed in the MediaCore ctor).
  - Public test seam `void MediaCore::addSourceForTest(std::shared_ptr<core::ISource> source);` (forwards to `sourceBus_->add`, under `coreMutex`).
  - `renderSyntheticTick` appends `sourceBus_->ingest(...)` video/audio into the merged `videoFrames`/audio, so a registered bus source composites into `ProgramFrame` exactly like a capture frame.

- [ ] **Step 1: Write the failing test**

```cpp
// append to native/tests/SourceBusTest.cpp
#include "core/MediaCore.h"
// (use the existing MediaCore test harness includes as CaptureIngestTest/MediaCoreCommandTest do)

TEST(SourceBusMediaCore, ATestPatternBusSourceCompositesIntoProgram) {
  corevideo::core::MediaCore core(corevideo::modules::createStubModules());
  core.addSourceForTest(std::make_shared<corevideo::core::TestPatternSource>("test:pattern"));

  // Route the bus source full-frame to program, then drive one render tick and
  // read the captured ProgramFrame preview (the F1 CPU-compositor gate).
  // (Follow the scene-load + renderSyntheticTick + lastProgramFrame path used by
  //  MediaCoreCommandTest; assert the center pixel is the green SMPTE bar and NOT
  //  colorFromParticipantId("test:pattern").)
  // ... drive tick, capture preview ...
  // EXPECT green center, EXPECT_NE slate.
}
```

Note to implementer: model the tick-drive + program-frame capture on the closest existing case in `native/tests/MediaCoreCommandTest.cpp` (search for `renderSyntheticTick` / `lastProgramFrameForTest` usage); the F1 pixel assertions are already written in `CaptureIngestTest.cpp:100-104` — reuse that shape.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build native/build-dev --config Release --target corevideo-native-tests`
Expected: FAIL — `MediaCore` has no `addSourceForTest`.

- [ ] **Step 3: Add the member, test seam, and ingest/merge**

In `native/src/core/MediaCore.h`: add `#include "core/SourceBus.h"`, a private `std::unique_ptr<core::SourceBus> sourceBus_;`, and a public `void addSourceForTest(std::shared_ptr<core::ISource> source);`.

In the MediaCore constructor body (`MediaCore.cpp`), construct it: `sourceBus_ = std::make_unique<core::SourceBus>();`.

Add the seam:
```cpp
void MediaCore::addSourceForTest(std::shared_ptr<core::ISource> source) {
  std::lock_guard<std::mutex> lock(coreMutex_);
  if (sourceBus_) sourceBus_->add(std::move(source));
}
```

In `renderSyntheticTick`, immediately after the capture frames are merged (`MediaCore.cpp:6043`, `videoFrames.insert(... captureFrames ...)`), ingest the bus and append — zero-copy, refs only:
```cpp
// Source bus (#535 slice 0): registered ISource sources publish here. Empty in
// production (no source registered outside the test seam); the test-pattern
// source is added only via addSourceForTest.
if (sourceBus_ && !sourceBus_->empty()) {
  const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  auto busResult = sourceBus_->ingest(mediaPresentationTime100ns, nowNs);
  videoFrames.insert(videoFrames.end(),
                     std::make_move_iterator(busResult.video.begin()),
                     std::make_move_iterator(busResult.video.end()));
  // busResult.audio is carried in a later slice (audio gather path); slice 0 is video.
}
```

- [ ] **Step 4: Run to verify PASS**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter=SourceBusMediaCore.*`
Expected: PASS (green SMPTE center in the captured program preview).

- [ ] **Step 5: Commit**

```bash
git add native/src/core/MediaCore.h native/src/core/MediaCore.cpp native/tests/SourceBusTest.cpp
git commit -m "feat(#535): MediaCore ingests the source bus + test seam (slice 0)"
```

---

### Task 4: Snapshot `sources[]` node with real counters

**Files:**
- Modify: `native/src/core/MediaCore.cpp` (`sessionState()`, add the node near `multiviewer` at `:984`)
- Test: `native/tests/SourceBusTest.cpp`

**Interfaces:**
- Consumes: `sourceBus_->snapshot(nowNs)` (Task 3), `SourceBus::SourceStatus` (Task 2).
- Produces: `sessionState()` carries a `sources` array; each element `{sourceId, kind, width, height, framesIngested, droppedFrames, health}`. Present even when empty (the multiviewer-node rule).

- [ ] **Step 1: Write the failing test**

```cpp
// append to native/tests/SourceBusTest.cpp
TEST(SourceBusSnapshot, SourcesNodeCarriesPerSourceCounters) {
  corevideo::core::MediaCore core(corevideo::modules::createStubModules());
  core.addSourceForTest(std::make_shared<corevideo::core::TestPatternSource>("test:pattern"));
  // drive one render tick (same helper as Task 3) ...
  const auto state = core.sessionState();
  const auto* sources = state.get("sources");
  ASSERT_NE(sources, nullptr);
  ASSERT_TRUE(sources->isArray());
  ASSERT_EQ(sources->array().size(), 1u);
  const auto& s = sources->array().front();
  EXPECT_EQ(s.get("sourceId")->stringValue(), "test:pattern");
  EXPECT_GE(s.get("framesIngested")->intValue(), 1);
  EXPECT_EQ(s.get("droppedFrames")->intValue(), 0);
}
```

Note: match the exact `rpc::Json` accessor names used around `MediaCore.cpp:644-1000` (`isArray()`/`array()`/`get()`/`stringValue()`/`intValue()`) — copy the shape from how the `multiviewer` or `zoomSubscriptionChurn` node is read in an existing sessionState test.

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build native/build-dev --config Release --target corevideo-native-tests`
Expected: FAIL — no `sources` node.

- [ ] **Step 3: Emit the node in `sessionState()`**

Near the `multiviewer` emission (`MediaCore.cpp:984`), mirror its unconditional shape:
```cpp
{
  rpc::Json::Array sourcesArr;
  if (sourceBus_) {
    const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    for (const auto& s : sourceBus_->snapshot(nowNs)) {
      sourcesArr.push_back(rpc::Json::Object{
          {"sourceId", s.descriptor.sourceId},
          {"kind", s.descriptor.kind},
          {"width", s.descriptor.width},
          {"height", s.descriptor.height},
          {"framesIngested", static_cast<int64_t>(s.counters.framesIngested)},
          {"droppedFrames", static_cast<int64_t>(s.counters.droppedFrames)},
          {"health", sourceHealthName(s.health)},  // small local: Producing->"producing" etc.
      });
    }
  }
  state.emplace("sources", std::move(sourcesArr));
}
```
Add a small pure `sourceHealthName(SourceHealth)` free function in `SourceBus.h` (returns `"producing"|"warming"|"stalled"|"failed"|"idle"`). Match the exact `rpc::Json` construction idiom already used at the `multiviewer`/`tiles` sites.

- [ ] **Step 4: Run to verify PASS**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter=SourceBusSnapshot.*:SourceBus*.*`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/SourceBus.h native/src/core/MediaCore.cpp native/tests/SourceBusTest.cpp
git commit -m "feat(#535): snapshot sources[] node with framesIngested/droppedFrames (slice 0)"
```

---

### Task 5: Full-suite verification (stub + Windows dev) + CLAUDE.md note

**Files:**
- Modify: `CLAUDE.md` (a short "Source bus (#535 slice 0)" section — the standing keep-CLAUDE.md-current directive)

- [ ] **Step 1: Run the stub gate (proves stub build green — Global Constraint)**

Run: `pwsh -File scripts/test-native.ps1`
Expected: PASS, and the new `SourceContract.*` / `SourceBus*.*` tests appear in the output (confirm they compiled into the stub binary; if a newly added test is absent, you ran a stale binary — see `CLAUDE.md` "Build & run").

- [ ] **Step 2: Run the full Windows dev suite**

Confirm the Release core: `native/build-dev/corevideo-native.exe` is ~2.1MB, not ~8.3MB (Debug). Then:
Run: `native/build-dev/corevideo-native-tests.exe`
Expected: the whole suite passes (~1037 tests on Windows), including the new source-bus tests and the unchanged `CaptureIngest.*` (proves the StubModules generator swap is byte-identical).

- [ ] **Step 3: Document it in CLAUDE.md**

Add a short section under the media/sources area: what the bus is, that it is header-only (`core/SourceBus.h`), that in production it is empty and the test-pattern source is test-seam-only, that `source_id == VideoFrame::participantId`, and that slices 1-3 migrate Zoom/capture/media onto it. Link the spec.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(#535): source bus slice 0 note in CLAUDE.md"
```

---

## Self-Review

**Spec coverage** (against `2026-09-18-source-bus-design.md`):
- §2 contract (id/format/dims/timestamp/pixels/PCM/health/clock offset) → Task 1 (types + `SourceDescriptor`/`SourceTick`).
- §2 pull model, no `layers` arg → Task 1 (`poll(programTime100ns)`).
- §2 clock offset reported, behavior unchanged → `SourceTick::clockOffset100ns` (Task 1); no live-kind timing touched (slice 0 registers only the test source).
- §3 bus owns sources, ingests under `coreMutex`, zero-copy → Task 3 (ingest in `renderSyntheticTick`, move-iterators/shared_ptr).
- §3 per-source counters + health at read → Task 2 (`framesIngested`/`droppedFrames`, `snapshot()` health).
- §3 snapshot `sources[]` present even at zero → Task 4.
- §5 slice 0 = contract + bus + test-pattern alongside old pipes, stub green, F1 gate → Tasks 1-5; three live pipes untouched.
- §6 proof: `SourceBusTest`, extended F1 CPU-compositor assertion → Tasks 2-3; continuity ledger unchanged (no live kind moved).
- Issue done-when #5 (stub green) → Task 5 Step 1.
- **Deferred by design (not slice 0):** done-when #2 (Zoom/UVC/media on the contract) and #3 (downstream consumes ONLY the bus) are slices 1-4; the frame pool / `droppedFrames` from real back-pressure lands when the first high-rate live kind migrates (the test source never drops). Slice 0 wires the `droppedFrames` field and reports 0 honestly.

**Placeholder scan:** the only prose-only step is Task 3 Step 1 / Task 4 Step 1's tick-drive, which points at the exact existing pattern to copy (`MediaCoreCommandTest.cpp` tick-drive + `CaptureIngestTest.cpp:100-104` pixel assertions) rather than inventing an unverified harness call — the harness accessor names differ across the suite and must be read from the real file, not guessed. No TBD/TODO/"add error handling" placeholders.

**Type consistency:** `ISource::poll(int64_t)`, `SourceTick{video,audio,health,clockOffset100ns}`, `SourceBus::ingest(programTime100ns, nowNs)→IngestResult`, `SourceBus::snapshot(nowNs)→vector<SourceStatus>`, `MediaCore::addSourceForTest(shared_ptr<ISource>)`, `sources[]` node fields — all names match across Tasks 1-4.
