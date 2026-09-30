#pragma once

#include <cstddef>
#include <cstdint>

namespace corevideo::core {

// STREAM BACKPRESSURE: recover one destination without rebuilding or
// throttling the shared Program encoder.
//
// Incident #597: a 10 Mbps H.264 stream to YouTube ran healthy at 60fps/1x for
// 96 seconds, then the network softened, the outgoing queue filled, and the
// core rebuilt the hardware encoder EIGHT TIMES in twenty seconds — each
// rebuild itself costing frames and a keyframe, digging the hole deeper. This
// policy replaced that reflex with a pressure ladder and GOP discard. Under
// #538 the ladder's historical `divisor()` is no longer applied to the
// compositor: one encoder texture feeds every destination, and a slow socket
// must not reduce all streams' input cadence. The divisor remains pressure
// telemetry; queue discard and #605 IDR recovery act on the destination.
//
// THE SIGNAL is the wall-clock AGE of the oldest chunk still sitting in the
// outgoing queue (`bufferedMs`), never a frame count converted to milliseconds
// via the configured frame rate. That conversion is wrong at exactly the
// moment it matters: a frame-count signal drifts against wall clock under
// congestion. Age of the oldest chunk is ground truth regardless of rate.
//
// The historical policy still represents two decisions:
//
//   Lever A (`divisor()`) is now only a pressure rung. It does not skip shared
//   encoder input. A congested destination drops only its own packets.
//
//   Lever B (`discardBacklog`) — a one-shot signal telling the caller to
//   discard everything in the queue up to (not including) the next keyframe:
//   a GOP-tail discard. This shrinks an already-bloated destination queue.
//
// WHY THE DISCARD IS A GOP TAIL, NOT AN ARBITRARY FRAME: our HEVC/AV1
// encoders run with B-frames disabled (the low-latency work elsewhere in this
// codebase — see MonitorShedPolicy's neighbours), so every frame in the GOP
// after the keyframe is a P-frame referencing the frame before it. There are
// no non-reference B-frames to drop for free. Dropping an arbitrary P-frame
// does not just lose that frame — every frame referencing it, and every frame
// referencing those, decodes corrupted until the next keyframe arrives. The
// only safe discard is "everything up to the next keyframe, then resume
// cleanly from it" — which is why the decision requires a keyframe to be
// sitting in the queue to discard UP TO (`keyframeInQueue`): with no keyframe
// there is nothing safe to cut forward to, and the policy must wait.
//
// THE ~1 SECOND LATENCY BUDGET every threshold below is justified against:
// at 1080p60 / 10 Mbps (the #597 profile), the outgoing queue holds roughly
// one second of video before it visibly falls behind live. kThrottleAboveBufferedMs
// (250ms) is a quarter of that budget — sustained growth past it means the
// network cannot currently carry the configured rate. The pressure rung
// records sustained congestion. kDiscardAboveBufferedMs (750ms) is three
// quarters of the budget, when the destination must cut its own backlog.
//
// Pure value-in/value-out state machine: no clock, no I/O, no allocation —
// integer comparisons only. Deliberately mirrors `MonitorShedPolicy.h`, the
// proven precedent in this codebase for exactly this shape: an observation
// struct, a transition enum, a saturating counter discipline, enter-fast /
// recover-slowly, and a hysteresis band between the enter and recover
// thresholds so the policy cannot flap between two states once per tick.

struct StreamBackpressureObservation {
  // Wall-clock age (ms) of the oldest chunk still queued for send. < 0 means
  // "unknown" and the tick is ignored (no evidence either way) — never
  // interpreted as "healthy".
  std::int64_t bufferedMs = -1;
  // Is a keyframe currently sitting in the outgoing queue? A GOP-tail discard
  // needs one to cut forward to; with none, Lever B cannot fire safely.
  bool keyframeInQueue = false;
};

enum class StreamBackpressureTransition {
  None,
  Enter,     // 1 -> 2: sustained destination pressure
  StepUp,    // divisor increases further
  StepDown,  // pressure rung decreases
  Exit,      // -> 1: pressure cleared
};

struct StreamBackpressureDecision {
  StreamBackpressureTransition transition = StreamBackpressureTransition::None;
  // true exactly on the tick Lever B should discard the queue up to (not
  // including) the next keyframe. One-shot: never true on two consecutive
  // ticks (see kDiscardCooldownTicks).
  bool discardBacklog = false;
};

class StreamBackpressurePolicy {
 public:
  // Historical divisor ceiling, now the maximum pressure rung. It does not
  // reduce the shared encoder's input rate.
  static constexpr int kMaxDivisor = 4;
  // 250ms = a quarter of the ~1s latency budget (see header comment above).
  // Sustained growth past this is the network failing to carry the
  // configured rate; mark sustained pressure before the deficit compounds.
  static constexpr std::int64_t kThrottleAboveBufferedMs = 250;
  // 750ms = three-quarters of the ~1s budget. A GOP-tail discard is needed
  // to return this destination toward live. The lower threshold establishes
  // sustained pressure before the lossy discard can fire.
  static constexpr std::int64_t kDiscardAboveBufferedMs = 750;
  // 100ms = the recovery threshold. The 100-250ms gap below
  // kThrottleAboveBufferedMs is an anti-flap hysteresis band. The queue must
  // show real headroom before its pressure recommendation backs off.
  static constexpr std::int64_t kRecoverBelowBufferedMs = 100;
  // 30 consecutive over-threshold ticks before stepping the divisor up. A
  // keyframe or a momentary scene-cut spikes the queue for a tick or two;
  // only SUSTAINED growth is evidence the network genuinely cannot keep up,
  // and reacting to a one-off spike would misreport congestion.
  static constexpr std::int64_t kEnterAfterOverWaterTicks = 30;
  // 600 consecutive healthy ticks (20x kEnterAfterOverWaterTicks) before
  // stepping down. Recovery is deliberately far slower than entry — the same
  // enter-fast/recover-slowly asymmetry MonitorShedPolicy uses. This changes
  // only the pressure signal; packet discard has its own cooldown.
  static constexpr std::int64_t kRecoverAfterHealthyTicks = 600;
  // 60 ticks of cooldown between discard events. A discard is a visible,
  // lossy event (real playback interruption at the viewer) — firing it every
  // tick the queue happens to sit above the discard threshold would turn one
  // bad network moment into a rolling stutter instead of one clean recovery.
  static constexpr std::int64_t kDiscardCooldownTicks = 60;

  StreamBackpressureDecision observe(const StreamBackpressureObservation& o) {
    StreamBackpressureDecision decision;
    if (o.bufferedMs < 0) return decision;  // no evidence either way
    if (discardCooldown_ > 0) --discardCooldown_;

    // Do not discard on a transient spike: the pressure rung must have entered
    // first, and a decodable keyframe must be available in this queue.
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

  // 1 = no sustained pressure; up to kMaxDivisor. Advisory only.
  [[nodiscard]] int divisor() const { return divisor_; }
  // 0 = no sustained pressure; kMaxDivisor - 1 at the highest rung.
  [[nodiscard]] int level() const { return divisor_ - 1; }
  // Times sustained pressure was entered (1 -> 2).
  [[nodiscard]] std::int64_t enteredCount() const { return enteredCount_; }
  // Cumulative GOP-tail discard events fired.
  [[nodiscard]] std::int64_t discardEvents() const { return discardEvents_; }
  // Why the divisor or discard state last changed:
  // "none" | "buffered-above-threshold" | "recovered" | "backlog-discard".
  [[nodiscard]] const char* lastReason() const { return lastReason_; }
  // The bufferedMs observed on the tick that caused the last change.
  [[nodiscard]] std::int64_t lastTransitionBufferedMs() const { return lastTransitionBufferedMs_; }

  [[nodiscard]] static const char* transitionName(StreamBackpressureTransition t) {
    switch (t) {
      case StreamBackpressureTransition::Enter: return "enter";
      case StreamBackpressureTransition::StepUp: return "step-up";
      case StreamBackpressureTransition::StepDown: return "step-down";
      case StreamBackpressureTransition::Exit: return "exit";
      case StreamBackpressureTransition::None: break;
    }
    return "none";
  }

 private:
  // Counters saturate rather than wrap; a snapshot must never see one go down.
  static constexpr std::int64_t kCounterCeiling = INT64_C(1) << 62;

  int divisor_ = 1;
  std::int64_t overStreak_ = 0;
  std::int64_t healthyStreak_ = 0;
  std::int64_t discardCooldown_ = 0;
  std::int64_t enteredCount_ = 0;
  std::int64_t discardEvents_ = 0;
  const char* lastReason_ = "none";
  std::int64_t lastTransitionBufferedMs_ = 0;
};

// #597 Lever B, fix round 1 (review finding 2): the "what to discard" decision,
// extracted so it can be unit-tested with NO seam of any kind on the sender -
// no queue, no lock, no mutex, no encoder. Pure value-in/value-out, exactly
// like StreamBackpressurePolicy::observe() above it.
//
// `chunks[i]` is any indexable, sized range; `isKeyframe(chunks[i])` answers
// whether that element is a keyframe. The caller's real bitstream queue and a
// bare vector<bool> in a test both satisfy this with zero adaptation.
//
// Returns the number of elements strictly AHEAD of the first keyframe found -
// exactly what is safe to discard (see the discard-safety rationale on
// StreamBackpressureDecision::discardBacklog above). With no keyframe present
// anywhere in the range, returns 0: there is nothing safe to cut forward to.
//
// THE SENTINEL IS THE WHOLE POINT (review finding 5). `keyframeIndex` starts
// at `size`, not `0` - so "no keyframe found" and "keyframe already at the
// head" are DISTINCT values, and only the explicit `keyframeIndex == size`
// guard turns "not found" into "drop nothing". Initializing the sentinel to 0
// instead collapses those two cases and makes a "no keyframe queued drops
// nothing" test pass even with the guard deleted - the regression this exists
// to prevent is initializing the sentinel to `size` and then OMITTING the
// guard, which drops the ENTIRE queue with nothing safe to resume at.
//
// WHICH keyframe to cut forward to is a PARAMETER, not a second copy of this
// logic, because the two callers want opposite things (Task 8b fix round 1):
//
//   - `GopCutPoint::Nearest` (the default, Lever B's ordinary discard) takes
//     the FIRST queued keyframe. Its job is recovering latency as the smallest
//     clean skip that helps; cutting further throws away picture the situation
//     did not need.
//   - `GopCutPoint::Last` (the queue's OVERFLOW path) takes the LAST queued
//     keyframe, maximising the room freed. It is the last resort and its only
//     alternative is failing the sender, which restarts it and rebuilds the
//     encoder - #597 itself. There, more freed room is unambiguously better.
//
// BOTH REST ON THE SAME SAFETY ARGUMENT, and it is the one the acceptance gate
// measured: every keyframe sample this encoder emits is a self-contained IDR
// carrying its own parameter sets in band (200 traced chunks each for h264 and
// hevc), so a decoder resumes cleanly at ANY of them. This is NOT the discard
// reasoning about which PRECEDING chunks are headers - that judgement stays
// forbidden, and nothing here inspects a chunk for anything but `isKeyframe`.
enum class GopCutPoint {
  Nearest,  // first queued keyframe: the smallest clean skip
  Last,     // last queued keyframe: the most room a safe cut can free
};

template <typename Chunks, typename IsKeyframe>
[[nodiscard]] std::size_t discardableGopTailLength(const Chunks& chunks, IsKeyframe isKeyframe,
                                                  GopCutPoint cutPoint = GopCutPoint::Nearest) {
  const std::size_t size = chunks.size();
  std::size_t keyframeIndex = size;  // sentinel: no keyframe found yet
  for (std::size_t i = 0; i < size; ++i) {
    if (isKeyframe(chunks[i])) {
      keyframeIndex = i;
      if (cutPoint == GopCutPoint::Nearest) break;
      // GopCutPoint::Last keeps scanning: the LAST keyframe wins.
    }
  }
  if (keyframeIndex == size) return 0;  // no keyframe queued: nothing safe to drop
  return keyframeIndex;  // 0 when the chosen keyframe is already at the head
}

// #597 Task 8b fix round 1, measured half-way through: THE ARRIVING CHUNK IS
// ALSO A CUT POINT.
//
// The burst gate reached the cap and the sender still failed, with this exact
// shape (two runs, both identical):
//
//   nothing safe to drop (no keyframe queued); queuedBytes=590149
//   queuedChunks=60 incomingBytes=22623
//
// 60 queued chunks averaging 9.8 KB - all P-frames, no keyframe anywhere, so
// `discardableGopTailLength` correctly frees nothing - while the chunk being
// REFUSED is 22.6 KB, keyframe-sized. The queue could not be cut because its
// own cut point was the thing at the door. (The window is one GOP wide and the
// GOP is 60 frames, so a queue that has just drained its keyframe holds none
// until the next one arrives - and Lever A stretches that wait in wall time,
// because it sheds INPUT frames while the keyframe interval is counted in
// frames.)
//
// A keyframe ARRIVAL makes the entire backlog discardable: the decoder resumes
// at that IDR, which is the identical safety argument as cutting to a queued
// one - the gate proved every keyframe here is self-contained - and it is the
// natural completion of "cut to the LAST keyframe", because the arrival IS the
// newest keyframe. Nothing here inspects a chunk for anything but `isKeyframe`.
//
// Returns how many QUEUED chunks are safe to drop to make room for `arrival`.
template <typename Chunks, typename IsKeyframe>
[[nodiscard]] std::size_t discardableBacklogForArrival(const Chunks& chunks, IsKeyframe isKeyframe,
                                                      bool arrivalIsKeyframe,
                                                      GopCutPoint cutPoint = GopCutPoint::Nearest) {
  if (arrivalIsKeyframe) return chunks.size();  // resume at the arriving IDR
  return discardableGopTailLength(chunks, isKeyframe, cutPoint);
}

}  // namespace corevideo::core
