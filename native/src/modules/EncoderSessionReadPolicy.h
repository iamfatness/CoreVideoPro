#pragma once

// HOW OFTEN the async encoder writer thread re-reads the wrapped sink's full
// session (#529).
//
// THE COST. `AsyncEncoderSink` runs ONE writer thread, and `submitIsoVideo`
// splits a batch to one source per queue item, so eight ISOs plus Program at
// 60 fps is ~540 items/s through it. After EVERY one of those items the loop
// called `inner->session()`. For the Media Foundation sink that is a rebuild of
// all eight per-ISO status records (each writer's snapshot mutex AND its
// RecordingTrackWorker's evidence mutex, two string-bearing structs apiece)
// plus a by-value copy of the whole OutputSession. At eight ISOs that is
// thousands of lock operations and tens of thousands of string copies a second
// spent on diagnostics, by the one thread that has to keep nine files fed.
//
// It also fed back on itself: once a writer has dropped anything its status
// warning is non-empty, so every later rebuild recomposed it with
// `std::to_string`. Slower dispatch -> deeper queue -> more drops -> longer
// strings -> slower dispatch. A live 1080p60 eight-ISO run lost 8,482 video
// items over ~100 s and never recovered inside the session.
//
// WHERE THE THROTTLE BELONGS, learned the hard way. The first fix put it inside
// the Media Foundation sink's submit path, and that was wrong: THAT sink's
// contract is that a caller which submits and then reads sees exact, current
// counts, and `EncoderRecordingSessionTest` asserts precisely that
// (videoFrameCount, audioSampleCount, isoStreams.size(), read synchronously
// after a handful of submits). Seven of those tests failed, and they were
// right to — they are Windows-only, so no CI job could say so.
//
// `AsyncEncoderSink::session()` is the layer that is ALLOWED to lag: its own
// header says so in as many words — "eventually consistent within a few frames
// — fine for the live app; unit tests that need exact synchronous counts use
// the wrapped sink directly", which is exactly what those tests do. So the
// saving is taken by reading LESS OFTEN here, never by making the wrapped sink
// answer a read with stale numbers.
//
// THE RULE, and why it is not a fixed rate. Media arrives in BURSTS, not evenly:
// `MediaCore::renderIsoVideoTick` drains everything pending, so one 60 Hz tick
// enqueues ~9 items (eight ISO + Program) at once. Reading once per DRAINED
// BURST instead of once per item is therefore ~9x fewer reads at eight ISOs —
// and the saving GROWS with the ISO count, which is exactly the load that
// caused the incident. It also costs no freshness that anybody can observe:
// the published snapshot is exact whenever the writer is idle, which is the
// only moment a settled reading exists at all (`drainForTest` waits for
// precisely that state).
//
// A sustained backlog would never drain, so `kStaleAfterMs` is a BACKSTOP: the
// snapshot is never older than one interval even if the queue never empties.
// It is a bound, not the mechanism.
//
// Structural items (configure / start / stop) and any failure ALWAYS read.
// Start latches the baseline frame count, Stop's barrier decides whether the
// take may claim completion, and a failure must reach the lifecycle at once —
// none of those may be deferred, and none of them happen at media rate.
//
// Pure: no threads, no I/O, no clock of its own — the caller passes the time it
// already sampled. Same shape as `CaptureReaderStallPolicy` / `MonitorShedPolicy`.

#include <algorithm>
#include <cstdint>

namespace corevideo::modules {

class EncoderSessionReadGate {
 public:
  // The backstop only. Deliberately far finer than the 1000 ms freshness budget
  // `OutputLifecyclePolicy` uses to decide `producing`, so throttling the read
  // can never be what decays a healthy destination.
  static constexpr int64_t kStaleAfterMs = 100;

  EncoderSessionReadGate() = default;
  // 0 reads on every item (the pre-#529 behaviour), kept as a real option so a
  // diagnostic build can compare without a code edit.
  explicit EncoderSessionReadGate(int64_t staleAfterMs)
      : staleAfterMs_(std::max<int64_t>(0, staleAfterMs)) {}

  // `structural` = configure/start/stop; `queueDrained` = nothing is left to
  // apply after this item; `failed` = this item threw or the sink reported an
  // error. Const: the caller records the read with `markRead`, so one writer of
  // the timestamp serves every reason for reading.
  [[nodiscard]] bool shouldRead(bool structural, bool queueDrained, bool failed, int64_t nowMs) const {
    if (structural || failed) return true;
    // The primary mechanism: a drained burst is a settled, observable state.
    if (queueDrained) return true;
    if (staleAfterMs_ == 0) return true;
    // Nothing published yet - the first item must establish a snapshot.
    if (!everRead_) return true;
    // A clock that went backwards must not latch the gate shut. Fail toward
    // reading: an extra read costs a read, a stuck gate costs a snapshot that
    // never updates again for the life of the recording.
    if (nowMs < lastReadMs_) return true;
    return nowMs - lastReadMs_ >= staleAfterMs_;
  }

  void markRead(int64_t nowMs) {
    lastReadMs_ = nowMs;
    everRead_ = true;
  }

  [[nodiscard]] int64_t staleAfterMs() const { return staleAfterMs_; }
  [[nodiscard]] bool everRead() const { return everRead_; }

 private:
  int64_t staleAfterMs_ = kStaleAfterMs;
  int64_t lastReadMs_ = 0;
  bool everRead_ = false;
};

}  // namespace corevideo::modules
