#include "rpc/JsonRpcServer.h"

#include <iostream>
#include <string>

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
    return success(id, Json::Object{
                           {"type", hasType(request, "media-core-sync") ? "media-core-sync" : "native-media-core-sync"},
                           {"snapshot", mediaCore_.applyCommands(commandBatch(request))},
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
    return success(id, Json::Object{{"devices", mediaCore_.captureDevices()}});
  }

  if (hasType(request, "select-capture-input")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "select-capture-input requires a payload.");
    }
    return success(id, Json::Object{{"devices", mediaCore_.selectCaptureInput(payload->getString("deviceId"), payload->getString("inputId"))}});
  }

  if (hasType(request, "set-capture-audio-sync-offset")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "set-capture-audio-sync-offset requires a payload.");
    }
    const Json* offset = payload->get("offsetMs");
    return success(id, Json::Object{{"devices", mediaCore_.setCaptureAudioSyncOffset(payload->getString("deviceId"), offset ? static_cast<int>(offset->asNumber()) : 0)}});
  }

  if (hasType(request, "connect-capture-device")) {
    const Json* payload = request.get("payload");
    if (!payload || !payload->isObject()) {
      return failure(id, "protocol-error", "connect-capture-device requires a payload.");
    }
    return success(id, Json::Object{{"devices", mediaCore_.connectCaptureDevice(payload->getString("deviceId"))}});
  }

  if (hasType(request, "start-program-output") || hasType(request, "load-scene-graph") ||
      hasType(request, "set-participant-transform") || hasType(request, "set-overlay-asset")) {
    return success(id, Json::Object{{"snapshot", mediaCore_.applyCommands(commandBatch(request))}});
  }

  return failure(id, "protocol-error", "Unsupported native media-core command: " + type);
}

void JsonRpcServer::run(std::istream& input, std::ostream& output) {
  output << handshake().stringify() << '\n';
  output.flush();

  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) {
      continue;
    }
    std::string error;
    auto request = Json::parse(line, &error);
    if (!request) {
      output << failure(Json("unknown"), "protocol-error", error).stringify() << '\n';
      output.flush();
      continue;
    }
    output << handle(*request).stringify() << '\n';
    flushFrameEvents(output);
    output.flush();
  }
}

void JsonRpcServer::flushFrameEvents(std::ostream& output) {
  for (const auto& event : mediaCore_.drainZoomVideoFrameEvents()) {
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
