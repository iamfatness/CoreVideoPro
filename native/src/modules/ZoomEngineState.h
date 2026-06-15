#pragma once

#include "modules/Interfaces.h"
#include "modules/ZoomEngineClient.h"
#include "rpc/Json.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace corevideo::modules {

struct ZoomEngineSubscriptionStats {
  std::string sourceUuid;
  std::string participantId;
  std::string kind;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t framesReceived = 0;
  std::uint32_t audioPacketsReceived = 0;
};

struct ZoomEngineRuntimeSnapshot {
  std::string meetingState = "idle";
  std::string activeSpeakerId;
  std::string screenShareParticipantId;
  std::vector<ZoomEngineParticipant> participants;
  std::vector<ZoomEngineSubscriptionStats> subscriptions;
  std::vector<std::string> events;
  std::vector<std::string> warnings;
};

class ZoomEngineRuntimeState {
 public:
  void apply(const ZoomEngineEvent& event);
  void reset();

  [[nodiscard]] ZoomEngineRuntimeSnapshot snapshot() const;
  [[nodiscard]] rpc::Json::Array participantsJson() const;
  [[nodiscard]] std::vector<VideoFrame> pollCompositorVideoFrames(int64_t timestampMs) const;
  [[nodiscard]] std::vector<AudioFrame> pollCompositorAudioFrames(int64_t timestampMs) const;

 private:
  std::string meetingState_ = "idle";
  std::uint32_t activeSpeakerId_ = 0;
  std::uint32_t screenShareParticipantId_ = 0;
  std::map<std::uint32_t, ZoomEngineParticipant> participants_;
  std::map<std::string, ZoomEngineSubscriptionStats> subscriptionStats_;
  std::vector<std::string> events_;
  std::vector<std::string> warnings_;
};

}  // namespace corevideo::modules
