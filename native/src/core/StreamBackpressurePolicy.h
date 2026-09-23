#pragma once

#include <cstddef>
#include <cstdint>

namespace corevideo::core {

// STREAM BACKPRESSURE: degrade the OUTGOING BITRATE, never thrash the ENCODER.
//
// Incident #597: a 10 Mbps H.264 stream to YouTube ran healthy at 60fps/1x for
// 96 seconds, then the network softened, the outgoing queue filled, and the
// core rebuilt the hardware encoder EIGHT TIMES in twenty seconds — each
// rebuild itself costing frames and a keyframe, digging the hole deeper. This
// policy exists to replace that reflex with two deliberate, bounded levers.
//
// THE SIGNAL is the wall-clock AGE of the oldest chunk still sitting in the
// outgoing queue (`bufferedMs`), never a frame count converted to milliseconds
// via the configured frame rate. That conversion is wrong at exactly the
// moment it matters: while Lever A below is changing the effective frame rate
// underneath it, a frame-count signal drifts against wall clock and either
// under- or over-reacts to the very throttling it is supposed to observe. Age
// of the oldest chunk is ground truth regardless of what rate is feeding the
// queue.
//
// TWO LEVERS, because one cannot do both jobs:
//
//   Lever A (the divisor, `divisor()`) — an input frame divisor from 1 up to
//   kMaxDivisor. At divisor d the caller submits roughly 1/d of its frames,
//   which stops the queue from GROWING further. It does nothing to what is
//   already queued: if the queue is already a full second deep, dropping the
//   input rate in half still drains it at the same wall-clock rate the
//   network allows, so a stream can stabilise a full second behind and STAY
//   there indefinitely. That is the exact shape of "still behind, still
//   healthy-looking" that let #597's operator not notice until the encoder
//   was already thrashing.
//
//   Lever B (`discardBacklog`) — a one-shot signal telling the caller to
//   discard everything in the queue up to (not including) the next keyframe:
//   a GOP-tail discard. This is the only thing that can actually shrink an
//   already-bloated queue, and it is why it exists as a separate lever from
//   the divisor: Lever A prevents growth, Lever B clears backlog. Neither
//   substitutes for the other.
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
// network cannot currently carry the configured rate, and it is cheaper to
// shed frames now than to let the deficit compound. kDiscardAboveBufferedMs
// (750ms) is three-quarters of the budget — the point where Lever A alone
// has manifestly not been enough and clearing backlog outright is the only
// way back to real time.
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
  Enter,     // 1 -> 2: throttling engaged
  StepUp,    // divisor increases further
  StepDown,  // divisor decreases, still throttled
  Exit,      // -> 1: back to every frame
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
  // 4 = the input divisor floor. Past this a stream is submitting a quarter
  // of its configured frame rate; further throttling starves the output
  // below anything a viewer would call live video, and the honest answer at
  // that point is a discard (Lever B), not a deeper divisor.
  static constexpr int kMaxDivisor = 4;
  // 250ms = a quarter of the ~1s latency budget (see header comment above).
  // Sustained growth past this is the network failing to carry the
  // configured rate; enter throttling before the deficit compounds.
  static constexpr std::int64_t kThrottleAboveBufferedMs = 250;
  // 750ms = three-quarters of the ~1s budget. Past this, Lever A alone has
  // manifestly not kept up and a GOP-tail discard is the only way back to
  // real time. Comfortably above kThrottleAboveBufferedMs so the discard
  // never fires before throttling has had a chance to work.
  static constexpr std::int64_t kDiscardAboveBufferedMs = 750;
  // 100ms = the recovery threshold. The 100-250ms gap below
  // kThrottleAboveBufferedMs is the anti-flap HYSTERESIS BAND: throttling
  // itself drains the queue, so recovering at the same threshold that
  // triggered entry would let the queue immediately regrow and flap the
  // divisor every cycle. A stream must show real headroom, not just
  // "no longer over the enter line", before Lever A backs off.
  static constexpr std::int64_t kRecoverBelowBufferedMs = 100;
  // 30 consecutive over-threshold ticks before stepping the divisor up. A
  // keyframe or a momentary scene-cut spikes the queue for a tick or two;
  // only SUSTAINED growth is evidence the network genuinely cannot keep up,
  // and reacting to a one-off spike would throttle a stream that was about
  // to recover on its own.
  static constexpr std::int64_t kEnterAfterOverWaterTicks = 30;
  // 600 consecutive healthy ticks (20x kEnterAfterOverWaterTicks) before
  // stepping down. Recovery is deliberately far slower than entry — the same
  // enter-fast/recover-slowly asymmetry MonitorShedPolicy uses, and for the
  // same reason: a wrongly-early recovery immediately regrows the backlog
  // that just took real effort to shed, while a late recovery only costs a
  // few more ticks at a divisor the stream was already tolerating.
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

  // 1 = every frame; up to kMaxDivisor.
  [[nodiscard]] int divisor() const { return divisor_; }
  // 0 = not throttled; kMaxDivisor - 1 at the floor.
  [[nodiscard]] int level() const { return divisor_ - 1; }
  // Times throttling was ENGAGED (1 -> 2). Steps within a throttle do not count.
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
template <typename Chunks, typename IsKeyframe>
[[nodiscard]] std::size_t discardableGopTailLength(const Chunks& chunks, IsKeyframe isKeyframe) {
  const std::size_t size = chunks.size();
  std::size_t keyframeIndex = size;  // sentinel: no keyframe found yet
  for (std::size_t i = 0; i < size; ++i) {
    if (isKeyframe(chunks[i])) {
      keyframeIndex = i;
      break;
    }
  }
  if (keyframeIndex == size) return 0;  // no keyframe queued: nothing safe to drop
  return keyframeIndex;  // 0 when the keyframe is already at the head
}

}  // namespace corevideo::core
