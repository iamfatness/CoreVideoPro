# Source Bus — Slice 3a (media onto the bus, parity migration) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Route media video — decoded clips/loops/backgrounds from `IMediaFrameSource` and route stills from `StillMediaFrameCache` — through the `ISource` / `SourceBus` contract, one source per emitted media frame key, so `sources[]` covers every live kind with framesIngested/health, while frame production, the request/pause semantics, the cue hand-off, the two injection points in the tick, and the on-air result stay byte-for-byte unchanged.

**Architecture:** Media is the one kind whose producer is *request-driven from the render plan every tick*: `OwnedMediaFrameSource::selectVideo(layers, t)` takes the Program+Preview plan layers (order load-bearing: first request for a key wins), decides which decoders exist (its manager admits/joins workers from `requests(layers)`), applies pause/hold (`mediaAssetPlaying`, `clockFrozen`), adopts a cued decoder for the #449 hand-off, and names the frame after the layer's `sourceId` (`media:<id>` or the paused-poster `preview:media:<id>`). That is why media is polled AFTER the plan is built (`MediaCore.cpp` "ORDER IS LOAD-BEARING" block), and why route stills are injected from their cache after the roster merge (they would not survive it). This slice does NOT move either decision: the polls stay exactly where they are; the frames they return are fed into per-key bus sources and produced by a **kind-filtered bus ingest at the same point**. The early ingest (Zoom + capture) excludes media kinds. Dropping the `layers` argument (spec §5 slice 3 wording) is deferred to **slice 3b** (own spec) because `layers` is the play-state channel and the cue hand-off input, and both hang on the open owner ruling on Take semantics (#449) — see "Deferred" at the end.

**Tech Stack:** C++17, MediaCore (`native/`), GoogleTest via the repo's shim (ONE wildcard per `--gtest_filter`), slice 0–2 `SourceBus` (`native/src/core/SourceBus.h`), CMake multi-config dev core.

**Spec:** `docs/superpowers/specs/2026-09-18-source-bus-design.md` (§4 Media row, §5 slice 3) and `docs/superpowers/specs/2026-09-10-persistent-sources-design.md` (§2 model, §3 tier 2 "one decoder per asset"). Slice 2's record (`docs/superpowers/plans/2026-09-19-source-bus-slice2-capture.md`, the CLAUDE.md slice-2 paragraph) is the template; read the #554 paragraph before Task 3.

## Global Constraints

- **Keys unchanged.** `descriptor().sourceId == VideoFrame::participantId` exactly as the producers emit it: `media:<assetId>` (route clip/still, background), `preview:media:<assetId>` (paused clip-cue poster, kept by persistent-sources slice 1), or a custom `layer.sourceId`. Never re-key.
- **Two kinds, two injection points:** kind `"still"` = frames from `StillMediaFrameCache::collectFrames` (injected after the engine-roster merge, before the ISO snapshot); kind `"media"` = frames from `modules_.mediaFrames->pollMediaFramesAt100ns(mediaLayers, …)` (injected after the plan, before `render`). Each is produced by a bus ingest filtered to its kind AT THAT POINT. The early ingest (before the merge) must exclude both kinds or a stale media frame would be emitted before its request update and duplicated later.
- **Video only.** `pollMediaAudioFrames`, the media strip/mixer path, and `syncMediaClock`/`prefetchMediaVideo` are UNTOUCHED.
- **Removal mirrors the producer, like capture.** The media owner emits a frame for every requested key each tick (paused/held keys included — pause holds the on-air frame); a key absent from the poll is no longer requested or has not decoded yet, and today nothing is drawn for it. So `syncMediaSources` removes sources of its kind absent from the tick. The bus never holds a media frame the producer stopped emitting (that would defeat pause/hold/hand-off semantics).
- **Zero-copy, no pixel work under `coreMutex`.** Frames travel as structs with `shared_ptr` payloads.
- **Order parity:** stills keep their position (after merge, before ISO snapshot); clip frames keep theirs (after plan, before render/continuity observe/take record). Within each group the order becomes sourceId-sorted (bus map) — every consumer matches by participantId (the same ruling as slice 2).
- **Regression gates that MUST stay green:** `MediaCoreCommand.*` (esp. `CompositesMediaRoutePixelsIntoProgramPreview`, `KeepsSceneBackgroundAndProgramMediaRouteFrameSourcesDistinct`, `PreviewSceneSingleSourceStillCompositesAndDedups`, `ARouteLoopFlagReachesTheMediaSourceOnBothBuses`, `AppliesMediaPlaybackCommandAndWarnsOnEmptyAsset`), `StillMediaFrameCache.*`, `MediaCueHandoff.*`, `MediaFrameSourceCompatibility.*`, `MediaPlaybackTimeline.*`, `SourceContinuityLedger.*`, `TakeRecordPolicy.*`/take-record tests, `CaptureIngest.*`, `SourceBus.*`, full dev suite 0 failed, stub gate `scripts/test-native.ps1`, `node scripts/validate-multiview.mjs`, `node scripts/validate-show-engine.mjs` (media-driven show), and the recorded gap test `python scripts/qa/zoom-gap-hold-ab.py` (luma holds).
- **Build/run the dev core with `--config Release`** (~2.27 MB exe). Full suite before each commit. Branch: `codex/535-slice3-media` off `origin/main` (10da512 or later). Commits reference `#535`.

---

### Task 1: `SourceBus::ingest` with a kind selector

**Files:**
- Modify: `native/src/core/SourceBus.h` (`ingest`)
- Test: `native/tests/SourceBusTest.cpp` (append)

**Interfaces:**
- Consumes: existing `SourceBus::ingest(int64_t programTime100ns, int64_t nowNs)`.
- Produces: `IngestResult ingest(int64_t programTime100ns, int64_t nowNs, const std::function<bool(const SourceDescriptor&)>& select)` — polls, counts and returns frames ONLY for sources whose descriptor passes `select`; sources not selected are neither polled nor counted (their `everProduced`/counters untouched). The 2-arg overload becomes `ingest(t, now, [](const SourceDescriptor&){ return true; })`.

- [ ] **Step 1: Write the failing test** (append to `native/tests/SourceBusTest.cpp`; `bgraFrame` and `zoomFrame` helpers already exist there)

```cpp
#include <functional>

TEST(SourceBus, IngestWithASelectorPollsAndCountsOnlyTheSelectedKinds) {
  corevideo::core::SourceBus bus;
  bus.add(std::make_shared<corevideo::core::TestPatternSource>("test:pattern"));          // kind "test"
  auto cam = std::make_shared<corevideo::core::CaptureDeviceSource>("capture:cam", 640, 360);  // kind "capture"
  cam->setLatest(bgraFrame("capture:cam", 640, 360, 1));
  bus.add(cam);

  const auto onlyCapture = [](const corevideo::core::SourceDescriptor& d) { return d.kind == "capture"; };
  auto r = bus.ingest(0, 1000, onlyCapture);
  ASSERT_EQ(r.video.size(), 1u);
  EXPECT_EQ(r.video[0].participantId, "capture:cam");

  // The unselected source was neither polled nor counted.
  auto snap = bus.snapshot(1000);
  for (const auto& s : snap) {
    if (s.descriptor.sourceId == "test:pattern") {
      EXPECT_EQ(s.counters.framesIngested, 0u);
      EXPECT_EQ(s.health, corevideo::core::SourceHealth::Warming);
    }
    if (s.descriptor.sourceId == "capture:cam") EXPECT_EQ(s.counters.framesIngested, 1u);
  }

  // The 2-arg overload still ingests everything.
  auto all = bus.ingest(0, 2000);
  EXPECT_EQ(all.video.size(), 2u);
}
```

- [ ] **Step 2: Run to verify it fails.** Build tests → compile error (no 3-arg `ingest`).

- [ ] **Step 3: Implement** in `SourceBus.h`:

```cpp
#include <functional>
// ...
  IngestResult ingest(int64_t programTime100ns, int64_t nowNs) {
    return ingest(programTime100ns, nowNs, [](const SourceDescriptor&) { return true; });
  }
  // Kind-selected ingest: MediaCore runs the bus at more than one point in the
  // tick (Zoom+capture before the roster merge; stills after it; decoded media
  // after the plan, because the media owner's request set IS the plan). A source
  // not selected is neither polled nor counted this call.
  IngestResult ingest(int64_t programTime100ns, int64_t nowNs,
                      const std::function<bool(const SourceDescriptor&)>& select) {
    IngestResult out;
    for (auto& [id, e] : entries_) {
      if (!select(e.source->descriptor())) continue;
      SourceTick tick = e.source->poll(programTime100ns);
      // ... (existing body unchanged: isNew accounting, push frames/audio)
    }
    return out;
  }
```
Move the existing loop body into the 3-arg overload verbatim; do not change the counting rules.

- [ ] **Step 4: Run** `--gtest_filter=*IngestWithASelector*` → PASS; `*SourceBus*`, `*SourceBusSnapshot*`, `*CaptureBusRoster*`, `*ZoomBusRoster*` → PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/SourceBus.h native/tests/SourceBusTest.cpp
git commit -m "feat(#535): SourceBus::ingest with a kind selector (slice 3a)"
```

---

### Task 2: `MediaAssetSource` + pure `syncMediaSources` helper

**Files:**
- Create: `native/src/core/MediaAssetSource.h` (header-only; mirrors `CaptureDeviceSource.h`)
- Create: `native/src/core/MediaBusRoster.h` (header-only; mirrors `CaptureBusRoster.h`)
- Test: `native/tests/SourceBusTest.cpp` (append)

**Interfaces:**
- Produces:
  - `class core::MediaAssetSource final : public core::ISource` — ctor `MediaAssetSource(std::string sourceId, std::string kind, int width, int height)`; `kind` is `"media"` or `"still"`; descriptor `pixelFormat = "bgra"`, `hasVideo = true`. `setLatest(modules::VideoFrame)`, `poll(int64_t)` (latest + `Producing`, else empty + `Warming`), `counters()` (diagnostic-only, same comment as the other sources).
  - `inline void core::syncMediaSources(SourceBus& bus, const std::vector<modules::VideoFrame>& frames, std::string_view kind)` — for each frame: add a `MediaAssetSource(frame.participantId, kind, w, h)` if absent (w/h from `pixelWidth/pixelHeight`, fallback `i420Width/i420Height`), then `setLatest` (only if the existing source's `descriptor().kind == kind`; a foreign kind under the same id is skipped with the same comment as slice 2). Then remove every bus source whose `descriptor().kind == kind` and whose id is not in this call's frames. Never touch any other kind.

- [ ] **Step 1: Write the failing tests**

```cpp
#include "core/MediaAssetSource.h"
#include "core/MediaBusRoster.h"

TEST(MediaAssetSource, PollServesTheOwnersLatestFrameKeyedByAsset) {
  corevideo::core::MediaAssetSource src("media:logo-1", "still", 1920, 1080);
  EXPECT_EQ(src.descriptor().sourceId, "media:logo-1");
  EXPECT_EQ(src.descriptor().kind, "still");
  EXPECT_TRUE(src.descriptor().hasVideo);
  EXPECT_EQ(src.poll(0).health, corevideo::core::SourceHealth::Warming);
  src.setLatest(bgraFrame("media:logo-1", 1920, 1080, 3));
  auto t = src.poll(0);
  ASSERT_EQ(t.video.size(), 1u);
  EXPECT_EQ(t.video[0].frameId, 3);
  EXPECT_EQ(t.health, corevideo::core::SourceHealth::Producing);
}

// The media owner emits a frame for every REQUESTED key each tick (a paused
// clip keeps emitting its held frame); a key absent from the poll is no longer
// requested or has not decoded, and nothing is drawn for it today. So, like
// capture and unlike Zoom (#554), absent from the tick == removed. The two
// media kinds are independent: syncing "media" never touches "still" sources.
TEST(MediaBusRoster, MirrorsTheOwnersTickPerKindAndNeverTouchesOtherKinds) {
  corevideo::core::SourceBus bus;
  bus.add(std::make_shared<corevideo::core::ZoomParticipantSource>("16778240", 1280, 720));
  corevideo::core::syncMediaSources(bus, {bgraFrame("media:logo-1", 800, 200, 1)}, "still");
  corevideo::core::syncMediaSources(bus, {bgraFrame("media:clip-1", 1920, 1080, 1),
                                          bgraFrame("preview:media:clip-2", 1920, 1080, 1)}, "media");
  EXPECT_EQ(bus.sourceFor("media:logo-1")->descriptor().kind, "still");
  EXPECT_EQ(bus.sourceFor("media:clip-1")->descriptor().kind, "media");
  EXPECT_TRUE(bus.contains("preview:media:clip-2"));

  // clip-2's poster is no longer requested; the still and the Zoom source survive
  // a "media"-kind sync that omits them.
  corevideo::core::syncMediaSources(bus, {bgraFrame("media:clip-1", 1920, 1080, 2)}, "media");
  EXPECT_FALSE(bus.contains("preview:media:clip-2"));
  EXPECT_TRUE(bus.contains("media:clip-1"));
  EXPECT_TRUE(bus.contains("media:logo-1"));
  EXPECT_TRUE(bus.contains("16778240"));

  // An empty "still" tick removes stills only.
  corevideo::core::syncMediaSources(bus, {}, "still");
  EXPECT_FALSE(bus.contains("media:logo-1"));
  EXPECT_TRUE(bus.contains("media:clip-1"));
  EXPECT_TRUE(bus.contains("16778240"));

  // Kind-selected ingest yields exactly that kind.
  auto onlyMedia = bus.ingest(0, 1000, [](const corevideo::core::SourceDescriptor& d) { return d.kind == "media"; });
  ASSERT_EQ(onlyMedia.video.size(), 1u);
  EXPECT_EQ(onlyMedia.video[0].participantId, "media:clip-1");
}
```

- [ ] **Step 2: Run to verify it fails.** Compile error (headers missing).

- [ ] **Step 3: Write the two headers** (copy `CaptureDeviceSource.h` / `CaptureBusRoster.h` and adapt: ctor takes `kind`; the helper takes `std::string_view kind`, uses it for the add, the guard, and the removal filter; comments state the removal rule above and that `kind` is `"media"` (decoded clips/loops/backgrounds from `IMediaFrameSource`) or `"still"` (route stills from `StillMediaFrameCache`)).

- [ ] **Step 4: Run** `*MediaAssetSource*`, `*MediaBusRoster*`, `*SourceBus*`, `*CaptureBusRoster*`, `*ZoomBusRoster*` → PASS.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/MediaAssetSource.h native/src/core/MediaBusRoster.h native/tests/SourceBusTest.cpp
git commit -m "feat(#535): MediaAssetSource + syncMediaSources per kind (slice 3a)"
```

---

### Task 3: MediaCore — media and stills produced by kind-filtered ingests at their existing points

**Files:**
- Modify: `native/src/core/MediaCore.cpp` (`renderSyntheticTick`: the early bus ingest; the still injection after the merge; the media poll after the plan)
- Modify: `native/src/core/MediaCore.h` (include `core/MediaBusRoster.h`)
- Test: `native/tests/MediaCoreCommandTest.cpp` (append)

**Design (three edits, nothing else moves):**
1. **Early ingest** (the slice-2 block that partitions `busResult.video` into `captureFrames`/`zoomBusFrames`): call `sourceBus_->ingest(t, now, [](const SourceDescriptor& d){ return d.kind != "media" && d.kind != "still"; })` instead of the 2-arg overload. Everything else in that block unchanged. (The `!sourceBus_->empty()` guard stays.)
2. **Stills** (currently: `auto stillFrames = stillMediaCache_->collectFrames(ts); videoFrames.insert(... stillFrames ...)`): becomes
```cpp
if (stillMediaCache_) {
  auto stillFrames = stillMediaCache_->collectFrames(frameTimestampMs);
  core::syncMediaSources(*sourceBus_, stillFrames, "still");
  auto stills = sourceBus_->ingest(mediaPresentationTime100ns, nowNs,
                                   [](const core::SourceDescriptor& d) { return d.kind == "still"; });
  videoFrames.insert(videoFrames.end(), std::make_move_iterator(stills.video.begin()),
                     std::make_move_iterator(stills.video.end()));
}
```
(`nowNs` — reuse the tick's steady-clock nanoseconds; compute it once near the early ingest and keep it in scope.)
3. **Decoded media** (currently: `auto mediaFrames = modules_.mediaFrames->pollMediaFramesAt100ns(mediaLayers, mediaPresentationTime100ns); videoFrames.insert(... mediaFrames ...)`): becomes
```cpp
auto polledMedia = modules_.mediaFrames->pollMediaFramesAt100ns(mediaLayers, mediaPresentationTime100ns);
core::syncMediaSources(*sourceBus_, polledMedia, "media");
auto media = sourceBus_->ingest(mediaPresentationTime100ns, nowNs,
                                [](const core::SourceDescriptor& d) { return d.kind == "media"; });
videoFrames.insert(videoFrames.end(), std::make_move_iterator(media.video.begin()),
                   std::make_move_iterator(media.video.end()));
```
The `mediaLayers` construction (Program first, Preview appended), the warnings merge, `syncMediaClock`, and everything after are unchanged. When `modules_.mediaFrames` is null the block is skipped as today — then also run `syncMediaSources(*sourceBus_, {}, "media")` in an `else` so stale "media" sources cannot linger if the module disappears (it cannot today; one line, loud-not-silent).

- [ ] **Step 1: Write the failing tests** (append to `native/tests/MediaCoreCommandTest.cpp`; reuse the `SolidMediaFrameSource` fake at the top of that file and the setup of `CompositesMediaRoutePixelsIntoProgramPreview` — a scene with a media route whose asset the fake serves)

```cpp
// #535 slice 3a: a decoded media route appears on the bus as kind "media" after a
// tick, and a route still as kind "still"; a route that is removed disappears
// from the bus on the next tick (the owner stops emitting it).
TEST(MediaCoreCommand, MediaRouteAppearsOnTheSourceBusAndLeavesWhenUnrouted) {
  // Setup: copy the module/scene setup of CompositesMediaRoutePixelsIntoProgramPreview
  // (SolidMediaFrameSource + load-scene-graph with one media route, asset id "clip-1").
  // ... two renderDisplayTick() ...
  auto state = mediaCore.sessionState();
  bool found = false;
  for (const auto& s : state.get("sources")->asArray()) {
    if (s.getString("sourceId") == "media:clip-1") {
      found = true;
      EXPECT_EQ(s.getString("kind"), "media");
      EXPECT_EQ(s.getString("health"), "producing");
      EXPECT_GE(s.get("framesIngested")->asNumber(), 1.0);
    }
  }
  EXPECT_TRUE(found);
  // Program still composites the media pixels (the existing assertion from
  // CompositesMediaRoutePixelsIntoProgramPreview, repeated here on the bus path).
  // ... same preview pixel probe as that test ...

  // Unroute: load a scene graph with no media route; two ticks; the source is gone.
  // ... load-scene-graph with routes = [] (or a capture route) ...
  state = mediaCore.sessionState();
  for (const auto& s : state.get("sources")->asArray()) EXPECT_NE(s.getString("sourceId"), "media:clip-1");
}
```
Write the still variant only if the stub still decoder seam (`setStillImageDecoderForTest`, used by `StillMediaFrameCache` tests / `PreviewSceneSingleSourceStillCompositesAndDedups`) is reachable from `MediaCoreCommandTest` without new fixtures; otherwise assert the still path through the existing `PreviewSceneSingleSourceStillCompositesAndDedups` staying green plus a `sources[]` check inside that test (kind `"still"` present) — add that check there rather than inventing a fixture.

- [ ] **Step 2: Run to verify it fails.** `--gtest_filter=*MediaRouteAppearsOnTheSourceBus*` → FAIL (no media source on the bus).

- [ ] **Step 3: Implement** the three edits; add the include.

- [ ] **Step 4: Run the gates, each PASS:** `*MediaRouteAppearsOnTheSourceBus*`, `*CompositesMediaRoutePixels*`, `*KeepsSceneBackgroundAndProgramMediaRoute*`, `*PreviewSceneSingleSourceStill*`, `*ARouteLoopFlagReaches*`, `*MediaCoreCommand*`, `*StillMediaFrameCache*`, `*MediaCueHandoff*`, `*MediaFrameSourceCompatibility*`, `*SourceContinuityLedger*`, `*TakeRecord*`, `*CaptureIngest*`, `*CaptureBusFramesGather*`, then the full suite (0 failed; expect ≥1063 passed).

- [ ] **Step 5: Commit**

```bash
git add native/src/core/MediaCore.cpp native/src/core/MediaCore.h native/tests/MediaCoreCommandTest.cpp
git commit -m "feat(#535): MediaCore produces media and stills from kind-filtered bus ingests (slice 3a)"
```

---

### Task 4: Regression validation + docs

**Files:**
- Modify: `CLAUDE.md` (source-bus section: slice 3a paragraph; fix any remaining "bus is empty" phrasing), `docs/BACKLOG.md` (#535 row), `docs/superpowers/specs/2026-09-18-source-bus-design.md` (§5: split slice 3 into 3a shipped / 3b deferred with the reason)

- [ ] **Step 1: Suites.** Full Windows dev suite (0 failed); stub gate `powershell -ExecutionPolicy Bypass -File scripts/test-native.ps1` green.
- [ ] **Step 2: Real-path gates.** `node scripts/validate-multiview.mjs` PASS; `node scripts/validate-show-engine.mjs` (or its documented headless invocation — read its header; it drives a media-backed show) PASS; `python scripts/qa/zoom-gap-hold-ab.py --core <abs path>/native/build-dev/corevideo-native.exe --fake <abs path>/native/build-dev/corevideo-zoom-engine-fake.exe --label slice3` (absolute paths — relative paths fail CreateProcess on this box) then the `ffmpeg signalstats` YAVG summary: luma holds (~188) through the dropout, no dip; `set COREVIDEO_FAKE_ENGINE_FPS=60` + `python scripts/mac-show-drill.py --seconds 40 --load 8` within the slice-1 table. Archive harness outputs under `artifacts/qa/slice3-gap-hold/` (not committed).
- [ ] **Step 3: Document.** CLAUDE.md slice-3a paragraph: what moved (frames onto the bus at the two existing points via kind-filtered ingest), what did NOT (request set from plan layers, pause/hold, cue hand-off, `preview:` poster key, media audio, still cache), the removal rule and why (producer-mirroring, like capture), the order note, the `sources[]` coverage (every live kind now: zoom/capture/still/media), the Task 4 numbers, and the pointer to slice 3b. Spec §5: "Slice 3a (2026-09-19) — media frames onto the bus, parity. Slice 3b — `layers` dropped: media request state moves from per-tick plan layers to source state set at command time; cue hand-off (#449) lives inside the source; needs the owner's Take-semantics ruling; own spec." BACKLOG `#535` row: slice 3a ready on branch (unmerged), slice 3b + slice 4 next.
- [ ] **Step 4: Commit** `docs(#535): source bus slice 3a (media, parity) note + 3b deferral`.

---

## Self-Review

**Spec coverage** (§4 Media row, §5 slice 3): "`poll(ts)` returns the asset's one decoder frame" → Task 2/3 (the frame the owner selected for `ts`, via the bus). "`layers` argument dropped" → **deliberately deferred to 3b** with the reason recorded in the spec (Task 4); the plan's Architecture states why it cannot be dropped without moving play-state/hand-off into the source. "Rides the persistent-sources media work" → uses `OwnedMediaFrameSource` as the producer unchanged. Video only, stills + clips both covered, gates in Task 4.

**Placeholder scan:** Task 3 Step 1 points at an existing test's setup by name and keeps its assertions; the still variant has an explicit fallback. No TBD.

**Type consistency:** `ingest(t, now, select)` (Task 1) used identically in Tasks 2, 3; `MediaAssetSource(sourceId, kind, w, h)`; `syncMediaSources(bus, frames, kind)` with `"media"`/`"still"` literals everywhere.

**Risk:** Task 3 touches two hot points, but neither the producer nor the consumer changes — only the vector the frames travel in, at the same point, in the same tick. The recorded gap gate, the media composite tests, the continuity/take-record tests, and the show-engine validation are the net.

## Deferred (slice 3b, own spec — do not start without the owner)
- Drop `layers` from the media producer: media request state (asset, playing, loop, which bus) becomes source state set at command time (`load-scene-graph`/`set-preview-scene`/`set-media-playback`), `poll(ts)` applies hold/roll from that state, the cue→Program hand-off (#449) and the `preview:` poster key move inside the source, the still cache becomes the still source's decoder. Blocked on the owner's Take-semantics ruling (#449 step 1 "hold outgoing picture on a plain cut") because go-live/roll-from-0 and hand-off are the same decision.
