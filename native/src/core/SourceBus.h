#pragma once
#include <cstdint>
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
  std::string kind;       // "zoom" | "capture" | "media" | "composed" | "test"
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
  virtual SourceIngestCounters counters() const = 0;
};

// SourceBus aggregates a set of ISource instances: it polls each once per
// ingest() call, merges their frames, and tracks per-source counters/health.
class SourceBus {
 public:
  struct IngestResult {
    std::vector<modules::VideoFrame> video;
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

  IngestResult ingest(int64_t programTime100ns, int64_t nowNs) {
    IngestResult out;
    for (auto& [id, e] : entries_) {
      SourceTick tick = e.source->poll(programTime100ns);
      for (auto& v : tick.video) {
        const bool isNew = !e.everProduced || v.frameId != e.counters.lastFrameId;
        if (isNew) {
          e.counters.framesIngested += 1;
          e.counters.lastFrameId = v.frameId;
          e.counters.lastNewFrameNs = nowNs;
          e.everProduced = true;
        }
        out.video.push_back(std::move(v));
      }
      for (auto& a : tick.audio) out.audio.push_back(std::move(a));
    }
    return out;
  }

  std::vector<SourceStatus> snapshot(int64_t nowNs) const {
    std::vector<SourceStatus> out;
    out.reserve(entries_.size());
    for (const auto& [id, e] : entries_) {
      SourceHealth h;
      if (!e.everProduced) {
        h = SourceHealth::Warming;
      } else if (nowNs - e.counters.lastNewFrameNs <= kStaleAfterNs) {
        h = SourceHealth::Producing;
      } else {
        h = SourceHealth::Stalled;
      }
      out.push_back({e.source->descriptor(), e.counters, h});
    }
    return out;
  }

 private:
  static constexpr int64_t kStaleAfterNs = 200'000'000;  // 200ms
  struct Entry {
    std::shared_ptr<ISource> source;
    SourceIngestCounters counters;
    bool everProduced = false;
  };
  std::map<std::string, Entry> entries_;  // stable id order for the snapshot
};

}  // namespace corevideo::core
