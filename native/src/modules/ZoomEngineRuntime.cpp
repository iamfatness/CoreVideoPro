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

constexpr double kFrameStaleAfterMs = 1000.0;

}  // namespace

ZoomEngineRuntime::ZoomEngineRuntime() : config_(loadConfig()), startedAt_(std::chrono::steady_clock::now()) {}

ZoomEngineRuntime::~ZoomEngineRuntime() {
  // Order matters: (1) stop the sender and drop anything still queued (the
  // process is going away — plan semantics: drop + log, never replay), but do
  // NOT join yet; (2) stopReader() terminates the engine process, which breaks
  // the pipe and unblocks a send that is mid-write on a wedged pipe; (3) only
  // then can the sender thread be joined without risking a hang.
  signalSenderStopAndDropQueue();
  stopReader();
  if (sender_.joinable()) {
    sender_.join();
  }
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
    std::lock_guard<std::mutex> lock(mutex_);
    if (process_ && process_->running()) {
      enqueueEngineSendLocked("leave", buildZoomEngineLeaveCommand());
    }
    state_.reset();
    initialized_ = false;
    mediaStarted_ = false;
    latestDecodedFrames_.clear();
    sentSubscriptions_.clear();  // a fresh join must re-subscribe from scratch
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
      // Async send (increment 3): the sender thread owns the pipe write. A send
      // failure is applied by the sender as an Error event with stage "init",
      // which the auth wait loop below observes (meetingState == "error").
      enqueueEngineSendLocked("init", buildZoomEngineInitCommand(initCommand));
      initialized_ = true;
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
    // Async send: a failed pipe write surfaces as an Error event (stage "join")
    // from the sender thread; the wait loop below returns on meetingState "error".
    enqueueEngineSendLocked("join", buildZoomEngineJoinCommand(command));
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
    enqueueEngineSendLocked("leave", buildZoomEngineLeaveCommand());
  }
  state_.reset();
  mediaStarted_ = false;
  latestDecodedFrames_.clear();
  sentSubscriptions_.clear();  // a rejoin must re-subscribe from scratch
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
    // Build THIS tick's desired subscription set, sending a subscribe command ONLY
    // for new or resolution-changed entries (not every tick — see sentSubscriptions_).
    std::map<std::string, int> desired;
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
      const auto kind = request.getString("kind");
      const auto purpose = request.getString("purpose");
      command.sourceUuid = kind + "-" + participantId + "-" + purpose;
      command.mode = kind == "screen-share" ? "screenshare" : "";
      // Resolution by purpose (0=360P, 1=720P, 2=1080P). TARGET is 1080p60 for EVERY
      // participant (product spec). But N concurrent 1080P raw subscriptions overloaded
      // the Zoom SDK (corevideo-zoom-engine ntdll 0xc000000d) and the CPU I420->BGRA path.
      // INTERIM until the GPU pipeline lands (GPU I420->BGRA + zero-copy composite, which
      // removes the CPU bottleneck and lets us pull all feeds at full res): the
      // active-speaker + screen share get 1080P (the program/feature candidate the user
      // explicitly wants full res), other multiview participants get 720P. The engine
      // downgrades further on per-feed failure.
      if (kind == "screen-share" || purpose == "active-speaker") {
        command.resolution = 2;  // 1080P
      } else {
        command.resolution = 1;  // 720P interim (target 1080P via the GPU pipeline)
      }
      // Audio subscriptions have no resolution concept; key them at -1 so a video
      // and an audio subscription for the same source don't alias.
      const int subscriptionKey = (kind == "participant-audio") ? -1 : command.resolution;
      desired[command.sourceUuid] = subscriptionKey;

      const auto existing = sentSubscriptions_.find(command.sourceUuid);
      if (existing != sentSubscriptions_.end() && existing->second == subscriptionKey) {
        continue;  // already subscribed at this resolution — don't re-send
      }
      // Increment 3: ENQUEUE for the dedicated sender thread instead of writing
      // the pipe here — syncSpine runs under coreMutex (via MediaCore), and a
      // slow/wedged engine pipe must never extend that hold. The dedup map is
      // updated at enqueue time; the single FIFO sender preserves order, so
      // "marked sent" still means "delivered exactly once, in order".
      if (kind == "participant-audio") {
        enqueueEngineSendLocked("subscribe", buildZoomEngineSubscribeAudioCommand(command));
      } else {
        enqueueEngineSendLocked("subscribe", buildZoomEngineSubscribeCommand(command));
      }
      sentSubscriptions_[command.sourceUuid] = subscriptionKey;
    }

    // Unsubscribe sources that were active but are no longer requested (participant
    // left / dropped from the show), then forget them.
    for (auto it = sentSubscriptions_.begin(); it != sentSubscriptions_.end();) {
      if (desired.find(it->first) == desired.end()) {
        enqueueEngineSendLocked("unsubscribe", buildZoomEngineUnsubscribeCommand(it->first));
        it = sentSubscriptions_.erase(it);
      } else {
        ++it;
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
    if (!decoded.i420 || decoded.width <= 0 || decoded.height <= 0) {
      continue;
    }
    VideoFrame frame;
    frame.participantId = participantId;
    frame.width = decoded.width;
    frame.height = decoded.height;
    frame.naturalWidth = decoded.width;
    frame.naturalHeight = decoded.height;
    frame.timestampMs = timestampMs;
    frame.i420 = decoded.i420;
    frame.i420Width = decoded.width;
    frame.i420Height = decoded.height;
    frame.frameId = decoded.frameId;
    frames.push_back(std::move(frame));
  }
  return frames;
}

bool ZoomEngineRuntime::ensureStartedLocked() {
  if (process_ && process_->running()) {
    return true;
  }

  // New engine process: retire lines queued for the previous (dead) process —
  // drop + log, never replay them into the new pipe — and forget per-process
  // session state so the new engine is re-initialized, re-media-started, and
  // re-subscribed from scratch.
  ++processGeneration_;
  purgeQueuedEngineSendsLocked("engine restart");
  sentSubscriptions_.clear();
  mediaStarted_ = false;

  process_ = std::make_shared<ZoomEngineProcessClient>();
  if (!process_->start({config_.executablePath, config_.connectTimeoutMs})) {
    state_.apply({ZoomEngineEventKind::Error, "error", "", "launch", process_->lastError()});
    return false;
  }
  // Record the token the client generated so frame SHM reads target the same
  // per-instance region names the engine writes.
  instanceToken_ = process_->instanceToken();
  initialized_ = false;
  startReaderLocked();
  startSenderLocked();
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

void ZoomEngineRuntime::startSenderLocked() {
  std::lock_guard<std::mutex> sendLock(sendMutex_);
  if (senderRunning_) {
    return;
  }
  senderRunning_ = true;
  // One sender for the runtime's lifetime: it does not depend on any particular
  // process instance (each queued line carries its own generation + the process
  // is re-resolved per line), so it survives engine restarts.
  sender_ = std::thread([this]() { senderLoop(); });
}

// Dedicated sender thread (phase 2 increment 3): the ONLY place engine stdin is
// written. Pops one line at a time (FIFO — ordering to the engine is preserved)
// and performs the potentially blocking pipe write with NO locks held, so a
// slow/wedged engine pipe blocks only this thread — never coreMutex, never
// mutex_, never the spine/command path.
void ZoomEngineRuntime::senderLoop() {
  for (;;) {
    PendingEngineSend item;
    {
      std::unique_lock<std::mutex> sendLock(sendMutex_);
      sendCv_.wait(sendLock, [&] { return !senderRunning_ || !sendQueue_.empty(); });
      if (!senderRunning_) {
        // Shutdown: drop whatever is still queued (the process is going away).
        if (!sendQueue_.empty()) {
          noteDroppedEngineSends(sendQueue_.size(), "shutdown");
          sendQueue_.clear();
        }
        return;
      }
      item = std::move(sendQueue_.front());
      sendQueue_.pop_front();
    }

    // Resolve the target process under mutex_ (brief; no I/O). The shared_ptr
    // copy keeps the client alive across the unlocked write even if the process
    // is replaced concurrently by a restart.
    std::shared_ptr<ZoomEngineProcessClient> process;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (item.generation == processGeneration_ && process_) {
        process = process_;
      }
    }
    if (!process || !process->running()) {
      // The process this line was built for died or was replaced: drop + log
      // (plan increment 3 semantics — never replay into a different process).
      noteDroppedEngineSends(1, "dead or replaced engine process");
      continue;
    }

    // Blocking pipe I/O — deliberately outside every lock. Concurrent
    // process_->stop() during shutdown is the same benign teardown race the
    // reader thread has always had with its unlocked readEvent(): the client
    // object is kept alive by the shared_ptr, and process termination makes the
    // in-flight write fail rather than hang.
    const bool sent = process->sendLine(item.line);
    if (!sent) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (item.generation == processGeneration_) {
        state_.apply({ZoomEngineEventKind::Error, "error", "", item.context, process->lastError()});
      }
    }
  }
}

void ZoomEngineRuntime::signalSenderStopAndDropQueue() {
  {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    senderRunning_ = false;
    if (!sendQueue_.empty()) {
      noteDroppedEngineSends(sendQueue_.size(), "shutdown");
      sendQueue_.clear();
    }
  }
  sendCv_.notify_all();
}

void ZoomEngineRuntime::enqueueEngineSendLocked(std::string context, std::string line) {
  if (!process_) {
    return;
  }
  {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    if (!senderRunning_) {
      noteDroppedEngineSends(1, "sender not running");
      return;
    }
    // Safety valve: with subscription dedup the steady-state queue is tiny; it
    // only grows without bound if the engine pipe is wedged, and then the whole
    // engine session is already broken — drop the OLDEST line (and log) rather
    // than grow forever. A restart clears the queue and re-subscribes anyway.
    constexpr std::size_t kMaxQueuedEngineSends = 1024;
    if (sendQueue_.size() >= kMaxQueuedEngineSends) {
      noteDroppedEngineSends(1, "queue overflow (wedged engine pipe?)");
      sendQueue_.pop_front();
    }
    sendQueue_.push_back({std::move(line), std::move(context), processGeneration_});
  }
  sendCv_.notify_one();
}

void ZoomEngineRuntime::purgeQueuedEngineSendsLocked(const char* reason) {
  std::size_t purged = 0;
  {
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    for (auto it = sendQueue_.begin(); it != sendQueue_.end();) {
      if (it->generation != processGeneration_) {
        it = sendQueue_.erase(it);
        ++purged;
      } else {
        ++it;
      }
    }
  }
  if (purged > 0) {
    noteDroppedEngineSends(purged, reason);
  }
}

void ZoomEngineRuntime::noteDroppedEngineSends(std::size_t count, const char* reason) {
  const auto total = droppedEngineSends_.fetch_add(count) + count;
  std::fprintf(stderr, "[zoom-engine] dropped %zu queued engine send(s): %s (total dropped %llu)\n",
               count, reason, static_cast<unsigned long long>(total));
}

void ZoomEngineRuntime::installEngineProcessForTest(std::shared_ptr<ZoomEngineProcessClient> process) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++processGeneration_;
  purgeQueuedEngineSendsLocked("engine restart");
  sentSubscriptions_.clear();
  mediaStarted_ = false;
  process_ = std::move(process);
  initialized_ = true;
  startSenderLocked();
}

std::size_t ZoomEngineRuntime::pendingEngineSendCountForTest() {
  std::lock_guard<std::mutex> sendLock(sendMutex_);
  return sendQueue_.size();
}

std::uint64_t ZoomEngineRuntime::droppedEngineSendCountForTest() const {
  return droppedEngineSends_.load();
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
  // Async send; optimistic. If the pipe write later fails, the sender applies an
  // Error event (stage "start_media"), and a process restart resets mediaStarted_
  // in ensureStartedLocked.
  enqueueEngineSendLocked("start_media", buildZoomEngineStartMediaCommand());
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
  if (!shm_region_open_read(region, zoomEngineVideoSharedMemoryName(event.sourceUuid, instanceToken_), size)) {
    state_.recordFrameIngestFailure(event.sourceUuid, event.participantId, "shared memory region could not be opened");
    return;
  }
  const auto closeRegion = [&region]() { shm_region_destroy(region); };
  // Snapshot the I420 planes for the GPU compositor and a small (<=640x360) BGRA
  // thumbnail for the WinUI base64 path. The expensive per-pixel I420->BGRA
  // convert at full resolution is gone — only the small thumbnail is converted on
  // the CPU; the compositor converts the I420 planes on the GPU in-shader.
  const auto frame = readZoomEngineI420FrameSnapshot(region.ptr, region.size, event.sourceUuid, event.participantId, 640, 360);
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

  // Tap the full-resolution I420 planes for the compositor without disturbing the
  // stdout/event queue below that feeds the WinUI multiview tiles.
  if (!frame->participantId.empty() && frame->i420Width > 0 && frame->i420Height > 0 && !frame->i420.empty()) {
    DecodedFrame& decoded = latestDecodedFrames_[frame->participantId];
    decoded.i420 = std::make_shared<const std::vector<std::uint8_t>>(frame->i420);
    decoded.width = static_cast<int>(frame->i420Width);
    decoded.height = static_cast<int>(frame->i420Height);
    decoded.frameId = static_cast<std::int64_t>(frame->frameId);
  }

  const auto observedAtMs = runtimeElapsedMs();

  // The compositor already has the full-res I420 frame (latestDecodedFrames_
  // above). The snapshot already produced a downscaled BGRA thumbnail (capped at
  // 640x360) for the WinUI monitors; stream it directly as base64.
  const int thumbW = static_cast<int>(frame->width);
  const int thumbH = static_cast<int>(frame->height);
  const auto& thumb = frame->rgba;
  // LATEST-WINS: this queue is drained by the render thread, which gets starved by
  // command processing (media-core-sync holds the core lock). Unbounded, it
  // accumulated tens of seconds of stale frames (the "10s+ latency"). Cap it and
  // drop the oldest so the WinUI always gets near-current frames.
  constexpr std::size_t kMaxPendingZoomFrameEvents = 16;  // ~2 frames x up to 8 participants
  if (pendingFrameEvents_.size() >= kMaxPendingZoomFrameEvents) {
    pendingFrameEvents_.erase(pendingFrameEvents_.begin());
  }
  pendingFrameEvents_.emplace_back(rpc::Json::Object{
      {"type", "zoom-video-frame"},
      {"frame",
       rpc::Json::Object{
           {"participantId", frame->participantId},
           {"width", thumbW},
           {"height", thumbH},
           {"frameId", static_cast<int>(frame->frameId)},
           {"observedAtMs", observedAtMs},
           {"emitWallMs", static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count())},
           {"bgraBase64", base64Encode(thumb.data(), thumb.size())},
       }},
  });
}

double ZoomEngineRuntime::runtimeElapsedMs() const {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt_).count());
}

}  // namespace corevideo::modules
