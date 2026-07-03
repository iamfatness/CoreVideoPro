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
  // Request 1080p from Zoom (2 = ZoomSDKResolution_1080P). Zoom adaptively delivers
  // up to this based on the sender + conditions; the engine falls back to 720/360 if
  // 1080p subscribe fails. 720p (1) was capping every participant below 1080p.
  std::uint32_t resolution = 2;
  std::string mode;
  bool isolateAudio = false;
  bool audienceAudio = false;
};

struct ZoomEngineParticipant {
  std::uint32_t id = 0;
  std::string displayName;
  bool hasVideo = false;
  bool isTalking = false;
  bool isMuted = false;
  bool isSharingScreen = false;
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
  std::vector<ZoomEngineParticipant> participants;
};

struct ZoomEngineRgbaFrame {
  std::string sourceUuid;
  std::string participantId;
  // Dimensions of the (downscaled) BGRA `rgba` thumbnail below.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t frameId = 0;
  // Downscaled BGRA thumbnail (capped at the maxWidth/maxHeight passed to
  // readZoomEngineI420FrameSnapshot) for the WinUI base64 multiview path.
  std::vector<std::uint8_t> rgba;
  // Full-resolution I420 (YUV 4:2:0 planar) planes copied verbatim from shared
  // memory: Y (i420Width*i420Height) then U then V. The compositor uploads these
  // to GPU textures and converts to RGB in-shader, so the per-pixel CPU convert
  // is no longer paid at full resolution (only for the small thumbnail above).
  std::vector<std::uint8_t> i420;
  std::uint32_t i420Width = 0;
  std::uint32_t i420Height = 0;
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

// SHM region names for a source. instanceToken must match the token the engine
// was launched with (ZoomEngineProcessClient::instanceToken); empty reproduces
// the legacy fixed names.
std::string zoomEngineVideoSharedMemoryName(const std::string& sourceUuid,
                                            const std::string& instanceToken = {});
std::string zoomEngineAudioSharedMemoryName(const std::string& sourceUuid,
                                            const std::string& instanceToken = {});
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
