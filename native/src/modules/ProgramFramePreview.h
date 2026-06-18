#pragma once

#include "modules/Interfaces.h"
#include "rpc/Json.h"

#include <cstdint>
#include <string>

namespace corevideo::modules {

inline constexpr int kProgramFramePreviewMaxWidth = 320;
inline constexpr int kProgramFramePreviewMaxHeight = 180;

void computeProgramFramePreviewSize(int sourceWidth, int sourceHeight, int& outWidth, int& outHeight);

[[nodiscard]] bool compositorLayerIsOverlay(const CompositorRenderPlanLayer& layer);

[[nodiscard]] bool compositorLayerIsLowerThird(const CompositorRenderPlanLayer& layer);

[[nodiscard]] bool compositorLayerIsChromaKey(const CompositorRenderPlanLayer& layer);

[[nodiscard]] float compositorLayerOpacity(const CompositorRenderPlanLayer& layer);

[[nodiscard]] CompositorRenderPlan sortCompositorRenderPlan(CompositorRenderPlan renderPlan);

void fillSyntheticProgramFramePreview(
    ProgramFramePreviewPixels& preview,
    const CompositorRenderPlan& renderPlan,
    const std::vector<VideoFrame>& frames,
    const ProgramFrame& frame);

void downscaleBgraNearestNeighbor(
    const uint8_t* source,
    int sourceWidth,
    int sourceHeight,
    int sourceStride,
    ProgramFramePreviewPixels& preview);

[[nodiscard]] std::string base64Encode(const uint8_t* data, size_t length);

[[nodiscard]] rpc::Json programSharedTextureJson(const ProgramFrame& frame);

[[nodiscard]] rpc::Json programSharedTextureEvent(const ProgramFrame& frame);

[[nodiscard]] rpc::Json programFramePreviewJson(const ProgramFrame& frame);

[[nodiscard]] rpc::Json programFramePreviewEvent(const ProgramFrame& frame);

}  // namespace corevideo::modules
