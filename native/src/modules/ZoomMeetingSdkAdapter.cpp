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
#include <meeting_service_interface.h>
#include <zoom_sdk.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace corevideo::modules {
namespace {

std::wstring widenAscii(const std::string& value) {
  return std::wstring(value.begin(), value.end());
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

 private:
  uint32_t lastUserId_ = 0;
  int64_t mixedPackets_ = 0;
  int64_t oneWayPackets_ = 0;
  int64_t sharePackets_ = 0;
  int64_t interpreterPackets_ = 0;
};

class ZoomMeetingSdkCaptureSource final : public IZoomMeetingSdkCaptureSource {
 public:
  explicit ZoomMeetingSdkCaptureSource(ZoomMeetingSdkRuntimeConfig config)
      : config_(std::move(config)), webDomain_(widenAscii(config_.webDomain)), brandingName_(L"CoreVideo Pro") {}

  ~ZoomMeetingSdkCaptureSource() override { shutdownSdk(); }

  bool join(const ZoomMeetingSdkJoinRequest& request) override {
    meetingNumber_ = request.meetingNumber;
    displayName_ = request.displayName;
    if (!runtimeReady()) {
      warnings_.push_back("Zoom Meeting SDK adapter is not ready; check runtime/auth/raw media readiness before joining.");
      meetingState_ = "error";
      return false;
    }

    if (!ensureSdkInitialized()) {
      meetingState_ = "error";
      return false;
    }

    // REQUIRES DEV MACHINE: authentication and SDK JWT/ZAK handoff land before calling
    // IMeetingService::Join. For now the adapter owns initialized SDK state and service
    // callbacks, then advertises a join-pending state instead of silently faking a meeting.
    joined_ = false;
    meetingState_ = "join-ready";
    if (request.passcodePresent) {
      warnings_.push_back("Join request includes a passcode; passcode redacted until auth/join wiring is implemented.");
    }
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
    activeSpeakerId_.clear();
    subscriptions_.clear();
    meetingState_ = "idle";
    // REQUIRES DEV MACHINE: raw renderer/audio helper unsubscription is added with
    // the subscription implementation so leave/rejoin cannot duplicate callbacks.
  }

  void syncSubscriptions(const std::vector<ZoomMeetingSdkSubscriptionRequest>& requests) override {
    subscriptions_ = requests;
    // REQUIRES DEV MACHINE: map stable SDK user IDs to raw video/audio/share subscriptions.
  }

  std::vector<VideoFrame> pollVideoFrames() override {
    if (!joined_) {
      return {};
    }

    std::vector<VideoFrame> frames;
    for (const auto& subscription : subscriptions_) {
      if (subscription.kind == "participant-video" || subscription.kind == "screen-share") {
        frames.push_back({subscription.participantId, subscription.kind == "screen-share" ? 1920 : 1280, subscription.kind == "screen-share" ? 1080 : 720, ++timestampMs_});
      }
    }
    return frames;
  }

  std::vector<AudioFrame> pollAudioFrames() override {
    if (!joined_) {
      return {};
    }

    std::vector<AudioFrame> frames;
    for (const auto& subscription : subscriptions_) {
      if (subscription.kind == "participant-audio") {
        frames.push_back({subscription.participantId, 48000, 1, ++timestampMs_});
      }
    }
    return frames;
  }

  std::string activeSpeakerId() const override { return activeSpeakerId_; }
  std::string meetingState() const override {
    if (meetingState_ == "join-ready") {
      return meetingState_;
    }
    if (meetingEvent_) {
      return meetingStatusName(meetingEvent_->status());
    }
    return meetingState_;
  }
  std::vector<std::string> warnings() const override { return warnings_; }

 private:
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
    return true;
  }

  void shutdownSdk() {
    if (meetingService_) {
      (void)meetingService_->SetEvent(nullptr);
      (void)ZOOMSDK::DestroyMeetingService(meetingService_);
      meetingService_ = nullptr;
    }
    meetingEvent_.reset();
    rawVideoDelegate_.reset();
    rawAudioDelegate_.reset();
    if (sdkInitialized_) {
      (void)ZOOMSDK::CleanUPSDK();
      sdkInitialized_ = false;
    }
  }

  ZoomMeetingSdkRuntimeConfig config_;
  bool sdkInitialized_ = false;
  bool joined_ = false;
  int64_t timestampMs_ = 0;
  std::string meetingNumber_;
  std::string displayName_;
  std::string activeSpeakerId_;
  std::string meetingState_ = "idle";
  std::wstring webDomain_;
  std::wstring brandingName_;
  ZOOMSDK::IMeetingService* meetingService_ = nullptr;
  std::unique_ptr<MeetingServiceEventSink> meetingEvent_;
  std::unique_ptr<RawVideoDelegate> rawVideoDelegate_;
  std::unique_ptr<RawAudioDelegate> rawAudioDelegate_;
  std::vector<ZoomMeetingSdkSubscriptionRequest> subscriptions_;
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
