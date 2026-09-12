#pragma once

#include "modules/Interfaces.h"
#include "modules/ZoomActiveSpeakerDirector.h"
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
  std::uint32_t lastFrameId = 0;
  std::uint32_t staleFrameCount = 0;
  std::uint32_t malformedFrameCount = 0;
  double firstFrameAtMs = -1.0;
  double lastFrameAtMs = -1.0;
  double firstFrameDelayMs = -1.0;
  double lastFrameAgeMs = -1.0;
  bool frameFresh = false;
};

struct ZoomEngineRuntimeSnapshot {
  std::string meetingState = "idle";
  std::string activeSpeakerId;
  std::string screenShareParticipantId;
  std::vector<ZoomEngineParticipant> participants;
  std::vector<ZoomEngineSubscriptionStats> subscriptions;
  std::vector<std::string> events;
  std::vector<std::string> warnings;
  // Engine-reported raw-media state (raw_media_status events). True only after
  // the engine confirmed StartRawRecording; false after stop_raw_media
  // completed (StopRawRecording + unsubscribe_all) or on leave/reset. The
  // Capture UI reflects THIS, not the last command we sent.
  bool rawMediaActive = false;
};

class ZoomEngineRuntimeState {
 public:
  void apply(const ZoomEngineEvent& event);
  void apply(const ZoomEngineEvent& event, std::uint64_t nowMs);
  void advanceActiveSpeaker(std::uint64_t nowMs);
  // #478 R1: restrict speaker direction to the shell's source set (see
  // ZoomActiveSpeakerDirector). `active == false` lifts the restriction.
  void setSpeakerSources(bool active, std::vector<std::uint32_t> sourceParticipantIds,
                         std::uint64_t nowMs);
  void reset();

  [[nodiscard]] ZoomEngineRuntimeSnapshot snapshot() const;
  [[nodiscard]] bool sdkAuthenticated() const { return sdkAuthenticated_; }
  // The DIRECTED speaker (the director's choice, not Zoom's raw event), "" when none.
  [[nodiscard]] std::string directedSpeakerIdString() const {
    return activeSpeakerId_ == 0 ? std::string{} : std::to_string(activeSpeakerId_);
  }
  // Roster membership without projecting the whole snapshot. Tells a dropped
  // subscription apart from a departed participant.
  [[nodiscard]] bool hasParticipant(std::uint32_t participantId) const {
    return participants_.find(participantId) != participants_.end();
  }
  // Camera state from the engine roster. Tells a video subscription dropped
  // because the camera went off apart from one the operator un-routed.
  [[nodiscard]] bool participantHasVideo(std::uint32_t participantId) const {
    const auto found = participants_.find(participantId);
    return found != participants_.end() && found->second.hasVideo;
  }
  [[nodiscard]] rpc::Json::Array participantsJson() const;
  [[nodiscard]] std::vector<VideoFrame> pollCompositorVideoFrames(int64_t timestampMs) const;
  [[nodiscard]] std::vector<AudioFrame> pollCompositorAudioFrames(int64_t timestampMs) const;
  void recordFrameIngestSuccess(const std::string& sourceUuid,
                                std::uint32_t participantId,
                                std::uint32_t width,
                                std::uint32_t height,
                                std::uint32_t frameId,
                                double observedAtMs);
  void recordFrameIngestFailure(const std::string& sourceUuid,
                                std::uint32_t participantId,
                                const std::string& reason);
  void refreshFrameFreshness(double nowMs, double staleAfterMs);

 private:
  void addWarning(const std::string& warning);
  void refreshSpeakerFrameReadiness();

  std::string meetingState_ = "idle";
  bool sdkAuthenticated_ = false;
  bool rawMediaActive_ = false;
  std::uint32_t activeSpeakerId_ = 0;
  std::uint32_t screenShareParticipantId_ = 0;
  std::map<std::uint32_t, ZoomEngineParticipant> participants_;
  std::map<std::string, ZoomEngineSubscriptionStats> subscriptionStats_;
  std::vector<std::string> events_;
  std::vector<std::string> warnings_;
  ZoomActiveSpeakerDirector speakerDirector_;
};

}  // namespace corevideo::modules
