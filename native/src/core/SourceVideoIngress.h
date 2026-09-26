#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/SourceBus.h"
#include "rpc/Json.h"

namespace corevideo::core {
class MediaTransports;

class CaptureVideoToSourceBus final : public modules::ICaptureVideoConsumer {
 public:
  explicit CaptureVideoToSourceBus(SourceBus& bus);
  void publish(modules::VideoFrame frame) override;
  void end(const std::string& participantId) override;
 private:
  SourceBus& bus_;
};

// Preserve legacy draw order and the Zoom subscription roster's authority.
std::vector<modules::VideoFrame> gatherSourceVideo(
    SourceBus& bus, MediaTransports* mediaTransports,
    const std::vector<modules::VideoFrame>& engineFrames,
    const std::vector<modules::VideoFrame>& browserFrames,
    bool engineLive, int64_t mediaPresentationTime100ns, int64_t nowNs);

void appendStillSourceVideo(SourceBus& bus,
    const std::vector<modules::VideoFrame>& stillFrames,
    int64_t mediaPresentationTime100ns, int64_t nowNs,
    std::vector<modules::VideoFrame>& frames);

struct SourcePresentationPolicy {
  std::string dropoutPolicy = "hold";
  std::string displayName;
};

// SourceBus owns counters and health; this is their one wire projection.
rpc::Json sourceHealthState(const SourceBus* bus, int64_t nowNs,
    const std::function<SourcePresentationPolicy(const std::string&)>& policyFor);

} // namespace corevideo::core
