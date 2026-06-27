#include "modules/ZoomEngineRuntime.h"

#include "config/ZoomMeetingSdkConfig.h"
#include "engine-ipc.h"
#include "modules/ProgramFramePreview.h"

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

// Nearest-neighbor downscale of a BGRA buffer to fit within maxW x maxH (keeping
// aspect). Used to keep the base64 multiview-tile payload small: the compositor
// gets the full-res frame in-process, but streaming a full 720p BGRA frame as
// base64 over stdout per participant per frame is a multi-MB firehose that
// saturates the pipe and reader thread (video trickles through = slow motion).
std::vector<std::uint8_t> downscaleBgraThumbnail(
    const std::vector<std::uint8_t>& src, int srcW, int srcH, int maxW, int maxH,
    int& outW, int& outH) {
  outW = srcW;
  outH = srcH;
  if (srcW > maxW || srcH > maxH) {
    const double scale = (std::min)(static_cast<double>(maxW) / srcW, static_cast<double>(maxH) / srcH);
    outW = (std::max)(1, static_cast<int>(srcW * scale));
    outH = (std::max)(1, static_cast<int>(srcH * scale));
  }
  if (outW == srcW && outH == srcH) {
    return src;
  }
  std::vector<std::uint8_t> dst(static_cast<std::size_t>(outW) * outH * 4);
  for (int y = 0; y < outH; ++y) {
    const int sy = (std::min)(srcH - 1, y * srcH / outH);
    for (int x = 0; x < outW; ++x) {
      const int sx = (std::min)(srcW - 1, x * srcW / outW);
      const std::uint8_t* s = &src[(static_cast<std::size_t>(sy) * srcW + sx) * 4];
      std::uint8_t* d = &dst[(static_cast<std::size_t>(y) * outW + x) * 4];
      d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
    }
  }
  return dst;
}

constexpr double kFrameStaleAfterMs = 1000.0;

}  // namespace

ZoomEngineRuntime::ZoomEngineRuntime() : config_(loadConfig()), startedAt_(std::chrono::steady_clock::now()) {}

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

void ZoomEngineRuntime::applyJoinCredentialsFromPayload(const rpc::Json& payload) {
  config_ = loadConfig();
  const auto payloadJwt = payload.getString("sdkJwt");
  const auto payloadZak = payload.getString("userZak");
  if (!payloadJwt.empty()) {
    config_.sdkJwt = payloadJwt;
    // Broker JWT auth replaces embedded public-app-key auth.
    config_.publicAppKey.clear();
  }
  if (!payloadZak.empty()) {
    config_.userZak = payloadZak;
  }
  if (!payloadJwt.empty() && initialized_) {
    if (process_ && process_->running()) {
      (void)process_->sendLine(buildZoomEngineLeaveCommand());
    }
    state_.reset();
    initialized_ = false;
    mediaStarted_ = false;
    latestDecodedFrames_.clear();
  }
}

rpc::Json ZoomEngineRuntime::join(const rpc::Json& payload) {
  if (!configured()) {
    return nullptr;
  }

  applyJoinCredentialsFromPayload(payload);

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
      ZoomEngineInitCommand initCommand;
      if (!config_.sdkJwt.empty()) {
        initCommand.jwt = config_.sdkJwt;
      } else if (!config_.publicAppKey.empty()) {
        initCommand.publicAppKey = config_.publicAppKey;
      }
      const bool sent = process_->sendLine(buildZoomEngineInitCommand(initCommand));
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
        if (state_.sdkAuthenticated()) {
          authReady = true;
          break;
        }
        if (state_.snapshot().meetingState == "error") {
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
    const auto payloadZak = payload.getString("userZak");
    command.userZak = !payloadZak.empty() ? payloadZak : config_.userZak;
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
  state_.apply({ZoomEngineEventKind::Error, "error", "", "join", "Timed out waiting for Zoom meeting join result."});
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
  latestDecodedFrames_.clear();
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
  // Raw capture (StartRawRecording / recording-privilege request) must wait for an
  // explicit operator action — the Studio "Engine On" toggle sets startCapture.
  // Previously this started unconditionally on the first spine sync after join,
  // which fired the Zoom recording-rights request the moment the meeting joined.
  const rpc::Json* startCapture = payload.get("startCapture");
  captureRequested_ = startCapture != nullptr && startCapture->asBool(false);
  if (captureRequested_) {
    (void)ensureMediaStartedLocked();
  }
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
  state_.refreshFrameFreshness(runtimeElapsedMs(), kFrameStaleAfterMs);
  return spineSnapshotLocked(payload, elapsedMs);
}

std::vector<VideoFrame> ZoomEngineRuntime::pollCompositorVideoFrames(int64_t timestampMs) {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_.pollCompositorVideoFrames(timestampMs);
}

std::vector<AudioFrame> ZoomEngineRuntime::pollCompositorAudioFrames(int64_t timestampMs) {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_.pollCompositorAudioFrames(timestampMs);
}

std::vector<rpc::Json> ZoomEngineRuntime::drainFrameEvents() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto events = std::move(pendingFrameEvents_);
  pendingFrameEvents_.clear();
  return events;
}

std::vector<VideoFrame> ZoomEngineRuntime::latestDecodedVideoFrames(int64_t timestampMs) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<VideoFrame> frames;
  frames.reserve(latestDecodedFrames_.size());
  for (const auto& [participantId, decoded] : latestDecodedFrames_) {
    if (!decoded.pixels || decoded.width <= 0 || decoded.height <= 0) {
      continue;
    }
    VideoFrame frame;
    frame.participantId = participantId;
    frame.width = decoded.width;
    frame.height = decoded.height;
    frame.naturalWidth = decoded.width;
    frame.naturalHeight = decoded.height;
    frame.timestampMs = timestampMs;
    frame.pixels = decoded.pixels;
    frame.pixelWidth = decoded.width;
    frame.pixelHeight = decoded.height;
    frame.pixelStride = decoded.width * 4;
    frame.frameId = decoded.frameId;
    frames.push_back(std::move(frame));
  }
  return frames;
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
    // Only auto-start raw media on join if the operator already enabled capture
    // (Engine On). Otherwise wait — syncSpine starts it when startCapture is set.
    if (captureRequested_) {
      (void)ensureMediaStartedLocked();
    }
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
          {"firstFrameAtMs", hasStats ? found->firstFrameAtMs : -1.0},
          {"lastFrameAtMs", hasStats ? found->lastFrameAtMs : -1.0},
          {"firstFrameDelayMs", hasStats ? found->firstFrameDelayMs : -1.0},
          {"lastFrameAgeMs", hasStats ? found->lastFrameAgeMs : -1.0},
          {"lastFrameId", hasStats ? static_cast<int>(found->lastFrameId) : 0},
          {"frameFresh", hasStats ? found->frameFresh : false},
          {"staleFrameCount", hasStats ? static_cast<int>(found->staleFrameCount) : 0},
          {"malformedFrameCount", hasStats ? static_cast<int>(found->malformedFrameCount) : 0},
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
    state_.recordFrameIngestFailure(event.sourceUuid, event.participantId, "frame event missing source, participant, width, or height");
    return;
  }

  ShmRegion region;
  const auto size = zoomEngineI420FrameByteSize(event.width, event.height);
  if (!shm_region_open_read(region, zoomEngineVideoSharedMemoryName(event.sourceUuid), size)) {
    state_.recordFrameIngestFailure(event.sourceUuid, event.participantId, "shared memory region could not be opened");
    return;
  }
  const auto closeRegion = [&region]() { shm_region_destroy(region); };
  // Full resolution (up to 1080p) for the compositor — the GPU composites and
  // shares at 1080p, so the participant source should match. The convert is
  // parallelized (readZoomEngineI420FrameSnapshot) so high res stays smooth.
  const auto frame = readZoomEngineI420FrameSnapshot(region.ptr, region.size, event.sourceUuid, event.participantId, 1920, 1080);
  closeRegion();
  if (!frame) {
    state_.recordFrameIngestFailure(event.sourceUuid, event.participantId, "shared memory snapshot was incomplete, stale, or malformed");
    return;
  }
  state_.recordFrameIngestSuccess(
      event.sourceUuid,
      event.participantId,
      event.width,
      event.height,
      frame->frameId,
      runtimeElapsedMs());

  // Tap the decoded BGRA pixels for the compositor without disturbing the
  // stdout/event queue below that feeds the WinUI multiview tiles.
  if (!frame->participantId.empty() && frame->width > 0 && frame->height > 0 && !frame->rgba.empty()) {
    DecodedFrame& decoded = latestDecodedFrames_[frame->participantId];
    decoded.pixels = std::make_shared<const std::vector<std::uint8_t>>(frame->rgba);
    decoded.width = static_cast<int>(frame->width);
    decoded.height = static_cast<int>(frame->height);
    decoded.frameId = static_cast<std::int64_t>(frame->frameId);
  }

  const auto observedAtMs = runtimeElapsedMs();

  // The compositor already has the full-res frame (latestDecodedFrames_ above).
  // Stream only a small thumbnail as base64 for the WinUI multiview tiles so the
  // stdout payload stays tiny — full-res here causes a multi-MB-per-frame firehose.
  int thumbW = 0;
  int thumbH = 0;
  const auto thumb = downscaleBgraThumbnail(
      frame->rgba, static_cast<int>(frame->width), static_cast<int>(frame->height), 320, 180, thumbW, thumbH);
  pendingFrameEvents_.emplace_back(rpc::Json::Object{
      {"type", "zoom-video-frame"},
      {"frame",
       rpc::Json::Object{
           {"participantId", frame->participantId},
           {"width", thumbW},
           {"height", thumbH},
           {"frameId", static_cast<int>(frame->frameId)},
           {"observedAtMs", observedAtMs},
           {"bgraBase64", base64Encode(thumb.data(), thumb.size())},
       }},
  });
}

double ZoomEngineRuntime::runtimeElapsedMs() const {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt_).count());
}

}  // namespace corevideo::modules
