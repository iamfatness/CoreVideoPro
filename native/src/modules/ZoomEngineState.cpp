#include "modules/ZoomEngineState.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace corevideo::modules {
namespace {

std::string participantIdString(std::uint32_t id) {
  return id == 0 ? std::string{} : std::to_string(id);
}

std::string frameWarningPrefix(const ZoomEngineSubscriptionStats& stats) {
  return "Zoom video frame ingest for participant " + stats.participantId + " (" + stats.sourceUuid + ")";
}

std::uint64_t monotonicMs() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

}  // namespace

void ZoomEngineRuntimeState::apply(const ZoomEngineEvent& event) {
  apply(event, monotonicMs());
}

void ZoomEngineRuntimeState::apply(const ZoomEngineEvent& event, std::uint64_t nowMs) {
  switch (event.kind) {
    case ZoomEngineEventKind::Ready:
      events_.emplace_back("Zoom engine ready.");
      break;
    case ZoomEngineEventKind::AuthOk:
      sdkAuthenticated_ = true;
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
      sdkAuthenticated_ = false;
      meetingState_ = "error";
      addWarning(!event.message.empty() ? event.message
                 : !event.stage.empty() ? "Zoom SDK authentication failed during " + event.stage + "."
                                        : "Zoom SDK authentication failed.");
      break;
    case ZoomEngineEventKind::Error:
      // A mid-meeting error (e.g. raw_media_start_failed while the record
      // privilege is pending) is a WARNING — demoting meetingState_ made the
      // shell read "not in a meeting" while live. Only pre-join errors are
      // join failures.
      if (meetingState_ != "in-meeting") {
        meetingState_ = "error";
      }
      addWarning(!event.message.empty() ? event.message
                 : !event.stage.empty() ? "Zoom engine failed during " + event.stage + "."
                                        : "Zoom engine reported an error.");
      break;
    case ZoomEngineEventKind::Participants: {
      participants_.clear();
      screenShareParticipantId_ = 0;
      for (const auto& participant : event.participants) {
        participants_.emplace(participant.id, participant);
        if (participant.isSharingScreen) {
          screenShareParticipantId_ = participant.id;
        }
      }
      auto rawSpeakerId = event.activeSpeakerId;
      if (rawSpeakerId == 0) {
        const auto talking = std::find_if(event.participants.begin(), event.participants.end(), [](const auto& participant) {
          return participant.isTalking;
        });
        if (talking != event.participants.end()) {
          rawSpeakerId = talking->id;
        }
      }
      speakerDirector_.updateRoster(event.participants, rawSpeakerId, nowMs);
      activeSpeakerId_ = speakerDirector_.directedSpeakerId();
      events_.emplace_back("Zoom roster updated.");
      break;
    }
    case ZoomEngineEventKind::ActiveSpeaker: {
      std::vector<ZoomEngineParticipant> roster;
      roster.reserve(participants_.size());
      for (const auto& [_, participant] : participants_) {
        roster.push_back(participant);
      }
      speakerDirector_.updateRoster(roster, event.participantId, nowMs);
      activeSpeakerId_ = speakerDirector_.directedSpeakerId();
      break;
    }
    case ZoomEngineEventKind::RawMediaStatus:
      rawMediaActive_ = event.rawMediaActive;
      events_.emplace_back(event.rawMediaActive
                               ? "Zoom raw media started" + (event.message.empty() ? "." : " (" + event.message + ").")
                               : "Zoom raw media stopped" + (event.message.empty() ? "." : " (" + event.message + ")."));
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

void ZoomEngineRuntimeState::advanceActiveSpeaker(std::uint64_t nowMs) {
  speakerDirector_.tick(nowMs);
  activeSpeakerId_ = speakerDirector_.directedSpeakerId();
}

void ZoomEngineRuntimeState::recordFrameIngestSuccess(const std::string& sourceUuid,
                                                      std::uint32_t participantId,
                                                      std::uint32_t width,
                                                      std::uint32_t height,
                                                      std::uint32_t frameId,
                                                      double observedAtMs) {
  auto& stats = subscriptionStats_[sourceUuid];
  stats.sourceUuid = sourceUuid;
  stats.participantId = participantIdString(participantId);
  stats.kind = "participant-video";
  stats.width = width;
  stats.height = height;
  if (stats.firstFrameAtMs < 0.0) {
    stats.firstFrameAtMs = observedAtMs;
    stats.firstFrameDelayMs = observedAtMs;
  }
  if (stats.lastFrameId == frameId && stats.lastFrameAtMs >= 0.0) {
    ++stats.staleFrameCount;
    addWarning(frameWarningPrefix(stats) + " repeated stale frame " + std::to_string(frameId) + ".");
  }
  stats.lastFrameId = frameId;
  stats.lastFrameAtMs = observedAtMs;
  stats.lastFrameAgeMs = 0.0;
  stats.frameFresh = true;
  refreshSpeakerFrameReadiness();
}

void ZoomEngineRuntimeState::recordFrameIngestFailure(const std::string& sourceUuid,
                                                      std::uint32_t participantId,
                                                      const std::string& reason) {
  auto& stats = subscriptionStats_[sourceUuid];
  stats.sourceUuid = sourceUuid;
  stats.participantId = participantIdString(participantId);
  stats.kind = "participant-video";
  ++stats.malformedFrameCount;
  stats.frameFresh = false;
  refreshSpeakerFrameReadiness();
  addWarning(frameWarningPrefix(stats) + " was malformed or unavailable" + (reason.empty() ? "." : ": " + reason + "."));
}

void ZoomEngineRuntimeState::refreshFrameFreshness(double nowMs, double staleAfterMs) {
  for (auto& [_, stats] : subscriptionStats_) {
    if (stats.kind != "participant-video" || stats.lastFrameAtMs < 0.0) {
      continue;
    }
    stats.lastFrameAgeMs = (std::max)(0.0, nowMs - stats.lastFrameAtMs);
    stats.frameFresh = stats.lastFrameAgeMs <= staleAfterMs;
    if (!stats.frameFresh) {
      addWarning(frameWarningPrefix(stats) + " is stale.");
    }
  }
  refreshSpeakerFrameReadiness();
}

void ZoomEngineRuntimeState::refreshSpeakerFrameReadiness() {
  for (const auto& [participantId, _] : participants_) {
    const auto participantIdText = participantIdString(participantId);
    const bool hasFreshFrame = std::any_of(
        subscriptionStats_.begin(), subscriptionStats_.end(),
        [&](const auto& entry) {
          const auto& stats = entry.second;
          return stats.kind == "participant-video" &&
                 stats.participantId == participantIdText && stats.frameFresh;
        });
    speakerDirector_.setFrameFresh(participantId, hasFreshFrame);
  }
}

void ZoomEngineRuntimeState::addWarning(const std::string& warning) {
  if (warning.empty()) {
    return;
  }
  if (std::find(warnings_.begin(), warnings_.end(), warning) == warnings_.end()) {
    warnings_.push_back(warning);
  }
}

void ZoomEngineRuntimeState::reset() {
  meetingState_ = "idle";
  sdkAuthenticated_ = false;
  rawMediaActive_ = false;
  activeSpeakerId_ = 0;
  speakerDirector_.reset();
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
  snapshot.rawMediaActive = rawMediaActive_;
  return snapshot;
}

std::vector<AudioFrame> ZoomEngineRuntimeState::pollCompositorAudioFrames(int64_t timestampMs) const {
  std::vector<AudioFrame> frames;
  for (const auto& [_, stats] : subscriptionStats_) {
    if (stats.kind != "participant-audio" || stats.audioPacketsReceived <= 0 || stats.participantId.empty()) {
      continue;
    }
    frames.push_back({stats.participantId, 48000, 1, timestampMs});
  }
  return frames;
}

std::vector<VideoFrame> ZoomEngineRuntimeState::pollCompositorVideoFrames(int64_t timestampMs) const {
  std::vector<VideoFrame> frames;
  for (const auto& [_, stats] : subscriptionStats_) {
    if (stats.kind != "participant-video" || stats.framesReceived <= 0 || stats.participantId.empty()) {
      continue;
    }
    VideoFrame frame;
    frame.participantId = stats.participantId;
    frame.width = stats.width > 0 ? static_cast<int>(stats.width) : 1280;
    frame.height = stats.height > 0 ? static_cast<int>(stats.height) : 720;
    frame.naturalWidth = frame.width;
    frame.naturalHeight = frame.height;
    frame.timestampMs = timestampMs;
    frames.push_back(std::move(frame));
  }
  return frames;
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
