#include "core/MediaCore.h"
#include "core/MultiviewFeed.h"
#include <algorithm>

namespace corevideo::core {

// Assemble only refs, plans and bounded export demand. Pixels remain on the
// independent monitor owner; Program rendering never gates this dispatch.
void MediaCore::submitIsolatedMonitorTick(const modules::CompositorRenderPlan& renderPlan,
    const std::vector<modules::VideoFrame>& videoFrames, int64_t sequence) {
  modules::MonitorRenderRequest request;
  request.sequence = sequence;
  request.programPlan = renderPlan;
  request.frames = videoFrames;
  request.multiviewActive = !multiviewSources_.empty() || multiviewLayoutMode_ != "grid";
  request.previewActive = hasPreviewScene();
  // WinUI presents Program/Preview/Multiview composites. Its only individual
  // GPU-source consumer is the legacy Preview fallback before a scene sync;
  // scene-editor layers consume CPU thumbnails. Keep that compatibility
  // demand until a composite Preview takes ownership, then stop those exports.
  if (!request.previewActive) {
    for (const auto& frame : videoFrames)
      request.sourceExports.push_back({frame.participantId,
          modules::SourceMonitorConsumer::PreviewFallback, "preview-fallback"});
  }
  // Sources inspectors need identity-safe full-source exports even while
  // composite Preview is active. Bound demand to ten assigned input rows;
  // never export every participant. programPlan carries off-air source grades.
  int inspectorRows = 0;
  for (const auto& source : multiviewSources_) {
    if (!sourcePreviewDemand_.active() || inspectorRows++ >= 10) break;
    const auto feed = resolveMultiviewFeed(source.kind, source.sourceId, source.participantId,
        source.captureDeviceId, source.mediaAssetId);
    const auto nativeId = feed.participantId.empty() ? feed.sourceId : feed.participantId;
    if (nativeId.empty()) continue;
    if (std::none_of(request.sourceExports.begin(), request.sourceExports.end(),
        [&](const auto& demand) { return demand.sourceId == nativeId; }))
      request.sourceExports.push_back({nativeId, modules::SourceMonitorConsumer::Inspector,
          "source-thumbnail:" + feed.sourceId});
  }
  if (request.multiviewActive) {
    request.multiviewPlan = buildMultiviewRenderPlan(videoFrames);
    request.multiviewPlan.skipCpuReadback = true;
    request.tiles = buildMultiviewTiles(zoomSnapshot().getString("activeSpeakerId"));
  }
  if (request.previewActive) {
    request.previewPlan = buildPreviewCompositorRenderPlan(videoFrames);
    if (const auto* wall = tilesWallSources_.find(previewTilesLayer_.layerId))
      wall->applyLatest(request.previewPlan, previewTilesLayer_.layerId);
    request.previewPlan.skipCpuReadback = true;
    lastPreviewPlanLayerCount_ = static_cast<int>(request.previewPlan.layers.size());
  }
  modules_.compositor->submitMonitors(std::move(request));
}

}  // namespace corevideo::core
