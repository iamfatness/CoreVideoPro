#include "modules/ZoomMeetingSdkAdapter.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_ZOOM

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <rawdata/rawdata_audio_helper_interface.h>
#include <rawdata/rawdata_renderer_interface.h>
#include <rawdata/zoom_rawdata_api.h>
#include <meeting_service_components/meeting_audio_interface.h>
#include <meeting_service_components/meeting_participants_ctrl_interface.h>
#include <meeting_service_components/meeting_raw_archiving_interface.h>
#include <meeting_service_components/meeting_recording_interface.h>
#include <meeting_service_components/meeting_video_interface.h>
#include <meeting_service_interface.h>
#include <zoom_sdk.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace corevideo::modules {
namespace {

std::wstring widenAscii(const std::string& value) {
  return std::wstring(value.begin(), value.end());
}

std::string narrowZchar(const zchar_t* value) {
  if (!value) {
    return {};
  }

  std::string result;
  for (auto cursor = value; *cursor; ++cursor) {
    const auto code = static_cast<unsigned int>(*cursor);
    result.push_back(code <= 0x7F ? static_cast<char>(code) : '?');
  }
  return result;
}

std::optional<UINT64> parseMeetingNumber(const std::string& value) {
  try {
    size_t parsed = 0;
    const auto meetingNumber = std::stoull(value, &parsed, 10);
    if (parsed != value.size() || meetingNumber == 0) {
      return std::nullopt;
    }
    return static_cast<UINT64>(meetingNumber);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

bool hasJoinCredential(const ZoomMeetingSdkJoinRequest& request) {
  return !request.zakToken.empty() || !request.appPrivilegeToken.empty() || !request.joinToken.empty();
}

std::optional<uint32_t> parseSdkUserId(const std::string& participantId) {
  try {
    size_t parsed = 0;
    const auto userId = std::stoul(participantId, &parsed, 10);
    if (parsed != participantId.size() || userId == 0) {
      return std::nullopt;
    }
    return static_cast<uint32_t>(userId);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

int videoWidthFor(const std::string& kind) {
  return kind == "screen-share" ? 1920 : 1280;
}

int videoHeightFor(const std::string& kind) {
  return kind == "screen-share" ? 1080 : 720;
}

std::string sdkErrorName(ZOOMSDK::SDKError error) {
  switch (error) {
    case ZOOMSDK::SDKERR_SUCCESS: return "SDKERR_SUCCESS";
    case ZOOMSDK::SDKERR_UNINITIALIZE: return "SDKERR_UNINITIALIZE";
    case ZOOMSDK::SDKERR_UNAUTHENTICATION: return "SDKERR_UNAUTHENTICATION";
    case ZOOMSDK::SDKERR_INVALID_PARAMETER: return "SDKERR_INVALID_PARAMETER";
    case ZOOMSDK::SDKERR_MODULE_LOAD_FAILED: return "SDKERR_MODULE_LOAD_FAILED";
    case ZOOMSDK::SDKERR_OTHER_SDK_INSTANCE_RUNNING: return "SDKERR_OTHER_SDK_INSTANCE_RUNNING";
    default: return "SDKERR_" + std::to_string(static_cast<int>(error));
  }
}

std::string meetingStatusName(ZOOMSDK::MeetingStatus status) {
  switch (status) {
    case ZOOMSDK::MEETING_STATUS_IDLE: return "idle";
    case ZOOMSDK::MEETING_STATUS_CONNECTING: return "joining";
    case ZOOMSDK::MEETING_STATUS_INMEETING: return "in-meeting";
    case ZOOMSDK::MEETING_STATUS_DISCONNECTING: return "leaving";
    case ZOOMSDK::MEETING_STATUS_ENDED: return "ended";
    case ZOOMSDK::MEETING_STATUS_FAILED: return "error";
    default: return "status-" + std::to_string(static_cast<int>(status));
  }
}

std::string userRoleName(ZOOMSDK::UserRole role) {
  switch (role) {
    case ZOOMSDK::USERROLE_HOST: return "host";
    case ZOOMSDK::USERROLE_COHOST: return "cohost";
    case ZOOMSDK::USERROLE_PANELIST: return "panelist";
    case ZOOMSDK::USERROLE_ATTENDEE: return "attendee";
    case ZOOMSDK::USERROLE_BREAKOUTROOM_MODERATOR: return "host";
    default: return "attendee";
  }
}

std::string recordingStatusName(ZOOMSDK::RecordingStatus status) {
  switch (status) {
    case ZOOMSDK::Recording_Start: return "recording";
    case ZOOMSDK::Recording_Stop: return "stopped";
    case ZOOMSDK::Recording_DiskFull: return "failed";
    case ZOOMSDK::Recording_Pause: return "paused";
    case ZOOMSDK::Recording_Connecting: return "starting";
    case ZOOMSDK::Recording_Fail: return "failed";
    default: return "status-" + std::to_string(static_cast<int>(status));
  }
}

int clampAudioLevel(int level) {
  return std::max(0, std::min(100, level));
}

class MeetingServiceEventSink final : public ZOOMSDK::IMeetingServiceEvent {
 public:
  void onMeetingStatusChanged(ZOOMSDK::MeetingStatus status, int iResult = 0) override {
    status_ = status;
    lastResult_ = iResult;
  }

  void onMeetingStatisticsWarningNotification(ZOOMSDK::StatisticsWarningType type) override { lastStatisticsWarning_ = type; }
  void onMeetingParameterNotification(const ZOOMSDK::MeetingParameter*) override {}
  void onSuspendParticipantsActivities() override { suspended_ = true; }
  void onAICompanionActiveChangeNotice(bool) override {}
  void onMeetingTopicChanged(const zchar_t*) override {}
  void onMeetingFullToWatchLiveStream(const zchar_t*) override {}
  void onUserNetworkStatusChanged(ZOOMSDK::MeetingComponentType, ZOOMSDK::ConnectionQuality, unsigned int userId, bool) override {
    lastNetworkUserId_ = userId;
  }
#if defined(WIN32)
  void onAppSignalPanelUpdated(ZOOMSDK::IMeetingAppSignalHandler*) override {}
#endif

  ZOOMSDK::MeetingStatus status() const { return status_; }
  int lastResult() const { return lastResult_; }

 private:
  ZOOMSDK::MeetingStatus status_ = ZOOMSDK::MEETING_STATUS_IDLE;
  int lastResult_ = 0;
  ZOOMSDK::StatisticsWarningType lastStatisticsWarning_ = static_cast<ZOOMSDK::StatisticsWarningType>(0);
  unsigned int lastNetworkUserId_ = 0;
  bool suspended_ = false;
};

class RawVideoDelegate final : public ZOOMSDK::IZoomSDKRendererDelegate {
 public:
  void onRendererBeDestroyed() override { destroyed_ = true; }
  void onRawDataFrameReceived(YUVRawDataI420*) override { ++framesReceived_; }
  void onRawDataStatusChanged(RawDataStatus status) override { status_ = status; }
  int64_t framesReceived() const { return framesReceived_; }

 private:
  bool destroyed_ = false;
  RawDataStatus status_ = RawData_Off;
  int64_t framesReceived_ = 0;
};

class RawAudioDelegate final : public ZOOMSDK::IZoomSDKAudioRawDataDelegate {
 public:
  void onMixedAudioRawDataReceived(AudioRawData*) override { ++mixedPackets_; }
  void onOneWayAudioRawDataReceived(AudioRawData*, uint32_t userId) override {
    lastUserId_ = userId;
    ++oneWayPackets_;
  }
  void onShareAudioRawDataReceived(AudioRawData*, uint32_t userId) override {
    lastUserId_ = userId;
    ++sharePackets_;
  }
  void onOneWayInterpreterAudioRawDataReceived(AudioRawData*, const zchar_t*) override { ++interpreterPackets_; }
  int64_t packetCount() const { return mixedPackets_ + oneWayPackets_ + sharePackets_ + interpreterPackets_; }
  int64_t packetCountFor(uint32_t userId) const { return userId == 0 || userId == lastUserId_ ? packetCount() : 0; }

 private:
  uint32_t lastUserId_ = 0;
  int64_t mixedPackets_ = 0;
  int64_t oneWayPackets_ = 0;
  int64_t sharePackets_ = 0;
  int64_t interpreterPackets_ = 0;
};

class MeetingVideoEventSink final : public ZOOMSDK::IMeetingVideoCtrlEvent {
 public:
  void onUserVideoStatusChange(unsigned int, ZOOMSDK::VideoStatus) override {}
  void onSpotlightedUserListChangeNotification(ZOOMSDK::IList<unsigned int>*) override {}
  void onHostRequestStartVideo(ZOOMSDK::IRequestStartVideoHandler*) override {}
  void onActiveSpeakerVideoUserChanged(unsigned int userid) override { activeSpeakerId_ = std::to_string(userid); }
  void onActiveVideoUserChanged(unsigned int userid) override { activeVideoUserId_ = std::to_string(userid); }
  void onHostVideoOrderUpdated(ZOOMSDK::IList<unsigned int>*) override {}
  void onLocalVideoOrderUpdated(ZOOMSDK::IList<unsigned int>*) override {}
  void onFollowHostVideoOrderChanged(bool) override {}
  void onUserVideoQualityChanged(ZOOMSDK::VideoConnectionQuality, unsigned int) override {}
  void onVideoAlphaChannelStatusChanged(bool) override {}
  void onCameraControlRequestReceived(unsigned int, ZOOMSDK::CameraControlRequestType, ZOOMSDK::ICameraControlRequestHandler*) override {}
  void onCameraControlRequestResult(unsigned int, ZOOMSDK::CameraControlRequestResult) override {}

  std::string activeSpeakerId() const { return activeSpeakerId_.empty() ? activeVideoUserId_ : activeSpeakerId_; }

 private:
  std::string activeSpeakerId_;
  std::string activeVideoUserId_;
};

class MeetingRecordingEventSink final : public ZOOMSDK::IMeetingRecordingCtrlEvent {
 public:
  void onRecordingStatus(ZOOMSDK::RecordingStatus status) override { localStatus_ = recordingStatusName(status); }
  void onCloudRecordingStatus(ZOOMSDK::RecordingStatus status) override { cloudStatus_ = recordingStatusName(status); }
  void onRecordPrivilegeChanged(bool canRecord) override { canRecord_ = canRecord; }
  void onLocalRecordingPrivilegeRequestStatus(ZOOMSDK::RequestLocalRecordingStatus) override {}
  void onRequestCloudRecordingResponse(ZOOMSDK::RequestStartCloudRecordingStatus) override {}
  void onLocalRecordingPrivilegeRequested(ZOOMSDK::IRequestLocalRecordingPrivilegeHandler*) override {}
  void onStartCloudRecordingRequested(ZOOMSDK::IRequestStartCloudRecordingHandler*) override {}
#if defined(WIN32)
  void onRecording2MP4Done(bool, int, const zchar_t*) override {}
  void onRecording2MP4Processing(int) override {}
  void onCustomizedLocalRecordingSourceNotification(ZOOMSDK::ICustomizedLocalRecordingLayoutHelper*) override {}
#endif
  void onCloudRecordingStorageFull(time_t) override { cloudStatus_ = "failed"; }
  void onEnableAndStartSmartRecordingRequested(ZOOMSDK::IRequestEnableAndStartSmartRecordingHandler*) override {}
  void onSmartRecordingEnableActionCallback(ZOOMSDK::ISmartRecordingEnableActionHandler*) override {}

  std::string status() const { return localStatus_; }
  bool canRecord() const { return canRecord_; }

 private:
  std::string localStatus_ = "idle";
  std::string cloudStatus_ = "idle";
  bool canRecord_ = false;
};

class ZoomMeetingSdkCaptureSource final : public IZoomMeetingSdkCaptureSource {
 public:
  explicit ZoomMeetingSdkCaptureSource(ZoomMeetingSdkRuntimeConfig config)
      : config_(std::move(config)),
        webDomain_(widenAscii(config_.webDomain)),
        brandingName_(L"CoreVideo Pro"),
        startedAt_(std::chrono::steady_clock::now()) {}

  ~ZoomMeetingSdkCaptureSource() override { shutdownSdk(); }

  bool join(const ZoomMeetingSdkJoinRequest& request) override {
    meetingNumber_ = request.meetingNumber;
    displayName_ = request.displayName;
    joinReturnCode_.clear();
    if (!runtimeReady()) {
      warnings_.push_back("Zoom Meeting SDK adapter is not ready; check runtime/auth/raw media readiness before joining.");
      meetingState_ = "error";
      return false;
    }

    const auto meetingNumber = parseMeetingNumber(request.meetingNumber);
    if (!meetingNumber) {
      warnings_.push_back("Zoom Meeting SDK join request has an invalid meeting number.");
      meetingState_ = "error";
      return false;
    }

    if (!hasJoinCredential(request)) {
      warnings_.push_back("Zoom Meeting SDK join credentials are missing; not calling Join.");
      meetingState_ = "join-ready";
      return false;
    }

    if (!ensureSdkInitialized()) {
      meetingState_ = "error";
      return false;
    }

    const auto joinResult = joinWithoutLogin(request, *meetingNumber);
    joinReturnCode_ = sdkErrorName(joinResult);
    if (joinResult != ZOOMSDK::SDKERR_SUCCESS) {
      warnings_.push_back("Zoom Meeting SDK Join returned " + joinReturnCode_ + ".");
      meetingState_ = "error";
      joined_ = false;
      return false;
    }

    meetingState_ = "joining";
    joined_ = true;
    return true;
  }

  void leave() override {
    if (meetingService_) {
      const auto error = meetingService_->Leave(ZOOMSDK::LEAVE_MEETING);
      if (error != ZOOMSDK::SDKERR_SUCCESS && error != ZOOMSDK::SDKERR_UNINITIALIZE && error != ZOOMSDK::SDKERR_NOT_IN_MEETING) {
        warnings_.push_back("Zoom Meeting SDK Leave returned " + sdkErrorName(error) + ".");
      }
    }
    joined_ = false;
    (void)stopRecordingProof();
    activeSpeakerId_.clear();
    subscriptions_.clear();
    subscriptionStates_.clear();
    subscriptionTiming_.clear();
    lastPolledVideoFrameCounts_.clear();
    lastPolledAudioPacketCounts_.clear();
    destroyRenderers();
    if (audioHelper_) {
      (void)audioHelper_->unSubscribe();
      audioHelper_ = nullptr;
    }
    meetingState_ = "idle";
  }

  void syncSubscriptions(const std::vector<ZoomMeetingSdkSubscriptionRequest>& requests) override {
    subscriptions_ = requests;
    subscriptionStates_.clear();
    subscriptionTiming_.clear();
    lastPolledVideoFrameCounts_.clear();
    lastPolledAudioPacketCounts_.clear();
    destroyRenderers();
    if (audioHelper_) {
      (void)audioHelper_->unSubscribe();
      audioHelper_ = nullptr;
    }

    for (const auto& request : requests) {
      subscriptionStates_.push_back(subscribeOne(request));
    }
  }

  std::vector<VideoFrame> pollVideoFrames() override {
    if (!joined_) {
      return {};
    }

    std::vector<VideoFrame> frames;
    for (const auto& state : subscriptionStates()) {
      const auto& request = state.request;
      if (state.status == "failed" || (request.kind != "participant-video" && request.kind != "screen-share")) {
        continue;
      }

      const auto subscriptionId = subscriptionIdFor(request);
      const auto previousCount = lastPolledVideoFrameCounts_[subscriptionId];
      if (state.framesReceived > previousCount) {
        recordFirstFrameTiming(subscriptionId, state.framesReceived == 1);
        VideoFrame frame;
        frame.participantId = request.participantId;
        frame.width = videoWidthFor(request.kind);
        frame.height = videoHeightFor(request.kind);
        frame.naturalWidth = frame.width;
        frame.naturalHeight = frame.height;
        frame.timestampMs = ++timestampMs_;
        frames.push_back(std::move(frame));
        lastPolledVideoFrameCounts_[subscriptionId] = state.framesReceived;
      }
    }
    return frames;
  }

  std::vector<AudioFrame> pollAudioFrames() override {
    if (!joined_) {
      return {};
    }

    std::vector<AudioFrame> frames;
    for (const auto& state : subscriptionStates()) {
      const auto& request = state.request;
      if (state.status == "failed" || request.kind != "participant-audio") {
        continue;
      }

      const auto subscriptionId = subscriptionIdFor(request);
      const auto previousCount = lastPolledAudioPacketCounts_[subscriptionId];
      if (state.audioPacketsReceived > previousCount) {
        frames.push_back({request.participantId, 48000, 1, ++timestampMs_});
        lastPolledAudioPacketCounts_[subscriptionId] = state.audioPacketsReceived;
      }
    }
    return frames;
  }

  std::string activeSpeakerId() const override {
    if (videoEvent_) {
      const auto activeSpeaker = videoEvent_->activeSpeakerId();
      if (!activeSpeaker.empty()) {
        return activeSpeaker;
      }
    }
    return activeSpeakerId_;
  }
  std::string meetingState() const override {
    if (meetingEvent_) {
      const auto sdkStatus = meetingEvent_->status();
      if (sdkStatus != ZOOMSDK::MEETING_STATUS_IDLE) {
        return meetingStatusName(sdkStatus);
      }
    }
    return meetingState_;
  }

  std::vector<ZoomMeetingSdkParticipant> participants() const override {
    if (!participantsController_) {
      return {};
    }

    auto* participantIds = participantsController_->GetParticipantsList();
    if (!participantIds) {
      return {};
    }

    std::vector<ZoomMeetingSdkParticipant> roster;
    const int count = std::max(0, participantIds->GetCount());
    roster.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
      const auto userId = participantIds->GetItem(index);
      auto* user = participantsController_->GetUserByUserID(userId);
      if (!user) {
        continue;
      }

      ZoomMeetingSdkParticipant participant;
      participant.sdkUserId = std::to_string(user->GetUserID());
      participant.displayName = narrowZchar(user->GetUserName());
      if (participant.displayName.empty()) {
        participant.displayName = "Zoom User " + participant.sdkUserId;
      }
      participant.role = userRoleName(user->GetUserRole());
      participant.videoOn = user->IsVideoOn();
      participant.muted = user->IsAudioMuted();
      participant.talking = user->IsTalking();
      participant.hasCamera = user->HasCamera();
      participant.audioLevel = clampAudioLevel(user->GetAudioVoiceLevel());
      roster.push_back(std::move(participant));
    }
    return roster;
  }

  std::vector<ZoomMeetingSdkSubscriptionState> subscriptionStates() const override {
    auto states = subscriptionStates_;
    for (auto& state : states) {
      if (state.status != "subscribed") {
        continue;
      }

      if (state.request.kind == "participant-audio") {
        const auto sdkUserId = parseSdkUserId(state.request.participantId);
        state.audioPacketsReceived = rawAudioDelegate_ ? rawAudioDelegate_->packetCountFor(sdkUserId.value_or(0)) : 0;
        if (state.audioPacketsReceived == 0) {
          state.status = "degraded";
          state.warning = "Zoom Meeting SDK audio subscription is active but no raw audio packets have arrived yet.";
        }
      } else {
        const auto renderer = std::find_if(renderers_.begin(), renderers_.end(), [&](const RendererSubscription& candidate) {
          return candidate.subscriptionId == subscriptionIdFor(state.request);
        });
        const auto previousFrames = state.framesReceived;
        state.framesReceived = renderer != renderers_.end() && renderer->delegate ? renderer->delegate->framesReceived() : 0;
        if (state.framesReceived > 0) {
          const auto subscriptionId = subscriptionIdFor(state.request);
          const auto observedAtMs = runtimeElapsedMs();
          auto timing = subscriptionTiming_.find(subscriptionId);
          if (timing == subscriptionTiming_.end()) {
            subscriptionTiming_[subscriptionId] = {observedAtMs, observedAtMs};
            state.firstFrameAtMs = observedAtMs;
            state.firstFrameDelayMs = observedAtMs;
            state.lastFrameAtMs = observedAtMs;
          } else {
            state.firstFrameAtMs = timing->second.first;
            state.firstFrameDelayMs = timing->second.first;
            if (state.framesReceived > previousFrames) {
              timing->second.second = observedAtMs;
              state.lastFrameAtMs = observedAtMs;
            } else {
              state.lastFrameAtMs = timing->second.second;
            }
          }
        }
        if (state.framesReceived == 0) {
          state.status = "degraded";
          state.warning = "Zoom Meeting SDK renderer subscription is active but no raw video frames have arrived yet.";
        }
      }
    }
    return states;
  }

  bool startRecordingProof() override {
    if (!canSubscribeRawMedia()) {
      recordingProof_.active = false;
      recordingProof_.status = "failed";
      recordingProof_.lastResultCode = "not-in-meeting";
      recordingProof_.warning = "Zoom raw recording proof cannot start until meeting status is in-meeting.";
      return false;
    }

    if (!recordingController_) {
      recordingProof_.active = false;
      recordingProof_.status = "failed";
      recordingProof_.lastResultCode = "recording-controller-unavailable";
      recordingProof_.warning = "Zoom Meeting SDK recording controller is unavailable.";
      return false;
    }

    const auto canStartRaw = recordingController_->CanStartRawRecording();
    if (canStartRaw != ZOOMSDK::SDKERR_SUCCESS) {
      recordingProof_.active = false;
      recordingProof_.status = "failed";
      recordingProof_.lastResultCode = sdkErrorName(canStartRaw);
      recordingProof_.warning = "Zoom Meeting SDK CanStartRawRecording returned " + recordingProof_.lastResultCode + ".";
      return false;
    }

    const auto rawRecordingResult = recordingController_->StartRawRecording();
    if (rawRecordingResult != ZOOMSDK::SDKERR_SUCCESS) {
      recordingProof_.active = false;
      recordingProof_.status = "failed";
      recordingProof_.lastResultCode = sdkErrorName(rawRecordingResult);
      recordingProof_.warning = "Zoom Meeting SDK StartRawRecording returned " + recordingProof_.lastResultCode + ".";
      return false;
    }

    recordingProof_.rawRecordingActive = true;
    recordingProof_.rawArchivingActive = false;
    if (rawArchivingController_) {
      const auto rawArchivingResult = rawArchivingController_->StartRawArchiving();
      if (rawArchivingResult == ZOOMSDK::SDKERR_SUCCESS) {
        recordingProof_.rawArchivingActive = true;
      } else {
        recordingProof_.warning = "Zoom Meeting SDK StartRawArchiving returned " + sdkErrorName(rawArchivingResult) + ".";
      }
    }

    recordingProof_.active = true;
    recordingProof_.status = recordingProof_.warning.empty() ? "recording" : "warning";
    recordingProof_.lastResultCode = "ok";
    recordingProof_.startedAtUnixSeconds = static_cast<int64_t>(std::time(nullptr));
    recordingProof_.stoppedAtUnixSeconds = 0;
    return true;
  }

  bool stopRecordingProof() override {
    bool stoppedCleanly = true;
    if (recordingProof_.rawArchivingActive && rawArchivingController_) {
      const auto error = rawArchivingController_->StopRawArchiving();
      stoppedCleanly = stoppedCleanly && error == ZOOMSDK::SDKERR_SUCCESS;
      if (error != ZOOMSDK::SDKERR_SUCCESS) {
        recordingProof_.warning = "Zoom Meeting SDK StopRawArchiving returned " + sdkErrorName(error) + ".";
        recordingProof_.lastResultCode = sdkErrorName(error);
      }
    }

    if (recordingProof_.rawRecordingActive && recordingController_) {
      const auto error = recordingController_->StopRawRecording();
      stoppedCleanly = stoppedCleanly && error == ZOOMSDK::SDKERR_SUCCESS;
      if (error != ZOOMSDK::SDKERR_SUCCESS) {
        recordingProof_.warning = "Zoom Meeting SDK StopRawRecording returned " + sdkErrorName(error) + ".";
        recordingProof_.lastResultCode = sdkErrorName(error);
      }
    }

    recordingProof_.active = false;
    recordingProof_.rawRecordingActive = false;
    recordingProof_.rawArchivingActive = false;
    recordingProof_.status = stoppedCleanly ? "stopped" : "warning";
    recordingProof_.stoppedAtUnixSeconds = static_cast<int64_t>(std::time(nullptr));
    if (stoppedCleanly) {
      recordingProof_.lastResultCode = "ok";
    }
    return stoppedCleanly;
  }

  ZoomMeetingSdkRecordingProof recordingProof() const override {
    auto proof = recordingProof_;
    if (recordingEvent_) {
      const auto eventStatus = recordingEvent_->status();
      if (proof.active && eventStatus != "idle") {
        proof.status = eventStatus;
      }
    }
    return proof;
  }

  std::vector<std::string> warnings() const override { return warnings_; }

 private:
  struct RendererSubscription {
    std::string subscriptionId;
    ZOOMSDK::IZoomSDKRenderer* renderer = nullptr;
    std::unique_ptr<RawVideoDelegate> delegate;
  };

  static std::string subscriptionIdFor(const ZoomMeetingSdkSubscriptionRequest& request) {
    return request.kind + ":" + request.participantId + ":" + request.purpose;
  }

  bool runtimeReady() const {
    return config_.appKeyPresent && config_.oauthConfigured && config_.jwtBrokerConfigured &&
           config_.rawVideoEnabled && config_.rawAudioEnabled && config_.rawShareEnabled;
  }

  bool ensureSdkInitialized() {
    if (sdkInitialized_) {
      return true;
    }

    ZOOMSDK::InitParam initParam;
    initParam.strWebDomain = webDomain_.c_str();
    initParam.strBrandingName = brandingName_.c_str();
    initParam.emLanguageID = ZOOMSDK::LANGUAGE_English;
    initParam.enableGenerateDump = true;
    initParam.enableLogByDefault = true;
    initParam.rawdataOpts.enableRawdataIntermediateMode = false;
    initParam.rawdataOpts.videoRawdataMemoryMode = ZOOMSDK::ZoomSDKRawDataMemoryModeHeap;
    initParam.rawdataOpts.shareRawdataMemoryMode = ZOOMSDK::ZoomSDKRawDataMemoryModeHeap;
    initParam.rawdataOpts.audioRawdataMemoryMode = ZOOMSDK::ZoomSDKRawDataMemoryModeHeap;

    auto error = ZOOMSDK::InitSDK(initParam);
    if (error != ZOOMSDK::SDKERR_SUCCESS) {
      warnings_.push_back("Zoom Meeting SDK InitSDK returned " + sdkErrorName(error) + ".");
      return false;
    }
    sdkInitialized_ = true;

    error = ZOOMSDK::CreateMeetingService(&meetingService_);
    if (error != ZOOMSDK::SDKERR_SUCCESS || !meetingService_) {
      warnings_.push_back("Zoom Meeting SDK CreateMeetingService returned " + sdkErrorName(error) + ".");
      shutdownSdk();
      return false;
    }

    meetingEvent_ = std::make_unique<MeetingServiceEventSink>();
    error = meetingService_->SetEvent(meetingEvent_.get());
    if (error != ZOOMSDK::SDKERR_SUCCESS) {
      warnings_.push_back("Zoom Meeting SDK SetEvent returned " + sdkErrorName(error) + ".");
      shutdownSdk();
      return false;
    }

    rawVideoDelegate_ = std::make_unique<RawVideoDelegate>();
    rawAudioDelegate_ = std::make_unique<RawAudioDelegate>();
    participantsController_ = meetingService_->GetMeetingParticipantsController();
    if (!participantsController_) {
      warnings_.push_back("Zoom Meeting SDK participants controller is unavailable; roster will remain empty.");
    }

    recordingController_ = meetingService_->GetMeetingRecordingController();
    if (recordingController_) {
      recordingEvent_ = std::make_unique<MeetingRecordingEventSink>();
      error = recordingController_->SetEvent(recordingEvent_.get());
      if (error != ZOOMSDK::SDKERR_SUCCESS) {
        warnings_.push_back("Zoom Meeting SDK recording controller SetEvent returned " + sdkErrorName(error) + ".");
        recordingEvent_.reset();
        recordingController_ = nullptr;
      }
    } else {
      warnings_.push_back("Zoom Meeting SDK recording controller is unavailable; raw recording proof cannot start.");
    }

    rawArchivingController_ = meetingService_->GetMeetingRawArchivingController();
    if (!rawArchivingController_) {
      warnings_.push_back("Zoom Meeting SDK raw archiving controller is unavailable; raw recording proof will use raw recording only.");
    }

    videoController_ = meetingService_->GetMeetingVideoController();
    if (videoController_) {
      videoEvent_ = std::make_unique<MeetingVideoEventSink>();
      error = videoController_->SetEvent(videoEvent_.get());
      if (error != ZOOMSDK::SDKERR_SUCCESS) {
        warnings_.push_back("Zoom Meeting SDK video controller SetEvent returned " + sdkErrorName(error) + ".");
        videoEvent_.reset();
        videoController_ = nullptr;
      }
    } else {
      warnings_.push_back("Zoom Meeting SDK video controller is unavailable; active-speaker routing will remain idle.");
    }
    return true;
  }

  bool canSubscribeRawMedia() const {
    if (meetingEvent_ && meetingEvent_->status() == ZOOMSDK::MEETING_STATUS_INMEETING) {
      return true;
    }
    return meetingState_ == "in-meeting";
  }

  ZoomMeetingSdkSubscriptionState subscribeOne(const ZoomMeetingSdkSubscriptionRequest& request) {
    ZoomMeetingSdkSubscriptionState state;
    state.request = request;
    if (!canSubscribeRawMedia()) {
      state.status = "failed";
      state.lastResultCode = "not-in-meeting";
      state.warning = "Raw media subscription deferred until Zoom meeting status is in-meeting.";
      return state;
    }

    const auto sdkUserId = parseSdkUserId(request.participantId);
    if (!sdkUserId) {
      state.status = "failed";
      state.lastResultCode = "participant-id-not-numeric";
      state.warning = "Zoom Meeting SDK raw media requires a numeric SDK user ID.";
      return state;
    }

    if (request.kind == "participant-audio") {
      return subscribeAudio(request, *sdkUserId);
    }
    if (request.kind == "participant-video" || request.kind == "screen-share") {
      return subscribeRenderer(request, *sdkUserId);
    }

    state.status = "failed";
    state.lastResultCode = "unsupported-kind";
    state.warning = "Unsupported Zoom raw media subscription kind.";
    return state;
  }

  ZoomMeetingSdkSubscriptionState subscribeRenderer(const ZoomMeetingSdkSubscriptionRequest& request, uint32_t sdkUserId) {
    ZoomMeetingSdkSubscriptionState state;
    state.request = request;
    auto delegate = std::make_unique<RawVideoDelegate>();
    ZOOMSDK::IZoomSDKRenderer* renderer = nullptr;
    auto error = ZOOMSDK::createRenderer(&renderer, delegate.get());
    if (error != ZOOMSDK::SDKERR_SUCCESS || !renderer) {
      state.status = "failed";
      state.lastResultCode = sdkErrorName(error);
      state.warning = "Zoom Meeting SDK createRenderer returned " + state.lastResultCode + ".";
      return state;
    }

    const auto rawType = request.kind == "screen-share" ? ZOOMSDK::RAW_DATA_TYPE_SHARE : ZOOMSDK::RAW_DATA_TYPE_VIDEO;
    (void)renderer->setRawDataResolution(request.kind == "screen-share" ? ZOOMSDK::ZoomSDKResolution_1080P : ZOOMSDK::ZoomSDKResolution_720P);
    error = renderer->subscribe(sdkUserId, rawType);
    if (error != ZOOMSDK::SDKERR_SUCCESS) {
      (void)ZOOMSDK::destroyRenderer(renderer);
      state.status = "failed";
      state.lastResultCode = sdkErrorName(error);
      state.warning = "Zoom Meeting SDK renderer subscribe returned " + state.lastResultCode + ".";
      return state;
    }

    state.status = "subscribed";
    state.lastResultCode = "ok";
    renderers_.push_back({subscriptionIdFor(request), renderer, std::move(delegate)});
    return state;
  }

  ZoomMeetingSdkSubscriptionState subscribeAudio(const ZoomMeetingSdkSubscriptionRequest& request, uint32_t) {
    ZoomMeetingSdkSubscriptionState state;
    state.request = request;
    if (!audioHelper_) {
      audioHelper_ = ZOOMSDK::GetAudioRawdataHelper();
    }
    if (!audioHelper_) {
      state.status = "failed";
      state.lastResultCode = "audio-helper-unavailable";
      state.warning = "Zoom Meeting SDK audio raw data helper is unavailable.";
      return state;
    }

    auto error = audioHelper_->subscribe(rawAudioDelegate_.get(), false);
    if (error != ZOOMSDK::SDKERR_SUCCESS) {
      state.status = "failed";
      state.lastResultCode = sdkErrorName(error);
      state.warning = "Zoom Meeting SDK audio subscribe returned " + state.lastResultCode + ".";
      return state;
    }

    state.status = "subscribed";
    state.lastResultCode = "ok";
    return state;
  }

  void destroyRenderers() {
    for (auto& renderer : renderers_) {
      if (renderer.renderer) {
        (void)renderer.renderer->unSubscribe();
        (void)ZOOMSDK::destroyRenderer(renderer.renderer);
        renderer.renderer = nullptr;
      }
    }
    renderers_.clear();
  }

  ZOOMSDK::SDKError joinWithoutLogin(const ZoomMeetingSdkJoinRequest& request, UINT64 meetingNumber) {
    if (!meetingService_) {
      return ZOOMSDK::SDKERR_UNINITIALIZE;
    }

    const std::wstring displayName = widenAscii(request.displayName.empty() ? "CoreVideo Pro" : request.displayName);
    const std::wstring passcode = widenAscii(request.passcode);
    const std::wstring zakToken = widenAscii(request.zakToken);
    const std::wstring appPrivilegeToken = widenAscii(request.appPrivilegeToken);
    const std::wstring joinToken = widenAscii(request.joinToken);
    const std::wstring webinarToken = widenAscii(request.webinarToken);
    const std::wstring customerKey = widenAscii(request.customerKey);

    ZOOMSDK::JoinParam joinParam;
    joinParam.userType = ZOOMSDK::SDK_UT_WITHOUT_LOGIN;
    auto& withoutLogin = joinParam.param.withoutloginuserJoin;
    withoutLogin.meetingNumber = meetingNumber;
    withoutLogin.userName = displayName.c_str();
    withoutLogin.psw = passcode.empty() ? nullptr : passcode.c_str();
    withoutLogin.userZAK = zakToken.empty() ? nullptr : zakToken.c_str();
    withoutLogin.app_privilege_token = appPrivilegeToken.empty() ? nullptr : appPrivilegeToken.c_str();
    withoutLogin.join_token = joinToken.empty() ? nullptr : joinToken.c_str();
    withoutLogin.webinarToken = webinarToken.empty() ? nullptr : webinarToken.c_str();
    withoutLogin.customer_key = customerKey.empty() ? nullptr : customerKey.c_str();
    withoutLogin.isVideoOff = request.startVideoOff;
    withoutLogin.isAudioOff = request.startAudioMuted;
    withoutLogin.isMyVoiceInMix = true;
    withoutLogin.isAudioRawDataStereo = false;
    withoutLogin.eAudioRawdataSamplingRate = ZOOMSDK::AudioRawdataSamplingRate_48K;
    withoutLogin.eVideoRawdataColorspace = ZOOMSDK::VideoRawdataColorspace_BT709_F;

    return meetingService_->Join(joinParam);
  }

  double runtimeElapsedMs() const {
    return static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt_).count());
  }

  void recordFirstFrameTiming(const std::string& subscriptionId, bool isFirstFrame) {
    const auto observedAtMs = runtimeElapsedMs();
    auto timing = subscriptionTiming_.find(subscriptionId);
    if (timing == subscriptionTiming_.end()) {
      subscriptionTiming_[subscriptionId] = {observedAtMs, observedAtMs};
      return;
    }
    if (isFirstFrame && timing->second.first < 0.0) {
      timing->second = {observedAtMs, observedAtMs};
      return;
    }
    timing->second.second = observedAtMs;
  }

  void shutdownSdk() {
    destroyRenderers();
    if (audioHelper_) {
      (void)audioHelper_->unSubscribe();
      audioHelper_ = nullptr;
    }
    if (meetingService_) {
      if (videoController_) {
        (void)videoController_->SetEvent(nullptr);
        videoController_ = nullptr;
      }
      if (recordingController_) {
        (void)recordingController_->SetEvent(nullptr);
        recordingController_ = nullptr;
      }
      rawArchivingController_ = nullptr;
      participantsController_ = nullptr;
      (void)meetingService_->SetEvent(nullptr);
      (void)ZOOMSDK::DestroyMeetingService(meetingService_);
      meetingService_ = nullptr;
    }
    meetingEvent_.reset();
    videoEvent_.reset();
    rawVideoDelegate_.reset();
    rawAudioDelegate_.reset();
    if (sdkInitialized_) {
      (void)ZOOMSDK::CleanUPSDK();
      sdkInitialized_ = false;
    }
  }

  ZoomMeetingSdkRuntimeConfig config_;
  std::chrono::steady_clock::time_point startedAt_;
  bool sdkInitialized_ = false;
  bool joined_ = false;
  int64_t timestampMs_ = 0;
  std::string meetingNumber_;
  std::string displayName_;
  std::string activeSpeakerId_;
  std::string meetingState_ = "idle";
  std::string joinReturnCode_;
  ZoomMeetingSdkRecordingProof recordingProof_;
  std::wstring webDomain_;
  std::wstring brandingName_;
  ZOOMSDK::IMeetingService* meetingService_ = nullptr;
  ZOOMSDK::IMeetingParticipantsController* participantsController_ = nullptr;
  ZOOMSDK::IMeetingVideoController* videoController_ = nullptr;
  ZOOMSDK::IMeetingRecordingController* recordingController_ = nullptr;
  ZOOMSDK::IMeetingRawArchivingController* rawArchivingController_ = nullptr;
  ZOOMSDK::IZoomSDKAudioRawDataHelper* audioHelper_ = nullptr;
  std::unique_ptr<MeetingServiceEventSink> meetingEvent_;
  std::unique_ptr<MeetingVideoEventSink> videoEvent_;
  std::unique_ptr<MeetingRecordingEventSink> recordingEvent_;
  std::unique_ptr<RawVideoDelegate> rawVideoDelegate_;
  std::unique_ptr<RawAudioDelegate> rawAudioDelegate_;
  std::vector<RendererSubscription> renderers_;
  std::vector<ZoomMeetingSdkSubscriptionRequest> subscriptions_;
  std::vector<ZoomMeetingSdkSubscriptionState> subscriptionStates_;
  mutable std::map<std::string, std::pair<double, double>> subscriptionTiming_;
  std::map<std::string, int64_t> lastPolledVideoFrameCounts_;
  std::map<std::string, int64_t> lastPolledAudioPacketCounts_;
  std::vector<std::string> warnings_;
};

}  // namespace

std::unique_ptr<IZoomMeetingSdkCaptureSource> createZoomMeetingSdkCaptureSource(ZoomMeetingSdkRuntimeConfig config) {
  return std::make_unique<ZoomMeetingSdkCaptureSource>(std::move(config));
}

}  // namespace corevideo::modules

#else

namespace corevideo::modules {

std::unique_ptr<IZoomMeetingSdkCaptureSource> createZoomMeetingSdkCaptureSource(ZoomMeetingSdkRuntimeConfig) {
  return nullptr;
}

}  // namespace corevideo::modules

#endif
