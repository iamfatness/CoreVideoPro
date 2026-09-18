#pragma once

// WHEN the recording sink may rebuild its per-ISO status snapshot (#529).
//
// THE DEFECT THIS EXISTS TO STOP. `MediaFoundationEncoderAdapter` publishes
// per-ISO health by rebuilding `session_.isoStreams` from scratch: for every
// ISO writer it takes that writer's snapshot mutex and copies a struct carrying
// seven std::strings, takes its `RecordingTrackWorker`'s evidence mutex and
// copies another struct with a string, and composes a warning. That rebuild ran
// on EVERY submitted media item.
//
// The encode is NOT on that thread — each ISO file has its own
// `RecordingTrackWorker`, so the `AsyncEncoderSink` writer thread is a
// DISPATCHER and everything it does per item is multiplied by the item rate.
// `submitIsoVideo` splits a batch to one source per item, so eight ISO sources
// plus Program at 60 fps is ~540 items/s: roughly 16 mutex acquisitions and
// >100 string copies per item spent on bookkeeping, by the one thread that has
// to keep nine files fed.
//
// It also had a FEEDBACK LOOP, which is what made the loss permanent instead of
// transient. Once a writer has dropped anything its status warning is no longer
// empty, so every later rebuild re-composes "ISO recording lost N video frames
// and M audio packets: <name>" with std::to_string. Slower dispatch -> deeper
// queue -> more drops -> longer strings -> slower dispatch. Measured live on an
// RTX 4090 (1080p60 Program + YouTube stream + eight ISOs): 8,482 dropped video
// items over ~100 s, and the loss never recovered inside the session — it
// continued with no scene changes at all, which is what ruled out Takes as the
// trigger.
//
// THE RULE. Status is DIAGNOSTIC; media is not. A rebuild driven by ARRIVING
// MEDIA is rate-limited. A rebuild driven by a STRUCTURAL event — opening the
// writers, finalizing them — always runs, because those are the points where
// the published status has to be exactly right and where there is no per-frame
// cost to pay.
//
// WHY RATE-LIMITING IS SAFE HERE, stated so nobody has to re-derive it: every
// number this gates is CUMULATIVE and MONOTONIC (dropped counts, bytes written,
// frame counts). Skipping a rebuild therefore makes the published value LAG by
// at most one interval; it can never lose a count or move one backwards. And
// the refresh at close is structural, so the finalized numbers an operator or a
// support bundle reads are never the throttled ones.
//
// Pure: no threads, no I/O, no clock of its own — the caller passes the time it
// already sampled. Same shape as `CaptureReaderStallPolicy` / `MonitorShedPolicy`.

#include <algorithm>
#include <cstdint>

namespace corevideo::modules {

class IsoStatusRefreshGate {
 public:
  // 100 ms = ten status rebuilds a second. Deliberately chosen to be cheap
  // rather than to be a promise: nothing reads this data faster. The shell
  // polls the core snapshot every 250 ms (`MediaCoreBridgeService.PollLoopAsync`),
  // so a status already rebuilt at 10 Hz is ahead of its only consumer.
  static constexpr int64_t kDefaultIntervalMs = 100;

  IsoStatusRefreshGate() = default;
  // 0 disables the gate entirely (every call rebuilds). Kept as a real option
  // so a diagnostic build can restore the old behaviour without a code edit,
  // and so tests can prove the gate is what changed the rate.
  explicit IsoStatusRefreshGate(int64_t intervalMs)
      : intervalMs_(std::max<int64_t>(0, intervalMs)) {}

  // True when a MEDIA-driven rebuild may run now. Const on purpose: it does not
  // record the rebuild, because a caller that ALSO rebuilds for structural
  // reasons must reset the same clock, and it does that through `markRefreshed`.
  // One writer of the timestamp, several readers of the decision.
  [[nodiscard]] bool allowMediaRefresh(int64_t nowMs) const {
    if (intervalMs_ == 0) return true;
    // The first status after a take opens must be published, not waited for:
    // until it exists the snapshot carries the PREVIOUS take's ISO rows.
    if (!refreshed_) return true;
    // A clock that went backwards (a caller sampling a different source, a
    // test driving synthetic time) must not latch the gate shut until real
    // time catches up. Fail toward refreshing: the cost of one extra rebuild
    // is a rebuild; the cost of a stuck gate is status that never updates.
    if (nowMs < lastRefreshMs_) return true;
    return nowMs - lastRefreshMs_ >= intervalMs_;
  }

  // Record that a rebuild happened, for ANY reason. Structural rebuilds call
  // this too, so an open or a close does not leave a media rebuild owing
  // immediately afterwards.
  void markRefreshed(int64_t nowMs) {
    lastRefreshMs_ = nowMs;
    refreshed_ = true;
  }

  [[nodiscard]] int64_t intervalMs() const { return intervalMs_; }
  // Has any rebuild been recorded? A fresh gate always allows the first one.
  [[nodiscard]] bool everRefreshed() const { return refreshed_; }

 private:
  int64_t intervalMs_ = kDefaultIntervalMs;
  int64_t lastRefreshMs_ = 0;
  bool refreshed_ = false;
};

}  // namespace corevideo::modules
