#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "modules/Interfaces.h"

namespace corevideo::core {

enum class SourceHealth { Producing, Warming, Stalled, Failed, Idle };

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

}  // namespace corevideo::core
