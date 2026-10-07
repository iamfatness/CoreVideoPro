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
inline bool monitorRequestNeedsFrame(const MonitorRenderRequest& request, const VideoFrame& frame) {
  return (request.previewActive && monitorPlanNeedsFrame(request.previewPlan, frame)) ||
      (request.multiviewActive && monitorPlanNeedsFrame(request.multiviewPlan, frame)) ||
      std::any_of(request.sourceExports.begin(), request.sourceExports.end(),
          [&](const auto& demand) { return demand.sourceId == frame.participantId; });
}
inline bool prepareMonitorFrames(MonitorRenderRequest& request) {
  request.unavailableInputs.clear();
  request.frames.erase(std::remove_if(request.frames.begin(), request.frames.end(), [&](const auto& frame) {
    return !monitorRequestNeedsFrame(request, frame);
  }), request.frames.end());
  for (auto& frame : request.frames) {
    const bool hadGpu = frame.hasGpuPixels();
    // Pool generations are independent. Validate role and dimensions, not
    // equality of those counters. Capture publishes both from the same copy.
    if (frame.monitorGpuPixels && (!frame.monitorGpuPixels->monitorPrivate ||
        frame.monitorGpuPixels == frame.gpuPixels ||
        (hadGpu && (frame.monitorGpuPixels->width != frame.gpuPixels->width ||
                    frame.monitorGpuPixels->height != frame.gpuPixels->height))))
      frame.monitorGpuPixels.reset();
    frame.gpuPixels = std::move(frame.monitorGpuPixels);
    if (hadGpu && !frame.hasContent()) request.unavailableInputs.push_back(frame.participantId);
  }
  return true; // unavailable inputs never refuse unrelated monitor work
}
}
