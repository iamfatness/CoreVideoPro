#include "core/MediaCore.h"

#include "core/Protocol.h"

namespace corevideo::core {
namespace {

rpc::Json::Array stringArray(const std::vector<std::string>& values) {
  rpc::Json::Array result;
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return result;
}

rpc::Json::Array capabilityArray() {
  rpc::Json::Array result;
  for (auto capability : kNativeMediaCoreCapabilities) {
    result.emplace_back(std::string(capability));
  }
  return result;
}

}  // namespace

MediaCore::MediaCore(modules::ModuleSet modules) : modules_(std::move(modules)) {}

rpc::Json MediaCore::profile() const {
  return rpc::Json::Object{
      {"name", "CoreVideo Pro Native Media Core Stub"},
      {"renderer", "software"},
      {"maxProgramResolution", "1920x1080"},
      {"maxProgramFps", 30},
      {"maxParticipantFeeds", 6},
      {"maxIsoRecordings", 2},
      {"capabilities", capabilityArray()},
  };
}

rpc::Json MediaCore::health() const {
  const auto session = modules_.encoder->session();
  return rpc::Json::Object{
      {"status", session.active ? "live" : "idle"},
      {"renderer", "software"},
      {"frameCount", static_cast<double>(lastProgramFrame_.frameNumber)},
      {"encodedFrameCount", static_cast<double>(session.encodedFrameCount)},
      {"mixedAudioFrames", static_cast<double>(mixedAudioFrameCount_)},
      {"captureDeviceCount", static_cast<double>(modules_.captureDevice->enumerate().size())},
      {"messages", rpc::Json::Array{"COREVIDEO_STUB synthetic media path active"}},
  };
}

rpc::Json MediaCore::sessionState() const {
  const auto session = modules_.encoder->session();
  return rpc::Json::Object{
      {"sceneId", sceneId_},
      {"routeCount", routeCount_},
      {"transformCount", transformCount_},
      {"overlayCount", overlayCount_},
      {"outputs", stringArray(session.destinations)},
      {"isoParticipantIds", stringArray(session.isoParticipantIds)},
      {"active", session.active},
      {"health", health()},
      {"profile", profile()},
  };
}

rpc::Json MediaCore::applyCommands(const rpc::Json::Array& commands) {
  for (const auto& command : commands) {
    (void)applyCommand(command);
  }
  renderSyntheticTick();
  return sessionState();
}

rpc::Json MediaCore::applyCommand(const rpc::Json& command) {
  const std::string type = command.getString("type");
  if (type == "load-scene-graph") {
    loadSceneGraph(command);
  } else if (type == "set-participant-transform") {
    setParticipantTransform(command);
  } else if (type == "set-overlay-asset") {
    setOverlayAsset(command);
  } else if (type == "start-program-output") {
    startProgramOutput(command);
  }
  return sessionState();
}

void MediaCore::loadSceneGraph(const rpc::Json& command) {
  sceneId_ = command.getString("sceneId", "unloaded");
  const rpc::Json* routes = command.get("routes");
  routeCount_ = routes && routes->isArray() ? static_cast<int>(routes->asArray().size()) : 0;
}

void MediaCore::setParticipantTransform(const rpc::Json&) {
  ++transformCount_;
}

void MediaCore::setOverlayAsset(const rpc::Json&) {
  ++overlayCount_;
}

void MediaCore::startProgramOutput(const rpc::Json& command) {
  modules_.encoder->start(command.getStringArray("destinations"), command.getStringArray("isoParticipantIds"));
  renderSyntheticTick();
}

void MediaCore::renderSyntheticTick() {
  auto videoFrames = modules_.zoom->pollVideoFrames();
  auto audioFrames = modules_.zoom->pollAudioFrames();
  mixedAudioFrameCount_ = modules_.mixer->mix(audioFrames);
  lastProgramFrame_ = modules_.compositor->render(videoFrames, routeCount_ + overlayCount_);
  modules_.encoder->submit(lastProgramFrame_);
}

}  // namespace corevideo::core
