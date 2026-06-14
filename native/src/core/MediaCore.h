#pragma once

#include "modules/Interfaces.h"
#include "rpc/Json.h"

#include <string>

namespace corevideo::core {

class MediaCore {
 public:
  explicit MediaCore(modules::ModuleSet modules = modules::createStubModules());

  [[nodiscard]] rpc::Json profile() const;
  [[nodiscard]] rpc::Json sessionState() const;
  [[nodiscard]] rpc::Json health() const;
  [[nodiscard]] rpc::Json applyCommand(const rpc::Json& command);
  [[nodiscard]] rpc::Json applyCommands(const rpc::Json::Array& commands);

 private:
  void loadSceneGraph(const rpc::Json& command);
  void setParticipantTransform(const rpc::Json& command);
  void setOverlayAsset(const rpc::Json& command);
  void startProgramOutput(const rpc::Json& command);
  void renderSyntheticTick();

  modules::ModuleSet modules_;
  std::string sceneId_ = "unloaded";
  int routeCount_ = 0;
  int transformCount_ = 0;
  int overlayCount_ = 0;
  int64_t mixedAudioFrameCount_ = 0;
  modules::ProgramFrame lastProgramFrame_;
};

}  // namespace corevideo::core
