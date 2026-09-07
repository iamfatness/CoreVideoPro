#pragma once

#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"
#include "rpc/Json.h"
#include <algorithm>
#include <tuple>

namespace corevideo::core {

// Owning attribution for the last composed Program frame, not current routing
// intent. Binding identity does not prove fresh pixels from a source.
class RenderedProgramSources {
  struct Overlay {
    std::string layerId, sourceId, title, org, text, imageUri, keyPosition, keyer, keyPhase;
    auto identity() const { return std::tie(layerId, sourceId, title, org, text, imageUri, keyPosition, keyer, keyPhase); }
    bool matches(const modules::CompositorRenderPlanLayer& layer) const {
      return identity() == std::tie(layer.layerId, layer.sourceId, layer.overlay.title,
          layer.overlay.org, layer.overlay.text, layer.overlay.imageUri, layer.overlay.keyPosition,
          layer.overlay.keyer, layer.overlay.keyPhase);
    }
  };
 public:
  void publish(const modules::CompositorRenderPlan& plan) {
    size_t overlayIndex = 0;
    bool overlaysChanged = false;
    for (const auto& layer : plan.layers) {
      if (!isVisibleOverlay(layer)) continue;
      if (overlayIndex >= overlayLayers_.size() || !overlayLayers_[overlayIndex].matches(layer)) overlaysChanged = true;
      ++overlayIndex;
    }
    if (overlaysChanged || overlayIndex != overlayLayers_.size()) {
      overlayLayers_.clear();
      for (const auto& layer : plan.layers) {
        if (isVisibleOverlay(layer)) overlayLayers_.push_back({layer.layerId, layer.sourceId,
            layer.overlay.title, layer.overlay.org, layer.overlay.text, layer.overlay.imageUri,
            layer.overlay.keyPosition, layer.overlay.keyer, layer.overlay.keyPhase});
      }
    }
    size_t index = 0;
    bool changed = sceneId_ != plan.sceneId;
    for (const auto& layer : plan.layers) {
      if (!isVideo(layer)) continue;
      if (index >= bindings_.size() || !bindings_[index].matches(layer)) changed = true;
      ++index;
    }
    if (!changed && index == bindings_.size()) return;
    sceneId_ = plan.sceneId;
    bindings_.clear();
    wire_.clear();
    bindings_.reserve(index);
    wire_.reserve(index);
    for (const auto& layer : plan.layers) {
      if (!isVideo(layer)) continue;
      bindings_.push_back({layer.layerId, layer.sourceId, layer.participantId, layer.kind, layer.order});
    }
    const auto sorted = modules::sortCompositorRenderPlan(plan);
    for (const auto& layer : sorted.layers) {
      if (!isVideo(layer)) continue;
      wire_.emplace_back(rpc::Json::Object{{"layerId", layer.layerId}, {"sourceId", layer.sourceId},
          {"participantId", layer.participantId}, {"kind", layer.kind}});
    }
  }
  const std::string& sceneId() const { return sceneId_; }
  const rpc::Json::Array& videoSources() const { return wire_; }
  void invalidate() {
    sceneId_.clear(); bindings_.clear(); wire_.clear(); overlayLayers_.clear();
  }
  bool containsOverlay(const std::string& layerId, const std::string& sourceId,
      const std::string& title, const std::string& org, const std::string& text,
      const std::string& imageUri, const std::string& keyPosition, const std::string& keyer,
      const std::string& keyPhase) const {
    return std::any_of(overlayLayers_.begin(), overlayLayers_.end(), [&](const auto& overlay) {
      return overlay.identity() == std::tie(layerId, sourceId, title, org, text, imageUri, keyPosition, keyer, keyPhase);
    });
  }

 private:
  static bool isVisibleOverlay(const modules::CompositorRenderPlanLayer& layer) {
    return layer.hasOverlayContent && layer.opacity > 0.f &&
        (layer.overlay.keyPhase == "on-air" ||
         (layer.overlay.keyPhase == "building-in" && layer.overlay.keyProgress > 0.f));
  }
  struct Binding {
    std::string layerId, sourceId, participantId, kind;
    int order;
    bool matches(const modules::CompositorRenderPlanLayer& layer) const {
      return layerId == layer.layerId && sourceId == layer.sourceId &&
          participantId == layer.participantId && kind == layer.kind && order == layer.order;
    }
  };
  static bool isVideo(const modules::CompositorRenderPlanLayer& layer) {
    return !layer.sourceId.empty() && !layer.hasOverlayContent && layer.opacity > 0.f &&
        (layer.kind == "participant-video" || layer.kind == "screen-share" || layer.kind == "media-video");
  }
  std::string sceneId_;
  std::vector<Binding> bindings_;
  rpc::Json::Array wire_;
  std::vector<Overlay> overlayLayers_;
};

}  // namespace corevideo::core
