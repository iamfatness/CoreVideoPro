# Source Bus — Slice 1 (Zoom video onto the bus, per-participant) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route real Zoom **video** through the `ISource` / `SourceBus` contract, one `ISource` per participant, keyed `zoom:<pid>`, with per-participant `framesIngested`/health in the snapshot — while keeping the frame-sync cushion, subscription churn, source continuity, and A/V behavior byte-for-byte unchanged.

**Architecture:** Each live Zoom participant becomes a `ZoomParticipantSource : core::ISource` held in the existing `SourceBus`. MediaCore's per-tick tap (which today feeds `RealZoomCaptureSource` from `ZoomEngineRuntime::latestDecodedVideoFrames`) instead ensures/feeds one participant-source per participant and removes departed ones; the bus ingest (added in slice 0) then produces the Zoom frames into the merged `videoFrames` vector. The existing engine-roster reconciliation and the synthetic-slate fallback for the no-engine case are preserved.

**Tech Stack:** C++17, MediaCore (`native/`), GoogleTest, the slice-0 `SourceBus` (`native/src/core/SourceBus.h`), CMake multi-config dev core.

**Spec:** `docs/superpowers/specs/2026-09-18-source-bus-design.md` (§4 Zoom row, §5 slice 1). Slice 0 shipped the contract + bus (see the "The source bus" section in `CLAUDE.md`).

## Global Constraints

- **Per-participant granularity (owner-ratified 2026-09-18):** each Zoom participant is its own `ISource` (`descriptor.sourceId == "zoom:<pid>"`), added when it first decodes a frame and removed when it leaves. NOT one multi-frame Zoom source — the slice-0 `SourceBus` counts per entry (one `lastFrameId` per source), so one source must emit at most one video frame per tick.
- **Video only.** Zoom audio (`ZoomEngineRuntime::pollCompositorAudioFrames`, `requiresSteadyFeedPriming`, the pre-`coreMutex` merge in `pollZoomAudioUnlocked`) is UNTOUCHED this slice. Do not route Zoom audio through the bus.
- **`source_id == VideoFrame::participantId == "zoom:<pid>"`.** Downstream (compositor, ISO, multiview, `SourceContinuityLedger`, churn) keys on `participantId`; the migrated frames MUST carry the identical key, `hasI420()`/`hasPixels()` content contract, and per-participant identity, or continuity reads every source as a cold restart.
- **Zero-copy on the hot path.** Feeding a participant source takes the existing zero-copy `shared_ptr<const vector<uint8_t>>` I420 buffer (the `ingestI420Frame` shared overload economics). No pixel copy under `coreMutex`.
- **The cushion, churn ledger, and speaker director stay in `ZoomEngineRuntime`.** This slice moves only where the decoded frame is *published to the render gather* (into per-participant `ISource`s), not how it is decoded, cushioned, or accounted.
- **Preserve the two existing behaviors exactly:** (1) the `engineLive && participantCount==0` suppression (no synthetic slate once the engine is live) and (2) the engine-roster merge that gives every subscribed participant a real-or-metadata frame (`MediaCore.cpp:6089-6119`).
- **Build/run the dev core with `--config Release`** and check exe size (~2.1MB, not ~8.3MB Debug) before believing any result. gtest shim takes ONE glob per `--gtest_filter` (no colon-OR) — run patterns separately. Run the full suite once before each commit.
- **Regression gates that MUST stay green** (from the slice-1 code map): `ZoomEngineRuntimeTest.*` (frame-sync + churn), `SourceContinuityLedgerTest.*`, `RealZoomCaptureSourceTest.*`, and the fake-engine drill `scripts/mac-show-drill.py --load 8` plus `scripts/validate-multiview.mjs` / `validate-iso-record.mjs`.

---

### Task 1: `ZoomParticipantSource` — one ISource per participant

**Files:**
- Create: `native/src/core/ZoomParticipantSource.h` (header-only, like `TestPatternSource.h`)
- Test: `native/tests/SourceBusTest.cpp` (append)

**Interfaces:**
- Consumes: `core::ISource`, `core::SourceDescriptor`, `core::SourceTick`, `core::SourceHealth` (slice 0, `SourceBus.h`); `modules::VideoFrame` (`Interfaces.h:14`).
- Produces:
  - `class core::ZoomParticipantSource : public core::ISource` — ctor `ZoomParticipantSource(std::string participantId, int width, int height)` (participantId is the full `"zoom:<pid>"` key). Members: descriptor (kind `"zoom"`, `hasVideo=true`, `pixelFormat "i420"`), the latest `modules::VideoFrame latest_`, a `bool hasFrame_`, and `SourceIngestCounters counters_`.
  - `void setLatest(modules::VideoFrame frame)` — stores the frame (move), sets `hasFrame_=true`. The frame's `participantId` MUST equal the descriptor id.
  - `poll(int64_t)` — returns `SourceTick` with `video = {latest_}` when `hasFrame_`, health `Producing`; empty video + health `Warming` when no frame yet.
  - `counters()` — returns `counters_` (self-report; the bus keeps the authoritative per-entry counts, so this can stay minimal — bump a local frame count in `setLatest`).

- [ ] **Step 1: Write the failing test**

```cpp
// append to native/tests/SourceBusTest.cpp
#include "core/ZoomParticipantSource.h"
using corevideo::core::ZoomParticipantSource;

TEST(ZoomParticipantSource, PollReturnsTheSetFrameKeyedByParticipant) {
  ZoomParticipantSource src("zoom:42", 1280, 720);
  EXPECT_EQ(src.descriptor().sourceId, "zoom:42");
  EXPECT_EQ(src.descriptor().kind, "zoom");
  EXPECT_TRUE(src.descriptor().hasVideo);
  // No frame yet -> Warming, no video.
  auto warmup = src.poll(0);
  EXPECT_TRUE(warmup.video.empty());
  EXPECT_EQ(warmup.health, corevideo::core::SourceHealth::Warming);

  corevideo::modules::VideoFrame f;
  f.participantId = "zoom:42";
  f.i420 = std::make_shared<const std::vector<uint8_t>>(1280 * 720 * 3 / 2, 0x10);
  f.i420Width = 1280; f.i420Height = 720; f.frameId = 7;
  src.setLatest(f);
  auto tick = src.poll(0);
  ASSERT_EQ(tick.video.size(), 1u);
  EXPECT_EQ(tick.video.front().participantId, "zoom:42");
  EXPECT_EQ(tick.video.front().frameId, 7);
  EXPECT_TRUE(tick.video.front().hasI420());
  EXPECT_EQ(tick.health, corevideo::core::SourceHealth::Producing);
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build native/build-dev --config Release --target corevideo-native-tests`
Expected: FAIL — `core/ZoomParticipantSource.h` not found.

- [ ] **Step 3: Implement `native/src/core/ZoomParticipantSource.h`**

```cpp
#pragma once
#include "core/SourceBus.h"

namespace corevideo::core {

class ZoomParticipantSource final : public ISource {
 public:
  ZoomParticipantSource(std::string participantId, int width, int height) {
    descriptor_.sourceId = std::move(participantId);
    descriptor_.kind = "zoom";
    descriptor_.width = width;
    descriptor_.height = height;
    descriptor_.pixelFormat = "i420";
    descriptor_.hasVideo = true;
  }
  const SourceDescriptor& descriptor() const override { return descriptor_; }

  void setLatest(modules::VideoFrame frame) {
    latest_ = std::move(frame);
    hasFrame_ = true;
    counters_.framesIngested += 1;
    counters_.lastFrameId = latest_.frameId;
  }

  SourceTick poll(int64_t) override {
    SourceTick tick;
    if (hasFrame_) {
      tick.video.push_back(latest_);           // shared_ptr payload: cheap copy
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

- [ ] **Step 4: Run to verify PASS**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter=ZoomParticipantSource.*`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/ZoomParticipantSource.h native/tests/SourceBusTest.cpp
git commit -m "feat(#535): ZoomParticipantSource ISource (slice 1)"
```

---

### Task 2: `SourceBus` — membership queries for roster sync

**Files:**
- Modify: `native/src/core/SourceBus.h` (add two const accessors to the `SourceBus` class)
- Test: `native/tests/SourceBusTest.cpp` (append)

**Interfaces:**
- Consumes: the slice-0 `SourceBus` (`add`/`remove`/`ingest`/`snapshot`/`empty`).
- Produces:
  - `bool SourceBus::contains(const std::string& sourceId) const;` — true if an entry with that id exists.
  - `std::vector<std::string> SourceBus::sourceIds() const;` — all current entry ids, stable (map) order.

  These let MediaCore add a participant source only when absent and compute the departed set to remove, without duplicating the bus's map.

- [ ] **Step 1: Write the failing test**

```cpp
// append to native/tests/SourceBusTest.cpp
TEST(SourceBus, ContainsAndSourceIdsReflectMembership) {
  SourceBus bus;
  EXPECT_FALSE(bus.contains("zoom:1"));
  bus.add(std::make_shared<ZoomParticipantSource>("zoom:1", 1280, 720));
  bus.add(std::make_shared<ZoomParticipantSource>("zoom:2", 1280, 720));
  EXPECT_TRUE(bus.contains("zoom:1"));
  EXPECT_TRUE(bus.contains("zoom:2"));
  auto ids = bus.sourceIds();
  ASSERT_EQ(ids.size(), 2u);
  EXPECT_EQ(ids[0], "zoom:1");   // std::map order
  EXPECT_EQ(ids[1], "zoom:2");
  bus.remove("zoom:1");
  EXPECT_FALSE(bus.contains("zoom:1"));
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build native/build-dev --config Release --target corevideo-native-tests`
Expected: FAIL — `SourceBus` has no `contains`/`sourceIds`.

- [ ] **Step 3: Add the accessors to `SourceBus`**

```cpp
// inside class SourceBus, near empty()
bool contains(const std::string& sourceId) const { return entries_.count(sourceId) != 0; }
std::vector<std::string> sourceIds() const {
  std::vector<std::string> ids;
  ids.reserve(entries_.size());
  for (const auto& [id, e] : entries_) ids.push_back(id);
  return ids;
}
```

- [ ] **Step 4: Run to verify PASS**

Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter=SourceBus.*`
Expected: PASS (existing SourceBus tests + the new one).

- [ ] **Step 5: Commit**

```bash
git add native/src/core/SourceBus.h native/tests/SourceBusTest.cpp
git commit -m "feat(#535): SourceBus membership queries (slice 1)"
```

---

### Task 3: MediaCore — feed per-participant Zoom sources and produce Zoom video from the bus

**Files:**
- Modify: `native/src/core/MediaCore.cpp` (`renderSyntheticTick`, the Zoom tap + suppression gate, `~6021-6119`)
- Modify: `native/src/core/MediaCore.h` (include `ZoomParticipantSource.h` if needed)
- Test: `native/tests/SourceBusTest.cpp` or `native/tests/MediaCoreCommandTest.cpp` (a MediaCore-level test using the `addSourceForTest` seam is NOT enough here — this needs the real tap; see step 1)

**Interfaces:**
- Consumes: `sourceBus_` (slice 0), `ZoomParticipantSource` (Task 1), `SourceBus::contains`/`sourceIds` (Task 2), the existing `zoomEngineRuntime_->latestDecodedVideoFrames(...)` tap and `engineLive` gate.
- Produces: real Zoom video frames now flow into the merged `videoFrames` via the bus ingest, one `ZoomParticipantSource` per participant; departed participants' sources are removed each tick.

**Design (what changes in `renderSyntheticTick`):**
1. The tap loop (`~6031-6052`) currently calls `realZoom->ingestI420Frame/ingestFrame`. Change it so that, for each decoded frame, it ensures a `ZoomParticipantSource` exists in `sourceBus_` for that `participantId` (`if (!sourceBus_->contains(pid)) sourceBus_->add(std::make_shared<core::ZoomParticipantSource>(pid, w, h));`) and calls `setLatest(...)` on it (build the `VideoFrame` exactly as it's built for `RealZoomCaptureSource` today — zero-copy I420 or BGRA, same `participantId`, `frameId`, `timestampMs`). Keep a local set of the participant ids seen THIS tick.
2. After the tap, remove any Zoom source (`sourceIds()` filtered to the `zoom:` prefix) whose id was NOT seen this tick — that participant left / stopped decoding (mirrors `ZoomEngineRuntime` retiring a slot).
3. The suppression gate (`~6062`) changes to: `auto videoFrames = engineLive ? std::vector<modules::VideoFrame>{} : modules_.zoom->pollVideoFrames();`. With the engine live, real Zoom frames now come from the bus ingest below; the synthetic slate is only served when there is NO engine (unchanged). The old `participantCount()==0` special case is subsumed: with no fed sources, the bus produces no Zoom frames and `videoFrames` stays empty — identical on-air result.
4. The bus ingest (already added in slice 0 at `~6077`) now yields the Zoom frames (plus any test source). Its result must be merged into `videoFrames` BEFORE the engine-roster merge (`~6089`), which is already the case. The roster merge then reconciles bus-produced Zoom frames against the engine roster exactly as before.

**A `zoom:` prefix helper:** add a small local `constexpr` or inline check; do not touch ids of non-zoom bus sources (e.g. a future test source) when removing departed participants — only remove ids beginning `"zoom:"`.

- [ ] **Step 1: Write the failing test**

Add a MediaCore-level test that exercises the real tap. Model it on the existing fake-engine-in-process tests (search `MediaCoreCommandTest.cpp` for how a test configures `zoomEngineRuntime_` / feeds decoded frames; if there is no in-process seam to inject decoded Zoom frames, use the smallest existing harness that drives `renderSyntheticTick` with a configured engine, e.g. the pattern in `ProgramPixelContinuityTest.cpp` or `RenderedSceneAttributionTest.cpp`, which already stand up Zoom-ish frames). The test must assert:
- after a tick, `sessionState().get("sources")` contains an entry with `sourceId` `"zoom:<pid>"` for each fed participant (proves per-participant sources registered), and
- `sessionState().get("programFrame")->get("videoSources")` still lists those participants (proves the bus-produced frames reach the render gather and the roster merge), and
- a participant fed on tick 1 but not tick 2 is GONE from `sources` on tick 2 (proves removal).

If no existing in-process seam feeds decoded Zoom frames to `renderSyntheticTick`, STOP and report NEEDS_CONTEXT — do not invent a private engine; the controller will point at the right harness (this is the one genuinely uncertain step).

- [ ] **Step 2: Run to verify it fails** (the assertions fail because the Zoom frames still come from `RealZoomCaptureSource`, not per-participant bus sources / the `sources` node lacks `zoom:` entries).

- [ ] **Step 3: Implement the tap + gate change** per the Design above.

- [ ] **Step 4: Run to verify PASS**, then the churn/continuity gates:
Run: `native/build-dev/corevideo-native-tests.exe --gtest_filter=ZoomEngineRuntime.*` then `--gtest_filter=SourceContinuityLedger.*` then the new test's filter. All PASS (continuity must NOT read the migrated sources as restarts — same `zoom:<pid>` keys).

- [ ] **Step 5: Commit**

```bash
git add native/src/core/MediaCore.cpp native/src/core/MediaCore.h native/tests/*.cpp
git commit -m "feat(#535): MediaCore feeds per-participant Zoom sources through the bus (slice 1)"
```

---

### Task 4: Regression validation + docs

**Files:**
- Modify: `CLAUDE.md` (extend the "The source bus (#535 slice 0…)" section with a slice-1 note)

- [ ] **Step 1: Full suites green.** Stub gate `pwsh -File scripts/test-native.ps1` (Zoom tap is behind an engine, so the stub path is unaffected — confirm still green) and the full Windows dev suite `native/build-dev/corevideo-native-tests.exe` (~1046+ tests). Confirm `RealZoomCaptureSourceTest.*` still passes (that class remains the no-engine synthetic fallback wrapper).

- [ ] **Step 2: Fake-engine drill (the real ingest path).** `python scripts/mac-show-drill.py --seconds 40 --load 8` — sustained fps / delivery / churn must match the pre-slice baseline (run it on `main` first for the baseline, then on this branch). `node scripts/validate-multiview.mjs` and `node scripts/validate-iso-record.mjs` green (Zoom frames still reach multiview + ISO by `zoom:<pid>`).

- [ ] **Step 3: Document.** In `CLAUDE.md`, under the existing source-bus section, add: slice 1 migrated Zoom **video** to one `ZoomParticipantSource : ISource` per participant, fed by MediaCore's decoded-frame tap and produced through the `SourceBus`; the synthetic slate still serves the no-engine case via `RealZoomCaptureSource`; Zoom **audio** and full deletion of `IZoomCaptureSource` remain for later slices; the cushion/churn/speaker-director stay in `ZoomEngineRuntime`.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md
git commit -m "docs(#535): source bus slice 1 (Zoom video per-participant) note"
```

---

## Self-Review

**Spec coverage** (§4 Zoom row, §5 slice 1):
- "poll() returns the participant's latest I420 slot" → Task 1 `ZoomParticipantSource`.
- "cushion stays inside the adapter" → Global Constraint + Task 3 (cushion untouched in `ZoomEngineRuntime`).
- per-participant granularity (owner ruling) → Tasks 1-3.
- "Zoom onto the bus" video → Task 3; **deliberately NOT** deleting `IZoomCaptureSource` (spec §5 slice 4 retires the three interfaces; slice 1 keeps `RealZoomCaptureSource` for the no-engine slate — stated in Global Constraints and Task 4 docs).
- Zoom audio deferred → Global Constraint (owner-chosen video-only scope).
- Live soak / churn / continuity green → Task 4.

**Placeholder scan:** Task 3 Step 1 is the one genuinely uncertain step (the in-process seam to feed decoded Zoom frames to `renderSyntheticTick`). It is written as "use the smallest existing harness; if none exists, report NEEDS_CONTEXT" rather than inventing an engine — an honest instruction, not a placeholder. No TBD/TODO elsewhere.

**Type consistency:** `ZoomParticipantSource(participantId, w, h)` / `setLatest(VideoFrame)` / `poll`/`counters`; `SourceBus::contains`/`sourceIds`; `sourceBus_` (slice 0). `source_id == "zoom:<pid>" == VideoFrame::participantId` throughout.

**Risk:** Task 3 is the delicate one — it rewires a hot, live-critical path. The safety net is that it changes only WHERE the decoded frame is published (per-participant `ISource` vs `RealZoomCaptureSource`), preserving the `zoom:<pid>` key, the suppression semantics, and the roster merge — so `SourceContinuityLedger` and the churn ledger see identical inputs. The fake-engine drill + continuity/churn tests are the gate that proves it.
