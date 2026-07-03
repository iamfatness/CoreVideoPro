#include "modules/ZoomEngineClient.h"

#include "rpc/Json.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine-ipc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <execution>
#include <utility>
#include <vector>

namespace corevideo::modules {
namespace {

using corevideo::rpc::Json;

std::uint32_t uintField(const Json& object, const std::string& key) {
  const Json* value = object.get(key);
  if (!value || !value->isNumber()) {
    return 0;
  }
  const auto number = value->asNumber();
  return number > 0 ? static_cast<std::uint32_t>(number) : 0;
}

bool boolField(const Json& object, const std::string& key) {
  const Json* value = object.get(key);
  return value ? value->asBool(false) : false;
}

std::uint8_t clampByte(int value) {
  return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void i420ToRgbaPixel(
    const std::uint8_t* yPlane,
    const std::uint8_t* uPlane,
    const std::uint8_t* vPlane,
    std::uint32_t sourceWidth,
    std::uint32_t sourceX,
    std::uint32_t sourceY,
    std::uint8_t* rgba) {
  const int y = static_cast<int>(yPlane[sourceY * sourceWidth + sourceX]);
  const int u = static_cast<int>(uPlane[(sourceY / 2) * (sourceWidth / 2) + (sourceX / 2)]);
  const int v = static_cast<int>(vPlane[(sourceY / 2) * (sourceWidth / 2) + (sourceX / 2)]);
  const int c = y;  // full-range luma (no -16 offset, unity scale)
  const int d = u - 128;
  const int e = v - 128;
  // Output BGRA (blue first) to match the compositor/preview pixel order. Emitting
  // RGBA here swaps red and blue on display (skin tones render blue).
  // BT.709 FULL-range YCbCr->RGB. Zoom delivers full-range (0-255) YUV; converting
  // it as limited-range (the prior -16 / 1.164 scaling) crushed shadows and read as
  // "way darker" in dim scenes.
  rgba[0] = clampByte((256 * c + 475 * d + 128) >> 8);          // B
  rgba[1] = clampByte((256 * c - 48 * d - 120 * e + 128) >> 8); // G
  rgba[2] = clampByte((256 * c + 403 * e + 128) >> 8);          // R
  rgba[3] = 255;                                                 // A
}

std::string commandLine(Json::Object object) {
  return Json(std::move(object)).stringify();
}

ZoomEngineEventKind kindForCommand(const std::string& command) {
  if (command == "ping") return ZoomEngineEventKind::Ping;
  if (command == IPC_EVT_READY) return ZoomEngineEventKind::Ready;
  if (command == IPC_EVT_AUTH_OK) return ZoomEngineEventKind::AuthOk;
  if (command == IPC_EVT_AUTH_FAIL) return ZoomEngineEventKind::AuthFail;
  if (command == IPC_EVT_JOINED) return ZoomEngineEventKind::Joined;
  if (command == IPC_EVT_LEFT) return ZoomEngineEventKind::Left;
  if (command == "participants") return ZoomEngineEventKind::Participants;
  if (command == "active_speaker") return ZoomEngineEventKind::ActiveSpeaker;
  if (command == IPC_EVT_FRAME) return ZoomEngineEventKind::Frame;
  if (command == IPC_EVT_AUDIO) return ZoomEngineEventKind::Audio;
  if (command == "debug") return ZoomEngineEventKind::Debug;
  if (command == IPC_EVT_ERROR) return ZoomEngineEventKind::Error;
  return ZoomEngineEventKind::Unknown;
}

}  // namespace

std::string buildZoomEngineInitCommand(const ZoomEngineInitCommand& command) {
  Json::Object object{{"cmd", IPC_CMD_INIT}};
  // Mirror ZoomObsEngine / ZoomEngineClient: send exactly one auth credential.
  // If both are present the engine prefers public_app_key and ignores OAuth JWT.
  if (!command.jwt.empty()) {
    object.emplace("jwt", command.jwt);
  } else if (!command.publicAppKey.empty()) {
    object.emplace("public_app_key", command.publicAppKey);
  }
  return commandLine(std::move(object));
}

std::string buildZoomEngineJoinCommand(const ZoomEngineJoinCommand& command) {
  Json::Object object{
      {"cmd", IPC_CMD_JOIN},
      {"meeting_id", command.meetingId},
      {"display_name", command.displayName.empty() ? "CoreVideo Pro" : command.displayName},
  };
  if (!command.passcode.empty()) object.emplace("passcode", command.passcode);
  if (!command.onBehalfToken.empty()) object.emplace("on_behalf_token", command.onBehalfToken);
  if (!command.userZak.empty()) object.emplace("user_zak", command.userZak);
  if (!command.appPrivilegeToken.empty()) object.emplace("app_privilege_token", command.appPrivilegeToken);
  return commandLine(std::move(object));
}

std::string buildZoomEngineLeaveCommand() {
  return commandLine({{"cmd", IPC_CMD_LEAVE}});
}

std::string buildZoomEngineStartMediaCommand() {
  return commandLine({{"cmd", IPC_CMD_START_MEDIA}});
}

std::string buildZoomEngineStopMediaCommand() {
  return commandLine({{"cmd", IPC_CMD_STOP_MEDIA}});
}

std::string buildZoomEngineQuitCommand() {
  return commandLine({{"cmd", IPC_CMD_QUIT}});
}

std::string buildZoomEngineSubscribeCommand(const ZoomEngineSubscribeCommand& command) {
  Json::Object object{
      {"cmd", IPC_CMD_SUBSCRIBE},
      {"source_uuid", command.sourceUuid},
      {"participant_id", static_cast<double>(command.participantId)},
      {"resolution", static_cast<double>(std::min<std::uint32_t>(command.resolution, 2))},
  };
  if (!command.mode.empty()) object.emplace("mode", command.mode);
  if (command.isolateAudio) object.emplace("isolate_audio", true);
  if (command.audienceAudio) object.emplace("audience_audio", true);
  return commandLine(std::move(object));
}

std::string buildZoomEngineSubscribeAudioCommand(const ZoomEngineSubscribeCommand& command) {
  Json::Object object{
      {"cmd", IPC_CMD_SUBSCRIBE_AUDIO},
      {"source_uuid", command.sourceUuid},
      {"participant_id", static_cast<double>(command.participantId)},
  };
  if (command.isolateAudio) object.emplace("isolate_audio", true);
  if (command.audienceAudio) object.emplace("audience_audio", true);
  return commandLine(std::move(object));
}

std::string buildZoomEngineUnsubscribeCommand(const std::string& sourceUuid) {
  return commandLine({{"cmd", IPC_CMD_UNSUBSCRIBE}, {"source_uuid", sourceUuid}});
}

std::optional<ZoomEngineEvent> parseZoomEngineEvent(const std::string& line) {
  std::string error;
  auto parsed = Json::parse(line, &error);
  if (!parsed || !parsed->isObject()) {
    return std::nullopt;
  }

  ZoomEngineEvent event;
  event.command = parsed->getString("cmd");
  event.kind = kindForCommand(event.command);
  event.sourceUuid = parsed->getString("source_uuid");
  event.stage = parsed->getString("stage");
  event.message = parsed->getString("msg", parsed->getString("message"));
  if (event.message == "meeting_failed" || event.message == "join_failed") {
    const auto reason = parsed->getString("reason");
    if (!reason.empty()) {
      event.message += ": " + reason;
    }
  }
  event.participantId = uintField(*parsed, "participant_id");
  event.activeSpeakerId = uintField(*parsed, "active_speaker_id");
  if (event.activeSpeakerId == 0) {
    event.activeSpeakerId = uintField(*parsed, "active");
  }
  event.width = uintField(*parsed, "w");
  event.height = uintField(*parsed, "h");
  event.byteLength = uintField(*parsed, "byte_len");
  event.ok = event.kind != ZoomEngineEventKind::Unknown && event.kind != ZoomEngineEventKind::AuthFail &&
             event.kind != ZoomEngineEventKind::Error;

  if (const Json* participants = parsed->get("participants"); participants && participants->isArray()) {
    event.participantCount = participants->asArray().size();
    event.participants.reserve(participants->asArray().size());
    for (const Json& participant : participants->asArray()) {
      if (!participant.isObject()) {
        continue;
      }
      event.participants.push_back({
          uintField(participant, "id"),
          participant.getString("name"),
          boolField(participant, "has_video"),
          boolField(participant, "is_talking"),
          boolField(participant, "is_muted"),
          boolField(participant, "is_sharing_screen"),
      });
    }
  }

  // Diagnostic: surface every control/status/error event from the engine on the
  // core's stderr (captured to media-core.log). Exclude high-rate frame/audio
  // events so the log stays readable while debugging join/auth failures.
  if (event.kind != ZoomEngineEventKind::Frame && event.kind != ZoomEngineEventKind::Audio) {
    std::fprintf(stderr, "[zoom-engine] %s\n", line.c_str());
    std::fflush(stderr);
  }

  return event;
}

std::string zoomEngineVideoSharedMemoryName(const std::string& sourceUuid,
                                            const std::string& instanceToken) {
  return ipc_shm_prefix(instanceToken) + sourceUuid;
}

std::string zoomEngineAudioSharedMemoryName(const std::string& sourceUuid,
                                            const std::string& instanceToken) {
  return ipc_shm_prefix(instanceToken) + sourceUuid + "_audio";
}

std::size_t zoomEngineI420FrameByteSize(std::uint32_t width, std::uint32_t height) {
  const std::size_t yLength = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  return sizeof(ShmFrameHeader) + yLength + yLength / 4 + yLength / 4;
}

std::size_t zoomEnginePcmAudioByteSize(std::uint32_t byteLength) {
  return sizeof(ShmAudioHeader) + static_cast<std::size_t>(byteLength);
}

std::optional<ZoomEngineRgbaFrame> readZoomEngineI420FrameSnapshot(
    const void* sharedMemory,
    std::size_t sharedMemorySize,
    const std::string& sourceUuid,
    std::uint32_t participantId,
    std::uint32_t maxWidth,
    std::uint32_t maxHeight) {
  if (!sharedMemory || sharedMemorySize < sizeof(ShmFrameHeader) || maxWidth == 0 || maxHeight == 0) {
    return std::nullopt;
  }

  ShmFrameHeader before{};
  std::memcpy(&before, sharedMemory, sizeof(before));
  if (before.sequence == 0 || (before.sequence & 1u) != 0 || before.width == 0 || before.height == 0) {
    return std::nullopt;
  }
  if ((before.width & 1u) != 0 || (before.height & 1u) != 0) {
    return std::nullopt;
  }
  const std::size_t yLength = static_cast<std::size_t>(before.width) * static_cast<std::size_t>(before.height);
  if (before.y_len != yLength) {
    return std::nullopt;
  }
  const std::size_t requiredSize = zoomEngineI420FrameByteSize(before.width, before.height);
  if (sharedMemorySize < requiredSize) {
    return std::nullopt;
  }

  const auto* bytes = static_cast<const std::uint8_t*>(sharedMemory);
  // Copy the I420 planes out FAST, then validate the sequence, then convert from
  // the local copy. The BGRA conversion is far slower than a memcpy; converting
  // directly from shared memory lets the engine overwrite the frame mid-convert,
  // and the tear check then rejects it — at higher resolutions that loses almost
  // every frame (video appears frozen / very slow). Copy-then-convert keeps the
  // tear window down to the memcpy only.
  const std::size_t planeBytes = yLength + (yLength / 4) * 2;
  std::vector<std::uint8_t> planes(planeBytes);
  std::memcpy(planes.data(), bytes + sizeof(ShmFrameHeader), planeBytes);

  ShmFrameHeader afterCopy{};
  std::memcpy(&afterCopy, sharedMemory, sizeof(afterCopy));
  if (afterCopy.sequence != before.sequence || (afterCopy.sequence & 1u) != 0 ||
      afterCopy.width != before.width || afterCopy.height != before.height ||
      afterCopy.y_len != before.y_len) {
    return std::nullopt;  // torn during copy — caller retries next tick
  }

  std::uint32_t outputWidth = before.width;
  std::uint32_t outputHeight = before.height;
  if (outputWidth > maxWidth || outputHeight > maxHeight) {
    const double scale = (std::min)(
        static_cast<double>(maxWidth) / static_cast<double>(before.width),
        static_cast<double>(maxHeight) / static_cast<double>(before.height));
    outputWidth = (std::max)(std::uint32_t{1}, static_cast<std::uint32_t>(std::floor(before.width * scale)));
    outputHeight = (std::max)(std::uint32_t{1}, static_cast<std::uint32_t>(std::floor(before.height * scale)));
  }

  ZoomEngineRgbaFrame frame;
  frame.sourceUuid = sourceUuid;
  frame.participantId = std::to_string(participantId);
  frame.width = outputWidth;
  frame.height = outputHeight;
  frame.frameId = before.sequence;
  // Carry the full-resolution I420 planes to the compositor (GPU convert). The
  // planes are taken by move; the plane pointers below read from the moved-into
  // buffer for the (small) thumbnail convert.
  frame.i420Width = before.width;
  frame.i420Height = before.height;
  frame.i420 = std::move(planes);
  const std::uint8_t* yPlane = frame.i420.data();
  const std::uint8_t* uPlane = yPlane + yLength;
  const std::uint8_t* vPlane = uPlane + yLength / 4;
  frame.rgba.resize(static_cast<std::size_t>(outputWidth) * static_cast<std::size_t>(outputHeight) * 4);

  // Convert rows in parallel for the (downscaled) thumbnail only — callers ask
  // for a small output size (e.g. 640x360), so this is cheap. The full-resolution
  // per-pixel convert that used to dominate the render is gone: the compositor
  // converts the I420 planes on the GPU instead.
  std::vector<std::uint32_t> rowIndices(outputHeight);
  for (std::uint32_t y = 0; y < outputHeight; ++y) {
    rowIndices[y] = y;
  }
  std::for_each(std::execution::par, rowIndices.begin(), rowIndices.end(), [&](std::uint32_t y) {
    const std::uint32_t sourceY = (std::min)(before.height - 1, static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(y) * before.height) / outputHeight));
    for (std::uint32_t x = 0; x < outputWidth; ++x) {
      const std::uint32_t sourceX = (std::min)(before.width - 1, static_cast<std::uint32_t>(
          (static_cast<std::uint64_t>(x) * before.width) / outputWidth));
      auto* rgba = frame.rgba.data() + (static_cast<std::size_t>(y) * outputWidth + x) * 4;
      i420ToRgbaPixel(yPlane, uPlane, vPlane, before.width, sourceX, sourceY, rgba);
    }
  });

  return frame;
}

}  // namespace corevideo::modules
