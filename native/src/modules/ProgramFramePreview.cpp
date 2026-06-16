#include "modules/ProgramFramePreview.h"

#include "compositor/CompositorLayout.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace corevideo::modules {
namespace {

uint32_t unpackColor(uint32_t rgba) {
  return rgba;
}

void setPixelBgra(std::vector<uint8_t>& pixels, int width, int x, int y, uint32_t rgba) {
  if (x < 0 || y < 0 || x >= width) {
    return;
  }
  const size_t offset = static_cast<size_t>((y * width + x) * 4);
  if (offset + 3 >= pixels.size()) {
    return;
  }
  pixels[offset + 0] = static_cast<uint8_t>(rgba & 0xff);
  pixels[offset + 1] = static_cast<uint8_t>((rgba >> 8) & 0xff);
  pixels[offset + 2] = static_cast<uint8_t>((rgba >> 16) & 0xff);
  pixels[offset + 3] = static_cast<uint8_t>((rgba >> 24) & 0xff);
}

void fillRectBgra(std::vector<uint8_t>& pixels, int width, int height, float x, float y, float w, float h, uint32_t rgba) {
  const int left = std::max(0, static_cast<int>(std::floor(x * static_cast<float>(width))));
  const int top = std::max(0, static_cast<int>(std::floor(y * static_cast<float>(height))));
  const int right = std::min(width, static_cast<int>(std::ceil((x + w) * static_cast<float>(width))));
  const int bottom = std::min(height, static_cast<int>(std::ceil((y + h) * static_cast<float>(height))));
  for (int py = top; py < bottom; ++py) {
    for (int px = left; px < right; ++px) {
      setPixelBgra(pixels, width, px, py, rgba);
    }
  }
}

}  // namespace

void computeProgramFramePreviewSize(int sourceWidth, int sourceHeight, int& outWidth, int& outHeight) {
  if (sourceWidth <= 0 || sourceHeight <= 0) {
    outWidth = 0;
    outHeight = 0;
    return;
  }

  const double scale = std::min(
      static_cast<double>(kProgramFramePreviewMaxWidth) / static_cast<double>(sourceWidth),
      static_cast<double>(kProgramFramePreviewMaxHeight) / static_cast<double>(sourceHeight));
  outWidth = std::clamp(static_cast<int>(std::lround(static_cast<double>(sourceWidth) * scale)), 1, kProgramFramePreviewMaxWidth);
  outHeight = std::clamp(static_cast<int>(std::lround(static_cast<double>(sourceHeight) * scale)), 1, kProgramFramePreviewMaxHeight);
}

void fillSyntheticProgramFramePreview(
    ProgramFramePreviewPixels& preview,
    const CompositorRenderPlan& renderPlan,
    const std::vector<VideoFrame>& frames,
    const ProgramFrame& frame) {
  int previewWidth = 0;
  int previewHeight = 0;
  computeProgramFramePreviewSize(
      frame.width > 0 ? frame.width : renderPlan.width,
      frame.height > 0 ? frame.height : renderPlan.height,
      previewWidth,
      previewHeight);
  if (previewWidth <= 0 || previewHeight <= 0) {
    preview = {};
    return;
  }

  preview.width = previewWidth;
  preview.height = previewHeight;
  preview.bgra.assign(static_cast<size_t>(previewWidth * previewHeight * 4), 0);
  const uint32_t background = 0xff0c1118;
  for (int y = 0; y < previewHeight; ++y) {
    for (int x = 0; x < previewWidth; ++x) {
      setPixelBgra(preview.bgra, previewWidth, x, y, background);
    }
  }

  if (!renderPlan.layers.empty()) {
    int videoIndex = 0;
    for (const auto& layer : renderPlan.layers) {
      auto rect = layer.rect;
      if (rect.width <= 0.f || rect.height <= 0.f) {
        if (layer.kind == "overlay") {
          const auto layout = layer.layerId.find("lower") != std::string::npos ? compositor::lowerThirdOverlay()
                                                                                 : compositor::topRightOverlay();
          rect = {layout.x, layout.y, layout.width, layout.height};
        } else {
          const int videoLayerCount = static_cast<int>(std::count_if(
              renderPlan.layers.begin(),
              renderPlan.layers.end(),
              [](const CompositorRenderPlanLayer& entry) { return entry.kind != "overlay"; }));
          const auto layout = compositor::gridCell((std::max)(1, videoLayerCount), videoIndex);
          rect = {layout.x, layout.y, layout.width, layout.height};
          ++videoIndex;
        }
      }

      uint32_t color = 0xff2a3548;
      if (layer.kind != "overlay") {
        if (!layer.participantId.empty()) {
          color = compositor::colorFromParticipantId(layer.participantId);
        } else if (videoIndex > 0 && videoIndex - 1 < static_cast<int>(frames.size())) {
          color = compositor::colorFromParticipantId(frames[static_cast<size_t>(videoIndex - 1)].participantId);
        }
      }
      fillRectBgra(preview.bgra, previewWidth, previewHeight, rect.x, rect.y, rect.width, rect.height, unpackColor(color));
    }
    return;
  }

  const int count = static_cast<int>(frames.size());
  for (int index = 0; index < count; ++index) {
    const auto layout = compositor::gridCell((std::max)(1, count), index);
    const auto color = compositor::colorFromParticipantId(frames[static_cast<size_t>(index)].participantId);
    fillRectBgra(preview.bgra, previewWidth, previewHeight, layout.x, layout.y, layout.width, layout.height, unpackColor(color));
  }
}

void downscaleBgraNearestNeighbor(
    const uint8_t* source,
    int sourceWidth,
    int sourceHeight,
    int sourceStride,
    ProgramFramePreviewPixels& preview) {
  if (!source || sourceWidth <= 0 || sourceHeight <= 0 || sourceStride < sourceWidth * 4) {
    preview = {};
    return;
  }

  int previewWidth = 0;
  int previewHeight = 0;
  computeProgramFramePreviewSize(sourceWidth, sourceHeight, previewWidth, previewHeight);
  if (previewWidth <= 0 || previewHeight <= 0) {
    preview = {};
    return;
  }

  preview.width = previewWidth;
  preview.height = previewHeight;
  preview.bgra.resize(static_cast<size_t>(previewWidth * previewHeight * 4));

  for (int y = 0; y < previewHeight; ++y) {
    const int sourceY = std::min(sourceHeight - 1, (y * sourceHeight) / previewHeight);
    const auto* sourceRow = source + static_cast<size_t>(sourceY) * static_cast<size_t>(sourceStride);
    for (int x = 0; x < previewWidth; ++x) {
      const int sourceX = std::min(sourceWidth - 1, (x * sourceWidth) / previewWidth);
      const auto* pixel = sourceRow + static_cast<size_t>(sourceX) * 4u;
      const size_t offset = static_cast<size_t>((y * previewWidth + x) * 4);
      preview.bgra[offset + 0] = pixel[0];
      preview.bgra[offset + 1] = pixel[1];
      preview.bgra[offset + 2] = pixel[2];
      preview.bgra[offset + 3] = pixel[3];
    }
  }
}

std::string base64Encode(const uint8_t* data, size_t length) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (!data || length == 0) {
    return {};
  }

  std::string encoded;
  encoded.reserve(((length + 2) / 3) * 4);
  for (size_t index = 0; index < length; index += 3) {
    const uint32_t chunk = (static_cast<uint32_t>(data[index]) << 16) |
                           ((index + 1 < length ? static_cast<uint32_t>(data[index + 1]) : 0u) << 8) |
                           (index + 2 < length ? static_cast<uint32_t>(data[index + 2]) : 0u);
    encoded.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
    encoded.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
    encoded.push_back(index + 1 < length ? kAlphabet[(chunk >> 6) & 0x3f] : '=');
    encoded.push_back(index + 2 < length ? kAlphabet[chunk & 0x3f] : '=');
  }
  return encoded;
}

rpc::Json programSharedTextureJson(const ProgramFrame& frame) {
  if (frame.sharedTexture.sharedHandleHex.empty() || frame.sharedTexture.width <= 0 || frame.sharedTexture.height <= 0) {
    return nullptr;
  }

  return rpc::Json::Object{
      {"sharedHandleHex", frame.sharedTexture.sharedHandleHex},
      {"width", frame.sharedTexture.width},
      {"height", frame.sharedTexture.height},
      {"format", frame.sharedTexture.format.empty() ? "B8G8R8A8_UNORM" : frame.sharedTexture.format},
      {"frameNumber", static_cast<double>(frame.sharedTexture.frameNumber > 0 ? frame.sharedTexture.frameNumber : frame.frameNumber)},
  };
}

rpc::Json programSharedTextureEvent(const ProgramFrame& frame) {
  const auto texture = programSharedTextureJson(frame);
  if (texture.isNull()) {
    return nullptr;
  }

  return rpc::Json::Object{
      {"type", "program-shared-texture"},
      {"texture", texture},
  };
}

rpc::Json programFramePreviewJson(const ProgramFrame& frame) {
  if (frame.preview.width <= 0 || frame.preview.height <= 0 || frame.preview.bgra.empty()) {
    return nullptr;
  }

  rpc::Json::Object preview{
      {"frameNumber", static_cast<double>(frame.frameNumber)},
      {"width", frame.preview.width},
      {"height", frame.preview.height},
      {"renderPlanId", frame.renderPlanId},
      {"renderer", frame.renderer},
      {"health", frame.health},
      {"pixelFormat", "bgra"},
      {"bgraBase64", base64Encode(frame.preview.bgra.data(), frame.preview.bgra.size())},
  };
  const auto sharedTexture = programSharedTextureJson(frame);
  if (!sharedTexture.isNull()) {
    preview.emplace("sharedTexture", sharedTexture);
  }
  return preview;
}

rpc::Json programFramePreviewEvent(const ProgramFrame& frame) {
  const auto preview = programFramePreviewJson(frame);
  if (preview.isNull()) {
    return nullptr;
  }

  return rpc::Json::Object{
      {"type", "program-frame-preview"},
      {"preview", preview},
  };
}

}  // namespace corevideo::modules