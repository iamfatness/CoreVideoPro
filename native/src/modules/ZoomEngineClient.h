#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace corevideo::modules {

struct ZoomEngineInitCommand {
  std::string jwt;
  std::string publicAppKey;
};

struct ZoomEngineJoinCommand {
  std::string meetingId;
  std::string passcode;
  std::string displayName = "CoreVideo Pro";
  std::string onBehalfToken;
  std::string userZak;
  std::string appPrivilegeToken;
};

struct ZoomEngineSubscribeCommand {
  std::string sourceUuid;
  std::uint32_t participantId = 0;
  std::uint32_t resolution = 1;
  std::string mode;
  bool isolateAudio = false;
  bool audienceAudio = false;
};

enum class ZoomEngineEventKind {
  Unknown,
  Ping,
  Debug,
  Ready,
  AuthOk,
  AuthFail,
  Joined,
  Left,
  Participants,
  ActiveSpeaker,
  Frame,
  Audio,
  Error,
};

struct ZoomEngineEvent {
  ZoomEngineEventKind kind = ZoomEngineEventKind::Unknown;
  std::string command;
  std::string sourceUuid;
  std::string stage;
  std::string message;
  std::uint32_t participantId = 0;
  std::uint32_t activeSpeakerId = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t byteLength = 0;
  std::size_t participantCount = 0;
  bool ok = false;
};

struct ZoomEngineRgbaFrame {
  std::string sourceUuid;
  std::string participantId;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t frameId = 0;
  std::vector<std::uint8_t> rgba;
};

std::string buildZoomEngineInitCommand(const ZoomEngineInitCommand& command);
std::string buildZoomEngineJoinCommand(const ZoomEngineJoinCommand& command);
std::string buildZoomEngineLeaveCommand();
std::string buildZoomEngineStartMediaCommand();
std::string buildZoomEngineStopMediaCommand();
std::string buildZoomEngineQuitCommand();
std::string buildZoomEngineSubscribeCommand(const ZoomEngineSubscribeCommand& command);
std::string buildZoomEngineSubscribeAudioCommand(const ZoomEngineSubscribeCommand& command);
std::string buildZoomEngineUnsubscribeCommand(const std::string& sourceUuid);

std::optional<ZoomEngineEvent> parseZoomEngineEvent(const std::string& line);

std::string zoomEngineVideoSharedMemoryName(const std::string& sourceUuid);
std::string zoomEngineAudioSharedMemoryName(const std::string& sourceUuid);
std::size_t zoomEngineI420FrameByteSize(std::uint32_t width, std::uint32_t height);
std::size_t zoomEnginePcmAudioByteSize(std::uint32_t byteLength);
std::optional<ZoomEngineRgbaFrame> readZoomEngineI420FrameSnapshot(
    const void* sharedMemory,
    std::size_t sharedMemorySize,
    const std::string& sourceUuid,
    std::uint32_t participantId,
    std::uint32_t maxWidth,
    std::uint32_t maxHeight);

}  // namespace corevideo::modules
