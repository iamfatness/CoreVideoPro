#pragma once

#include "modules/Interfaces.h"
#include "rpc/Json.h"

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
  [[nodiscard]] rpc::Json zoomSnapshot();
  [[nodiscard]] rpc::Json syncZoomMediaSpine(const rpc::Json& payload, double elapsedMs) const;
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
  void renderSyntheticTick();
  [[nodiscard]] rpc::Json encoderSessionState(const modules::OutputSession& session) const;
  [[nodiscard]] rpc::Json outputSenderSessionState() const;
  [[nodiscard]] rpc::Json captureDevicesState() const;
  [[nodiscard]] rpc::Json recordingState(const modules::OutputSession& session) const;

  modules::ModuleSet modules_;
  std::string sceneId_ = "unloaded";
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
  bool zoomJoined_ = false;
  int zoomSnapshotTick_ = 0;
  std::string zoomDisplayName_ = "Guest Producer";
};

}  // namespace corevideo::core
