#include "rpc/JsonRpcServer.h"

#include "core/LockHoldGuardrail.h"
#include "rpc/CommandMailbox.h"
#include "contracts/Lifecycle.h"
#include <random>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>
#endif

namespace corevideo::rpc {
namespace {

Json requestId(const Json& request) {
  const Json* id = request.get("id");
  return id ? *id : Json("unknown");
}

bool hasType(const Json& request, const std::string& type) {
  return request.getString("type") == type;
}

Json::Array commandBatch(const Json& request) {
  const Json* commands = request.get("commands");
  if (commands && commands->isArray()) {
    return commands->asArray();
  }
  return Json::Array{request};
}

}  // namespace

JsonRpcServer::JsonRpcServer(core::MediaCore& mediaCore, JoinHandler joinHandler)
    : mediaCore_(mediaCore), joinHandler_(std::move(joinHandler)) {
  std::random_device random;
  processEpoch_ = std::to_string(random()) + "-" + std::to_string(random());
}

Json JsonRpcServer::handshake() const {
  return Json::Object{
      {"id", "handshake"},
      {"ok", true},
      {"type", "handshake"},
      {"profile", mediaCore_.profile()},
      {"protocolVersion", contracts::toJson(contracts::ProtocolVersion{1, 0})},
      {"processEpoch", processEpoch_},
  };
}

Json JsonRpcServer::handle(const Json& request) {
  const Json id = requestId(request);
  const std::string type = request.getString("type");
  if (const auto* version = request.get("protocolVersion"); version && !contracts::validateProtocolVersion(*version)) {
    return failure(id, "incompatible-protocol", "Unsupported protocol version; this core supports major 1.");
  }
  if (type.empty()) {
    return failure(id, "protocol-error", "Request is missing a type field.");
  }

  if (hasType(request, "handshake")) {
    return success(id, Json::Object{{"type", "handshake"}, {"profile", mediaCore_.profile()},
        {"protocolVersion", contracts::toJson(contracts::ProtocolVersion{1, 0})}, {"processEpoch", processEpoch_}});
  }

  if (hasType(request, "ping")) {
    return success(id, Json::Object{{"type", "ping"}});
  }

  if (hasType(request, "zoom-media-spine-sync")) {
    const Json* payload = request.get("spinePayload");
    if (!payload) {
      payload = request.get("payload");
    }
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "Zoom media spine sync request must include payload.");
    }
    const auto snapshot = mediaCore_.syncZoomMediaSpine(*payload, request.get("elapsedMs") ? request.get("elapsedMs")->asNumber() : 0);
    return success(id, Json::Object{
                           {"type", "zoom-media-spine-sync"},
                           {"spineSnapshot", snapshot},
                           {"zoom", snapshot},
                       });
  }

  if (hasType(request, "zoom-join")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "zoom-join requires a payload.");
    }
    return success(id, Json::Object{
                           {"type", "zoom-join"},
                           {"snapshot", mediaCore_.joinZoom(*payload)},
                       });
  }

  if (hasType(request, "zoom-leave") || hasType(request, "zoom-cancel")) {
    return success(id, Json::Object{
                           {"type", "zoom-leave"},
                           {"snapshot", mediaCore_.leaveZoom()},
                       });
  }

  // Capture-off: stop Zoom raw media (recording indicator + frames) while
  // staying in the meeting. Non-blocking — the engine command is enqueued for
  // ZoomEngineRuntime's sender thread, so handling this under coreMutex is
  // sub-ms; the snapshot carries the engine-reported `rawMediaActive`.
  if (hasType(request, "zoom-stop-capture")) {
    return success(id, Json::Object{
                           {"type", "zoom-stop-capture"},
                           {"snapshot", mediaCore_.stopZoomCapture()},
                       });
  }

  if (hasType(request, "zoom-snapshot")) {
    return success(id, Json::Object{
                           {"type", "zoom-snapshot"},
                           {"snapshot", mediaCore_.zoomSnapshot()},
                       });
  }

  if (hasType(request, "media-core-sync") || hasType(request, "native-media-core-sync") || request.get("commands")) {
    const double elapsedMs = request.get("elapsedMs") ? request.get("elapsedMs")->asNumber() : 0.0;
    return success(id, Json::Object{
                           {"type", hasType(request, "media-core-sync") ? "media-core-sync" : "native-media-core-sync"},
                           {"snapshot", mediaCore_.applyCommands(commandBatch(request), elapsedMs)},
                       });
  }

  if (hasType(request, "snapshot")) {
    return success(id, Json::Object{{"snapshot", mediaCore_.sessionState()}});
  }

  if (hasType(request, "get-output-health")) {
    return success(id, Json::Object{{"health", mediaCore_.health()}});
  }

  if (hasType(request, "get-output-session")) {
    return success(id, Json::Object{{"session", mediaCore_.sessionState()}});
  }

  if (hasType(request, "list-capture-devices")) {
    return success(id, Json::Object{{"type", "capture-devices"}, {"devices", mediaCore_.captureDevices()}});
  }

  if (hasType(request, "select-capture-input")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "select-capture-input requires a payload.");
    }
    return success(id, Json::Object{{"type", "capture-devices"}, {"devices", mediaCore_.selectCaptureInput(payload->getString("deviceId"), payload->getString("inputId"))}});
  }

  if (hasType(request, "set-capture-audio-sync-offset")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "set-capture-audio-sync-offset requires a payload.");
    }
    const Json* offset = payload->get("offsetMs");
    return success(id, Json::Object{{"type", "capture-devices"}, {"devices", mediaCore_.setCaptureAudioSyncOffset(payload->getString("deviceId"), offset ? static_cast<int>(offset->asNumber()) : 0)}});
  }

  if (hasType(request, "connect-capture-device")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "connect-capture-device requires a payload.");
    }
    return success(id, Json::Object{{"type", "capture-devices"},
                                    {"devices", mediaCore_.connectCaptureDevice(
                                                    payload->getString("deviceId"),
                                                    payload->getString("outputSourceId"))}});
  }

  if (hasType(request, "disconnect-capture-device")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "disconnect-capture-device requires a payload.");
    }
    return success(id, Json::Object{{"type", "capture-devices"}, {"devices", mediaCore_.disconnectCaptureDevice(payload->getString("deviceId"))}});
  }

  if (hasType(request, "register-capture-shm")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "register-capture-shm requires a payload.");
    }
    mediaCore_.registerCaptureShm(
        payload->getString("deviceId"),
        payload->getString("shmName"),
        static_cast<int>(payload->getNumber("width")),
        static_cast<int>(payload->getNumber("height")));
    return success(id, Json::Object{{"type", "ack"}});
  }

  if (hasType(request, "browser-add")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "browser-add requires a payload.");
    }
    std::string error;
    const Json state = mediaCore_.addBrowserSource(*payload, error);
    if (!error.empty()) {
      return failure(id, "browser-error", error);
    }
    return success(id, Json::Object{{"type", "browser-sources"}, {"browserSources", state}});
  }

  if (hasType(request, "browser-remove")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "browser-remove requires a payload.");
    }
    std::string error;
    const Json state = mediaCore_.removeBrowserSource(payload->getString("browserId"), error);
    if (!error.empty()) {
      return failure(id, "browser-error", error);
    }
    return success(id, Json::Object{{"type", "browser-sources"}, {"browserSources", state}});
  }

  if (hasType(request, "browser-reload")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "browser-reload requires a payload.");
    }
    std::string error;
    const Json state = mediaCore_.reloadBrowserSource(payload->getString("browserId"), error);
    if (!error.empty()) {
      return failure(id, "browser-error", error);
    }
    return success(id, Json::Object{{"type", "browser-sources"}, {"browserSources", state}});
  }

  if (hasType(request, "unregister-capture-shm")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "unregister-capture-shm requires a payload.");
    }
    mediaCore_.unregisterCaptureShm(payload->getString("deviceId"));
    return success(id, Json::Object{{"type", "ack"}});
  }

  // A2: state pull needs a REAL response payload (the base64 blob), so it gets
  // a dedicated route instead of the applyCommands snapshot path. Control
  // plane: the state round trip runs on this command thread against the
  // host's dedicated state events, never the audio exchange.
  if (hasType(request, "get-vst-state")) {
    const Json state = mediaCore_.getVstInsertState(request);
    const std::string error = state.getString("error");
    if (!error.empty()) {
      return failure(id, "vst-state-error", error);
    }
    return success(id, Json::Object{{"type", "vst-state"}, {"state", state}});
  }

  // A2: slider drags send set-vst-param at gesture rate — a cheap ack instead
  // of a full snapshot rebuild per tick. Both handlers touch only the plugin
  // host leaf mutexes (no coreMutex), so they are safe on this thread.
  if (hasType(request, "set-vst-param")) {
    mediaCore_.setVstInsertParam(request);
    return success(id, Json::Object{{"type", "ack"}});
  }
  if (hasType(request, "set-vst-state")) {
    mediaCore_.setVstInsertState(request);
    return success(id, Json::Object{{"type", "ack"}});
  }

  // A1 regression guard (owner-reported "Open controls shows no plugin UI,
  // ever"): the shell sends open-vst-editor (and can send scan-vst-plugins) as
  // a TOP-LEVEL request, not inside a media-core-sync commands batch. Before
  // these types were routed here the request fell through to the
  // protocol-error below — a silently swallowed rejection, so the editor
  // command never reached MediaCore::openVstPluginEditor. (set-vst-param /
  // set-vst-state have their own cheap-ack routes above.)
  if (hasType(request, "start-program-output") || hasType(request, "load-scene-graph") ||
      hasType(request, "set-participant-transform") || hasType(request, "set-overlay-asset") ||
      hasType(request, "open-vst-editor") || hasType(request, "scan-vst-plugins")) {
    return success(id, Json::Object{{"snapshot", mediaCore_.applyCommands(commandBatch(request))}});
  }

  return failure(id, "protocol-error", "Unsupported native media-core command: " + type);
}

void JsonRpcServer::run(std::istream& input, std::ostream& output) {
  // Decoupled I/O: a dedicated reader thread keeps draining stdin (so the host's
  // writes never block on a full OS pipe), a dedicated writer thread serializes
  // all stdout (so a slow consumer can never stall processing), and this thread
  // owns all command handling (preserving single-threaded access to mediaCore_).
  // Previously this was one synchronous read->handle->write->flush loop, so a
  // blocking handler or a full stdout buffer would stop reading stdin and
  // deadlock the entire host<->core channel.
  //
  // CRITICAL: untie the input stream from the output stream. By default std::cin
  // is tied to std::cout, so every getline() first flushes cout. When stdout is
  // backed up (host reading slower than the frame stream), the reader thread's
  // getline would block on that flush and stop draining stdin — re-creating the
  // deadlock this threading exists to prevent. sync_with_stdio(false) also drops
  // the C stdio sync overhead on this hot path.
  std::ios_base::sync_with_stdio(false);
  input.tie(nullptr);
  // Two output lanes on one writer: command responses (high priority) are never
  // stuck behind the high-rate frame-preview stream (low priority, bounded and
  // droppable). Frame events are pumped on a throttled cadence off the per-command
  // path so command throughput stays high even while the compositor streams at
  // 60fps. Sharing one stdout channel without this lets frame data starve RPC
  // responses, so the host's commands time out and the channel appears dead.
  // Keep the preview queue SHALLOW: it is latest-wins (drop-oldest below), so a deep
  // queue just adds latency — the monitor would show frames seconds behind reality.
  // A few frames absorbs jitter while keeping the live feed low-latency.
  constexpr std::size_t kMaxPendingFrameEvents = 6;
  constexpr auto kFramePumpInterval = std::chrono::milliseconds(33);  // ~30fps (kept: faster starves RPC)

  std::mutex outMx;
  std::condition_variable outCv;
  // INSTRUMENTATION: each queued message carries its enqueue timestamp so the
  // writer can report a response's "sojourn" (enqueue -> actually written). This is
  // the key signal that distinguishes "the core was slow to handle" (queue-wait +
  // handle, measured on the command loop below) from "the response was generated
  // fast but stuck in the output queue / blocked in flush() because the stdout pipe
  // is backed up by the high-rate base64 frame stream and a slow consumer".
  using Stamp = std::chrono::steady_clock::time_point;
  std::deque<std::pair<std::string, Stamp>> outHi;  // command responses / failures
  std::deque<std::pair<std::string, Stamp>> outLo;  // frame-preview events (droppable)
  std::atomic<bool> stopping{false};

  auto enqueueResponse = [&](std::string message) {
    {
      std::lock_guard<std::mutex> lock(outMx);
      outHi.emplace_back(std::move(message), std::chrono::steady_clock::now());
    }
    outCv.notify_one();
  };
  auto enqueueFrame = [&](std::string message) {
    {
      std::lock_guard<std::mutex> lock(outMx);
      if (outLo.size() >= kMaxPendingFrameEvents) {
        outLo.pop_front();  // drop oldest frame; preview is latest-wins
      }
      outLo.emplace_back(std::move(message), std::chrono::steady_clock::now());
    }
    outCv.notify_one();
  };

  std::thread writer([&] {
    std::unique_lock<std::mutex> lock(outMx);
    for (;;) {
      outCv.wait(lock, [&] { return !outHi.empty() || !outLo.empty() || stopping.load(); });
      while (!outHi.empty() || !outLo.empty()) {
        std::string message;
        Stamp enqueuedAt;
        bool isResponse;
        std::size_t hiDepth = outHi.size();
        std::size_t loDepth = outLo.size();
        if (!outHi.empty()) {
          message = std::move(outHi.front().first);
          enqueuedAt = outHi.front().second;
          outHi.pop_front();
          isResponse = true;
        } else {
          message = std::move(outLo.front().first);
          enqueuedAt = outLo.front().second;
          outLo.pop_front();
          isResponse = false;
        }
        lock.unlock();
        const auto wStart = std::chrono::steady_clock::now();
        const auto sojournMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(wStart - enqueuedAt).count();
        output << message << '\n';
        output.flush();
        const auto flushMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - wStart)
                                 .count();
        // A response that sat >500ms before it could be written, or any single
        // write that blocked >200ms in flush(), means the stdout pipe is the
        // bottleneck (slow consumer / frame flood), NOT the core's handler.
        if ((isResponse && sojournMs >= 500) || flushMs >= 200) {
          std::fprintf(stderr,
                       "[writer] %s sojourn=%lldms flush=%lldms bytes=%zu hiDepth=%zu loDepth=%zu\n",
                       isResponse ? "RESPONSE" : "frame", static_cast<long long>(sojournMs),
                       static_cast<long long>(flushMs), message.size(), hiDepth, loDepth);
        }
        lock.lock();
      }
      if (stopping.load() && outHi.empty() && outLo.empty()) {
        break;
      }
    }
  });

  std::mutex inMx;
  std::condition_variable inCv;
  CommandMailbox inQ;
  std::atomic<bool> inputClosed{false};
  // The reader parses once, rejects overload explicitly, and never guesses command
  // types from text inside a payload. getline's individual-line limit is handled
  // below before JSON parsing; queued wire storage has a separate byte bound.
  std::thread reader([&] {
    std::string line;
    // A 1 MiB VST state blob grows under base64; leave room for its envelope.
    constexpr std::size_t maxLineBytes = 4 * 1024 * 1024;
    while (input.good()) {
      line.clear();
      bool oversized = false;
      char ch;
      while (input.get(ch) && ch != '\n') {
        if (line.size() < maxLineBytes) line.push_back(ch);
        else oversized = true;
      }
      if (oversized) {
        enqueueResponse(failure(Json("unknown"), "request-too-large", "Control request exceeds 4 MiB.").stringify());
        continue;
      }
      if (line.empty()) continue;
      std::string error;
      auto request = Json::parse(line, &error);
      if (!request) {
        enqueueResponse(failure(Json("unknown"), "protocol-error", error).stringify());
        continue;
      }
      std::optional<Json> superseded;
      CommandMailbox::Result result;
      {
        std::lock_guard<std::mutex> lock(inMx);
        result = inQ.push({*request, line.size(), std::chrono::steady_clock::now()}, superseded);
      }
      if (result == CommandMailbox::Result::overloaded) {
        enqueueResponse(failure(requestId(*request), "control-overloaded", "Command mailbox is full; request was not accepted.").stringify());
      } else if (superseded) {
        enqueueResponse(success(requestId(*superseded), Json::Object{
            {"type", superseded->getString("type")}, {"superseded", true}}).stringify());
      }
      inCv.notify_one();
    }
    inputClosed.store(true);
    inCv.notify_one();
  });

  enqueueResponse(handshake().stringify());

  // The shared-texture handle is tiny and drives the 60fps GPU program present; the
  // render thread drains it every frame. The base64 zoom-frame/preview payloads are
  // heavy and only a UI thumbnail, so this loop pumps them on a throttled cadence.
  auto pumpHeavyFrameEvents = [&] {
    for (const auto& event : mediaCore_.drainProgramFramePreviewEvents()) {
      enqueueFrame(event.stringify());
    }
  };

  // The render thread, this command loop, and the audio/output worker (below) touch
  // MediaCore. coreMutex serializes fast in-memory state + the GPU/video path; the
  // worker additionally uses MediaCore's own audioOutputMutex_ for the long DSP/IO
  // span (held NEVER together with coreMutex), so the render thread — which takes
  // ONLY coreMutex — is never blocked by audio mix / monitor / encoder / output I/O.
  //
  // LOCK ORDER (docs/phase2-threading-plan.md §6; violating it is a deadlock):
  //   coreMutex → audioOutputMutex_                      (audio/output control plane)
  //   coreMutex → ZoomEngineRuntime::mutex_ → ::sendMutex_ (spine sync / engine ops)
  // audioOutputMutex_ and the ZoomEngineRuntime locks are never held together, and
  // the audio worker / engine sender thread never hold coreMutex across blocking
  // work. Engine pipe writes happen ONLY on the runtime's dedicated sender thread
  // (increment 3) — no engine I/O ever runs under coreMutex.
  std::mutex coreMutex;

  // Flip MediaCore to worker mode: the heavy audio/output half no longer runs on the
  // command thread inside renderSyntheticTick — the audioOutputThread below drives it.
  // (Set before any thread starts so the bool is published before concurrent reads.)
  mediaCore_.enableAudioOutputWorker();

  // Dedicated render thread: drives the light, video-only display render (GPU
  // compositor + tiny shared-texture handle, no audio/encoder/output I/O) at
  // ~60fps, fully decoupled from command handling and the blocking output/encoder
  // path so neither can stall the on-screen program. The blocking GPU readback is
  // already skipped on this path, so the lock is held only ~1ms per frame.
  std::thread renderThread([&] {
    // Past this per-tick render cost the loop is saturated and must yield.
    constexpr long long kRenderYieldThresholdMs = 12;
    constexpr long long kRenderYieldMs = 6;
    long long frames = 0;
    long long lockWaitUs = 0;
    long long renderUs = 0;
    long long drainUs = 0;
    auto rateStamp = std::chrono::steady_clock::now();
    // FIXED-CADENCE pacing. The deadline used to be `t0 + budget` with t0 read at
    // the top of EVERY iteration, so each frame's overshoot silently became the
    // next frame's start: a pure relative deadline that can only ever lose time.
    // Measured 17.27ms/frame => 57.9fps on a tick with 10ms of headroom — the
    // render was never the problem, the clock was. Accumulate from a fixed anchor
    // so a late frame is followed by a SHORTER wait and the average holds 60,
    // with bounded catch-up (same discipline as the audio worker's pacer) so a
    // genuinely overrunning tick can't build unpayable debt.
    constexpr long long kFrameBudgetUs = 16666;  // 60fps
    constexpr int kMaxCatchUpFrames = 3;
    // A 60.0 AVERAGE can still hide judder: one 33ms frame plus one 0ms frame
    // averages perfectly and looks broken on motion. Broadcast switchers are
    // judged on DROPPED frames, not mean fps, so count intervals that ran past
    // 1.5x budget (a frame the operator/stream actually lost) and keep the worst.
    long long lateFrames = 0;
    long long worstFrameUs = 0;
    auto lastFrameStart = std::chrono::steady_clock::now();
    auto nextDeadline = std::chrono::steady_clock::now() + std::chrono::microseconds(kFrameBudgetUs);
#ifdef _WIN32
    // Raise the system timer resolution to 1ms so sub-frame sleeps in this loop are
    // accurate. The Windows default (~15.6ms) rounds any sleep up to a full tick, which
    // capped the 60fps-budget pace at ~33-45fps regardless of how cheap the render was.
    timeBeginPeriod(1);
#endif
    while (!stopping.load()) {
      const auto t0 = std::chrono::steady_clock::now();
      {
        // Explicit long long: microseconds::rep is `long` on Linux/GCC and
        // `long long` on MSVC, so an `auto` here makes the std::max below a
        // deduction failure that breaks the Linux build only.
        const long long intervalUs = static_cast<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(t0 - lastFrameStart).count());
        if (intervalUs > kFrameBudgetUs * 3 / 2) {
          ++lateFrames;
        }
        worstFrameUs = (std::max)(worstFrameUs, intervalUs);
        lastFrameStart = t0;
      }
      long long tickRenderMs = 0;
      {
        std::unique_lock<std::mutex> lock(coreMutex);
        // Increment 6 guardrail: sanctioned long-hold site — the video-only GPU
        // tick typically holds ~1-2ms; warn (rate-capped) past half a frame.
        core::ScopedLockHoldTimer holdGuard("render.display-tick",
                                            core::LockHoldGuardrail::kRenderTickBudgetUs);
        const auto t1 = std::chrono::steady_clock::now();
        mediaCore_.renderDisplayTick();
        // The event drain + stringify + enqueue below runs UNDER coreMutex but
        // outside MediaCore's own stage instrumentation, so it was invisible in
        // the "[render] stages" line — the unaccounted remainder of a long tick.
        // Participant texture events scale with the roster (10x1080p = 10 events
        // per tick, each JSON-serialized here), so it must be attributed.
        const auto tDrain = std::chrono::steady_clock::now();
        for (const auto& event : mediaCore_.drainProgramSharedTextureEvents()) {
          enqueueFrame(event.stringify());
        }
        for (const auto& event : mediaCore_.drainParticipantSharedTextureEvents()) {
          enqueueFrame(event.stringify());
        }
        // The multiview shared-texture event is tiny and emitted only on
        // structural change, so draining it on the render thread is cheap.
        for (const auto& event : mediaCore_.drainMultiviewSharedTextureEvents()) {
          enqueueFrame(event.stringify());
        }
        // The preview shared-texture event is likewise tiny + structural-change-
        // gated, so it is cheap to drain here alongside program/multiview.
        for (const auto& event : mediaCore_.drainPreviewSharedTextureEvents()) {
          enqueueFrame(event.stringify());
        }
        const auto t2 = std::chrono::steady_clock::now();
        lockWaitUs += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        renderUs += std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
        drainUs += std::chrono::duration_cast<std::chrono::microseconds>(t2 - tDrain).count();
        // The 120-frame average below can hide a single multi-second stall; surface
        // any individual coreMutex acquire or render tick that blocks > 200ms.
        const auto holdLockMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const auto holdRenderMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        if (holdLockMs >= 200 || holdRenderMs >= 200) {
          std::fprintf(stderr, "[render] STALL lockWait=%lldms render=%lldms\n",
                       static_cast<long long>(holdLockMs), static_cast<long long>(holdRenderMs));
        }
        tickRenderMs = holdRenderMs;
      }
      // Wake the program-video-out worker HERE — coreMutex is released. Doing it
      // inside the scope above woke a thread that immediately blocked on the lock
      // we still held, and cost operator command p99 51ms -> 107ms.
      mediaCore_.notifyProgramFramePublished();
      // COMMAND PRIORITY. A saturated tick (a real show: several 1080p Zoom
      // feeds plus capture sources pushed this to ~34ms) exceeds the frame
      // budget, so the pacer below sleeps zero and this loop re-acquires
      // coreMutex immediately — the lock is then held ~100% of wall time and
      // NO request can ever acquire it, so every shell command times out
      // (joins, scene syncs, assigns). Yield a fixed slice with the lock
      // RELEASED whenever the tick overran. Dropping a frame is invisible;
      // a timed-out command breaks the app.
      if (tickRenderMs > kRenderYieldThresholdMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kRenderYieldMs));
      }
      if (++frames >= 120) {
        const auto now = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(now - rateStamp).count();
        mediaCore_.reportRenderDeadlineMisses(lateFrames);
        std::fprintf(stderr,
                     "[render] %.1ffps  lockWait=%.1fms  render=%.1fms  drain=%.1fms  "
                     "dropped=%lld  worst=%.1fms  (avg/frame over %lld)\n",
                     sec > 0 ? frames / sec : 0.0, lockWaitUs / (frames * 1000.0),
                     renderUs / (frames * 1000.0), drainUs / (frames * 1000.0),
                     lateFrames, worstFrameUs / 1000.0, frames);
        frames = 0;
        lockWaitUs = 0;
        renderUs = 0;
        drainUs = 0;
        lateFrames = 0;
        worstFrameUs = 0;
        rateStamp = now;
      }
      // Precise 60fps pacer. sleep_for alone overshoots ~1-2ms even at
      // timeBeginPeriod(1), which capped a 12-13ms render tick at ~55fps. The old
      // solution slept then SPUN the final 1.5ms (yield loop) - 60x/s that is ~9% of a
      // core of pure spin, and during a system stress test (vcam + browser + DAW) it
      // showed up as OTHER apps' audio glitching. A high-resolution waitable timer
      // (Win10 1803+) gives ~0.5ms wakeup precision without burning the CPU; only a
      // ~200us yield tail remains to absorb the residual jitter. Heavy iterations
      // (work >= budget) blow past the deadline and run flat out, as before.
      const auto deadline = nextDeadline;
#ifdef _WIN32
      static thread_local HANDLE pacerTimer = ::CreateWaitableTimerExW(
          nullptr, nullptr,
          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION | CREATE_WAITABLE_TIMER_MANUAL_RESET,
          TIMER_ALL_ACCESS);
      // 200us tail. The old 500us guard existed to mask the RELATIVE-deadline pacer:
      // with `deadline = t0 + budget` every overshoot became the next frame's start,
      // so a 200us guard measured 58.7fps and 500us was needed to "re-lock" 60. The
      // deadline is now accumulated from a fixed anchor (above), which reclaims that
      // time by construction, so the guard no longer has drift to hide. Re-measured
      // on the owner's rig at 8x1080p60, 3 interleaved 40s drill runs each:
      //   500us -> 59.9/60.0/60.0 fps, 0 dropped, 64.1/63.3/64.5 s core CPU
      //   200us -> 60.0/60.0/60.0 fps, 0 dropped, 60.7/60.7/55.7 s core CPU
      // Same delivery (90-91%) and coreMutex over-budget (1-2%) either way; the CPU
      // ranges do not overlap. That reclaimed spin is in the exact loop implicated in
      // glitching OTHER apps' audio during the virtual-camera work, so it is worth
      // real money. Do not raise this without re-running the drill — and never raise
      // it to paper over a pacing bug again.
      constexpr auto kSpinGuardUs = std::chrono::microseconds(200);
      auto nowPace = std::chrono::steady_clock::now();
      if (pacerTimer != nullptr && nowPace + kSpinGuardUs < deadline) {
        const auto waitUs =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - nowPace) -
            kSpinGuardUs;
        LARGE_INTEGER due;
        due.QuadPart = -(waitUs.count() * 10);  // relative, 100ns ticks
        if (::SetWaitableTimer(pacerTimer, &due, 0, nullptr, nullptr, FALSE)) {
          ::WaitForSingleObject(pacerTimer, 20);
        }
      } else if (nowPace + std::chrono::microseconds(1500) < deadline) {
        std::this_thread::sleep_for((deadline - nowPace) - std::chrono::microseconds(1500));
      }
#else
      constexpr auto kSpinGuardUs = std::chrono::microseconds(1500);
      auto nowPace = std::chrono::steady_clock::now();
      if (nowPace + kSpinGuardUs < deadline) {
        std::this_thread::sleep_for((deadline - nowPace) - kSpinGuardUs);
      }
#endif
      while (std::chrono::steady_clock::now() < deadline) {
        // Tiny tail to absorb timer overshoot; yield keeps it civil.
        std::this_thread::yield();
      }
      nextDeadline += std::chrono::microseconds(kFrameBudgetUs);
      // Bounded catch-up: if the tick genuinely overran (heavy show, thermal
      // throttle) don't try to reclaim unbounded lost frames by free-running —
      // re-anchor and keep real-time cadence from here.
      const auto afterPace = std::chrono::steady_clock::now();
      if (nextDeadline + std::chrono::microseconds(kFrameBudgetUs * kMaxCatchUpFrames) <
          afterPace) {
        nextDeadline = afterPace + std::chrono::microseconds(kFrameBudgetUs);
      }
    }
  });

  // Dedicated Zoom-frame pump: drains the engine frame queue -> stdout on its own
  // ~8ms cadence and NEVER takes the core lock. The render and command threads both
  // contend on the core lock (media-core-sync holds it 50-100ms), which starved the
  // Zoom drain and made the feed buffer (~360ms) and drop unevenly.
  // drainZoomVideoFrameEvents is thread-safe (engine's own mutex); enqueueFrame uses
  // the output mutex, so this is safe to run concurrently.
  std::thread zoomPumpThread([&] {
    while (!stopping.load()) {
      for (const auto& event : mediaCore_.drainZoomVideoFrameEvents()) {
        enqueueFrame(event.stringify());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
  });

  // Dedicated audio/output worker (Phase 2 decouple): drives the audio mix, routed-bus
  // matrix, monitor device render, BS.1770 loudness, encoder submit, output-sender
  // network sync and recording mux on a steady ~20ms cadence — finer & steadier than
  // the old bursty 250ms poll (better for the monitor device, not a new glitch source:
  // it reuses the source-drain model, no new PCM ring buffer). renderAudioOutputTick
  // takes coreMutex only briefly (gather/publish) and audioOutputMutex_ for the long
  // DSP/IO span, NEVER both at once, so the render thread is never blocked by it.
  std::thread audioOutputThread([&] {
#ifdef _WIN32
    // The worker is the program audio clock, not background application work.
    // MMCSS protects its 20 ms deadline from render/UI/build pressure without
    // using an unbounded time-critical priority.
    DWORD audioTaskIndex = 0;
    HANDLE audioMmcss = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &audioTaskIndex);
    if (audioMmcss != nullptr) {
      (void)::AvSetMmThreadPriority(audioMmcss, AVRT_PRIORITY_HIGH);
    }
#endif
    constexpr long long kAudioBudgetUs = 20000;  // 50Hz
    long long ticks = 0;
    long long workUs = 0;
    auto rateStamp = std::chrono::steady_clock::now();
    // Absolute-deadline pacer (audio overhaul spec 4.2). The prior relative
    // sleep_for(budget - work) added the Windows sleep overshoot (~1ms even at
    // timeBeginPeriod(1)) to EVERY period, so the cadence could only approach
    // 50Hz from below — it measured 47.3 ticks/s, a chronic ~6% shortfall the
    // real-time WASAPI monitor endpoint turned into silent underruns. Advance a
    // fixed deadline grid instead: overshoot on one tick is reclaimed on the
    // next, so the long-run cadence locks at 50.0.
    //
    // BOUNDED CATCH-UP on a blown deadline (2026-07-13). The old policy
    // re-anchored the grid to now on every blown deadline, on the premise that
    // "audio sources are drained whole, so skipped grid slots carry no lost
    // samples". That premise is false downstream: steadyAudioFrameFeed emits
    // at most ONE tick of samples per tick and used to shed its FIFO past 6 ticks, so
    // every skipped slot permanently loses 20ms of real-time audio. Measured on
    // the recording mux: ~48.2 ticks/s → the MP4 audio track ran 3.1% short of
    // video (925ms drift over a 30s recording). Now a blown deadline runs the
    // next tick IMMEDIATELY (no sleep) until the grid is regained — each
    // catch-up tick still drains exactly one 960-frame block, so the spec-4.2
    // fixed block size (and every DSP invariant behind it) is untouched. Only
    // Only re-anchor after 500 ms. The feed now retains 640 ms, allowing ordinary
    // control/render stalls to run catch-up ticks and preserve every PCM block.
    constexpr long long kMaxCatchupBehindUs = kAudioBudgetUs * 25;
    long long reanchors = 0;
    auto lastReanchorLog = std::chrono::steady_clock::now();
    auto deadline = std::chrono::steady_clock::now();
    while (!stopping.load()) {
      const auto t0 = std::chrono::steady_clock::now();
      mediaCore_.renderAudioOutputTick(coreMutex);
      const long long iterUs =
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
      workUs += iterUs;
      if (++ticks >= 120) {
        const auto now = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(now - rateStamp).count();
        std::fprintf(stderr, "[audioOut] %.1f ticks/s  work=%.1fms  (avg over %lld)\n",
                     sec > 0 ? ticks / sec : 0.0, workUs / (ticks * 1000.0), ticks);
        ticks = 0;
        workUs = 0;
        rateStamp = now;
      }
      deadline += std::chrono::microseconds(kAudioBudgetUs);
      const auto now = std::chrono::steady_clock::now();
      if (deadline <= now) {
        if (now - deadline > std::chrono::microseconds(kMaxCatchupBehindUs)) {
          deadline = now;  // hopelessly behind: re-anchor (audio WILL be shed)
          ++reanchors;
          if (now - lastReanchorLog > std::chrono::seconds(5)) {
            std::fprintf(stderr,
                         "[audioOut] pacer re-anchored %lld time(s): worker >500ms behind, real-time audio shed\n",
                         reanchors);
            lastReanchorLog = now;
            reanchors = 0;
          }
        }
        // else: behind but recoverable — loop immediately (catch-up tick).
      } else {
        std::this_thread::sleep_until(deadline);
      }
    }
#ifdef _WIN32
    if (audioMmcss != nullptr) {
      ::AvRevertMmThreadCharacteristics(audioMmcss);
    }
#endif
  });

  // Dedicated PROGRAM VIDEO OUT worker. Everything leaving the app used to be
  // sampled by the audio worker above, whose 20ms period is an AUDIO constant —
  // so a 60fps program was recorded and streamed at ~51fps. Raising that worker
  // to 60Hz would break the 960-sample block contract (spec 4.2) that its pacer
  // exists to hold, so video gets its own grid at the OUTPUT frame rate.
  //
  // Same absolute-deadline pacer as the audio worker and for the same reason: a
  // relative sleep_for adds the Windows overshoot (~1ms even at
  // timeBeginPeriod(1)) to every period, which can only approach the target from
  // below. Unlike audio there is nothing to "catch up" — a late video tick has
  // no buffered samples to shed — so a blown deadline just re-anchors.
  mediaCore_.setVideoOutputTickRunning(true);
  std::thread videoOutputThread([&] {
    // NO PACER. renderVideoOutputTick BLOCKS until the compositor publishes a new
    // program frame (bounded ~20ms so it can still re-evaluate output state when
    // the program is idle), so the wait IS the pacing. Two paced designs were
    // measured and both were wrong: at 60Hz the sampler aliased against the 60Hz
    // producer and muxed 51.7fps, and at 120Hz the extra coreMutex acquisitions
    // dropped the 8x1080p60 drill to 57.4fps with a 141ms command p99.
    long long ticks = 0;
    long long workUs = 0;
    auto rateStamp = std::chrono::steady_clock::now();
    while (!stopping.load()) {
      const auto t0 = std::chrono::steady_clock::now();
      mediaCore_.renderVideoOutputTick(coreMutex);  // blocks until a new frame
      workUs += std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
      if (++ticks >= 120) {
        const auto now = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(now - rateStamp).count();
        // `work` here INCLUDES the wait for the next frame, so it tracks the
        // frame interval rather than the cost of a submit.
        std::fprintf(stderr, "[videoOut] %.1f ticks/s  work=%.1fms  (avg over %lld)\n",
                     sec > 0 ? ticks / sec : 0.0, workUs / (ticks * 1000.0), ticks);
        ticks = 0;
        workUs = 0;
        rateStamp = now;
      }
    }
  });

  // One lifecycle worker owns potentially blocking join/auth. The command loop
  // keeps servicing Take/Stop/Leave; pending work is bounded to one join.
  std::mutex joinMx;
  std::condition_variable joinCv;
  std::optional<std::pair<Json, std::uint64_t>> pendingJoin;
  std::atomic<std::uint64_t> joinGeneration{0};
  bool joinWorkerStopping = false;
  bool joinBusy = false;
  std::thread joinWorker([&] {
    for (;;) {
      Json request;
      std::uint64_t generation;
      {
        std::unique_lock<std::mutex> lock(joinMx);
        joinCv.wait(lock, [&] { return joinWorkerStopping || pendingJoin.has_value(); });
        if (joinWorkerStopping && !pendingJoin) return;
        request = std::move(pendingJoin->first);
        generation = pendingJoin->second;
        pendingJoin.reset();
      }
      const auto cancelled = [&] { return joinGeneration.load() != generation; };
      const auto operationId = "zoom-join-" + std::to_string(generation);
      const bool async = request.get("asyncOperation") && request.get("asyncOperation")->asBool();
      Json response;
      try {
        const auto snapshot = joinHandler_ ? joinHandler_(*request.get("payload"), cancelled) :
            mediaCore_.joinZoom(*request.get("payload"), cancelled);
        if (cancelled()) {
          response = failure(requestId(request), "operation-cancelled", "Zoom join was cancelled.");
        } else {
          auto operation = contracts::OperationStatus{processEpoch_, operationId,
              snapshot.getString("meetingState") == "in_meeting" ? "completed" : "failed", {}};
          response = success(requestId(request), Json::Object{{"type", "zoom-join"},
              {"snapshot", snapshot}, {"operation", contracts::toJson(operation)}});
        }
      } catch (const std::exception& error) {
        response = failure(requestId(request), "zoom-join-failed", error.what());
      } catch (...) {
        response = failure(requestId(request), "zoom-join-failed", "Zoom lifecycle worker failed.");
      }
      // Serialize final publication with Leave/cancel invalidation. A completion
      // cannot pass its generation check and then overtake an accepted Leave.
      std::lock_guard<std::mutex> completionLock(joinMx);
      if (cancelled()) response = failure(requestId(request), "operation-cancelled", "Zoom join was cancelled.");
      if (async) {
        const auto* ok = response.get("ok");
        const auto* snapshot = response.get("snapshot");
        const std::string state = cancelled() ? "cancelled" :
            (ok && ok->asBool() && snapshot && snapshot->getString("meetingState") == "in_meeting" ? "completed" : "failed");
        enqueueResponse(Json(Json::Object{{"type", "operation-completed"},
            {"operation", contracts::toJson(contracts::OperationStatus{processEpoch_, operationId, state, {}})},
            {"result", response}}).stringify());
      } else {
        enqueueResponse(response.stringify());
      }
      joinBusy = false;
    }
  });

  auto lastPump = std::chrono::steady_clock::now();
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(inMx);
      if (inQ.empty() && !inputClosed.load()) {
        inCv.wait_for(lock, std::chrono::milliseconds(10),
                      [&] { return !inQ.empty() || inputClosed.load(); });
      }
    }

    // Drain ALL queued commands before rendering so a burst (e.g. the Zoom join
    // sequence) is serviced immediately and is never paced by the display tick.
    for (;;) {
      std::optional<Json> request;
      Stamp enqueuedAt;
      {
        std::lock_guard<std::mutex> lock(inMx);
        if (inQ.empty()) break;
        auto entry = inQ.pop();
        request = std::move(entry.request);
        enqueuedAt = entry.enqueuedAt;
      }
      const auto dequeuedAt = std::chrono::steady_clock::now();
      const auto queueWaitMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(dequeuedAt - enqueuedAt).count();
      {
        const std::string reqType = request->getString("type");
        std::string responseStr;
        std::chrono::steady_clock::time_point h0, h1;
        if ((reqType == "zoom-leave" || reqType == "zoom-cancel") &&
            (!request->get("protocolVersion") || contracts::validateProtocolVersion(*request->get("protocolVersion")))) {
          std::lock_guard<std::mutex> lock(joinMx);
          ++joinGeneration; // invalidate auth/spawn waits before applying Leave
        }
        if (reqType == "zoom-join" && (joinHandler_ || mediaCore_.zoomEngineConfigured()) &&
            request->get("payload") && request->get("payload")->isObject() &&
            (!request->get("protocolVersion") || contracts::validateProtocolVersion(*request->get("protocolVersion")))) {
          std::lock_guard<std::mutex> lock(joinMx);
          if (joinBusy) {
            enqueueResponse(failure(requestId(*request), "operation-in-progress",
                "A Zoom join is already in progress; cancel or leave before retrying.").stringify());
          } else {
            joinBusy = true;
            const auto generation = ++joinGeneration;
            if (request->get("asyncOperation") && request->get("asyncOperation")->asBool()) {
              enqueueResponse(success(requestId(*request), Json::Object{{"type", "zoom-join"},
                  {"operation", contracts::toJson(contracts::OperationStatus{processEpoch_,
                      "zoom-join-" + std::to_string(generation), "accepted", {}})}}).stringify());
            }
            pendingJoin = std::make_pair(*request, generation);
            joinCv.notify_one();
          }
          continue;
        } else {
          std::lock_guard<std::mutex> lock(coreMutex);
          // Increment 6 guardrail: sanctioned long-hold site — command-carrying
          // syncs legitimately render a capped catch-up of synthetic ticks under
          // the lock; the budget is a regression backstop above the 30ms [cmd]
          // warning below.
          core::ScopedLockHoldTimer holdGuard("cmd.handle",
                                              core::LockHoldGuardrail::kCommandHandleBudgetUs);
          h0 = std::chrono::steady_clock::now();
          responseStr = handle(*request).stringify();
          h1 = std::chrono::steady_clock::now();
        }
        const auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(h1 - h0).count();
        const auto lockWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(h0 - dequeuedAt).count();
        if (heldMs >= 30) {
          const bool lockFree = reqType == "zoom-join" && mediaCore_.zoomEngineConfigured();
          std::fprintf(stderr, "[cmd] '%s' %s %lldms%s\n", reqType.c_str(),
                       lockFree ? "handled lock-free in" : "held core lock",
                       static_cast<long long>(heldMs), lockFree ? " (render unaffected)" : " (starves render)");
        }
        // The whole in-core latency for this request: time spent waiting in inQ +
        // waiting for coreMutex + handling. If this is small but the host still
        // times out, the delay is downstream in the writer/pipe (see [writer]).
        if (queueWaitMs + lockWaitMs + heldMs >= 500) {
          std::fprintf(stderr,
                       "[req] '%s' queueWait=%lldms lockWait=%lldms handle=%lldms (total in-core=%lldms)\n",
                       reqType.c_str(), static_cast<long long>(queueWaitMs),
                       static_cast<long long>(lockWaitMs), static_cast<long long>(heldMs),
                       static_cast<long long>(queueWaitMs + lockWaitMs + heldMs));
        }
        enqueueResponse(responseStr);
      }
    }

    {
      std::lock_guard<std::mutex> lock(inMx);
      if (inQ.empty() && inputClosed.load()) {
        break;
      }
    }

    const auto now = std::chrono::steady_clock::now();

    // The render thread drives the display render + shared-texture pump. Here we
    // only pump the heavy base64 frame/preview events, on a throttled cadence so
    // they don't starve responses. Lock the core while draining its queues.
    if (now - lastPump >= kFramePumpInterval) {
      const auto p0 = std::chrono::steady_clock::now();
      {
        std::lock_guard<std::mutex> lock(coreMutex);
        // Increment 6 guardrail: sanctioned long-hold site (base64 preview
        // stringify on the throttled pump).
        core::ScopedLockHoldTimer holdGuard("cmd.frame-pump",
                                            core::LockHoldGuardrail::kFramePumpBudgetUs);
        pumpHeavyFrameEvents();
      }
      const auto pumpMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - p0).count();
      // pumpHeavyFrameEvents runs on the command loop under coreMutex; if it ever
      // blocks for long it stalls both command handling and the render thread.
      if (pumpMs >= 200) {
        std::fprintf(stderr, "[pump] pumpHeavyFrameEvents %lldms (blocks command loop + render)\n",
                     static_cast<long long>(pumpMs));
      }
      lastPump = now;
    }
  }

  {
    std::lock_guard<std::mutex> lock(joinMx);
    ++joinGeneration;
    joinWorkerStopping = true;
  }
  joinCv.notify_one();
  joinWorker.join(); // waits only for cancellation-aware spawn/auth teardown, never coreMutex
  stopping.store(true);
  outCv.notify_one();
  if (renderThread.joinable()) {
    renderThread.join();
  }
  if (zoomPumpThread.joinable()) {
    zoomPumpThread.join();
  }
  if (videoOutputThread.joinable()) {
    videoOutputThread.join();
  }
  mediaCore_.setVideoOutputTickRunning(false);
  if (audioOutputThread.joinable()) {
    audioOutputThread.join();
  }
  if (reader.joinable()) {
    reader.join();
  }
  if (writer.joinable()) {
    writer.join();
  }
}

void JsonRpcServer::flushFrameEvents(std::ostream& output) {
  for (const auto& event : mediaCore_.drainZoomVideoFrameEvents()) {
    output << event.stringify() << '\n';
  }
  for (const auto& event : mediaCore_.drainProgramFramePreviewEvents()) {
    output << event.stringify() << '\n';
  }
  for (const auto& event : mediaCore_.drainProgramSharedTextureEvents()) {
    output << event.stringify() << '\n';
  }
  for (const auto& event : mediaCore_.drainParticipantSharedTextureEvents()) {
    output << event.stringify() << '\n';
  }
  for (const auto& event : mediaCore_.drainMultiviewSharedTextureEvents()) {
    output << event.stringify() << '\n';
  }
  for (const auto& event : mediaCore_.drainPreviewSharedTextureEvents()) {
    output << event.stringify() << '\n';
  }
}

Json JsonRpcServer::success(const Json& id, Json::Object payload) const {
  payload.emplace("id", id);
  payload.emplace("ok", true);
  return payload;
}

Json JsonRpcServer::failure(const Json& id, std::string code, std::string message) const {
  return Json::Object{
      {"id", id},
      {"ok", false},
      {"error", Json::Object{{"code", std::move(code)}, {"message", std::move(message)}}},
  };
}

}  // namespace corevideo::rpc
