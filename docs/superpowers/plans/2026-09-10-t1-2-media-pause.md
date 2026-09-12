# T1.2 — Pause is a clock state, not a new decoder — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use `- [ ]`.

**Goal:** Pausing a clip that is rolling on Program freezes the exact frame on air and silences its audio; Play resumes from that frame. Neither Pause, Play nor a bin-row tap ever restarts a clip or cuts it to its first frame. Restart from the top happens only when the clip goes live (the go-live generation) — unchanged.

**Architecture:** Today playing/paused is part of the media decoder's identity in two places — the owned media source's request key (`native/src/modules/OwnedMediaFrameSource.h:131`) and the adapter's playback identity (`native/src/modules/MediaFoundationMediaFrameSourceAdapter.cpp:552`) — so a pause opens a new decoder (first frame) and a resume opens another (from the top). The fix removes playing/paused from both identities and makes it a state of the clip's existing clock (`MediaPlaybackTimeline`): paused time does not advance the clock; the decoder holds its presented frame; audio emits nothing while paused and continues from the same media position on resume. In the shell, the bin row shows the clip's real on-air state and a tap toggles pause/resume without a restart.

**Tech stack:** C++20 core (`native/`), C# .NET 9 WinUI shell (`native-shell/`).

**Spec:** `docs/superpowers/specs/2026-09-10-persistent-sources-design.md` §2 (a source has one owner and one clock; restart is a go-live policy, never a side effect). Backlog: `docs/BACKLOG.md` T1.2, issue #429 (re-scoped, owner-approved 2026-09-10).

## Global Constraints

- One decoder per clip across Pause/Play: pausing and resuming a Program clip must create NO new decoder (counting-factory tests).
- A paused clip holds the frame that was on air at the moment of pause (same `frameId`), never the first frame.
- Resume continues from the held frame: the next frames' `frameId`s are greater than the held one; media PTS continues from the paused position (no jump to 0, no skip of the paused duration).
- Audio: no media PCM is emitted while paused; on resume audio continues from the paused media position and stays within the house A/V budget (50 ms) of video.
- The Preview clip cue poster (`preview:media:<id>`, paused, never played) keeps its current behaviour: decode one first frame and hold it.
- A new go-live generation (playback key `media:<id>:live:<n>` advancing) still means a new decoder rolling from 0 — unchanged.
- No pixel work, blocking I/O or unbounded logging under `coreMutex`; decoder work stays on the owned worker threads.
- Build with `npm run build:native-dev` (`ZOOM_SDK_DIR` set); run `native/build-dev/corevideo-native-tests.exe` (not `Release/`); `--gtest_filter` is ignored. Baselines: native 827, WinUI 1301, MediaCore 2159, Control 74.
- Commit trailer: `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>` + `Claude-Session: https://claude.ai/code/session_01SAB8v62BEV8ihmaeGhhtY9`.

---

### Task 1: The core clock pauses instead of reopening

**Files:**
- Modify: `native/src/modules/MediaPlaybackTimeline.h` (`MediaPlaybackTimeline`)
- Modify: `native/src/modules/OwnedMediaFrameSource.h` (`requests()`, `Entry`, `run()`, `selectVideo`, `pollMediaAudioFrames`, `manage()`)
- Modify: `native/src/modules/MediaFoundationMediaFrameSourceAdapter.cpp` (`mediaLayerPlaybackKey`, `stateFor`, `decodeLayer`, `decodeLayerAudio`, and the paused-cue branch)
- Modify if needed: `native/src/modules/MediaVideoPresentation.h`
- Test: `native/tests/MediaPlaybackTimelineTest.cpp` (timeline + `OwnedMediaFrameSource.*`), `native/tests/MediaCoreCommandTest.cpp` (`DecodesFirstFrameForPausedPreviewCue` must still pass)

**Interfaces produced:**
```cpp
// MediaPlaybackTimeline — pause is a clock state.
// configure(identity, playing, now): resets (new epoch, ++generation) ONLY when identity changes.
// A playing -> paused change freezes elapsed time at its current value;
// paused -> playing resumes from that value (epoch shifts by the paused duration).
bool configure(const std::string& identity, bool playing, int64_t now100ns);
int64_t elapsed100ns(int64_t now100ns) const;   // frozen while paused
bool paused() const;
uint64_t generation() const;                      // unchanged by pause/resume
```
The request key in `OwnedMediaFrameSource::requests()` and the playback identity in the adapter exclude playing/paused; the layer's `mediaAssetPlaying` still reaches the worker every poll (update `entry->layer` for an existing entry) so it can pause.

- [ ] **Step 1: Failing timeline tests** in `MediaPlaybackTimelineTest.cpp`:
  - `MediaPlaybackTimeline.PauseFreezesElapsedAndResumeContinues`: configure(id, true, 0); elapsed(10'000'000)=1 s; configure(id, false, 10'000'000); elapsed(50'000'000) still 1 s; configure(id, true, 50'000'000); elapsed(60'000'000) = 2 s; generation unchanged across pause/resume.
  - `MediaPlaybackTimeline.ANewIdentityStillResets`: generation increments and elapsed restarts at 0.
  - The existing `RestartAndPauseResetSharedGeneration` pins the OLD contract (pause resets): rewrite it to the new contract (identity change resets; pause does not) — do not delete it.
- [ ] **Step 2: Failing owned-source tests** (reuse `TestDecoder`/`DecodeGate`; add a counting factory):
  - `OwnedMediaFrameSource.PauseAndResumeKeepOneDecoder`: poll a playing layer until frames arrive; poll the same layer with `mediaAssetPlaying=false` for 200 ms; poll playing again → factory call count == 1 throughout.
  - `OwnedMediaFrameSource.PauseHoldsTheOnAirFrame`: with a decoder whose frames carry increasing `frameId`, after pausing, every poll for ≥200 ms returns the same `frameId` as the last frame before the pause; after resume, a later poll returns a greater `frameId`.
  - `OwnedMediaFrameSource.NoAudioWhilePausedAndAudioResumes`: audio polls return nothing (not even silence) for a paused layer; after resume, audio frames arrive again.
  Use ≥2 s deadline polls, never fixed sleeps, as the existing tests do.
- [ ] **Step 3:** Build; confirm RED.
- [ ] **Step 4: Implement.**
  - Timeline: add `paused_`, `pausedAt_`; implement the contract above.
  - `requests()`: drop the `|playing`/`|paused` component; keep the collision warning working (two identities for one id can still happen via different keys/paths — keep its test green).
  - `manage()`: for an existing entry, refresh `entry->layer` (under the entry mutex) so a pause reaches the worker; do not recreate the entry.
  - Worker (`run()`): when the layer is paused, stop prefetching video and audio, keep `entry->video`'s current frame, drop queued-but-not-presented samples (their due times belong to the old epoch) and re-prefetch on resume. Wake the worker on a pause/resume change.
  - Adapter: `mediaLayerPlaybackKey` / `stateFor` identity exclude playing; `state.clock.configure(identity, playing, now)` carries the pause. In `decodeLayer`, a paused layer whose state has already presented a playing frame returns `state.presentedFrame` without reading (hold); a paused layer that has never played keeps today's poster path (decode one first frame, hold). Resume continues reading from the reader's current position; the clock's frozen elapsed time keeps video due-times aligned. Audio: `decodeLayerAudio` returns nothing while paused and continues from `audioWindows`' cursor on resume.
  - Keep the go-live contract: a new `mediaPlaybackKey` is a new identity → new decoder from 0.
- [ ] **Step 5:** Build; run the whole native suite; GREEN with 0 failures (baseline 827 + new tests). `DecodesFirstFrameForPausedPreviewCue` and all `OwnedMediaFrameSource.*` pass.
- [ ] **Step 6: Commit** — "Pause a clip on its clock instead of reopening its decoder".

---

### Task 2: The shell pauses and resumes without restarting

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs` (`PlayMediaAsset` ~6096-6150, `IsMediaAssetPlaying` ~6056, remove-asset check ~5968)
- Modify: `native-shell/CoreVideoPro.WinUI/Services/MediaRoutePlaybackService.cs` (pure decision helpers)
- Test: `native-shell/CoreVideoPro.WinUI.Tests/MediaRoutePlaybackServiceTests.cs`

**Behaviour:**
- `PlayMediaAsset` (bin-row tap and the toggle) on a clip routed on Program: if it is playing on air → `RecordPause`; if it is paused on air → `RecordPlay`. **Never** `RecordRestart` from this path (delete that call). Selection moves to the tapped asset either way.
- A clip NOT on Program keeps today's select/cue behaviour.
- `IsMediaAssetPlaying(assetId)` reports the real on-air state: routed on Program AND (looping OR not in the ledger's paused set). The bin row and toggle label read it, so a rolling clip that is not the selection shows as playing and its tap pauses it.
- Put the decision in a pure static helper in `MediaRoutePlaybackService` (e.g. `ResolveTap(assetId, isOnProgram, isLooping, isPaused) -> {Pause, Resume, Select}` and `IsPlayingOnAir(...)`) and have StudioViewModel call it — no new logic on the god file.

- [ ] **Step 1: Failing tests** in `MediaRoutePlaybackServiceTests`:
  - A tap on a rolling Program clip that is not selected → Pause (not Resume, not Restart); generation unchanged.
  - A tap on a paused Program clip → Resume; generation unchanged; paused set no longer contains it.
  - A tap on a clip not on Program → Select (no ledger change).
  - `IsPlayingOnAir` is true for a rolling unselected Program clip, false for a paused one, true for a looping one.
  - Update any existing test that asserted "Play on a Program clip restarts it" to the new contract (rewrite, don't delete).
- [ ] **Step 2:** RED (build error or failing asserts).
- [ ] **Step 3:** Implement as above.
- [ ] **Step 4:** WinUI, MediaCore and Control test projects GREEN.
- [ ] **Step 5: Commit** — "Tap pauses and resumes a Program clip instead of restarting it".

---

### Task 3: Docs and live check

**Files:** `CLAUDE.md` (section "Media is a persistent source"), `docs/BACKLOG.md` (T1.2 row wording → re-scoped; add T1.6 #455 under Tier 1 and T5.6 #456 under Tier 5; strike T1.1 as closed/not reproduced #428).

- [ ] **Step 1:** Document: pause is a clock state; identity excludes playing; the hold/resume contract; restart only via go-live; the cue poster exception. Update BACKLOG rows.
- [ ] **Step 2: Commit** — "Document pause as a clock state and update the backlog".
- [ ] **Step 3 (controller, with the owner):** rebuild the app from the branch, owner pauses and resumes a clip on Program and taps a rolling clip's bin row; confirm no restart on air.
