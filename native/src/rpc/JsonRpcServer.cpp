#include "rpc/JsonRpcServer.h"

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

JsonRpcServer::JsonRpcServer(core::MediaCore& mediaCore) : mediaCore_(mediaCore) {}

Json JsonRpcServer::handshake() const {
  return Json::Object{
      {"id", "handshake"},
      {"ok", true},
      {"type", "handshake"},
      {"profile", mediaCore_.profile()},
  };
}

Json JsonRpcServer::handle(const Json& request) {
  const Json id = requestId(request);
  const std::string type = request.getString("type");
  if (type.empty()) {
    return failure(id, "protocol-error", "Request is missing a type field.");
  }

  if (hasType(request, "handshake")) {
    return success(id, Json::Object{{"type", "handshake"}, {"profile", mediaCore_.profile()}});
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

  if (hasType(request, "zoom-leave")) {
    return success(id, Json::Object{
                           {"type", "zoom-leave"},
                           {"snapshot", mediaCore_.leaveZoom()},
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
    return success(id, Json::Object{{"type", "capture-devices"}, {"devices", mediaCore_.connectCaptureDevice(payload->getString("deviceId"))}});
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

  if (hasType(request, "unregister-capture-shm")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "unregister-capture-shm requires a payload.");
    }
    mediaCore_.unregisterCaptureShm(payload->getString("deviceId"));
    return success(id, Json::Object{{"type", "ack"}});
  }

  if (hasType(request, "start-program-output") || hasType(request, "load-scene-graph") ||
      hasType(request, "set-participant-transform") || hasType(request, "set-overlay-asset")) {
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
  // INSTRUMENTATION: stamp each request as the reader enqueues it so the command
  // loop can report queue-wait (dequeue - enqueue) separately from handle duration.
  std::deque<std::pair<std::string, Stamp>> inQ;
  std::atomic<bool> inputClosed{false};

  std::thread reader([&] {
    std::string line;
    while (std::getline(input, line)) {
      {
        std::lock_guard<std::mutex> lock(inMx);
        inQ.emplace_back(std::move(line), std::chrono::steady_clock::now());
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

  // The render thread and this command loop are the only two threads that touch
  // MediaCore. Serialize ALL core access with one mutex held briefly in each — no
  // per-method locking inside MediaCore is needed.
  std::mutex coreMutex;

  // Dedicated render thread: drives the light, video-only display render (GPU
  // compositor + tiny shared-texture handle, no audio/encoder/output I/O) at
  // ~60fps, fully decoupled from command handling and the blocking output/encoder
  // path so neither can stall the on-screen program. The blocking GPU readback is
  // already skipped on this path, so the lock is held only ~1ms per frame.
  std::thread renderThread([&] {
    long long frames = 0;
    long long lockWaitUs = 0;
    long long renderUs = 0;
    auto rateStamp = std::chrono::steady_clock::now();
#ifdef _WIN32
    // Raise the system timer resolution to 1ms so sub-frame sleeps in this loop are
    // accurate. The Windows default (~15.6ms) rounds any sleep up to a full tick, which
    // capped the 60fps-budget pace at ~33-45fps regardless of how cheap the render was.
    timeBeginPeriod(1);
#endif
    while (!stopping.load()) {
      const auto t0 = std::chrono::steady_clock::now();
      {
        std::unique_lock<std::mutex> lock(coreMutex);
        const auto t1 = std::chrono::steady_clock::now();
        mediaCore_.renderDisplayTick();
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
        const auto t2 = std::chrono::steady_clock::now();
        lockWaitUs += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        renderUs += std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
        // The 120-frame average below can hide a single multi-second stall; surface
        // any individual coreMutex acquire or render tick that blocks > 200ms.
        const auto holdLockMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const auto holdRenderMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        if (holdLockMs >= 200 || holdRenderMs >= 200) {
          std::fprintf(stderr, "[render] STALL lockWait=%lldms render=%lldms\n",
                       static_cast<long long>(holdLockMs), static_cast<long long>(holdRenderMs));
        }
      }
      if (++frames >= 120) {
        const auto now = std::chrono::steady_clock::now();
        const double sec = std::chrono::duration<double>(now - rateStamp).count();
        std::fprintf(stderr,
                     "[render] %.1ffps  lockWait=%.1fms  render=%.1fms  (avg/frame over %lld)\n",
                     sec > 0 ? frames / sec : 0.0, lockWaitUs / (frames * 1000.0),
                     renderUs / (frames * 1000.0), frames);
        frames = 0;
        lockWaitUs = 0;
        renderUs = 0;
        rateStamp = now;
      }
      // Pace to a 60fps budget: sleep only the REMAINDER of the ~16.6ms frame after the
      // work this iteration, instead of a flat 8ms (which capped the loop near ~45fps even
      // when the render was cheap). On heavy iterations (elapsed >= budget) we don't sleep
      // at all and run as fast as the work allows. Low latency is the product north-star.
      constexpr long long kFrameBudgetUs = 16666;  // 60fps
      const long long iterUs =
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
      if (iterUs < kFrameBudgetUs) {
        std::this_thread::sleep_for(std::chrono::microseconds(kFrameBudgetUs - iterUs));
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
      std::string line;
      Stamp enqueuedAt;
      {
        std::lock_guard<std::mutex> lock(inMx);
        if (inQ.empty()) {
          break;
        }
        line = std::move(inQ.front().first);
        enqueuedAt = inQ.front().second;
        inQ.pop_front();
      }
      if (line.empty()) {
        continue;
      }
      // Queue-wait: how long this request sat in inQ before the command loop got to
      // it (i.e. the loop was busy handling earlier commands or pumping frames).
      const auto dequeuedAt = std::chrono::steady_clock::now();
      const auto queueWaitMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(dequeuedAt - enqueuedAt).count();
      std::string error;
      auto request = Json::parse(line, &error);
      if (!request) {
        // Ungated: a request that failed to parse (e.g. a truncated/split large line)
        // is answered with id="unknown", so the bridge's real request id never matches
        // and it times out. Surface the length + error to catch line-protocol breakage.
        std::fprintf(stderr, "[parse-dbg] FAILED len=%zu err='%s' head='%.60s'\n",
                     line.size(), error.c_str(), line.c_str());
        enqueueResponse(failure(Json("unknown"), "protocol-error", error).stringify());
      } else {
        const std::string reqType = request->getString("type");
        std::string responseStr;
        std::chrono::steady_clock::time_point h0, h1;
        {
          std::lock_guard<std::mutex> lock(coreMutex);
          h0 = std::chrono::steady_clock::now();
          responseStr = handle(*request).stringify();
          h1 = std::chrono::steady_clock::now();
        }
        const auto heldMs = std::chrono::duration_cast<std::chrono::milliseconds>(h1 - h0).count();
        const auto lockWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(h0 - dequeuedAt).count();
        if (heldMs >= 30) {
          std::fprintf(stderr, "[cmd] '%s' held core lock %lldms (starves render)\n",
                       reqType.c_str(), static_cast<long long>(heldMs));
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

  stopping.store(true);
  outCv.notify_one();
  if (renderThread.joinable()) {
    renderThread.join();
  }
  if (zoomPumpThread.joinable()) {
    zoomPumpThread.join();
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
