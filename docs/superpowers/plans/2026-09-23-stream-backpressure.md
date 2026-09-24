# Stream Backpressure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A live stream whose destination cannot sustain the configured bitrate sheds frame rate, recovers its accumulated latency, and stays up, with zero encoder rebuilds.

**Architecture:** A pure policy reads one signal — the wall-clock age of the oldest chunk in the GPU-direct bitstream queue — and drives two levers. Lever A skips program frames before they reach the encoder, which lowers the data rate while holding per-frame quality. Lever B discards the queued chunks ahead of the next keyframe, which recovers latency Lever A cannot. The policy mirrors `core/MonitorShedPolicy.h` in shape: integer-only, enter fast, recover slowly, hysteresis band between.

**Tech Stack:** C++20 core (`native/`), the in-house gtest shim (`native/tests/gtest/gtest.h`), Media Foundation hardware encoders on Windows, FFmpeg N-124549 as muxer, Node for the acceptance gate.

**Spec:** `docs/superpowers/specs/2026-09-23-stream-backpressure-design.md`

## Global Constraints

- **Task 1 gates everything.** If the CBR bits-per-frame assumption fails, Lever A cannot work as designed. Stop and report to the owner rather than substituting dynamic bitrate, which contradicts their "keep quality" ruling.
- The measure is **buffered milliseconds**, defined as `now - oldestQueuedChunkEnqueuedAt`. Never convert a frame count to milliseconds: that conversion is wrong precisely while the frame rate is being changed underneath it.
- Thresholds, exact: `kThrottleAboveBufferedMs = 250`, `kDiscardAboveBufferedMs = 750`, `kRecoverBelowBufferedMs = 100`, `kMaxDivisor = 4`, `kEnterAfterOverWaterTicks = 30`, `kRecoverAfterHealthyTicks = 600`, `kDiscardCooldownTicks = 60`.
- Divisor ladder: 1 = 60 fps, 2 = 30, 3 = 20, 4 = 15.
- **Never discard an arbitrary chunk.** The only safe discard is every chunk ahead of a keyframe already in the queue. With no keyframe queued, discard nothing.
- **A network or destination fault must never rebuild the encoder.** It restarts the connection and muxer and asks the running encoder for a keyframe.
- Both levers apply to the GPU-direct path only. The raw CPU path already drops stale frames and is unchanged.
- Per destination. One struggling destination must not throttle or discard for a healthy sibling.
- The gtest shim's `--gtest_filter` takes ONE positive wildcard. No `:` lists, no `-` negation. Run suites separately.
- Build Release: `cmake --build native\build-dev --config Release --target corevideo-native-tests corevideo-native`. Debug and Release write the same exe path, so a missing `--config Release` silently builds Debug (~8 MB vs ~2.3 MB).
- Never run the native suite while the owner's installed app is streaming or has the virtual camera on.
- Counters saturate at `kCounterCeiling` and never wrap; a snapshot must never see one go down.

---

### Task 1: Measure the CBR bits-per-frame assumption (gates the whole plan)

**Files:**
- Create: `native/tests/StreamBackpressureRateProbeTest.cpp`
- Modify: `native/CMakeLists.txt` (register it beside `tests/MediaFoundationGpuVideoEncoderTest.cpp`, inside the same `COREVIDEO_WITH_MF_ENCODER` guard)

**Interfaces:**
- Consumes: `corevideo::modules::createMediaFoundationGpuVideoEncoder()`, `GpuVideoEncoderConfig`, `GpuEncodedChunkSink` (all in `native/src/modules/GpuVideoEncoder.h` and the MF impl).
- Produces: a measured ratio, printed and asserted. No production code.

Lever A assumes the encoder allocates `bitrate / declaredFrameRate` bits per frame, so halving the input frame rate roughly halves egress while per-frame quality holds. If instead its rate control chases the bitrate on a wall clock, it will spend more bits per frame and egress will not fall.

- [ ] **Step 1: Write the probe test**

```cpp
#include <gtest/gtest.h>
#if defined(_WIN32) && defined(COREVIDEO_WITH_MF_ENCODER) && COREVIDEO_WITH_MF_ENCODER
// Feed the SAME source at 60 submits/s and at 30 submits/s for the same wall
// time, and compare bytes emitted. Lever A of the backpressure design rests on
// this ratio: bits per frame constant => egress falls with the frame rate.
TEST(StreamBackpressureRateProbe, HalvingTheInputRateRoughlyHalvesEgress) {
  const auto bytesFor = [](int submitsPerSecond) -> std::int64_t {
    // Reuse MediaFoundationGpuVideoEncoderTest's compositor + encoder setup
    // verbatim (same plan, same 1920x1080@60 config, 10000 kbps, codec h264);
    // the ONLY difference is how often submit() is called over 10 seconds.
    // Sum chunk.size in the sink.
    return 0;  // replaced by the real harness below
  };
  const auto full = bytesFor(60);
  const auto half = bytesFor(30);
  if (full == 0 || half == 0) {
    std::fprintf(stderr, "[  SKIPPED ] StreamBackpressureRateProbe (no hardware encoder)\n");
    return;
  }
  const double ratio = static_cast<double>(half) / static_cast<double>(full);
  std::fprintf(stderr, "[rate-probe] full=%lld half=%lld ratio=%.3f\n",
               static_cast<long long>(full), static_cast<long long>(half), ratio);
  EXPECT_GT(ratio, 0.35) << "egress collapsed far below half: check submit pacing";
  EXPECT_LT(ratio, 0.75) << "egress did NOT fall with the input rate: the CBR "
                            "bits-per-frame assumption is FALSE and Lever A "
                            "cannot work as designed — STOP and report";
}
#endif
```

Build the real harness by copying the compositor/encoder setup out of `native/tests/MediaFoundationGpuVideoEncoderTest.cpp`'s `runRoundTrip` (same plan, same config, same self-skip shape) and driving `submit()` on a fixed schedule for 10 seconds per leg. Sum `chunk.size` in the sink.

- [ ] **Step 2: Configure, build, run**

```powershell
$vs = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
cmd /c "call `"$vs`" -arch=amd64 >nul 2>&1 && cmake -S native -B native\build-dev >nul && cmake --build native\build-dev --config Release --target corevideo-native-tests 2>&1" | Select-String " error |tests.vcxproj -> "
native\build-dev\corevideo-native-tests.exe --gtest_filter='StreamBackpressureRateProbe.*'
```

- [ ] **Step 3: Act on the result**

Ratio in (0.35, 0.75): the assumption holds. Record the measured number in the report and continue to Task 2.

Ratio at or above 0.75: **STOP.** Report `BLOCKED` with the measured ratio. Lever A does not reduce egress on this encoder and the design needs an owner ruling, because the alternative (dynamic bitrate) contradicts "keep quality". Do not proceed to Task 2.

- [ ] **Step 4: Commit**

```bash
git add native/tests/StreamBackpressureRateProbeTest.cpp native/CMakeLists.txt
git commit -m "Probe: does halving the encoder's input rate halve egress (gates Lever A)"
```

---

### Task 2: The pure policy

**Files:**
- Create: `native/src/core/StreamBackpressurePolicy.h`
- Create: `native/tests/StreamBackpressurePolicyTest.cpp`
- Modify: `native/CMakeLists.txt` (register the test in the UNCONDITIONAL list, right after `tests/MonitorShedPolicyTest.cpp` at line ~643)

**Interfaces:**
- Produces, consumed by Tasks 4, 5 and 6:
```cpp
namespace corevideo::core {
struct StreamBackpressureObservation {
  std::int64_t bufferedMs = -1;   // < 0 = unknown; the tick is ignored
  bool keyframeInQueue = false;   // is a GOP-tail discard possible right now?
};
enum class StreamBackpressureTransition { None, Enter, StepUp, StepDown, Exit };
struct StreamBackpressureDecision {
  StreamBackpressureTransition transition = StreamBackpressureTransition::None;
  bool discardBacklog = false;
};
class StreamBackpressurePolicy {
 public:
  StreamBackpressureDecision observe(const StreamBackpressureObservation&);
  int divisor() const;          // 1..kMaxDivisor
  int level() const;            // divisor - 1
  std::int64_t enteredCount() const;
  std::int64_t shedFrames() const;
  std::int64_t discardEvents() const;
  const char* lastReason() const;             // "none"|"buffered-above-threshold"|"recovered"|"backlog-discard"
  std::int64_t lastTransitionBufferedMs() const;
  void noteShedFrame();                       // called when Lever A skips a frame
  static const char* transitionName(StreamBackpressureTransition);
  static constexpr int kMaxDivisor = 4;
  static constexpr std::int64_t kThrottleAboveBufferedMs = 250;
  static constexpr std::int64_t kDiscardAboveBufferedMs = 750;
  static constexpr std::int64_t kRecoverBelowBufferedMs = 100;
  static constexpr std::int64_t kEnterAfterOverWaterTicks = 30;
  static constexpr std::int64_t kRecoverAfterHealthyTicks = 600;
  static constexpr std::int64_t kDiscardCooldownTicks = 60;
};
}
```

- [ ] **Step 1: Write the failing tests**

```cpp
#include "core/StreamBackpressurePolicy.h"

#include <gtest/gtest.h>

using corevideo::core::StreamBackpressureObservation;
using corevideo::core::StreamBackpressurePolicy;
using corevideo::core::StreamBackpressureTransition;

namespace {
StreamBackpressureObservation at(std::int64_t bufferedMs, bool keyframe = true) {
  StreamBackpressureObservation o;
  o.bufferedMs = bufferedMs;
  o.keyframeInQueue = keyframe;
  return o;
}
// Feed `ticks` observations at `bufferedMs` and return the last decision.
corevideo::core::StreamBackpressureDecision feed(StreamBackpressurePolicy& p, std::int64_t bufferedMs,
                                                 int ticks, bool keyframe = true) {
  corevideo::core::StreamBackpressureDecision d;
  for (int i = 0; i < ticks; ++i) d = p.observe(at(bufferedMs, keyframe));
  return d;
}
}  // namespace

TEST(StreamBackpressurePolicy, StartsUnthrottled) {
  StreamBackpressurePolicy p;
  EXPECT_EQ(p.divisor(), 1);
  EXPECT_EQ(std::string(p.lastReason()), "none");
}

TEST(StreamBackpressurePolicy, AnUnknownMeasurementIsIgnored) {
  StreamBackpressurePolicy p;
  feed(p, -1, 5000);
  EXPECT_EQ(p.divisor(), 1);
}

// A keyframe spikes the queue for a tick or two. Only sustained growth throttles.
TEST(StreamBackpressurePolicy, ASingleSpikeDoesNotThrottle) {
  StreamBackpressurePolicy p;
  feed(p, 900, StreamBackpressurePolicy::kEnterAfterOverWaterTicks - 1);
  EXPECT_EQ(p.divisor(), 1);
  feed(p, 0, 1);  // one healthy tick clears the streak
  feed(p, 900, StreamBackpressurePolicy::kEnterAfterOverWaterTicks - 1);
  EXPECT_EQ(p.divisor(), 1);
}

TEST(StreamBackpressurePolicy, SustainedBacklogStepsDownOneLevelAtATime) {
  StreamBackpressurePolicy p;
  const auto enter = feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  EXPECT_EQ(p.divisor(), 2);
  EXPECT_EQ(enter.transition, StreamBackpressureTransition::Enter);
  EXPECT_EQ(std::string(p.lastReason()), "buffered-above-threshold");
  const auto up = feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  EXPECT_EQ(p.divisor(), 3);
  EXPECT_EQ(up.transition, StreamBackpressureTransition::StepUp);
}

TEST(StreamBackpressurePolicy, NeverExceedsTheFloor) {
  StreamBackpressurePolicy p;
  feed(p, 900, StreamBackpressurePolicy::kEnterAfterOverWaterTicks * 20);
  EXPECT_EQ(p.divisor(), StreamBackpressurePolicy::kMaxDivisor);
}

// The 100-250ms band is the anti-flap hysteresis. Throttling drains the queue,
// so recovering at the throttle threshold would oscillate.
TEST(StreamBackpressurePolicy, TheHysteresisBandHoldsWithoutRecovering) {
  StreamBackpressurePolicy p;
  feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  ASSERT_EQ(p.divisor(), 2);
  feed(p, 200, StreamBackpressurePolicy::kRecoverAfterHealthyTicks * 3);
  EXPECT_EQ(p.divisor(), 2) << "buffered inside the band must neither throttle nor recover";
}

TEST(StreamBackpressurePolicy, RecoversOnlyAfterASustainedHealthyRun) {
  StreamBackpressurePolicy p;
  feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  ASSERT_EQ(p.divisor(), 2);
  feed(p, 10, StreamBackpressurePolicy::kRecoverAfterHealthyTicks - 1);
  EXPECT_EQ(p.divisor(), 2);
  const auto exit = feed(p, 10, 1);
  EXPECT_EQ(p.divisor(), 1);
  EXPECT_EQ(exit.transition, StreamBackpressureTransition::Exit);
  EXPECT_EQ(std::string(p.lastReason()), "recovered");
}

// Lever B: only above the higher threshold, only once the throttle has engaged,
// only when a keyframe is queued to discard up to, and never every tick.
TEST(StreamBackpressurePolicy, DiscardNeedsBacklogThrottleKeyframeAndCooldown) {
  StreamBackpressurePolicy p;
  EXPECT_FALSE(feed(p, 900, 1).discardBacklog) << "not while still at divisor 1";
  feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  ASSERT_EQ(p.divisor(), 2);
  EXPECT_FALSE(p.observe(at(400)).discardBacklog) << "400ms is below the discard threshold";
  EXPECT_FALSE(p.observe(at(900, /*keyframe=*/false)).discardBacklog) << "no keyframe to discard up to";
  const auto fired = p.observe(at(900));
  EXPECT_TRUE(fired.discardBacklog);
  EXPECT_EQ(std::string(p.lastReason()), "backlog-discard");
  EXPECT_EQ(p.discardEvents(), 1);
  EXPECT_FALSE(p.observe(at(900)).discardBacklog) << "cooldown: never two ticks running";
  feed(p, 900, StreamBackpressurePolicy::kDiscardCooldownTicks);
  EXPECT_EQ(p.discardEvents(), 2);
}

TEST(StreamBackpressurePolicy, ShedFramesAndEnteredCountAreCounted) {
  StreamBackpressurePolicy p;
  feed(p, 300, StreamBackpressurePolicy::kEnterAfterOverWaterTicks);
  p.noteShedFrame();
  p.noteShedFrame();
  EXPECT_EQ(p.shedFrames(), 2);
  EXPECT_EQ(p.enteredCount(), 1);
}
```

Register the test file in `native/CMakeLists.txt` immediately after `tests/MonitorShedPolicyTest.cpp`.

- [ ] **Step 2: Configure, build, confirm RED**

```powershell
cmd /c "call `"$vs`" -arch=amd64 >nul 2>&1 && cmake -S native -B native\build-dev >nul && cmake --build native\build-dev --config Release --target corevideo-native-tests 2>&1" | Select-String " error "
```
Expected: `Cannot open include file: 'core/StreamBackpressurePolicy.h'`.

- [ ] **Step 3: Write the policy**

Create `native/src/core/StreamBackpressurePolicy.h` with a header comment that states: the incident (#597), the signal (age of the oldest queued chunk, never a frame-count conversion), the two levers and why Lever B exists (Lever A stops growth but never clears what is already queued), why the discard is a GOP tail (our B-frames-off configuration leaves two priority tiers), and that the shape deliberately mirrors `MonitorShedPolicy`.

```cpp
StreamBackpressureDecision observe(const StreamBackpressureObservation& o) {
  StreamBackpressureDecision decision;
  if (o.bufferedMs < 0) return decision;           // no evidence either way
  if (discardCooldown_ > 0) --discardCooldown_;

  // Lever B is independent of the divisor ladder, but never precedes it: the
  // throttle is invisible, a discard is a visible skip.
  if (o.bufferedMs >= kDiscardAboveBufferedMs && divisor_ > 1 && o.keyframeInQueue &&
      discardCooldown_ == 0) {
    decision.discardBacklog = true;
    discardCooldown_ = kDiscardCooldownTicks;
    if (discardEvents_ < kCounterCeiling) ++discardEvents_;
    lastReason_ = "backlog-discard";
    lastTransitionBufferedMs_ = o.bufferedMs;
  }

  if (o.bufferedMs >= kThrottleAboveBufferedMs) {
    healthyStreak_ = 0;
    if (overStreak_ < kCounterCeiling) ++overStreak_;
    if (overStreak_ >= kEnterAfterOverWaterTicks && divisor_ < kMaxDivisor) {
      overStreak_ = 0;
      ++divisor_;
      lastReason_ = "buffered-above-threshold";
      lastTransitionBufferedMs_ = o.bufferedMs;
      if (divisor_ == 2) {
        if (enteredCount_ < kCounterCeiling) ++enteredCount_;
        decision.transition = StreamBackpressureTransition::Enter;
      } else {
        decision.transition = StreamBackpressureTransition::StepUp;
      }
    }
    return decision;
  }
  overStreak_ = 0;
  if (divisor_ == 1) return decision;
  if (o.bufferedMs > kRecoverBelowBufferedMs) {
    // Inside the hysteresis band: the shed is working. Hold.
    healthyStreak_ = 0;
    return decision;
  }
  if (++healthyStreak_ < kRecoverAfterHealthyTicks) return decision;
  healthyStreak_ = 0;
  --divisor_;
  lastReason_ = "recovered";
  lastTransitionBufferedMs_ = o.bufferedMs;
  decision.transition = divisor_ == 1 ? StreamBackpressureTransition::Exit
                                      : StreamBackpressureTransition::StepDown;
  return decision;
}
```

State: `int divisor_ = 1;` plus `overStreak_`, `healthyStreak_`, `discardCooldown_`, `enteredCount_`, `shedFrames_`, `discardEvents_`, `lastReason_ = "none"`, `lastTransitionBufferedMs_ = 0`, and `static constexpr std::int64_t kCounterCeiling = INT64_C(1) << 62;` (copy the saturating-counter discipline from `MonitorShedPolicy`).

- [ ] **Step 4: Build and run GREEN**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='StreamBackpressurePolicy.*'
```
Expected: 9 passed, 0 failed.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/StreamBackpressurePolicy.h native/tests/StreamBackpressurePolicyTest.cpp native/CMakeLists.txt
git commit -m "StreamBackpressurePolicy: buffered-ms signal, input divisor, GOP-tail discard"
```

---

### Task 3: Publish the queue's buffered latency

**Files:**
- Modify: `native/src/modules/RtmpOutputSenderAdapter.cpp` — `QueuedBitstream` (line ~2383), members (~2387-2392), `enqueueBitstream` (~1783), `bitstreamWriterLoop` (~1801)

**Interfaces:**
- Produces, consumed by Tasks 4 and 5: private members
  `std::atomic<std::int64_t> bitstreamHeadEnqueuedNs_{0}` (0 = queue empty),
  `std::atomic<std::int64_t> bitstreamQueuedChunks_{0}`,
  `std::atomic<bool> bitstreamQueueHasKeyframe_{false}`,
  and a helper `std::int64_t bitstreamBufferedMs() const`.

The submit path must read buffered latency without taking `bitstreamQueueMutex_`: the submit runs on the sender's sync path while the writer thread services the queue. Publishing the HEAD'S ENQUEUE TIME rather than a precomputed age is what makes the read correct — the age keeps growing while the queue is untouched.

- [ ] **Step 1: Add the enqueue timestamp to the queued chunk**

```cpp
  struct QueuedBitstream {
    std::vector<uint8_t> bytes;
    GpuEncodedChunk metadata;
    // Wall-clock moment this chunk entered the queue. The backpressure signal is
    // the AGE of the head of the queue, which is the only measure that stays
    // honest while the frame rate is being changed underneath it.
    std::chrono::steady_clock::time_point enqueuedAt{};
  };
```

- [ ] **Step 2: Add the atomics and the republish helper**

Next to the existing queue members:

```cpp
  // Read by the submit path with NO lock (see bitstreamBufferedMs). Written only
  // under bitstreamQueueMutex_, where the queue is already being mutated.
  std::atomic<std::int64_t> bitstreamHeadEnqueuedNs_{0};  // 0 = empty
  std::atomic<std::int64_t> bitstreamQueuedChunks_{0};
  std::atomic<bool> bitstreamQueueHasKeyframe_{false};
```

```cpp
  // Caller must hold bitstreamQueueMutex_.
  void republishQueueTelemetryLocked() {
    bitstreamQueuedChunks_.store(static_cast<std::int64_t>(bitstreamQueue_.size()),
                                 std::memory_order_relaxed);
    bitstreamHeadEnqueuedNs_.store(
        bitstreamQueue_.empty()
            ? 0
            : bitstreamQueue_.front().enqueuedAt.time_since_epoch().count(),
        std::memory_order_relaxed);
    bool keyframe = false;
    for (const auto& q : bitstreamQueue_) {
      if (q.metadata.keyframe) { keyframe = true; break; }
    }
    bitstreamQueueHasKeyframe_.store(keyframe, std::memory_order_relaxed);
  }

  // 0 when the queue is empty. Lock-free: reads the head's enqueue time and ages
  // it against now, so the number keeps rising while the writer is blocked.
  [[nodiscard]] std::int64_t bitstreamBufferedMs() const {
    const auto head = bitstreamHeadEnqueuedNs_.load(std::memory_order_relaxed);
    if (head == 0) return 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto ageNs = now - head;
    return ageNs <= 0 ? 0 : static_cast<std::int64_t>(ageNs / 1'000'000);
  }
```

- [ ] **Step 3: Stamp on enqueue and republish on both sides**

In `enqueueBitstream`, replace the push with:
```cpp
      bitstreamQueue_.push_back({std::vector<uint8_t>(chunk.data, chunk.data + chunk.size), chunk,
                                 std::chrono::steady_clock::now()});
      bitstreamQueuedBytes_ += chunk.size;
      republishQueueTelemetryLocked();
```
In `bitstreamWriterLoop`, immediately after `bitstreamQueuedBytes_ -= packet.bytes.size();` (still under the lock) add `republishQueueTelemetryLocked();`.

Wherever the queue is cleared on stop/failure, call `republishQueueTelemetryLocked()` too, so a stopped sender does not leave a stale non-zero head.

Add `#include <atomic>` and `#include <chrono>` if absent.

- [ ] **Step 4: Build and run the full suite (no behavior change yet)**

```powershell
cmd /c "call `"$vs`" -arch=amd64 >nul 2>&1 && cmake --build native\build-dev --config Release --target corevideo-native-tests corevideo-native 2>&1" | Select-String " error "
native\build-dev\corevideo-native-tests.exe
```
Expected: the suite's current total, 0 failed. This task adds telemetry only.

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/RtmpOutputSenderAdapter.cpp
git commit -m "Publish the bitstream queue's buffered latency, lock-free for the submit path"
```

---

### Task 4: Lever A — the input divisor, applied at the encoder-texture export

> **AMENDED after Task 1's measurement (controller ruling).** The plan originally
> gated `submitFrameToGpuEncoder`. Task 1 PROVED that does nothing: skipping only
> `submit()` produced a byte-identical stream (ratio 0.998). The encoder's thread
> advances on the KEYED MUTEX, when the compositor releases a new frame — so the
> throttle must stop the compositor exporting to the encoder texture. The divisor
> now travels sender -> MediaCore -> compositor.

**Files:**
- Modify: `native/src/modules/Interfaces.h` — minimal `OutputBackpressureState{divisor}` on `OutputSender`; `ICompositor::setEncoderExportDivisor(int)`
- Modify: `native/src/modules/RtmpOutputSenderAdapter.cpp` — observe the policy, publish the divisor
- Modify: `native/src/modules/D3D11CompositorAdapter.cpp` — gate the export at line ~228
- Modify: `native/src/core/MediaCore.cpp` — read the senders' divisor, drive the compositor
- Modify: `native/tests/MediaCoreCommandTest.cpp` — pin the decision where it is applied

**Interfaces:**
- Consumes: Task 2's policy, Task 3's `bitstreamBufferedMs()` / `bitstreamQueueHasKeyframe_`.
- Produces: private `corevideo::core::StreamBackpressurePolicy backpressure_;` on the sender and `OutputSender::backpressure` (`OutputBackpressureState{divisor}`), consumed by Tasks 5 and 6; `ICompositor::setEncoderExportDivisor(int)`.

- [ ] **Step 1: Write the failing tests**

Pin the decision where it is APPLIED, not only in the leaf policy (the #481
rule). Two assertions, both GPU-free:
1. the compositor honours the divisor - at divisor 2 it exports on every second
   frame number and not the others;
2. a backed-up sender publishes a divisor above 1, and MediaCore passes the max
   across senders to the compositor.

Deleting either the export gate or the MediaCore plumbing must fail a test. In
`native/tests/MediaCoreCommandTest.cpp`, beside the existing tests that drive
`createRtmpOutputSender()->sync()`:

```cpp
// #597: the divisor must gate the EXPORT. Task 1 proved gating submit() does
// nothing (ratio 0.998) - the keyed mutex paces the encoder. Deleting the export
// gate must fail this test.
TEST(RtmpOutputSenderBackpressure, TheDivisorGatesTheEncoderTextureExport) {
  // Call setEncoderExportDivisor(2) on the compositor, drive 200 render ticks
  // with fullProgramReadback true, and count the exports. Expect every even
  // frameNumber exported and no odd one - an exact count, not a ratio, because
  // this leg has no timing in it at all.
}

// #597: a backed-up sender must PUBLISH a divisor, and MediaCore must carry the
// max across senders to the compositor. Deleting either half fails this.
TEST(RtmpOutputSenderBackpressure, TheSendersDivisorReachesTheCompositor) {
  // Drive the sender with a program frame carrying an encoder texture, holding
  // the bitstream queue backed up past kThrottleAboveBufferedMs for more than
  // kEnterAfterOverWaterTicks frames, then assert sender.session().backpressure
  // reports divisor > 1. With a second, healthy GPU-direct sender present,
  // assert the compositor stub recorded the MAX of the two, set once on the
  // transition rather than every tick.
}
```

Build the harness from the existing sender tests: install a counting `GpuVideoEncoder` through the sender's encoder factory seam, and force the buffered measure by seeding the queue (or by injecting the policy's observation through a test-only setter if seeding proves impractical — say which you used and why in the report).

- [ ] **Step 2: Build and confirm RED**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='RtmpOutputSenderBackpressure.*'
```
Expected: FAIL — every frame is exported to the encoder because no gate exists.

- [ ] **Step 3: Implement the gate (three edits, in this order)**

**(a) The compositor stops exporting on shed frames.** `ICompositor` gains
`virtual void setEncoderExportDivisor(int) {}` (a default no-op, so Metal and the
stub are unaffected). In `D3D11CompositorAdapter`, store it and gate the existing
call at line ~228:

```cpp
    // #597 Lever A. The encoder's thread advances on the KEYED MUTEX - when this
    // export releases a new frame - NOT on the sender's submit(). Task 1 measured
    // it directly: skipping only submit() left the stream byte-identical (ratio
    // 0.998), while halving the export rate halved egress (0.500). So the throttle
    // lives here. Skipping the export leaves the encoder waiting, which is exactly
    // the intended "fewer frames, same quality each".
    if (renderPlan.fullProgramReadback &&
        (encoderExportDivisor_ <= 1 || (frame.frameNumber % encoderExportDivisor_) == 0)) {
      exportEncoderSharedTexture(frame);
    }
```

**(b) The sender observes the policy and publishes its divisor.** Add
`#include "core/StreamBackpressurePolicy.h"` and a member
`corevideo::core::StreamBackpressurePolicy backpressure_;`. Observe once per
`sync()` on the GPU-direct path, where the sender already holds the frame:

```cpp
    // #597 Lever A: skip program frames BEFORE the encoder. Compressed frames
    // cannot be dropped individually, so the only safe throttle is upstream. The
    // encoder's declared frame rate is unchanged, so bits-per-frame — and with
    // it per-frame quality — holds while the data rate falls.
    corevideo::core::StreamBackpressureObservation observation;
    observation.bufferedMs = bitstreamBufferedMs();
    observation.keyframeInQueue = bitstreamQueueHasKeyframe_.load(std::memory_order_relaxed);
    const auto decision = backpressure_.observe(observation);
    if (decision.transition != corevideo::core::StreamBackpressureTransition::None) {
      ::corevideo::core::nativeLogf(
          "[stream-backpressure] %s divisor=%d buffered=%lldms\n",
          corevideo::core::StreamBackpressurePolicy::transitionName(decision.transition),
          backpressure_.divisor(), static_cast<long long>(observation.bufferedMs));
    }
    // Task 5 handles decision.discardBacklog here.
```

Publish the divisor so MediaCore can read it. In `Interfaces.h`, beside
`OutputSupervisorState`:

```cpp
struct OutputBackpressureState {
  int divisor = 1;  // Task 6 adds the remaining published fields
};
```
and `std::optional<OutputBackpressureState> backpressure;` on `OutputSender`,
set by the sender whenever `useGpuDirect_` is true.

**(c) MediaCore drives the compositor.** Where it already walks the sender
snapshots each output tick, take the MAX divisor across active GPU-direct
senders and call `compositor->setEncoderExportDivisor(n)` only when it CHANGES
— a control-plane call on a transition, never per frame.

MAX, not min, and say so in the report: one encoder texture feeds every
destination, so the spec's per-destination rule cannot fully hold while a
single encoder serves them all. A healthy sibling is throttled by a struggling
one. That is a real limitation of this slice, not an oversight — name it rather
than let a reviewer discover it.

- [ ] **Step 4: Build and run GREEN**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='RtmpOutputSenderBackpressure.*'
native\build-dev\corevideo-native-tests.exe
```
Expected: the new test passes; full suite 0 failed.

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/Interfaces.h native/src/modules/RtmpOutputSenderAdapter.cpp \n  native/src/modules/D3D11CompositorAdapter.cpp native/src/core/MediaCore.cpp \n  native/tests/MediaCoreCommandTest.cpp
git commit -m "Lever A: throttle by holding the encoder texture export, not the submit"
```

---

### Task 5: Lever B — discard the backlog to the next keyframe

**Files:**
- Modify: `native/src/modules/RtmpOutputSenderAdapter.cpp` — new `discardBacklogToNextKeyframe()`, called from `submitFrameToGpuEncoder`
- Modify: `native/tests/MediaCoreCommandTest.cpp` — discard-correctness tests

**Interfaces:**
- Consumes: Task 2's `decision.discardBacklog`, Task 3's queue telemetry.
- Produces: `std::size_t discardBacklogToNextKeyframe()` returning how many chunks it dropped.

- [ ] **Step 1: Write the failing tests**

```cpp
// A queue with no keyframe must discard NOTHING: dropping an arbitrary
// reference frame corrupts every frame after it until the next keyframe.
TEST(RtmpOutputSenderBackpressure, DiscardWithNoQueuedKeyframeDropsNothing) { /* ... */ }

// With a keyframe queued, discard drops exactly the chunks ahead of it and
// never the keyframe itself, so the stream resumes cleanly at that point.
TEST(RtmpOutputSenderBackpressure, DiscardDropsTheGopTailAndKeepsTheKeyframe) { /* ... */ }

// The head's enqueue time must be republished after a discard, or buffered
// latency would keep reporting the age of a chunk that is gone.
TEST(RtmpOutputSenderBackpressure, DiscardRepublishesTheBufferedMeasure) { /* ... */ }
```

Fill these in against the queue directly through a test seam on the sender (the queue is private; add a narrow test-only accessor following whatever seam this file already uses, and say which in the report).

- [ ] **Step 2: Build and confirm RED**

- [ ] **Step 3: Implement**

```cpp
  // #597 Lever B. Lever A stops the queue growing; it never clears what is
  // already in it, so a stream can stabilise a full second behind and stay
  // there. Discarding every chunk AHEAD of the next queued keyframe recovers
  // that latency as a clean skip. Dropping an arbitrary chunk instead would
  // corrupt every frame until the next keyframe. Our HEVC/AV1 encoders run with
  // B-frames disabled (the low-latency work), so there are no non-reference
  // frames to drop cheaply and the GOP tail is the only safe unit.
  std::size_t discardBacklogToNextKeyframe() {
    std::lock_guard<std::mutex> lock(bitstreamQueueMutex_);
    std::size_t keyframeIndex = 0;
    bool found = false;
    for (std::size_t i = 0; i < bitstreamQueue_.size(); ++i) {
      if (bitstreamQueue_[i].metadata.keyframe) { keyframeIndex = i; found = true; break; }
    }
    if (!found || keyframeIndex == 0) return 0;  // nothing ahead of a keyframe to drop
    std::size_t dropped = 0;
    for (std::size_t i = 0; i < keyframeIndex; ++i) {
      bitstreamQueuedBytes_ -= bitstreamQueue_.front().bytes.size();
      bitstreamQueue_.pop_front();
      ++dropped;
    }
    republishQueueTelemetryLocked();
    return dropped;
  }
```

In `submitFrameToGpuEncoder`, where Task 4 left the placeholder:
```cpp
    if (decision.discardBacklog) {
      const auto dropped = discardBacklogToNextKeyframe();
      if (dropped > 0) {
        backpressureDiscardedChunks_ += static_cast<std::int64_t>(dropped);
        ::corevideo::core::nativeLogf(
            "[stream-backpressure] discard dropped=%zu divisor=%d buffered=%lldms\n",
            dropped, backpressure_.divisor(), static_cast<long long>(observation.bufferedMs));
      }
    }
```
Add `std::int64_t backpressureDiscardedChunks_ = 0;`.

- [ ] **Step 4: Build and run GREEN**

```powershell
native\build-dev\corevideo-native-tests.exe --gtest_filter='RtmpOutputSenderBackpressure.*'
native\build-dev\corevideo-native-tests.exe
```

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/Interfaces.h native/src/modules/RtmpOutputSenderAdapter.cpp \n  native/src/modules/D3D11CompositorAdapter.cpp native/src/core/MediaCore.cpp \n  native/tests/MediaCoreCommandTest.cpp
git commit -m "Lever B: recover latency by discarding the GOP tail ahead of a queued keyframe"
```

---

### Task 6: The snapshot node

> **AMENDED after Task 4 (controller ruling).** `shedFrames` was specified as a
> PER-SENDER field, which became a category error the moment Lever A's gate moved
> into the compositor: ONE encoder texture serves every GPU-direct destination, so
> frames are shed once, globally — not once per sender. Publishing the same global
> count on each sender would read as each having shed its own, and publishing 0
> (what `StreamBackpressurePolicy::shedFrames()` returns, since nothing on the
> sender side sheds anything any more) would be worse. So: `shedFrames` LEAVES the
> per-sender node and the real count is published ONCE, from the compositor, beside
> the effective divisor. The per-sender node keeps only what is genuinely
> per-destination — the divisor THIS sender asked for, its buffered age, its queue
> depth, and its own discards.
>
> `StreamBackpressurePolicy::noteShedFrame()` / `shedFrames()` are now dead by
> construction (the compositor sheds, and it holds no reference to any sender's
> policy). Delete both, and their `shedFrames_` member, in this task — do not leave
> a public method nothing can ever call.

**Files:**
- Modify: `native/src/modules/Interfaces.h` — a `backpressure` optional on `OutputSender` beside `supervisor` (line ~679)
- Modify: `native/src/modules/RtmpOutputSenderAdapter.cpp` — populate it in `snapshot()`
- Modify: `native/src/core/MediaCore.cpp` — emit it beside the `supervisor` node (line ~5281)

**Interfaces:**
- Produces: `sessionState().outputSenderSession.senders[].backpressure` = `{divisor, level, bufferedMs, queuedChunks, enteredCount, discardedChunks, discardEvents, lastReason, lastTransitionBufferedMs}` — the per-destination facts.
- Produces: `sessionState().realtimeEvidence.encoderExport` = `{divisor, shedFrames}` — the ONE effective divisor MediaCore pushed to the compositor (the MAX across GPU-direct senders) and the frames the compositor actually held back. Published unconditionally, like the multiviewer node: a divisor of 1 and 0 shed frames is the healthy reading, not an absent node.

- [ ] **Step 1: Add the struct**

In `Interfaces.h`, next to `OutputSupervisorState`:
```cpp
// #597: the backpressure policy's view of this destination. Published
// UNCONDITIONALLY for a GPU-direct sender (the multiviewer-node rule: a node
// that vanishes in the case worth detecting is the mistake). A stream quietly
// running at 15 fps is the same class of defect as a silent codec downgrade.
struct OutputBackpressureState {
  int divisor = 1;
  int level = 0;
  std::int64_t bufferedMs = 0;
  std::int64_t queuedChunks = 0;
  std::int64_t enteredCount = 0;
  // NO shedFrames here: the compositor sheds once for every destination, so a
  // per-sender count would claim this destination shed them on its own. The real
  // number is published once, at realtimeEvidence.encoderExport.shedFrames.
  std::int64_t discardedChunks = 0;
  std::int64_t discardEvents = 0;
  std::string lastReason = "none";
  std::int64_t lastTransitionBufferedMs = 0;
};
```
and `std::optional<OutputBackpressureState> backpressure;` on `OutputSender`.

- [ ] **Step 2: Populate it in the sender's `snapshot()`**

Set it whenever `useGpuDirect_` is true, from `backpressure_`, `bitstreamBufferedMs()`, `bitstreamQueuedChunks_` and `backpressureDiscardedChunks_`.

- [ ] **Step 3: Emit it in MediaCore**

Directly after the `supervisor` block:
```cpp
    if (sender.backpressure) {
      const auto& bp = *sender.backpressure;
      senderJson.emplace("backpressure", rpc::Json::Object{
          {"divisor", bp.divisor},
          {"level", bp.level},
          {"bufferedMs", static_cast<double>(bp.bufferedMs)},
          {"queuedChunks", static_cast<double>(bp.queuedChunks)},
          {"enteredCount", static_cast<double>(bp.enteredCount)},
          {"discardedChunks", static_cast<double>(bp.discardedChunks)},
          {"discardEvents", static_cast<double>(bp.discardEvents)},
          {"lastReason", bp.lastReason},
          {"lastTransitionBufferedMs", static_cast<double>(bp.lastTransitionBufferedMs)},
      });
    }
```

- [ ] **Step 3b: Publish the compositor's real shed count**

`D3D11CompositorAdapter` already skips the export; have it COUNT what it skipped
in a saturating `std::int64_t encoderExportShedFrames_` and expose it, with the
applied divisor, through `ICompositor` (default implementations returning 1 and 0,
so Metal and the stub are unaffected). Emit both in `realtimeEvidence` as
`encoderExport {divisor, shedFrames}`, unconditionally.

Then DELETE `noteShedFrame()`, `shedFrames()` and `shedFrames_` from
`StreamBackpressurePolicy`, and the assertion on `shedFrames()` in
`StreamBackpressurePolicyTest.cpp`'s `ShedFramesAndEnteredCountAreCounted`
(rename it to `EnteredCountIsCounted`). Nothing can call them: the object that
sheds is the compositor, which holds no reference to a sender's policy.

- [ ] **Step 3b-bis: Reset the discard counter with the policy**

Task 5 leaves `backpressureDiscardedChunks_` write-only. Publishing it here is
half the job: it must ALSO be reset on the `!wantsRtmp` stop path, right beside
the `backpressure_` reset Task 4 added. Backpressure state is per-stream-run,
so a counter carried across runs would report the previous show's discards as
this one's — the same reasoning that made resetting the policy correct.

- [ ] **Step 3c: Two residuals carried from Task 4's re-review**

Both are one-liners in files this task already opens. Neither is on air today;
both are traps for the next person.

1. **Reset `lastSubmittedEncoderFrameNumber_` when `encoderExport_` is
   recreated** (a dimension change). Without it, a shed frame after a
   recreation publishes a frame number that was submitted to the PREVIOUS
   exporter. There are no production readers of that field today, which is
   exactly why this must be fixed now rather than when one is added.
2. **Note the stop-path residual at `applyEncoderExportDivisor`.**
   `AsyncOutputSender::sync` returns a CACHED pre-stop snapshot, so the final
   "one tick past the last destination" sync can feed a stale `live` record and
   leave the divisor set with nothing streaming. It is inert —
   `fullProgramReadback` is false in that state, so nothing is shed, and the
   next stream's first sync pushes 1 — but the worst case is one shed frame at
   the next stream's start. A comment naming it, not a guard; do not add a
   second reset path that could fight the first.

- [ ] **Step 4: Build, run the full suite**

- [ ] **Step 5: Commit**

```bash
git add native/src/modules/Interfaces.h native/src/modules/RtmpOutputSenderAdapter.cpp native/src/core/MediaCore.cpp
git commit -m "Publish the backpressure node so a degraded stream is visible, not inferred"
```

---

### Task 7: The restart floor

**Files:**
- Modify: `native/src/modules/OutputDestinationSupervisor.cpp` and/or `OutputDestinationSupervisorPolicy.h`
- Modify: `native/tests/OutputDestinationSupervisorTest.cpp`

**Interfaces:**
- Produces: no two encoder rebuilds for one destination closer together than the ladder's current rung.

The incident's restarts were 2.5 to 3.5 seconds apart while the ladder's first rung is 5 seconds (`kHealthyRunMs` is 30 s, so a healthy-run budget return cannot explain it). The ladder was demonstrably not being applied.

- [ ] **Step 1: Find out why, and write it down before changing anything**

Read the path from a sender failure to `PendingAction{Restart}` and identify what resets the per-destination policy state. The first candidate to check is the destination record and its `policy` being reconstructed — and therefore `reset()` — as the sender list is re-synced each tick, which would restart the ladder at rung one every time. Record the finding in the report with file:line before writing a fix; if the cause is something else, the fix follows the real cause, not this guess.

- [ ] **Step 2: Write the failing test**

```cpp
// #597: eight rebuilds in twenty seconds, 2.5-3.5s apart, against a ladder whose
// first rung is 5s. Whatever resets the ladder, the destination must never be
// rebuilt faster than its current rung.
TEST(OutputDestinationSupervisor, RestartsAreNeverCloserThanTheCurrentLadderRung) {
  // Drive a destination through repeated failures with the sender list re-synced
  // between each, as the live path does, and assert the interval between
  // successive Restart actions is monotonically non-decreasing and never below
  // the first rung.
}
```

- [ ] **Step 3: Confirm RED, fix per the Step 1 finding, confirm GREEN**

- [ ] **Step 4: Full suite, then commit**

```bash
git add native/src/modules/OutputDestinationSupervisor.cpp native/src/modules/OutputDestinationSupervisorPolicy.h native/tests/OutputDestinationSupervisorTest.cpp
git commit -m "Restart floor: a destination is never rebuilt faster than its ladder rung (#597)"
```

---

### Task 8: The acceptance gate — a deliberately slow sink

**Files:**
- Modify: `scripts/validate-gpu-encode.mjs`

**Interfaces:**
- Produces: `node scripts/validate-gpu-encode.mjs --slow-sink [--sink-rate 0.85] [--seconds 240]`.

- [ ] **Step 1: Add the slow sink**

The listener at line ~106-109 currently reads as fast as it can. Add `--slow-sink`, which sets the reader's input to a fraction of real time so the destination cannot keep up. Try FFmpeg's `-readrate <speed>` on the listener input first (this build is N-124549 and should have it); verify it actually throttles by watching the queue grow, and if it does not, fall back to piping the listener's output through a rate-limited consumer. Say in the report which mechanism you used and how you verified it throttles.

- [ ] **Step 2: Assert the whole property**

Over `--seconds 240` with `--slow-sink`, assert all of:
- the stream never stops (the sender never reaches `failed`),
- **zero encoder rebuilds** — no second `[gpu-encode] started` line for the run,
- the divisor steps down and later recovers (read the backpressure node from `/snapshot`, or the `[stream-backpressure]` lines),
- buffered latency returns below `kRecoverBelowBufferedMs` rather than sitting at the bound,
- Program holds 60 fps,
- the shell-side sample cadence stays in its normal band — see Step 3.

- [ ] **Step 3: Assert the #597 signature specifically**

The incident's fingerprint was a 21 s gap in `perf.log` whose SAMPLE COUNTER advanced normally. Assert the core's snapshot emission cadence stays within its normal band for the whole run, measured against the counter rather than wall time alone, so a producer slowdown is distinguishable from a UI freeze. This is the assertion that would have caught #597.

- [ ] **Step 3b: Settle the parameter-set question — BLOCKING for Lever B**

Task 5's review raised a stream-corruption risk it could not settle statically,
and this is the only place that can. Lever B discards every chunk ahead of the
first `CleanPoint` chunk, which is safe ONLY if each `CleanPoint` sample is a
self-contained IDR carrying its own VPS/SPS/PPS in band. Nothing in the tree
configures sequence-header repetition or writes a header prologue. If the MFT
ever emits the parameter sets as a SEPARATE non-`CleanPoint` sample just before
the IDR, the discard eats them and the stream is corrupt until the next
keyframe — and it would only ever happen under congestion, which is the worst
possible repro profile.

Log the first ~30 chunks of a real GPU-direct stream (size, `keyframe` flag, and
the first few bytes' NAL types) for h264 and for hevc. Put the evidence in the
report either way.

If the parameter sets are NOT in band with the IDR, the fix is NOT to complicate
the discard: make the IDR self-contained by construction in
`MediaFoundationGpuVideoEncoder` (`CODECAPI_AVEncVideoPrependSPSPPSToIDR`, or
the sequence-header attribute on the output type), then re-run this check. A
discard that has to reason about which preceding chunks are headers is a discard
that will get it wrong under load.

- [ ] **Step 4: Run it, and re-run the healthy gates**

```bash
node scripts/validate-gpu-encode.mjs --seconds 240 --slow-sink
node scripts/validate-gpu-encode.mjs --seconds 30 --codec h264
node scripts/validate-gpu-encode.mjs --seconds 30 --codec hevc
```
Paste all three summary blocks into the report. The last two must pass unchanged against a healthy sink.

- [ ] **Step 5: Commit**

```bash
git add scripts/validate-gpu-encode.mjs
git commit -m "Gate: a deliberately slow sink must degrade the stream, never rebuild the encoder"
```

---

### Task 8b: Close the storm gap the gate found

> **ADDED after Task 8 measured it (controller ruling).** The gate ran the
> congested case three times. Two runs held at exactly one encoder start; the
> third stormed with eleven. The gate did its job — this is the negative result
> the whole task existed to produce, and it must be closed before Lever A and
> Lever B can be described as working.

**The cause chain the gate measured**, in order:
1. The outgoing queue crossed 22 to 60 chunks — its hard cap — inside one second.
2. `enqueueBitstream`'s overflow path **fails the sender on purpose** so the
   supervisor restarts it (`RtmpOutputSenderAdapter.cpp`, the comment reads "An
   overrun fails the sender so its supervisor can restart it").
3. The supervisor restarted it, and the encoder was rebuilt.

**So backpressure's own last-resort bound calls the restart storm.** That
predates this plan and is exactly what the spec forbids: a destination fault
must never rebuild the encoder. Three things lose the race that gets us there —
Lever B is gated on `divisor > 1` so it cannot fire during the ~0.5 s Lever A
needs to step; the 750 ms discard threshold sits only ~15 chunks below the
60-chunk cap; and Lever B's yield is phase-dependent (measured 2 to 51 chunks
freed) because the GOP is about the size of the whole queue.

**Files:**
- Modify: `native/src/modules/RtmpOutputSenderAdapter.cpp` — the overflow path
- Modify: `native/tests/MediaCoreCommandTest.cpp`

- [ ] **Step 1: Write the failing test**

A full queue whose contents include a keyframe must NOT fail the sender. Drive
the real queue to its cap through the existing enqueue seam with a keyframe
somewhere in it, and assert: the sender is still healthy, the queue shrank, and
no overflow failure was recorded. Then the case with NO keyframe queued: there
the sender must still fail, because there is nothing safe to drop and a bounded
queue is not optional.

- [ ] **Step 2: Confirm RED, then implement**

On overflow, run the GOP-tail discard FIRST and accept the incoming chunk if it
freed room. Fail only when the discard frees nothing — i.e. no keyframe is
queued, which is the one case where dropping anything would corrupt the stream
until the next keyframe.

This is Lever B doing exactly the job it was built for, at the one moment it
matters most, and it removes the `divisor > 1` race by construction: the
overflow path does not consult the divisor at all. Do NOT raise the 60-chunk cap
to buy headroom — an unbounded queue is unbounded latency, which is the defect
this whole sub-project exists to remove.

- [ ] **Step 3: Re-run the gate**

`node scripts/validate-gpu-encode.mjs --seconds 240 --slow-sink`, three times.
Report every run. One storming run out of three is a FAILURE, not noise — that
is the state this task exists to remove, and reporting two greens out of three
as a pass would be the same mistake as calling a flaky test green.

- [ ] **Step 4: Full suite, then commit**

```bash
git add native/src/modules/RtmpOutputSenderAdapter.cpp native/tests/MediaCoreCommandTest.cpp
git commit -m "Queue overflow discards the GOP tail instead of failing the sender (#597)"
```

---

### Task 9: Documentation

**Files:**
- Modify: `CLAUDE.md` (the GPU-direct streaming section), `docs/BACKLOG.md`
- Modify: `docs/superpowers/specs/2026-09-23-stream-backpressure-design.md` (an Outcome section)

- [ ] **Step 1: CLAUDE.md**

Add a bullet to the GPU-direct section covering: the #597 incident in two sentences with the measured numbers; the signal (age of the oldest queued chunk, and why not a frame-count conversion); the two levers and why both are needed; why the discard is a GOP tail (our own B-frames-off configuration); the thresholds with their justification against the ~1 s queue; that a network fault must never rebuild the encoder; the gate command; and the Task 1 rate-probe result. Include the diagnostic technique: **compare `perf.log` gaps against the sample COUNTER, not wall time — a normal counter delta with 0.0 ms dispatch is a producer slowdown, a frozen counter is a UI block.**

- [ ] **Step 2: Spec Outcome section**

Record what the Task 1 probe measured, what the Task 7 investigation found, and any threshold the gate forced you to re-tune with the number and the reason.

- [ ] **Step 3: Backlog row**

Amend the #597 row: slice 1 shipped, what it covers, and that the egress-based health signal and the phantom-fault fix remain as slice 2.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md docs/BACKLOG.md docs/superpowers/specs/2026-09-23-stream-backpressure-design.md
git commit -m "Document the backpressure design, its measured numbers, and the #597 diagnostic technique"
```

---

## Self-review

- **Spec coverage:** §1 signal → Task 3; §2 Lever A → Tasks 2, 4; §3 Lever B → Tasks 2, 5; §4 thresholds → Task 2; §5 encoder/connection separation → Task 7 (floor) and the Task 8 zero-rebuild assertion; §6 restart floor → Task 7; §7 observability → Task 6; §8 scope → Tasks 4, 5 (GPU-direct only, per destination); Testing → Tasks 2, 4, 5, 8; Risks: the CBR assumption → Task 1 (gates the plan), the understated measure → recorded in Task 9, keyframe cadence → Task 9 Step 2, one-rig calibration → Task 8 re-tuning.
- **Placeholder scan:** Tasks 4, 5 and 7 carry test bodies described rather than fully written, because each needs a seam into private sender or supervisor state whose shape must be read from the current file first. Each says explicitly what to assert, what the RED looks like, and to report which seam was used. Every production-code step carries its real code.
- **Type consistency:** `StreamBackpressureObservation{bufferedMs, keyframeInQueue}` and `StreamBackpressureDecision{transition, discardBacklog}` (Task 2) are what Task 4 constructs and reads; `bitstreamBufferedMs()` / `bitstreamQueueHasKeyframe_` / `republishQueueTelemetryLocked()` (Task 3) are what Tasks 4, 5 and 6 call; `discardBacklogToNextKeyframe()` (Task 5) is called only from the site Task 4 marked; `OutputBackpressureState` (Task 6) names the same fields the policy exposes.
