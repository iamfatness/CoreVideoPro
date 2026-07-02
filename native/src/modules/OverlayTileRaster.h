#pragma once

#include "modules/Interfaces.h"

#include <cstdint>
#include <string>
#include <vector>

namespace corevideo::modules {

// Shared overlay/lower-third/caption tile rasterization (Item 9).
//
// Both compositor paths raster an overlay's *content* into a straight-alpha
// BGRA tile whose geometry comes from computeOverlayTileLayout(), so the CPU
// preview, the D3D11 fallback upload, and the DirectWrite/WIC raster all agree
// on placement and brand styling:
//   - the portable CPU path (ProgramFramePreview + rasterizeOverlayTileBgra)
//     uses a deterministic 5x7 bitmap font and an imageUri-derived checker;
//   - the Windows D3D11 path uploads a tile produced by DirectWrite/D2D + WIC
//     (DirectWriteOverlayRaster.h), falling back to this CPU tile when the
//     platform raster is unavailable or fails.
// The animated keyPhase transform (slide/scale/alpha) is applied by the
// compositor when the tile is composited, NOT baked into the tile, so tiles
// only re-raster when their content changes (see overlayContentSignature).

// A rect in tile pixel space.
struct OverlayTileRect {
  float x = 0.f;
  float y = 0.f;
  float width = 0.f;
  float height = 0.f;
};

// One line of overlay text (pixel-space rect + packed 0xAARRGGBB color).
struct OverlayTileTextLine {
  std::string text;
  OverlayTileRect rect;
  uint32_t colorArgb = 0xffffffffu;
};

// Resolved tile geometry + brand styling for an overlay's content, in the
// pixel space of a widthPx x heightPx tile. The background band always covers
// the full tile.
struct OverlayTileLayout {
  uint32_t backgroundArgb = 0xff0c1118u;
  uint32_t accentArgb = 0xff44c1a1u;
  uint32_t accentSecondaryArgb = 0xfff0a85cu;
  // Accent bar: left edge for lower-thirds, top edge for captions.
  OverlayTileRect accentBar;
  bool hasImage = false;
  OverlayTileRect imageRect;
  std::vector<OverlayTileTextLine> textLines;
  std::string fontFamily = "Inter";
};

// Computes the tile layout for an overlay's content. Mirrors the historical
// ProgramFramePreview band/bar/image/text fractions so preview and program
// stay in visual agreement.
OverlayTileLayout computeOverlayTileLayout(
    const CompositorOverlayContent& overlay, int widthPx, int heightPx);

// Returns the 7 row bitmasks (5 bits, MSB = leftmost column) for a printable
// ASCII glyph of the deterministic 5x7 bitmap font. Unknown characters fall
// back to a filled block so rastered text is always non-uniform.
const uint8_t* overlayGlyphRows5x7(char ch);

// Rasters the overlay's content into a straight-alpha BGRA tile
// (widthPx * heightPx * 4 bytes). The band is opaque; only out-of-band pixels
// (none in the current layout) would be transparent. Returns false when the
// tile dimensions are not positive.
bool rasterizeOverlayTileBgra(
    const CompositorOverlayContent& overlay,
    int widthPx,
    int heightPx,
    std::vector<uint8_t>& outBgra);

// Draws a single line of text into a BGRA tile with the 5x7 bitmap font,
// scaling the glyph cell to the rect. Exposed for the preview path and tests.
void drawOverlayTileTextLine(
    std::vector<uint8_t>& bgra,
    int widthPx,
    int heightPx,
    const OverlayTileTextLine& line);

// Stable FNV-1a signature over everything that changes the *content* of the
// rastered tile (text, image, brand styling, tile size). Deliberately excludes
// keyPhase/keyProgress/keyPosition/keyer: animation is a composite-time
// transform, so a tile stays cached across an entire build-in/out sweep.
uint64_t overlayContentSignature(
    const CompositorOverlayContent& overlay, int widthPx, int heightPx);

}  // namespace corevideo::modules
