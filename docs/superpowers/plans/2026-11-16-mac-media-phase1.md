# macOS media clip playback, Phase 1 (N1, N2, S1, S2, S3) implementation plan

Status: DRAFT for owner review, 2026-11-16.

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:subagent-driven-development (recommended)
> or superpowers:executing-plans to run this plan task by task. Steps use checkbox (`- [ ]`) syntax.
> `docs/superpowers/plan-authoring-rules.md` is binding: every task report lists the named mutations
> and whether each went red. **When a supplied test contradicts a reasonable implementation, the test
> is the first suspect** (rule 4 corollary): stop and report, do not bend the code to a bad fixture.

**Goal:** an operator on a Mac can import a clip, cue it in Preview, Take it, watch it roll from frame 0
with its audio on the "media" strip, pause and resume it on air without a restart, loop a video behind a
scene, and manage the bin (filter, kinds, duration, select, remove) the way Windows 1.0 does.

**Spec:** `docs/superpowers/specs/2026-11-15-mac-media-clip-playback-design.md` (slices N1, N2, S1, S2, S3).
Sequencing: `docs/superpowers/plans/2026-11-15-mac-parity-master-plan.md` Phase 1, rules §7.

**Architecture in one paragraph.** The shared core already owns everything about a clip
(`core::MediaTransports`, one entry and one decoder per `media:<assetId>` / `background:<assetId>`,
`MediaTransportPolicy`, `MediaPlaybackTimeline` pause-as-clock-state, `MediaAudioWindows`,
`set-media-transport`, `mediaSources[]`). The Mac gets nothing because `ModuleSet::mediaDecoderFactory`
is empty there (`native/src/modules/ModuleComposition.cpp:440-442`; the Media Foundation factory returns
`{}` off Windows at `MediaFoundationMediaFrameSourceAdapter.cpp:1308-1316`), so `mediaTransports_` is never
built (`MediaCore.cpp:469-471`). This plan adds a second factory, `createAvFoundationMediaDecoderFactory()`,
returning a **portable** `modules::MediaClipDecoder` (the Media Foundation adapter's `IMediaDecoder` +
`IMediaVideoPrefetch` state machine, ported row for row) driven through a small non-blocking reader seam
(`IClipReaderFactory`) whose Apple implementation is two `AVAssetReader`s on private serial queues. No
shared owner code changes: frames are BGRA in `VideoFrame::pixels`, which is what
`MediaVideoPresentation::push` already accepts. The Mac shell then ports the Windows media decisions
(`MediaRoutePlaybackService`, `MediaBinPlaybackProjection`, `MediaTransportGestureLedger`,
`TakeMediaSelectionRollback`) to Swift with their Windows test names.

**Why a portable decoder plus a reader seam, not one `.mm` file.** Every row of spec §4.1 (cue poster,
pause hold, resume without reopen, loop offset, end, frame ids, position) is a decision about samples, not
about AVFoundation. Behind a seam they are tested deterministically with scripted readers on every
platform, and the AVFoundation code shrinks to "open, decode, copy, hand over, cancel". The same seam is
where N5's FFmpeg reader plugs in later. The seam is new code only; it is not wired into Windows.

---

## Defaults taken (owner may override)

| # | Default | Source | Tasks that change if overridden |
|---|---|---|---|
| D1 | **WebM and any file AVFoundation cannot open is refused loudly on Mac** until N5: decoder warning "…: AVFoundation cannot open .webm files; WebM playback on Mac needs FFmpeg, which this build does not use yet.", a `[media-decoder]` log line, the background picker omits `.webm`, and Cue refuses it with the same sentence. | Spec owner decision 1 (rec. (b) as N5, (a) until then) | 2, 5, 11 |
| D2 | Chapter-markers card not ported. | Spec decision 2 | none |
| D3 | No grade readouts on the Media tab; the Mac grade window stays. | Spec decision 3 | none |
| D4 | **Local audition preview is muted** (AVPlayer `isMuted = true`, no UI to unmute). | Spec decision 4 | 9 |
| D5 | Zero-copy (N4) is sequenced after N1/N2 **unless Task 4's drill fails**, in which case Task 4's report says so and N4 becomes the next native slice. | Spec decision 5 | 4 (report only) |
| D6 | New capability name **`media-clip-playback`** joins `kNativeMediaCoreCapabilities` (24 → 25). Windows will report it `available` too (its MF factory is non-empty), an additive profile change. | Spec §4.1 | 2 |
| D7 | The five `MediaFoundationMediaFrameSource.*` decoder tests keep their suite name when re-gated to run on either decoder (no Windows test-id churn; nothing in `scripts/`, `.github/` or `native/CMakeLists.txt` filters on them, grep-verified). | Plan | 2, 6 |
| D8 | Mac gesture retry semantics: a transport failure (`BridgeError.timeout`, `.notRunning`, `.processExited`) is the Mac's "skipped" and re-arms (120 ms, 5 attempts); `BridgeError.remote` is "failed". Windows has a backpressure skip the Mac bridge does not. | Plan (no Mac twin of `SingleSendBackpressure`) | 12 |
| D9 | Media-bin durations are probed on Mac although Windows' scan leaves `DurationMs` null (`MediaBinClassifier.cs:212`). The label format is Windows' `DurationLabel` (`ProductionModels.cs:883-884`). | Spec §4.2 | 9 |

---

## Spec corrections (planned against the code, not the spec)

1. **Only one `MediaCoreCommandTest.cpp` block is a decoder block.** `:4077-4494` holds the five
   `MediaFoundationMediaFrameSource.*` tests. `:1981` and `:2610` are *encoder* capability gates that
   already carry `#elif COREVIDEO_WITH_AVF_ENCODER`; `:5180-5268` are `HardwareEncoderAdapter.*` MF
   *encoder* tests. Re-gating those would be wrong. Task 2/6 re-gate `:4087`, `:4121`, `:4202`, `:4253`;
   the two FFmpeg cases (`:4315`, `:4400`) stay Media-Foundation-only.
2. **`MediaTransports.*` and `ProgramPixelContinuity.*` do not run against Media Foundation on Windows.**
   They use fake decoders (`corevideo::testing::mediaFactoryOf<…>`, `native/tests/MediaTestSupport.h`) on
   every platform and already pass on this Mac (39/39 in the media filters, measured). Real-decoder
   coverage on Mac comes from the re-gated decoder tests and the new Metal pixel tests (Task 3).
3. **`contracts/observation.schema.json:144-148` names the `mediaSources` node only** (projection flags).
   It does not describe the row. The row oracle is `native/src/core/CoreStateCapture.cpp:1199-1217` plus
   the C# reader `NativeMediaCoreMediaSource` (`native-shell/CoreVideoPro.MediaCore/Models/NativeMediaCoreProtocol.cs:770-791`).
4. **Still scene backgrounds do not render on Mac today either.** `syncMediaTransportsDesired` hands every
   background, still or video, to `MediaTransports` (`MediaCore.cpp:2363-2373`); `StillMediaFrameCache`
   serves route stills only (`MediaCore.cpp:2308-2335`). With `mediaTransports_` null, no background ever
   renders, and the picker is empty anyway (`ShellApp.swift:1037`). N1's "background still: decode once
   and hold" row fixes stills and videos together.
5. **The live capability report is `CoreStateCapture.cpp:231`.** `MediaCore.cpp:241` is an unused
   anonymous-namespace copy (no caller, grep-verified). Adding a capability also needs `core/Protocol.h`
   (`kNativeMediaCoreCapabilities`, enforced by `ContractParity` at `native/tests/ContractParityTest.cpp:235-247`).
6. **Windows promotes the selection on the local swap, before the sync, and rolls it back on failure**
   (`TransportCoordinator.cs:179-216, 270-297`), not "after a confirmed Take". The Mac port follows Windows.
7. **Gesture give-up wording** is Windows' "Could not pause <name> - the media core stayed busy. Press it
   again." (`StudioViewModel.cs:6466-6474`), not the spec's "Pause not confirmed by the media core".
8. **`scripts/validate-record-audio.mjs` also hard-codes `native/build-dev` (`:40`)**, beside the spec's
   `:59` (`C:\ffmpeg` tool path), `:172` (`platform: "windows"`) and `:315` (ffprobe candidates). It also
   still sends the retired `mediaPlaybackKey`/`mediaAssetPlaying` (`:369-370`).
9. **`sceneHasMediaRoute` needs no change.** It already keys on the fixed route wire (`AppModel.swift:3973-3978`)
   and so counts clips and the logo bug, matching Windows `ResolveSceneMediaAudioSourceIds`, which counts
   any fixed media route including stills (`StudioViewModel.cs:9553-9561`).
10. Line drift (re-read 2026-11-16): `kMaxDecoders` is `MediaTransports.h:552`, `kReleaseGraceMs` `:567`,
    the worker `run()` `:605`, `selectVideo` `:387`; `setMediaTransport` `MediaCore.cpp:3563-3585`.

---

## Before you start

- **Branch:** create `mac-media-phase1` from `mac-tier0b` in a new worktree
  (`.claude/worktrees/mac-media-phase1`). Commit per task; never push; never touch `main`. End commit
  messages with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Tracking issue (master plan §7):** `gh issue list --label backlog --search "media clip playback mac"`.
  If none exists, ask the owner before filing "macOS media clip playback (spec 2026-11-15)". Reference it
  in every commit body.
- **Baseline measured 2026-11-16 at `e98436fc` (worktree `mac-tier0b`):**
  - Shell harness: `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 .build/release/CoreVideoProShell`
    → **236 cases, 3961 shell checks passed, 0 failed.**
  - Native: `native/build-metal/corevideo-native-tests` (config Metal + AVF encoder + CoreAudio + MAC_VIRTUALCAM,
    Release, binary dated 2026-11-15 19:06; later commits are docs only) → **1983 tests passed, 0 failed**
    (whole binary, realtime cases included). Media filters
    (`MediaTransports.*:MediaPlaybackTimeline.*:MediaVideoPresentation.*:MediaAudioWindows.*:ProgramPixelContinuity.*`)
    → 39 passed.
  - **Not measured by the plan author:** `scripts/mac-show-drill.py --load 8`. It writes recordings and
    evidence into `artifacts/show-drill/`, which the plan brief forbids the author to create. Task 4 Step 1
    measures it first, on the same quiet machine, as the no-clip baseline.
- **Builds on this Mac.** Native config for every native task (add the new option from Task 2 on):
  `cmake -S native -B native/build-metal -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOREVIDEO_STUB=OFF -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DCOREVIDEO_WITH_METAL=ON -DCOREVIDEO_WITH_AVF_ENCODER=ON -DCOREVIDEO_WITH_COREAUDIO=ON -DCOREVIDEO_WITH_MAC_VIRTUALCAM=ON -DCOREVIDEO_WITH_AVF_MEDIA_DECODER=ON`,
  build `cmake --build native/build-metal -j 8 --target corevideo-native corevideo-native-tests corevideo-zoom-engine-fake`.
  The stub config must stay green too: `cmake -S native -B native/build-stub -G Ninja -DCMAKE_BUILD_TYPE=Release`
  then `cmake --build native/build-stub -j 8 && ctest --test-dir native/build-stub -j 6`.
  After any change to `Interfaces.h` rebuild with `--clean-first` before believing a crash (CLAUDE.md,
  "Build gotcha"). Shell: the harness command above; `bash scripts/lint-mac-design.sh` must stay green;
  attach a `COREVIDEO_SHELL_TAB_SNAPSHOT` PNG for every visible change.
- **gtest shim:** the vendored gtest has no `GTEST_SKIP`. A test that cannot run prints
  `[  SKIPPED ] <Suite>.<Name>: <reason>` to stderr and returns (the `MediaFoundationMediaFrameSource`
  FFmpeg cases at `MediaCoreCommandTest.cpp:4320-4325` are the model). Never skip silently.
- **Windows inertness.** No task changes Windows behaviour. The only shared files touched are additive:
  `Interfaces.h` (one declaration), `ModuleComposition.cpp` (an `else if` after the MF factory plus one
  capability record), `StubModules.cpp` (null factory + `omitted` record), `Protocol.h`, `CoreStateCapture.cpp`
  (one `factory(...)` name), `native/CMakeLists.txt`, and test gates. Task 2 lists the Windows check.

## Lanes and order

| Task | Slice | Lane | Depends on | Can run in parallel with |
|---|---|---|---|---|
| 1 | N1 | native | — | 7, 8, 9 |
| 2 | N1 | native | 1 | shell lane |
| 3 | N1 | native | 2 | shell lane |
| 4 | N1 | native | 3 | shell lane |
| 5 | N1 (consumer) | shell | — (live proof needs 2) | native lane |
| 6 | N2 | native | 2 | shell lane |
| 7 | N2 | native | 6 | shell lane |
| 8 | S3 | shell | — | native lane |
| 9 | S3 | shell | 8 | native lane |
| 10 | S1 | shell | 8 | native lane |
| 11 | S1 | shell | 9, 10 | native lane |
| 12 | S2 | shell | 11 (live proof needs 2) | native lane |
| 13 | S2 | shell | 12 | native lane |

Shell tasks all touch `AppModel.swift`, so the shell lane is serial: 5, 8, 9, 10, 11, 12, 13. The native
lane is serial: 1, 2, 3, 4, 6, 7. The two lanes run in parallel. Slice PRs: **N1 = Tasks 1-5**, **N2 = 6-7**,
**S3 = 8-9**, **S1 = 10-11**, **S2 = 12-13**. A slice PR merges only with its consumer task (rule: no
unwired foundation).

---

### Task 1: `MediaClipDecoder` — the portable decoder state machine (video)

**Lane:** native. **Slice:** N1.

**Files:**
- NEW `native/src/modules/MediaClipDecoder.h`, NEW `native/src/modules/MediaClipDecoder.cpp`
- NEW `native/tests/MediaClipDecoderTest.cpp`
- Modify `native/CMakeLists.txt`: add `src/modules/MediaClipDecoder.cpp` to `corevideo_native` (every
  config, it is portable C++20) and `tests/MediaClipDecoderTest.cpp` to `corevideo-native-tests`.

**Interfaces:**

```cpp
// native/src/modules/MediaClipDecoder.h
#pragma once
#include "modules/Interfaces.h"
#include "modules/MediaVideoPresentation.h"
#include <functional>
#include <memory>
#include <string>

namespace corevideo::modules {

enum class ClipReadStatus { Ready, Pending, EndOfStream, Failed };

struct ClipVideoSample {
  std::shared_ptr<const std::vector<uint8_t>> bgra;  // tightly packed, stride width*4, STRAIGHT alpha
  int width = 0, height = 0;
  int64_t pts100ns = 0;        // media time within THIS reader, 0 at the start of the file
  int64_t duration100ns = 0;
};

struct ClipAudioSample {       // used from Task 6
  std::vector<float> pcm;      // interleaved float32, 48000 Hz, 2 channels
  int64_t pts100ns = 0;        // media time within THIS reader, offset by the reader's start
};

// A reader NEVER blocks. take() hands over at most one decoded sample.
//  Ready       -> `out` filled, sample consumed.
//  Pending     -> nothing yet (opening, or the reader queue is behind).
//  EndOfStream -> sticky.
//  Failed      -> sticky; `error` is an operator-facing cause.
// Destroying a reader cancels its work and returns only after its queue is
// drained: no callback may run after the destructor returns.
class IClipVideoReader {
 public:
  virtual ~IClipVideoReader() = default;
  virtual ClipReadStatus take(ClipVideoSample& out, std::string& error) = 0;
  [[nodiscard]] virtual int64_t duration100ns() const = 0;  // -1 until the container says
};
class IClipAudioReader {
 public:
  virtual ~IClipAudioReader() = default;
  virtual ClipReadStatus take(ClipAudioSample& out, std::string& error) = 0;
};

class IClipReaderFactory {
 public:
  virtual ~IClipReaderFactory() = default;
  // Returns at once; opening runs off the caller's thread. `wake` is called with
  // NO reader lock held whenever take() may return something new. nullptr with a
  // non-empty `error` = the file cannot be opened at all (missing, unsupported
  // container); the error is worded for the operator.
  virtual std::unique_ptr<IClipVideoReader> openVideo(const std::string& path,
      std::function<void()> wake, std::string& error) = 0;
  // Task 6. nullptr with EMPTY error = the asset has no audio track (not a failure).
  virtual std::unique_ptr<IClipAudioReader> openAudio(const std::string& path, int64_t start100ns,
      std::function<void()> wake, std::string& error) {
    (void)path; (void)start100ns; (void)wake; (void)error;
    return nullptr;
  }
  // Background stills (kind "media-background" on a still path): decode once,
  // straight-alpha BGRA into frame.pixels.
  virtual bool decodeStill(const std::string& path, VideoFrame& frame, std::string& error) = 0;
};

class MediaClipDecoder final : public IMediaDecoder, public IMediaVideoPrefetch {
 public:
  // `nowMs` is the steady clock in milliseconds; tests inject one. It is used
  // ONLY by playbackPositionMs(); every poll takes its time as an argument.
  explicit MediaClipDecoder(std::shared_ptr<IClipReaderFactory> readers,
                            std::function<int64_t()> nowMs = {});
  ~MediaClipDecoder() override;
  std::vector<VideoFrame> pollMediaFrames(const MediaDecodeRequest& request, int64_t timestampMs) override;
  std::vector<VideoFrame> pollMediaFramesAt100ns(const MediaDecodeRequest& request, int64_t timestamp100ns) override;
  std::vector<AudioFrame> pollMediaAudioFrames(const MediaDecodeRequest& request, int64_t timestampMs) override;
  [[nodiscard]] std::vector<std::string> warnings() const override;
  void setMediaWakeCallback(std::function<void()> callback) override;
  std::vector<ScheduledMediaVideo> prefetchMediaVideo(const MediaDecodeRequest& request, int64_t nowMs) override;
  void syncMediaClock(const MediaDecodeRequest& request, int64_t nowMs) override;
  [[nodiscard]] int64_t playbackPositionMs() const override;
  [[nodiscard]] int64_t mediaDurationMs() const override;
  [[nodiscard]] bool mediaEnded() const override;
  // Test-only observation, never read by production code.
  [[nodiscard]] int videoReadersOpenedForTest() const;
};

}  // namespace corevideo::modules
```

**Behaviour** (each rule cites the Media Foundation line it ports; MF is
`native/src/modules/MediaFoundationMediaFrameSourceAdapter.cpp`, abbreviated `MF:`):

- **Identity** is normalized path + loop flag (`normalizeMediaAssetPath`, `"|loop"`/`"|once"`), never
  `playing` (MF:733-747). A new identity cancels and destroys the current readers and opens a new reader
  at 0 on the next poll. **Deviation from MF, deliberate:** frame ids keep rising across an identity
  change (MF resets `frameId` to 0 with `state = {}`); rising ids can never collide with
  `MediaVideoPresentation`'s repeat dedup.
- **Clock:** one `MediaPlaybackTimeline`, `configure(identity, playing, nowMs*10000)` on every
  `syncMediaClock`, `prefetchMediaVideo`, poll and audio poll that reaches the clip (MF:632-637, 754).
- **Source id stamped** on every frame: `request.sourceId` if non-empty, else `"media:" + assetId` (MF:177-179).
- **Still on a `media-video` request** returns no frames and opens nothing (`isStillImageMediaAsset`,
  MF:660-663). **Still on a `media-background` request**: `decodeStill` once, hold that frame forever
  (MF:776-789); a decode failure warns `"Media asset <id> could not be decoded as an image."`.
- **Never played and not playing (a cue poster):** open the reader if needed, take exactly one sample,
  hold it; further polls return the same frame and call `take()` no more (MF:800-819).
- **Playing:** mark `wasPlaying`. Prefetch path takes at most one new sample per call and returns it with
  `due100ns = clock.epoch100ns() + videoLoopOffset + sample.pts100ns` (MF:639-652). Direct-poll path
  (`pollMediaFramesAt100ns`) presents by media time: up to 4 takes per poll, a sample becomes the
  presented frame once `clock.videoDue(loopOffset + pts, t)`; one sample of lookahead is kept pending
  (MF:925-935). A cue that starts playing continues from the reader's next sample, no reopen (MF:820-824).
- **Paused after rolling:** take nothing; return the presented frame if the direct path presented one,
  else the last decoded frame, same `frameId` (MF:790-799).
- **End of a clip (not loop):** reader returns `EndOfStream` → hold the last frame, `mediaEnded()` true
  (MF:902-910, 626-630). A reader that only returns `Pending` is a stall, never an end.
- **Loop:** `mediaEnded()` is never true. When the decoder takes a sample with
  `pts + duration >= duration100ns() - 3'000'000` (300 ms before the end, duration known), it opens the
  NEXT reader (pre-open). On `EndOfStream` it switches to the pre-opened reader (opening one then if
  none was pre-opened), adds `lastPts + lastDuration` to `videoLoopOffset`, keeps the last frame on air
  (MF:885-901, plus the pre-open, which is new; spec §7 "loop seam" risk).
- **Position:** `-1` before the first request reaches the clip; else `elapsed = clock.elapsed100ns(nowMs()*10000)`,
  clamped at 0, wrapped `% duration` for a loop and clamped to `duration` for a clip when the duration
  is known, raw elapsed when it is not; returned in ms (MF:599-616). **Duration:** reader's
  `duration100ns()/10000`, `-1` while unknown (MF:617-621).
- **Warnings** are rebuilt per poll (MF:656, 674). Wording copied from MF so the shell and support
  bundle read the same text on both platforms:
  - open failure while playing: `"Media asset <id> could not be opened for Program playback: <error>"` (MF:872-875);
  - open failure while cueing: `"Media asset <id> could not be opened for Preview cueing: <error>"` (MF:806-809);
  - reader `Failed` mid-stream: `"Media asset <id> failed while decoding a video frame."` (MF:1155);
  - loop reopen failure: `"Media background <id> could not restart its loop: <error>"` (MF:891-894).
  Every open failure also writes one `[media-decoder] <path>: NO DECODER - <error>` line through
  `core::nativeLogf`, once per open attempt, never per poll (MF:1040-1046; `docs/reference/media-no-decoder.md`).
- **Wake relay:** the decoder owns a `shared_ptr<WakeRelay>` (mutex + `std::function`).
  `setMediaWakeCallback` stores into it; every reader gets a lambda holding a `weak_ptr<WakeRelay>`.
  The destructor clears the relay **before** destroying readers, so a reader callback racing teardown
  calls nothing (spec §7 "reader callbacks outliving a retired decoder").
- **Threading:** all public methods are called from the one `MediaTransports` worker thread
  (`MediaTransports.h:605-760`); only the wake relay is touched from other threads. No method blocks.

**Complete tests** — `native/tests/MediaClipDecoderTest.cpp`. The scripted fake honours the reader
contract (rule 9): it never blocks, it calls `wake` when it makes a sample available, and its destructor
records that it ran.

```cpp
#include "modules/MediaClipDecoder.h"
#include <gtest/gtest.h>
#include <atomic>
#include <memory>

using namespace corevideo::modules;

namespace {
// Fixture invariants (rule 1): every script has >= 1 sample; pts are strictly
// increasing and start at 0; durations are equal (a 30 fps clip = 333'333);
// a sample's colour byte is its INDEX in the script + 1 (so index 0 -> 0x01),
// which makes "which sample is on air" independent of the code under test.
constexpr int64_t kFrame = 333'333;
struct Script {
  int samples = 10;
  int pendingBeforeFirst = 0;   // take() returns Pending this many times first
  bool failOpen = false;        // openVideo returns nullptr + error
  int failAfter = -1;           // take() returns Failed after this many samples
  bool stallAfterLast = false;  // Pending forever instead of EndOfStream
  int64_t duration100ns = 10 * kFrame;
};
struct Shared {
  int opened = 0, takesReady = 0, stillDecodes = 0, destroyed = 0;
  std::function<void()> lastWake;
  Script script;
};
class FakeReader final : public IClipVideoReader {
 public:
  explicit FakeReader(std::shared_ptr<Shared> s, std::function<void()> wake) : s_(std::move(s)) {
    s_->lastWake = std::move(wake);
  }
  ~FakeReader() override { s_->destroyed += 1; }
  ClipReadStatus take(ClipVideoSample& out, std::string& error) override {
    if (pending_ < s_->script.pendingBeforeFirst) { ++pending_; return ClipReadStatus::Pending; }
    if (s_->script.failAfter >= 0 && next_ >= s_->script.failAfter) { error = "decode error"; return ClipReadStatus::Failed; }
    if (next_ >= s_->script.samples)
      return s_->script.stallAfterLast ? ClipReadStatus::Pending : ClipReadStatus::EndOfStream;
    out.width = 4; out.height = 2;
    out.bgra = std::make_shared<std::vector<uint8_t>>(4u * 2u * 4u, static_cast<uint8_t>(next_ + 1));
    out.pts100ns = next_ * kFrame; out.duration100ns = kFrame;
    ++next_; s_->takesReady += 1;
    return ClipReadStatus::Ready;
  }
  int64_t duration100ns() const override { return s_->script.duration100ns; }
 private:
  std::shared_ptr<Shared> s_;
  int pending_ = 0, next_ = 0;
};
class FakeReaders final : public IClipReaderFactory {
 public:
  explicit FakeReaders(std::shared_ptr<Shared> s) : s_(std::move(s)) {}
  std::unique_ptr<IClipVideoReader> openVideo(const std::string& path, std::function<void()> wake,
                                              std::string& error) override {
    if (s_->script.failOpen) { error = "AVFoundation cannot open " + path; return nullptr; }
    s_->opened += 1;
    return std::make_unique<FakeReader>(s_, std::move(wake));
  }
  bool decodeStill(const std::string&, VideoFrame& frame, std::string&) override {
    s_->stillDecodes += 1;
    frame.width = frame.pixelWidth = frame.naturalWidth = 2;
    frame.height = frame.pixelHeight = frame.naturalHeight = 2;
    frame.pixelStride = 8;
    frame.pixels = std::make_shared<std::vector<uint8_t>>(16u, 0x7f);
    return true;
  }
 private:
  std::shared_ptr<Shared> s_;
};
MediaDecodeRequest clip(bool playing, bool loop = false, const std::string& path = "/m/clip.mp4") {
  MediaDecodeRequest r;
  r.kind = "media-video"; r.sourceId = "media:clip"; r.assetId = "clip";
  r.assetKind = "stinger"; r.assetPath = path; r.playing = playing; r.loop = loop;
  return r;
}
uint8_t colour(const VideoFrame& f) { return (*f.pixels)[0]; }
struct Rig {
  std::shared_ptr<Shared> s = std::make_shared<Shared>();
  int64_t now = 1'000;
  std::unique_ptr<MediaClipDecoder> d;
  explicit Rig(Script script = {}) {
    s->script = script;
    d = std::make_unique<MediaClipDecoder>(std::make_shared<FakeReaders>(s), [this] { return now; });
  }
  // Prefetch until a sample arrives (fake is synchronous; Pending only by script).
  ScheduledMediaVideo next(const MediaDecodeRequest& r) {
    for (int i = 0; i < 20; ++i) {
      auto v = d->prefetchMediaVideo(r, now);
      if (!v.empty()) return v.front();
    }
    ADD_FAILURE() << "no sample in 20 prefetches";
    return {};
  }
};
}  // namespace

TEST(MediaClipDecoder, ACuePosterDecodesExactlyOneFrameAndHoldsIt) {
  Rig rig;
  for (int i = 0; i < 25; ++i) {
    const auto frames = rig.d->pollMediaFrames(clip(false), 1'000 + i * 4);
    ASSERT_EQ(frames.size(), 1u) << "poll " << i;
    EXPECT_EQ(frames.front().frameId, 1);
    EXPECT_EQ(colour(frames.front()), 0x01);
    EXPECT_EQ(frames.front().participantId, "media:clip");
  }
  EXPECT_EQ(rig.s->takesReady, 1) << "a poster reads ONE sample";
  EXPECT_EQ(rig.s->opened, 1);
}

TEST(MediaClipDecoder, ACueThatRollsContinuesFromTheSecondSampleWithoutReopening) {
  Rig rig;
  (void)rig.d->pollMediaFrames(clip(false), 1'000);
  rig.d->syncMediaClock(clip(true), 1'000);
  const auto s = rig.next(clip(true));
  EXPECT_EQ(s.frame.frameId, 2);
  EXPECT_EQ(colour(s.frame), 0x02) << "the reader continued; a reopen would show 0x01";
  EXPECT_EQ(rig.s->opened, 1);
}

TEST(MediaClipDecoder, PlayingFramesCarryDueTimeEpochPlusPts) {
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);            // epoch = 1000 ms = 10'000'000 x100ns
  for (int i = 0; i < 3; ++i) {
    const auto s = rig.next(clip(true));
    EXPECT_EQ(s.frame.frameId, i + 1);
    EXPECT_EQ(s.due100ns, 10'000'000 + i * kFrame) << "sample " << i;
  }
}

TEST(MediaClipDecoder, PausedAfterRollingReadsNothingAndHoldsTheFrame) {
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);
  for (int i = 0; i < 5; ++i) (void)rig.next(clip(true));
  const int takesBefore = rig.s->takesReady;            // == 5
  rig.d->syncMediaClock(clip(false), 1'200);
  for (int i = 0; i < 30; ++i) {
    const auto frames = rig.d->pollMediaFrames(clip(false), 1'200 + i * 16);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().frameId, 5);
    EXPECT_EQ(colour(frames.front()), 0x05);
  }
  EXPECT_EQ(rig.s->takesReady, takesBefore) << "a paused clip reads nothing";
}

TEST(MediaClipDecoder, ResumeContinuesWithTheNextSampleOnTheShiftedClock) {
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);
  for (int i = 0; i < 5; ++i) (void)rig.next(clip(true));
  rig.d->syncMediaClock(clip(false), 1'200);             // paused at 200 ms elapsed
  rig.now = 1'700;
  rig.d->syncMediaClock(clip(true), 1'700);              // 500 ms paused -> epoch 1500 ms
  const auto s = rig.next(clip(true));
  EXPECT_EQ(s.frame.frameId, 6) << "bounded below: not a hold; bounded above: no skip";
  EXPECT_EQ(colour(s.frame), 0x06) << "a restart from 0 would be 0x01";
  EXPECT_EQ(s.due100ns, 15'000'000 + 5 * kFrame);
  EXPECT_EQ(rig.s->opened, 1) << "resume never reopens";
}

TEST(MediaClipDecoder, AClipHoldsItsLastFrameAndReportsEndedAtEndOfStream) {
  Script script; script.samples = 3; script.duration100ns = 3 * kFrame;
  Rig rig(script);
  rig.d->syncMediaClock(clip(true), 1'000);
  for (int i = 0; i < 3; ++i) (void)rig.next(clip(true));
  EXPECT_FALSE(rig.d->mediaEnded()) << "EOS has not been READ yet";
  (void)rig.d->prefetchMediaVideo(clip(true), 1'200);    // reads EOS
  EXPECT_TRUE(rig.d->mediaEnded());
  const auto held = rig.d->pollMediaFramesAt100ns(clip(true), 13'000'000);
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held.front().frameId, 3);
}

TEST(MediaClipDecoder, AStalledReaderNeverReportsEndedAndKeepsTheHeldFrame) {
  Script script; script.samples = 2; script.stallAfterLast = true;
  Rig rig(script);
  rig.d->syncMediaClock(clip(true), 1'000);
  (void)rig.next(clip(true)); (void)rig.next(clip(true));
  for (int t = 0; t < 7'000; t += 100) {
    rig.now = 1'000 + t;
    (void)rig.d->prefetchMediaVideo(clip(true), rig.now);
  }
  EXPECT_FALSE(rig.d->mediaEnded()) << "a stall is not the end of the media";
  const auto held = rig.d->pollMediaFrames(clip(false), rig.now);
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held.front().frameId, 2);
}

TEST(MediaClipDecoder, ALoopPreopensItsNextReaderAndAdvancesTheOffset) {
  // 3 samples of 333'333 = duration 999'999; the pre-open threshold is
  // duration - 3'000'000 < 0, so the FIRST sample taken already pre-opens.
  Script script; script.samples = 3; script.duration100ns = 3 * kFrame;
  Rig rig(script);
  rig.d->syncMediaClock(clip(true, /*loop=*/true), 1'000);
  (void)rig.next(clip(true, true));
  EXPECT_EQ(rig.s->opened, 2) << "the next reader is opened before the end";
  (void)rig.next(clip(true, true)); (void)rig.next(clip(true, true));
  const auto s = rig.next(clip(true, true));             // first sample of loop 2
  EXPECT_EQ(s.frame.frameId, 4) << "ids keep rising across the seam";
  EXPECT_EQ(colour(s.frame), 0x01) << "loop 2 starts at the top of the file";
  EXPECT_EQ(s.due100ns, 10'000'000 + 3 * kFrame) << "offset = lastPts + lastDuration";
  EXPECT_FALSE(rig.d->mediaEnded()) << "a loop never ends";
}

TEST(MediaClipDecoder, PositionWrapsForALoopClampsForAClipAndIsMinusOneBeforeAnyRequest) {
  Script script; script.duration100ns = 30'000'000;      // 3 s
  Rig loop(script), once(script);
  EXPECT_EQ(once.d->playbackPositionMs(), -1);
  EXPECT_EQ(once.d->mediaDurationMs(), -1) << "no reader yet";
  loop.d->syncMediaClock(clip(true, true), 1'000);  (void)loop.next(clip(true, true));
  once.d->syncMediaClock(clip(true, false), 1'000); (void)once.next(clip(true, false));
  loop.now = once.now = 1'000 + 3'400;
  EXPECT_EQ(loop.d->playbackPositionMs(), 400);
  EXPECT_EQ(once.d->playbackPositionMs(), 3'000);
  EXPECT_EQ(once.d->mediaDurationMs(), 3'000);
}

TEST(MediaClipDecoder, AnUnknownDurationReportsRawElapsedAndMinusOneDuration) {
  Script script; script.duration100ns = -1;
  Rig rig(script);
  rig.d->syncMediaClock(clip(true, true), 1'000); (void)rig.next(clip(true, true));
  rig.now = 1'000 + 3'400;
  EXPECT_EQ(rig.d->playbackPositionMs(), 3'400);
  EXPECT_EQ(rig.d->mediaDurationMs(), -1);
}

TEST(MediaClipDecoder, AStillOnAMediaVideoRequestReturnsNothingAndOpensNothing) {
  Rig rig;
  auto r = clip(true, false, "/m/logo.png");
  EXPECT_TRUE(rig.d->pollMediaFrames(r, 1'000).empty());
  EXPECT_TRUE(rig.d->prefetchMediaVideo(r, 1'000).empty());
  EXPECT_EQ(rig.s->opened, 0);
  EXPECT_EQ(rig.s->stillDecodes, 0);
}

TEST(MediaClipDecoder, ABackgroundStillDecodesOnceAndHolds) {
  Rig rig;
  auto r = clip(true, true, "/m/backdrop.png");
  r.kind = "media-background"; r.sourceId = "background:bg"; r.assetId = "bg";
  for (int i = 0; i < 10; ++i) {
    const auto frames = rig.d->pollMediaFrames(r, 1'000 + i * 16);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().participantId, "background:bg");
  }
  EXPECT_EQ(rig.s->stillDecodes, 1);
  EXPECT_EQ(rig.s->opened, 0);
}

TEST(MediaClipDecoder, OpenFailuresWarnWithTheMediaFoundationWording) {
  Script script; script.failOpen = true;
  Rig rig(script);
  EXPECT_TRUE(rig.d->pollMediaFrames(clip(true), 1'000).empty());
  ASSERT_EQ(rig.d->warnings().size(), 1u);
  EXPECT_EQ(rig.d->warnings()[0],
            "Media asset clip could not be opened for Program playback: AVFoundation cannot open /m/clip.mp4");
  Rig cue(script);
  EXPECT_TRUE(cue.d->pollMediaFrames(clip(false), 1'000).empty());
  ASSERT_EQ(cue.d->warnings().size(), 1u);
  EXPECT_EQ(cue.d->warnings()[0],
            "Media asset clip could not be opened for Preview cueing: AVFoundation cannot open /m/clip.mp4");
}

TEST(MediaClipDecoder, AMidStreamFailureWarnsAndHoldsTheLastFrame) {
  Script script; script.failAfter = 2;
  Rig rig(script);
  rig.d->syncMediaClock(clip(true), 1'000);
  (void)rig.next(clip(true)); (void)rig.next(clip(true));
  EXPECT_TRUE(rig.d->prefetchMediaVideo(clip(true), 1'100).empty());
  ASSERT_FALSE(rig.d->warnings().empty());
  EXPECT_EQ(rig.d->warnings()[0], "Media asset clip failed while decoding a video frame.");
  const auto held = rig.d->pollMediaFrames(clip(false), 1'200);
  ASSERT_EQ(held.size(), 1u);
  EXPECT_EQ(held.front().frameId, 2);
}

TEST(MediaClipDecoder, TheDirectPollPathPresentsByMediaTime) {
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);
  // 340 ms elapsed: samples at 0 and 333.3 ms are due, 666.7 ms is not.
  const auto at340 = rig.d->pollMediaFramesAt100ns(clip(true), 10'000'000 + 3'400'000);
  ASSERT_EQ(at340.size(), 1u);
  EXPECT_EQ(at340.front().frameId, 2);
  EXPECT_EQ(colour(at340.front()), 0x02);
  const auto again = rig.d->pollMediaFramesAt100ns(clip(true), 10'000'000 + 3'500'000);
  ASSERT_EQ(again.size(), 1u);
  EXPECT_EQ(again.front().frameId, 2) << "a repeated poll holds the presented frame";
}

TEST(MediaClipDecoder, AnIdentityChangeOpensANewReaderAndIdsKeepRising) {
  Rig rig;
  rig.d->syncMediaClock(clip(true), 1'000);
  for (int i = 0; i < 3; ++i) (void)rig.next(clip(true));
  auto other = clip(true, false, "/m/other.mp4");
  rig.d->syncMediaClock(other, 1'100);
  const auto s = rig.next(other);
  EXPECT_EQ(rig.s->opened, 2);
  EXPECT_EQ(colour(s.frame), 0x01) << "the new file starts at its top";
  EXPECT_EQ(s.frame.frameId, 4) << "ids never restart inside one decoder";
  EXPECT_EQ(rig.s->destroyed, 1) << "the old reader was destroyed";
}

TEST(MediaClipDecoder, AReaderWakeReachesTheOwnerAndNeverAfterDestruction) {
  Rig rig;
  std::atomic<int> wakes{0};
  rig.d->setMediaWakeCallback([&] { wakes.fetch_add(1); });
  (void)rig.d->pollMediaFrames(clip(false), 1'000);
  ASSERT_TRUE(static_cast<bool>(rig.s->lastWake));
  rig.s->lastWake();
  EXPECT_EQ(wakes.load(), 1);
  auto lateWake = rig.s->lastWake;                       // a reader callback still in flight
  rig.d.reset();
  lateWake();
  EXPECT_EQ(wakes.load(), 1) << "no wake may reach a retired decoder's owner";
}
```

- [ ] **Step 1:** Write the test file and register it. Run
  `native/build-metal/corevideo-native-tests --gtest_filter='MediaClipDecoder.*'`. Expected: build fails
  (header missing).
- [ ] **Step 2:** Implement `MediaClipDecoder` (video half; `pollMediaAudioFrames` returns `{}` until Task 6).
- [ ] **Step 3:** Run the filter on `build-metal` and on `build-stub`. Expected: 17/17 pass on both.
- [ ] **Step 4:** Full suites: `ctest --test-dir native/build-metal -j 6` and `ctest --test-dir native/build-stub -j 6`. Expected: green; native count = baseline + 17.

**Mutations to run** (report each; each must go red):
- Reopen on paused→playing (call `openVideo` on every play transition): `ResumeContinues…` and `ACueThatRolls…` red on `opened`/colour.
- Put `playing` into the identity string: the same two red.
- Take a sample while paused: `PausedAfterRolling…` red on `takesReady`.
- Drop `videoLoopOffset` accumulation: `ALoopPreopens…` red on `due100ns`.
- Remove the pre-open (open only at EOS): `ALoopPreopens…` red on `opened == 2`.
- Report `mediaEnded()` for a loop: `ALoopPreopens…` red.
- Reset frame ids on loop or on identity change: `ALoopPreopens…` / `AnIdentityChange…` red.
- Treat `Pending` as end after N polls: `AStalledReader…` red.
- Clamp instead of wrap for a loop: `PositionWraps…` red on 400.
- Remove the still filter: `AStillOnAMediaVideoRequest…` red.
- Clear the wake relay after (not before) destroying readers, or relay through a raw `this`: `AReaderWake…` red (run under `-fsanitize=address` if it passes, and report).
- Direct path presents every sample it takes (no due check): `TheDirectPollPath…` red (id 3).

**Done when:** 17 tests green on Metal and stub builds, all mutations reported red, nothing else in the
tree calls `MediaClipDecoder` yet (it lands with Task 2 in the N1 PR).

---

### Task 2: AVFoundation readers, the factory, the build option and the capability

**Lane:** native. **Slice:** N1.

**Files:**
- NEW `native/src/modules/AVFoundationMediaDecoderAdapter.mm` (ARC). Body gated
  `#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AVF_MEDIA_DECODER`, `#else` the
  factory returns `{}` (the `AVFoundationEncoderAdapter.mm:20, 1018-1032` pattern).
- Modify `native/src/modules/Interfaces.h` (declare the factory beside the MF one, `:2086-2087`).
- Modify `native/src/modules/StubModules.cpp` (null factory inside the existing `#if !defined(__APPLE__)`
  block at `:700`; record `media-clip-playback` `omitted` in `createStubModules`'s loop at `:675-679`).
- Modify `native/src/modules/ModuleComposition.cpp:440-442` (try MF, then AVF; record the capability).
- Modify `native/src/core/Protocol.h` (`kNativeMediaCoreCapabilities` 24 → 25, add `"media-clip-playback"`).
- Modify `native/src/core/CoreStateCapture.cpp` (add `"media-clip-playback"` to the `factory(...)` loop
  at `:268-270`). Do **not** touch the unused copy at `MediaCore.cpp:241`.
- Modify `native/CMakeLists.txt`: option `COREVIDEO_WITH_AVF_MEDIA_DECODER` ("Build the macOS AVFoundation
  media clip decoder. Requires COREVIDEO_ENABLE_DEV_ADAPTERS."), FATAL_ERROR unless dev adapters and
  APPLE (the `:310-319` pattern), links `AVFoundation CoreMedia CoreVideo VideoToolbox`; add the `.mm` to
  the `if(APPLE)` source list at `:200-210` with `-fobjc-arc`; add the define to both
  `target_compile_definitions` lists (`:225-251`, `:1070-1092`); add NEW `tests/AvfMediaFixtures.mm` and
  NEW `tests/AvfMediaDecoderTest.cpp` to the test target under
  `if(COREVIDEO_WITH_AVF_MEDIA_DECODER AND NOT COREVIDEO_STUB)` (the `:1040-1044` pattern).
- Modify `scripts/run-mac-shell.sh:8-14`, `scripts/release-macos.sh:178-186`, `.github/workflows/ci.yml:62-67`
  and `:280-289`: add `-DCOREVIDEO_WITH_AVF_MEDIA_DECODER=ON` wherever `-DCOREVIDEO_WITH_AVF_ENCODER=ON` is set.
- Modify `native/tests/MediaCoreCommandTest.cpp:4077`: gate becomes
  `#if !COREVIDEO_STUB && (COREVIDEO_WITH_MF_ENCODER || COREVIDEO_WITH_AVF_MEDIA_DECODER)`; add helper
  `testMediaDecoder()` (MF factory if non-empty, else AVF) and use it in `DecodesFirstFrameForPausedPreviewCue`
  (`:4087`) and `PausingMidPlaybackHoldsTheOnAirFrameAndResumeContinues` (`:4202`); wrap `:4121`, `:4253`
  (audio, Task 6), `:4315`, `:4400` (FFmpeg) in `#if COREVIDEO_WITH_MF_ENCODER` for now. The base64 MP4
  fixture (`writeMfVideoFixture`, 1 s, 30 frames, 64x64 H.264) is container-standard; AVFoundation reads it.
- NEW `native/tests/AvfMediaFixtures.h` / `.mm` (test helper, Apple only).

**Interfaces:**

```cpp
// Interfaces.h, beside createMediaFoundationMediaDecoderFactory():
// One AVFoundation-backed MediaClipDecoder per call; empty unless COREVIDEO_WITH_AVF_MEDIA_DECODER.
std::function<std::unique_ptr<IMediaDecoder>()> createAvFoundationMediaDecoderFactory();

// AVFoundationMediaDecoderAdapter.mm (not in Interfaces.h): the reader seam's Apple side.
std::shared_ptr<IClipReaderFactory> createAvFoundationClipReaderFactory();

// native/tests/AvfMediaFixtures.h — every fixture is written at test time, nothing binary is committed.
namespace corevideo::testing {
struct FlatSegment { int frames; uint8_t b, g, r, a = 0xff; };
// H.264 (or ProRes 4444 when `prores4444`) MP4/MOV from BGRA source buffers. Returns "" on success, else why.
std::string writeFlatColourClip(const std::string& path, int width, int height, int fps,
                                const std::vector<FlatSegment>& segments, bool prores4444 = false);
// H.264 MP4 from a '420v' (BT.709 limited) source with four 32x32 quadrants of KNOWN Y/Cb/Cr:
// TL (16,128,128) black, TR (235,128,128) white, BL (173,42,26) BT.709 green, BR (63,102,240) BT.709 red.
std::string writeBt709BarsClip(const std::string& path, int frames, int fps);
}
```

**Behaviour (AVFoundation side):**
- `openVideo`: returns a reader immediately. On its private serial queue it loads the asset
  (`loadTracksWithMediaType:` + `load(.duration)`; never the deprecated sync accessors), creates an
  `AVAssetReader` + `AVAssetReaderTrackOutput` with
  `kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA` and `AVVideoColorPropertiesKey`
  {primaries, transfer, matrix all `ITU_R_709_2`}, `alwaysCopiesSampleData = NO`, and starts it.
  Missing file → `take()` `Failed` with "the file does not exist". A file AVFoundation cannot open →
  `Failed` with "AVFoundation cannot open this file (<NSError localizedDescription>)"; for `.webm` the
  error is D1's sentence. **No video track** → `Failed` "the file has no video track".
- The queue keeps **at most one decoded sample ready** (spec §4.1 "one sample beyond the owner's request"):
  after `take()` consumes it, the queue copies the next one. `copyNextSampleBuffer` runs only on that
  queue. Each `CVPixelBuffer` is copied row by row into a tightly packed `std::vector<uint8_t>`
  (`pixelStride = width*4`, `core::tryMakeFrameBuffer`, holding the last frame on allocation failure
  like MF:1171-1178). When `wake` fires, no reader lock is held (MF:93-102).
- **Alpha:** if the track's format description does not contain an alpha channel
  (`kCMFormatDescriptionExtension_ContainsAlphaChannel` absent/false), the frame is opaque. Do **not**
  add a per-pixel alpha pass unless the opaque test below shows VideoToolbox left alpha ≠ 0xFF. For an
  alpha track the bytes must reach the owner as **straight** alpha; if the alpha test shows premultiplied
  output, un-premultiply on the reader queue (alpha codecs only).
- `duration100ns()` from the asset duration once loaded, else -1.
- **Teardown:** the reader destructor sets a cancelled flag, calls `cancelReading`, then
  `dispatch_sync`s an empty block onto its queue so nothing it scheduled can run after it returns.
- **Diagnostics:** one `nativeVerboseLogf("[media-decoder] copy avg %.2f ms max %.2f ms (%d frames, %dx%d)")`
  per 600 copied frames per reader — the measurement Task 4 reads (spec §4.1 "N1 must measure it").
- `decodeStill` uses `createPlatformStillImageDecoder()` (ImageIO on Apple, `StillMediaFrameCache.cpp:149-200`).
- Factory: `return [] { return std::make_unique<MediaClipDecoder>(createAvFoundationClipReaderFactory()); }`.
- `ModuleComposition.cpp`: `if (auto mf = createMediaFoundationMediaDecoderFactory()) {…} else if (auto avf =
  createAvFoundationMediaDecoderFactory()) {…}` then
  `recordCapability(modules, "media-clip-playback", constructed ? "available" : (COREVIDEO_WITH_MF_ENCODER ||
  COREVIDEO_WITH_AVF_MEDIA_DECODER) ? "failed-to-construct" : "omitted", COREVIDEO_STUB ? "stub-build" : "")`.
  (Add `COREVIDEO_WITH_AVF_MEDIA_DECODER` to the macro set `ModuleComposition.cpp` reads; it is defined
  `PUBLIC` on `corevideo_native`.)

**Complete tests** — `native/tests/AvfMediaDecoderTest.cpp` (gated on the option, real AVFoundation, real
clock; bounded waits on real work, never fixed sleeps that assume speed):

```cpp
#include "AvfMediaFixtures.h"
#include "modules/Interfaces.h"
#include "modules/MediaVideoPresentation.h"
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <thread>

using namespace corevideo;
namespace {
int64_t steadyMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::filesystem::path tmp(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         ("corevideo-avf-" + std::to_string(steadyMs()) + "-" + name);
}
struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code e; std::filesystem::remove(p, e); } };
modules::MediaDecodeRequest req(const std::filesystem::path& p, bool playing, bool loop = false) {
  modules::MediaDecodeRequest r;
  r.kind = "media-video"; r.sourceId = "media:fx"; r.assetId = "fx"; r.assetKind = "stinger";
  r.assetPath = p.string(); r.playing = playing; r.loop = loop;
  return r;
}
std::vector<modules::VideoFrame> pollUntilFrame(modules::IMediaDecoder& d,
    const modules::MediaDecodeRequest& r, int timeoutMs = 3000) {
  const auto deadline = steadyMs() + timeoutMs;
  while (steadyMs() < deadline) {
    auto f = d.pollMediaFrames(r, steadyMs());
    if (!f.empty()) return f;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return {};
}
std::array<int, 4> px(const modules::VideoFrame& f, int x, int y) {
  const size_t o = static_cast<size_t>(y) * f.pixelStride + static_cast<size_t>(x) * 4;
  return {(*f.pixels)[o], (*f.pixels)[o + 1], (*f.pixels)[o + 2], (*f.pixels)[o + 3]};
}
}  // namespace

TEST(AvfMediaDecoder, TheFactoryIsLiveAndTheCapabilityIsAvailable) {
  ASSERT_TRUE(static_cast<bool>(modules::createAvFoundationMediaDecoderFactory()));
  auto modulesSet = modules::createDefaultModules();
  ASSERT_TRUE(static_cast<bool>(modulesSet.mediaDecoderFactory));
  EXPECT_EQ(modulesSet.capabilityConstruction["media-clip-playback"].state, "available");
}

TEST(AvfMediaDecoder, ACuePosterIsTheFirstFrameAndOpaque) {
  // Fixture: 640x360 @30, 10 red frames then 10 blue. Red = BGRA(0x20,0x20,0xE0).
  const auto p = tmp("cue.mp4"); Cleanup c{p};
  ASSERT_EQ(testing::writeFlatColourClip(p, 640, 360, 30, {{10, 0x20, 0x20, 0xE0}, {10, 0xE0, 0x20, 0x20}}), "");
  auto d = modules::createAvFoundationMediaDecoderFactory()();
  const auto f = pollUntilFrame(*d, req(p, false));
  ASSERT_EQ(f.size(), 1u);
  EXPECT_EQ(f[0].width, 640); EXPECT_EQ(f[0].height, 360); EXPECT_EQ(f[0].pixelStride, 640 * 4);
  const auto centre = px(f[0], 320, 180);
  EXPECT_NEAR(centre[2], 0xE0, 8); EXPECT_NEAR(centre[0], 0x20, 8);
  for (size_t o = 3; o < f[0].pixels->size(); o += 4) ASSERT_EQ((*f[0].pixels)[o], 0xff) << "opaque codec";
  EXPECT_TRUE(d->warnings().empty());
}

TEST(AvfMediaDecoder, Bt709LimitedBarsDecodeToTheirKnownRgb) {
  // Independent oracle: the quadrants' Y/Cb/Cr are fixture constants; expected RGB is the
  // BT.709 limited-range inverse, computed by hand. Margins that discriminate the failure modes:
  // full-range decoding moves black to ~16 and white to ~235 (16 off); a BT.601 matrix moves
  // green's R to ~20 (20 off). Tolerance 6 < both margins.
  const auto p = tmp("bars.mp4"); Cleanup c{p};
  ASSERT_EQ(testing::writeBt709BarsClip(p, 10, 30), "");
  auto d = modules::createAvFoundationMediaDecoderFactory()();
  const auto f = pollUntilFrame(*d, req(p, false));
  ASSERT_EQ(f.size(), 1u);
  const auto black = px(f[0], 16, 16), white = px(f[0], 48, 16), green = px(f[0], 16, 48), red = px(f[0], 48, 48);
  for (int ch = 0; ch < 3; ++ch) { EXPECT_NEAR(black[ch], 0, 6); EXPECT_NEAR(white[ch], 255, 6); }
  EXPECT_NEAR(green[2], 0, 6); EXPECT_NEAR(green[1], 255, 6); EXPECT_NEAR(green[0], 0, 6);
  EXPECT_NEAR(red[2], 255, 6); EXPECT_NEAR(red[1], 0, 6); EXPECT_NEAR(red[0], 0, 6);
}

TEST(AvfMediaDecoder, AProRes4444AlphaClipReachesTheOwnerAsStraightAlpha) {
  // Source pixel BGRA(0, 0, 200, 128), straight. Premultiplied output would read R ~100.
  const auto p = tmp("alpha.mov"); Cleanup c{p};
  const auto written = testing::writeFlatColourClip(p, 64, 64, 30, {{10, 0, 0, 200, 128}}, /*prores4444=*/true);
  ASSERT_EQ(written, "") << "the alpha fixture could not be written on this OS: " << written;
  auto d = modules::createAvFoundationMediaDecoderFactory()();
  const auto f = pollUntilFrame(*d, req(p, false));
  ASSERT_EQ(f.size(), 1u);
  const auto v = px(f[0], 32, 32);
  EXPECT_NEAR(v[3], 128, 4); EXPECT_NEAR(v[2], 200, 6);
}

TEST(AvfMediaDecoder, PlayPauseResumeOnTheRealClockContinuesTheClip) {
  // The Windows oracle (MediaFoundationMediaFrameSource.PausingMidPlayback…, now re-gated) on the
  // prefetch path the owner actually uses. Fixture: 2 s @30, 60 frames.
  const auto p = tmp("pause.mp4"); Cleanup c{p};
  ASSERT_EQ(testing::writeFlatColourClip(p, 128, 72, 30, {{30, 0x20, 0x20, 0xE0}, {30, 0xE0, 0x20, 0x20}}), "");
  auto d = modules::createAvFoundationMediaDecoderFactory()();
  auto* pre = dynamic_cast<modules::IMediaVideoPrefetch*>(d.get());
  ASSERT_NE(pre, nullptr);
  auto r = req(p, true);
  pre->syncMediaClock(r, steadyMs());
  int64_t held = 0;
  for (auto deadline = steadyMs() + 3000; held < 5 && steadyMs() < deadline;) {
    for (auto& s : pre->prefetchMediaVideo(r, steadyMs())) held = s.frame.frameId;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_GE(held, 5); ASSERT_LT(held, 50);
  r.playing = false; pre->syncMediaClock(r, steadyMs());
  for (auto end = steadyMs() + 300; steadyMs() < end;) {
    const auto f = d->pollMediaFrames(r, steadyMs());
    ASSERT_EQ(f.size(), 1u); EXPECT_EQ(f[0].frameId, held);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  r.playing = true; pre->syncMediaClock(r, steadyMs());
  int64_t next = held;
  for (auto deadline = steadyMs() + 2000; next == held && steadyMs() < deadline;) {
    for (auto& s : pre->prefetchMediaVideo(r, steadyMs())) next = s.frame.frameId;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(next, held + 1) << "the next frame of the same reader, not a reopen and not a skip";
}

TEST(AvfMediaDecoder, AClipEndsOnItsLastFrameAndALoopKeepsGoingWithABoundedSeam) {
  const auto p = tmp("short.mp4"); Cleanup c{p};
  ASSERT_EQ(testing::writeFlatColourClip(p, 64, 64, 30, {{30, 0x40, 0x40, 0x40}}), "");   // 1 s
  {
    auto d = modules::createAvFoundationMediaDecoderFactory()();
    auto* pre = dynamic_cast<modules::IMediaVideoPrefetch*>(d.get());
    auto r = req(p, true); pre->syncMediaClock(r, steadyMs());
    for (auto deadline = steadyMs() + 3000; !pre->mediaEnded() && steadyMs() < deadline;) {
      (void)pre->prefetchMediaVideo(r, steadyMs());
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(pre->mediaEnded());
    EXPECT_EQ(pre->mediaDurationMs(), 1000);
    const auto held = d->pollMediaFrames(r, steadyMs());
    ASSERT_EQ(held.size(), 1u); EXPECT_EQ(held[0].frameId, 30);
  }
  {
    auto d = modules::createAvFoundationMediaDecoderFactory()();
    auto* pre = dynamic_cast<modules::IMediaVideoPrefetch*>(d.get());
    auto r = req(p, true, /*loop=*/true); pre->syncMediaClock(r, steadyMs());
    // Consume at presentation pace through MediaVideoPresentation, exactly as the owner does,
    // and measure the longest wall gap between new on-air frames across 2.5 s (2 seams).
    modules::MediaVideoPresentation presentation;
    int64_t lastId = 0, lastChangeMs = steadyMs(), worstGapMs = 0, newFrames = 0;
    for (auto end = steadyMs() + 2500; steadyMs() < end;) {
      if (presentation.hasRoom()) for (auto& s : pre->prefetchMediaVideo(r, steadyMs())) presentation.push(std::move(s));
      const auto& on = presentation.select(steadyMs() * 10000);
      if (on.hasPixels() && on.frameId != lastId) {
        if (lastId != 0) worstGapMs = std::max(worstGapMs, steadyMs() - lastChangeMs);
        lastId = on.frameId; lastChangeMs = steadyMs(); ++newFrames;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::fprintf(stderr, "[avf-loop-seam] worst gap %lld ms over %lld frames\n",
                 static_cast<long long>(worstGapMs), static_cast<long long>(newFrames));
    EXPECT_FALSE(pre->mediaEnded());
    EXPECT_GE(newFrames, 66) << "2.5 s @30 = 75 frames; a stalled seam loses many";
    EXPECT_LE(worstGapMs, 67) << "the seam may cost at most one extra frame period (2 x 33.3 ms)";
  }
}

TEST(AvfMediaDecoder, MissingAndUnsupportedFilesFailLoudly) {
  auto d = modules::createAvFoundationMediaDecoderFactory()();
  const auto missing = tmp("absent.mp4");
  EXPECT_TRUE(pollUntilFrame(*d, req(missing, true), 1500).empty());
  ASSERT_FALSE(d->warnings().empty());
  EXPECT_EQ(d->warnings()[0].rfind("Media asset fx could not be opened for Program playback: ", 0), 0u);
  EXPECT_NE(d->warnings()[0].find("does not exist"), std::string::npos);
  const auto webm = tmp("clip.webm"); Cleanup c{webm};
  { std::ofstream(webm) << "not really webm"; }
  auto w = modules::createAvFoundationMediaDecoderFactory()();
  EXPECT_TRUE(pollUntilFrame(*w, req(webm, false), 1500).empty());
  ASSERT_FALSE(w->warnings().empty());
  EXPECT_NE(w->warnings()[0].find("WebM playback on Mac needs FFmpeg"), std::string::npos);
}

TEST(AvfMediaDecoder, DestroyingMidReadNeverCallsBackIntoARetiredDecoder) {
  const auto p = tmp("churn.mp4"); Cleanup c{p};
  ASSERT_EQ(testing::writeFlatColourClip(p, 640, 360, 60, {{120, 0x30, 0x60, 0x90}}), "");
  auto alive = std::make_shared<std::atomic<bool>>(true);
  auto lateWakes = std::make_shared<std::atomic<int>>(0);
  for (int i = 0; i < 50; ++i) {
    auto d = modules::createAvFoundationMediaDecoderFactory()();
    auto* pre = dynamic_cast<modules::IMediaVideoPrefetch*>(d.get());
    auto token = std::make_shared<std::atomic<bool>>(true);
    pre->setMediaWakeCallback([token, lateWakes] { if (!token->load()) lateWakes->fetch_add(1); });
    auto r = req(p, true); pre->syncMediaClock(r, steadyMs());
    for (int k = 0; k < i % 7; ++k) (void)pre->prefetchMediaVideo(r, steadyMs());
    d.reset();
    token->store(false);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(lateWakes->load(), 0);
}

TEST(AvfMediaDecoder, SixteenDecodersOpenAndEachDeliversAFrame) {
  // MediaTransports' cap is 16 (MediaTransports.h:552). If VideoToolbox refuses earlier, this
  // must fail with the decoder's own warning, which is what the operator would see.
  const auto p = tmp("sixteen.mp4"); Cleanup c{p};
  ASSERT_EQ(testing::writeFlatColourClip(p, 1920, 1080, 30, {{30, 0x50, 0x50, 0x50}}), "");
  std::vector<std::unique_ptr<modules::IMediaDecoder>> decoders;
  for (int i = 0; i < 16; ++i) decoders.push_back(modules::createAvFoundationMediaDecoderFactory()());
  for (int i = 0; i < 16; ++i) {
    const auto f = pollUntilFrame(*decoders[i], req(p, false), 5000);
    EXPECT_EQ(f.size(), 1u) << "decoder " << i << ": "
        << (decoders[i]->warnings().empty() ? std::string("no warning") : decoders[i]->warnings()[0]);
  }
}
```

Also in `MediaCoreCommandTest.cpp` (after the re-gate) the existing bodies of
`MediaFoundationMediaFrameSource.DecodesFirstFrameForPausedPreviewCue` and
`.PausingMidPlaybackHoldsTheOnAirFrameAndResumeContinues` run unchanged against AVFoundation on Mac.
These are the Windows acceptance oracle verbatim (64x64, `participantId == "preview:media:diagnostic"`,
every alpha byte 0xFF; held ∈ [5, 25), resume `next ∈ (held, held+2]`). And in `ContractParityTest`, the
existing `ProfileCapabilityStatesMatchTheManifest` case (`:235-247`) now covers the 25th name.

- [ ] **Step 1:** Write `AvfMediaFixtures.{h,mm}`, `AvfMediaDecoderTest.cpp`, the re-gate. Configure with
  the option ON. Expected: build fails (factory undeclared), then the tests fail.
- [ ] **Step 2:** Implement the `.mm`, the CMake option, the factory wiring, the capability (5 files).
- [ ] **Step 3:** Run `--gtest_filter='AvfMediaDecoder.*:MediaFoundationMediaFrameSource.*:MediaClipDecoder.*:ContractParity.*'`
  on `build-metal`. Expected: all pass. Paste the `[avf-loop-seam]` line into the report.
- [ ] **Step 4:** Full `ctest --test-dir native/build-metal -j 6` and the stub config. Expected: green; the
  stub reports `media-clip-playback: omitted`.
- [ ] **Step 5: Windows inertness (owner or Windows lane):** on the Windows box,
  `native/build-dev/corevideo-native-tests.exe --gtest_filter=MediaFoundationMediaFrameSource.*:ContractParity.*:MediaCoreCommand.Profile*`
  with `--config Release` (CLAUDE.md). Expected: unchanged passes, plus the 25th capability `available`.
  If no Windows build is available in this session, write that in the report as UNVERIFIED.
- [ ] **Step 6:** Prove the log line: run the core (`native/build-metal/corevideo-native`) with a
  `load-scene-graph` naming a missing `.mp4` route and grep stderr for `[media-decoder] … NO DECODER -`.
  Paste the line.

**Mutations to run:**
- Request `kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange` and convert on the CPU with a BT.601 matrix:
  `Bt709LimitedBars…` red on green R.
- Drop `AVVideoColorPropertiesKey`: report whether `Bt709LimitedBars…` goes red. If it stays green, label
  the colour-properties key "confirmatory on this OS" in the test comment (rule 8).
- Remove the `dispatch_sync` drain in the reader destructor: `DestroyingMidRead…` red (run 3 times; report).
- Keep 4 samples decoded ahead instead of 1: report the effect on `PlayPauseResume…` (it must still
  pass — the bound is behavioural); record memory per decoder at 1080p in the report.
- Remove the pre-open: `AClipEnds…Loop…` red on the 67 ms seam bound, or report the measured gap if green.
- Return `"omitted"` for the capability when constructed: `TheFactoryIsLive…` red.
- Leave `media-clip-playback` out of `Protocol.h`: `ContractParity.*` red.

**Done when:** all tests green on Metal and stub, mutations reported, Windows step done or marked
UNVERIFIED, log line pasted, the four script/CI configure sites carry the option.

---

### Task 3: Pixel proof on Program — real decoder, real Metal compositor, real MediaCore

**Lane:** native. **Slice:** N1. No production code is expected to change; if a test exposes a defect,
fix it in this task and name it in the report.

**Files:** NEW `native/tests/AvfMediaPixelTest.cpp` (gated
`#if !COREVIDEO_STUB && COREVIDEO_WITH_METAL && COREVIDEO_WITH_AVF_MEDIA_DECODER`), registered in the
same CMake block as Task 2's test. Portability fixes in `scripts/qa/media-take-ab.py` only if Step 4
needs them.

**Interfaces consumed:** `MediaCore::applyCommands(cmds, 0.0)` (one FULL tick when no audio worker is
running, `MediaCore.cpp:1168-1196`; a full tick sets `skipCpuReadback=false`, `MediaCore.cpp:6058-6059`,
so the Metal compositor fills `lastProgramFrameForTest().preview`, a 320x180 BGRA thumbnail of Program);
`useZoomSourcesForTest({})`; `sessionState()["mediaSources"]`.

**Fixture invariants:** the module set is `createStubModules()` with `compositor = createMetalCompositor()`,
`mediaDecoderFactory = createAvFoundationMediaDecoderFactory()` and a `NoCaptureDevice` (copy the class
from `ProgramPixelContinuityTest.cpp:56-62`). Clip colours are chosen far from every slate
(`kWarmingSlateRgba 0x1b1f27`, `kFailedSlateRgba 0x23181c`, black; `CompositorLayout.h:504-506`):
RED = BGRA(0x20,0x20,0xE0), BLUE = BGRA(0xE0,0x20,0x20). Every scene carries ONE full-canvas fixed route
`{"routeId":"r","mode":"fixed","mediaAssetId":"clip","mediaAssetName":"Clip","mediaAssetKind":"stinger","mediaAssetPath":<p>,"mediaAssetLoop":false,"rect":{0,0,1,1}}`.
The Metal device check uses the `MAKE_COMPOSITOR_OR_SKIP` rule: no device → print `[  SKIPPED ]` and return.

**Complete tests (exact fixtures + assertions):**

1. `AvfMediaPixel.AClipOnProgramRollsFromItsFirstFrameAndNeverShowsASlate` — clip 640x360@30: 30 RED then
   60 BLUE (3 s). `load-scene-graph` with the route. Tick (`applyCommands({}, 0)` + 2 ms sleep) until the
   centre pixel of `preview` is within ±12 of RED on every channel (deadline 3 s). From the first tick
   whose `preview` is non-empty, **every** sampled centre pixel must be RED±12 or BLUE±12; any other
   colour fails with the tick index and the pixel. Within 2.5 s of the first RED tick, a BLUE±12 tick
   must appear (the clip rolled). `mediaSources` row `media:clip` reads `state:"live", onProgram:true`.
2. `AvfMediaPixel.APausedClipHoldsTheExactOnAirPixels` — same clip. Once BLUE is on air, send
   `set-media-transport {mediaAssetId:"clip", action:"pause"}`; tick 2 times, then record the full
   `preview.bgra`; for the next 30 ticks over ≥ 500 ms the buffer must be **byte-identical** to the
   recording, and `mediaSources[0].state == "paused"`, `presentedVideoFrames` unchanged across the 30
   ticks. Then `action:"play"`: within 1 s `presentedVideoFrames` increases and `state == "live"`.
3. `AvfMediaPixel.ACuedClipTakenToProgramStartsOnItsPosterFrame` — the real-decoder twin of
   `ProgramPixelContinuity.ACuedClipTakenToProgramNeverShowsThePlaceholder`. Program = a scene whose only
   route is a full-canvas **still** route (`mediaAssetId "green"`, a 64x64 PNG of BGRA(0x20,0xE0,0x20)
   written with ImageIO; route stills are served by `StillMediaFrameCache` and already work on Mac);
   wait until the centre pixel is GREEN±12. Preview = the clip scene (`set-preview-scene`).
   (The route wire has no source-less fill colour — `hasFillColor` is set only inside the core,
   `MediaCore.cpp:4512, 4775` — so a still is the outgoing picture.) Wait until
   `mediaSources` row reads `cued` and `decodedVideoFrames >= 1` (deadline 3 s). Take =
   `begin-take-transition {mode:"cut", durationMs:0}` + `load-scene-graph` (clip scene) +
   `set-preview-scene` (green scene). For the next 10 ticks the centre pixel is GREEN±12 or RED±12, never a
   slate, and at least the last 5 are RED±12. (The cold-cut hold keeps the outgoing green until the
   first clip frame, `MediaCore.cpp:2302`.)
4. `AvfMediaPixel.ALoopingVideoBackgroundKeepsChangingPastItsEnd` — clip 64x64@30: 15 RED, 15 BLUE (1 s),
   sent as `load-scene-graph` with **no routes** and `background:{mediaAssetId:"bg",mediaAssetName:"BG",mediaAssetKind:"stinger",mediaAssetPath:<p>}`.
   Over 3 s of ticks: RED→BLUE→RED→BLUE transitions observed ≥ 4 times (it looped twice); no non-RED/BLUE
   centre pixel after the first content tick; `mediaSources` row `background:bg` has `loop:true`,
   `state:"live"` throughout.
5. `AvfMediaPixel.AStillSceneBackgroundRenders` — a 64x64 PNG written with ImageIO (BGRA 0x20,0xE0,0x20)
   as the background, no routes: within 2 s the centre pixel is that green ±6. (Spec correction 4: this
   never rendered on Mac before.)

- [ ] **Step 1:** Write the five tests. Run `--gtest_filter='AvfMediaPixel.*'` against a build **without**
  the option (stub decoder factory empty). Expected: compile-gated out — so instead verify red by
  temporarily pointing `mediaDecoderFactory` at `{}` in a local edit: all five fail. Revert the local edit.
- [ ] **Step 2:** Run with the option. Expected: 5/5 pass. If one fails, diagnose before touching it
  (rule 4 corollary).
- [ ] **Step 3:** `ctest --test-dir native/build-metal -j 6`. Expected: green.
- [ ] **Step 4: `scripts/qa/media-take-ab.py` on the Mac core.**
  `python3 scripts/qa/media-take-ab.py --core native/build-metal/corevideo-native --fake native/build-metal/corevideo-zoom-engine-fake --label mac-n1`
  then the same with `--skip-cue --label mac-n1-cold`. Expected: both PASS (the cold run passes through
  the cold-cut hold, spec §2.1). Paste both summaries. Fix only portability defects in the script (path
  suffixes, ffmpeg discovery) and list them.

**Mutations to run:**
- In `MediaClipDecoder`, return the frame without `pixels` for a playing clip (drop the copy): tests 1-4 red.
- Make resume reopen the reader (Task 1 mutation, now end to end): test 2's play leg shows frame ids
  restarting — report whether test 2 catches it; if not, add `positionMs` continuity to test 2
  (position after play ≥ position at pause) and re-run.
- Remove the pre-open AND the loop reopen (end at EOS for loops): test 4 red.
- Set `skipCpuReadback` forcibly true in the test path: every test fails on empty `preview` (proves the
  readback path is what is judged, not a stale buffer).

**Done when:** 5/5 green with a real Metal device, `media-take-ab.py` cued and cold both PASS on Mac.

---

### Task 4: `mac-show-drill.py --load 8` with a clip on Program — per-frame delivery verdict

**Lane:** native. **Slice:** N1. This task changes only the drill script and records a measurement.

**Files:** Modify `scripts/mac-show-drill.py`.

**Interfaces:**
- New flag `--media`. With it, the drill generates (once, cached by name) two fixtures with `ffmpeg`
  (found like `scripts/qa/media-take-ab.py:62-81`: `--ffmpeg`, `COREVIDEO_FFMPEG_BIN_DIR`, `PATH`;
  missing → `FAIL harness: --media needs ffmpeg`):
  - `artifacts/show-drill/media-clip-1080p60.mp4`: 70 s, `testsrc2=s=1920x1080:r=60`, `libx264
    -preset veryfast -pix_fmt yuv420p`, `sine=f=440` AAC 48 kHz stereo. 70 s outlasts every phase.
  - `artifacts/show-drill/media-bg-1080p30.mp4`: 10 s, `testsrc2=s=1920x1080:r=30`, no audio (loops).
- With `--media`, the configure step's Program scene is
  `routes: [participant 101 full canvas (as today), {fixed media route, mediaAssetId "drill-clip", kind
  "stinger", path clip, mediaAssetLoop false, rect x .5 y 0 w .5 h .5, zIndex 1}]` plus
  `background: {mediaAssetId "drill-bg", mediaAssetName "drill-bg", mediaAssetKind "stinger", mediaAssetPath bg}`.
- New report block `Media sources (steady window)`: for each `mediaSources` row, at the start and end of
  the operator-traffic window, the deltas of `decodedVideoFrames`, `presentedVideoFrames`,
  `decoderRestarts`, the end `videoQueued`, `state`, and the per-second rates.
- New gates (only with `--media`), each a FAIL line naming its numbers:
  - `drill-clip` state is `live` at both ends of the window;
  - clip `presentedVideoFrames` rate ≥ 0.95 × 60 /s and `decodedVideoFrames` rate ≥ 0.95 × 60 /s;
  - `drill-bg` presented rate ≥ 0.95 × 30 /s;
  - `decoderRestarts` delta 0 for both.
  The drill's existing gates (sustained fps, dropped frames, delivery, latency, coreMutex ratio,
  recording) are unchanged and still decide PASS/FAIL.

**Behaviour rules:** the media rows are read from the `snapshot` the drill already receives on each
`poll` step; no new request type. A missing `mediaSources` node is a FAIL ("core publishes no
mediaSources") — the node is unconditional (`CoreStateCapture.cpp:1192-1221`).

**Steps (the measurement, on a quiet machine, release core, owner's Mac):**
- [ ] **Step 1: Baseline.** `git status` clean. `COREVIDEO_FAKE_ENGINE_FPS=60 python3 scripts/mac-show-drill.py --seconds 40 --load 8`
  — the no-clip baseline. Record every printed metric (fps, dropped, render hold, worst frame,
  coreMutex over-budget, command p50/p99, source→render p50/p99, decoded-frame delivery, the
  recording-window stage rates). Confirm the fake engine sourced ~480 f/s (`COREVIDEO_FAKE_ENGINE_LOG`).
- [ ] **Step 2:** Implement `--media`; a dry run with `--seconds 5 --load 0 --media` shows the media block.
- [ ] **Step 3: Measured run,** moments after Step 1:
  `COREVIDEO_FAKE_ENGINE_FPS=60 python3 scripts/mac-show-drill.py --seconds 40 --load 8 --media`.
- [ ] **Step 4: Report** a two-column table (baseline | media) for every metric, the media block, and the
  `[media-decoder] copy avg … max …` lines (verbose diagnostics are on in the drill), plus exact hardware
  (`sysctl -n machdep.cpu.brand_string`, RAM), macOS build, core binary size and `--config`.
- [ ] **Step 5: Verdict.** PASS only if the media run passes every existing gate (60.0 of 60 sustained,
  **0 dropped**, delivery ≥ the drill's threshold, coreMutex ratio under budget) **and** the new media
  gates. Mean fps is not evidence (CLAUDE.md frame-delivery rule). If it fails: N1 still ships (the
  decoder is correct), and the report states "N4 moves up" for the owner (D5). Never relax a threshold.

**Mutations to run:**
- Set the clip route's path to a missing file: the media gates must FAIL (state not live, rate 0).
- Make the drill read `decodedVideoFrames` from the wrong row (background for clip): the clip-rate gate
  must FAIL at 30 /s vs 57 /s.

**Done when:** both runs recorded with the table, verdict stated with the numbers behind it.

---

### Task 5: N1's consumer — the scene background picker lists videos (and stills that actually exist)

**Lane:** shell. **Slice:** N1 (consumer).

**Files:** Modify `mac-shell/Sources/CoreVideoProShell/MediaBin.swift`,
`mac-shell/Sources/CoreVideoProShell/ShellApp.swift:1035-1038` and its `summary` text, `ShellTests.swift`
(register the case in `runAndExit`).

**Interfaces:**

```swift
extension MediaBin {
    /// Assets a scene background may use on this Mac: stills and AVFoundation-playable video.
    /// `.webm` is excluded until FFmpeg playback exists (default D1). Audio is never a background.
    static func backgroundOptions(_ assets: [MediaAsset]) -> [MediaAsset]
}
```

**Behaviour:** `SuperSourceBackgroundPicker.options` returns `MediaBin.backgroundOptions(model.mediaAssets)`
(today it filters on kinds `image`/`background`, which the classifier never produces, `ShellApp.swift:1037`).
Order preserved from the bin. The empty-state summary becomes
`"No stills or videos in the media bin — import one to use as a background for <scene>."`. The wire is
unchanged here (S1 removes `playing`).

**Complete test:**

```swift
@MainActor
private static func testBackgroundPickerListsStillsAndPlayableVideo() {
    func asset(_ id: String, _ file: String, _ kind: String) -> MediaAsset {
        MediaAsset(id: id, name: id, kind: kind, relativePath: kind + "/" + file,
                   filePath: "/bin/" + kind + "/" + file, naturalWidth: 0, naturalHeight: 0)
    }
    // Fixture: one of every class the bin can hold, in bin order.
    let bin = [asset("sting", "open.mp4", "stinger"), asset("mov", "loop.mov", "stinger"),
               asset("web", "x.webm", "stinger"), asset("logo", "logo.png", "lower-third"),
               asset("jpg", "card.jpg", "lower-third"), asset("bed", "bed.wav", "audio-bed"),
               asset("slate", "slate-break.mp4", "slate")]
    let ids = MediaBin.backgroundOptions(bin).map(\.id)
    expectEqual(ids, ["sting", "mov", "logo", "jpg", "slate"],
                "stills + mp4/mov in bin order; no webm (D1), no audio")
}
```

- [ ] **Step 1:** Write the test; run the harness. Expected: compile failure.
- [ ] **Step 2:** Implement; run. Expected: pass; 237 cases.
- [ ] **Step 3: Live (needs Task 2):** `scripts/run-mac-shell.sh`, import a 10 s `.mp4`, pick it as the
  Preview scene's background, Take: the background plays and loops behind the scene on Program.
  Attach a screenshot and a `COREVIDEO_SHELL_TAB_SNAPSHOT` of the Studio rail picker.

**Mutations:** keep the old kind filter → red; allow `.webm` → red; allow `.wav` → red.

**Done when:** test green, live check recorded.

---

### Task 6: Clip audio — the second reader, windows cut at the clock position

**Lane:** native. **Slice:** N2.

**Files:** Modify `MediaClipDecoder.{h,cpp}`, `AVFoundationMediaDecoderAdapter.mm`,
`MediaClipDecoderTest.cpp`, `AvfMediaDecoderTest.cpp`, `AvfMediaFixtures.{h,mm}`,
`MediaCoreCommandTest.cpp` (move `:4121` and `:4253` under the either-decoder gate, using
`testMediaDecoder()`).

**Interfaces:** `IClipReaderFactory::openAudio` (Task 1 declaration) is implemented. New fixture helper:

```cpp
// AvfMediaFixtures.h
// H.264 + AAC MP4: `seconds` of video (flat grey 64x64 @30) and a sine at `toneHz`, 48 kHz stereo.
std::string writeToneClip(const std::string& path, int seconds, double toneHz);
```

**Behaviour** (ports `MF:938-988, 1054-1103, 1223-1290`):
- `pollMediaAudioFrames` returns nothing unless `request.kind == "media-video"`, `playing`, and the path
  is not a still (MF:673-681). Paused emits nothing, not even silence.
- First playing poll opens the audio reader at `start100ns = mediaLoopSeek100ns(clock.elapsed100ns(t), audioLoopOffset)`
  (MF:1093-1101). No audio track → `openAudio` returns nullptr with empty error → the clip is silent and
  NOT warned (MF:1080-1086).
- Each poll: `audioWindows.seek(elapsed * 48000 / 10^7)`; take up to 8 samples while
  `bufferedThrough < cursor + 960` (`MF:972-977`), appending `pts + audioLoopOffset`; emit one 960-frame
  window (`sampleRate 48000, channels 2, voiceActive true, participantId` as video) only when a full
  window is buffered or the reader is at its end (MF:978-986).
- Loop: the audio reader is pre-opened (start 0) when a taken sample's `pts + dur ≥ duration − 300 ms`;
  on its `EndOfStream` the decoder switches to it and sets `audioLoopOffset = audioEndPts` (MF:953-958,
  plus pre-open).
- Resume: the clock's frozen elapsed time is where audio continues; `MediaAudioWindows`' 200 ms history
  replays samples decoded ahead before the pause (`MediaPlaybackTimeline.h:74-118`).
- AVF reader: `AVAssetReaderAudioMixOutput` (or track output) with `AVFormatIDKey kAudioFormatLinearPCM`,
  `AVSampleRateKey 48000`, `AVNumberOfChannelsKey 2`, 32-bit float, interleaved; `timeRange` starts at
  `start100ns`. Same queue, one-ahead, wake and teardown rules as video. Mono sources are upmixed by
  AVFoundation; 44.1 kHz sources are resampled by it.

**Complete tests:**

Fake readers (`MediaClipDecoderTest.cpp`), extended with an audio script whose sample values encode
their own media millisecond, `value = (ms + 1) / 4096` (the timecode scheme of
`MediaCoreCommandTest.cpp:4166-4196`, so "which media time is this" is a fixture constant). Fixture
invariants: chunks are 480 frames (10 ms) of interleaved stereo; pts are contiguous; a 3 s script.

```cpp
TEST(MediaClipDecoder, NoAudioWhilePausedAndAudioResumesAtTheClockPosition) {
  // Play 300 ms, pause 400 ms, play: the first non-zero sample after resume is within
  // 50 ms (the G2 A/V budget) of the last one heard before the pause.
  // Assertions: windows.size() == 0 for every poll during the pause;
  //             |resumedMs - lastHeardMs| <= 50; resumedMs >= lastHeardMs - 50 (bounded both sides);
  //             audio readers opened == 1 (resume never reopens).
}
TEST(MediaClipDecoder, AudioWindowsAreContiguousAcrossALoopSeam) {
  // 1 s looping script. Collect 2.5 s of windows at a fixed 20 ms cadence. Concatenate:
  // no sample is exactly 0.0f after the first window (0.0 = a hole; the timecode is never 0),
  // and decoded ms runs 0..999 then wraps to 0 (each step +0 or +1 except at the wrap).
}
TEST(MediaClipDecoder, AnAssetWithNoAudioTrackIsSilentAndNotWarned) {
  // openAudio returns nullptr + "" : pollMediaAudioFrames empty, warnings() empty, video unaffected.
}
TEST(MediaClipDecoder, AudioWindowsAreExactly960StereoFramesAt48k) {
  // Every emitted frame: sampleRate 48000, channels 2, sampleCount 960, pcm.size() 1920,
  // participantId "media:clip".
}
```

The implementer writes these four bodies to exactly the assertions in their comments, using the fake
audio reader added beside `FakeReader` (same never-block, wake-on-ready, destructor-recorded contract).

Real AVFoundation (`AvfMediaDecoderTest.cpp`):

```cpp
TEST(AvfMediaDecoder, A440HzClipDecodesTo440HzStereoAt48k) {
  // writeToneClip(p, 3, 440.0). Play; collect 1 s of windows at 20 ms cadence (video polled too,
  // as the owner does). Assert: every window 48000/2/960; peak > 0.1; zero crossings on the left
  // channel over the collected second / 2 is within 440 ± 10 Hz (the tone, not noise).
}
```

plus `MediaFoundationMediaFrameSource.DecodesSceneMediaAudioPcmFromLocalWav` (44.1 kHz mono WAV → 48 kHz
stereo, peak > 0.05) and `.PausedAudioIsSilentAndResumesFromThePausedPosition` (timecoded float WAV,
silence while paused, `|resumed − lastHeard| ≤ 50 ms`), now running on AVFoundation unchanged — the
Windows oracle. (Decoding a WAV at decoder level is in scope here; audio-only *assets on air* are N3.)

- [ ] **Step 1:** Tests first; run `MediaClipDecoder.*:AvfMediaDecoder.*:MediaFoundationMediaFrameSource.*`.
  Expected: the new and re-gated audio cases fail.
- [ ] **Step 2:** Implement. Run. Expected: pass.
- [ ] **Step 3:** Full Metal + stub ctest green.

**Mutations to run:**
- Emit a silent window while paused: `NoAudioWhilePaused…` red.
- Reopen the audio reader on resume (seek to clock): report; it must stay within 50 ms (then it is a
  cost, not a defect — say so) or go red.
- Remove the audio pre-open: `AudioWindowsAreContiguous…` red (a hole of zeros at the seam).
- Drop `audioLoopOffset`: `AudioWindowsAreContiguous…` red (time runs backwards without the wrap rule).
- Resample to 44.1 kHz: `A440Hz…` and `AudioWindowsAreExactly960…` red.

**Done when:** all green; mutation results reported.

---

### Task 7: Audio proven with audio — the mixer's measured levels and a decoded recording

**Lane:** native. **Slice:** N2.

**Files:** Modify `native/tests/MediaCoreCommandTest.cpp` (one new test next to `:3861`), modify
`scripts/validate-record-audio.mjs`.

**Complete test (real decoder through the real mixer):**

```cpp
#if !COREVIDEO_STUB && COREVIDEO_WITH_AVF_MEDIA_DECODER
// The Windows oracle SceneMediaAudioReachesMasterThroughTheShellMediaStrip (:3861) uses a fake
// decoder. This is the same assertion set on the REAL decoder, plus the tone check that proves the
// PCM on master is the clip's and not anything else.
TEST(MediaCoreCommand, ARealClipsToneReachesMasterThroughTheMediaStripAndMuteSilencesIt) {
  // Fixture: writeToneClip(p, 5, 440.0) (AvfMediaFixtures). Modules: createStubModules() with
  // mediaDecoderFactory = createAvFoundationMediaDecoderFactory().
  // Commands: load-scene-graph with ONE fixed route {mediaAssetId "tone", kind "stinger", path p,
  //   mediaAssetLoop false, full canvas} + shellAudioMix({shellAudioStrip("media"), shellAudioStrip("zoom-mix")})
  //   + shellMediaRouting()  (the helpers already in this file).
  // Assert (applyUntil, 4000 ms): peakOfSamples(programAudioTapPcm()) > 0.05 AND
  //   peakOfSamples(audioBusTapPcm("stream")) > 0.05.
  // Assert: the "media" strip's outputLevel > 0 and status != "waiting-for-pcm".
  // Assert: zero-crossing frequency of programAudioTapPcm() left channel within 440 ± 15 Hz
  //   (requires >= 960 frames in the tap; the tap length is a fixture fact — assert it first).
  // Then mute: shellAudioMix({shellAudioStrip("media", true), …}); one more applyCommands({});
  //   peak(master) < 0.001, peak(stream) < 0.001, strip outputLevel == 0.
}
#endif
```

**`validate-record-audio.mjs` portability** (spec correction 8): build dir =
`process.env.COREVIDEO_BUILD_DIR ?? (darwin ? native/build-metal : native/build-dev)`; tool discovery
tries `PATH`, then `/opt/homebrew/bin`, then `C:\ffmpeg\bin` (`findTool`, `ffprobeVerdict`); readiness
`platform: process.platform === "darwin" ? "macos" : "windows"`; the media route drops
`mediaPlaybackKey`/`mediaAssetPlaying` and sends `mediaAssetLoop: false`. No other change.

- [ ] **Step 1:** Write the test; run. Expected: it passes or fails for a real reason — if it passes at
  once, run the mutations to prove it can fail.
- [ ] **Step 2:** Port the script. Run `node scripts/validate-record-audio.mjs --media` on Mac. Expected:
  PASS, judging the **decoded recording's** audio (440 Hz tone share, peak, RMS). Paste the verdict JSON.
- [ ] **Step 3:** Run it on Windows (owner or Windows lane) to show the script still passes there, or mark
  UNVERIFIED.

**Mutations to run:**
- Remove the `"media"` alias pre-sum in a local edit of `AudioControlSourcePolicy.h` usage (or drop the
  `"media"` strip from the test's console): the new test red on master peak (the FADER LAW drops it).
- Return silence (zeros of the right length) from the AVF audio reader: red on peak and on 440 Hz.
- Feed a 1 kHz fixture to the test while asserting 440: red on frequency (proves the frequency check
  discriminates).

**Done when:** the test green, the decoded-recording verdict pasted, mutations reported.

---

### Task 8: Media bin parity — filter, classification, grouping, Select, Remove with the in-use guard

**Lane:** shell. **Slice:** S3.

**Files:** Modify `mac-shell/Sources/CoreVideoProShell/MediaBin.swift`, `AppModel.swift` (media bin
section `:3817-3830`), `ShellTests.swift` (register the cases).

**Interfaces:**

```swift
extension MediaBin {
    static let supportedExtensions = ["mp4", "mov", "webm", "png", "jpg", "jpeg", "gif",
                                      "wav", "mp3", "aac", "m4a"]               // MediaBinService.cs:22-23
    /// Windows MediaBinClassifier.ClassifyFile: slate (folder or name prefix) first, then by
    /// extension; nil for an unknown extension (MediaBinClassifier.cs:77-107).
    static func classify(fileName: String, relativePath: String) -> String?
    static func kindLabel(_ kind: String) -> String                              // :161-169
    /// MediaBinClassifier.GroupAssets: kinds in `kinds` order, empty groups omitted (:317-324).
    static func groups(_ assets: [MediaAsset]) -> [(kind: String, label: String, assets: [MediaAsset])]
    /// Moves the file into <root>/.trash with a unique name (MediaBinService.DeleteAsset :84-100).
    static func moveToTrash(_ asset: MediaAsset) -> Bool
    /// Harness-isolated: under ShellHarness.isActive the root is a per-process temp dir, never the
    /// operator's real bin (the ShellPrefs rule, ShellPrefs.swift:9-35).
    static var root: String { get }
    static var realRoot: String { get }
    struct ImportResult: Equatable { let imported: [MediaAsset]; let refused: [String] }
    static func importFiles(_ urls: [URL]) -> ImportResult                       // refused = file names
}
extension AppModel {
    func importMedia(urls: [URL])                        // sets mediaStatus per Windows :5970-5982
    func selectMediaAsset(_ id: String)                  // Windows SelectMediaAsset :6047-6074
    func removeMediaAsset(_ id: String)                  // Windows RemoveMediaAsset :6083-6125
    func mediaAssetReferences(_ id: String) -> [String]  // labels: "scene <name>", "scene <name> background", "logo bug"
    @Published var mediaStatus: String                   // the Windows CommandStatus line for media
}
```

**Behaviour:**
- `classify` returns nil for anything outside the 11 extensions; `importFiles` skips (and reports) those.
  The open panel's `allowedContentTypes` is built from `supportedExtensions` (Windows picker filter,
  `StudioViewModel.cs:5953-5956`).
- Import status, verbatim Windows (`StudioViewModel.cs:5964-5982`): cancelled panel → "Media import
  canceled"; one imported → "<name> imported and selected" and it becomes the selection; several →
  "<n> media assets imported" (first selected); none usable → "No supported media files imported".
- `selectMediaAsset`: sets `selectedAssetId`; `mediaStatus = "<name> ready to cue or audition"`. Never
  pauses or plays anything (S2 adds the on-air flag).
- `removeMediaAsset`: references = every scene whose `layers` contain `mediaAssetId == id` ("scene
  <name>"), every scene whose `backgroundAssetId == id` ("scene <name> background"), and the logo bug
  ("logo bug"). Non-empty → refuse with
  `"'<name>' is in use (<refs joined by ", ">). Unassign it there before removing."` and write
  `lifecycle: media remove blocked <id> - referenced by <refs joined by "; ">` to `ShellLog`. Otherwise
  move to `.trash`, clear the selection if it was this asset, `refreshMediaBin()`, status
  `"Removed '<name>' (recoverable from the media .trash folder)."`; failure → `"Could not remove '<name>'."`.
- `MediaAsset` gains `var durationMs: Int? = nil` (filled in Task 9) and computed `supportsPlayback`
  (`!isStillImage`, Windows `ProductionModels.cs:900`), `durationLabel` (Windows `:883-884`, banker's
  rounding: 1250 ms → "1.2s", 1350 ms → "1.4s", 3000 ms → "3s", nil → ""). New properties carry
  defaults so every existing memberwise call site (e.g. `ShellTestsTier0.swift:965`) still compiles.

**Complete tests:**

```swift
private static func testMediaClassifierMatchesWindows() {
    // MediaBinServiceTests.ClassifyFile_MapsSupportedExtensions, case for case.
    let cases: [(String, String)] = [("intro.mp4", "stinger"), ("clip.MOV", "stinger"),
        ("transition.webm", "stinger"), ("overlay.png", "lower-third"), ("title.JPG", "lower-third"),
        ("bed.wav", "audio-bed"), ("music.mp3", "audio-bed"), ("voice.m4a", "audio-bed")]
    for (file, kind) in cases {
        expectEqual(MediaBin.classify(fileName: file, relativePath: file), kind, file)
    }
    expectEqual(MediaBin.classify(fileName: "show-open.png", relativePath: "slates/show-open.png"),
                "slate", "slates folder")
    expectEqual(MediaBin.classify(fileName: "slate-break.mp4", relativePath: "assets"), "slate", "slate prefix")
    expect(MediaBin.classify(fileName: "readme.txt", relativePath: "readme.txt") == nil,
           "unknown extension is refused (was lower-third)")
    expectEqual(MediaBin.assetId(relativePath: "stingers\\brand.mp4"),
                MediaBin.assetId(relativePath: "stingers/brand.mp4"), "id separator-stable")
}

private static func testMediaGroupsOrderKindsAndOmitEmpty() {
    // MediaBinServiceTests.GroupAssets_OrdersKindsAndOmitsEmptyGroups.
    func a(_ id: String, _ kind: String) -> MediaAsset {
        MediaAsset(id: id, name: id, kind: kind, relativePath: kind + "/" + id,
                   filePath: "/b/" + id, naturalWidth: 0, naturalHeight: 0)
    }
    let groups = MediaBin.groups([a("bed", "audio-bed"), a("st", "stinger"), a("ov", "lower-third")])
    expectEqual(groups.map(\.kind), ["stinger", "lower-third", "audio-bed"], "Windows kind order")
    expectEqual(groups.first?.label ?? "", "Stinger", "label")
    expectEqual(groups.last?.label ?? "", "Audio bed", "label")
    expect(!groups.contains { $0.kind == "slate" }, "empty group omitted")
}

private static func testMediaDurationLabelUsesWindowsRounding() {
    func d(_ ms: Int?) -> String {
        var asset = MediaAsset(id: "x", name: "x", kind: "stinger", relativePath: "s/x.mp4",
                               filePath: "/b/s/x.mp4", naturalWidth: 0, naturalHeight: 0)
        asset.durationMs = ms
        return asset.durationLabel
    }
    expectEqual(d(3000), "3s", "whole seconds drop the decimal")
    expectEqual(d(1250), "1.2s", "banker's rounding, like .NET Math.Round")
    expectEqual(d(1350), "1.4s", "banker's rounding the other way")
    expectEqual(d(nil), "", "unknown duration is blank")
}

@MainActor
private static func testMediaRootIsIsolatedUnderTheHarness() {
    expect(MediaBin.root != MediaBin.realRoot, "the harness never touches the operator's bin")
    expect(MediaBin.root.hasPrefix(NSTemporaryDirectory()), "harness bin is under the temp dir")
}

@MainActor
private static func testMediaRemoveIsRefusedWhileInUseAndTrashesOtherwise() {
    // Fixture: real files in the harness bin, so moveToTrash does real I/O.
    let fm = FileManager.default
    let dir = MediaBin.root + "/stinger"
    try? fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
    for name in ["used.mp4", "free.mp4"] { fm.createFile(atPath: dir + "/" + name, contents: Data([1])) }
    let model = sceneEditModel()                       // s1 "Intro" (preview), s2 "Panel" (program)
    model.refreshMediaBin()
    let used = model.mediaAssets.first { $0.name == "used" }!
    let free = model.mediaAssets.first { $0.name == "free" }!
    model.scenes[1].backgroundAssetId = used.id        // "Panel" background
    model.scenes[0].layers.append(SceneLayer(id: "s1-layer-9", slotId: nil, mediaAssetId: used.id))
    model.removeMediaAsset(used.id)
    expectEqual(model.mediaStatus,
                "'used' is in use (scene Intro, scene Panel background). Unassign it there before removing.",
                "refused, every reference named")
    expect(fm.fileExists(atPath: used.filePath), "a refused remove leaves the file")
    model.selectMediaAsset(free.id)
    model.removeMediaAsset(free.id)
    expectEqual(model.mediaStatus, "Removed 'free' (recoverable from the media .trash folder).", "removed")
    expect(!fm.fileExists(atPath: free.filePath), "moved out of the bin")
    expect(fm.fileExists(atPath: MediaBin.root + "/.trash/free.mp4"), "into .trash")
    expectEqual(model.selectedAssetId, "", "selection cleared")
    expect(!model.mediaAssets.contains { $0.id == free.id }, "bin refreshed")
    fm.createFile(atPath: dir + "/free.mp4", contents: Data([2]))     // same name again
    model.refreshMediaBin()
    model.removeMediaAsset(model.mediaAssets.first { $0.name == "free" }!.id)
    expect(fm.fileExists(atPath: MediaBin.root + "/.trash/free-1.mp4"), "unique name in .trash")
    try? fm.removeItem(atPath: MediaBin.root)
}

@MainActor
private static func testMediaImportStatusIsWindowsWording() {
    let src = NSTemporaryDirectory() + "cvp-import-\(getpid())"
    try? FileManager.default.createDirectory(atPath: src, withIntermediateDirectories: true)
    for name in ["a.mp4", "b.png", "notes.txt"] {
        FileManager.default.createFile(atPath: src + "/" + name, contents: Data([1]))
    }
    let model = AppModel()
    model.importMedia(urls: [URL(fileURLWithPath: src + "/a.mp4")])
    expectEqual(model.mediaStatus, "a imported and selected", "one file")
    expectEqual(model.mediaAssets.first { $0.id == model.selectedAssetId }?.name ?? "", "a", "selected")
    model.importMedia(urls: [URL(fileURLWithPath: src + "/b.png"), URL(fileURLWithPath: src + "/notes.txt")])
    expectEqual(model.mediaStatus, "b imported and selected", "txt refused, png imported")
    model.importMedia(urls: [URL(fileURLWithPath: src + "/notes.txt")])
    expectEqual(model.mediaStatus, "No supported media files imported", "nothing usable")
    model.importMedia(urls: [])
    expectEqual(model.mediaStatus, "Media import canceled", "cancel")
    try? FileManager.default.removeItem(atPath: src)
    try? FileManager.default.removeItem(atPath: MediaBin.root)
}
```

Fixture invariants: `sceneEditModel()` (`ShellTests.swift:1949-1963`) names s1 "Intro", s2 "Panel";
the references string lists scenes in `model.scenes` order, layers before backgrounds per scene — the
expected text depends on that order. `importMedia(urls: [])` is the cancelled-panel path (the panel
code calls it with no URLs on cancel).

- [ ] **Step 1:** Tests first; run harness. Expected: compile failures, then reds.
- [ ] **Step 2:** Implement. Expected: all pass; cases = previous + 6.

**Mutations:** classify unknown → `lower-third` (red); groups in alphabetical order (red); remove
ignores backgrounds (red: the file moves); `.trash` without unique name (red: second remove fails);
`root` ignores the harness (red); `Math.Round` away-from-zero (red on 1250 ms).

**Done when:** green, `lint-mac-design.sh` green.

---

### Task 9: The Media tab layout, durations and the local preview

**Lane:** shell. **Slice:** S3.

**Files:** Modify `ShellApp.swift` (`MediaPane`, `:1859-1914`), `MediaBin.swift` (duration probe),
`AppModel.swift`, `ShellTests.swift`; NEW `mac-shell/Sources/CoreVideoProShell/MediaPreviewPlayer.swift`.

**Interfaces:**

```swift
extension MediaBin {
    /// AVURLAsset.load(.duration) per playable asset, off the main actor. Stills and failures -> absent.
    static func probeDurations(_ assets: [MediaAsset]) async -> [String: Int]
    /// "<n> assets · <k> clips · <s> stills · <a> audio"  (counts by supportsPlayback / audio ext)
    static func summary(_ assets: [MediaAsset]) -> String
}
struct MediaPreviewPlayer: NSViewRepresentable   // AVPlayerView, controlsStyle .none, player.isMuted = true (D4)
```

**Behaviour:** layout follows `W/Views/MediaPage.xaml`: a selected-asset card (name, kind, duration,
dimensions, the local preview — `MediaPreviewPlayer` for a clip, `NSImage` for a still, nothing for
audio), then the bin: summary line, kind headers from `MediaBin.groups`, each row with name, detail
(`kind · duration · WxH`), **Select** (or "Selected"), **Remove**; Bug/Unbug and the bin path stay. The
preview shows the selection paused at 0 until S2 (Task 12) binds it to the play state. `refreshMediaBin()`
publishes the list synchronously, then fills `durationMs` from `probeDurations` on a detached task and
writes it back on the main actor (one write per refresh, never per frame). Every interactive control gets
an accessibility label (CLAUDE.md rule: "give every new interactive control an automation name").

**Complete tests:**

```swift
private static func testMediaSummaryCountsByKind() {
    func a(_ f: String) -> MediaAsset {
        MediaAsset(id: f, name: f, kind: "x", relativePath: f, filePath: "/b/" + f,
                   naturalWidth: 0, naturalHeight: 0)
    }
    expectEqual(MediaBin.summary([a("a.mp4"), a("b.mov"), a("c.png"), a("d.wav")]),
                "4 assets · 3 clips · 1 still · 1 audio",
                "audio counts as a clip for playback AND as audio")
    expectEqual(MediaBin.summary([]), "Bin is empty — import stills, stingers or audio beds.", "empty")
}

@MainActor
private static func testMediaDurationsProbeRealFiles() {
    // Fixture: a 1.0 s silent AAC .m4a written with AVAssetWriter into the harness bin by the test
    // helper ShellTests.writeSilentM4a(path:seconds:) (added in this task), plus a PNG.
    // probeDurations -> ["<m4a id>": 1000 ± 25] and no entry for the PNG.
}
```

(`writeSilentM4a` is ~25 lines of AVAssetWriter in `ShellTests.swift`; the duration bound ±25 ms covers
AAC priming.) Tab snapshots: `COREVIDEO_SHELL_TAB_SNAPSHOT=<dir> COREVIDEO_SHELL_TAB_LIST=media` with
`TabSnapshotHarness.seed` extended to put four assets (one per kind) in the harness bin and select the
clip — attach both widths.

- [ ] **Step 1:** Tests first; then implement; harness green.
- [ ] **Step 2:** Tab snapshots attached; `lint-mac-design.sh` green.

**Mutations:** count audio only as audio (summary red); probe on the main actor — assert in the test
that `probeDurations` is called from a non-main context (`Thread.isMainThread == false` inside the probe,
recorded via a test hook) and confirm it goes red.

**Done when:** green, snapshots attached.

---

### Task 10: `mediaSources[]` decode, the capability flag, the wire cleanup and `set-media-playback`

**Lane:** shell. **Slice:** S1.

**Files:** NEW `mac-shell/Sources/CoreVideoProShell/MediaPlayback.swift`; modify `AppModel.swift`
(`applySnapshot` `:1592`, routes `:2580-2596, 2612-2626, 2650-2664`, `backgroundPayload` `:3220-3236`,
`sceneSyncBatch` `:3245`), `ShellTests.swift`.

**Interfaces:**

```swift
/// One row of snapshot["mediaSources"] (CoreStateCapture.cpp:1199-1217; C# NativeMediaCoreMediaSource).
struct MediaSourceRow: Equatable {
    let sourceId: String
    let mediaAssetId: String?
    let state: String            // "cued" | "live" | "paused" | "ended"; unknown strings kept verbatim
    let loop: Bool
    let onProgram: Bool
    let onPreview: Bool
    let positionMs: Double
    let durationMs: Double       // -1 = unknown
    let decodedVideoFrames: Double?, lastDecodedAgeMs: Double?, presentedVideoFrames: Double?
    let lastPresentedAgeMs: Double?, videoQueued: Double?, decoderRestarts: Double?
    /// nil when the node is absent (an older core); [] when present and empty. Rows without a
    /// string sourceId or state are dropped.
    static func parse(_ snapshot: JSONObject) -> [MediaSourceRow]?
}
struct MediaPlaybackReadModel: Equatable {   // snapshot["mediaPlayback"] (CoreStateCapture.cpp:2353-2397)
    let status: String; let mediaAssetId: String?; let playing: Bool; let summary: String; let warnings: [String]
    static func parse(_ snapshot: JSONObject) -> MediaPlaybackReadModel?
}
extension AppModel {
    @Published private(set) var mediaSources: [MediaSourceRow]       // last parsed; unchanged when node absent
    @Published private(set) var mediaClipPlaybackAvailable: Bool     // profile.capabilityStates["media-clip-playback"].state == "available"
    @Published private(set) var mediaPlayback: MediaPlaybackReadModel?
}
```

**Behaviour:**
- `applySnapshot` parses both nodes on every non-empty snapshot. `mediaSources` is assigned **only when
  it changed** (Equatable compare) so SwiftUI does not invalidate at snapshot rate.
  `mediaClipPlaybackAvailable` is false until a snapshot says `available` (an unknown state disables clip
  Cue/Play honestly, spec §4.2 degraded states).
- Route wire, all three media route sites (canvas still layer, logo bug ×2): send
  `"mediaAssetLoop": MediaRoutePlayback.isLoopingKind(asset.kind)` (Windows `IsLoopingKind`,
  `MediaRoutePlaybackService.cs:137-140`; false for every kind the classifier produces); **stop sending**
  `mediaPlaybackKey` and `mediaAssetPlaying` (`MediaCoreCommandBuilder.cs:285-291`).
- Background wire: stop sending `playing` (`MediaCoreCommandBuilder.cs:246-261`).
- `sceneSyncBatch` appends, when `selectedAssetId` names an asset in the bin,
  `{"type":"set-media-playback","mediaAssetId","mediaAssetName","mediaAssetKind","mediaAssetPath"}` (root
  level, not payload-wrapped; `MediaCore.cpp:3523-3552` reads the root). `selectMediaAsset` calls
  `syncScenes()` so the core's `mediaPlayback.status` follows the selection.

**Complete tests:**

```swift
private static func testMediaSourcesDecodeTheCoreRowShape() {
    // Fixture: two rows exactly as CoreStateCapture.cpp:1199-1217 writes them (numbers are doubles).
    let snapshot: JSONObject = ["mediaSources": [
        ["sourceId": "media:clip", "mediaAssetId": "clip", "state": "live", "loop": false,
         "onProgram": true, "onPreview": false, "positionMs": 1250.0, "durationMs": 3000.0,
         "decodedVideoFrames": 75.0, "lastDecodedAgeMs": 4.0, "presentedVideoFrames": 74.0,
         "lastPresentedAgeMs": 9.0, "videoQueued": 1.0, "decoderRestarts": 0.0],
        ["sourceId": "background:bg", "mediaAssetId": "bg", "state": "live", "loop": true,
         "onProgram": true, "onPreview": true, "positionMs": 400.0, "durationMs": -1.0],
        ["mediaAssetId": "orphan", "state": "live"],                       // no sourceId: dropped
    ]]
    let rows = MediaSourceRow.parse(snapshot) ?? []
    expectEqual(rows.count, 2, "row without sourceId dropped")
    expectEqual(rows[0].state, "live", "state"); expectEqual(rows[0].positionMs, 1250, "position")
    expectEqual(rows[0].decodedVideoFrames, 75, "#701 trace read")
    expectEqual(rows[1].durationMs, -1, "unknown duration stays -1, never 0")
    expect(rows[1].decodedVideoFrames == nil, "absent trace field is nil (older core)")
    expect(MediaSourceRow.parse([:]) == nil, "absent node -> nil")
    expectEqual(MediaSourceRow.parse(["mediaSources": [JSONObject]()])?.count ?? -1, 0, "present-empty -> []")
}

@MainActor
private static func testMediaSourcesApplyOnlyWhenChanged() {
    let model = AppModel()
    let snap: JSONObject = ["mediaSources": [["sourceId": "media:c", "mediaAssetId": "c", "state": "live",
                                              "onProgram": true]]]
    var publishes = 0
    let sink = model.$mediaSources.dropFirst().sink { _ in publishes += 1 }
    model.applySnapshotForTesting(snap); model.applySnapshotForTesting(snap); model.applySnapshotForTesting(snap)
    expectEqual(publishes, 1, "identical snapshots publish once")
    model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:c", "mediaAssetId": "c",
                                                     "state": "paused", "onProgram": true]]])
    expectEqual(publishes, 2, "a state change publishes")
    model.applySnapshotForTesting(["programBlackout": false])
    expectEqual(model.mediaSources.first?.state ?? "", "paused", "a snapshot without the node keeps the rows")
    sink.cancel()
}

@MainActor
private static func testClipCapabilityComesFromTheProfile() {
    let model = AppModel()
    expect(!model.mediaClipPlaybackAvailable, "unknown -> unavailable")
    model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "omitted"]]]])
    expect(!model.mediaClipPlaybackAvailable, "omitted -> unavailable")
    model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]])
    expect(model.mediaClipPlaybackAvailable, "available")
}

@MainActor
private static func testMediaRouteWireDropsRetiredFieldsAndSendsLoop() {
    let core = FakeTakeCore()
    let model = takeModel(core)                         // Program s2, Preview s1
    model.mediaAssets = [MediaAsset(id: "clip", name: "Clip", kind: "stinger",
        relativePath: "stinger/clip.mp4", filePath: "/b/stinger/clip.mp4", naturalWidth: 0, naturalHeight: 0),
        MediaAsset(id: "bg-asset", name: "BG", kind: "stinger", relativePath: "stinger/bg.mp4",
        filePath: "/b/stinger/bg.mp4", naturalWidth: 0, naturalHeight: 0)]
    model.scenes[0].layers.append(SceneLayer(id: "s1-layer-1", slotId: nil, mediaAssetId: "clip"))
    model.selectedAssetId = "clip"
    _ = runTake(model)                                  // s1 becomes Program
    let cmds = core.bodies.first?["commands"] as? [JSONObject] ?? []
    let load = cmds.first { $0["type"] as? String == "load-scene-graph" } ?? [:]
    let media = (load["routes"] as? [JSONObject] ?? []).first { $0["mediaAssetId"] as? String == "clip" } ?? [:]
    expect(media["mediaPlaybackKey"] == nil, "retired mediaPlaybackKey not sent")
    expect(media["mediaAssetPlaying"] == nil, "retired mediaAssetPlaying not sent")
    expectEqual(media["mediaAssetLoop"] as? Bool, false, "mediaAssetLoop sent, false for a stinger")
    let bg = load["background"] as? JSONObject ?? [:]
    expect(bg["playing"] == nil, "background carries no playing flag")
    expectEqual(bg["mediaAssetPath"] as? String ?? "", "/b/stinger/bg.mp4", "background path")
    let sel = cmds.first { $0["type"] as? String == "set-media-playback" } ?? [:]
    expectEqual(sel["mediaAssetId"] as? String ?? "", "clip", "selection rides the batch")
    expectEqual(sel["mediaAssetPath"] as? String ?? "", "/b/stinger/clip.mp4", "with its path")
    expect(sel["payload"] == nil, "root-level command")
}
```

Fixture invariants: `takeModel`/`sceneEditModel` set s1 `backgroundAssetId = "bg-asset"`
(`ShellTests.swift:1956`), so the bin fixture must contain an asset with id `bg-asset`; s1 already has one
slot layer, so the appended media layer is index 1 (`routeId "s1-layer-1"`).

- [ ] **Step 1:** Tests first (they fail on the current wire, `AppModel.swift:2593, 3234`).
- [ ] **Step 2:** Implement. Harness green.

**Mutations:** keep `mediaAssetPlaying` on the logo-bug route only (red — extend the test to the bug
route if it stays green, and say so); assign `mediaSources` unconditionally (publish count red); treat a
missing capability as available (red); wrap `set-media-playback` in `payload` (red).

**Done when:** green.

---

### Task 11: Cue in Preview, the selected-asset card and playout status

**Lane:** shell. **Slice:** S1.

**Files:** `MediaPlayback.swift`, `AppModel.swift`, `ShellApp.swift` (`MediaPane` card from Task 9),
`ShellTests.swift`.

**Interfaces:**

```swift
enum MediaRoutePlayback {
    /// Assets on fixed media routes, route order, de-duplicated (route wire = buildRoutes output).
    static func programMediaAssetIds(_ routes: [JSONObject]) -> [String]
    static func isRoutedOnProgram(_ assetId: String, programRoutes: [JSONObject]) -> Bool
    static func isLoopingKind(_ kind: String?) -> Bool                          // "background", case-insensitive
    /// StudioViewModel.FormatSelectedMediaAssetSummary (:5878-5898), verbatim.
    static func selectedSummary(name: String?, isOnProgram: Bool, isCuedInPreview: Bool, playing: Bool) -> String
}
extension AppModel {
    /// Windows AddSelectedMediaAssetToPreview (StudioViewModel.cs:14556-14586). Returns the status it set.
    @discardableResult func cueSelectedInPreview() -> String
    var canCueSelectedMedia: Bool { get }
}
```

**Behaviour** (status strings verbatim from Windows unless marked Mac):
- No selection or the asset left the bin → "Select a media asset before adding it to Preview".
- Preview is not an operator scene (missing, or a solo scene, `Self.isSoloScene`) → (Mac)
  "Cue in Preview needs an operator scene in Preview."
- Clip (`supportsPlayback`) while `!mediaClipPlaybackAvailable` → (Mac, spec §4.2)
  "Clip playback is unavailable in this build". `.webm` → D1's sentence. Stills are always cueable.
- Already on a fixed route of the Preview scene's wire → "<name> is already cued in Preview".
- Else `ensureLayers(previewSceneId)` then append
  `SceneLayer(id: "<scene>-layer-<first free index>", slotId: nil, x: 0, y: 0, width: 1, height: 1, fitMode: "fill", opacity: 1, mediaAssetId: id)`
  (Windows `BuildAddedSourceRoute`: full canvas, `FitMode "fill"`, appended on top,
  `SceneRoutingService.cs:105-145`, `SceneRoutingModels.cs:22`), `syncScenes()`, status
  "<name> cued in Preview; Take to show it on Program" (still) or
  "<name> cued in Preview; Take to start playback on Program" (clip).
- Card shows `selectedSummary(...)` with `isOnProgram` from the Program wire, `isCuedInPreview` from the
  Preview wire, `playing` from the selection's on-air flag (S2; false until then); the playout status line
  shows `mediaStatus`, and when `mediaPlayback?.warnings` is non-empty its first warning under it.
- The Cue button is disabled with its reason as the help text whenever `cueSelectedInPreview` would refuse.

**Complete tests:**

```swift
@MainActor
private static func cueModel() -> AppModel {
    let model = sceneEditModel()                        // Preview s1 "Intro" (1 slot layer), Program s2
    model.mediaAssets = [
        MediaAsset(id: "clip", name: "Sting", kind: "stinger", relativePath: "stinger/s.mp4",
                   filePath: "/b/stinger/s.mp4", naturalWidth: 0, naturalHeight: 0),
        MediaAsset(id: "logo", name: "Logo", kind: "lower-third", relativePath: "lower-third/l.png",
                   filePath: "/b/lower-third/l.png", naturalWidth: 0, naturalHeight: 0),
        MediaAsset(id: "bg-asset", name: "BG", kind: "lower-third", relativePath: "lower-third/bg.png",
                   filePath: "/b/lower-third/bg.png", naturalWidth: 0, naturalHeight: 0)]
    return model
}

@MainActor
private static func testCueInPreviewAddsAFullCanvasMediaLayerAndRefusesADuplicate() {
    let model = cueModel()
    model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]])
    expectEqual(model.cueSelectedInPreview(), "Select a media asset before adding it to Preview", "no selection")
    model.selectedAssetId = "clip"
    expectEqual(model.cueSelectedInPreview(),
                "Sting cued in Preview; Take to start playback on Program", "clip wording")
    let layer = model.scenes[0].layers.last!
    expectEqual(layer.mediaAssetId, "clip", "media layer"); expectEqual(layer.id, "s1-layer-1", "first free index")
    expectEqual([layer.x, layer.y, layer.width, layer.height], [0, 0, 1, 1], "full canvas")
    expectEqual(layer.fitMode, "fill", "Windows default fit")
    expectEqual(model.scenes[0].layers.count, 2, "appended on top of the existing layer")
    expectEqual(model.cueSelectedInPreview(), "Sting is already cued in Preview", "duplicate refused")
    expectEqual(model.scenes[0].layers.count, 2, "no second layer")
    model.selectedAssetId = "logo"
    expectEqual(model.cueSelectedInPreview(), "Logo cued in Preview; Take to show it on Program", "still wording")
}

@MainActor
private static func testCueRefusesAClipWhenTheBuildCannotPlayClipsButCuesStills() {
    let model = cueModel()                              // no capability snapshot -> unavailable
    model.selectedAssetId = "clip"
    expectEqual(model.cueSelectedInPreview(), "Clip playback is unavailable in this build", "clip refused")
    expectEqual(model.scenes[0].layers.count, 1, "nothing added")
    model.selectedAssetId = "logo"
    expect(model.cueSelectedInPreview().hasPrefix("Logo cued in Preview"), "stills still cue")
}

@MainActor
private static func testCueRefusesASoloPreview() {
    let model = cueModel()
    model.applySnapshotForTesting(["profile": ["capabilityStates": ["media-clip-playback": ["state": "available"]]]])
    model.selectedAssetId = "logo"
    model.previewSceneId = "mv-solo-a"                  // a solo scene id (AppModel.isSoloScene)
    expectEqual(model.cueSelectedInPreview(), "Cue in Preview needs an operator scene in Preview.", "solo refused")
}

private static func testSelectedSummaryIsWindowsWording() {
    typealias P = MediaRoutePlayback
    expectEqual(P.selectedSummary(name: nil, isOnProgram: false, isCuedInPreview: false, playing: false),
                "No media asset selected", "none")
    expectEqual(P.selectedSummary(name: "Sting", isOnProgram: true, isCuedInPreview: false, playing: true),
                "Playing Sting on Program", "live")
    expectEqual(P.selectedSummary(name: "Sting", isOnProgram: true, isCuedInPreview: true, playing: false),
                "Sting paused on Program", "Program wins over Preview")
    expectEqual(P.selectedSummary(name: "Sting", isOnProgram: false, isCuedInPreview: true, playing: false),
                "Sting cued in Preview", "cued")
    expectEqual(P.selectedSummary(name: "Sting", isOnProgram: false, isCuedInPreview: false, playing: false),
                "Sting ready to cue", "idle")
}
```

Fixture invariant: `"mv-solo-a"` is `AppModel.soloSceneA` (`AppModel.swift:2892-2899`), so
`isSoloScene` is true for it; it is not in `model.scenes`, which the refusal must not depend on (the
solo check comes before the scene lookup).

- [ ] **Step 1:** Tests first; implement; harness green.
- [ ] **Step 2:** Tab snapshots: empty bin, clip selected and cued (Preview badge), clip on Program.
- [ ] **Step 3: Owner check 1 (needs Tasks 2-3 merged into the build):** import a sting; Cue; Preview shows
  its first frame; Take; it rolls from frame 0 (spec §6.4 check 1, video half).

**Mutations:** duplicate check on Program wire instead of Preview (red); append at index 0 (red: layer
order); allow clips when unavailable (red); fit "fit" (red).

**Done when:** green, snapshots attached, owner check scheduled with the S1 build.

---

### Task 12: Play/Pause — tap rules, the bin projection, the gesture ledger, row play state, the preview binding

**Lane:** shell. **Slice:** S2.

**Files:** `MediaPlayback.swift`, `AppModel.swift`, `ShellApp.swift`, `MediaPreviewPlayer.swift`,
`ShellTests.swift`.

**Interfaces** (Swift ports; names map 1:1 to the Windows oracle):

```swift
extension MediaRoutePlayback {
    enum TapAction: Equatable { case select, pause, resume }
    static func resolveTap(isOnProgram: Bool, isLooping: Bool, state: String?) -> TapAction          // :165-181
    static func resolveTransportRow(_ assetId: String?, rows: [MediaSourceRow]) -> MediaSourceRow?   // :216-246
    static func resolveTransportState(_ assetId: String?, rows: [MediaSourceRow]) -> String?
    static func isPlayingOnAir(_ assetId: String, rows: [MediaSourceRow]) -> Bool
    static func actionLabel(isOnProgram: Bool, playing: Bool) -> String     // StudioViewModel.cs:5901-5904
}
enum MediaBinPlaybackProjection {                                           // MediaBinPlaybackProjection.cs
    struct Result: Equatable {
        let liveSignature: String; let rewriteBinScalars: Bool; let rewriteSelection: Bool
        let selectedPlaying: Bool; let selectedStatus: String?; let selectedTransportState: String?
    }
    static func resolve(rows: [MediaSourceRow], lastLiveSignature: String, lastSelectedTransportState: String?,
                        selectedId: String?, selectedName: String?, selectedPlaying: Bool) -> Result
    static func binRowIsPlaying(_ assetId: String, supportsPlayback: Bool, rows: [MediaSourceRow],
                                selectedId: String?, selectedPlaying: Bool) -> Bool
    static func liveSignature(_ rows: [MediaSourceRow]) -> String
}
final class MediaTransportGestureLedger {                                   // MediaTransportGestureLedger.cs
    enum Step: Equatable { case superseded, retry, giveUp }
    init(retryAttempts: Int = 5)
    func claim(_ assetId: String) -> Int64
    func shouldSend(_ assetId: String, _ token: Int64) -> Bool
    func onSkipped(_ assetId: String, _ token: Int64, attempt: Int) -> Step
    @discardableResult func release(_ assetId: String, _ token: Int64) -> Bool
}
extension AppModel {
    func playMediaAsset(_ id: String)            // Windows PlayMediaAsset :6326-6390
    func toggleSelectedMediaPlayback()           // :6492-6502 ("Select a media asset before playback")
    @Published private(set) var selectedMediaPlaying: Bool
    @Published private(set) var mediaLiveSignature: String   // the only bin-wide play-state scalar
    var mediaPlaybackButtonLabel: String { get }
    var mediaTransportSendForTesting: ((JSONObject) async throws -> JSONObject)?   // seam; default outputRequest
}
```

**Behaviour:**
- `playMediaAsset`: as Windows `:6326-6390` — tap = `resolveTap(isOnProgram (Program wire),
  isLoopingKind(kind), resolveTransportState(id, mediaSources))`; select the asset; `.pause` → send the
  gesture `"pause"`, `selectedMediaPlaying = false`; `.resume` → `"play"`, `true`; `.select` →
  `selectedMediaPlaying = !(sameAsset && selectedMediaPlaying)` (local audition). Status, verbatim:
  on Program "Playing <name> on Program" / "<name> paused on Program"; off Program "Auditioning <name>" /
  "<name> audition paused". Then `syncScenes()` (selection rides the batch, Task 10).
- **The gesture is one-shot** (never part of `sceneSyncBatch`, never re-sent on a core respawn): `claim` a
  token, then `attempt(token, 0)`: if `!shouldSend` return; send
  `mediaCoreSync(elapsedMs, [{"type":"set-media-transport","mediaAssetId":id,"action":a}])` through
  `outputRequest`; success → `release` and `applySnapshot(response["snapshot"])`; `BridgeError.timeout`,
  `.notRunning`, `.processExited` → `onSkipped(…, attempt)`: `.retry` re-arms after 120 ms on the main
  actor (it asks `shouldSend` again before sending), `.giveUp` →
  status "Could not <pause|play> <name> - the media core stayed busy. Press it again." (D8, Windows
  `:6466-6474`); `BridgeError.remote(msg)` → if `release` then
  "Could not <pause|play> <name>; the media core rejected it. <msg>".
- Snapshot apply runs `MediaBinPlaybackProjection.resolve` with the stored `mediaLiveSignature` and
  `lastSelectedTransportState`: when `rewriteBinScalars`, assign `mediaLiveSignature` (rows read it);
  when `rewriteSelection`, assign `selectedMediaPlaying` and, if `selectedStatus` is non-nil, `mediaStatus`.
  Nothing else on the bin is reassigned at snapshot rate.
- Bin row play button: label "Play"/"Pause" for `supportsPlayback`, "Still" otherwise (Windows
  `PlaybackLabel`, `ProductionModels.cs:902`); state from `binRowIsPlaying`; tap = `playMediaAsset`.
  Disabled with "Clip playback is unavailable in this build" for clips when the capability is not
  available.
- The card's transport button reads `actionLabel(isOnProgram:playing:)`: "Pause Program" / "Resume
  Program" / "Pause audition" / "Audition".
- `MediaPreviewPlayer` plays when `selectedMediaPlaying` and pauses otherwise (muted, D4); it never seeks
  on a toggle.
- A refused gesture: the next snapshot's projection reverts the flag; if `programFrame.warnings`
  contains `"media:<id> is a loop…"` or `"media:<id> is not on Program…"` (`MediaTransportPolicy.h:51-52`),
  that text becomes `mediaStatus`.

**Complete tests** — the Windows tests ported **by name**. Each must be shown failing against the
pre-task code first (the type does not exist → compile failure counts as the red for new types; for
`AppModel` cases, run them against a stubbed `playMediaAsset` that only selects).

```swift
private static func row(_ source: String, _ asset: String, _ state: String,
                        onProgram: Bool = true, onPreview: Bool = false) -> MediaSourceRow {
    MediaSourceRow(sourceId: source, mediaAssetId: asset, state: state, loop: source.hasPrefix("background:"),
                   onProgram: onProgram, onPreview: onPreview, positionMs: 0, durationMs: -1,
                   decodedVideoFrames: nil, lastDecodedAgeMs: nil, presentedVideoFrames: nil,
                   lastPresentedAgeMs: nil, videoQueued: nil, decoderRestarts: nil)
}

// MediaRoutePlaybackServiceTests (Windows) — names kept.
private static func testResolveTap() {
    typealias P = MediaRoutePlayback
    expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: "live"), .pause, "ResolveTap_ALiveProgramClipPauses")
    expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: "paused"), .resume, "ResolveTap_APausedOrEndedProgramClipResumes/paused")
    expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: "ended"), .resume, "…/ended")
    expectEqual(P.resolveTap(isOnProgram: false, isLooping: false, state: "live"), .select, "ResolveTap_AClipNotOnProgramIsJustSelected")
    expectEqual(P.resolveTap(isOnProgram: true, isLooping: true, state: "live"), .select, "ResolveTap_ALoopOnProgramIsJustSelectedNeverPaused")
    expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: nil), .select, "ResolveTap_AnUnknownOrMissingStateIsJustSelected/nil")
    expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: "unavailable"), .select, "…/unavailable")
}
private static func testResolveTransportStateAndOnAir() {
    typealias P = MediaRoutePlayback
    expectEqual(P.resolveTransportState("clip", rows: [row("media:clip", "clip", "paused")]), "paused",
                "ResolveTransportState_ReturnsTheRowsStateForTheAsset")
    expect(P.resolveTransportState("clip", rows: []) == nil, "ResolveTransportState_ReturnsNullWhenTheCoreHasNoRow")
    expect(P.resolveTransportState(nil, rows: [row("media:clip", "clip", "live")]) == nil, "…/nil asset")
    let both = [row("background:clip", "clip", "live"), row("media:clip", "clip", "paused")]
    expectEqual(P.resolveTransportState("clip", rows: both), "paused",
                "ResolveTransportState_TheRouteRowWinsWhenAnAssetIsBothARouteAndABackground")
    expectEqual(P.resolveTransportState("bg", rows: [row("background:bg", "bg", "live")]), "live",
                "ResolveTransportState_ABackgroundRowStillAnswersWhenTheAssetHasNoRoute")
    expectEqual(P.resolveTap(isOnProgram: true, isLooping: false, state: P.resolveTransportState("clip", rows: both)),
                .resume, "ResolveTap_ALiveBackgroundRowCannotMakeAPausedClipLookPausable")
    expect(P.isPlayingOnAir("clip", rows: [row("media:clip", "clip", "live")]), "IsPlayingOnAir_ReadsTheCoreRowLive")
    for s in ["paused", "cued", "ended"] {
        expect(!P.isPlayingOnAir("clip", rows: [row("media:clip", "clip", s)]), "IsPlayingOnAir_ReadsTheCoreRowPausedAsNotPlaying/\(s)")
    }
    expect(!P.isPlayingOnAir("clip", rows: []), "IsPlayingOnAir_ReadsTheCoreRowAbsentAsNotPlaying")
    expect(!P.isPlayingOnAir("clip", rows: [row("media:other", "other", "live")]), "…/another asset")
    expect(P.isPlayingOnAir("bg", rows: [row("background:bg", "bg", "live")]), "IsPlayingOnAir_ALoopingBackgroundRowAnswersForItsAsset")
    expectEqual(P.actionLabel(isOnProgram: true, playing: true), "Pause Program", "label")
    expectEqual(P.actionLabel(isOnProgram: true, playing: false), "Resume Program", "label")
    expectEqual(P.actionLabel(isOnProgram: false, playing: true), "Pause audition", "label")
    expectEqual(P.actionLabel(isOnProgram: false, playing: false), "Audition", "label")
}

// MediaBinPlaybackProjectionTests (Windows) — names kept, bodies 1:1.
private static func testMediaBinPlaybackProjection() {
    typealias J = MediaBinPlaybackProjection
    var r = J.resolve(rows: [row("media:clip", "clip", "cued", onProgram: false, onPreview: true)],
                      lastLiveSignature: "", lastSelectedTransportState: nil,
                      selectedId: "clip", selectedName: "Clip", selectedPlaying: true)
    expect(!r.rewriteSelection && r.selectedPlaying && r.selectedStatus == nil, "ACuedPreviewRowNeverClearsALocalAudition")
    r = J.resolve(rows: [], lastLiveSignature: "", lastSelectedTransportState: nil,
                  selectedId: "clip", selectedName: "Clip", selectedPlaying: true)
    expect(!r.rewriteSelection && r.selectedPlaying, "NoRowAtAllNeverClearsALocalAudition")
    r = J.resolve(rows: [row("media:clip", "clip", "live")], lastLiveSignature: "", lastSelectedTransportState: nil,
                  selectedId: "clip", selectedName: "Clip", selectedPlaying: false)
    expect(r.rewriteSelection && r.selectedPlaying, "AProgramRowOwnsTheSelectedFlag/live")
    expectEqual(r.selectedStatus ?? "", "Playing Clip on Program", "…/live status")
    r = J.resolve(rows: [row("media:clip", "clip", "paused")], lastLiveSignature: "", lastSelectedTransportState: nil,
                  selectedId: "clip", selectedName: "Clip", selectedPlaying: true)
    expect(r.rewriteSelection && !r.selectedPlaying, "AProgramRowOwnsTheSelectedFlag/paused")
    expectEqual(r.selectedStatus ?? "", "Clip paused on Program", "…/paused status")
    r = J.resolve(rows: [row("media:clip", "clip", "ended")], lastLiveSignature: "", lastSelectedTransportState: "paused",
                  selectedId: "clip", selectedName: "Clip", selectedPlaying: false)
    expect(r.rewriteSelection && !r.selectedPlaying, "TheStatusIsRewrittenWhenTheSTATEMovesEvenThoughTheBooleanDoesNot")
    expectEqual(r.selectedStatus ?? "", "Clip ended on Program", "…/status")
    expectEqual(r.selectedTransportState ?? "", "ended", "…/state")
    r = J.resolve(rows: [row("media:clip", "clip", "live")], lastLiveSignature: "clip", lastSelectedTransportState: "live",
                  selectedId: "clip", selectedName: "Clip", selectedPlaying: true)
    expect(!r.rewriteSelection && !r.rewriteBinScalars, "ASettledSelectionWritesNothing")
    let rows = [row("media:clip", "clip", "live")]
    let first = J.resolve(rows: rows, lastLiveSignature: "", lastSelectedTransportState: nil,
                          selectedId: nil, selectedName: nil, selectedPlaying: false)
    expect(first.rewriteBinScalars, "TheBinScalarsAreRewrittenOnlyWhenTheLiveSetMoves/first")
    expectEqual(first.liveSignature, "clip", "…/signature")
    let second = J.resolve(rows: rows, lastLiveSignature: first.liveSignature, lastSelectedTransportState: nil,
                           selectedId: nil, selectedName: nil, selectedPlaying: false)
    expect(!second.rewriteBinScalars, "…/second")
    let other = [row("media:other", "other", "live")]
    expect(J.binRowIsPlaying("other", supportsPlayback: true, rows: other, selectedId: nil, selectedPlaying: false),
           "ABinRowsIndicatorIsTheSameExpressionAsTheOperatorRebuild/on air")
    expect(J.binRowIsPlaying("clip", supportsPlayback: true, rows: other, selectedId: "clip", selectedPlaying: true),
           "…/local audition")
    expect(!J.binRowIsPlaying("other", supportsPlayback: false, rows: other, selectedId: nil, selectedPlaying: false),
           "…/still")
    r = J.resolve(rows: [row("background:clip", "clip", "live"), row("media:clip", "clip", "paused")],
                  lastLiveSignature: "", lastSelectedTransportState: nil,
                  selectedId: "clip", selectedName: "Clip", selectedPlaying: true)
    expect(r.rewriteSelection && !r.selectedPlaying, "ALiveBackgroundRowDoesNotSpeakForAClipThatIsAlsoARoute")
    expectEqual(r.selectedStatus ?? "", "Clip paused on Program", "…/status")
}

// MediaTransportGestureLedgerTests (Windows) — names kept.
private static func testMediaTransportGestureLedger() {
    var l = MediaTransportGestureLedger()
    let pause = l.claim("clip")
    expect(l.shouldSend("clip", pause), "ASupersededRetryIsRefusedBeforeItSends/a")
    expectEqual(l.onSkipped("clip", pause, attempt: 0), .retry, "…/b")
    let resume = l.claim("clip")
    expect(l.shouldSend("clip", resume), "…/c"); expect(l.release("clip", resume), "…/d")
    expect(!l.shouldSend("clip", pause), "…/stale pause refused")
    l = MediaTransportGestureLedger()
    let p2 = l.claim("clip"), r2 = l.claim("clip")
    expect(!l.shouldSend("clip", p2) && l.shouldSend("clip", r2), "ANewerTapSupersedesAnOlderOneWhileItIsStillInFlight")
    expectEqual(l.onSkipped("clip", p2, attempt: 0), .superseded, "…/superseded")
    expect(!l.release("clip", p2) && l.shouldSend("clip", r2) && l.release("clip", r2),
           "ASupersededGestureNeverReleasesTheNewerOnesSlot")
    l = MediaTransportGestureLedger()
    let intro = l.claim("intro"), outro = l.claim("outro")
    expect(l.shouldSend("intro", intro) && l.shouldSend("outro", outro), "TheSlotIsPerAssetSoTwoClipsDoNotSupersedeEachOther")
    l = MediaTransportGestureLedger(retryAttempts: 2)
    let t = l.claim("clip")
    expectEqual(l.onSkipped("clip", t, attempt: 0), .retry, "ASkippedGestureReArmsUntilTheAttemptBudgetRunsOut/0")
    expectEqual(l.onSkipped("clip", t, attempt: 1), .retry, "…/1")
    expectEqual(l.onSkipped("clip", t, attempt: 2), .giveUp, "…/2")
    expect(!l.shouldSend("clip", t), "…/released on give-up")
    l = MediaTransportGestureLedger()
    let done = l.claim("clip"); l.release("clip", done)
    expect(!l.shouldSend("clip", done), "ACompletedGestureCannotSendAgain")
    expect(!l.shouldSend("never-claimed", 1), "AnUnknownAssetNeverSends")
}
```

`AppModel` integration (drives the real send path through the seam; rule 10: drive the structure):

```swift
@MainActor
private static func testPlayMediaAssetSendsOneShotGesturesAndRetriesOnlyTheLatest() {
    // Fixture: Program s2 carries a fixed media route for "clip" (append a SceneLayer with
    // mediaAssetId "clip" to s2 after ensureLayers); the core row says live.
    let model = cueModel()
    model.ensureLayers(for: "s2")
    model.scenes[1].layers.append(SceneLayer(id: "s2-layer-9", slotId: nil, mediaAssetId: "clip"))
    model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:clip", "mediaAssetId": "clip",
                                                     "state": "live", "onProgram": true]]])
    var sent: [JSONObject] = []
    var failNext = 1                                   // the FIRST send times out, then succeed
    model.mediaTransportSendForTesting = { body in
        sent.append(body)
        if failNext > 0 { failNext -= 1; throw BridgeError.timeout }
        return ["ok": true]
    }
    model.playMediaAsset("clip")                       // live on Program -> pause
    expect(!model.selectedMediaPlaying, "pause flips the flag at once")
    expectEqual(model.mediaStatus, "Sting paused on Program", "status")
    spin(until: { sent.count == 2 }, limit: 2)
    let gestures = sent.compactMap { ($0["commands"] as? [JSONObject])?.first }
    expectEqual(gestures.map { $0["type"] as? String ?? "" }, ["set-media-transport", "set-media-transport"],
                "a skipped send re-arms the SAME gesture")
    expectEqual(gestures.map { $0["action"] as? String ?? "" }, ["pause", "pause"], "pause twice")
    // Supersession end to end: tap 1 times out and is re-armed; tap 2 lands first; tap 1 must not send.
    sent = []; failNext = 1
    model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:clip", "mediaAssetId": "clip",
                                                     "state": "paused", "onProgram": true]]])
    model.playMediaAsset("clip")                       // paused -> play (times out, retry armed)
    model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:clip", "mediaAssetId": "clip",
                                                     "state": "live", "onProgram": true]]])
    model.playMediaAsset("clip")                       // live -> pause (succeeds)
    spin(until: { false }, limit: 0.4)                 // let the 120 ms retry fire if it would
    let actions = sent.compactMap { (($0["commands"] as? [JSONObject])?.first?["action"]) as? String }
    expectEqual(actions, ["play", "pause"], "the stale play retry never reaches the core")
    expect(!sent.contains { body in (body["commands"] as? [JSONObject] ?? []).contains { $0["type"] as? String == "load-scene-graph" } },
           "the gesture is never folded into a scene sync")
}

@MainActor
private static func testAGestureThatStaysBusyGivesUpVisibly() {
    let model = cueModel()
    model.ensureLayers(for: "s2")
    model.scenes[1].layers.append(SceneLayer(id: "s2-layer-9", slotId: nil, mediaAssetId: "clip"))
    model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:clip", "mediaAssetId": "clip",
                                                     "state": "live", "onProgram": true]]])
    var sends = 0
    model.mediaTransportSendForTesting = { _ in sends += 1; throw BridgeError.timeout }
    model.playMediaAsset("clip")
    spin(until: { model.mediaStatus.hasPrefix("Could not") }, limit: 2)
    expectEqual(sends, 6, "attempt 0 + 5 retries")
    expectEqual(model.mediaStatus, "Could not pause Sting - the media core stayed busy. Press it again.", "Windows wording")
}

@MainActor
private static func testAnOffProgramTapIsALocalAuditionAndSendsNothing() {
    let model = cueModel()
    var sends = 0
    model.mediaTransportSendForTesting = { _ in sends += 1; return [:] }
    model.playMediaAsset("clip")
    expect(model.selectedMediaPlaying, "audition on"); expectEqual(model.mediaStatus, "Auditioning Sting", "status")
    model.playMediaAsset("clip")
    expect(!model.selectedMediaPlaying, "audition off"); expectEqual(model.mediaStatus, "Sting audition paused", "status")
    expectEqual(sends, 0, "no core gesture for an off-Program clip")
    expectEqual(model.mediaPlaybackButtonLabel, "Audition", "label")
}
```

Fixture invariants: `cueModel()`'s Program is s2 "Panel" with no layers until `ensureLayers` seeds the
two-up preset; the appended media layer's `slotId` is nil and `mediaAssetId` set, so `buildRoutes` emits a
fixed media route for it (`AppModel.swift:2583-2595`). `spin(until:limit:)` is the harness helper used by
`runTake` (`ShellTests.swift:2964`).

- [ ] **Step 1:** Tests first (each named case red: compile failure for new types; for the AppModel cases,
  run against a `playMediaAsset` that only selects and record the reds).
- [ ] **Step 2:** Implement. Harness green.
- [ ] **Step 3: Owner check 2 (needs N1 + N2 in the build):** pause on air, wait ten seconds, resume —
  the picture continues from the held frame, audio does not jump, nothing restarts (spec §6.4 check 2).

**Mutations to run:**
- Gate only the retry scheduling, not the send (`shouldSend` checked only in `onSkipped`): the
  supersession case red ("play" retried after "pause").
- Background row answers before the route row: `ResolveTransportState_TheRouteRowWins…` and
  `ALiveBackgroundRowDoesNotSpeak…` red.
- Projection adopts a Preview-only row: `ACuedPreviewRowNeverClearsALocalAudition` red.
- Rewrite status only when the boolean moves: `TheStatusIsRewrittenWhenTheSTATEMoves…` red.
- Send the gesture inside `sceneSyncBatch`: the "never folded into a scene sync" assertion red.
- Treat `BridgeError.remote` as skipped: add the one-line assertion "a remote error sends once" if this
  mutation stays green, then re-run.

**Done when:** green, mutations reported, owner check 2 scheduled.

---

### Task 13: Take promotion and the media-selection rollback

**Lane:** shell. **Slice:** S2.

**Files:** `MediaPlayback.swift`, `AppModel.swift` (`take()` `:3004-3060`), `ShellTests.swift`.

**Interfaces:**

```swift
extension MediaRoutePlayback {
    static func assetsEnteringProgram(previous: [JSONObject], current: [JSONObject]) -> [String]      // :81-102
    static func chooseAssetToPromote(_ wentLive: [String], selected: String?,
                                     supportsPlayback: ((String) -> Bool)?) -> String?             // :111-125
    static func selectedAssetLeftProgram(_ selected: String?, previous: [JSONObject], current: [JSONObject]) -> Bool // :34-46
}
struct MediaSelectionState: Equatable {
    var assetId: String?; var name: String?; var supportsPlayback: Bool; var playing: Bool; var status: String
}
enum TakeMediaSelectionRollback {                                       // TakeMediaSelectionRollback.cs:61-88
    static func resolve(beforeTake: MediaSelectionState, afterTake: MediaSelectionState,
                        current: MediaSelectionState, attemptedProgramRoutes: [JSONObject],
                        restoredProgramRoutes: [JSONObject], rows: [MediaSourceRow]) -> MediaSelectionState
}
extension AppModel { func captureMediaSelection() -> MediaSelectionState; func restoreMediaSelection(_ s: MediaSelectionState) }
```

**Behaviour** (Windows ordering, spec correction 6): in `take()`, capture `before = captureMediaSelection()`
and `previousRoutes = buildRoutes(for: previousProgram, isProgram: true)` **before** the swap; after the
local swap compute `entering`; promote = `chooseAssetToPromote(entering, selected, supportsPlayback:)`;
if promoted: select it, `selectedMediaPlaying = true`, `mediaStatus = "Playing <name> on Program"`
(`StudioViewModel.cs:6289-6323`); else if the Program media id set changed and
`selectedAssetLeftProgram(...)`: `selectedMediaPlaying = false`, `mediaStatus = "<name> left Program"`
(`:6264-6283`). Capture `after` and `attempted = buildRoutes(for: taken, isProgram: true)`. On failure,
when the scene rollback runs (the existing `untouched` branch), call `syncScenes()` first (reconciliation,
Windows `:281`), then `restoreMediaSelection(TakeMediaSelectionRollback.resolve(before, after,
captureMediaSelection(), attempted, buildRoutes(for: previousProgram…), mediaSources))`. When the scene
rollback is refused (`newer local edits preserved`), the selection is left exactly as it is.

**Complete tests** (the Windows `Take_Rollback*` set, ported by name onto the real Mac `take()` with
`FakeTakeCore.failure`):

```swift
@MainActor
private static func mediaTakeModel(programClips: [String], previewClips: [String]) -> (AppModel, FakeTakeCore) {
    // Program "intro", Preview "interview"; each scene is ONE layer per clip id (slotless media layers).
    let core = FakeTakeCore()
    let model = AppModel()
    func scene(_ id: String, _ clips: [String]) -> SceneDef {
        SceneDef(id: id, name: id.capitalized, layout: "single",
                 layers: clips.enumerated().map { SceneLayer(id: "\(id)-layer-\($0.offset)", slotId: nil, mediaAssetId: $0.element) })
    }
    model.scenes = [scene("intro", programClips), scene("interview", previewClips)]
    model.mediaAssets = ["bed", "x", "y", "z"].map {
        MediaAsset(id: $0, name: $0.uppercased(), kind: "stinger", relativePath: "stinger/\($0).mp4",
                   filePath: "/b/stinger/\($0).mp4", naturalWidth: 0, naturalHeight: 0)
    }
    model.programSceneId = "intro"; model.previewSceneId = "interview"
    model.quitRequestForTesting = { body, _ in try await core.handle(body) }
    model.status = .connected(renderer: "test")
    return (model, core)
}

@MainActor
private static func testTakeRollbackRestoresTheSelectionAClipThatWentLiveTookOver() {
    let (model, core) = mediaTakeModel(programClips: [], previewClips: ["y"])
    model.selectMediaAsset("bed")                      // "BED ready to cue or audition"
    let prior = model.captureMediaSelection()
    core.failure = "core answered with an unparseable snapshot"
    _ = runTake(model)
    expectEqual(model.captureMediaSelection(), prior, "Take_RollbackRestoresTheSelectionAClipThatWentLiveTookOver")
    expectEqual(model.mediaPlaybackButtonLabel, "Audition", "…/toggle")
}

@MainActor
private static func testTakeRollbackKeepsAPausedProgramClipPausedWithAnHonestToggle() {
    let (model, core) = mediaTakeModel(programClips: ["x"], previewClips: ["x", "y"])
    model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:x", "mediaAssetId": "x",
                                                     "state": "paused", "onProgram": true]]])
    model.selectMediaAsset("x")
    expectEqual(model.mediaPlaybackButtonLabel, "Resume Program", "precondition")
    core.failure = "media core did not answer within 4000 ms"
    _ = runTake(model)
    expectEqual(model.selectedAssetId, "x", "Take_RollbackKeepsAPausedProgramClipPaused…/selection")
    expect(!model.selectedMediaPlaying, "…/still paused")
    expectEqual(model.mediaStatus, "X paused on Program", "…/status")
    expectEqual(model.mediaPlaybackButtonLabel, "Resume Program", "…/toggle")
}

@MainActor
private static func testTakeRollbackPutsARollingClipThatLeftProgramBackOnPauseProgram() {
    let (model, core) = mediaTakeModel(programClips: ["x"], previewClips: [])
    model.applySnapshotForTesting(["mediaSources": [["sourceId": "media:x", "mediaAssetId": "x",
                                                     "state": "live", "onProgram": true]]])
    model.selectMediaAsset("x")
    expect(model.selectedMediaPlaying, "precondition: selecting a live Program clip reads playing")
    core.failure = "native rejected scene"
    _ = runTake(model)
    expect(model.selectedMediaPlaying, "Take_RollbackPutsARollingClipThatLeftProgramBackOnPauseProgram")
    expectEqual(model.mediaStatus, "Playing X on Program", "…/status")
    expectEqual(model.mediaPlaybackButtonLabel, "Pause Program", "…/toggle")
}

@MainActor
private static func testTakeRefusedRollbackLeavesTheSelectionAlone() {
    let (model, core) = mediaTakeModel(programClips: [], previewClips: ["y"])
    model.selectMediaAsset("bed")
    core.hold = true; core.failure = "native rejected scene"
    let box = runTake(model, wait: false)
    spin(until: { core.bodies.count == 1 }, limit: 2)
    model.previewSceneId = "intro"                     // a newer edit while the sync is pending (still "intro" exists)
    model.renameScene("interview", to: "Interview 2")  // any edit the rollback guard treats as newer
    core.hold = false
    spin(until: { box.result != nil }, limit: 3)
    expectEqual(model.selectedAssetId, "y", "Take_RefusedRollbackLeavesTheSelectionAlone")
}

@MainActor
private static func testTakeRollbackGivesAnOperatorPickThatLeftProgramOffProgramWording() {
    let (model, core) = mediaTakeModel(programClips: [], previewClips: ["y", "z"])
    model.selectMediaAsset("bed")
    core.hold = true; core.failure = "native rejected scene"
    let box = runTake(model, wait: false)
    spin(until: { core.bodies.count == 1 }, limit: 2)
    model.selectMediaAsset("z")                        // the operator's pick during the pending sync
    core.hold = false
    spin(until: { box.result != nil }, limit: 3)
    expectEqual(model.selectedAssetId, "z", "Take_RollbackGivesAnOperatorPickThatLeftProgramOffProgramWording/kept")
    expect(!model.selectedMediaPlaying, "…/not playing")
    expectEqual(model.mediaStatus, "Z left Program", "…/status")
    expectEqual(model.mediaPlaybackButtonLabel, "Audition", "…/toggle")
}

@MainActor
private static func testASuccessfulTakePromotesTheClipThatWentLiveButNeverAStill() {
    let (model, _) = mediaTakeModel(programClips: [], previewClips: ["y"])
    model.mediaAssets.append(MediaAsset(id: "logo", name: "LOGO", kind: "lower-third",
        relativePath: "lower-third/logo.png", filePath: "/b/lower-third/logo.png", naturalWidth: 0, naturalHeight: 0))
    model.scenes[1].layers.insert(SceneLayer(id: "interview-layer-s", slotId: nil, mediaAssetId: "logo"), at: 0)
    model.selectMediaAsset("bed")
    _ = runTake(model)
    expectEqual(model.selectedAssetId, "y", "AStillGoingLiveIsNeverPromoted: the clip, not the logo")
    expect(model.selectedMediaPlaying, "promoted clip reads playing")
    expectEqual(model.mediaStatus, "Playing Y on Program", "status")
}
```

Plus the pure ports, by name: `AssetsEnteringProgram_NamesAClipThatEntersProgram` / `_IgnoresAClipThatStaysOnProgram`
/ `_IgnoresAClipThatLeavesProgram`, `ChooseAssetToPromote_PrefersTheSelectedAssetWhenItWentLive` /
`_OtherwiseTakesTheFirstWentLiveAsset` / `_NothingWentLivePromotesNothing`, `AStillGoingLiveIsNeverPromoted`
(`chooseAssetToPromote(["logo"], "logo", stills) == nil`, `(["logo","clip"], "logo", stills) == "clip"`),
and the five `SelectedAssetLeftProgram_*` cases — fixtures copied from
`MediaRoutePlaybackServiceTests.cs:69-89, 172-250`, with routes built as
`["mode": "fixed", "mediaAssetId": id]` dictionaries.

Fixture invariant for the refused-rollback case: the Mac rollback guard (`AppModel.swift:3045-3047`)
compares `programSceneId`, `previewSceneId` and the two solo cues; the "newer edit" must change one of
those. The implementer chooses the smallest edit that trips the guard (changing `previewSceneId` to
`"intro"` does), drops the rename line if it is not needed, and states which in the report.

- [ ] **Step 1:** Tests first (all rollback cases red against today's `take()`, which never touches the
  selection — record which assertions fail).
- [ ] **Step 2:** Implement. Harness green.
- [ ] **Step 3: Owner checks 3 and 6:** Take to another scene and back — the clip rolls from 0; remove a clip
  a scene uses — refused, with the scene named (spec §6.4).

**Mutations to run:**
- Promote every asset on the new Program (not only those entering): the "stays on Program" pure case red.
- Drop the `supportsPlayback` filter: `AStillGoingLiveIsNeverPromoted` red (selection = logo).
- Restore the saved `playing` flag instead of the on-air row: `…PutsARollingClipThatLeftProgramBack…`
  red.
- Restore the pre-Take selection even when the operator moved it: `…OperatorPick…` red.
- Restore the selection on a refused rollback: `Take_RefusedRollbackLeavesTheSelectionAlone` red.

**Done when:** green, mutations reported, owner checks scheduled with the S2 build.

---

## Integration and owner checks (the Phase 1 media build)

Build the signed, notarized `.pkg` with every option on (`scripts/release-macos.sh`, which now carries
`-DCOREVIDEO_WITH_AVF_MEDIA_DECODER=ON`; the release gate runs the full native ctest including the new
tests). Install from the `.pkg` (never the build tree — system-level proof is on the installed build).
Then the owner runs spec §6.4 on the installed app:

1. Import a sting; Cue; Preview shows its first frame; Take; it rolls from frame 0 with audio on master,
   in the recording and in the stream (Tasks 2-3, 6-7, 11).
2. Pause on air, wait ten seconds, resume: picture continues from the held frame, audio does not jump,
   nothing restarts (Tasks 6, 12).
3. Take to another scene and back: the clip rolls from 0 (core policy; Task 13 keeps the selection honest).
4. A looping video background sits behind guests across several Takes without restarting (Tasks 3, 5).
5. Mute the Media strip: the clip is silent on every bus (Task 7's test is the unit proof; this is the live one).
6. Remove a clip a scene uses: refused, with the scene named (Task 8).
7. (Deferred to N3) ProRes 4444 sting keyed over a guest — Task 2 proves straight alpha reaches the owner;
   compositing proof is N3.

And the performance verdict from Task 4 is attached to the release notes for the build.

## Risks carried from the spec

| Risk | Where this plan handles it |
|---|---|
| Two 8.3 MB BGRA copies per 1080p frame cost Program frames at load 8 | Task 2 logs per-reader copy cost; Task 4 measures `--load 8` with a 1080p60 clip + 1080p loop against a same-session baseline; failure moves N4 up (D5). |
| VideoToolbox alpha straight vs premultiplied | Task 2 `AProRes4444AlphaClipReachesTheOwnerAsStraightAlpha` decides; conversion only for alpha codecs. |
| Colour range/matrix | Task 2 `Bt709LimitedBarsDecodeToTheirKnownRgb` with independently known Y/Cb/Cr. HDR/10-bit tone mapping is left to VideoToolbox — known limit, unmeasured here. |
| Loop seam | Pre-open in Task 1 (deterministic test) and the measured `[avf-loop-seam]` bound ≤ 67 ms at 30 fps in Task 2. |
| Reader callbacks outliving a retired decoder | Task 1 wake relay test + Task 2 50-iteration destroy-mid-read test. |
| Hardware decode session limit | Task 2 `SixteenDecodersOpenAndEachDeliversAFrame` at 1080p. |
| Slow/removed volume | Task 1 `AStalledReaderNeverReportsEnded…`; the owner's 6 s backstop already covers a decoder that cannot say (`MediaTransports.h:564`, existing tests). |
| Re-gated tests assuming MF details | Only the two video + two audio decoder tests are re-gated; the FFmpeg ones stay MF-only (spec correction 1). |
| Shared-code drift on Windows | Additive changes only; Task 2 Step 5 and Task 7 Step 3 run the Windows checks or mark them UNVERIFIED. |
| Not covered by any task: audio-only assets on air (no video frame → layer slate) | N3, by design; Task 6 decodes WAV at decoder level only. |

## Out of scope

N3 (audio-only assets on air, alpha compositing, rotation), N4 (zero-copy IOSurface, `hasContent()`
gates), N5 (FFmpeg fallback for WebM); `media.rundown.*` remote actions and `volt-rundown` (control spec,
roadmap M2.3/M2.4 — they will call `cueSelectedInPreview()` and `take()`); the Sources tab showing media
as a source type; per-source grade; scrubbing, in/out points, playlists, clip-to-clip transitions;
"Add overlay" listing clips (the canvas picker stays stills-only; Cue is the clip path); Intel Macs and
macOS before 13.
