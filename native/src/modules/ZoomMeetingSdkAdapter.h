#pragma once

#include "modules/Interfaces.h"

#include <memory>
#include <string>
#include <vector>

namespace corevideo::modules {

struct ZoomMeetingSdkJoinRequest {
  std::string meetingNumber;
  std::string displayName;
  std::string passcode;
  std::string zakToken;
  std::string appPrivilegeToken;
  std::string joinToken;
  std::string webinarToken;
  std::string customerKey;
  bool passcodePresent = false;
  bool startVideoOff = true;
  bool startAudioMuted = false;
};

struct ZoomMeetingSdkSubscriptionRequest {
  std::string participantId;
  std::string kind;
  std::string purpose;
  int priority = 0;
};

struct ZoomMeetingSdkSubscriptionState {
  ZoomMeetingSdkSubscriptionRequest request;
  std::string status = "idle";
  std::string lastResultCode = "not-started";
  int64_t framesReceived = 0;
  int64_t audioPacketsReceived = 0;
  std::string warning;
};

struct ZoomMeetingSdkParticipant {
  std::string sdkUserId;
  std::string displayName;
  std::string role = "attendee";
  bool videoOn = false;
  bool muted = true;
  bool talking = false;
  bool hasCamera = false;
  int audioLevel = 0;
};

struct ZoomMeetingSdkRecordingProof {
  bool active = false;
  bool rawRecordingActive = false;
  bool rawArchivingActive = false;
  std::string status = "idle";
  std::string lastResultCode = "not-started";
  int64_t startedAtUnixSeconds = 0;
  int64_t stoppedAtUnixSeconds = 0;
  std::string warning;
};

struct ZoomMeetingSdkRuntimeConfig {
  std::string sdkRoot;
  std::string sdkVersion = "unknown";
  std::string webDomain = "https://zoom.us";
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
  virtual std::string meetingState() const = 0;
  virtual std::vector<ZoomMeetingSdkParticipant> participants() const = 0;
  virtual std::vector<ZoomMeetingSdkSubscriptionState> subscriptionStates() const = 0;
  virtual bool startRecordingProof() = 0;
  virtual bool stopRecordingProof() = 0;
  virtual ZoomMeetingSdkRecordingProof recordingProof() const = 0;
  virtual std::vector<std::string> warnings() const = 0;
};

std::unique_ptr<IZoomMeetingSdkCaptureSource> createZoomMeetingSdkCaptureSource(ZoomMeetingSdkRuntimeConfig config);

}  // namespace corevideo::modules
