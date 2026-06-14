#include "modules/ZoomEngineRuntime.h"

#include "config/ZoomMeetingSdkConfig.h"
#include "engine-ipc.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace corevideo::modules {
namespace {

std::string envString(const char* name) {
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string{};
}

int envInt(const char* name, int fallback) {
  const auto value = envString(name);
  if (value.empty()) {
    return fallback;
  }
  try {
    return (std::max)(0, std::stoi(value));
  } catch (...) {
    return fallback;
  }
}

std::string participantIdString(std::uint32_t id) {
  return id == 0 ? std::string{} : std::to_string(id);
}

std::string meetingIdFromJoinPayload(const rpc::Json& payload) {
  std::string source = payload.getString("meetingNumber");
  if (source.empty()) {
    source = payload.getString("meetingUrl");
  }

  std::string digits;
  for (char ch : source) {
    if (ch >= '0' && ch <= '9') {
      digits.push_back(ch);
    }
  }
  return digits;
}

rpc::Json::Array stringArray(const std::vector<std::string>& values) {
  rpc::Json::Array result;
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return result;
}

}  // namespace

ZoomEngineRuntime::ZoomEngineRuntime() : config_(loadConfig()) {}

ZoomEngineRuntime::~ZoomEngineRuntime() {
  stopReader();
}

ZoomEngineRuntime::Config ZoomEngineRuntime::loadConfig() {
  Config config;
  config.executablePath = envString("COREVIDEO_ZOOM_ENGINE_PATH");
  config.sdkJwt = envString("COREVIDEO_ZOOM_SDK_JWT");
  config.publicAppKey = envString("COREVIDEO_ZOOM_PUBLIC_APP_KEY");
  if (config.publicAppKey.empty()) {
    config.publicAppKey = config::kEmbeddedZoomPublicAppKey;
  }
  config.passcode = envString("COREVIDEO_ZOOM_MEETING_PASSCODE");
  config.onBehalfToken = envString("COREVIDEO_ZOOM_ON_BEHALF_TOKEN");
  config.userZak = envString("COREVIDEO_ZOOM_USER_ZAK");
  config.appPrivilegeToken = envString("COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN");
  config.connectTimeoutMs = envInt("COREVIDEO_ZOOM_ENGINE_CONNECT_TIMEOUT_MS", config.connectTimeoutMs);
  config.joinWaitMs = envInt("COREVIDEO_ZOOM_JOIN_WAIT_MS", config.joinWaitMs);
  return config;
}

bool ZoomEngineRuntime::configured() const {
  return !config_.executablePath.empty();
}

rpc::Json ZoomEngineRuntime::join(const rpc::Json& payload) {
  if (!configured()) {
    return nullptr;
  }

  const auto meetingId = meetingIdFromJoinPayload(payload);
  if (meetingId.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    return rpc::Json::Object{
        {"meetingState", "error"},
        {"participants", rpc::Json::Array{}},
        {"tick", ++fallbackTick_},
        {"warnings", rpc::Json::Array{"Zoom join request did not include a numeric meeting id."}},
    };
  }

  bool waitForAuth = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureStartedLocked()) {
      return rawCaptureSnapshotLocked();
    }

    if (!initialized_) {
      const bool sent = process_->sendLine(buildZoomEngineInitCommand({config_.sdkJwt, config_.publicAppKey}));
      initialized_ = sent;
      if (!sent) {
        state_.apply({ZoomEngineEventKind::Error, "error", "", "init", process_->lastError()});
        return rawCaptureSnapshotLocked();
      }
      waitForAuth = true;
    }
  }

  if (waitForAuth) {
    bool authReady = false;
    const auto authDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.joinWaitMs);
    while (std::chrono::steady_clock::now() < authDeadline) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto snapshot = state_.snapshot();
        if (snapshot.meetingState == "joining") {
          authReady = true;
          break;
        }
        if (snapshot.meetingState == "error") {
          return rawCaptureSnapshotLocked();
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!authReady) {
      std::lock_guard<std::mutex> lock(mutex_);
      state_.apply({ZoomEngineEventKind::Error, "error", "", "auth", "Timed out waiting for Zoom SDK authentication."});
      return rawCaptureSnapshotLocked();
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    ZoomEngineJoinCommand command;
    command.meetingId = meetingId;
    command.displayName = payload.getString("displayName", "CoreVideo Pro");
    command.passcode = payload.getString("passcode", config_.passcode);
    command.onBehalfToken = config_.onBehalfToken;
    command.userZak = config_.userZak;
    command.appPrivilegeToken = config_.appPrivilegeToken;
    if (!process_->sendLine(buildZoomEngineJoinCommand(command))) {
      state_.apply({ZoomEngineEventKind::Error, "error", "", "join", process_->lastError()});
      return rawCaptureSnapshotLocked();
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.joinWaitMs);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto snapshot = state_.snapshot();
      if (snapshot.meetingState == "in-meeting" || snapshot.meetingState == "error") {
        return rawCaptureSnapshotLocked();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  return rawCaptureSnapshotLocked();
}

rpc::Json ZoomEngineRuntime::leave() {
  if (!configured()) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (process_ && process_->running()) {
    (void)process_->sendLine(buildZoomEngineLeaveCommand());
  }
  state_.reset();
  mediaStarted_ = false;
  ++fallbackTick_;
  return rawCaptureSnapshotLocked();
}

rpc::Json ZoomEngineRuntime::snapshot() {
  if (!configured()) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  return rawCaptureSnapshotLocked();
}

rpc::Json ZoomEngineRuntime::syncSpine(const rpc::Json& payload, double elapsedMs) {
  if (!configured()) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  (void)ensureMediaStartedLocked();
  const rpc::Json* subscriptions = payload.get("subscriptions");
  if (process_ && process_->running() && subscriptions && subscriptions->isArray()) {
    for (const auto& request : subscriptions->asArray()) {
      const auto participantId = request.getString("participantId");
      if (participantId.empty()) {
        continue;
      }
      ZoomEngineSubscribeCommand command;
      try {
        command.participantId = static_cast<std::uint32_t>(std::stoul(participantId));
      } catch (...) {
        continue;
      }
      command.sourceUuid = request.getString("kind") + "-" + participantId + "-" + request.getString("purpose");
      command.mode = request.getString("kind") == "screen-share" ? "screenshare" : "";
      if (request.getString("kind") == "participant-audio") {
        (void)process_->sendLine(buildZoomEngineSubscribeAudioCommand(command));
      } else {
        (void)process_->sendLine(buildZoomEngineSubscribeCommand(command));
      }
    }
  }
  return spineSnapshotLocked(payload, elapsedMs);
}

std::vector<rpc::Json> ZoomEngineRuntime::drainFrameEvents() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto events = std::move(pendingFrameEvents_);
  pendingFrameEvents_.clear();
  return events;
}

bool ZoomEngineRuntime::ensureStartedLocked() {
  if (process_ && process_->running()) {
    return true;
  }

  process_ = std::make_unique<ZoomEngineProcessClient>();
  if (!process_->start({config_.executablePath, config_.connectTimeoutMs})) {
    state_.apply({ZoomEngineEventKind::Error, "error", "", "launch", process_->lastError()});
    return false;
  }
  initialized_ = false;
  startReaderLocked();
  return true;
}

void ZoomEngineRuntime::startReaderLocked() {
  if (readerRunning_) {
    return;
  }
  readerRunning_ = true;
  reader_ = std::thread([this]() { readerLoop(); });
}

void ZoomEngineRuntime::readerLoop() {
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!readerRunning_ || !process_ || !process_->running()) {
        return;
      }
    }

    auto event = process_->readEvent();
    if (!event) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (readerRunning_) {
        state_.apply({ZoomEngineEventKind::Error, "error", "", "read", process_ ? process_->lastError() : "Zoom engine stopped."});
      }
      return;
    }
    applyEvent(*event);
  }
}

void ZoomEngineRuntime::applyEvent(const ZoomEngineEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_.apply(event);
  if (event.kind == ZoomEngineEventKind::Joined) {
    (void)ensureMediaStartedLocked();
  }
  if (event.kind == ZoomEngineEventKind::Frame) {
    enqueueFrameEventLocked(event);
  }
}

void ZoomEngineRuntime::stopReader() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    readerRunning_ = false;
    if (process_) {
      process_->stop();
    }
  }
  if (reader_.joinable()) {
    reader_.join();
  }
}

rpc::Json ZoomEngineRuntime::rawCaptureSnapshotLocked() {
  ++fallbackTick_;
  const auto snapshot = state_.snapshot();
  rpc::Json::Array participants;
  for (const auto& participant : snapshot.participants) {
    participants.emplace_back(rpc::Json::Object{
        {"userId", participantIdString(participant.id)},
        {"displayName", participant.displayName.empty() ? "Zoom User " + participantIdString(participant.id) : participant.displayName},
        {"role", "Guest"},
        {"muted", participant.isMuted},
        {"videoOn", participant.hasVideo},
        {"talking", participant.isTalking || participantIdString(participant.id) == snapshot.activeSpeakerId},
        {"sharingScreen", participant.isSharingScreen},
        {"audioLevel", participant.isTalking || participantIdString(participant.id) == snapshot.activeSpeakerId ? 78 : 12},
        {"networkQuality", "good"},
    });
  }

  rpc::Json::Object result{
      {"meetingState", snapshot.meetingState == "in-meeting" ? "in_meeting" : snapshot.meetingState},
      {"participants", participants},
      {"tick", fallbackTick_},
  };
  if (!snapshot.activeSpeakerId.empty()) {
    result.emplace("activeSpeakerId", snapshot.activeSpeakerId);
  }
  if (!snapshot.warnings.empty()) {
    result.emplace("warnings", stringArray(snapshot.warnings));
  }
  return result;
}

rpc::Json ZoomEngineRuntime::spineSnapshotLocked(const rpc::Json& payload, double elapsedMs) {
  const auto runtime = state_.snapshot();
  rpc::Json::Array subscriptions;
  const rpc::Json* requested = payload.get("subscriptions");
  if (requested && requested->isArray()) {
    for (const auto& request : requested->asArray()) {
      const auto participantId = request.getString("participantId");
      auto found = std::find_if(runtime.subscriptions.begin(), runtime.subscriptions.end(), [&](const auto& stats) {
        return stats.participantId == participantId && stats.kind == request.getString("kind");
      });
      const bool hasStats = found != runtime.subscriptions.end();
      subscriptions.emplace_back(rpc::Json::Object{
          {"participantId", participantId},
          {"kind", request.getString("kind")},
          {"purpose", request.getString("purpose")},
          {"priority", request.get("priority") ? *request.get("priority") : rpc::Json(0)},
          {"subscriptionId", request.getString("kind") + ":" + participantId + ":" + request.getString("purpose")},
          {"status", hasStats ? "subscribed" : "pending"},
          {"lastResultCode", hasStats ? "ok" : "pending"},
          {"deliveredWidth", hasStats ? static_cast<int>(found->width) : 0},
          {"deliveredHeight", hasStats ? static_cast<int>(found->height) : 0},
          {"deliveredFps", hasStats ? 30 : 0},
          {"framesReceived", hasStats ? static_cast<int>(found->framesReceived) : 0},
          {"audioPacketsReceived", hasStats ? static_cast<int>(found->audioPacketsReceived) : 0},
      });
    }
  }

  return rpc::Json::Object{
      {"meetingState", runtime.meetingState},
      {"sdkVersion", "zoom-engine"},
      {"participantCount", static_cast<int>(runtime.participants.size())},
      {"activeSpeakerId", runtime.activeSpeakerId},
      {"screenShareParticipantId", runtime.screenShareParticipantId},
      {"participants", state_.participantsJson()},
      {"subscriptions", subscriptions},
      {"recording",
       rpc::Json::Object{
           {"evidence",
            rpc::Json::Object{
                {"programFramesWritten", 0},
                {"isoFramesWritten", 0},
                {"audioPacketsObserved", 0},
                {"subscribedVideoFeeds", static_cast<int>(runtime.subscriptions.size())},
            }},
       }},
      {"warnings", stringArray(runtime.warnings)},
      {"events", stringArray(runtime.events)},
  };
}

bool ZoomEngineRuntime::ensureMediaStartedLocked() {
  if (mediaStarted_ || !process_ || !process_->running()) {
    return mediaStarted_;
  }
  if (state_.snapshot().meetingState != "in-meeting") {
    return false;
  }
  if (!process_->sendLine(buildZoomEngineStartMediaCommand())) {
    state_.apply({ZoomEngineEventKind::Error, "error", "", "start_media", process_->lastError()});
    return false;
  }
  mediaStarted_ = true;
  return true;
}

void ZoomEngineRuntime::enqueueFrameEventLocked(const ZoomEngineEvent& event) {
  if (event.sourceUuid.empty() || event.participantId == 0 || event.width == 0 || event.height == 0) {
    return;
  }

  ShmRegion region;
  const auto size = zoomEngineI420FrameByteSize(event.width, event.height);
  if (!shm_region_open_read(region, zoomEngineVideoSharedMemoryName(event.sourceUuid), size)) {
    return;
  }
  const auto closeRegion = [&region]() { shm_region_destroy(region); };
  const auto frame = readZoomEngineI420FrameSnapshot(region.ptr, region.size, event.sourceUuid, event.participantId, 320, 180);
  closeRegion();
  if (!frame) {
    return;
  }

  rpc::Json::Array rgba;
  rgba.reserve(frame->rgba.size());
  for (const auto byte : frame->rgba) {
    rgba.emplace_back(static_cast<int>(byte));
  }

  pendingFrameEvents_.emplace_back(rpc::Json::Object{
      {"type", "zoom-video-frame"},
      {"frame",
       rpc::Json::Object{
           {"participantId", frame->participantId},
           {"width", static_cast<int>(frame->width)},
           {"height", static_cast<int>(frame->height)},
           {"frameId", static_cast<int>(frame->frameId)},
           {"rgba", rgba},
       }},
  });
}

}  // namespace corevideo::modules
