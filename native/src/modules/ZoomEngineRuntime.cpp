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
  // Stop the video-ingest thread FIRST: it takes mutex_ briefly and touches
  // SHM regions that teardown below releases.
  videoIngestRun_.store(false, std::memory_order_release);
  if (videoIngestThread_.joinable()) {
    videoIngestThread_.join();
  }
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
    pendingAudio_.clear();
    closeAudioStreamsLocked();
  closeVideoStreamsLocked();
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

  if (!ensureStarted()) {
    std::lock_guard<std::mutex> lock(mutex_);
    return rawCaptureSnapshotLocked();
  }

  bool waitForAuth = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!process_ || !process_->running()) {
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
  pendingAudio_.clear();
  closeAudioStreamsLocked();
  closeVideoStreamsLocked();
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
        // ISO audio: each participant's audio target carries THAT participant's
        // one-way stream, so the mixer gets a real per-channel signal (faders,
        // mutes, meters per participant). Without isolate every target receives
        // the same meeting mix N times over.
        command.isolateAudio = true;
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
  // Frames are ingested by the dedicated video-ingest thread; this poll just
  // returns published state (pixel work on the render tick collapsed the
  // audio worker to 8 ticks/s in soak run 15).
  std::lock_guard<std::mutex> lock(mutex_);
  return state_.pollCompositorVideoFrames(timestampMs);
}

std::vector<AudioFrame> ZoomEngineRuntime::pollCompositorAudioFrames(int64_t timestampMs) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Z2b: steady-state ring drains happen HERE at the poll cadence (50Hz),
  // with regions held open - decoupled from pipe-event timing entirely.
  for (auto& [uuid, ref] : audioStreams_) {
    drainAudioStreamLocked(uuid, ref);
  }
  auto frames = state_.pollCompositorAudioFrames(timestampMs);
  // Overlay real decoded PCM: a participant with pending audio gets ONE
  // coalesced PCM frame (all samples ingested since the last poll), replacing
  // the metadata-only placeholder the state emits from packet counters.
  // Participants without pending PCM keep their placeholder so the mixer
  // roster stays complete (their meters just hold at silence).
  for (auto& [participantId, pending] : pendingAudio_) {
    if (pending.pcm.empty() || pending.channels <= 0 || pending.sampleRate <= 0) {
      continue;
    }
    AudioFrame frame;
    frame.participantId = participantId;
    frame.sampleRate = pending.sampleRate;
    frame.channels = pending.channels;
    frame.timestampMs = timestampMs;
    frame.sampleCount = static_cast<int>(pending.pcm.size() / static_cast<std::size_t>(pending.channels));
    frame.pcm = std::move(pending.pcm);
    pending.pcm.clear();  // defined-empty after the move
    const auto existing = std::find_if(frames.begin(), frames.end(), [&](const AudioFrame& candidate) {
      return candidate.participantId == frame.participantId;
    });
    if (existing != frames.end()) {
      *existing = std::move(frame);
    } else {
      frames.push_back(std::move(frame));
    }
  }
  return frames;
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

bool ZoomEngineRuntime::ensureStarted() {
  std::shared_ptr<ZoomEngineProcessClient> client;
  std::string executablePath;
  int connectTimeoutMs = 0;
  std::uint64_t generationAtStart = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (process_ && process_->running()) {
      return true;
    }

    // New engine process: retire lines queued for the previous (dead) process —
    // drop + log, never replay them into the new pipe — and forget per-process
    // session state so the new engine is re-initialized, re-media-started, and
    // re-subscribed from scratch.
    ++processGeneration_;
    generationAtStart = processGeneration_;
    purgeQueuedEngineSendsLocked("engine restart");
    sentSubscriptions_.clear();
    mediaStarted_ = false;
    client = std::make_shared<ZoomEngineProcessClient>();
    executablePath = config_.executablePath;
    connectTimeoutMs = config_.connectTimeoutMs;
  }

  // BLOCKING: CreateProcess + engine IPC connect (seconds; bounded by
  // connectTimeoutMs). Runs with NO runtime lock held so the render/audio
  // threads' per-tick frame polls (which take mutex_) never stall behind the
  // spawn — the studio keeps compositing while the engine boots. Together with
  // the RPC server routing zoom-join around coreMutex, this closes the
  // "whole studio freezes for the length of every join" P0.
  const bool started = client->start({executablePath, connectTimeoutMs});

  std::lock_guard<std::mutex> lock(mutex_);
  if (processGeneration_ != generationAtStart) {
    // Superseded mid-start (test process installed / another restart): discard
    // the freshly spawned process rather than clobbering the newer one.
    client->stop();
    return process_ && process_->running();
  }
  if (!started) {
    state_.apply({ZoomEngineEventKind::Error, "error", "", "launch", client->lastError()});
    return false;
  }
  process_ = std::move(client);
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
  if (event.kind == ZoomEngineEventKind::Audio) {
    ingestAudioEventLocked(event);
  }
}

void ZoomEngineRuntime::applyEngineEventForTest(const ZoomEngineEvent& event) { applyEvent(event); }

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
  // in ensureStarted.
  enqueueEngineSendLocked("start_media", buildZoomEngineStartMediaCommand());
  mediaStarted_ = true;
  return true;
}

void ZoomEngineRuntime::enqueueFrameEventLocked(const ZoomEngineEvent& event) {
  if (event.sourceUuid.empty() || event.participantId == 0 || event.width == 0 || event.height == 0) {
    state_.recordFrameIngestFailure(event.sourceUuid, event.participantId, "frame event missing source, participant, width, or height");
    return;
  }
  // Video-beacon fix: the event REGISTERS the stream (and primes an immediate
  // read); steady-state frame reads happen on the render poll with the region
  // held open, gated by a header-sequence peek. Per-event full-frame copies
  // under the core lock were the measured queue-drowning source
  // (zoom-media-spine-sync queueWait 3.7s in soak run 10).
  auto& ref = videoStreams_[event.sourceUuid];
  if (ref.width != event.width || ref.height != event.height) {
    ref.regionOpaque.reset();  // shared_ptr deleter closes the mapping
    ref.lastSequence = 0;
  }
  ref.participantId = event.participantId;
  ref.width = event.width;
  ref.height = event.height;
  ensureVideoIngestThreadLocked();
}

void ZoomEngineRuntime::ensureVideoIngestThreadLocked() {
  if (videoIngestRun_.load(std::memory_order_acquire)) {
    return;
  }
  videoIngestRun_.store(true, std::memory_order_release);
  videoIngestThread_ = std::thread([this] { videoIngestLoop(); });
}

void ZoomEngineRuntime::videoIngestLoop() {
  while (videoIngestRun_.load(std::memory_order_acquire)) {
    drainVideoStreamsThreePhase();
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
}

void ZoomEngineRuntime::drainVideoStreamsThreePhase() {
  // Phase 1 (locked, cheap): open missing regions, peek sequences, collect
  // the streams that have a NEW complete frame. shared_ptr region holders let
  // phase 2 read safely even if a leave/reset drops the stream meanwhile.
  struct SnapshotJob {
    std::string uuid;
    std::shared_ptr<void> holder;
    ShmRegion* region = nullptr;
    std::uint32_t participantId = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t sequence = 0;
    bool buildThumbnail = false;
  };
  // Thumbnail-event pace: ~2/s per participant is plenty for the shell's roster
  // thumbs; the full-res I420 tap below feeds the compositor EVERY frame.
  constexpr std::int64_t kThumbnailEmitIntervalMs = 500;
  std::vector<SnapshotJob> jobs;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [uuid, ref] : videoStreams_) {
      if (ref.width == 0 || ref.height == 0 || ref.participantId == 0) {
        continue;
      }
      if (!ref.regionOpaque) {
        auto* opened = new ShmRegion();
        if (!shm_region_open_read(*opened, zoomEngineVideoSharedMemoryName(uuid, instanceToken_),
                                  zoomEngineI420FrameByteSize(ref.width, ref.height))) {
          delete opened;
          continue;  // not created yet; retry next poll
        }
        ref.regionOpaque = std::shared_ptr<void>(opened, [](void* pointer) {
          auto* region = static_cast<ShmRegion*>(pointer);
          shm_region_destroy(*region);
          delete region;
        });
      }
      auto* region = static_cast<ShmRegion*>(ref.regionOpaque.get());
      const auto sequence = readZoomEngineI420FrameSequence(region->ptr, region->size);
      if (sequence == 0 || (sequence & 1u) != 0 || sequence == ref.lastSequence) {
        continue;  // never written, mid-write, or unchanged: 16-byte cost
      }
      const auto nowMs = runtimeElapsedMs();
      const bool buildThumbnail = ref.lastThumbnailEmitMs < 0 ||
                                  nowMs - ref.lastThumbnailEmitMs >= kThumbnailEmitIntervalMs;
      jobs.push_back({uuid, ref.regionOpaque, region, ref.participantId, ref.width, ref.height, sequence,
                      buildThumbnail});
    }
  }

  // Phase 2 (UNLOCKED, heavy): full I420 copy + thumbnail conversion per new
  // frame. The seqlock inside the snapshot re-validates against tearing.
  struct SnapshotResult {
    SnapshotJob job;
    std::optional<ZoomEngineRgbaFrame> frame;
  };
  std::vector<SnapshotResult> results;
  results.reserve(jobs.size());
  for (auto& job : jobs) {
    results.push_back({job, readZoomEngineI420FrameSnapshot(job.region->ptr, job.region->size, job.uuid,
                                                            job.participantId, 640, 360,
                                                            job.buildThumbnail)});
  }

  // Phase 3 (locked, cheap): publish.
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& result : results) {
    auto stream = videoStreams_.find(result.job.uuid);
    if (stream == videoStreams_.end()) {
      continue;  // stream left while we were reading
    }
    if (!result.frame) {
      state_.recordFrameIngestFailure(result.job.uuid, result.job.participantId,
                                      "shared memory snapshot was incomplete, stale, or malformed");
      continue;
    }
    stream->second.lastSequence = result.job.sequence;
    publishVideoFrameLocked(result.job.uuid, stream->second, *result.frame);
  }
}

void ZoomEngineRuntime::publishVideoFrameLocked(const std::string& uuid, VideoStreamRef& ref,
                                                const ZoomEngineRgbaFrame& frame) {
  state_.recordFrameIngestSuccess(uuid, ref.participantId, ref.width, ref.height, frame.frameId,
                                  runtimeElapsedMs());

  // Tap the full-resolution I420 planes for the compositor without disturbing
  // the stdout/event queue below that feeds the WinUI multiview tiles.
  if (!frame.participantId.empty() && frame.i420Width > 0 && frame.i420Height > 0 && !frame.i420.empty()) {
    DecodedFrame& decoded = latestDecodedFrames_[frame.participantId];
    decoded.i420 = std::make_shared<const std::vector<std::uint8_t>>(frame.i420);
    decoded.width = static_cast<int>(frame.i420Width);
    decoded.height = static_cast<int>(frame.i420Height);
    decoded.frameId = static_cast<std::int64_t>(frame.frameId);
  }

  // Thumbnail-throttled frames carry only the I420 tap (above) — no event. The
  // pace decision lives in the phase-1 peek (VideoStreamRef.lastThumbnailEmitMs).
  if (frame.rgba.empty()) {
    return;
  }
  ref.lastThumbnailEmitMs = runtimeElapsedMs();

  const auto observedAtMs = runtimeElapsedMs();
  const int thumbW = static_cast<int>(frame.width);
  const int thumbH = static_cast<int>(frame.height);
  const auto& thumb = frame.rgba;
  // LATEST-WINS cap (see the pre-beacon history: unbounded, this queue once
  // accumulated tens of seconds of stale frames).
  constexpr std::size_t kMaxPendingZoomFrameEvents = 16;
  if (pendingFrameEvents_.size() >= kMaxPendingZoomFrameEvents) {
    pendingFrameEvents_.erase(pendingFrameEvents_.begin());
  }
  pendingFrameEvents_.emplace_back(rpc::Json::Object{
      {"type", "zoom-video-frame"},
      {"frame",
       rpc::Json::Object{
           {"participantId", frame.participantId},
           {"width", thumbW},
           {"height", thumbH},
           {"frameId", static_cast<int>(frame.frameId)},
           {"observedAtMs", observedAtMs},
           {"emitWallMs", static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count())},
           {"bgraBase64", base64Encode(thumb.data(), thumb.size())},
       }},
  });
}

void ZoomEngineRuntime::ingestAudioEventLocked(const ZoomEngineEvent& event) {
  // Two Zoom audio stream kinds (owner requirement 2026-07-05):
  //  - ISO per-participant streams ("participant-audio-*", isolate_audio) -
  //    NOTE Zoom gates these server-side (silence suppression: packets stop
  //    and resume between talk bursts), so they are inherently choppy for
  //    non-active speakers.
  //  - The MEETING MIX (the engine mirrors mixed audio onto the active-
  //    speaker video target) - continuous, Zoom-processed program audio.
  //    Ingested as the dedicated "zoom-mix" source so it gets its own
  //    mixer row and routing. Other video-target mirrors stay dropped
  //    (ingesting every mirror would sum the same signal repeatedly).
  const bool isIso = event.sourceUuid.rfind("participant-audio-", 0) == 0;
  static const std::string kMixSuffix = "-active-speaker";
  const bool isMix = event.sourceUuid.size() > kMixSuffix.size() &&
                     event.sourceUuid.compare(event.sourceUuid.size() - kMixSuffix.size(),
                                              kMixSuffix.size(), kMixSuffix) == 0;
  if (event.participantId == 0 || event.byteLength == 0 || (!isIso && !isMix)) {
    return;
  }

  // Z2b: the pipe event only REGISTERS the stream (and primes an immediate
  // drain); steady-state draining happens on the 50Hz poll with the region
  // held open - the per-event open/drain/close cycle lost hundreds of
  // packets per source at 500 events/s (soak-measured).
  if (isMix && event.sourceUuid != mixStreamUuid_) {
    // ONE live mix stream at a time: two concurrent -active-speaker streams
    // draining into pendingAudio_["zoom-mix"] interleave two different
    // signals packet-by-packet (soak run 11: phase chaos at every packet
    // seam). Speaker changes hand the mix over sequentially instead.
    if (!mixStreamUuid_.empty()) {
      auto previous = audioStreams_.find(mixStreamUuid_);
      if (previous != audioStreams_.end()) {
        if (previous->second.regionOpaque != nullptr) {
          auto* stale = static_cast<ShmRegion*>(previous->second.regionOpaque);
          shm_region_destroy(*stale);
          delete stale;
        }
        audioStreams_.erase(previous);
      }
    }
    mixStreamUuid_ = event.sourceUuid;
  }
  auto& ref = audioStreams_[event.sourceUuid];
  ref.pendingKey = isIso ? std::to_string(event.participantId) : std::string("zoom-mix");
  drainAudioStreamLocked(event.sourceUuid, ref);
}

// Z2b: drain every packet available in one stream's ring into its pending
// buffer. Called on the 50Hz poll with the region held OPEN - the previous
// per-pipe-event open/drain/close cycle could not keep up with 100 events/s
// per stream and the rings lapped the reader (soak-measured: hundreds of
// packets lost per source = tick-sized phase splices).
void ZoomEngineRuntime::drainAudioStreamLocked(const std::string& uuid, AudioStreamRef& ref) {
  if (ref.regionOpaque == nullptr) {
    auto* region = new ShmRegion();
    if (!shm_region_open_read(*region, zoomEngineAudioSharedMemoryName(uuid, instanceToken_),
                              zoomEngineAudioRingByteSize())) {
      delete region;
      return;  // ring not created yet; retry next poll
    }
    ref.regionOpaque = region;
  }
  auto* region = static_cast<ShmRegion*>(ref.regionOpaque);
  auto& pending = pendingAudio_[ref.pendingKey];
  std::vector<ZoomEnginePcmAudioChunk> chunks;
  const auto lost = readZoomEnginePcmAudioRing(region->ptr, region->size, pending.nextReadCounter, chunks);
  if (lost > 0) {
    const auto before = pending.lostPackets;
    pending.lostPackets += static_cast<std::int64_t>(lost);
    if (before == 0 || (before / 100) != (pending.lostPackets / 100)) {
      std::fprintf(stderr, "[zoom-audio] stream %s lost %zu packet(s) (total %lld)\n",
                   uuid.c_str(), lost, static_cast<long long>(pending.lostPackets));
    }
  }
  // Diagnostic ring tap (env-gated like the mix taps): chunk PCM exactly as
  // decoded off the ring, BEFORE pending/feed - splits writer/ring corruption
  // from downstream (phase forensics showed whole-packet skips with zero
  // counted losses; this tap decides which side of the ring they enter).
  static const char* ringTapDir = std::getenv("COREVIDEO_AUDIO_DEBUG_DIR");
  if (ringTapDir != nullptr && !chunks.empty()) {
    static std::map<std::string, FILE*> s_ringTapFiles;
    const std::string path = std::string(ringTapDir) + "/tap-ring-" + ref.pendingKey + ".f32";
    auto it = s_ringTapFiles.find(path);
    if (it == s_ringTapFiles.end()) {
      it = s_ringTapFiles.emplace(path, std::fopen(path.c_str(), "ab")).first;
    }
    if (FILE* file = it->second) {
      for (const auto& chunk : chunks) {
        std::fwrite(chunk.pcm.data(), sizeof(float), chunk.pcm.size(), file);
      }
    }
  }
  constexpr std::size_t kMaxPendingAudioSamplesPerChannel = 48000;
  for (const auto& chunk : chunks) {
    if (appendZoomEnginePcmChunk(pending, chunk, kMaxPendingAudioSamplesPerChannel)) {
      ++pending.ingestedChunks;
      if (pending.ingestedChunks == 1 || pending.ingestedChunks % 3000 == 0) {
        std::fprintf(stderr, "[zoom-audio] stream %s chunk #%lld rate=%d ch=%d pending=%zu\n",
                     uuid.c_str(), static_cast<long long>(pending.ingestedChunks), chunk.sampleRate,
                     chunk.channels, pending.pcm.size());
      }
    }
  }
}

void ZoomEngineRuntime::closeVideoStreamsLocked() {
  // shared_ptr holders: the deleter destroys each region at last release
  // (possibly after an in-flight unlocked snapshot completes - safe).
  videoStreams_.clear();
}

void ZoomEngineRuntime::closeAudioStreamsLocked() {
  for (auto& [uuid, ref] : audioStreams_) {
    if (ref.regionOpaque != nullptr) {
      auto* region = static_cast<ShmRegion*>(ref.regionOpaque);
      shm_region_destroy(*region);
      delete region;
      ref.regionOpaque = nullptr;
    }
  }
  audioStreams_.clear();
}

double ZoomEngineRuntime::runtimeElapsedMs() const {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startedAt_).count());
}

}  // namespace corevideo::modules
