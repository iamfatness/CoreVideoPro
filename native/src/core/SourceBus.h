#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "modules/Interfaces.h"

namespace corevideo::core {

enum class SourceHealth { Producing, Warming, Stalled, Failed, Idle };

inline const char* sourceHealthName(SourceHealth health) {
  switch (health) {
    case SourceHealth::Producing: return "producing";
    case SourceHealth::Warming: return "warming";
    case SourceHealth::Stalled: return "stalled";
    case SourceHealth::Failed: return "failed";
    case SourceHealth::Idle: return "idle";
  }
  return "idle";
}

struct SourceDescriptor {
  std::string sourceId;   // canonical scheme:id == VideoFrame::participantId
  std::string kind;       // "zoom" | "capture" | "media" | "still" | "composed" | "test"
  int width = 0;
  int height = 0;
  std::optional<int> fpsNumerator, fpsDenominator;
  std::string pixelFormat;  // "bgra" | "i420"
  bool hasVideo = false;
  bool hasAudio = false;
  bool composed = false;
};

struct SourceIngestCounters {
  uint64_t framesIngested = 0;   // a poll() that returned a NEW frameId
  uint64_t droppedFrames = 0;    // pool refusal or source-reported drop
  int64_t lastFrameId = 0;
  int64_t lastNewFrameNs = 0;    // caller monotonic clock; never UTC
  uint64_t audioPacketsIngested = 0;
  uint64_t audioSamplesIngested = 0; // interleaved PCM frames, not metadata placeholders
  int64_t lastAudioNs = 0;
};

struct SourceTick {
  std::vector<modules::VideoFrame> video;  // 0..1 normal; N for share+cam
  std::vector<modules::AudioFrame> audio;  // 0..1 program-rate PCM chunk
  SourceHealth health = SourceHealth::Idle;
  int64_t clockOffset100ns = 0;            // source clock - program epoch
};

class ISource {
 public:
  virtual ~ISource() = default;
  virtual const SourceDescriptor& descriptor() const = 0;
  virtual SourceTick poll(int64_t programTime100ns) = 0;
  // Separate cadence: a render poll must never drain PCM intended for the
  // 20ms audio worker. Called in the same serialized bus ownership domain.
  virtual std::vector<modules::AudioFrame> pollAudio(int64_t) { return {}; }
  virtual SourceIngestCounters counters() const = 0;
};

// SourceBus aggregates a set of ISource instances: it polls each once per
// ingest() call, merges their frames, and tracks per-source counters/health.
class SourceBus {
 public:
  struct IngestResult {
    std::vector<modules::VideoFrame> video;
    // Parallel to `video`: the kind of the source that produced each frame.
    // A source may emit several participant ids, so the kind cannot be
    // recovered by looking the frame id up on the bus.
    std::vector<std::string> videoKinds;
    std::vector<modules::AudioFrame> audio;
  };
  struct SourceStatus {
    SourceDescriptor descriptor;
    SourceIngestCounters counters;
    SourceHealth health = SourceHealth::Idle;
  };

  void add(std::shared_ptr<ISource> source) {
    if (!source) return;
    const std::string id = source->descriptor().sourceId;
    entries_[id] = Entry{std::move(source), {}, false};
  }
  void remove(const std::string& sourceId) { entries_.erase(sourceId); }
  bool empty() const { return entries_.empty(); }
  bool contains(const std::string& sourceId) const { return entries_.count(sourceId) != 0; }
  ISource* sourceFor(const std::string& sourceId) const {
    auto it = entries_.find(sourceId);
    return it == entries_.end() ? nullptr : it->second.source.get();
  }
  std::vector<std::string> sourceIds() const {
    std::vector<std::string> ids;
    ids.reserve(entries_.size());
    for (const auto& [id, e] : entries_) ids.push_back(id);
    return ids;
  }

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
      const auto kind = e.source->descriptor().kind;
      for (auto& v : tick.video) {
        const bool isNew = !e.everProduced || v.frameId != e.counters.lastFrameId;
        if (isNew) {
          e.counters.framesIngested += 1;
          e.counters.lastFrameId = v.frameId;
          e.counters.lastNewFrameNs = nowNs;
          e.everProduced = true;
        }
        out.video.push_back(std::move(v));
        out.videoKinds.push_back(kind);
      }
      for (auto& a : tick.audio) out.audio.push_back(std::move(a));
    }
    return out;
  }

  std::vector<SourceStatus> snapshot(int64_t nowNs) const {
    std::vector<SourceStatus> out;
    out.reserve(entries_.size());
    for (const auto& [id, e] : entries_) {
      out.push_back({e.source->descriptor(), e.counters, healthOf(e, nowNs)});
    }
    return out;
  }

  std::vector<modules::AudioFrame> ingestAudio(int64_t programTime100ns, int64_t nowNs) {
    std::vector<modules::AudioFrame> out;
    for (auto& [id, e] : entries_) {
      if (!e.source->descriptor().hasAudio) continue;
      for (auto& frame : e.source->pollAudio(programTime100ns)) {
        if (!frame.pcm.empty() && frame.channels > 0) {
          ++e.counters.audioPacketsIngested;
          e.counters.audioSamplesIngested += frame.pcm.size() / frame.channels;
          e.counters.lastAudioNs = nowNs;
        }
        out.push_back(std::move(frame));
      }
    }
    return out;
  }

  // nullopt when the source is not on the bus at all; otherwise the same
  // derivation snapshot() uses for that source's entry.
  std::optional<SourceHealth> healthFor(const std::string& sourceId, int64_t nowNs) const {
    auto it = entries_.find(sourceId);
    if (it == entries_.end()) return std::nullopt;
    return healthOf(it->second, nowNs);
  }

  // #535 slice 4a (R1): the on-air stall decision needs more than the 200ms
  // diagnostic health snapshot() reports — it needs to know the SOURCE KIND
  // (stall-on-cadence is Zoom-only) and how long since a real new frame (the
  // 1.5s on-air hysteresis is a different threshold entirely). This sits
  // alongside healthFor/snapshot rather than replacing them: snapshot()'s
  // sources[] diagnostic stays on the 200ms rule unchanged.
  struct OnAirStatus {
    SourceHealth health = SourceHealth::Idle;   // the 200ms diagnostic health
    int64_t sinceLastNewFrameNs = 0;            // 0 if never produced
    std::string_view kind;                      // descriptor().kind, e.g. "zoom"
  };
  std::optional<OnAirStatus> onAirStatusFor(const std::string& sourceId, int64_t nowNs) const {
    auto it = entries_.find(sourceId);
    if (it == entries_.end()) return std::nullopt;
    const Entry& e = it->second;
    OnAirStatus status;
    status.health = healthOf(e, nowNs);
    status.sinceLastNewFrameNs = e.everProduced ? (nowNs - e.counters.lastNewFrameNs) : 0;
    status.kind = e.source->descriptor().kind;
    return status;
  }

 private:
  static constexpr int64_t kStaleAfterNs = 200'000'000;  // 200ms
  struct Entry {
    std::shared_ptr<ISource> source;
    SourceIngestCounters counters;
    bool everProduced = false;
  };
  std::map<std::string, Entry> entries_;  // stable id order for the snapshot

  static SourceHealth healthOf(const Entry& e, int64_t nowNs) {
    if (!e.source->descriptor().hasVideo && e.source->descriptor().hasAudio) {
      if (e.counters.audioPacketsIngested == 0) return SourceHealth::Warming;
      return nowNs - e.counters.lastAudioNs <= kStaleAfterNs
          ? SourceHealth::Producing : SourceHealth::Stalled;
    }
    if (!e.everProduced) return SourceHealth::Warming;
    if (nowNs - e.counters.lastNewFrameNs <= kStaleAfterNs) return SourceHealth::Producing;
    return SourceHealth::Stalled;
  }
};

// #535 slice 4a (R1): the ON-AIR stall threshold, deliberately different from
// SourceBus::kStaleAfterNs (200ms). 200ms is a fast DIAGNOSTIC signal (used
// for sources[] / operator troubleshooting); an on-air CUT to black must
// never fire on ordinary cadence noise — browser sources, WGC screen
// capture, stills and paused clips legitimately re-serve the same frameId
// for long stretches while perfectly healthy, and even a live Zoom guest on
// a slow link or sharing a static screen can go well past 200ms between
// real frames. 1.5s is long enough that only a genuinely dead Zoom feed
// trips it, and it recovers on the very next new frame (lastNewFrameNs
// advances the instant one arrives — no separate "recovered" state to get
// wrong).
inline constexpr int64_t kOnAirStallNs = 1'500'000'000;

}  // namespace corevideo::core
