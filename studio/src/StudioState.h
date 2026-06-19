#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace corevideo::studio {

struct StudioState {
  bool coreRunning = false;
  bool connected = false;
  bool handshakeSeen = false;

  std::string renderer;
  std::string encoder;
  std::vector<std::string> capabilities;
  std::size_t capabilityCount = 0;

  std::string healthStatus;
  std::string programFrameHealth;
  std::uint64_t frameCount = 0;
  bool firstFrameSeen = false;
  std::uint64_t previewFrameNumber = 0;
  std::string recordingArtifactPath;
  std::string recordingHealth;
  std::uint64_t recordingBytesWritten = 0;
  std::uint64_t recordingDurationMs = 0;
  std::uint64_t recordingFrameCount = 0;
  bool recordingMetadataValid = false;

  std::string sceneId;
  int routeCount = 0;
  int overlayCount = 0;
  std::vector<std::string> outputDestinations;
  bool captionsEnabled = false;
  std::string captionStatus;

  std::string outputStatus;
  int activeSenderCount = 0;
  std::string encoderLifecycleStatus;

  std::string meetingState;
  std::string activeSpeakerId;
  std::vector<std::string> participantIds;
  std::vector<std::string> participantLines;

  std::string lastErrorText;
  std::string lastSummaryText;
};

[[nodiscard]] std::optional<std::string> extractJsonString(const std::string& json, const std::string& key);
[[nodiscard]] std::optional<double> extractJsonNumber(const std::string& json, const std::string& key);
[[nodiscard]] std::optional<bool> extractJsonBool(const std::string& json, const std::string& key);
[[nodiscard]] std::vector<std::string> extractJsonStringArray(const std::string& json, const std::string& key);
[[nodiscard]] std::optional<std::string> extractJsonObject(const std::string& json, const std::string& key);
[[nodiscard]] std::optional<std::string> extractJsonArray(const std::string& json, const std::string& key);
[[nodiscard]] std::vector<std::string> extractJsonObjectArray(const std::string& json, const std::string& key);

[[nodiscard]] StudioState parseStudioStateLine(const std::string& line);
void applyStudioStateLine(StudioState& state, const std::string& line);
void notePreviewFrame(StudioState& state, std::uint64_t frameNumber, const std::string& health);
[[nodiscard]] std::string summarizeStudioState(const StudioState& state);

}  // namespace corevideo::studio
