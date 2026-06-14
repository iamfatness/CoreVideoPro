#include "modules/ZoomEngineState.h"

#include <algorithm>

namespace corevideo::modules {
namespace {

std::string participantIdString(std::uint32_t id) {
  return id == 0 ? std::string{} : std::to_string(id);
}

}  // namespace

void ZoomEngineRuntimeState::apply(const ZoomEngineEvent& event) {
  switch (event.kind) {
    case ZoomEngineEventKind::Ready:
      events_.emplace_back("Zoom engine ready.");
      break;
    case ZoomEngineEventKind::AuthOk:
      events_.emplace_back("Zoom SDK authenticated.");
      if (meetingState_ == "idle") {
        meetingState_ = "joining";
      }
      break;
    case ZoomEngineEventKind::Joined:
      meetingState_ = "in-meeting";
      events_.emplace_back("Zoom meeting joined.");
      break;
    case ZoomEngineEventKind::Left:
      reset();
      events_.emplace_back("Zoom meeting left.");
      break;
    case ZoomEngineEventKind::AuthFail:
    case ZoomEngineEventKind::Error:
      meetingState_ = "error";
      warnings_.emplace_back(event.message.empty() ? "Zoom engine reported an error." : event.message);
      break;
    case ZoomEngineEventKind::Participants:
      participants_.clear();
      screenShareParticipantId_ = 0;
      for (const auto& participant : event.participants) {
        participants_.emplace(participant.id, participant);
        if (participant.isSharingScreen) {
          screenShareParticipantId_ = participant.id;
        }
      }
      activeSpeakerId_ = event.activeSpeakerId;
      if (activeSpeakerId_ == 0) {
        const auto talking = std::find_if(event.participants.begin(), event.participants.end(), [](const auto& participant) {
          return participant.isTalking;
        });
        if (talking != event.participants.end()) {
          activeSpeakerId_ = talking->id;
        }
      }
      events_.emplace_back("Zoom roster updated.");
      break;
    case ZoomEngineEventKind::ActiveSpeaker:
      activeSpeakerId_ = event.participantId;
      break;
    case ZoomEngineEventKind::Frame: {
      auto& stats = subscriptionStats_[event.sourceUuid];
      stats.sourceUuid = event.sourceUuid;
      stats.participantId = participantIdString(event.participantId);
      stats.kind = "participant-video";
      stats.width = event.width;
      stats.height = event.height;
      ++stats.framesReceived;
      break;
    }
    case ZoomEngineEventKind::Audio: {
      auto key = event.sourceUuid.empty() ? "audio:" + participantIdString(event.participantId) : event.sourceUuid + ":audio";
      auto& stats = subscriptionStats_[key];
      stats.sourceUuid = event.sourceUuid;
      stats.participantId = participantIdString(event.participantId);
      stats.kind = "participant-audio";
      ++stats.audioPacketsReceived;
      break;
    }
    default:
      break;
  }
}

void ZoomEngineRuntimeState::reset() {
  meetingState_ = "idle";
  activeSpeakerId_ = 0;
  screenShareParticipantId_ = 0;
  participants_.clear();
  subscriptionStats_.clear();
  events_.clear();
  warnings_.clear();
}

ZoomEngineRuntimeSnapshot ZoomEngineRuntimeState::snapshot() const {
  ZoomEngineRuntimeSnapshot snapshot;
  snapshot.meetingState = meetingState_;
  snapshot.activeSpeakerId = participantIdString(activeSpeakerId_);
  snapshot.screenShareParticipantId = participantIdString(screenShareParticipantId_);
  for (const auto& [_, participant] : participants_) {
    snapshot.participants.push_back(participant);
  }
  for (const auto& [_, stats] : subscriptionStats_) {
    snapshot.subscriptions.push_back(stats);
  }
  snapshot.events = events_;
  snapshot.warnings = warnings_;
  return snapshot;
}

rpc::Json::Array ZoomEngineRuntimeState::participantsJson() const {
  rpc::Json::Array result;
  for (const auto& [id, participant] : participants_) {
    result.emplace_back(rpc::Json::Object{
        {"sdkUserId", participantIdString(id)},
        {"displayName", participant.displayName},
        {"role", "guest"},
        {"videoOn", participant.hasVideo},
        {"muted", participant.isMuted},
        {"talking", participant.isTalking || id == activeSpeakerId_},
        {"sharingScreen", participant.isSharingScreen},
        {"audioLevel", participant.isTalking || id == activeSpeakerId_ ? 70 : 0},
        {"networkQuality", "good"},
    });
  }
  return result;
}

}  // namespace corevideo::modules
