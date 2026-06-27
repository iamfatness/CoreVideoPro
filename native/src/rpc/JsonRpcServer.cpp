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
  constexpr std::size_t kMaxPendingFrameEvents = 180;  // ~ a few seconds of preview
  constexpr auto kFramePumpInterval = std::chrono::milliseconds(33);  // ~30fps

  std::mutex outMx;
  std::condition_variable outCv;
  std::deque<std::string> outHi;  // command responses / failures
  std::deque<std::string> outLo;  // frame-preview events (droppable)
  std::atomic<bool> stopping{false};

  auto enqueueResponse = [&](std::string message) {
    {
      std::lock_guard<std::mutex> lock(outMx);
      outHi.push_back(std::move(message));
    }
    outCv.notify_one();
  };
  auto enqueueFrame = [&](std::string message) {
    {
      std::lock_guard<std::mutex> lock(outMx);
      if (outLo.size() >= kMaxPendingFrameEvents) {
        outLo.pop_front();  // drop oldest frame; preview is latest-wins
      }
      outLo.push_back(std::move(message));
    }
    outCv.notify_one();
  };

  std::thread writer([&] {
    std::unique_lock<std::mutex> lock(outMx);
    for (;;) {
      outCv.wait(lock, [&] { return !outHi.empty() || !outLo.empty() || stopping.load(); });
      while (!outHi.empty() || !outLo.empty()) {
        std::string message;
        if (!outHi.empty()) {
          message = std::move(outHi.front());
          outHi.pop_front();
        } else {
          message = std::move(outLo.front());
          outLo.pop_front();
        }
        lock.unlock();
        output << message << '\n';
        output.flush();
        lock.lock();
      }
      if (stopping.load() && outHi.empty() && outLo.empty()) {
        break;
      }
    }
  });

  std::mutex inMx;
  std::condition_variable inCv;
  std::deque<std::string> inQ;
  std::atomic<bool> inputClosed{false};

  std::thread reader([&] {
    std::string line;
    while (std::getline(input, line)) {
      {
        std::lock_guard<std::mutex> lock(inMx);
        inQ.push_back(std::move(line));
      }
      inCv.notify_one();
    }
    inputClosed.store(true);
    inCv.notify_one();
  });

  enqueueResponse(handshake().stringify());

  auto pumpFrameEvents = [&] {
    for (const auto& event : mediaCore_.drainZoomVideoFrameEvents()) {
      enqueueFrame(event.stringify());
    }
    for (const auto& event : mediaCore_.drainProgramFramePreviewEvents()) {
      enqueueFrame(event.stringify());
    }
    for (const auto& event : mediaCore_.drainProgramSharedTextureEvents()) {
      enqueueFrame(event.stringify());
    }
  };

  auto lastPump = std::chrono::steady_clock::now();
  for (;;) {
    std::string line;
    bool haveLine = false;
    {
      std::unique_lock<std::mutex> lock(inMx);
      inCv.wait_for(lock, std::chrono::milliseconds(10),
                    [&] { return !inQ.empty() || inputClosed.load(); });
      if (!inQ.empty()) {
        line = std::move(inQ.front());
        inQ.pop_front();
        haveLine = true;
      } else if (inputClosed.load()) {
        break;
      }
    }

    if (haveLine && !line.empty()) {
      std::string error;
      auto request = Json::parse(line, &error);
      if (!request) {
        enqueueResponse(failure(Json("unknown"), "protocol-error", error).stringify());
      } else {
        enqueueResponse(handle(*request).stringify());
      }
    }

    // Pump frame events on a throttled cadence, NOT once per command, so a flood
    // of commands is serviced at full speed instead of being paced by frame I/O.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastPump >= kFramePumpInterval) {
      pumpFrameEvents();
      lastPump = now;
    }
  }

  stopping.store(true);
  outCv.notify_one();
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
