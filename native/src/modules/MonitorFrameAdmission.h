#pragma once
#include "modules/Interfaces.h"
#include <algorithm>

namespace corevideo::modules {
// Named plans consume only their routed sources. Legacy unnamed/positional
// plans conservatively keep the input order until their caller supplies names.
inline bool monitorPlanNeedsFrame(const CompositorRenderPlan& plan, const VideoFrame& frame) {
  if (plan.layers.empty()) return true;
  return std::any_of(plan.layers.begin(), plan.layers.end(), [&](const auto& layer) {
    if (layer.participantId == frame.participantId || layer.sourceId == frame.participantId ||
        (!layer.mediaAssetId.empty() && "media:" + layer.mediaAssetId == frame.participantId)) return true;
    return layer.participantId.empty() && layer.mediaAssetId.empty() && !layer.hasFillColor;
  });
}
inline bool prepareMonitorFrames(MonitorRenderRequest& request) {
  request.frames.erase(std::remove_if(request.frames.begin(), request.frames.end(), [&](const auto& frame) {
    return !(request.previewActive && monitorPlanNeedsFrame(request.previewPlan, frame)) &&
        !(request.multiviewActive && monitorPlanNeedsFrame(request.multiviewPlan, frame)) &&
        std::none_of(request.sourceExports.begin(), request.sourceExports.end(),
            [&](const auto& demand) { return demand.sourceId == frame.participantId; });
  }), request.frames.end());
  bool admitted = true;
  for (auto& frame : request.frames) {
    const bool hadGpu = frame.hasGpuPixels();
    frame.gpuPixels = std::move(frame.monitorGpuPixels);
    if (hadGpu && !frame.hasContent()) admitted = false;
  }
  return admitted;
}
}
