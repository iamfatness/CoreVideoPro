# Source Bus — Slice 2 (capture devices onto the bus) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route every capture-kind video source (UVC/camera, screen, browser, SRT-ingest transport) through the `ISource` / `SourceBus` contract, one `ISource` per device, keyed by the existing `capture:<id>` frame key, so the per-source `sources[]` snapshot node (framesIngested / droppedFrames / health) covers capture — while the on-air result, the `CaptureIngest.*` tests, the roster merge, and the still-media path stay byte-for-byte unchanged.

**Architecture:** Capture adapters already keep their own last frame and re-emit it every tick (`WinUiCaptureDeviceAdapter::pollVideoFrames` "Emit the latest frame we have"), and a disconnected / never-delivered device emits nothing (`CaptureIngest.DisconnectedDeviceEmitsNoFrame`). So the bus mirrors the adapters exactly: MediaCore polls the capture adapter + browser host as today, feeds one `CaptureDeviceSource` per emitted frame through a pure `syncCaptureSources` helper, removes capture sources absent from the tick (the adapter is the frame holder, not the bus — the opposite of the Zoom rule in `ZoomBusRoster.h`, and for a stated reason), and the slice-0 bus ingest then produces the capture frames into `videoFrames` in the same position they occupy today.

**Tech Stack:** C++17, MediaCore (`native/`), GoogleTest via the repo's gtest shim, the slice-0/1 `SourceBus` (`native/src/core/SourceBus.h`), CMake multi-config dev core (`cmake --build native/build-dev --config Release --target corevideo-native corevideo-native-tests`).

**Spec:** `docs/superpowers/specs/2026-09-18-source-bus-design.md` (§4 "Capture" row: "one `ISource` per device; `poll()` serves the reader's latest BGRA; `droppedFrames` from the reader's own counter"; §5 slice 2). Slice 1 shipped Zoom (`docs/superpowers/plans/2026-09-18-source-bus-slice1-zoom.md`) and its live regression + fix are recorded in `CLAUDE.md` ("Slice 1 shipped a live regression the drill could not see (#554…)") — read that paragraph before Task 3.

## Global Constraints

- **Per-device granularity:** one `CaptureDeviceSource` per capture frame key. `descriptor().sourceId == VideoFrame::participantId == "capture:<deviceId>"` exactly as the adapters already emit it (`"capture:" + deviceId`, browser sources `"capture:browser:<n>"`). Never re-key a frame.
- **Video only.** Capture-transport audio (`modules_.captureDevice->pollAudioFrames`, MediaCore ~7060) and WASAPI capture audio are UNTOUCHED. Do not route audio through the bus.
- **Zero-copy.** A `VideoFrame`'s `pixels` is a `shared_ptr`; feed the source by moving/copying the struct, never the bytes. No pixel work under `coreMutex`.
- **Removal rule is the OPPOSITE of Zoom, deliberately.** Zoom removes a bus source only when the engine roster has let the participant go, because `ZoomEngineRuntime` erases its decoded frame on unsubscribe and the old store held it (#554). Capture adapters hold their own last frame and emit it every tick while connected; a device absent from a tick's frames is disconnected or has never delivered, and today's compositor shows no frame for it. So `syncCaptureSources` removes capture sources absent from the tick. Encode this reasoning in the helper's comment and in a test named for it.
- **Order and merge parity.** Today `videoFrames = [captureFrames…]` then `[bus frames…]` (Zoom) before the engine-roster merge, and the merge re-appends `captureFrames` after the roster (`MediaCore.cpp` "for (auto& captureFrame : captureFrames) merged.push_back"). After this slice the capture frames come out of the bus ingest; they must (a) still be appended to `videoFrames` BEFORE the Zoom bus frames and (b) still be carried through the roster merge. Partition the bus output by the bus source's `descriptor().kind`.
- **Regression gates that MUST stay green:** `CaptureIngest.*` (3 tests; `CaptureFrameCompositesRealPixelsIntoProgramPreview` proves capture pixels reach program), `MediaCoreCommand.*`, `SourceBus.*`, `ZoomBusRoster.*`, `SourceContinuityLedger.*`, the full Windows dev suite, the stub gate `scripts/test-native.ps1`, `node scripts/validate-multiview.mjs`, and the recorded gap test `python scripts/qa/zoom-gap-hold-ab.py` (program luma must hold through the gap exactly as on `main`).
- **Build/run the dev core with `--config Release`** and check `corevideo-native.exe` is ~2.26 MB (not ~8 MB Debug). The gtest shim accepts ONE wildcard pattern per `--gtest_filter` (`*ZoomBusRoster*` works; `A.*:B.*` does not). Run the full suite before each commit.
- **Branch:** `codex/535-slice2-capture` off `origin/main` (e07f9f5 or later). Commits reference `#535`.

---

### Task 1: `CaptureDeviceSource` — one ISource per capture device

**Files:**
- Create: `native/src/core/CaptureDeviceSource.h` (header-only, mirrors `native/src/core/ZoomParticipantSource.h`)
- Test: `native/tests/SourceBusTest.cpp` (append)

**Interfaces:**
- Consumes: `core::ISource`, `core::SourceDescriptor`, `core::SourceTick`, `core::SourceHealth`, `core::SourceIngestCounters` (`SourceBus.h`); `modules::VideoFrame` (`Interfaces.h`).
- Produces: `class core::CaptureDeviceSource final : public core::ISource`
  - ctor `CaptureDeviceSource(std::string sourceId, int width, int height)` — `sourceId` is the full `"capture:<id>"` key; descriptor `kind = "capture"`, `pixelFormat = "bgra"`, `hasVideo = true`.
  - `void setLatest(modules::VideoFrame frame)` — stores the frame (move), `hasFrame_ = true`, bumps `counters_.framesIngested`, sets `counters_.lastFrameId`.
  - `SourceTick poll(int64_t)` — `video = {latest_}` + `Producing` when a frame is held; empty + `Warming` otherwise.
  - `SourceIngestCounters counters() const`.

- [ ] **Step 1: Write the failing test** (append to `native/tests/SourceBusTest.cpp`, after the `ZoomParticipantSource` test)

```cpp
#include "core/CaptureDeviceSource.h"

static corevideo::modules::VideoFrame bgraFrame(const std::string& id, int w, int h, int64_t frameId) {
  corevideo::modules::VideoFrame f;
  f.participantId = id;
  f.width = f.pixelWidth = f.naturalWidth = w;
  f.height = f.pixelHeight = f.naturalHeight = h;
  f.pixelStride = w * 4;
  f.pixels = std::make_shared<const std::vector<uint8_t>>(static_cast<size_t>(w) * h * 4, 0x7f);
  f.frameId = frameId;
  return f;
}

TEST(CaptureDeviceSource, PollServesTheAdaptersLatestBgraFrameKeyedByDevice) {
  corevideo::core::CaptureDeviceSource src("capture:decklink-1", 640, 360);
  EXPECT_EQ(src.descriptor().sourceId, "capture:decklink-1");
  EXPECT_EQ(src.descriptor().kind, "capture");
  EXPECT_EQ(src.descriptor().pixelFormat, "bgra");
  EXPECT_TRUE(src.descriptor().hasVideo);

  auto warm = src.poll(0);
  EXPECT_TRUE(warm.video.empty());
  EXPECT_EQ(warm.health, corevideo::core::SourceHealth::Warming);

  src.setLatest(bgraFrame("capture:decklink-1", 640, 360, 7));
  auto tick = src.poll(0);
  ASSERT_EQ(tick.video.size(), 1u);
  EXPECT_EQ(tick.video[0].participantId, "capture:decklink-1");
  EXPECT_TRUE(tick.video[0].hasPixels());
  EXPECT_EQ(tick.video[0].frameId, 7);
  EXPECT_EQ(tick.health, corevideo::core::SourceHealth::Producing);
  EXPECT_EQ(src.counters().lastFrameId, 7);
  EXPECT_EQ(src.counters().framesIngested, 1u);
}
```

- [ ] **Step 2: Run to verify it fails.** Build: `cmake --build native/build-dev --config Release --target corevideo-native-tests`. Expected: compile error, `core/CaptureDeviceSource.h` not found.

- [ ] **Step 3: Write the header**

```cpp
// native/src/core/CaptureDeviceSource.h
#pragma once
#include "core/SourceBus.h"

namespace corevideo::core {

// One capture device (UVC/camera, screen, browser host, SRT-ingest transport) on
// the source bus (#535 slice 2). The ADAPTER owns the frame: it keeps the last
// BGRA frame and re-emits it every tick while the device is connected, so this
// source only mirrors the frame MediaCore's capture poll handed it this tick.
// Keyed by the adapter's own "capture:<deviceId>" frame key — never re-keyed.
class CaptureDeviceSource final : public ISource {
 public:
  CaptureDeviceSource(std::string sourceId, int width, int height) {
    descriptor_.sourceId = std::move(sourceId);
    descriptor_.kind = "capture";
    descriptor_.width = width;
    descriptor_.height = height;
    descriptor_.pixelFormat = "bgra";
    descriptor_.hasVideo = true;
  }
  const SourceDescriptor& descriptor() const override { return descriptor_; }

  void setLatest(modules::VideoFrame frame) {
    latest_ = std::move(frame);  // shared_ptr payload: no pixel copy
    hasFrame_ = true;
    counters_.framesIngested += 1;
    counters_.lastFrameId = latest_.frameId;
  }

  SourceTick poll(int64_t) override {
    SourceTick tick;
    if (hasFrame_) {
      tick.video.push_back(latest_);
      tick.health = SourceHealth::Producing;
    } else {
      tick.health = SourceHealth::Warming;
    }
    return tick;
  }
  SourceIngestCounters counters() const override { return counters_; }

 private:
  SourceDescriptor descriptor_;
  modules::VideoFrame latest_;
  bool hasFrame_ = false;
  SourceIngestCounters counters_;
};

}  // namespace corevideo::core
```

- [ ] **Step 4: Run to verify it passes.** Build, then `native/build-dev/corevideo-native-tests.exe --gtest_filter=*CaptureDeviceSource*`. Expected: 1 passed. Then `--gtest_filter=*SourceBus*` and `--gtest_filter=*ZoomBusRoster*` still pass.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/CaptureDeviceSource.h native/tests/SourceBusTest.cpp
git commit -m "feat(#535): CaptureDeviceSource ISource (slice 2)"
```

---

### Task 2: Pure `syncCaptureSources` helper (adapter frames → bus)

**Files:**
- Create: `native/src/core/CaptureBusRoster.h` (header-only, mirrors `native/src/core/ZoomBusRoster.h`)
- Test: `native/tests/SourceBusTest.cpp` (append)

**Interfaces:**
- Consumes: `core::SourceBus` (`add/remove/contains/sourceFor/sourceIds`), `core::CaptureDeviceSource` (Task 1), `modules::VideoFrame`.
- Produces: `inline void core::syncCaptureSources(SourceBus& bus, const std::vector<modules::VideoFrame>& captureFrames)`.
  - For each frame: if `!bus.contains(f.participantId)` → `bus.add(make_shared<CaptureDeviceSource>(f.participantId, w, h))` with `w/h` from `pixelWidth/pixelHeight` (fall back to `i420Width/i420Height` if a future adapter delivers I420); then `static_cast<CaptureDeviceSource*>(bus.sourceFor(id))->setLatest(f)`.
  - Then remove every bus source whose `descriptor().kind == "capture"` and whose id is NOT in this tick's frames. Never touch any other kind.

- [ ] **Step 1: Write the failing tests** (append to `native/tests/SourceBusTest.cpp`)

```cpp
#include "core/CaptureBusRoster.h"

// Capture adapters hold their own last frame and re-emit it every tick while the
// device is connected (WinUiCaptureDeviceAdapter::pollVideoFrames); a device that
// is absent from a tick has disconnected or never delivered, and today the
// compositor draws no frame for it. So, UNLIKE the Zoom rule (ZoomBusRoster.h,
// #554), the bus does NOT hold a capture frame: absent from the tick == removed.
TEST(CaptureBusRoster, MirrorsTheAdaptersTickAddsFeedsAndRemovesOnAbsence) {
  corevideo::core::SourceBus bus;
  bus.add(std::make_shared<corevideo::core::TestPatternSource>("test:pattern"));
  bus.add(std::make_shared<corevideo::core::ZoomParticipantSource>("16778240", 1280, 720));

  corevideo::core::syncCaptureSources(bus, {bgraFrame("capture:decklink-1", 640, 360, 1),
                                            bgraFrame("capture:browser:1", 1920, 1080, 1)});
  EXPECT_TRUE(bus.contains("capture:decklink-1"));
  EXPECT_TRUE(bus.contains("capture:browser:1"));
  EXPECT_EQ(bus.sourceFor("capture:decklink-1")->descriptor().kind, "capture");
  EXPECT_EQ(bus.sourceFor("capture:decklink-1")->descriptor().width, 640);

  auto r = bus.ingest(0, 1000);
  int captureFrames = 0;
  for (const auto& v : r.video) {
    if (v.participantId.rfind("capture:", 0) == 0) { ++captureFrames; EXPECT_TRUE(v.hasPixels()); }
  }
  EXPECT_EQ(captureFrames, 2);

  // decklink-1 disconnected (adapter emits nothing for it): removed this tick.
  // The Zoom and test sources are never touched by the capture sync.
  corevideo::core::syncCaptureSources(bus, {bgraFrame("capture:browser:1", 1920, 1080, 2)});
  EXPECT_FALSE(bus.contains("capture:decklink-1"));
  EXPECT_TRUE(bus.contains("capture:browser:1"));
  EXPECT_TRUE(bus.contains("16778240"));
  EXPECT_TRUE(bus.contains("test:pattern"));

  // A device that reconnects comes back as a fresh source.
  corevideo::core::syncCaptureSources(bus, {bgraFrame("capture:decklink-1", 1920, 1080, 1)});
  EXPECT_TRUE(bus.contains("capture:decklink-1"));
  EXPECT_EQ(bus.sourceFor("capture:decklink-1")->descriptor().width, 1920);
}

TEST(CaptureBusRoster, EmptyTickRemovesEveryCaptureSourceAndNothingElse) {
  corevideo::core::SourceBus bus;
  bus.add(std::make_shared<corevideo::core::ZoomParticipantSource>("16778240", 1280, 720));
  corevideo::core::syncCaptureSources(bus, {bgraFrame("capture:screen:3", 1024, 600, 1)});
  corevideo::core::syncCaptureSources(bus, {});
  EXPECT_FALSE(bus.contains("capture:screen:3"));
  EXPECT_TRUE(bus.contains("16778240"));
}
```

- [ ] **Step 2: Run to verify it fails.** Build → compile error (`core/CaptureBusRoster.h` missing).

- [ ] **Step 3: Write the helper**

```cpp
// native/src/core/CaptureBusRoster.h
#pragma once
#include <string>
#include <unordered_set>
#include <vector>

#include "core/CaptureDeviceSource.h"
#include "core/SourceBus.h"

namespace corevideo::core {

// Capture adapters → bus, one CaptureDeviceSource per "capture:<id>" frame the
// adapters emitted THIS tick. The adapter is the frame holder: it re-emits its
// last frame every tick while the device is connected and emits nothing once it
// is disconnected or before it ever delivered — which is exactly what the
// compositor drew before the bus. So a capture source absent from the tick is
// removed. This is the OPPOSITE of syncZoomParticipantSources on purpose: the
// Zoom engine erases its decoded frame on unsubscribe and the pre-bus store
// masked that (#554); no capture adapter erases on a subscription gap.
inline void syncCaptureSources(SourceBus& bus,
                               const std::vector<modules::VideoFrame>& captureFrames) {
  std::unordered_set<std::string> present;
  for (const auto& f : captureFrames) {
    present.insert(f.participantId);
    if (!bus.contains(f.participantId)) {
      const int w = f.pixelWidth > 0 ? f.pixelWidth : f.i420Width;
      const int h = f.pixelHeight > 0 ? f.pixelHeight : f.i420Height;
      bus.add(std::make_shared<CaptureDeviceSource>(f.participantId, w, h));
    }
    static_cast<CaptureDeviceSource*>(bus.sourceFor(f.participantId))->setLatest(f);
  }
  for (const std::string& id : bus.sourceIds()) {
    const ISource* source = bus.sourceFor(id);
    if (source && source->descriptor().kind == "capture" && present.find(id) == present.end()) {
      bus.remove(id);
    }
  }
}

}  // namespace corevideo::core
```

- [ ] **Step 4: Run to verify it passes.** `--gtest_filter=*CaptureBusRoster*` → 2 passed; `*SourceBus*`, `*ZoomBusRoster*`, `*CaptureDeviceSource*` still green.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/CaptureBusRoster.h native/tests/SourceBusTest.cpp
git commit -m "feat(#535): pure syncCaptureSources adapter->bus helper (slice 2)"
```

---

### Task 3: MediaCore — produce capture video from the bus, same position, same merge

**Files:**
- Modify: `native/src/core/MediaCore.cpp` (`renderSyntheticTick`, from the capture poll `auto captureFrames = modules_.captureDevice->pollVideoFrames(frameTimestampMs);` through the engine-roster merge that ends with `videoFrames = std::move(merged);`)
- Modify: `native/src/core/MediaCore.h` (add `#include "core/CaptureBusRoster.h"` next to `core/ZoomBusRoster.h`)
- Test: `native/tests/MediaCoreCommandTest.cpp` (append one snapshot assertion)

**Interfaces:**
- Consumes: `sourceBus_` (slice 0), `syncCaptureSources` (Task 2), `syncZoomParticipantSources` (slice 1), `engineFrames` (already polled once per tick before the Zoom tap since #556).
- Produces: capture frames flow into `videoFrames` via the bus ingest; `sessionState()["sources"]` lists `kind:"capture"` entries with `framesIngested`/`health`.

**Design — the exact rewrite of that block (read the current code first; line numbers drift):**

Today:
```cpp
auto videoFrames = engineLive ? std::vector<modules::VideoFrame>{} : modules_.zoom->pollVideoFrames();
auto captureFrames = modules_.captureDevice->pollVideoFrames(frameTimestampMs);
if (!browserSources_->empty()) { /* append browser frames to captureFrames */ }
markStage(s_subPollUs, 0);
videoFrames.insert(videoFrames.end(), captureFrames.begin(), captureFrames.end());
if (sourceBus_ && !sourceBus_->empty()) {
  auto busResult = sourceBus_->ingest(mediaPresentationTime100ns, nowNs);
  videoFrames.insert(videoFrames.end(), move(busResult.video)...);
}
if (engine configured) { if (!engineFrames.empty()) { merged = roster-with-content; for (captureFrame : captureFrames) merged.push_back(...); videoFrames = merged; } }
```

After:
```cpp
auto videoFrames = engineLive ? std::vector<modules::VideoFrame>{} : modules_.zoom->pollVideoFrames();
auto polledCapture = modules_.captureDevice->pollVideoFrames(frameTimestampMs);
if (!browserSources_->empty()) { /* append browser frames to polledCapture, unchanged */ }
markStage(s_subPollUs, 0);
// #535 slice 2: capture rides the bus. The adapters hold their frames; the bus
// mirrors this tick's poll (CaptureBusRoster.h), so on-air output is identical.
if (sourceBus_) {
  core::syncCaptureSources(*sourceBus_, polledCapture);
}
// Bus ingest, partitioned by kind so the merged vector keeps today's order:
// capture frames first (where the direct insert used to put them), then Zoom.
std::vector<modules::VideoFrame> captureFrames;   // KEEP this name: the merge below uses it
std::vector<modules::VideoFrame> zoomBusFrames;
if (sourceBus_ && !sourceBus_->empty()) {
  const int64_t nowNs = ...;   // unchanged
  auto busResult = sourceBus_->ingest(mediaPresentationTime100ns, nowNs);
  for (auto& frame : busResult.video) {
    const auto* source = sourceBus_->sourceFor(frame.participantId);
    const bool isCapture = source && source->descriptor().kind == "capture";
    (isCapture ? captureFrames : zoomBusFrames).push_back(std::move(frame));
  }
}
videoFrames.insert(videoFrames.end(), captureFrames.begin(), captureFrames.end());
videoFrames.insert(videoFrames.end(), make_move_iterator(zoomBusFrames.begin()), make_move_iterator(zoomBusFrames.end()));
// engine-roster merge: UNCHANGED — it still finds Zoom content in videoFrames by
// participantId and re-appends `captureFrames` after the roster.
```

Notes for the implementer:
- `zoomBusFrames` also carries the test-pattern source (kind `"test"`) — it went through the old "bus frames after capture" position too, so grouping every non-capture kind there preserves order.
- Do not delete the `polledCapture` poll or the browser append; only where the frames go changes.
- `SourceBus::ingest` counts `isNew` per source by `frameId`, so a held capture frame re-emitted by the adapter with the same `frameId` correctly does NOT bump `framesIngested` — that is the honest per-source rate.

- [ ] **Step 1: Write the failing test** (append to `native/tests/MediaCoreCommandTest.cpp`; look at how the existing slice-0/1 tests there construct `MediaCore` with `createStubModules()` and read `sessionState()` — copy that setup)

```cpp
// #535 slice 2: the stub capture set has decklink-1 connected with signal, so
// after one render tick the bus must list it as a capture source that produced.
TEST(MediaCoreCommand, StubCaptureDeviceAppearsOnTheSourceBusAfterATick) {
  auto core = MediaCoreCommandTestHelpers::makeStubCore();   // whatever the file's existing helper is named
  MediaCoreCommandTestHelpers::renderOneTick(*core);          // whatever the file's existing helper is named
  const auto state = core->sessionState();
  const rpc::Json* sources = state.get("sources");
  ASSERT_NE(sources, nullptr);
  ASSERT_TRUE(sources->isArray());
  bool found = false;
  for (const auto& s : sources->asArray()) {
    if (s.getString("sourceId") == "capture:decklink-1") {
      found = true;
      EXPECT_EQ(s.getString("kind"), "capture");
      EXPECT_EQ(s.getString("health"), "producing");
      EXPECT_GE(s.get("framesIngested")->asNumber(), 1.0);
    }
    EXPECT_NE(s.getString("sourceId"), "capture:aja-io-1") << "a detected-only device emits nothing and must not be on the bus";
  }
  EXPECT_TRUE(found);
}
```
If the file has no "render one tick" helper, use the same call the existing `sources[]` slice-0 test in that file uses to drive a tick (search for `"sources"` in `MediaCoreCommandTest.cpp` and reuse its setup verbatim).

- [ ] **Step 2: Run to verify it fails.** Build; `--gtest_filter=*StubCaptureDeviceAppearsOnTheSourceBus*` → FAIL (`found` false: capture is not on the bus yet).

- [ ] **Step 3: Implement the rewrite** per the Design above; add the include to `MediaCore.h`.

- [ ] **Step 4: Run the gates.** In this order, each must PASS: `--gtest_filter=*StubCaptureDeviceAppearsOnTheSourceBus*`, `--gtest_filter=*CaptureIngest*` (all 3, including `CaptureFrameCompositesRealPixelsIntoProgramPreview`), `--gtest_filter=*MediaCoreCommand*`, `--gtest_filter=*SourceContinuityLedger*`, `--gtest_filter=*ZoomBusRoster*`, then the full suite `native/build-dev/corevideo-native-tests.exe` (expect ≥1057 passed, 0 failed).

- [ ] **Step 5: Commit**

```bash
git add native/src/core/MediaCore.cpp native/src/core/MediaCore.h native/tests/MediaCoreCommandTest.cpp
git commit -m "feat(#535): MediaCore produces capture video from the bus (slice 2)"
```

---

### Task 4: Measured per-source fps on the Sources page line (core)

**Files:**
- Modify: `native/src/modules/ZoomEngineRuntime.cpp` (`spineSnapshotLocked`: the `{"deliveredFps", hasStats ? 30 : 0}` entry)
- Test: the existing runtime/state test file that covers `spineSnapshotLocked` or `subscriptions` JSON (search `native/tests` for `deliveredFps`; if nothing asserts it, add the test to `native/tests/ZoomEngineRuntimeTest.cpp` next to the other spine-snapshot assertions)

**Interfaces:**
- Consumes: `ZoomSubscriptionStats` fields `framesReceived`, `firstFrameAtMs`, `lastFrameAtMs` (`ZoomEngineState.h`).
- Produces: `deliveredFps` = measured average since the first frame: `framesReceived >= 2 && lastFrameAtMs > firstFrameAtMs ? round((framesReceived - 1) * 1000.0 / (lastFrameAtMs - firstFrameAtMs)) : 0`.

Why: the Sources page prints `"{w}x{h} @ {fps}fps"` from this field (`ProductionModels.BuildFeedHealthRows`), and today it is a constant 30 for any subscribed feed — a number that survives every real fps problem. PR #557 fixed the rows being empty; this makes the number honest.

- [ ] **Step 1: Write the failing test** (drive the state the way the file's existing subscription tests do: record N frame-ingest successes with known `elapsedMs` stamps, then read the spine snapshot's `subscriptions[0].deliveredFps`)

```cpp
TEST(ZoomEngineRuntime, SpineSnapshotReportsMeasuredDeliveredFps) {
  // 61 frames over 1000 ms == 60 fps (61 - 1 intervals).
  // Use the file's existing helper to publish a subscribed participant-video
  // stream, then record frame ingest successes at elapsedMs = 0, 16.67, ... 1000.
  // Assert: deliveredFps == 60 (not the old constant 30).
}
```
Write the body against the real helpers in that file (do not invent new ones); the assertion is `EXPECT_EQ(subscription.get("deliveredFps")->asNumber(), 60.0)`. Also assert a stream with a single frame reports `0`.

- [ ] **Step 2: Run to verify it fails.** `--gtest_filter=*MeasuredDeliveredFps*` → FAIL (reads 30).

- [ ] **Step 3: Implement** the formula in `spineSnapshotLocked` (a small static helper `measuredDeliveredFps(const ZoomSubscriptionStats&)` next to the function).

- [ ] **Step 4: Run** `--gtest_filter=*MeasuredDeliveredFps*` → PASS; `--gtest_filter=*ZoomEngineRuntime*` → all PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/ZoomEngineRuntime.cpp native/tests/ZoomEngineRuntimeTest.cpp
git commit -m "fix(zoom): report the measured per-feed fps in the spine snapshot, not a constant 30"
```

---

### Task 5: Regression validation + docs

**Files:**
- Modify: `CLAUDE.md` (extend the source-bus section after the slice-1 / #554 paragraphs)
- Modify: `docs/BACKLOG.md` (the #535 row: slice 2 shipped; slice 3 media next)

- [ ] **Step 1: Full suites green.** Windows dev suite `native/build-dev/corevideo-native-tests.exe` (0 failed) and the stub gate `pwsh -File scripts/test-native.ps1` (capture flows through the stub `FakeCaptureDevice`, so this path IS exercised — it must be green).

- [ ] **Step 2: The two recorded/real-ingest gates.** `node scripts/validate-multiview.mjs` PASS (capture tiles still reach multiview). `python scripts/qa/zoom-gap-hold-ab.py --core native/build-dev/corevideo-native.exe --fake native/build-dev/corevideo-zoom-engine-fake.exe --label slice2` then `ffmpeg -i rec-slice2/*/Program.mp4 -vf signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=slice2.yavg.txt -f null -` — program luma must hold (~188) through the gap with no dip (the slice-1 regression signature was ~150 for 0.5 s). Also `python scripts/mac-show-drill.py --seconds 40 --load 8` with `COREVIDEO_FAKE_ENGINE_FPS=60`: fps/delivery/over-budget within the slice-1 table in `CLAUDE.md`.

- [ ] **Step 3: Document.** In `CLAUDE.md` under the source-bus section add a "Slice 2 (date): capture onto the bus" paragraph: one `CaptureDeviceSource` per `capture:<id>` frame, fed from the unchanged adapter poll through `syncCaptureSources`; the bus MIRRORS the adapters' tick (removal on absence) because adapters hold their own last frame — the opposite of the Zoom rule, and why; bus output partitioned by kind so capture frames keep their pre-bus position and still ride the roster merge; `sources[]` now lists capture; audio untouched; `ICaptureDevice` not deleted (slice 4). Record the Task 5 numbers. Add one line for the measured `deliveredFps`. Update the `#535` row in `docs/BACKLOG.md`.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md docs/BACKLOG.md
git commit -m "docs(#535): source bus slice 2 (capture) note + measured fps"
```

---

## Self-Review

**Spec coverage** (§4 Capture row, §5 slice 2):
- "one `ISource` per device" → Task 1.
- "`poll()` serves the reader's latest BGRA" → Task 1 (`setLatest`/`poll`), fed per tick by Task 3.
- "`droppedFrames` from the reader's own counter" → NOT wired this slice: `CaptureDeviceInfo::droppedFrames` is a per-device enumerate() field, not on the frame; the bus's own `droppedFrames` stays 0 for capture. Stated in Task 5 docs as a slice-4 item (when `ICaptureDevice` collapses into the source, the counter moves with it). Not a placeholder — a scoped omission.
- "probe-only devices become a uniform warming/stalled health instead of a per-kind slate" → partially: a device that never delivered is not on the bus (adapter emits nothing), so it is absent rather than `warming`; the compositor's per-kind slate remains until slice 4 by the spec's own phasing. Stated in Task 5 docs.
- Video only, audio later → Global Constraints.
- Live/recorded gates → Task 5 (multiview, gap-hold A/B, drill).

**Placeholder scan:** Task 3 Step 1 and Task 4 Step 1 point the implementer at the file's EXISTING helpers by search rather than inventing names — the assertions and formulas are fully specified. No TBD/TODO.

**Type consistency:** `CaptureDeviceSource(sourceId, w, h)` / `setLatest(VideoFrame)` / `poll` / `counters`; `syncCaptureSources(SourceBus&, const vector<VideoFrame>&)`; `SourceBus::contains/sourceFor/sourceIds/add/remove` (slice 0/1); descriptor `kind == "capture"` used identically in Tasks 1, 2, 3; the merge keeps the variable name `captureFrames`.

**Risk:** Task 3 touches the hot gather. Its safety net is that nothing about frame production changes — only the vector the frames travel in — and the order/merge parity is explicit. The #554 lesson is applied up front: the removal rule is argued from the adapter's actual hold behavior (`WinUiCaptureDeviceAdapter::pollVideoFrames` re-emits; `DisconnectedDeviceEmitsNoFrame`), and the recorded gap test plus `CaptureIngest.*` prove the on-air result.
