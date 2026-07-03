#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

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

// Insets a rect uniformly by `pad` (normalized) on every side, clamped so the
// rect never inverts. Used to leave a thin gutter between multiview cells.
inline LayerRect insetRect(const LayerRect& rect, float pad) {
  const float maxPad = 0.4f * std::min(rect.width, rect.height);
  const float p = std::clamp(pad, 0.f, maxPad);
  return {rect.x + p, rect.y + p, rect.width - 2.f * p, rect.height - 2.f * p};
}

// Returns the largest `aspect` (default 16:9) rect that fits inside `cell`,
// centered, computed in PIXELS against a `canvasWidth`x`canvasHeight` canvas and
// mapped back to normalized canvas coordinates. This is the fix for the multiview
// "center-crop" bug: an even rows x cols grid yields non-16:9 cells, so instead of
// center-cropping a 16:9 feed to fill a non-16:9 cell we place a centered 16:9
// tile inside the cell and letterbox off-aspect feeds within it.
inline LayerRect centeredAspectRect(
    const LayerRect& cell,
    float canvasWidth,
    float canvasHeight,
    float aspect = 16.f / 9.f) {
  const float cw = canvasWidth > 0.f ? canvasWidth : 1920.f;
  const float ch = canvasHeight > 0.f ? canvasHeight : 1080.f;
  const float cellPxW = cell.width * cw;
  const float cellPxH = cell.height * ch;
  if (cellPxW <= 0.f || cellPxH <= 0.f || aspect <= 0.f) {
    return cell;
  }
  float tilePxW = cellPxW;
  float tilePxH = cellPxW / aspect;
  if (tilePxH > cellPxH) {
    tilePxH = cellPxH;
    tilePxW = cellPxH * aspect;
  }
  const float tileW = tilePxW / cw;
  const float tileH = tilePxH / ch;
  return {
      cell.x + (cell.width - tileW) * 0.5f,
      cell.y + (cell.height - tileH) * 0.5f,
      tileW,
      tileH};
}

// The raw (pre-aspect) cell regions for one multiview layout mode. Every consumer
// (the render-plan layer placement and the emitted tile rects) shares this so the
// core and the WinUI overlay agree on geometry. Cells here are already inset by a
// small gutter; apply centeredAspectRect() to each to get the final 16:9 tile.
struct MultiviewLayoutPlan {
  bool hasProgramPreview = false;
  LayerRect programCell;               // Valid only when hasProgramPreview.
  LayerRect previewCell;               // Valid only when hasProgramPreview.
  std::vector<LayerRect> sourceCells;  // One per source tile, in slot order.
};

// Computes the raw cell layout for `sourceCount` source tiles in `mode`.
//   "grid"        -> aspect-aware even grid (legacy behavior).
//   "pgmPvwTop"   -> PVW|PGM big on top half (Preview left, Program right);
//                    sources wrap 1-2 rows below (2 rows above 5 tiles).
//   "pgmPvwLarge" -> same top-half geometry as pgmPvwTop (kept for compat).
//   "pgmPvwSide"  -> PGM over PVW on the left ~2/3; sources in a right strip.
// Unknown modes fall back to "grid".
inline MultiviewLayoutPlan computeMultiviewLayout(
    const std::string& mode,
    int sourceCount,
    float pad = 0.006f) {
  MultiviewLayoutPlan plan;
  const int count = std::max(0, sourceCount);

  auto pushCell = [&](const LayerRect& raw) { plan.sourceCells.push_back(insetRect(raw, pad)); };

  if (mode == "pgmPvwTop" || mode == "pgmPvwLarge" || mode == "pgmPvwSide") {
    plan.hasProgramPreview = true;
  }

  if (mode == "pgmPvwSide") {
    const float leftW = 2.f / 3.f;
    plan.programCell = insetRect({0.f, 0.f, leftW, 0.5f}, pad);
    plan.previewCell = insetRect({0.f, 0.5f, leftW, 0.5f}, pad);
    const float rightX = leftW;
    const float rightW = 1.f - leftW;
    const int n = std::max(1, count);
    const float cellH = 1.f / static_cast<float>(n);
    for (int i = 0; i < count; ++i) {
      pushCell({rightX, static_cast<float>(i) * cellH, rightW, cellH});
    }
    return plan;
  }

  if (mode == "pgmPvwTop" || mode == "pgmPvwLarge") {
    // Broadcast convention (ATEM/Riedel/R&S): PREVIEW on the left, PROGRAM on the right.
    plan.previewCell = insetRect({0.f, 0.f, 0.5f, 0.5f}, pad);
    plan.programCell = insetRect({0.5f, 0.f, 0.5f, 0.5f}, pad);
    const float regionY = 0.5f;
    const float regionH = 0.5f;
    // Wrap to two source rows above 5 tiles so a full 10-input wall stays readable.
    const int rows = (count > 5) ? 2 : 1;
    const int cols = std::max(1, (std::max(1, count) + rows - 1) / rows);
    const float cellW = 1.f / static_cast<float>(cols);
    const float cellH = regionH / static_cast<float>(rows);
    for (int i = 0; i < count; ++i) {
      const int row = i / cols;
      const int col = i % cols;
      pushCell({static_cast<float>(col) * cellW,
                regionY + static_cast<float>(row) * cellH,
                cellW,
                cellH});
    }
    return plan;
  }

  // "grid" (and any unknown mode): aspect-aware even grid.
  for (int i = 0; i < count; ++i) {
    plan.sourceCells.push_back(gridCell(std::max(1, count), i));
  }
  return plan;
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
