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
//   sourceScale: zoom. 1.0 = none; >1 samples a smaller centered region
//                (clamped to >= a tiny epsilon so the region never collapses).
//   sourceOffsetX/Y: pan in normalized units; shifts the sampled region and is
//                clamped so the region stays inside the source bounds.
inline SourceFraming computeSourceFraming(
    int sourceWidth,
    int sourceHeight,
    const LayerRect& dest,
    const std::string& fitMode,
    float sourceScale,
    float sourceOffsetX,
    float sourceOffsetY) {
  SourceFraming framing;
  // The whole destination rect is the default content area.
  framing.contentX = dest.x;
  framing.contentY = dest.y;
  framing.contentW = dest.width;
  framing.contentH = dest.height;

  // --- Aspect-driven sampling window in UV before zoom/pan. ---
  // For "stretch" the window is the full source. For "fill"/"cover" we crop the
  // over-long axis. For "fit"/"contain" the source is shown whole and the
  // content rect (not the UV window) shrinks.
  float uvW = 1.f;
  float uvH = 1.f;
  const bool validAspect = sourceWidth > 0 && sourceHeight > 0 && dest.width > 0.f && dest.height > 0.f;

  if (validAspect && fitMode != "stretch") {
    const float sourceAspect = static_cast<float>(sourceWidth) / static_cast<float>(sourceHeight);
    const float destAspect = dest.width / dest.height;
    // Aspects within a small relative tolerance are treated as matched so
    // float rounding (e.g. 1.6/0.9 vs 1920/1080) never produces hairline bars.
    const float ratio = sourceAspect / destAspect;
    const bool wider = ratio > 1.f + 1e-3f;
    const bool taller = ratio < 1.f - 1e-3f;
    if (fitMode == "fit" || fitMode == "contain") {
      // Letterbox: source shown whole (UV window stays full), content shrinks.
      if (wider) {
        // Source wider than dest -> bars top/bottom.
        framing.hasLetterbox = true;
        const float contentH = dest.height * (destAspect / sourceAspect);
        framing.contentY = dest.y + (dest.height - contentH) * 0.5f;
        framing.contentH = contentH;
      } else if (taller) {
        // Source taller than dest -> bars left/right.
        framing.hasLetterbox = true;
        const float contentW = dest.width * (sourceAspect / destAspect);
        framing.contentX = dest.x + (dest.width - contentW) * 0.5f;
        framing.contentW = contentW;
      }
    } else {
      // "fill"/"cover": crop the over-long axis of the source.
      if (wider) {
        // Source wider -> sample a narrower horizontal slice.
        uvW = destAspect / sourceAspect;
      } else if (taller) {
        // Source taller -> sample a shorter vertical slice.
        uvH = sourceAspect / destAspect;
      }
    }
  }

  // --- Zoom (sourceScale). >1 samples a smaller centered region. ---
  const float safeScale = sourceScale > 1e-4f ? sourceScale : 1e-4f;
  uvW /= safeScale;
  uvH /= safeScale;

  // Center the sampling window, then apply pan and clamp to source bounds.
  float u0 = (1.f - uvW) * 0.5f + sourceOffsetX * (1.f - uvW);
  float v0 = (1.f - uvH) * 0.5f + sourceOffsetY * (1.f - uvH);
  // Clamp so the window stays fully inside [0,1].
  const float maxU0 = 1.f - uvW;
  const float maxV0 = 1.f - uvH;
  u0 = std::clamp(u0, 0.f, maxU0 > 0.f ? maxU0 : 0.f);
  v0 = std::clamp(v0, 0.f, maxV0 > 0.f ? maxV0 : 0.f);

  framing.u0 = u0;
  framing.v0 = v0;
  framing.u1 = u0 + uvW;
  framing.v1 = v0 + uvH;
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