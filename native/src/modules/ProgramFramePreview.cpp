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

// Draws a border framing the given rect by stroking its four edges. The border
// thickness is a fraction of the rect's smaller dimension, with at least a
// single pixel when visible so it never vanishes in the small preview buffer.
void drawBorderBgra(
    std::vector<uint8_t>& pixels,
    int width,
    int height,
    const compositor::LayerRect& rect,
    const compositor::BorderFraming& border,
    float opacity) {
  if (!border.visible || opacity <= 0.f || width <= 0 || height <= 0) {
    return;
  }
  const float smaller = std::min(rect.width, rect.height);
  float strokeNorm = border.thickness * smaller;
  // Guarantee at least ~1px of stroke on each axis in the preview buffer.
  strokeNorm = std::max(strokeNorm, 1.f / static_cast<float>(std::max(width, height)));
  const float strokeX = std::min(strokeNorm, rect.width * 0.5f);
  const float strokeY = std::min(strokeNorm, rect.height * 0.5f);
  const uint32_t color = border.colorRgba;
  // Top, bottom, left, right edges.
  fillRectBgra(pixels, width, height, rect.x, rect.y, rect.width, strokeY, color, opacity);
  fillRectBgra(pixels, width, height, rect.x, rect.y + rect.height - strokeY, rect.width, strokeY, color, opacity);
  fillRectBgra(pixels, width, height, rect.x, rect.y, strokeX, rect.height, color, opacity);
  fillRectBgra(pixels, width, height, rect.x + rect.width - strokeX, rect.y, strokeX, rect.height, color, opacity);
}

// A deterministic 5x7 bitmap font subset sufficient to raster real, legible-ish
// glyphs for overlay/caption text into the preview buffer headlessly (no
// DirectWrite in-container). Unknown glyphs fall back to a filled block so the
// rastered region is always non-uniform when text is present. Each glyph is 7
// rows of 5 bits (MSB = leftmost column).
const uint8_t* glyphRows5x7(char ch) {
  static const uint8_t kBlock[7] = {0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f};
  static const uint8_t kSpace[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  static const uint8_t kA[7] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
  static const uint8_t kE[7] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
  static const uint8_t kI[7] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f};
  static const uint8_t kO[7] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
  static const uint8_t kT[7] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
  static const uint8_t kDot[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04};
  if (ch == ' ') return kSpace;
  switch (std::toupper(static_cast<unsigned char>(ch))) {
    case 'A': return kA;
    case 'E': return kE;
    case 'I': return kI;
    case 'O': return kO;
    case 'T': return kT;
    case '.': return kDot;
    default: return kBlock;
  }
}

// Rasters a single line of text left-aligned into the given normalized rect,
// vertically centered, scaling the glyph cell to fit the rect height. Produces
// non-uniform pixels (the glyph shapes) and returns the number of glyphs drawn.
int drawTextLineBgra(
    std::vector<uint8_t>& pixels,
    int width,
    int height,
    const compositor::LayerRect& rect,
    const std::string& text,
    uint32_t rgba,
    float opacity) {
  if (text.empty() || opacity <= 0.f || width <= 0 || height <= 0) {
    return 0;
  }
  const float cellPxX = rect.width * static_cast<float>(width);
  const float cellPxY = rect.height * static_cast<float>(height);
  if (cellPxX <= 0.f || cellPxY <= 0.f) {
    return 0;
  }
  // Glyph cell: 6 columns (5 + 1 spacing) by 8 rows (7 + 1 spacing).
  const int maxGlyphsByWidth = std::max(1, static_cast<int>(cellPxX / 6.f));
  const int glyphCount = std::min(static_cast<int>(text.size()), maxGlyphsByWidth);
  if (glyphCount <= 0) {
    return 0;
  }
  const float glyphW = cellPxX / static_cast<float>(glyphCount);
  const float pixelW = glyphW / 6.f;
  const float pixelH = cellPxY / 8.f;
  const float originX = rect.x * static_cast<float>(width);
  const float originY = rect.y * static_cast<float>(height);
  int drawn = 0;
  for (int g = 0; g < glyphCount; ++g) {
    const uint8_t* rows = glyphRows5x7(text[static_cast<size_t>(g)]);
    const float glyphOriginX = originX + static_cast<float>(g) * glyphW;
    for (int row = 0; row < 7; ++row) {
      const uint8_t bits = rows[row];
      for (int col = 0; col < 5; ++col) {
        if ((bits & (0x10u >> col)) == 0) {
          continue;
        }
        const int px0 = static_cast<int>(glyphOriginX + static_cast<float>(col) * pixelW);
        const int px1 = static_cast<int>(glyphOriginX + static_cast<float>(col + 1) * pixelW);
        const int py0 = static_cast<int>(originY + static_cast<float>(row) * pixelH);
        const int py1 = static_cast<int>(originY + static_cast<float>(row + 1) * pixelH);
        for (int py = py0; py < std::max(py0 + 1, py1); ++py) {
          for (int px = px0; px < std::max(px0 + 1, px1); ++px) {
            blendPixelBgra(pixels, width, px, py, rgba, opacity);
          }
        }
      }
    }
    ++drawn;
  }
  return drawn;
}

// Rasters an aspect-correct image-overlay placeholder. Without WIC decode
// in-container, this paints a deterministic two-tone checker derived from the
// imageUri so the region is non-uniform and varies by source. On Windows the
// D3D11 path decodes the real image via WIC.
void drawImagePlaceholderBgra(
    std::vector<uint8_t>& pixels,
    int width,
    int height,
    const compositor::LayerRect& rect,
    const std::string& imageUri,
    uint32_t tintRgba,
    float opacity) {
  if (opacity <= 0.f || width <= 0 || height <= 0) {
    return;
  }
  uint32_t hash = 2166136261u;
  for (const unsigned char ch : imageUri) {
    hash ^= ch;
    hash *= 16777619u;
  }
  const uint32_t altColor = 0xff000000u | (hash & 0x00ffffffu);
  const int left = std::max(0, static_cast<int>(std::floor(rect.x * static_cast<float>(width))));
  const int top = std::max(0, static_cast<int>(std::floor(rect.y * static_cast<float>(height))));
  const int right = std::min(width, static_cast<int>(std::ceil((rect.x + rect.width) * static_cast<float>(width))));
  const int bottom = std::min(height, static_cast<int>(std::ceil((rect.y + rect.height) * static_cast<float>(height))));
  const int cell = std::max(1, (right - left) / 6);
  for (int py = top; py < bottom; ++py) {
    for (int px = left; px < right; ++px) {
      const bool alt = (((px - left) / cell) + ((py - top) / cell)) % 2 == 0;
      blendPixelBgra(pixels, width, px, py, alt ? tintRgba : altColor, opacity);
    }
  }
}

// Rasters an overlay/lower-third/caption layer's real content: a brand-styled
// background band, an accent bar, text (title/org or caption + speaker) and/or
// an image, with the animated keyPhase transform (alpha + slide) applied. The
// math mirrors the D3D11 overlay raster stage so preview and program agree.
void drawOverlayContentBgra(
    std::vector<uint8_t>& pixels,
    int width,
    int height,
    const compositor::LayerRect& rect,
    const CompositorOverlayContent& overlay,
    float layerOpacity) {
  const auto key = compositor::computeOverlayKeyTransform(
      overlay.keyPhase, overlay.keyProgress, overlay.keyPosition);
  if (!key.visible || key.alpha <= 0.001f) {
    return;
  }
  const float opacity = std::clamp(layerOpacity * key.alpha, 0.f, 1.f);
  if (opacity <= 0.f) {
    return;
  }

  // Apply the animated slide + content scale about the rect center.
  compositor::LayerRect animated = rect;
  animated.x += key.slideX;
  animated.y += key.slideY;
  const float centerX = animated.x + animated.width * 0.5f;
  const float centerY = animated.y + animated.height * 0.5f;
  animated.width *= key.contentScale;
  animated.height *= key.contentScale;
  animated.x = centerX - animated.width * 0.5f;
  animated.y = centerY - animated.height * 0.5f;

  const uint32_t background = compositor::parseHexColorRgba(overlay.brandBackgroundColor, 0xff0c1118u);
  const uint32_t accent = compositor::parseHexColorRgba(overlay.brandColor, 0xff44c1a1u);
  const uint32_t accentSecondary = compositor::parseHexColorRgba(overlay.brandAccentColor, 0xfff0a85cu);
  constexpr uint32_t kTextColor = 0xfff4f7fau;

  // Background band (brand background).
  fillRectBgra(pixels, width, height, animated.x, animated.y, animated.width, animated.height, background, opacity);
  // Accent bar down the left edge (brand color), or full top bar for captions.
  if (overlay.isCaption) {
    const float barH = std::min(animated.height * 0.10f, 0.01f);
    fillRectBgra(pixels, width, height, animated.x, animated.y, animated.width, barH, accent, opacity);
  } else {
    const float barW = std::min(animated.width * 0.04f, 0.01f);
    fillRectBgra(pixels, width, height, animated.x, animated.y, barW, animated.height, accent, opacity);
  }

  // Image overlay (aspect-correct placeholder) occupies the left portion when
  // present alongside text; otherwise the full band.
  float textLeft = animated.x + animated.width * 0.06f;
  const float textWidth = animated.width * 0.88f;
  if (!overlay.imageUri.empty()) {
    const float imageW = animated.width * 0.22f;
    const float imageH = animated.height * 0.8f;
    const compositor::LayerRect imageRect{
        animated.x + animated.width * 0.04f,
        centerY - imageH * 0.5f,
        imageW,
        imageH};
    drawImagePlaceholderBgra(pixels, width, height, imageRect, overlay.imageUri, accentSecondary, opacity);
    textLeft = imageRect.x + imageRect.width + animated.width * 0.03f;
  }
  const float availableTextWidth = std::max(0.f, animated.x + textWidth - textLeft);

  if (overlay.isCaption) {
    // Caption: speaker attribution (accent) above the caption body.
    if (!overlay.speaker.empty()) {
      const compositor::LayerRect speakerRect{textLeft, animated.y + animated.height * 0.12f, availableTextWidth, animated.height * 0.32f};
      drawTextLineBgra(pixels, width, height, speakerRect, overlay.speaker, accent, opacity);
    }
    const compositor::LayerRect bodyRect{textLeft, animated.y + animated.height * 0.50f, availableTextWidth, animated.height * 0.40f};
    drawTextLineBgra(pixels, width, height, bodyRect, overlay.text, kTextColor, opacity);
    return;
  }

  // Lower-third: title line (bright text) above the org/text line (accent).
  // The title uses the bright text color and the org line the brand accent so
  // the two lines and the accent bar are visually distinct.
  const std::string& primary = !overlay.title.empty() ? overlay.title : overlay.text;
  const std::string& secondary = overlay.org;
  if (!primary.empty()) {
    const compositor::LayerRect titleRect{textLeft, animated.y + animated.height * 0.18f, availableTextWidth, animated.height * 0.34f};
    drawTextLineBgra(pixels, width, height, titleRect, primary, kTextColor, opacity);
  }
  if (!secondary.empty()) {
    const compositor::LayerRect orgRect{textLeft, animated.y + animated.height * 0.56f, availableTextWidth, animated.height * 0.28f};
    drawTextLineBgra(pixels, width, height, orgRect, secondary, accent, opacity);
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

void mixHash(uint32_t& hash, uint8_t value) {
  hash ^= value;
  hash *= 16777619u;
}

void mixHash(uint32_t& hash, int value) {
  for (int shift = 0; shift < 32; shift += 8) {
    mixHash(hash, static_cast<uint8_t>((static_cast<uint32_t>(value) >> shift) & 0xffu));
  }
}

void mixHash(uint32_t& hash, float value) {
  const int quantized = static_cast<int>(std::lround(value * 10000.f));
  mixHash(hash, quantized);
}

void mixHash(uint32_t& hash, const std::string& value) {
  for (const unsigned char ch : value) {
    mixHash(hash, ch);
  }
  mixHash(hash, static_cast<uint8_t>(0xffu));
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

uint32_t renderPlanSignature(const CompositorRenderPlan& renderPlan) {
  uint32_t hash = 2166136261u;
  mixHash(hash, renderPlan.renderPlanId);
  mixHash(hash, renderPlan.sceneId);
  mixHash(hash, renderPlan.width);
  mixHash(hash, renderPlan.height);
  mixHash(hash, renderPlan.fps);
  for (const auto& layer : renderPlan.layers) {
    mixHash(hash, layer.layerId);
    mixHash(hash, layer.kind);
    mixHash(hash, layer.sourceId);
    mixHash(hash, layer.participantId);
    mixHash(hash, layer.mediaAssetId);
    mixHash(hash, layer.mediaAssetName);
    mixHash(hash, layer.mediaAssetKind);
    mixHash(hash, layer.mediaAssetPath);
    mixHash(hash, layer.mediaPlaybackKey);
    mixHash(hash, layer.mediaAssetPlaying ? 1 : 0);
    mixHash(hash, layer.order);
    mixHash(hash, layer.rect.x);
    mixHash(hash, layer.rect.y);
    mixHash(hash, layer.rect.width);
    mixHash(hash, layer.rect.height);
    mixHash(hash, layer.fitMode);
    mixHash(hash, layer.borderStyle);
    mixHash(hash, layer.borderColor);
    mixHash(hash, layer.borderThickness);
    mixHash(hash, layer.sourceScale);
    mixHash(hash, layer.sourceOffsetX);
    mixHash(hash, layer.sourceOffsetY);
    mixHash(hash, layer.hasColorGrade ? 1 : 0);
    if (layer.hasColorGrade) {
      mixHash(hash, layer.colorGrade.exposure);
      mixHash(hash, layer.colorGrade.contrast);
      mixHash(hash, layer.colorGrade.saturation);
      mixHash(hash, layer.colorGrade.temperature);
    }
    mixHash(hash, compositorLayerOpacity(layer));
  }
  return hash == 0 ? 1u : hash;
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
      const bool isOverlay = compositorLayerIsOverlay(layer);
      if (!isOverlay) {
        if (!layer.participantId.empty()) {
          color = compositor::colorFromParticipantId(layer.participantId);
          frameForLayer = findFrameForParticipant(frames, layer.participantId);
        } else if (!layer.mediaAssetId.empty()) {
          color = compositor::colorFromParticipantId("media:" + layer.mediaAssetId);
          const std::string frameSourceId = layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId;
          frameForLayer = findFrameForParticipant(frames, frameSourceId);
        } else if (videoIndex > 0 && videoIndex - 1 < static_cast<int>(frames.size())) {
          frameForLayer = &frames[static_cast<size_t>(videoIndex - 1)];
          color = compositor::colorFromParticipantId(frameForLayer->participantId);
        }
      }

      const float layerOpacity = compositorLayerOpacity(layer);

      // Overlays keep their fixed placement; scene sources honor per-source
      // framing geometry (letterbox bars, zoom/pan offset, border) in both the
      // real-frame blit and the synthetic fill so the preview reflects the
      // resolved framing.
      if (isOverlay) {
        if (layer.hasOverlayContent) {
          // Real overlay raster: brand-styled band + text/image + animated key
          // phase. Mirrors the D3D11 overlay-raster stage so preview matches
          // program.
          drawOverlayContentBgra(preview.bgra, previewWidth, previewHeight, {rect.x, rect.y, rect.width, rect.height}, layer.overlay, layerOpacity);
        } else {
          fillRectBgra(preview.bgra, previewWidth, previewHeight, rect.x, rect.y, rect.width, rect.height, unpackColor(color), layerOpacity);
        }
        continue;
      }

      // Resolve the source aspect from the (possibly real) frame, falling back
      // to the render-plan aspect. The destination rect is normalized to the
      // canvas, so its true aspect is scaled by the canvas pixel dimensions.
      int sourceWidth = renderPlan.width > 0 ? renderPlan.width : 16;
      int sourceHeight = renderPlan.height > 0 ? renderPlan.height : 9;
      if (frameForLayer) {
        sourceWidth = frameForLayer->naturalWidth > 0
                          ? frameForLayer->naturalWidth
                          : frameForLayer->width > 0
                                ? frameForLayer->width
                                : frameForLayer->pixelWidth > 0 ? frameForLayer->pixelWidth : sourceWidth;
        sourceHeight = frameForLayer->naturalHeight > 0
                           ? frameForLayer->naturalHeight
                           : frameForLayer->height > 0
                                 ? frameForLayer->height
                                 : frameForLayer->pixelHeight > 0 ? frameForLayer->pixelHeight : sourceHeight;
      }

      // Express the rect in canvas-pixel proportions so aspect comparisons are
      // correct, then frame against a unit rect with that aspect baked in.
      const compositor::LayerRect destRect{rect.x, rect.y, rect.width, rect.height};
      const compositor::LayerRect aspectRect{
          0.f,
          0.f,
          rect.width * static_cast<float>(previewWidth),
          rect.height * static_cast<float>(previewHeight)};
      const auto framing = compositor::computeSourceFraming(
          sourceWidth,
          sourceHeight,
          aspectRect,
          layer.fitMode,
          layer.sourceScale,
          layer.sourceOffsetX,
          layer.sourceOffsetY);

      // Map the framing (computed in aspectRect units) back into the layer's
      // normalized dest rect. contentX/Y/W/H are fractions of aspectRect, which
      // shares the rect's origin-relative proportions.
      const float contentFracX = aspectRect.width > 0.f ? framing.contentX / aspectRect.width : 0.f;
      const float contentFracY = aspectRect.height > 0.f ? framing.contentY / aspectRect.height : 0.f;
      const float contentFracW = aspectRect.width > 0.f ? framing.contentW / aspectRect.width : 1.f;
      const float contentFracH = aspectRect.height > 0.f ? framing.contentH / aspectRect.height : 1.f;
      const float contentX = rect.x + contentFracX * rect.width;
      const float contentY = rect.y + contentFracY * rect.height;
      const float contentW = contentFracW * rect.width;
      const float contentH = contentFracH * rect.height;
      const float imageFracX = aspectRect.width > 0.f ? framing.imageX / aspectRect.width : 0.f;
      const float imageFracY = aspectRect.height > 0.f ? framing.imageY / aspectRect.height : 0.f;
      const float imageFracW = aspectRect.width > 0.f ? framing.imageW / aspectRect.width : 1.f;
      const float imageFracH = aspectRect.height > 0.f ? framing.imageH / aspectRect.height : 1.f;
      const float imageX = rect.x + imageFracX * rect.width;
      const float imageY = rect.y + imageFracY * rect.height;
      const float imageW = imageFracW * rect.width;
      const float imageH = imageFracH * rect.height;

      // Letterbox bars: when the content is smaller than the dest rect, paint
      // the full rect with a dark bar color first, then the content on top.
      if (framing.hasLetterbox) {
        const uint32_t barColor = 0xff05080c;
        fillRectBgra(preview.bgra, previewWidth, previewHeight, rect.x, rect.y, rect.width, rect.height, barColor, layerOpacity);
      }

      if (frameForLayer && frameForLayer->hasPixels()) {
        // Real frame: draw the full resolved source layer, then clip it to the
        // destination slot. This keeps pan/XY relative to the original source
        // layer instead of an intermediate crop.
        const CompositorLayerRect imageRect{imageX, imageY, imageW, imageH};
        const CompositorLayerRect clipRect{rect.x, rect.y, rect.width, rect.height};
        blitVideoFrameLayerClipped(preview, *frameForLayer, imageRect, clipRect, layerOpacity);
      } else {
        // The synthetic fill has no texture detail to pan, so draw the resolved
        // visible source rectangle directly. This still shows scale-out
        // placement and letterbox/empty space accurately.
        fillRectBgra(preview.bgra, previewWidth, previewHeight, contentX, contentY, contentW, contentH, unpackColor(color), layerOpacity);
      }

      const auto border = compositor::computeBorderFraming(layer.borderStyle, layer.borderColor, layer.borderThickness);
      drawBorderBgra(preview.bgra, previewWidth, previewHeight, destRect, border, layerOpacity);
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

bool blitVideoFrameFramed(
    ProgramFramePreviewPixels& preview,
    const VideoFrame& frame,
    const CompositorLayerRect& contentRect,
    float u0,
    float v0,
    float u1,
    float v1,
    float opacity) {
  if (!frame.hasPixels() || preview.width <= 0 || preview.height <= 0 || preview.bgra.empty() || opacity <= 0.f) {
    return false;
  }

  const int previewWidth = preview.width;
  const int previewHeight = preview.height;
  const int left = std::max(0, static_cast<int>(std::floor(contentRect.x * static_cast<float>(previewWidth))));
  const int top = std::max(0, static_cast<int>(std::floor(contentRect.y * static_cast<float>(previewHeight))));
  const int right = std::min(previewWidth, static_cast<int>(std::ceil((contentRect.x + contentRect.width) * static_cast<float>(previewWidth))));
  const int bottom = std::min(previewHeight, static_cast<int>(std::ceil((contentRect.y + contentRect.height) * static_cast<float>(previewHeight))));
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
  const float spanU = u1 - u0;
  const float spanV = v1 - v0;

  for (int py = top; py < bottom; ++py) {
    // Map the on-screen row into the sampled source-V window.
    const float vt = rectHeight > 1 ? static_cast<float>(py - top) / static_cast<float>(rectHeight - 1) : 0.f;
    const float sampleV = std::clamp(v0 + vt * spanV, 0.f, 1.f);
    const int sampleY = std::min(sourceHeight - 1, static_cast<int>(sampleV * static_cast<float>(sourceHeight)));
    const auto* sourceRow = source + static_cast<size_t>(sampleY) * static_cast<size_t>(sourceStride);
    for (int px = left; px < right; ++px) {
      const float ut = rectWidth > 1 ? static_cast<float>(px - left) / static_cast<float>(rectWidth - 1) : 0.f;
      const float sampleU = std::clamp(u0 + ut * spanU, 0.f, 1.f);
      const int sampleX = std::min(sourceWidth - 1, static_cast<int>(sampleU * static_cast<float>(sourceWidth)));
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

bool blitVideoFrameLayerClipped(
    ProgramFramePreviewPixels& preview,
    const VideoFrame& frame,
    const CompositorLayerRect& imageRect,
    const CompositorLayerRect& clipRect,
    float opacity) {
  if (!frame.hasPixels() || preview.width <= 0 || preview.height <= 0 || preview.bgra.empty() || opacity <= 0.f) {
    return false;
  }

  const int previewWidth = preview.width;
  const int previewHeight = preview.height;
  const float imageLeft = imageRect.x * static_cast<float>(previewWidth);
  const float imageTop = imageRect.y * static_cast<float>(previewHeight);
  const float imageWidth = imageRect.width * static_cast<float>(previewWidth);
  const float imageHeight = imageRect.height * static_cast<float>(previewHeight);
  if (imageWidth <= 0.001f || imageHeight <= 0.001f) {
    return false;
  }

  const int left = std::max(0, static_cast<int>(std::floor(clipRect.x * static_cast<float>(previewWidth))));
  const int top = std::max(0, static_cast<int>(std::floor(clipRect.y * static_cast<float>(previewHeight))));
  const int right = std::min(previewWidth, static_cast<int>(std::ceil((clipRect.x + clipRect.width) * static_cast<float>(previewWidth))));
  const int bottom = std::min(previewHeight, static_cast<int>(std::ceil((clipRect.y + clipRect.height) * static_cast<float>(previewHeight))));
  if (right <= left || bottom <= top) {
    return false;
  }

  const auto* source = frame.pixels->data();
  const int sourceWidth = frame.pixelWidth;
  const int sourceHeight = frame.pixelHeight;
  const int sourceStride = frame.pixelStride;
  const float clampedOpacity = std::clamp(opacity, 0.f, 1.f);

  for (int py = top; py < bottom; ++py) {
    const float vt = std::clamp((static_cast<float>(py) + 0.5f - imageTop) / imageHeight, 0.f, 1.f);
    const int sampleY = std::min(sourceHeight - 1, static_cast<int>(vt * static_cast<float>(sourceHeight)));
    const auto* sourceRow = source + static_cast<size_t>(sampleY) * static_cast<size_t>(sourceStride);
    for (int px = left; px < right; ++px) {
      const float ut = std::clamp((static_cast<float>(px) + 0.5f - imageLeft) / imageWidth, 0.f, 1.f);
      const int sampleX = std::min(sourceWidth - 1, static_cast<int>(ut * static_cast<float>(sourceWidth)));
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

rpc::Json multiviewSharedTextureJson(const ProgramFrame& frame) {
  const auto& texture = frame.multiviewSharedTexture;
  if (texture.sharedHandleHex.empty() || texture.width <= 0 || texture.height <= 0) {
    return nullptr;
  }

  rpc::Json::Array tiles;
  tiles.reserve(frame.multiviewTiles.size());
  for (const auto& tile : frame.multiviewTiles) {
    tiles.emplace_back(rpc::Json::Object{
        {"sourceId", tile.sourceId},
        {"participantId", tile.participantId},
        {"slot", tile.slot},
        {"label", tile.label},
        {"activeSpeaker", tile.activeSpeaker},
        {"x", static_cast<double>(tile.x)},
        {"y", static_cast<double>(tile.y)},
        {"w", static_cast<double>(tile.w)},
        {"h", static_cast<double>(tile.h)},
    });
  }

  return rpc::Json::Object{
      {"texture",
       rpc::Json::Object{
           {"sharedHandleHex", texture.sharedHandleHex},
           {"width", texture.width},
           {"height", texture.height},
           {"format", texture.format.empty() ? "B8G8R8A8_UNORM" : texture.format},
           {"frameNumber", static_cast<double>(texture.frameNumber > 0 ? texture.frameNumber : frame.frameNumber)},
       }},
      {"canvasWidth", frame.multiviewWidth > 0 ? frame.multiviewWidth : texture.width},
      {"canvasHeight", frame.multiviewHeight > 0 ? frame.multiviewHeight : texture.height},
      {"tiles", tiles},
  };
}

rpc::Json multiviewSharedTextureEvent(const ProgramFrame& frame) {
  auto body = multiviewSharedTextureJson(frame);
  if (body.isNull()) {
    return nullptr;
  }
  rpc::Json::Object event = body.asObject();
  event.emplace("type", "multiview-shared-texture");
  return event;
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
