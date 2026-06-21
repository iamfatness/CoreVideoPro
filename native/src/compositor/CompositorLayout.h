#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace corevideo::compositor {

struct LayerRect {
  float x = 0.f;
  float y = 0.f;
  float width = 1.f;
  float height = 1.f;
};

// Result of resolving a layer's framing. The sampled source region is in
// normalized source UV space [0,1]; (u0,v0) is the top-left and (u1,v1) the
// bottom-right of the rectangle of the source texture that ends up on screen.
// The content rectangle is in the same normalized space as the destination
// rect (the on-screen region the source actually covers); for letterboxed
// ("fit"/"contain") layers it is smaller than the destination and centered,
// leaving bars. For "fill"/"cover"/"stretch" the content rectangle equals the
// destination rect (the whole rect is covered) and hasLetterbox is false.
struct SourceFraming {
  float u0 = 0.f;
  float v0 = 0.f;
  float u1 = 1.f;
  float v1 = 1.f;
  float imageX = 0.f;
  float imageY = 0.f;
  float imageW = 1.f;
  float imageH = 1.f;
  float contentX = 0.f;
  float contentY = 0.f;
  float contentW = 1.f;
  float contentH = 1.f;
  bool hasLetterbox = false;
};

// Resolved border geometry for a layer. thickness is expressed as a fraction
// of the destination rect's smaller dimension and is only meaningful when
// visible is true.
struct BorderFraming {
  bool visible = false;
  float thickness = 0.f;
  uint32_t colorRgba = 0xff000000u;
};

inline int gridColumns(int layerCount) {
  if (layerCount <= 1) {
    return 1;
  }
  if (layerCount <= 4) {
    return 2;
  }
  if (layerCount <= 6) {
    return 3;
  }
  return 4;
}

inline LayerRect gridCell(int layerCount, int index, float padding = 0.01f) {
  const int cols = gridColumns(layerCount);
  const int rows = (layerCount + cols - 1) / cols;
  const int col = index % cols;
  const int row = index / cols;
  const float cellW = 1.f / static_cast<float>(cols);
  const float cellH = 1.f / static_cast<float>(rows);
  return {
      col * cellW + padding,
      row * cellH + padding,
      cellW - 2.f * padding,
      cellH - 2.f * padding,
  };
}

inline LayerRect lowerThirdOverlay() {
  return {0.05f, 0.78f, 0.9f, 0.16f};
}

inline LayerRect topRightOverlay() {
  return {0.78f, 0.04f, 0.18f, 0.12f};
}

// Parses a "#RRGGBB" or "#RRGGBBAA" hex color into a packed 0xAARRGGBB value.
// Anything unparsable falls back to fully-opaque `fallback` (also 0xAARRGGBB).
inline uint32_t parseHexColorRgba(const std::string& value, uint32_t fallback = 0xff44c1a1u) {
  auto hexDigit = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };

  std::string digits;
  digits.reserve(8);
  for (const char ch : value) {
    if (ch == '#' || ch == ' ') {
      continue;
    }
    if (hexDigit(ch) < 0) {
      return fallback;
    }
    digits.push_back(ch);
  }
  if (digits.size() != 6 && digits.size() != 8) {
    return fallback;
  }

  uint32_t parsed = 0;
  for (const char ch : digits) {
    parsed = (parsed << 4) | static_cast<uint32_t>(hexDigit(ch));
  }
  if (digits.size() == 6) {
    return 0xff000000u | parsed;  // #RRGGBB -> opaque ARGB.
  }
  // #RRGGBBAA -> reorder trailing alpha into the high byte (ARGB).
  const uint32_t alpha = parsed & 0xffu;
  const uint32_t rgb = (parsed >> 8) & 0xffffffu;
  return (alpha << 24) | rgb;
}

// Computes the source sampling rectangle (normalized source UV) and the
// on-screen content rectangle for a layer, given the natural source size, the
// destination rect, and the framing parameters.
//
//   fitMode:  "fill"/"cover"  -> crop the over-long axis so the source covers
//                                the whole destination (no bars).
//             "fit"/"contain" -> letterbox: shrink the content so the whole
//                                source is visible, centered, with bars.
//             "stretch"       -> map the full source to the full destination,
//                                ignoring aspect ratio.
//   sourceScale: source size. 1.0 = natural fit/fill/stretch size; >1 grows the
//                rendered source and clips/pans inside the destination; <1
//                shrinks the rendered source so it can be positioned inside the
//                destination instead of becoming an invalid oversized UV crop.
//   sourceOffsetX/Y: normalized source position. For oversized content it pans
//                the rendered source behind the clipped destination; for
//                undersized content it positions the rendered source inside the
//                destination.
inline SourceFraming computeSourceFraming(
    int sourceWidth,
    int sourceHeight,
    const LayerRect& dest,
    const std::string& fitMode,
    float sourceScale,
    float sourceOffsetX,
    float sourceOffsetY) {
  SourceFraming framing;
  const bool validAspect = sourceWidth > 0 && sourceHeight > 0 && dest.width > 0.f && dest.height > 0.f;
  if (!validAspect) {
    framing.imageX = dest.x;
    framing.imageY = dest.y;
    framing.imageW = dest.width;
    framing.imageH = dest.height;
    framing.contentX = dest.x;
    framing.contentY = dest.y;
    framing.contentW = dest.width;
    framing.contentH = dest.height;
    return framing;
  }

  const float sourceAspect = static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
  const float destAspect = dest.width / dest.height;
  const float ratio = sourceAspect / destAspect;
  const bool wider = ratio > 1.f + 1e-3f;
  const bool taller = ratio < 1.f - 1e-3f;

  float baseW = dest.width;
  float baseH = dest.height;
  if (fitMode != "stretch") {
    if (fitMode == "fit" || fitMode == "contain") {
      if (wider) {
        baseH = dest.width / sourceAspect;
      } else if (taller) {
        baseW = dest.height * sourceAspect;
      }
    } else {
      if (wider) {
        baseW = dest.height * sourceAspect;
      } else if (taller) {
        baseH = dest.width / sourceAspect;
      }
    }
  }

  const float safeScale = std::clamp(sourceScale, 0.25f, 4.f);
  const float renderW = baseW * safeScale;
  const float renderH = baseH * safeScale;
  const float offsetX = std::clamp(sourceOffsetX, -1.f, 1.f);
  const float offsetY = std::clamp(sourceOffsetY, -1.f, 1.f);
  const float travelX = std::abs(renderW - dest.width);
  const float travelY = std::abs(renderH - dest.height);
  const float imageX =
      dest.x + (dest.width - renderW) * 0.5f + (renderW >= dest.width ? -offsetX : offsetX) * travelX * 0.5f;
  const float imageY =
      dest.y + (dest.height - renderH) * 0.5f + (renderH >= dest.height ? -offsetY : offsetY) * travelY * 0.5f;
  framing.imageX = imageX;
  framing.imageY = imageY;
  framing.imageW = renderW;
  framing.imageH = renderH;

  const float visibleX0 = std::max(dest.x, imageX);
  const float visibleY0 = std::max(dest.y, imageY);
  const float visibleX1 = std::min(dest.x + dest.width, imageX + renderW);
  const float visibleY1 = std::min(dest.y + dest.height, imageY + renderH);
  framing.contentX = visibleX0;
  framing.contentY = visibleY0;
  framing.contentW = std::max(0.f, visibleX1 - visibleX0);
  framing.contentH = std::max(0.f, visibleY1 - visibleY0);
  framing.hasLetterbox =
      framing.contentX > dest.x + 1e-4f ||
      framing.contentY > dest.y + 1e-4f ||
      framing.contentX + framing.contentW < dest.x + dest.width - 1e-4f ||
      framing.contentY + framing.contentH < dest.y + dest.height - 1e-4f;

  if (renderW > 1e-4f && renderH > 1e-4f && framing.contentW > 0.f && framing.contentH > 0.f) {
    framing.u0 = std::clamp((visibleX0 - imageX) / renderW, 0.f, 1.f);
    framing.v0 = std::clamp((visibleY0 - imageY) / renderH, 0.f, 1.f);
    framing.u1 = std::clamp((visibleX1 - imageX) / renderW, 0.f, 1.f);
    framing.v1 = std::clamp((visibleY1 - imageY) / renderH, 0.f, 1.f);
  }
  return framing;
}

// Resolves the border geometry/visibility for a layer. The border is invisible
// when the style is "none" or the thickness is non-positive. Colors follow the
// scene-routing palette: "accent" -> studio green, "program" -> amber,
// "warning" -> red, "solid" -> the layer's explicit color, "none" -> hidden.
// thicknessPx is the layer's raw thickness (UI pixels, clamped 0..12); it is
// normalized into a fraction of a nominal 200px tile edge for the preview.
inline BorderFraming computeBorderFraming(
    const std::string& borderStyle,
    const std::string& borderColor,
    float thicknessPx) {
  BorderFraming border;
  if (borderStyle == "none" || thicknessPx <= 0.f) {
    return border;
  }

  border.visible = true;
  // Normalize the clamped 0..12 px UI thickness against a 200px nominal edge.
  border.thickness = std::clamp(thicknessPx, 0.f, 12.f) / 200.f;
  if (borderStyle == "program") {
    border.colorRgba = 0xfff5a623u;  // Amber.
  } else if (borderStyle == "warning") {
    border.colorRgba = 0xffe5484du;  // Red.
  } else if (borderStyle == "accent") {
    border.colorRgba = 0xff3ddc97u;  // Studio accent green.
  } else {
    // "solid" (and any other) uses the explicit color.
    border.colorRgba = parseHexColorRgba(borderColor);
  }
  return border;
}

// Resolved animated overlay keying state. alpha multiplies the layer opacity;
// (slideX, slideY) is an additional normalized translation applied to the
// overlay content (a slide-in/out), and contentScale scales the content about
// its center (a subtle pop). All values are deterministic functions of phase +
// progress so the D3D11 and CPU paths animate identically.
struct OverlayKeyTransform {
  float alpha = 1.f;
  float slideX = 0.f;
  float slideY = 0.f;
  float contentScale = 1.f;
  bool visible = true;
};

// Smoothstep easing for animated keying (3t^2 - 2t^3), clamped to [0,1].
inline float overlayEase(float t) {
  t = std::clamp(t, 0.f, 1.f);
  return t * t * (3.f - 2.f * t);
}

// Resolves the animated keying transform for an overlay layer from its phase
// and normalized progress. "building-in" slides up + fades in, "on-air" is
// fully settled, "building-out" slides down + fades out, "hidden" is invisible.
// keyPosition controls the slide direction's anchor (lower vs upper third).
inline OverlayKeyTransform computeOverlayKeyTransform(
    const std::string& keyPhase,
    float keyProgress,
    const std::string& keyPosition = "lower-left") {
  OverlayKeyTransform transform;
  const float progress = std::clamp(keyProgress, 0.f, 1.f);
  // Lower-third slides up from below; upper third slides down from above.
  const float slideSign = keyPosition == "upper-left" ? -1.f : 1.f;
  const float slideTravel = 0.08f;  // Normalized slide distance.

  if (keyPhase == "hidden") {
    transform.visible = false;
    transform.alpha = 0.f;
    transform.slideY = slideSign * slideTravel;
    transform.contentScale = 0.96f;
    return transform;
  }
  if (keyPhase == "building-in") {
    const float eased = overlayEase(progress);
    transform.alpha = eased;
    transform.slideY = slideSign * slideTravel * (1.f - eased);
    transform.contentScale = 0.96f + 0.04f * eased;
    return transform;
  }
  if (keyPhase == "building-out") {
    const float eased = overlayEase(progress);
    transform.alpha = 1.f - eased;
    transform.slideY = slideSign * slideTravel * eased;
    transform.contentScale = 1.f - 0.04f * eased;
    if (transform.alpha <= 0.001f) {
      transform.visible = false;
    }
    return transform;
  }
  // "on-air" (and any unknown phase): fully settled.
  transform.alpha = 1.f;
  transform.slideY = 0.f;
  transform.contentScale = 1.f;
  return transform;
}

inline uint32_t colorFromParticipantId(const std::string& participantId) {
  uint32_t hash = 2166136261u;
  for (const unsigned char ch : participantId) {
    hash ^= ch;
    hash *= 16777619u;
  }
  const uint8_t r = static_cast<uint8_t>(72 + (hash & 0x7fu));
  const uint8_t g = static_cast<uint8_t>(72 + ((hash >> 8) & 0x7fu));
  const uint8_t b = static_cast<uint8_t>(72 + ((hash >> 16) & 0x7fu));
  return 0xff000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

}  // namespace corevideo::compositor
