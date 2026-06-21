#pragma once

#include "modules/Interfaces.h"
#include "rpc/Json.h"

#include <cstdint>
#include <string>
#include <vector>

namespace corevideo::modules {

inline constexpr int kProgramFramePreviewMaxWidth = 320;
inline constexpr int kProgramFramePreviewMaxHeight = 180;

void computeProgramFramePreviewSize(int sourceWidth, int sourceHeight, int& outWidth, int& outHeight);

[[nodiscard]] bool compositorLayerIsOverlay(const CompositorRenderPlanLayer& layer);

[[nodiscard]] bool compositorLayerIsLowerThird(const CompositorRenderPlanLayer& layer);

[[nodiscard]] bool compositorLayerIsChromaKey(const CompositorRenderPlanLayer& layer);

[[nodiscard]] float compositorLayerOpacity(const CompositorRenderPlanLayer& layer);

[[nodiscard]] CompositorRenderPlan sortCompositorRenderPlan(CompositorRenderPlan renderPlan);

[[nodiscard]] uint32_t renderPlanSignature(const CompositorRenderPlan& renderPlan);

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

[[nodiscard]] std::vector<uint8_t> base64Decode(const std::string& encoded);

// Copies a layer's decoded BGRA frame into its rect of the preview buffer,
// honoring the layer opacity. The source is scaled (nearest-neighbor) to fit
// the rect. Returns true when pixels were drawn, false when the frame was not
// usable (callers then fall back to the synthetic color fill).
bool blitVideoFrameIntoPreviewRect(
    ProgramFramePreviewPixels& preview,
    const VideoFrame& frame,
    const CompositorLayerRect& rect,
    float opacity);

// Like blitVideoFrameIntoPreviewRect, but applies per-source framing (Item 8):
// the frame is sampled from the normalized source-UV window [u0,v0]-[u1,v1] and
// drawn into the on-screen `contentRect` (which is inset/centered for letterbox
// fit modes). This is the CPU mirror of the D3D11 framing UV transform so the
// software preview samples the same source region as the GPU program.
bool blitVideoFrameFramed(
    ProgramFramePreviewPixels& preview,
    const VideoFrame& frame,
    const CompositorLayerRect& contentRect,
    float u0,
    float v0,
    float u1,
    float v1,
    float opacity);

[[nodiscard]] rpc::Json programSharedTextureJson(const ProgramFrame& frame);

[[nodiscard]] rpc::Json programSharedTextureEvent(const ProgramFrame& frame);

[[nodiscard]] rpc::Json programFramePreviewJson(const ProgramFrame& frame);

[[nodiscard]] rpc::Json programFramePreviewEvent(const ProgramFrame& frame);

}  // namespace corevideo::modules
