#pragma once

// Per-tile name line for a Tiles wall (#1073): one line of text, the member's
// display name, inside each tile rect, in the scene's lower-third style.
//
// This header is PURE GEOMETRY. It decides where the band sits inside a tile,
// how large its text is, and whether the tile is big enough to carry a
// readable line at all. The pixels come from the SAME raster the scene lower
// third uses: MediaCore emits one ordinary overlay layer per named tile
// (kind "overlay", hasOverlayContent, appearance enabled) and the compositor's
// CompositorOverlayRaster / OverlayTileRaster draw it exactly as they draw the
// scene key. There is deliberately no second text renderer.
//
// Reference space: the scene lower-third appearance sizes its text in pixels
// at 1080 lines (MediaCore converts `nameSize + titleSize + padding*3` to a
// canvas fraction by dividing by 1080), so every px number below is in that
// same 1080-line reference space and the result scales with the output
// resolution through the overlay rect, like the scene key does.

#include "compositor/CompositorLayout.h"
#include "compositor/LowerThirdAppearance.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace corevideo::compositor {

// The 1080-line reference height the appearance numbers are authored against.
constexpr double kTilesNameReferenceHeightPx = 1080.0;
// A name shrinks with its tile until it reaches this font height, then holds.
constexpr double kTilesNameMinimumFontPx = 18.0;
// Suppression: the band may take at most this share of the tile's height
// (a 3x3 wall at 1080p is 360 px tall, a 7x7 one 154 px, an 8x8 one 135 px:
// with the default style the first two carry a name, the last does not).
constexpr double kTilesNameMaxBandShareOfTile = 0.30;
// Suppression: the tile must be at least this many font heights wide, or the
// line would be an ellipsis and nothing else.
constexpr double kTilesNameMinimumTileWidthInFonts = 6.0;

struct TilesNameLinePlacement {
  bool shown = false;
  LayerRect rect{0.f, 0.f, 0.f, 0.f};  // canvas fraction, like every plan layer
  LowerThirdAppearance appearance;      // the per-tile scaled copy of the style
};

// The style the names use when the scene has no lower-third appearance of its
// own: the struct's own defaults, switched on.
inline LowerThirdAppearance tilesNameDefaultAppearance() {
  LowerThirdAppearance a;
  a.enabled = true;
  return a;
}

inline bool tilesNameAnchorRight(const std::string& anchor) { return anchor.find("right") != std::string::npos; }
inline bool tilesNameAnchorTop(const std::string& anchor) { return anchor.find("upper") != std::string::npos; }

// Places the name band for one tile.
//   tileRect      the tile in canvas fractions (after the wall-rect mapping)
//   gutterX/Y     the grid's gutter in canvas fractions (the inset from the
//                 tile edge, so the band keeps the same breathing room the
//                 tiles keep from each other)
//   style         the scene lower-third appearance (enabled or not; a disabled
//                 style is replaced by tilesNameDefaultAppearance())
//   canvasAspect  width/height of the output canvas (for the width check)
inline TilesNameLinePlacement placeTilesNameLine(const LayerRect& tileRect,
                                                 float gutterX,
                                                 float gutterY,
                                                 const LowerThirdAppearance& style,
                                                 double canvasAspect = 16.0 / 9.0) {
  TilesNameLinePlacement out;
  const LowerThirdAppearance base = style.enabled ? style : tilesNameDefaultAppearance();
  if (!(tileRect.width > 0.f) || !(tileRect.height > 0.f) || !std::isfinite(tileRect.width) ||
      !std::isfinite(tileRect.height) || !std::isfinite(canvasAspect) || canvasAspect <= 0.0) {
    return out;
  }
  const double tileHeightPx = double(tileRect.height) * kTilesNameReferenceHeightPx;
  const double tileWidthPx = double(tileRect.width) * kTilesNameReferenceHeightPx * canvasAspect;

  // Scale with the tile: a full-canvas tile reads exactly like the scene key,
  // a half-height tile at half the size, floored so it never shrinks below
  // readable and capped so it never exceeds the style.
  const double fontPx = std::clamp(base.nameSize * double(tileRect.height), kTilesNameMinimumFontPx, base.nameSize);
  const double scale = fontPx / base.nameSize;
  const double padPx = base.padding * scale;
  // The raster's layout formula is height = nameSize + titleSize + padding*3;
  // with no secondary line that is the whole band.
  const double bandPx = fontPx + padPx * 3.0;

  if (bandPx > tileHeightPx * kTilesNameMaxBandShareOfTile) return out;
  if (tileWidthPx < fontPx * kTilesNameMinimumTileWidthInFonts) return out;

  const float insetX = std::max(0.f, gutterX);
  const float insetY = std::max(0.f, gutterY);
  const float bandWidth = std::min(tileRect.width - 2.f * insetX, tileRect.width * float(base.width));
  const float bandHeight = float(bandPx / kTilesNameReferenceHeightPx);
  if (!(bandWidth > 0.f) || bandHeight + 2.f * insetY > tileRect.height) return out;

  const bool right = tilesNameAnchorRight(base.anchor);
  const bool top = tilesNameAnchorTop(base.anchor);
  out.rect = {right ? tileRect.x + tileRect.width - insetX - bandWidth : tileRect.x + insetX,
              top ? tileRect.y + insetY : tileRect.y + tileRect.height - insetY - bandHeight,
              bandWidth, bandHeight};

  out.appearance = base;
  out.appearance.enabled = true;
  out.appearance.nameSize = fontPx;
  out.appearance.titleSize = 0;  // top line only, never a secondary line
  out.appearance.padding = padPx;
  out.appearance.cornerRadius = base.cornerRadius * scale;
  out.appearance.showLogo = false;
  out.shown = true;
  return out;
}

// Moves a placed name band with its tile while the wall animates: the band
// keeps the offset it had from the tile's anchored corner at the SETTLED
// (target) tile rect, applied to the tile's current (sampled) rect. Size is
// not re-derived mid-flight: the text does not breathe while a tile springs
// into place, it just rides along.
inline LayerRect followTilesNameLine(const LayerRect& nameRect,
                                     const LayerRect& targetTile,
                                     const LayerRect& sampledTile,
                                     const std::string& anchor) {
  const bool right = tilesNameAnchorRight(anchor);
  const bool top = tilesNameAnchorTop(anchor);
  const float dx = right ? (targetTile.x + targetTile.width) - (nameRect.x + nameRect.width) : nameRect.x - targetTile.x;
  const float dy = top ? nameRect.y - targetTile.y : (targetTile.y + targetTile.height) - (nameRect.y + nameRect.height);
  LayerRect out = nameRect;
  out.x = right ? sampledTile.x + sampledTile.width - dx - nameRect.width : sampledTile.x + dx;
  out.y = top ? sampledTile.y + dy : sampledTile.y + sampledTile.height - dy - nameRect.height;
  return out;
}

}  // namespace corevideo::compositor
