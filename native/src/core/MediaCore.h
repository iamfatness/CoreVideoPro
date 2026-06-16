#pragma once

#include "modules/Interfaces.h"
#include "modules/ZoomEngineRuntime.h"
#include "rpc/Json.h"

#include <memory>
#include <string>
#include <vector>

namespace corevideo::core {

class MediaCore {
 public:
  explicit MediaCore(modules::ModuleSet modules = modules::createDefaultModules());

  [[nodiscard]] rpc::Json profile() const;
  [[nodiscard]] rpc::Json sessionState() const;
  [[nodiscard]] rpc::Json health() const;
  [[nodiscard]] rpc::Json captureDevices() const;
  [[nodiscard]] rpc::Json selectCaptureInput(const std::string& deviceId, const std::string& inputId);
  [[nodiscard]] rpc::Json setCaptureAudioSyncOffset(const std::string& deviceId, int offsetMs);
  [[nodiscard]] rpc::Json connectCaptureDevice(const std::string& deviceId);
  [[nodiscard]] rpc::Json joinZoom(const rpc::Json& payload);
  [[nodiscard]] rpc::Json leaveZoom();
  [[nodiscard]] rpc::Json zoomSnapshot() const;
  [[nodiscard]] rpc::Json syncZoomMediaSpine(const rpc::Json& payload, double elapsedMs);
  [[nodiscard]] std::vector<rpc::Json> drainZoomVideoFrameEvents();
  [[nodiscard]] std::vector<rpc::Json> drainProgramFramePreviewEvents();
  [[nodiscard]] std::vector<rpc::Json> drainProgramSharedTextureEvents();
  [[nodiscard]] rpc::Json applyCommand(const rpc::Json& command);
  [[nodiscard]] rpc::Json applyCommands(const rpc::Json::Array& commands);

 private:
  void loadSceneGraph(const rpc::Json& command);
  void setParticipantTransform(const rpc::Json& command);
  void setOverlayAsset(const rpc::Json& command);
  void startProgramOutput(const rpc::Json& command);
  void prepareEncoderSession(const rpc::Json& command);
  void startEncoderSession(const rpc::Json& command);
  void stopEncoderSession(const rpc::Json& command);
  void failOutputSender(const rpc::Json& command);
  void recoverOutputSender(const rpc::Json& command);
  void setRecordingTargets(const rpc::Json& command);
  void startRecordingSession(const rpc::Json& command);
  void stopRecordingSession(const rpc::Json& command);
  void failRecordingSession(const rpc::Json& command);
  void recoverRecordingSession(const rpc::Json& command);
  void syncParticipantAudioMix(const rpc::Json& command);
  void pushCaptionCue(const rpc::Json& command);
  void setCaptionEnabled(const rpc::Json& command);
  void setBrandKit(const rpc::Json& command);
  void simulateBreakoutRoomChange(const rpc::Json& command);
  void renderSyntheticTick();
  void enqueueProgramFramePreviewEvent();
  void enqueueProgramSharedTextureEvent();
  [[nodiscard]] rpc::Json encoderSessionState(const modules::OutputSession& session) const;
  [[nodiscard]] rpc::Json audioMixSessionState() const;
  [[nodiscard]] rpc::Json captionTrackState() const;
  [[nodiscard]] rpc::Json brandKitState() const;
  [[nodiscard]] rpc::Json outputSenderSessionState() const;
  [[nodiscard]] rpc::Json captureDevicesState() const;
  [[nodiscard]] rpc::Json recordingState(const modules::OutputSession& session) const;
  [[nodiscard]] std::string resolveMeetingStateForSession() const;

  struct SceneRouteState {
    std::string routeId;
    std::string mode;
    std::string participantId;
    std::string audioRole;
  };

  [[nodiscard]] modules::CompositorRenderPlan buildCompositorRenderPlan(const std::vector<modules::VideoFrame>& videoFrames) const;

  modules::ModuleSet modules_;
  std::string sceneId_ = "unloaded";
  std::vector<SceneRouteState> sceneRoutes_;
  int routeCount_ = 0;
  int transformCount_ = 0;
  int overlayCount_ = 0;
  int64_t mixedAudioFrameCount_ = 0;
  modules::ProgramFrame lastProgramFrame_;
  std::string encoderLifecycleStatus_ = "idle";
  std::string encoderLastTransition_ = "Encoder session idle.";
  double encoderPreparedAtMs_ = 0;
  double encoderStartedAtMs_ = 0;
  double encoderStoppedAtMs_ = 0;
  std::string recordingSessionId_;
  std::string recordingStatus_ = "stopped";
  std::string recordingWriterStatus_ = "stopped";
  std::string recordingTargetFolder_ = "Recordings/CoreVideo Pro/native-core";
  std::string recordingFilenamePrefix_ = "program";
  std::string recordingFormat_ = "mp4";
  std::string recordingQuality_ = "high";
  std::vector<std::string> recordingIsoParticipantIds_;
  double recordingStartedAtMs_ = 0;
  double recordingElapsedMs_ = 0;
  int64_t recordingProgramFramesWritten_ = 0;
  int64_t recordingIsoFramesWritten_ = 0;
  int64_t recordingDroppedFrames_ = 0;
  std::string recordingError_;
  std::string recordingWarning_;
  std::unique_ptr<modules::ZoomEngineRuntime> zoomEngineRuntime_;
  bool zoomJoined_ = false;
  mutable int zoomSnapshotTick_ = 0;
  std::string zoomDisplayName_ = "Guest Producer";
  std::string breakoutRoomId_ = "main";
  std::string breakoutRoomName_ = "Main room";
  struct ParticipantAudioChannelInput {
    std::string participantId;
    int inputLevel = 0;
    bool muted = false;
    bool noiseSuppression = false;
    double manualGainDb = 0;
    bool hasManualGain = false;
  };
  std::vector<ParticipantAudioChannelInput> audioChannels_;
  bool captionEnabled_ = true;
  std::string captionText_;
  std::string captionSpeaker_;
  double captionAtMs_ = 0;
  int captionConfidence_ = 0;
  std::vector<std::string> captionWarnings_;
  std::string brandName_ = "CoreVideo Pro House";
  std::string brandLogoText_ = "CoreVideo Pro";
  std::string brandColor_ = "#44c1a1";
  std::string brandAccentColor_ = "#f0a85c";
  std::string brandBackgroundColor_ = "#0c1118";
  std::string brandFontFamily_ = "Inter";
  std::string brandLowerThirdStyle_ = "gradient";
  std::vector<std::string> brandWarnings_;
  std::vector<rpc::Json> pendingProgramFramePreviewEvents_;
  std::vector<rpc::Json> pendingProgramSharedTextureEvents_;
};

}  // namespace corevideo::core
