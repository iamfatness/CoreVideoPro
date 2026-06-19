#include "modules/ProgramFramePreview.h"

#include "compositor/CompositorLayout.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace corevideo::modules {
namespace {

uint32_t unpackColor(uint32_t rgba) {
  return rgba;
}

std::string lowercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool layerTextContains(const CompositorRenderPlanLayer& layer, const std::string& needle) {
  return lowercaseAscii(layer.layerId).find(needle) != std::string::npos ||
         lowercaseAscii(layer.kind).find(needle) != std::string::npos ||
         lowercaseAscii(layer.sourceId).find(needle) != std::string::npos;
}

int semanticLayerDepth(const CompositorRenderPlanLayer& layer) {
  if (compositorLayerIsLowerThird(layer)) {
    return 3000;
  }
  if (compositorLayerIsOverlay(layer)) {
    return 2500;
  }
  if (compositorLayerIsChromaKey(layer)) {
    return 1500;
  }
  return 1000;
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

void blendPixelBgra(std::vector<uint8_t>& pixels, int width, int x, int y, uint32_t rgba, float opacity) {
  if (x < 0 || y < 0 || x >= width) {
    return;
  }
  const size_t offset = static_cast<size_t>((y * width + x) * 4);
  if (offset + 3 >= pixels.size()) {
    return;
  }

  const float sourceAlpha = std::clamp(static_cast<float>((rgba >> 24) & 0xff) / 255.f * opacity, 0.f, 1.f);
  const float inverseAlpha = 1.f - sourceAlpha;
  pixels[offset + 0] = static_cast<uint8_t>(std::lround(static_cast<float>(rgba & 0xff) * sourceAlpha + pixels[offset + 0] * inverseAlpha));
  pixels[offset + 1] = static_cast<uint8_t>(std::lround(static_cast<float>((rgba >> 8) & 0xff) * sourceAlpha + pixels[offset + 1] * inverseAlpha));
  pixels[offset + 2] = static_cast<uint8_t>(std::lround(static_cast<float>((rgba >> 16) & 0xff) * sourceAlpha + pixels[offset + 2] * inverseAlpha));
  pixels[offset + 3] = 0xff;
}

const VideoFrame* findFrameForParticipant(const std::vector<VideoFrame>& frames, const std::string& participantId) {
  if (participantId.empty()) {
    return nullptr;
  }
  for (const auto& frame : frames) {
    if (frame.participantId == participantId) {
      return &frame;
    }
  }
  return nullptr;
}

void fillRectBgra(std::vector<uint8_t>& pixels, int width, int height, float x, float y, float w, float h, uint32_t rgba, float opacity) {
  if (opacity <= 0.f) {
    return;
  }
  const int left = std::max(0, static_cast<int>(std::floor(x * static_cast<float>(width))));
  const int top = std::max(0, static_cast<int>(std::floor(y * static_cast<float>(height))));
  const int right = std::min(width, static_cast<int>(std::ceil((x + w) * static_cast<float>(width))));
  const int bottom = std::min(height, static_cast<int>(std::ceil((y + h) * static_cast<float>(height))));
  for (int py = top; py < bottom; ++py) {
    for (int px = left; px < right; ++px) {
      blendPixelBgra(pixels, width, px, py, rgba, opacity);
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

bool compositorLayerIsOverlay(const CompositorRenderPlanLayer& layer) {
  return layer.kind == "overlay" || layerTextContains(layer, "overlay");
}

bool compositorLayerIsLowerThird(const CompositorRenderPlanLayer& layer) {
  return compositorLayerIsOverlay(layer) &&
         (layerTextContains(layer, "lower-third") || layerTextContains(layer, "lower_third") || layerTextContains(layer, "lowerthird"));
}

bool compositorLayerIsChromaKey(const CompositorRenderPlanLayer& layer) {
  return layerTextContains(layer, "chroma-key") || layerTextContains(layer, "chromakey") || layerTextContains(layer, "green-screen");
}

float compositorLayerOpacity(const CompositorRenderPlanLayer& layer) {
  return std::clamp(layer.opacity, 0.f, 1.f);
}

CompositorRenderPlan sortCompositorRenderPlan(CompositorRenderPlan renderPlan) {
  std::stable_sort(
      renderPlan.layers.begin(),
      renderPlan.layers.end(),
      [](const CompositorRenderPlanLayer& left, const CompositorRenderPlanLayer& right) {
        const int leftDepth = semanticLayerDepth(left);
        const int rightDepth = semanticLayerDepth(right);
        if (leftDepth != rightDepth) {
          return leftDepth < rightDepth;
        }
        if (left.order != right.order) {
          return left.order < right.order;
        }
        if (left.layerId != right.layerId) {
          return left.layerId < right.layerId;
        }
        return left.sourceId < right.sourceId;
      });
  return renderPlan;
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
    const auto sortedPlan = sortCompositorRenderPlan(renderPlan);
    for (const auto& layer : sortedPlan.layers) {
      if (compositorLayerIsChromaKey(layer) && compositorLayerOpacity(layer) <= 0.f) {
        continue;
      }
      auto rect = layer.rect;
      const VideoFrame* frameForLayer = nullptr;
      if (rect.width <= 0.f || rect.height <= 0.f) {
        if (compositorLayerIsOverlay(layer)) {
          const auto layout = compositorLayerIsLowerThird(layer) ? compositor::lowerThirdOverlay() : compositor::topRightOverlay();
          rect = {layout.x, layout.y, layout.width, layout.height};
        } else {
          const int videoLayerCount = static_cast<int>(std::count_if(
              sortedPlan.layers.begin(),
              sortedPlan.layers.end(),
              [](const CompositorRenderPlanLayer& entry) { return !compositorLayerIsOverlay(entry); }));
          const auto layout = compositor::gridCell((std::max)(1, videoLayerCount), videoIndex);
          rect = {layout.x, layout.y, layout.width, layout.height};
          ++videoIndex;
        }
      }

      uint32_t color = 0xff2a3548;
      if (!compositorLayerIsOverlay(layer)) {
        if (!layer.participantId.empty()) {
          color = compositor::colorFromParticipantId(layer.participantId);
          frameForLayer = findFrameForParticipant(frames, layer.participantId);
        } else if (videoIndex > 0 && videoIndex - 1 < static_cast<int>(frames.size())) {
          frameForLayer = &frames[static_cast<size_t>(videoIndex - 1)];
          color = compositor::colorFromParticipantId(frameForLayer->participantId);
        }
      }
      if (frameForLayer && blitVideoFrameIntoPreviewRect(preview, *frameForLayer, rect, compositorLayerOpacity(layer))) {
        continue;
      }
      fillRectBgra(preview.bgra, previewWidth, previewHeight, rect.x, rect.y, rect.width, rect.height, unpackColor(color), compositorLayerOpacity(layer));
    }
    return;
  }

  const int count = static_cast<int>(frames.size());
  for (int index = 0; index < count; ++index) {
    const auto layout = compositor::gridCell((std::max)(1, count), index);
    const CompositorLayerRect rect{layout.x, layout.y, layout.width, layout.height};
    const auto& videoFrame = frames[static_cast<size_t>(index)];
    if (blitVideoFrameIntoPreviewRect(preview, videoFrame, rect, 1.f)) {
      continue;
    }
    const auto color = compositor::colorFromParticipantId(videoFrame.participantId);
    fillRectBgra(preview.bgra, previewWidth, previewHeight, rect.x, rect.y, rect.width, rect.height, unpackColor(color), 1.f);
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

std::vector<uint8_t> base64Decode(const std::string& encoded) {
  auto sextet = [](char ch) -> int {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
  };

  std::vector<uint8_t> decoded;
  decoded.reserve((encoded.size() / 4) * 3);
  uint32_t buffer = 0;
  int bits = 0;
  for (const char ch : encoded) {
    if (ch == '=') {
      break;
    }
    const int value = sextet(ch);
    if (value < 0) {
      continue;  // Skip whitespace/newlines and any stray characters.
    }
    buffer = (buffer << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      decoded.push_back(static_cast<uint8_t>((buffer >> bits) & 0xff));
    }
  }
  return decoded;
}

bool blitVideoFrameIntoPreviewRect(
    ProgramFramePreviewPixels& preview,
    const VideoFrame& frame,
    const CompositorLayerRect& rect,
    float opacity) {
  if (!frame.hasPixels() || preview.width <= 0 || preview.height <= 0 || preview.bgra.empty() || opacity <= 0.f) {
    return false;
  }

  const int previewWidth = preview.width;
  const int previewHeight = preview.height;
  const int left = std::max(0, static_cast<int>(std::floor(rect.x * static_cast<float>(previewWidth))));
  const int top = std::max(0, static_cast<int>(std::floor(rect.y * static_cast<float>(previewHeight))));
  const int right = std::min(previewWidth, static_cast<int>(std::ceil((rect.x + rect.width) * static_cast<float>(previewWidth))));
  const int bottom = std::min(previewHeight, static_cast<int>(std::ceil((rect.y + rect.height) * static_cast<float>(previewHeight))));
  if (right <= left || bottom <= top) {
    return false;
  }

  const auto* source = frame.pixels->data();
  const int sourceWidth = frame.pixelWidth;
  const int sourceHeight = frame.pixelHeight;
  const int sourceStride = frame.pixelStride;
  const int rectWidth = right - left;
  const int rectHeight = bottom - top;
  const float clampedOpacity = std::clamp(opacity, 0.f, 1.f);

  for (int py = top; py < bottom; ++py) {
    const int sampleY = std::min(sourceHeight - 1, ((py - top) * sourceHeight) / rectHeight);
    const auto* sourceRow = source + static_cast<size_t>(sampleY) * static_cast<size_t>(sourceStride);
    for (int px = left; px < right; ++px) {
      const int sampleX = std::min(sourceWidth - 1, ((px - left) * sourceWidth) / rectWidth);
      const auto* pixel = sourceRow + static_cast<size_t>(sampleX) * 4u;
      const uint32_t bgra = (static_cast<uint32_t>(pixel[3]) << 24) |
                            (static_cast<uint32_t>(pixel[2]) << 16) |
                            (static_cast<uint32_t>(pixel[1]) << 8) |
                            static_cast<uint32_t>(pixel[0]);
      blendPixelBgra(preview.bgra, previewWidth, px, py, bgra, clampedOpacity);
    }
  }
  return true;
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
      {"layerCount", frame.layerCount},
      {"programPixelSignature", static_cast<double>(frame.programPixelSignature)},
      {"renderPlanSignature", static_cast<double>(frame.renderPlanSignature)},
      {"pixelFormat", "bgra"},
      {"bgraBase64", base64Encode(frame.preview.bgra.data(), frame.preview.bgra.size())},
  };
  if (!frame.warnings.empty()) {
    rpc::Json::Array warnings;
    for (const auto& warning : frame.warnings) {
      warnings.emplace_back(warning);
    }
    preview.emplace("warnings", warnings);
  }
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
