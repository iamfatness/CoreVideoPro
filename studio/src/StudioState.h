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
  std::string recordingArtifactPath;

  std::string outputStatus;
  int activeSenderCount = 0;

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
[[nodiscard]] std::string summarizeStudioState(const StudioState& state);

}  // namespace corevideo::studio
