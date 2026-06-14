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

class ZoomMeetingSdkCaptureSource final : public IZoomMeetingSdkCaptureSource {
 public:
  explicit ZoomMeetingSdkCaptureSource(ZoomMeetingSdkRuntimeConfig config)
      : config_(std::move(config)), webDomain_(widenAscii(config_.webDomain)), brandingName_(L"CoreVideo Pro") {}

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
    activeSpeakerId_.clear();
    subscriptions_.clear();
    subscriptionStates_.clear();
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
      if (state.status != "subscribed" || (request.kind != "participant-video" && request.kind != "screen-share")) {
        continue;
      }

      const auto subscriptionId = subscriptionIdFor(request);
      const auto previousCount = lastPolledVideoFrameCounts_[subscriptionId];
      if (state.framesReceived > previousCount) {
        frames.push_back({request.participantId, videoWidthFor(request.kind), videoHeightFor(request.kind), ++timestampMs_});
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
      if (state.status != "subscribed" || request.kind != "participant-audio") {
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

  std::string activeSpeakerId() const override { return activeSpeakerId_; }
  std::string meetingState() const override {
    if (meetingEvent_) {
      const auto sdkStatus = meetingEvent_->status();
      if (sdkStatus != ZOOMSDK::MEETING_STATUS_IDLE) {
        return meetingStatusName(sdkStatus);
      }
    }
    return meetingState_;
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
      } else {
        const auto renderer = std::find_if(renderers_.begin(), renderers_.end(), [&](const RendererSubscription& candidate) {
          return candidate.subscriptionId == subscriptionIdFor(state.request);
        });
        state.framesReceived = renderer != renderers_.end() && renderer->delegate ? renderer->delegate->framesReceived() : 0;
      }
    }
    return states;
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

  void shutdownSdk() {
    destroyRenderers();
    if (audioHelper_) {
      (void)audioHelper_->unSubscribe();
      audioHelper_ = nullptr;
    }
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
  std::string joinReturnCode_;
  std::wstring webDomain_;
  std::wstring brandingName_;
  ZOOMSDK::IMeetingService* meetingService_ = nullptr;
  ZOOMSDK::IZoomSDKAudioRawDataHelper* audioHelper_ = nullptr;
  std::unique_ptr<MeetingServiceEventSink> meetingEvent_;
  std::unique_ptr<RawVideoDelegate> rawVideoDelegate_;
  std::unique_ptr<RawAudioDelegate> rawAudioDelegate_;
  std::vector<RendererSubscription> renderers_;
  std::vector<ZoomMeetingSdkSubscriptionRequest> subscriptions_;
  std::vector<ZoomMeetingSdkSubscriptionState> subscriptionStates_;
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
