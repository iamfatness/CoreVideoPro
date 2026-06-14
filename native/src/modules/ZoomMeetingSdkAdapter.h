#pragma once

#include "modules/Interfaces.h"

#include <memory>
#include <string>
#include <vector>

namespace corevideo::modules {

struct ZoomMeetingSdkJoinRequest {
  std::string meetingNumber;
  std::string displayName;
  bool passcodePresent = false;
};

struct ZoomMeetingSdkSubscriptionRequest {
  std::string participantId;
  std::string kind;
  std::string purpose;
  int priority = 0;
};

struct ZoomMeetingSdkRuntimeConfig {
  std::string sdkRoot;
  std::string sdkVersion = "unknown";
  bool appKeyPresent = false;
  bool oauthConfigured = false;
  bool jwtBrokerConfigured = false;
  bool rawVideoEnabled = false;
  bool rawAudioEnabled = false;
  bool rawShareEnabled = false;
};

class IZoomMeetingSdkCaptureSource : public IZoomCaptureSource {
 public:
  ~IZoomMeetingSdkCaptureSource() override = default;
  virtual bool join(const ZoomMeetingSdkJoinRequest& request) = 0;
  virtual void leave() = 0;
  virtual void syncSubscriptions(const std::vector<ZoomMeetingSdkSubscriptionRequest>& requests) = 0;
  virtual std::string activeSpeakerId() const = 0;
  virtual std::vector<std::string> warnings() const = 0;
};

std::unique_ptr<IZoomMeetingSdkCaptureSource> createZoomMeetingSdkCaptureSource(ZoomMeetingSdkRuntimeConfig config);

}  // namespace corevideo::modules
