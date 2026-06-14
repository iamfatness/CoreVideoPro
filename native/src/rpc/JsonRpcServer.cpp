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
    return success(id, Json::Object{{"profile", mediaCore_.profile()}});
  }

  if (hasType(request, "native-media-core-sync") || request.get("commands")) {
    return success(id, Json::Object{{"state", mediaCore_.applyCommands(commandBatch(request))}});
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

  if (hasType(request, "start-program-output") || hasType(request, "load-scene-graph") ||
      hasType(request, "set-participant-transform") || hasType(request, "set-overlay-asset")) {
    return success(id, Json::Object{{"state", mediaCore_.applyCommands(commandBatch(request))}});
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
    output.flush();
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
