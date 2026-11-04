#include "core/MediaCore.h"
#include "modules/StillMediaFrameCache.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace corevideo::core {
namespace {
bool layoutId(const std::string& id) {
  return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
  });
}

bool layoutRevision(double value, double minimum = 1) {
  return value >= minimum && value <= 9007199254740991.0 && std::floor(value) == value;
}

void addColdAsset(const rpc::Json& asset, const std::string& prefix, std::vector<std::string>& sources) {
  const auto id = asset.getString("mediaAssetId");
  const auto path = asset.getString("mediaAssetPath");
  if (!id.empty() && !path.empty() && !modules::isStillImageMediaAsset(asset.getString("mediaAssetKind"), path))
    sources.push_back(prefix + id);
}
}  // namespace

bool MediaCore::handleGuardedLayoutBatch(const rpc::Json::Array& commands) {
  const rpc::Json* transition = nullptr;
  const rpc::Json* incoming = nullptr;
  size_t transitions = 0, sceneLoads = 0;
  for (const auto& command : commands) {
    const auto type = command.getString("type");
    if (type == "begin-take-transition") {
      ++transitions;
      if (command.get("graphicsLayout")) transition = &command;
    } else if (type == "load-scene-graph") {
      incoming = &command;
      ++sceneLoads;
    }
  }
  if (!transition) {
    if (!pendingLayoutTake_ || (sceneLoads == 0 && transitions == 0)) return false;
    commandProtocolFailures_.push_back("guarded-layout-take-pending");
    return true;
  }

  const auto id = transition->getString("operationId");
  if (!layoutId(id)) {
    commandProtocolFailures_.push_back("invalid-layout-operation-id");
    return true;
  }
  if (pendingLayoutTake_ && pendingLayoutTake_->operationId == id) {
    if (pendingLayoutTake_->ready) {
      auto take = std::move(*pendingLayoutTake_);
      pendingLayoutTake_.reset();
      commitGuardedLayout(std::move(take));
    }
    return true;
  }
  if (std::any_of(layoutTakeReceipts_.begin(), layoutTakeReceipts_.end(),
                  [&](const auto& receipt) { return receipt.getString("operationId") == id; }))
    return true;
  const auto refuse = [&](const std::string& reason) {
    recordLayoutTake(id, "refused", reason);
    return true;
  };
  if (pendingLayoutTake_) return refuse("another-layout-take-is-pending");
  if (takeTransition_.waitingForColdMedia) return refuse("an-earlier-take-is-warming");
  if (transitions != 1 || sceneLoads != 1 || transition != &commands.front())
    return refuse("invalid-layout-take-batch");
  if (std::any_of(commands.begin(), commands.end(), [](const auto& command) {
        return command.getString("type").starts_with("graphics-");
      })) return refuse("layout-take-batch-has-graphics-mutations");

  const auto& guard = *transition->get("graphicsLayout");
  if (!guard.isObject()) return refuse("invalid-layout-guard");
  const auto* cues = guard.get("cues");
  const auto* keep = guard.get("keepProgramScene");
  const auto* playOut = guard.get("playOutOthers");
  const auto sceneId = guard.getString("sceneId");
  const auto previewRevision = guard.getNumber("previewRevision", -1);
  if (!cues || !cues->isArray() || cues->asArray().empty() || cues->asArray().size() > 8 ||
      (keep && !keep->isBool()) || !playOut || !playOut->isBool() ||
      sceneId.empty() || sceneId == "unloaded" || incoming->getString("sceneId") != sceneId ||
      !layoutRevision(previewRevision, keep && keep->asBool() ? 0 : 1))
    return refuse("invalid-layout-guard");

  GuardedLayoutTake take;
  take.operationId = id;
  take.coreEpoch = guard.getString("coreEpoch");
  take.sceneId = sceneId;
  take.previewRevision = static_cast<uint64_t>(previewRevision);
  take.keepProgramScene = keep && keep->asBool();
  take.playOutOthers = playOut->asBool();
  std::set<std::string> instances;
  for (const auto& cue : cues->asArray()) {
    const auto instanceId = cue.getString("instanceId");
    const auto revision = cue.getNumber("cueRevision", -1);
    if (!layoutId(instanceId) || !layoutRevision(revision) || !instances.insert(instanceId).second)
      return refuse("invalid-layout-cue");
    take.cues.push_back({instanceId, static_cast<uint64_t>(revision)});
  }
  if (const auto reason = validateGuardedLayout(take); !reason.empty()) return refuse(reason);
  take.commands = commands;
  if (!take.keepProgramScene && sceneId_ != sceneId && transition->getString("mode", "cut") == "cut") {
    if (const auto* routes = incoming->get("routes"); routes && routes->isArray())
      for (const auto& route : routes->asArray()) addColdAsset(route, "media:", take.coldSources);
    if (const auto* background = incoming->get("background"); background && background->isObject())
      addColdAsset(*background, "background:", take.coldSources);
  }
  if (take.coldSources.empty()) {
    commitGuardedLayout(std::move(take));
  } else {
    pendingLayoutTake_ = std::move(take);
    recordLayoutTake(id, "warming");
  }
  return true;
}

std::string MediaCore::validateGuardedLayout(const GuardedLayoutTake& take) const {
  if (graphicsInstances_.snapshot().getString("coreEpoch") != take.coreEpoch)
    return "layout-core-changed";
  if (previewSceneRevision_ != take.previewRevision ||
      (take.keepProgramScene ? sceneId_ != take.sceneId : previewSceneId_ != take.sceneId))
    return "layout-scene-changed";
  if (const auto instance = graphicsInstances_.unavailableLayoutCue(take.operationId, take.cues); !instance.empty())
    return "layout-cue-unavailable:" + instance;
  return {};
}

void MediaCore::observeGuardedLayoutWarmup(const std::vector<modules::VideoFrame>& frames, double elapsedMs) {
  if (!pendingLayoutTake_) return;
  auto& take = *pendingLayoutTake_;
  if (const auto reason = validateGuardedLayout(take); !reason.empty()) {
    recordLayoutTake(take.operationId, "refused", reason);
    pendingLayoutTake_.reset();
    return;
  }
  take.warmupMs += elapsedMs;
  if (take.warmupMs >= 5000.0) {
    recordLayoutTake(take.operationId, "refused", "layout-take-expired");
    pendingLayoutTake_.reset();
    return;
  }
  const bool decoded = std::all_of(take.coldSources.begin(), take.coldSources.end(), [&](const auto& source) {
    return std::any_of(frames.begin(), frames.end(), [&](const auto& frame) {
      return frame.participantId == source && frame.hasContent();
    });
  });
  // Use the existing bounded cut warmup. The command lane applies the batch
  // on a replay of this operation ID after the render tick reports readiness.
  const bool ready = decoded || take.warmupMs >= 750.0;
  if (ready != take.ready) {
    take.ready = ready;
    recordLayoutTake(take.operationId, ready ? "ready" : "warming");
  }
}

void MediaCore::commitGuardedLayout(GuardedLayoutTake take) {
  if (const auto reason = validateGuardedLayout(take); !reason.empty()) {
    recordLayoutTake(take.operationId, "refused", reason);
    return;
  }
  if (!graphicsInstances_.commitLayoutTake(take.operationId, take.cues, take.playOutOthers)) {
    recordLayoutTake(take.operationId, "refused", "layout-cues-changed");
    return;
  }

  committingLayoutTake_ = true;
  for (const auto& command : take.commands) {
    const auto type = command.getString("type");
    if (take.keepProgramScene && (type == "begin-take-transition" || type == "load-scene-graph" ||
                                  type == "set-preview-scene")) continue;
    applyCommandMutation(command);
    ++appliedControlRevision_;
  }
  committingLayoutTake_ = false;
  recordLayoutTake(take.operationId, "committed");
}

void MediaCore::recordLayoutTake(const std::string& id, const std::string& disposition, const std::string& reason) {
  const auto existing = std::find_if(layoutTakeReceipts_.begin(), layoutTakeReceipts_.end(),
                                     [&](const auto& row) { return row.getString("operationId") == id; });
  if (existing != layoutTakeReceipts_.end()) layoutTakeReceipts_.erase(existing);
  layoutTakeReceipts_.push_back(rpc::Json::Object{{"operationId", id}, {"disposition", disposition}, {"reason", reason}});
  if (layoutTakeReceipts_.size() > 32) layoutTakeReceipts_.pop_front();
}
}  // namespace corevideo::core
