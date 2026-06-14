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
#include <utility>

namespace corevideo::modules {
namespace {

class ZoomMeetingSdkCaptureSource final : public IZoomMeetingSdkCaptureSource {
 public:
  explicit ZoomMeetingSdkCaptureSource(ZoomMeetingSdkRuntimeConfig config) : config_(std::move(config)) {}

  bool join(const ZoomMeetingSdkJoinRequest& request) override {
    meetingNumber_ = request.meetingNumber;
    displayName_ = request.displayName;
    joined_ = runtimeReady();
    if (!joined_) {
      warnings_.push_back("Zoom Meeting SDK adapter is not ready; check runtime/auth/raw media readiness before joining.");
    }
    // REQUIRES DEV MACHINE: initialize Zoom SDK, authenticate through OAuth/JWT broker,
    // join the meeting, and subscribe raw data callbacks here.
    return joined_;
  }

  void leave() override {
    joined_ = false;
    activeSpeakerId_.clear();
    subscriptions_.clear();
    // REQUIRES DEV MACHINE: leave meeting, clear callback registrations, and release SDK services.
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
  std::vector<std::string> warnings() const override { return warnings_; }

 private:
  bool runtimeReady() const {
    return config_.appKeyPresent && config_.oauthConfigured && config_.jwtBrokerConfigured &&
           config_.rawVideoEnabled && config_.rawAudioEnabled && config_.rawShareEnabled;
  }

  ZoomMeetingSdkRuntimeConfig config_;
  bool joined_ = false;
  int64_t timestampMs_ = 0;
  std::string meetingNumber_;
  std::string displayName_;
  std::string activeSpeakerId_;
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
