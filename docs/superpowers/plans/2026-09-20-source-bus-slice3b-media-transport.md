# Source Bus — Slice 3b (media transport lives in the source) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A media route or background is ONE core source that owns its decoder, clock and transport state (cued / live / paused / ended); the core sets that state from the two scene graphs and an operator transport command; the render plan no longer feeds the decoder; the shell's playback keys, go-live generations, `preview:` poster namespace and cue hand-off are deleted.

**Architecture:** `core::MediaTransports` (the evolved `OwnedMediaFrameSource`: same manager thread, per-entry worker, 16-decoder cap, audio demand clocks) is fed a **desired set** computed by `MediaCore` at the three command sites (`loadSceneGraph`, `applyPreviewScene`, the new `set-media-transport`) — the same shape as `syncStillMediaDesired`. A pure `MediaTransportPolicy.h` decides every transition. Each entry is wrapped by a `MediaAssetSource` on the `SourceBus`, so media video rides the ordinary early bus ingest and media audio is popped from the entries on the audio worker. `IMediaFrameSource`/`IMediaVideoPrefetch` survive only as the per-entry DECODER contract (retired in slice 4b); `ModuleSet::mediaFrames` becomes `ModuleSet::mediaDecoderFactory`.

**Tech Stack:** C++17 (`native/`), GoogleTest via the repo shim (ONE wildcard per `--gtest_filter`), .NET 9 / xUnit shell (`native-shell/`), Python QA oracle + ffmpeg signalstats.

**Spec:** `docs/superpowers/specs/2026-09-20-source-bus-slice3b-media-source-state-design.md` (binding). Parent: `docs/superpowers/specs/2026-09-18-source-bus-design.md` §5. Read the CLAUDE.md sections "Media is a persistent source (slice 1)" and "The source bus" → "Slice 3a" before Task 2.

## Global Constraints

- **Owner rulings (spec §0):** a clip restarts from 0 on the cut; Preview shows the SAME rolling picture as Program for a shared clip; the core owns play/pause, driven by Take; loops/backgrounds never pause and never restart on a Take.
- **Transition table = spec §3.2, verbatim.** Enters Program = present in the current Program desired set and absent from the PREVIOUS Program desired set held by `MediaCore` — never a generation string, never a clock.
- **Source ids unchanged:** `media:<assetId>` (route), `background:<assetId>` (scene background); `descriptor().sourceId == VideoFrame::participantId`. The `preview:` namespace is DELETED (no code may emit or match it).
- **Zero-copy, no pixel work or decoder I/O under `coreMutex`.** `MediaTransports::apply` starts threads and sets flags only; decoder open/restart happens on the entry's worker.
- **Stills untouched:** `StillMediaFrameCache`, `syncStillMediaDesired`, kind `"still"` and its post-merge ingest stay exactly as slice 3a left them.
- **Wire tolerance:** the core IGNORES `mediaPlaybackKey`/`mediaAssetPlaying` on routes, `playing` on backgrounds, and `mediaPlaybackKey`/`playing` on `set-media-playback` (never a warning, never a refusal).
- **Snapshot nodes are published unconditionally** (`mediaSources: []` when none — the multiviewer-node rule).
- **Shell 0xc000027b rule:** snapshot-driven bin state updates per-row SCALARS; never replace `MediaBinGroups` at snapshot rate.
- **Test discipline:** every test that used to inject a synchronous `IMediaFrameSource` into `modules.mediaFrames` now injects a FACTORY and pumps with `renderUntil` (Task 3) — frames are produced by a worker thread, exactly as in production. No inline-decode test mode exists.
- **Gates that must stay green:** `MediaCoreCommand.*`, `StillMediaFrameCache.*`, `MediaPlaybackTimeline.*`, `ProgramPixelContinuity.*`, `TakeRecord*`, `SourceContinuityLedger.*`, `SourceBus*`, `ProgramFramePreviewHealth.*`, `D3D11Compositor*`, full Windows dev suite 0 failed, stub gate `scripts/test-native.ps1`, `dotnet test` (MediaCore.Tests, Control.Tests, WinUI.Tests), `node scripts/validate-multiview.mjs`, `node scripts/validate-tiles.mjs`, `python scripts/qa/zoom-gap-hold-ab.py` (luma holds), show drill `--load 8` with `COREVIDEO_FAKE_ENGINE_FPS=60`, and the new `scripts/qa/media-take-ab.py`.
- **Build the dev core with `--config Release`** (`cmake --build native/build-dev --config Release --target corevideo-native corevideo-native-tests`; exe ≈ 2.28 MB). Changing `Interfaces.h` (Tasks 3/4) → rebuild `--clean-first` before believing any crash. Branch: `codex/535-slice3b-media-transport` (already holds the spec commit). Commits reference `#535` and `#449`.

---

### Task 1: `MediaTransportPolicy.h` — the pure transition table

**Files:**
- Create: `native/src/core/MediaTransportPolicy.h`
- Create: `native/tests/MediaTransportPolicyTest.cpp`
- Modify: `native/tests/CMakeLists.txt` (add the test TU next to `SourceBusTest.cpp`)

**Interfaces:**
- Produces (used by Tasks 2–4):

```cpp
namespace corevideo::core {
struct MediaTransportDesired {
  std::string sourceId;   // "media:<assetId>" | "background:<assetId>"
  std::string assetId, path, kind;
  bool loop = false;      // route.mediaAssetLoop; backgrounds always true
  bool onProgram = false, onPreview = false;
};
enum class MediaTransportState { Cued, Live, Paused, Ended };
enum class MediaTransportAction {
  None,         // nothing to do
  OpenLive,     // open a decoder, roll from 0, audio on            -> Live
  OpenCued,     // open a decoder, poster at 0, no audio            -> Cued
  Resume,       // the cued decoder rolls from 0, audio on          -> Live
  RestartCued,  // new decoder at 0 behind the held frame, no audio -> Cued
  Reopen,       // path changed: release + OpenLive/OpenCued        -> Live|Cued
  Pause,        // operator pause                                   -> Paused
  Play,         // operator play from the frozen position           -> Live
  Release       // absent from both buses: retire decoder + source
};
enum class MediaOperatorAction { Pause, Play };
const char* mediaTransportStateName(MediaTransportState s);   // "cued"|"live"|"paused"|"ended"
// Scene-driven decision. `previous`/`current` are the entry's desired rows before and after
// the command (nullopt = not desired). `state` is the entry's current state (ignored when
// previous is nullopt). Returns the action and the state that results from it.
struct MediaTransportDecision { MediaTransportAction action; MediaTransportState next; };
MediaTransportDecision decideMediaTransport(const std::optional<MediaTransportDesired>& previous,
                                            const std::optional<MediaTransportDesired>& current,
                                            MediaTransportState state);
// Operator pause/play. Returns nullopt with `reason` filled when refused (loop, or not on Program).
std::optional<MediaTransportDecision> decideMediaOperator(const MediaTransportDesired& current,
                                                          MediaTransportState state,
                                                          MediaOperatorAction action,
                                                          std::string& reason);
}
```

- [ ] **Step 1: Write the failing tests** — `native/tests/MediaTransportPolicyTest.cpp`:

```cpp
#include "core/MediaTransportPolicy.h"
#include <gtest/gtest.h>
using namespace corevideo::core;
namespace {
MediaTransportDesired clip(bool onProgram, bool onPreview, const char* path = "C:/m/clip.mp4") {
  MediaTransportDesired d; d.sourceId = "media:clip"; d.assetId = "clip"; d.path = path;
  d.kind = "video"; d.loop = false; d.onProgram = onProgram; d.onPreview = onPreview; return d;
}
MediaTransportDesired loop(bool onProgram, bool onPreview) {
  auto d = clip(onProgram, onPreview, "C:/m/bg.mp4"); d.sourceId = "background:bg"; d.assetId = "bg";
  d.kind = "background"; d.loop = true; return d;
}
}
TEST(MediaTransportPolicy, ANewSourceOnProgramOpensLive) {
  const auto d = decideMediaTransport(std::nullopt, clip(true, false), MediaTransportState::Cued);
  EXPECT_EQ(d.action, MediaTransportAction::OpenLive); EXPECT_EQ(d.next, MediaTransportState::Live);
}
TEST(MediaTransportPolicy, ANewSourceOnPreviewOnlyOpensCued) {
  const auto d = decideMediaTransport(std::nullopt, clip(false, true), MediaTransportState::Cued);
  EXPECT_EQ(d.action, MediaTransportAction::OpenCued); EXPECT_EQ(d.next, MediaTransportState::Cued);
}
TEST(MediaTransportPolicy, ACuedClipEnteringProgramResumesTheSameDecoder) {
  const auto d = decideMediaTransport(clip(false, true), clip(true, true), MediaTransportState::Cued);
  EXPECT_EQ(d.action, MediaTransportAction::Resume); EXPECT_EQ(d.next, MediaTransportState::Live);
}
TEST(MediaTransportPolicy, ALiveClipLeavingProgramButStillCuedRestartsAtZero) {
  for (auto s : {MediaTransportState::Live, MediaTransportState::Paused, MediaTransportState::Ended}) {
    const auto d = decideMediaTransport(clip(true, false), clip(false, true), s);
    EXPECT_EQ(d.action, MediaTransportAction::RestartCued); EXPECT_EQ(d.next, MediaTransportState::Cued);
  }
}
TEST(MediaTransportPolicy, ALiveClipStayingOnProgramAcrossATakeIsLeftAlone) {
  const auto d = decideMediaTransport(clip(true, false), clip(true, true), MediaTransportState::Live);
  EXPECT_EQ(d.action, MediaTransportAction::None); EXPECT_EQ(d.next, MediaTransportState::Live);
  const auto p = decideMediaTransport(clip(true, true), clip(true, false), MediaTransportState::Paused);
  EXPECT_EQ(p.action, MediaTransportAction::None); EXPECT_EQ(p.next, MediaTransportState::Paused);
}
TEST(MediaTransportPolicy, AnIdenticalDesiredRowIsANoOp) {
  const auto d = decideMediaTransport(clip(true, true), clip(true, true), MediaTransportState::Live);
  EXPECT_EQ(d.action, MediaTransportAction::None);
  const auto c = decideMediaTransport(clip(false, true), clip(false, true), MediaTransportState::Cued);
  EXPECT_EQ(c.action, MediaTransportAction::None); EXPECT_EQ(c.next, MediaTransportState::Cued);
}
TEST(MediaTransportPolicy, AbsentFromBothBusesReleases) {
  const auto d = decideMediaTransport(clip(true, true), std::nullopt, MediaTransportState::Live);
  EXPECT_EQ(d.action, MediaTransportAction::Release);
}
TEST(MediaTransportPolicy, APathChangeReopens) {
  const auto d = decideMediaTransport(clip(true, false), clip(true, false, "C:/m/other.mp4"), MediaTransportState::Live);
  EXPECT_EQ(d.action, MediaTransportAction::Reopen); EXPECT_EQ(d.next, MediaTransportState::Live);
  const auto c = decideMediaTransport(clip(false, true), clip(false, true, "C:/m/other.mp4"), MediaTransportState::Cued);
  EXPECT_EQ(c.action, MediaTransportAction::Reopen); EXPECT_EQ(c.next, MediaTransportState::Cued);
}
TEST(MediaTransportPolicy, ALoopIsLiveOnAnyBusAndNeverRestartsOnATake) {
  EXPECT_EQ(decideMediaTransport(std::nullopt, loop(false, true), MediaTransportState::Cued).action, MediaTransportAction::OpenLive);
  EXPECT_EQ(decideMediaTransport(loop(false, true), loop(true, true), MediaTransportState::Live).action, MediaTransportAction::None);
  EXPECT_EQ(decideMediaTransport(loop(true, true), loop(false, true), MediaTransportState::Live).action, MediaTransportAction::None);
}
TEST(MediaTransportPolicy, OperatorPauseAndPlayOnlyOnALiveProgramClip) {
  std::string reason;
  auto d = decideMediaOperator(clip(true, false), MediaTransportState::Live, MediaOperatorAction::Pause, reason);
  ASSERT_TRUE(d); EXPECT_EQ(d->action, MediaTransportAction::Pause); EXPECT_EQ(d->next, MediaTransportState::Paused);
  d = decideMediaOperator(clip(true, false), MediaTransportState::Paused, MediaOperatorAction::Play, reason);
  ASSERT_TRUE(d); EXPECT_EQ(d->action, MediaTransportAction::Play); EXPECT_EQ(d->next, MediaTransportState::Live);
  EXPECT_FALSE(decideMediaOperator(loop(true, false), MediaTransportState::Live, MediaOperatorAction::Pause, reason));
  EXPECT_NE(reason.find("loop"), std::string::npos);
  EXPECT_FALSE(decideMediaOperator(clip(false, true), MediaTransportState::Cued, MediaOperatorAction::Pause, reason));
  EXPECT_NE(reason.find("Program"), std::string::npos);
  // Pausing an already paused clip / playing a live one is a no-op, not a refusal.
  d = decideMediaOperator(clip(true, false), MediaTransportState::Paused, MediaOperatorAction::Pause, reason);
  ASSERT_TRUE(d); EXPECT_EQ(d->action, MediaTransportAction::None);
  // Play on an ENDED clip restarts it from 0 (the only way an operator re-rolls a finished clip on air).
  d = decideMediaOperator(clip(true, false), MediaTransportState::Ended, MediaOperatorAction::Play, reason);
  ASSERT_TRUE(d); EXPECT_EQ(d->action, MediaTransportAction::OpenLive); EXPECT_EQ(d->next, MediaTransportState::Live);
}
TEST(MediaTransportPolicy, StateNamesAreTheWireVocabulary) {
  EXPECT_STREQ(mediaTransportStateName(MediaTransportState::Cued), "cued");
  EXPECT_STREQ(mediaTransportStateName(MediaTransportState::Live), "live");
  EXPECT_STREQ(mediaTransportStateName(MediaTransportState::Paused), "paused");
  EXPECT_STREQ(mediaTransportStateName(MediaTransportState::Ended), "ended");
}
```

- [ ] **Step 2: Add the TU to `native/tests/CMakeLists.txt`** (next to `SourceBusTest.cpp`), build `--config Release`, run `corevideo-native-tests.exe --gtest_filter=MediaTransportPolicy.*` → expect compile failure (header missing).

- [ ] **Step 3: Implement `native/src/core/MediaTransportPolicy.h`** (header-only, no includes beyond `<optional>`, `<string>`):

```cpp
#pragma once
#include <optional>
#include <string>
namespace corevideo::core {
// ... the structs/enums from the Interfaces block above, verbatim ...
inline const char* mediaTransportStateName(MediaTransportState s) {
  switch (s) { case MediaTransportState::Cued: return "cued"; case MediaTransportState::Live: return "live";
               case MediaTransportState::Paused: return "paused"; default: return "ended"; }
}
inline MediaTransportDecision decideMediaTransport(const std::optional<MediaTransportDesired>& previous,
                                                   const std::optional<MediaTransportDesired>& current,
                                                   MediaTransportState state) {
  using A = MediaTransportAction; using S = MediaTransportState;
  if (!current) return {previous ? A::Release : A::None, state};
  const bool wantsLive = current->onProgram || current->loop;   // a loop is live on any bus
  if (!previous) return wantsLive ? MediaTransportDecision{A::OpenLive, S::Live} : MediaTransportDecision{A::OpenCued, S::Cued};
  if (previous->path != current->path || previous->loop != current->loop)
    return wantsLive ? MediaTransportDecision{A::Reopen, S::Live} : MediaTransportDecision{A::Reopen, S::Cued};
  if (current->loop) return {A::None, S::Live};
  const bool wasOnProgram = previous->onProgram;
  if (!wasOnProgram && current->onProgram) return {A::Resume, S::Live};          // enters Program
  if (wasOnProgram && !current->onProgram) return {A::RestartCued, S::Cued};     // leaves Program, still cued
  return {A::None, state};                                                        // same bus membership
}
inline std::optional<MediaTransportDecision> decideMediaOperator(const MediaTransportDesired& current,
    MediaTransportState state, MediaOperatorAction action, std::string& reason) {
  using A = MediaTransportAction; using S = MediaTransportState;
  if (current.loop) { reason = current.sourceId + " is a loop and cannot be paused or played."; return std::nullopt; }
  if (!current.onProgram) { reason = current.sourceId + " is not on Program; pause/play applies to the Program clip only."; return std::nullopt; }
  if (action == MediaOperatorAction::Pause) {
    if (state == S::Live) return MediaTransportDecision{A::Pause, S::Paused};
    return MediaTransportDecision{A::None, state};
  }
  if (state == S::Paused) return MediaTransportDecision{A::Play, S::Live};
  if (state == S::Ended) return MediaTransportDecision{A::OpenLive, S::Live};
  return MediaTransportDecision{A::None, state};
}
}  // namespace corevideo::core
```

- [ ] **Step 4: Run** `--gtest_filter=MediaTransportPolicy.*` → all pass.
- [ ] **Step 5: Commit** `git add native/src/core/MediaTransportPolicy.h native/tests/MediaTransportPolicyTest.cpp native/tests/CMakeLists.txt && git commit -m "core: MediaTransportPolicy — the pure media transport transition table (#535 3b, #449)"`.

---

### Task 2: `core::MediaTransports` — the owner, evolved from `OwnedMediaFrameSource`

**Files:**
- Create: `native/src/core/MediaTransports.h` (moved + evolved from `native/src/modules/OwnedMediaFrameSource.h`)
- Delete: `native/src/modules/OwnedMediaFrameSource.h`, `native/src/modules/MediaCueHandoff.h`, `native/tests/MediaCueHandoffTest.cpp` (remove from `native/tests/CMakeLists.txt`)
- Modify: `native/src/modules/MediaFoundationMediaFrameSourceAdapter.cpp` (identity drops the key; factory export), `native/src/modules/Interfaces.h` (`ModuleSet::mediaDecoderFactory`), `native/src/modules/StubModules.cpp`
- Modify: `native/tests/MediaPlaybackTimelineTest.cpp` (port the `OwnedMediaFrameSource.*` tests → `MediaTransports.*`)

**Interfaces:**
- Consumes: Task 1's policy types; `IMediaFrameSource`/`IMediaVideoPrefetch` as the decoder contract (unchanged).
- Produces:

```cpp
namespace corevideo::core {
class MediaTransports final {
 public:
  using DecoderFactory = std::function<std::unique_ptr<modules::IMediaFrameSource>()>;
  struct Entry;                                   // opaque outside the header; shared_ptr held by MediaAssetSource
  struct Change { std::string sourceId; bool added; std::shared_ptr<Entry> entry; };   // added==false -> removed
  struct Status { std::string sourceId, assetId; MediaTransportState state; bool loop, onProgram, onPreview;
                  int64_t positionMs, durationMs; };
  explicit MediaTransports(DecoderFactory factory);
  ~MediaTransports();
  // Command-time desired set (Program + Preview). Applies decideMediaTransport per source id,
  // starts/stops workers (never opens a decoder inline), returns the bus membership changes.
  std::vector<Change> apply(const std::vector<MediaTransportDesired>& desired, int64_t nowNs);
  // Operator pause/play on `media:<assetId>`; false + reason when refused.
  bool operatorAction(const std::string& assetId, MediaOperatorAction action, std::string& reason);
  // Render tick (under coreMutex, cheap): the frame due at ts for one entry, or the held frame.
  static std::optional<modules::VideoFrame> selectVideo(Entry& entry, int64_t timestamp100ns);
  // Audio worker (under coreMutex, cheap): one 20 ms window per LIVE entry that is due.
  std::vector<modules::AudioFrame> popAudio(int64_t nowMs);
  std::vector<std::string> warnings() const;
  std::vector<Status> snapshot() const;
};
}
```

- [ ] **Step 1: Move and rename.** `git mv native/src/modules/OwnedMediaFrameSource.h native/src/core/MediaTransports.h`; namespace `corevideo::core`; class `MediaTransports`; drop `: public IMediaFrameSource` and the two `pollMedia*` overrides; keep `Factory` → `DecoderFactory`, `manager_`, `manage()`, `run()`, the cap (16), `audioNextTime_` (`MediaAudioDemandClock`), `collisionWarnings_` (delete — one identity per source id now makes collisions impossible; delete `rebuildCollisionWarnings`), `capWarningsLogged_`.

- [ ] **Step 2: Entry gains transport state** (replace `CompositorRenderPlanLayer layer` as the source of truth):

```cpp
struct Entry {
  MediaTransportDesired desired;          // guarded by mutex; the worker builds the decoder layer from it
  MediaTransportState state = MediaTransportState::Cued;
  bool restartRequested = false;          // worker: new decoder instance at 0 behind video.current_
  bool resumeRequested = false;           // worker: cued -> live: dropQueued + audio on + clock resume
  // ... existing: mutex, changed, wake, clockFrozen, frozenAtMs, video, audio, audioNextTime,
  //     audioEverProduced, warnings, stop, finished, wantsVideo, wantsAudio, thread ...
  int64_t durationMs = 0, positionMs = 0;   // published by the worker for snapshot()
};
// The layer the decoder is handed. Play state == (state == Live). Never from the plan.
static modules::CompositorRenderPlanLayer decoderLayerOf(const Entry& e) {
  modules::CompositorRenderPlanLayer l;
  l.kind = e.desired.sourceId.rfind("background:", 0) == 0 ? "media-background" : "media-video";
  l.sourceId = e.desired.sourceId; l.mediaAssetId = e.desired.assetId; l.mediaAssetPath = e.desired.path;
  l.mediaAssetKind = e.desired.kind; l.mediaAssetLoop = e.desired.loop;
  l.mediaAssetPlaying = e.state == MediaTransportState::Live;
  return l;
}
```

- [ ] **Step 3: `apply()`** replaces `requests(layers)` + `manage()`'s desired diff. Under `mutex_`: build `std::map<sourceId, MediaTransportDesired>` from `desired`; for every existing entry → `decideMediaTransport(prevDesired, curDesired, state)`; for every new id → `decideMediaTransport(nullopt, cur, Cued)`. Apply:
  - `OpenLive`/`OpenCued`: cap check (unchanged message), create `Entry` with `desired`, `state = next`, `wantsVideo = true`, `wantsAudio = (next == Live)`, push to `starting`, record `Change{id, true, entry}`.
  - `Resume`: `entry.state = Live; entry.resumeRequested = true; wantsAudio = true; wake`.
  - `RestartCued`: `entry.state = Cued; entry.restartRequested = true; wantsAudio = false; audio.clear(); wake`.
  - `Reopen`: stop + retire the entry (as `manage()` retires today), then create a fresh one as OpenLive/OpenCued (same id; `Change{id,false}` then `Change{id,true,newEntry}`).
  - `Pause`/`Play`: set state, wake (the worker's existing pause/resume block acts on `playing`).
  - `Release`: stop + retire, `Change{id,false}`.
  - `None`: update `entry.desired = cur` (bus flags may have changed).
  Keep `previousDesired_` (the map) for the next call. Start threads after the lock exactly as `manage()` does now. `manage()` keeps only its reaper role (join finished/retired workers, 2 ms wait).

- [ ] **Step 4: Worker (`run`)** — port verbatim, then: read `layer = decoderLayerOf(*entry)` under the entry mutex instead of `entry->layer`; keep the T1.2 pause/resume block, `everPlayed` is deleted; handle the two flags at the top of each loop iteration:

```cpp
bool restart = false, resume = false;
{ std::lock_guard<std::mutex> lock(entry->mutex);
  restart = std::exchange(entry->restartRequested, false); resume = std::exchange(entry->resumeRequested, false); }
if (restart) {
  // RestartCued: a NEW decoder instance at 0. video.current_ (the held frame) survives; the queue was
  // scheduled on the old clock and can never come due on the new one.
  decoder = factory_(); if (!decoder) throw std::runtime_error("Media decoder unavailable.");
  prefetchDecoder = dynamic_cast<modules::IMediaVideoPrefetch*>(decoder.get()); /* re-attach wake callback */
  haveSyncedPlaying = false; playedOnce = false;
  { std::lock_guard<std::mutex> lock(entry->mutex); entry->video.dropQueued(); entry->clockFrozen = false; entry->audio.clear(); }
}
if (resume) {
  // Cued -> Live (#449 hand-off, now in-place): the poster frames were scheduled on the paused epoch.
  std::lock_guard<std::mutex> lock(entry->mutex); entry->video.dropQueued(); entry->audio.clear();
}
```
  Also: when the decoder returns no video for a non-loop entry after it has played and `prefetchDecoder` reports nothing pending for 500 ms while `playing`, set `entry->state = Ended` under the mutex (the MF adapter already holds `lastFrame` at EOS — the state flip is for the snapshot and the operator Play rule; the worker keeps holding, so this is a bookkeeping change: implement as "no new frameId for 500 ms while Live and `!loop` and video queue empty").
  `positionMs` = `(video.current_.timestampMs - epoch)` is NOT available here; publish `positionMs` from the decoder if it is a `MediaFoundationMediaFrameSource` (`state.clock.elapsed100ns/10000`) via a new optional `IMediaVideoPrefetch::playbackPositionMs()` default `-1`; `durationMs` likewise via `mediaDurationMs()` default `-1`. Publish -1 when unknown.

- [ ] **Step 5: `selectVideo(Entry&, ts)`** = the per-entry body of today's `selectVideo` loop: `hold = state != Live || clockFrozen`; `video.hold()` vs `video.select(ts)`; stamp `participantId = desired.sourceId`, `timestampMs = ts/10000`; set `wake`, notify. `popAudio(nowMs)` = today's `pollMediaAudioFrames` loop over `entries_` with `audioRequests_` replaced by "entries whose `wantsAudio`" and the pause check `state != Live || clockFrozen`.

- [ ] **Step 6: MF adapter** — in `MediaFoundationMediaFrameSourceAdapter.cpp`: `stateFor` identity becomes `normalizeMediaPath(path) + (loop ? "|loop" : "|once")` (delete `mediaLayerPlaybackKey` and its uses); add the two optional prefetch getters; change the exported factory to
  `std::function<std::unique_ptr<IMediaFrameSource>()> createMediaFoundationMediaDecoderFactory()` returning `[] { return std::make_unique<MediaFoundationMediaFrameSource>(); }` (and the `#else` null variant returning an empty function). `Interfaces.h`: `ModuleSet::mediaFrames` → `std::function<std::unique_ptr<IMediaFrameSource>()> mediaDecoderFactory;`. `StubModules.cpp`: stub leaves it empty; `createDefaultModules` assigns the MF factory when non-empty. (MediaCore compiles again in Task 3 — Task 2 ends with the tests TU green and `corevideo_native` may not link until Task 3; run `--target corevideo-native-tests` filtered to `MediaTransports.*` and `MediaPlaybackTimeline.*` only. Ruling recorded here: Task 2 and Task 3 are committed separately but the dev suite as a whole is green only after Task 3.)

- [ ] **Step 7: Port the tests** in `native/tests/MediaPlaybackTimelineTest.cpp`: rename `TEST(OwnedMediaFrameSource, …)` → `TEST(MediaTransports, …)`; replace `pollMediaFrames({layer}, now)` with `apply({desired}, nowNs)` + `MediaTransports::selectVideo(*entry, now*10000)` where `entry` comes from the `Change` returned by `apply`; helper:

```cpp
MediaTransportDesired testDesired(bool onProgram, bool onPreview, bool loop = false) {
  MediaTransportDesired d; d.sourceId = loop ? "background:test" : "media:test"; d.assetId = "test";
  d.path = "test.wav"; d.kind = loop ? "background" : "video"; d.loop = loop; d.onProgram = onProgram; d.onPreview = onPreview; return d;
}
int64_t pollUntilFrame(MediaTransports::Entry& e, int64_t greaterThan = 0);  // same 2 s loop, calling selectVideo
```
  Keep: `SlowRetiredGenerationCannotBlockOrOverwriteNewPlayback` (now: `Reopen` on a path change), `AudioPrefetchIsBoundedAndDecoderStopsOnDestruction`, `TheCapWarningNamesTheAssetItRefused`, `PauseAndResumeKeepOneDecoder` (pause via `operatorAction`), `PauseHoldsTheOnAirFrame`, `NoAudioWhilePausedAndAudioResumes`. Delete: `TheSameRequestFromTwoBusesStartsOneDecoder` (no request dedup exists any more — one id one entry by construction), `AStillRouteOnBothBusesStartsNoDecoder…` (stills never reach `apply`; MediaCore filters them — covered in Task 3), `TwoPlaybackIdentitiesForOneSourceIdAreLoud`, `APausedCopyOfTheSameSourceCannotPauseTheProgramRoll`, the four `*Cue*`/`*Adopted*` tests. Add:

```cpp
TEST(MediaTransports, ACuedClipEnteringProgramRollsTheSameDecoderWithAudio) {
  std::atomic<int> created{0};
  MediaTransports t([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto ch = t.apply({testDesired(false, true)}, 0); ASSERT_EQ(ch.size(), 1u); auto e = ch.front().entry;
  const auto poster = pollUntilFrame(*e); ASSERT_GT(poster, 0);
  // Cued: holds the poster (frameId does not advance) and emits no audio.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(MediaTransports::selectVideo(*e, steadyNowMs() * 10000)->frameId, poster);
  EXPECT_TRUE(t.popAudio(steadyNowMs()).empty());
  EXPECT_TRUE(t.apply({testDesired(true, true)}, 0).empty());           // membership unchanged: no Change rows
  ASSERT_GT(pollUntilFrame(*e, poster), 0);                            // rolls
  EXPECT_EQ(created.load(), 1);                                        // the SAME decoder
  bool audio = false; const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!audio && std::chrono::steady_clock::now() < until) { audio = hasNonSilentPcm(t.popAudio(steadyNowMs())); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
  EXPECT_TRUE(audio);
}
TEST(MediaTransports, LeavingProgramWhileCuedRestartsBehindTheHeldFrame) {
  std::atomic<int> created{0};
  MediaTransports t([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto e = t.apply({testDesired(true, true)}, 0).front().entry;
  auto id = pollUntilFrame(*e); id = pollUntilFrame(*e, id + 3); ASSERT_GT(id, 0);
  EXPECT_TRUE(t.apply({testDesired(false, true)}, 0).empty());
  // The very next select still returns a frame (the held one), never nothing.
  ASSERT_TRUE(MediaTransports::selectVideo(*e, steadyNowMs() * 10000).has_value());
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2); bool restarted = false;
  while (!restarted && std::chrono::steady_clock::now() < until) {
    restarted = created.load() == 2; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
  EXPECT_TRUE(restarted);
  EXPECT_EQ(t.snapshot().front().state, MediaTransportState::Cued);
}
TEST(MediaTransports, ALoopNeverPausesAndNeverRestartsAcrossATake) {
  std::atomic<int> created{0};
  MediaTransports t([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto e = t.apply({testDesired(false, true, true)}, 0).front().entry;
  auto id = pollUntilFrame(*e); ASSERT_GT(id, 0);
  std::string reason; EXPECT_FALSE(t.operatorAction("test", MediaOperatorAction::Pause, reason));
  EXPECT_TRUE(t.apply({testDesired(true, false, true)}, 0).empty());
  ASSERT_GT(pollUntilFrame(*e, id), 0); EXPECT_EQ(created.load(), 1);
}
TEST(MediaTransports, AnIdenticalApplyIsANoOp) {
  std::atomic<int> created{0};
  MediaTransports t([&created] { ++created; return std::make_unique<CountingDecoder>(); });
  auto e = t.apply({testDesired(true, false)}, 0).front().entry;
  auto id = pollUntilFrame(*e); ASSERT_GT(id, 0);
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(t.apply({testDesired(true, false)}, 0).empty());
  ASSERT_GT(pollUntilFrame(*e, id), 0); EXPECT_EQ(created.load(), 1);
}
```

- [ ] **Step 8: Run** `--gtest_filter=MediaTransports.*` and `--gtest_filter=MediaPlaybackTimeline.*` → pass.
- [ ] **Step 9: Commit** `"core: MediaTransports owns the media decoder lifecycle from a command-time desired set (#535 3b)"`.

---

### Task 3: MediaCore — desired set at command time, bus membership, tick rewiring, test migration

**Files:**
- Modify: `native/src/core/MediaAssetSource.h` (kind `"media"` wraps a `MediaTransports::Entry`; kind `"still"` keeps `setLatest`)
- Modify: `native/src/core/MediaCore.h/.cpp`; `native/src/core/MediaBusRoster.h` (doc: "media" no longer synced from frames)
- Create: `native/tests/MediaTestSupport.h`
- Modify tests: `MediaCoreCommandTest.cpp`, `ProgramBufferIntegrationTest.cpp`, `RenderedSceneAttributionTest.cpp`, `ProgramPixelContinuityTest.cpp`, `SourceBusTest.cpp` (any `mediaFrames` use)

**Interfaces:**
- Consumes: Task 2's `MediaTransports`.
- Produces: `MediaCore::syncMediaTransportsDesired()` (private), `mediaTransports_` (`std::unique_ptr<core::MediaTransports>`, null when the module set has no factory), `MediaAssetSource(std::shared_ptr<MediaTransports::Entry>)` ctor; test seam `MediaCore::mediaTransportsForTest()`.

- [ ] **Step 1: `MediaAssetSource`** — add a second constructor `MediaAssetSource(std::string sourceId, std::shared_ptr<core::MediaTransports::Entry> entry)` (kind `"media"`, width/height filled from the first frame); `poll(ts)`: if `entry_` → `MediaTransports::selectVideo(*entry_, ts)`; frame → `Producing`, none → `Warming`. Keep the still path (`setLatest`) unchanged. Update the header comment: "media" is command-driven, "still" mirrors the cache.

- [ ] **Step 2: `MediaCore` ownership.** Constructor: `if (modules_.mediaDecoderFactory) mediaTransports_ = std::make_unique<core::MediaTransports>(modules_.mediaDecoderFactory);`. Destructor order: `mediaTransports_` declared AFTER `sourceBus_` so it is destroyed first (its workers may still be referenced by bus sources).

- [ ] **Step 3: `syncMediaTransportsDesired()`** (call it where `syncStillMediaDesired()` is called: end of `loadSceneGraph`, end of `applyPreviewScene` (only when it returned true, i.e. the signature changed), and from the new transport command in Task 4):

```cpp
void MediaCore::syncMediaTransportsDesired() {
  if (!mediaTransports_ || !sourceBus_) return;
  std::map<std::string, core::MediaTransportDesired> desired;
  const auto addRoutes = [&](const std::vector<SceneRouteState>& routes, bool program) {
    for (const auto& r : routes) {
      if (r.mediaAssetId.empty() || r.mediaAssetPath.empty()) continue;
      if (modules::isStillImageMediaAsset(r.mediaAssetKind, r.mediaAssetPath)) continue;   // stills: the cache
      auto& d = desired["media:" + r.mediaAssetId];
      d.sourceId = "media:" + r.mediaAssetId; d.assetId = r.mediaAssetId; d.kind = r.mediaAssetKind;
      d.path = modules::normalizeMediaAssetPath(r.mediaAssetPath); d.loop = d.loop || r.mediaAssetLoop;
      (program ? d.onProgram : d.onPreview) = true;
    }
  };
  const auto addBackground = [&](const SceneBackgroundState& bg, bool program) {
    if (!bg.enabled) return;
    auto& d = desired["background:" + bg.mediaAssetId];
    d.sourceId = "background:" + bg.mediaAssetId; d.assetId = bg.mediaAssetId; d.kind = bg.mediaAssetKind;
    d.path = modules::normalizeMediaAssetPath(bg.mediaAssetPath); d.loop = true;
    (program ? d.onProgram : d.onPreview) = true;
  };
  addRoutes(sceneRoutes_, true); addBackground(sceneBackground_, true);
  if (previewSceneActive_) { addRoutes(previewSceneRoutes_, false); addBackground(previewSceneBackground_, false); }
  std::vector<core::MediaTransportDesired> rows; rows.reserve(desired.size());
  for (auto& [id, d] : desired) rows.push_back(std::move(d));
  const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  for (const auto& change : mediaTransports_->apply(rows, nowNs)) {
    if (change.added) sourceBus_->add(std::make_shared<core::MediaAssetSource>(change.sourceId, change.entry));
    else sourceBus_->remove(change.sourceId);
  }
}
```
  NOTE the background `playing` flag is no longer read (loops always play); leave `SceneBackgroundState::playing` parsed but unused this task (Task 4 deletes it).

- [ ] **Step 4: Render tick.** In `renderSyntheticTick`: the early ingest selector becomes `d.kind != "still"` (media rides it); delete the post-plan block (`if (modules_.mediaFrames) { … ORDER IS LOAD-BEARING … }` through its `else`), replacing it with only the warnings merge: `if (mediaTransports_) for (const auto& w : mediaTransports_->warnings()) push unique into renderPlan.warnings`. Delete `core::syncMediaSources(*sourceBus_, …, "media")` calls (keep the `"still"` ones). The early ingest already passes `mediaPresentationTime100ns` — confirm it is the value the old block used (`mediaPresentationTime100ns`), else pass it.

- [ ] **Step 5: Audio worker.** In `gatherAudioOutputWork`, replace the `if (modules_.mediaFrames) { const auto plan = buildCompositorRenderPlan({}); … pollMediaAudioFrames(plan.layers, …) }` block with `if (mediaTransports_) { auto a = mediaTransports_->popAudio(frameTimestampMs); audioFrames.insert(...); }`.

- [ ] **Step 6: Test seam + support header.** `MediaCore::mediaTransportsForTest()` returns `mediaTransports_.get()`. Create `native/tests/MediaTestSupport.h`:

```cpp
#pragma once
#include "core/MediaCore.h"
#include <chrono>
#include <functional>
#include <thread>
namespace corevideo::testing {
template <typename Decoder, typename... Args>
std::function<std::unique_ptr<modules::IMediaFrameSource>()> mediaFactoryOf(Args... args) {
  return [=] { return std::unique_ptr<modules::IMediaFrameSource>(new Decoder(args...)); };
}
// Renders display ticks until `done(core)` is true or `timeoutMs` elapses; returns whether it became true.
// Media frames are produced by a worker thread (exactly as in production), so a test that wants pixels
// after load-scene-graph pumps here instead of assuming the first tick has them.
inline bool renderUntil(core::MediaCore& core, const std::function<bool(core::MediaCore&)>& done, int timeoutMs = 2000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    core.renderDisplayTick();
    if (done(core)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}
// True once the source bus reports a frame for `sourceId` (health producing).
inline bool busSourceProducing(core::MediaCore& core, const std::string& sourceId) {
  const auto state = core.sessionState(); const auto* sources = state.get("sources");
  if (!sources) return false;
  for (const auto& s : sources->asArray()) if (s.getString("sourceId") == sourceId) return s.getString("health") == "producing";
  return false;
}
}
```

- [ ] **Step 7: Migrate the injection sites** (mechanical; keep each test's assertions): every `modules.mediaFrames = std::make_unique<X>()` / `std::move(x)` → `modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<X>();`. Where a test kept a raw pointer to the fake to read `seenPlaybackKeys`/`pollCount`/`seenLoops` (`SolidMediaFrameSource`), make those fields `static` on the fake (reset in the test) so the factory-made instance still reports. Every test that reads pixels/frames/`sources[]` right after `applyCommands` inserts `ASSERT_TRUE(renderUntil(core, [](auto& c){ return busSourceProducing(c, "media:<id>"); }));` before the assertion (`ProgramPixelContinuity` background test: the SAME pump after the first apply, before counting ticks; the cold-start tests in `RenderedSceneAttributionTest` deliberately keep NO pump — a cold start is what they assert). `ProgramPixelContinuity.ACuedClipTakenToProgramNeverShowsThePlaceholder`: the cue scene omits `mediaPlaybackKey`/`mediaAssetPlaying`; the Take is `load-scene-graph` with the clip route; keep the 2 s warm loop and the 10-tick luma loop unchanged. Tests asserting `seenPlaybackKeys` (`ARouteLoopFlagReachesTheMediaSourceOnBothBuses` and neighbours) now assert the LOOP flag only and that ONE decoder instance served both buses (`created == 1`).

- [ ] **Step 8: Build `--clean-first --config Release`, run the full dev suite** → 0 failed; `scripts/test-native.ps1` (stub) green.
- [ ] **Step 9: Commit** `"core: media transports are fed from the scene graphs at command time; media rides the early bus ingest (#535 3b)"`.

---

### Task 4: Wire, snapshot and plan fields

**Files:**
- Modify: `native/src/core/MediaCore.h/.cpp` (`SceneRouteState`, `SceneBackgroundState`, parse sites ×2, `setMediaPlayback`, new `setMediaTransport`, snapshot), `native/src/modules/Interfaces.h` (`CompositorRenderPlanLayer`), `native/src/modules/ProgramFramePreview.cpp` (hash), `native/src/modules/MediaFoundationMediaFrameSourceAdapter.cpp` (no `layer.mediaPlaybackKey` reads remain), `native/src/modules/MediaVideoPresentation.h` (getter defaults from Task 2 if not done there)
- Test: `native/tests/MediaCoreCommandTest.cpp`

**Interfaces:**
- Produces: command `set-media-transport {mediaAssetId, action:"pause"|"play"}`; snapshot `mediaSources[]` `{sourceId, mediaAssetId, state, loop, onProgram, onPreview, positionMs, durationMs}`; `mediaPlayback.playing/status` from the selected asset's transport (`mediaPlaybackKey` always `""`).

- [ ] **Step 1: Failing tests** (append to `MediaCoreCommandTest.cpp`; `clipScene(sceneId, type)` = a `load-scene-graph`/`set-preview-scene` object with ONE fixed route `mediaAssetId:"clip"`, `mediaAssetKind:"video"`, `mediaAssetPath:"C:\\media\\clip.mp4"`, rect full — no key, no playing flag; `emptyScene(sceneId, type)`):

```cpp
TEST(MediaCoreCommand, MediaSourcesNodeIsPublishedEmptyAndThenPerSource) {
  auto modules = corevideo::modules::createStubModules();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore core(std::move(modules));
  auto state = core.sessionState();
  ASSERT_NE(state.get("mediaSources"), nullptr); EXPECT_TRUE(state.get("mediaSources")->asArray().empty());
  (void)core.applyCommands({emptyScene("a", "load-scene-graph"), clipScene("b", "set-preview-scene")});
  state = core.sessionState(); const auto& rows = state.get("mediaSources")->asArray(); ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].getString("sourceId"), "media:clip"); EXPECT_EQ(rows[0].getString("state"), "cued");
  EXPECT_FALSE(rows[0].get("onProgram")->asBool()); EXPECT_TRUE(rows[0].get("onPreview")->asBool());
  (void)core.applyCommands({clipScene("b", "load-scene-graph"), emptyScene("a", "set-preview-scene")});
  EXPECT_EQ(core.sessionState().get("mediaSources")->asArray()[0].getString("state"), "live");
}
TEST(MediaCoreCommand, SetMediaTransportPausesAndPlaysTheProgramClipOnly) {
  auto modules = corevideo::modules::createStubModules();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore core(std::move(modules));
  (void)core.applyCommands({clipScene("b", "load-scene-graph")});
  auto s = core.applyCommand({{"type","set-media-transport"},{"mediaAssetId","clip"},{"action","pause"}});
  EXPECT_EQ(s.get("mediaSources")->asArray()[0].getString("state"), "paused");
  s = core.applyCommand({{"type","set-media-transport"},{"mediaAssetId","clip"},{"action","play"}});
  EXPECT_EQ(s.get("mediaSources")->asArray()[0].getString("state"), "live");
  // Refused: not on Program (cued only) -> warning, state unchanged.
  (void)core.applyCommands({emptyScene("a", "load-scene-graph"), clipScene("b", "set-preview-scene")});
  s = core.applyCommand({{"type","set-media-transport"},{"mediaAssetId","clip"},{"action","pause"}});
  EXPECT_EQ(s.get("mediaSources")->asArray()[0].getString("state"), "cued");
  const auto& warnings = s.get("sceneValidationWarnings")->asArray();   // (use the node the other refusals use)
  EXPECT_TRUE(std::any_of(warnings.begin(), warnings.end(), [](const auto& w){ return w.asString().find("Program") != std::string::npos; }));
}
TEST(MediaCoreCommand, TheSelectedAssetsPlaybackNodeReportsTheTransportState) {
  // set-media-playback is selection only; playing/status come from the transport.
  auto modules = corevideo::modules::createStubModules();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore core(std::move(modules));
  auto s = core.applyCommand({{"type","set-media-playback"},{"mediaAssetId","clip"},{"mediaAssetName","Clip"},
                              {"mediaAssetPath","C:\\media\\clip.mp4"},{"playing", true}});   // `playing` ignored
  EXPECT_EQ(s.get("mediaPlayback")->getString("status"), "cued"); EXPECT_FALSE(s.get("mediaPlayback")->get("playing")->asBool());
  (void)core.applyCommands({clipScene("b", "load-scene-graph")});
  s = core.sessionState();
  EXPECT_EQ(s.get("mediaPlayback")->getString("status"), "live"); EXPECT_TRUE(s.get("mediaPlayback")->get("playing")->asBool());
  EXPECT_EQ(s.get("mediaPlayback")->getString("mediaPlaybackKey"), "");
}
TEST(MediaCoreCommand, LegacyPlaybackFieldsOnTheWireAreIgnoredNotRefused) {
  auto modules = corevideo::modules::createStubModules();
  modules.mediaDecoderFactory = corevideo::testing::mediaFactoryOf<SolidMediaFrameSource>();
  corevideo::core::MediaCore core(std::move(modules));
  auto scene = clipScene("b", "load-scene-graph");
  auto& route = scene.get("routes")->asArray()[0]; route.set("mediaPlaybackKey", "media:clip:live:7"); route.set("mediaAssetPlaying", false);
  const auto s = core.applyCommands({scene});
  EXPECT_EQ(s.get("mediaSources")->asArray()[0].getString("state"), "live");
  EXPECT_TRUE(s.get("sceneValidationWarnings")->asArray().empty());
}
```
  Adjust the JSON builder calls to the test file's existing `rpc::Json` idioms (`Json::Object{...}` and `Json::Array{...}`); `AppliesMediaPlaybackCommandAndWarnsOnEmptyAsset` is REWRITTEN to the new contract (selection echo, `status` = `"idle"` with no selection, `"cued"|"live"|"paused"|"ended"` otherwise, `"unavailable"` when the asset is not on either bus, empty-id warning kept).

- [ ] **Step 2: Run** `--gtest_filter=*MediaSourcesNode*` etc. → fail (nodes missing).

- [ ] **Step 3: Implement.**
  - `Interfaces.h`: delete `CompositorRenderPlanLayer::mediaPlaybackKey` and `mediaAssetPlaying`. Fix every compile error by deletion (`ProgramFramePreview.cpp` hash lines 280–281; `MediaCore.cpp` layer fills 5735–5736/5688; `buildPreviewCompositorRenderPlan`'s `pausedClipCue` block — DELETE the whole loop; the preview dedup signature at ~3591 drops the `:p/s` term but keeps `l/o`). The MF adapter's `decoderLayerOf` (Task 2) is the ONLY producer of a `mediaAssetPlaying` value now — so move that flag onto a NEW small struct the decoder takes? NO: keep the layer type for the decoder contract until 4b by re-adding the two fields as `// decoder-contract only (slice 4b retires)` in a sub-struct? Simplest that compiles and keeps the plan honest: keep `mediaAssetPlaying` on the layer type but assert at plan build that no plan layer sets it (it defaults false; nothing in MediaCore writes it) — DELETE `mediaPlaybackKey` outright, KEEP `mediaAssetPlaying` with the comment "written only by MediaTransports::decoderLayerOf for the decoder; never by a plan". Remove it from the two hashes/signatures regardless.
  - `SceneRouteState`: delete `mediaPlaybackKey`, `mediaAssetPlaying`; both parse sites stop reading them. `SceneBackgroundState::playing` deleted; its parse line deleted.
  - `setMediaPlayback`: keep the asset fields; delete `mediaPlaybackKey_`/`mediaPlaybackPlaying_` members and warnings about keys.
  - New `setMediaTransport(command)`: `mediaAssetId` empty → warning; `action` not `pause|play` → warning; else `mediaTransports_->operatorAction(id, action, reason)`; a `false` return pushes `reason` into `sceneValidationWarnings_` (dedup like `applyPreviewScene`). Route in the command switch next to `set-media-playback`. Then `syncMediaTransportsDesired()` is NOT needed (operator action does not change membership).
  - Snapshot: `mediaSources` array from `mediaTransports_->snapshot()` (empty array when no transports); `mediaPlayback.status` = the selected asset's `media:<id>` row state name, `"unavailable"` if not on the bus, `"idle"` when no selection; `playing` = state == live; `mediaPlaybackKey` = `""`; summary `"<name> <state>."`.

- [ ] **Step 4: Full dev suite + stub gate** green. `validate-multiview.mjs`, `validate-tiles.mjs` PASS.
- [ ] **Step 5: Commit** `"core: set-media-transport, mediaSources[] snapshot, legacy playback fields ignored (#535 3b)"`.

---

### Task 5: Behaviour tests for every operator rule (core)

**Files:**
- Modify: `native/tests/MediaCoreCommandTest.cpp` (append), `native/tests/ProgramPixelContinuityTest.cpp`
- Uses `native/tests/MediaTestSupport.h`

Each test uses a `FrameIdDecoder` fake (increasing frameId per video poll, non-silent audio per audio poll — port `CountingDecoder` from `MediaPlaybackTimelineTest.cpp` into `MediaTestSupport.h` as `corevideo::testing::CountingDecoder`) and a helper `frameIdOnProgram(core)` = the `media:clip` frame id in `core.lastProgramFrameForTest()`'s render (use `sessionState().programFrame`'s per-source list if it carries ids, else read `sourceBus` counters `lastFrameId` via `sources[]`).

- [ ] **Step 1: Write the tests** (names are the contract; each is red before Task 3/4 code paths exist for it):
  - `ACuedClipTakenToProgramRollsFromZeroWithOneDecoder` — cue in Preview, pump to a poster, Take, assert frameId advances and `created == 1`, audio frames appear on `popAudio` within 2 s.
  - `TheSameClipOnBothBusesShowsOneRollingPictureOnPreview` — clip on Program and on Preview; render 10 ticks; the preview plan's `media:clip` layer resolves the SAME frameId as Program's (no `preview:` id anywhere in `buildPreviewCompositorRenderPlan` output — assert no layer sourceId starts with `preview:`).
  - `AClipThatLeavesProgramAndReturnsRollsFromZeroAgain` — live, capture frameId N; Take away (still cued); pump until `created == 2`; Take back; the first new frameId after the return is < N... (CountingDecoder ids are per-instance, so a new instance starts at 1: assert the first frame after return has id ≤ 3).
  - `ATakeBetweenTwoScenesBothHoldingTheClipDoesNotRestartIt` — scene A and scene B both route the clip; Take A→B; pump 20 ticks; `created == 1` and frameIds strictly increasing across the take.
  - `PauseHoldsTheFrameAndSilencesAudioPlayResumes` — via `set-media-transport`.
  - `ALoopBackgroundIgnoresPauseAndSurvivesATake` — `background:bg` on both scenes; pause refused (warning); Take; `created == 1`, ids advance.
  - `AnIdenticalSceneGraphResentChangesNothing` — send the same `load-scene-graph` 5 times; `created == 1`; no new `Change` (assert `mediaSources[0].state` stable and frameIds advance).
  - `AFinishedClipHoldsItsLastFrameAndReadsEnded` — a decoder that returns 10 frames then none: after the 10th, `selectVideo` keeps returning id 10 and `mediaSources[0].state == "ended"` within 1 s; `set-media-transport play` restarts it (`created == 2`).
  - Take record: re-run the existing `TakeRecord.AMediaBackgroundThatHasNoFrameOnTheFirstProgramTickIsRebuilt` unchanged (cold start still honest) and add `TakeRecord.ACuedClipTakenToProgramReadsNoMissingSources` (pump the cue, Take, first program tick's take record has `missingSources == []`).
- [ ] **Step 2: Run each with its own wildcard**, fix core bugs they find (log each fix in the ledger), full suite green.
- [ ] **Step 3: Commit** `"tests: slice 3b operator rules — cue/take/leave/return/pause/loop/idempotent/ended (#535 3b, #449)"`.

---

### Task 6: Shell — retire the playback key and the ledger; drive the bin from the snapshot

**Files:**
- Modify: `native-shell/CoreVideoPro.MediaCore/Models/MediaCoreProductionSyncContext.cs` (drop `MediaPlaybackKey`, `MediaAssetPlaying` from `MediaCoreSceneRouteWire`; drop `SelectedMediaPlaybackKey`, `SelectedMediaAssetPlaying`), `Services/MediaCoreCommandBuilder.cs` (route + playback command; new `BuildMediaTransportCommand(string assetId, string action)`), `Models/NativeMediaCoreProtocol.cs` (`NativeMediaCoreMediaSource`, `IReadOnlyList<NativeMediaCoreMediaSource> MediaSources` on wire + base + synthesized snapshots, default `[]`; `MediaPlaybackKey` stays nullable for old cores), `Models/CoreProtocolModels.cs`, `Services/NativeMediaCoreStateMapper.cs` (carry `MediaSources`), `Services/SyntheticMediaCore.cs` (echo `mediaSources` from its scene state: state `live` for Program routes, `cued` for Preview-only)
- Modify: `native-shell/CoreVideoPro.WinUI/Services/MediaRoutePlaybackService.cs` (delete `BuildSceneMediaPlaybackKey`, `ResolveSceneRoutePlayback`, `SceneRoutePlayback`, `ShouldPlaySceneMediaRoute`, `ResolvePlaybackSelection`, `MediaGoLiveLedger`; add `public static IReadOnlyList<string> AssetsEnteringProgram(previousProgramRoutes, programRoutes)` (the set diff from `RecordTake`, no state); change `IsPlayingOnAir(string assetId, IReadOnlyList<NativeMediaCoreMediaSource> sources)` → true iff a row `sourceId == "media:"+assetId` has `State == "live"`; `ResolveTap(bool isOnProgram, bool isLooping, string? state)` → Pause when `state == "live"`, Resume when `"paused"|"ended"`, else Select)
- Modify: `ViewModels/Transport/ITransportHost.cs` (`RecordProgramMediaGoLive` → `IReadOnlyList<string> AssetsEnteringProgram(IReadOnlyList<SourceRoute> previous)`; delete `OperatorPausedMediaAssetIds`), `TransportCoordinator.cs` (call rename only), `TakeMediaSelectionRollback.cs` (playing from the snapshot rows passed in), `StudioViewModel.cs` + `StudioViewModel.Transport.cs` (delete `_mediaGoLive`; `IsMediaAssetPlaying(id)` reads `_bridge.LastSnapshot?.MediaSources`; `PlayMediaAsset` Pause/Resume cases send `BuildMediaTransportCommand` through the bridge's single-command path (`_bridge.SendCommandAsync` / the same path `source.dropout.set` uses) instead of touching a ledger; `BuildProductionSyncContext` stops computing the key/playing; on snapshot apply (`ApplySnapshot`), a new `RefreshMediaBinPlaybackScalars()` sets each existing `MediaBinItem.IsPlaying` in place from `MediaSources` (only when the set of live ids changed — keep a `string _lastLiveMediaSignature`), and `SelectedMediaAssetPlaying`/`MediaPlaybackStatus` from the selected asset's row; `MediaBinItem.IsPlaying` becomes an observable scalar if it is not already)
- Tests: `CoreVideoPro.MediaCore.Tests/MediaCoreCommandBuilderTests.cs`, `NativeMediaCoreStateMapperTests.cs`; `CoreVideoPro.WinUI.Tests/MediaRoutePlaybackServiceTests.cs`, `TransportCoordinatorTests.cs`, `StudioViewModelAudioStatusTests.cs` (the "key media:clip-intro:live:3" status string → no key)

- [ ] **Step 1: Failing tests.** Builder: `SerializesMediaAssetPayloadForSceneGraphRoutes` asserts the route dictionary has NO `mediaPlaybackKey`/`mediaAssetPlaying` keys and HAS `mediaAssetLoop`; `PushesMediaPlaybackCommandOnlyWhenAnAssetIsSelected` asserts no `playing`/`mediaPlaybackKey`; new `BuildMediaTransportCommand_EmitsPauseAndPlayForOneAsset`. Mapper: `MediaSources` round-trips from the wire (`state`, `onProgram`, `positionMs`), default `[]`. Service: replace the deleted methods' tests with `AssetsEnteringProgram_*` (3 cases: enters, stays, leaves), `IsPlayingOnAir_ReadsTheCoreRow*` (live true, paused false, absent false), `ResolveTap_*` re-pointed at `state`. Transport: `Take_PromotesOnlyAssetsEnteringProgram` (existing promote tests re-pointed), rollback tests pass `MediaSources`. Audio status: `"Native: Intro Sting live."`.
- [ ] **Step 2: Implement** the file list above. Delete `MediaGoLiveLedger` last (compile errors are the checklist).
- [ ] **Step 3:** `dotnet test native-shell/CoreVideoPro.MediaCore.Tests`, `...Control.Tests`, `...WinUI.Tests` → 0 failed; `dotnet build native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj -c Release -p:Platform=x64` → 0 errors.
- [ ] **Step 4: Commit** `"shell: media play state comes from the core — playback keys, go-live ledger and preview poster retired (#535 3b, #449)"`.

---

### Task 7: Recorded clip-Take oracle, live gates, docs

**Files:**
- Create: `scripts/qa/media-take-ab.py`
- Modify: `CLAUDE.md` (source-bus section: a "Slice 3b" paragraph; the "Media is a persistent source" section: strike the cue hand-off / `preview:` / go-live-generation paragraphs, point at 3b), `docs/BACKLOG.md` (#535 row: 3b state; #449 row: media half closed by 3b, step 1 still open), `docs/superpowers/specs/2026-09-18-source-bus-design.md` §5 (3b "shipped on branch" wording), `docs/superpowers/specs/2026-09-19-source-bus-slice4-scoping.md` (3b prerequisite satisfied)

- [ ] **Step 1: `scripts/qa/media-take-ab.py`** — same `Core` class and phase shape as `zoom-gap-hold-ab.py` (copy its helpers verbatim). Args: `--core`, `--fake`, `--label`, `--clip` (default: generated), `--ffmpeg` (default from `COREVIDEO_FFMPEG_BIN_DIR` or PATH). Flow:
  1. If `--clip` absent, generate `<label>-clip.mp4` with ffmpeg: 3 s, 1280x720, 30 fps, H.264, the FIRST second solid `color=c=0x10A0F0` (BT.709 luma ≈ 100) then `testsrc2` (moving pattern), concatenated via `-filter_complex "[0:v][1:v]concat=n=2:v=1:a=0"`, `-pix_fmt yuv420p`.
  2. Join the fake meeting (participant 101 on Program as in the gap script), `load-scene-graph pgm` (101) + `set-preview-scene pvw` with ONE fixed route `mediaAssetId:"clip"`, `mediaAssetKind:"video"`, `mediaAssetPath:<clip>`; poll 40× recording `mediaSources` (expect `cued`).
  3. Start recording (same three commands as the gap script), poll 20×.
  4. TAKE: `begin-take-transition` + `load-scene-graph pvw` (clip route) + `set-preview-scene pgm` (101); poll 60× at 50 ms (expect `live`).
  5. Stop recording, leave, kill.
  6. Judge the MP4 with `signalstats` YAVG per frame: find the take frame = first frame whose YAVG leaves the 101 fixture range (~188–205); assert the next 15 frames have YAVG within ±6 of 100 (the clip's first-second colour; never the slate ~30 nor black 16), and that YAVG within frames +45…+75 (the moving pattern after 1 s) differs from 100 by > 10 on at least half of them (it rolls). Print the phase summary and `PASS`/`FAIL`, exit code accordingly.
- [ ] **Step 2: Run it** on the branch build: `python scripts/qa/media-take-ab.py --core native/build-dev/corevideo-native.exe --fake native/build-dev/corevideo-zoom-engine-fake.exe --label s3b` → PASS. Archive under `artifacts/qa/slice3b/`.
- [ ] **Step 3: Regression gates** (record every number in the ledger and CLAUDE.md): full dev suite; stub gate; `dotnet test` ×3; `validate-multiview.mjs`; `validate-tiles.mjs`; `zoom-gap-hold-ab.py --label s3b-hold` (luma holds); show drill `COREVIDEO_FAKE_ENGINE_FPS=60 python scripts/mac-show-drill.py --seconds 40 --load 8` (60 fps, 0 dropped, ≥90% delivery).
- [ ] **Step 4: Docs** as listed; keep the CLAUDE.md paragraph in the same voice as slices 2–4a (what moved, what did NOT, the removal rule, the accepted notes, the gate numbers).
- [ ] **Step 5: Commit** `"qa+docs: media-take-ab oracle, slice 3b record (#535 3b, #449)"`.

---

## Self-review

- **Spec coverage:** §2 rules 1–6 → Task 5 tests + Task 2 worker actions; §3.1 → Task 2/3; §3.2 → Task 1; §3.3 → Task 3 step 3; §3.4 → Task 3 steps 4–5 and Task 4 field deletions; §3.5 → Task 2 step 6; §3.6 → Task 4; §4 → Task 6; §5 → Tasks 5, 6, 7; §6 out of scope respected (stills untouched, no seek UI, `IMediaFrameSource` kept as decoder contract); §7 risks: restart-behind-held-frame covered by `LeavingProgramWhileCuedRestartsBehindTheHeldFrame`, audio on resume by `ACuedClipEnteringProgramRollsTheSameDecoderWithAudio`, skew by `LegacyPlaybackFieldsOnTheWireAreIgnoredNotRefused`.
- **Type consistency:** `MediaTransportDesired`/`MediaTransportState`/`MediaTransportAction`/`MediaOperatorAction` (Task 1) are the names used in Tasks 2–4; `MediaTransports::Change{sourceId, added, entry}` (Task 2) is what Task 3's sync consumes; `ModuleSet::mediaDecoderFactory` (Task 2) is what Task 3's tests set; `mediaSources[]` field names (Task 4) match `NativeMediaCoreMediaSource` (Task 6).
- **Placeholders:** none; the two "port verbatim" instructions name the exact source functions (`OwnedMediaFrameSource::run`, `pollMediaAudioFrames`) that exist on the branch at Task 2's start.
