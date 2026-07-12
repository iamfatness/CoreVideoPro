#include "modules/OverlayTileRaster.h"

#include "compositor/CompositorLayout.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace corevideo::modules {
namespace {

// Classic public-domain 5x7 ASCII font (printable 0x20..0x7e), column-major:
// 5 bytes per glyph, bit 0 = top row. Converted to the row-major bitmasks the
// raster loop consumes by glyphRowsFor().
constexpr uint8_t kFont5x7Columns[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00},  // ' '
    {0x00, 0x00, 0x5f, 0x00, 0x00},  // '!'
    {0x00, 0x07, 0x00, 0x07, 0x00},  // '"'
    {0x14, 0x7f, 0x14, 0x7f, 0x14},  // '#'
    {0x24, 0x2a, 0x7f, 0x2a, 0x12},  // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62},  // '%'
    {0x36, 0x49, 0x55, 0x22, 0x50},  // '&'
    {0x00, 0x05, 0x03, 0x00, 0x00},  // '\''
    {0x00, 0x1c, 0x22, 0x41, 0x00},  // '('
    {0x00, 0x41, 0x22, 0x1c, 0x00},  // ')'
    {0x14, 0x08, 0x3e, 0x08, 0x14},  // '*'
    {0x08, 0x08, 0x3e, 0x08, 0x08},  // '+'
    {0x00, 0x50, 0x30, 0x00, 0x00},  // ','
    {0x08, 0x08, 0x08, 0x08, 0x08},  // '-'
    {0x00, 0x60, 0x60, 0x00, 0x00},  // '.'
    {0x20, 0x10, 0x08, 0x04, 0x02},  // '/'
    {0x3e, 0x51, 0x49, 0x45, 0x3e},  // '0'
    {0x00, 0x42, 0x7f, 0x40, 0x00},  // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46},  // '2'
    {0x21, 0x41, 0x45, 0x4b, 0x31},  // '3'
    {0x18, 0x14, 0x12, 0x7f, 0x10},  // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39},  // '5'
    {0x3c, 0x4a, 0x49, 0x49, 0x30},  // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03},  // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36},  // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1e},  // '9'
    {0x00, 0x36, 0x36, 0x00, 0x00},  // ':'
    {0x00, 0x56, 0x36, 0x00, 0x00},  // ';'
    {0x08, 0x14, 0x22, 0x41, 0x00},  // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14},  // '='
    {0x00, 0x41, 0x22, 0x14, 0x08},  // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06},  // '?'
    {0x32, 0x49, 0x79, 0x41, 0x3e},  // '@'
    {0x7e, 0x11, 0x11, 0x11, 0x7e},  // 'A'
    {0x7f, 0x49, 0x49, 0x49, 0x36},  // 'B'
    {0x3e, 0x41, 0x41, 0x41, 0x22},  // 'C'
    {0x7f, 0x41, 0x41, 0x22, 0x1c},  // 'D'
    {0x7f, 0x49, 0x49, 0x49, 0x41},  // 'E'
    {0x7f, 0x09, 0x09, 0x09, 0x01},  // 'F'
    {0x3e, 0x41, 0x49, 0x49, 0x7a},  // 'G'
    {0x7f, 0x08, 0x08, 0x08, 0x7f},  // 'H'
    {0x00, 0x41, 0x7f, 0x41, 0x00},  // 'I'
    {0x20, 0x40, 0x41, 0x3f, 0x01},  // 'J'
    {0x7f, 0x08, 0x14, 0x22, 0x41},  // 'K'
    {0x7f, 0x40, 0x40, 0x40, 0x40},  // 'L'
    {0x7f, 0x02, 0x0c, 0x02, 0x7f},  // 'M'
    {0x7f, 0x04, 0x08, 0x10, 0x7f},  // 'N'
    {0x3e, 0x41, 0x41, 0x41, 0x3e},  // 'O'
    {0x7f, 0x09, 0x09, 0x09, 0x06},  // 'P'
    {0x3e, 0x41, 0x51, 0x21, 0x5e},  // 'Q'
    {0x7f, 0x09, 0x19, 0x29, 0x46},  // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31},  // 'S'
    {0x01, 0x01, 0x7f, 0x01, 0x01},  // 'T'
    {0x3f, 0x40, 0x40, 0x40, 0x3f},  // 'U'
    {0x1f, 0x20, 0x40, 0x20, 0x1f},  // 'V'
    {0x3f, 0x40, 0x38, 0x40, 0x3f},  // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63},  // 'X'
    {0x07, 0x08, 0x70, 0x08, 0x07},  // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43},  // 'Z'
    {0x00, 0x7f, 0x41, 0x41, 0x00},  // '['
    {0x02, 0x04, 0x08, 0x10, 0x20},  // '\\'
    {0x00, 0x41, 0x41, 0x7f, 0x00},  // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04},  // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40},  // '_'
    {0x00, 0x01, 0x02, 0x04, 0x00},  // '`'
    {0x20, 0x54, 0x54, 0x54, 0x78},  // 'a'
    {0x7f, 0x48, 0x44, 0x44, 0x38},  // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x20},  // 'c'
    {0x38, 0x44, 0x44, 0x48, 0x7f},  // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18},  // 'e'
    {0x08, 0x7e, 0x09, 0x01, 0x02},  // 'f'
    {0x0c, 0x52, 0x52, 0x52, 0x3e},  // 'g'
    {0x7f, 0x08, 0x04, 0x04, 0x78},  // 'h'
    {0x00, 0x44, 0x7d, 0x40, 0x00},  // 'i'
    {0x20, 0x40, 0x44, 0x3d, 0x00},  // 'j'
    {0x7f, 0x10, 0x28, 0x44, 0x00},  // 'k'
    {0x00, 0x41, 0x7f, 0x40, 0x00},  // 'l'
    {0x7c, 0x04, 0x18, 0x04, 0x78},  // 'm'
    {0x7c, 0x08, 0x04, 0x04, 0x78},  // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38},  // 'o'
    {0x7c, 0x14, 0x14, 0x14, 0x08},  // 'p'
    {0x08, 0x14, 0x14, 0x18, 0x7c},  // 'q'
    {0x7c, 0x08, 0x04, 0x04, 0x08},  // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x20},  // 's'
    {0x04, 0x3f, 0x44, 0x40, 0x20},  // 't'
    {0x3c, 0x40, 0x40, 0x20, 0x7c},  // 'u'
    {0x1c, 0x20, 0x40, 0x20, 0x1c},  // 'v'
    {0x3c, 0x40, 0x30, 0x40, 0x3c},  // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44},  // 'x'
    {0x0c, 0x50, 0x50, 0x50, 0x3c},  // 'y'
    {0x44, 0x64, 0x54, 0x4c, 0x44},  // 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00},  // '{'
    {0x00, 0x00, 0x7f, 0x00, 0x00},  // '|'
    {0x00, 0x41, 0x36, 0x08, 0x00},  // '}'
    {0x08, 0x04, 0x08, 0x10, 0x08},  // '~'
};

// Row-major cache: 95 glyphs x 7 rows of 5-bit masks (MSB = leftmost column),
// built once from the column-major table.
const std::array<std::array<uint8_t, 7>, 95>& glyphRowTable() {
  static const auto table = [] {
    std::array<std::array<uint8_t, 7>, 95> rows{};
    for (size_t glyph = 0; glyph < 95; ++glyph) {
      for (int row = 0; row < 7; ++row) {
        uint8_t bits = 0;
        for (int col = 0; col < 5; ++col) {
          if ((kFont5x7Columns[glyph][col] >> row) & 0x01u) {
            bits |= static_cast<uint8_t>(0x10u >> col);
          }
        }
        rows[glyph][static_cast<size_t>(row)] = bits;
      }
    }
    return rows;
  }();
  return table;
}

void setTilePixel(std::vector<uint8_t>& bgra, int width, int x, int y, uint32_t argb) {
  if (x < 0 || y < 0 || x >= width) {
    return;
  }
  const size_t offset = static_cast<size_t>((y * width + x) * 4);
  if (offset + 3 >= bgra.size()) {
    return;
  }
  bgra[offset + 0] = static_cast<uint8_t>(argb & 0xff);
  bgra[offset + 1] = static_cast<uint8_t>((argb >> 8) & 0xff);
  bgra[offset + 2] = static_cast<uint8_t>((argb >> 16) & 0xff);
  bgra[offset + 3] = static_cast<uint8_t>((argb >> 24) & 0xff);
}

void fillTileRect(std::vector<uint8_t>& bgra, int width, int height,
                  const OverlayTileRect& rect, uint32_t argb) {
  const int left = std::max(0, static_cast<int>(std::floor(rect.x)));
  const int top = std::max(0, static_cast<int>(std::floor(rect.y)));
  const int right = std::min(width, static_cast<int>(std::ceil(rect.x + rect.width)));
  const int bottom = std::min(height, static_cast<int>(std::ceil(rect.y + rect.height)));
  for (int py = top; py < bottom; ++py) {
    for (int px = left; px < right; ++px) {
      setTilePixel(bgra, width, px, py, argb);
    }
  }
}

// Deterministic two-tone checker derived from the imageUri, so the image
// region is non-uniform and varies by source without a platform decoder. The
// Windows DirectWrite/WIC raster replaces this with the real decoded image.
void drawTileImagePlaceholder(std::vector<uint8_t>& bgra, int width, int height,
                              const OverlayTileRect& rect, const std::string& imageUri,
                              uint32_t tintArgb) {
  uint32_t hash = 2166136261u;
  for (const unsigned char ch : imageUri) {
    hash ^= ch;
    hash *= 16777619u;
  }
  const uint32_t altColor = 0xff000000u | (hash & 0x00ffffffu);
  const int left = std::max(0, static_cast<int>(std::floor(rect.x)));
  const int top = std::max(0, static_cast<int>(std::floor(rect.y)));
  const int right = std::min(width, static_cast<int>(std::ceil(rect.x + rect.width)));
  const int bottom = std::min(height, static_cast<int>(std::ceil(rect.y + rect.height)));
  const int cell = std::max(1, (right - left) / 6);
  for (int py = top; py < bottom; ++py) {
    for (int px = left; px < right; ++px) {
      const bool alt = (((px - left) / cell) + ((py - top) / cell)) % 2 == 0;
      setTilePixel(bgra, width, px, py, alt ? tintArgb : altColor);
    }
  }
}

void hashBytes(uint64_t& hash, const void* data, size_t size) {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
}

void hashString(uint64_t& hash, const std::string& value) {
  hashBytes(hash, value.data(), value.size());
  const unsigned char separator = 0x1f;
  hashBytes(hash, &separator, 1);
}

}  // namespace

const uint8_t* overlayGlyphRows5x7(char ch) {
  static const uint8_t kBlock[7] = {0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f};
  const auto code = static_cast<unsigned char>(ch);
  if (code < 0x20 || code > 0x7e) {
    return kBlock;
  }
  return glyphRowTable()[static_cast<size_t>(code - 0x20)].data();
}

OverlayTileLayout computeOverlayTileLayout(
    const CompositorOverlayContent& overlay, int widthPx, int heightPx) {
  OverlayTileLayout layout;
  const auto width = static_cast<float>(widthPx);
  const auto height = static_cast<float>(heightPx);
  layout.backgroundArgb = compositor::parseHexColorRgba(overlay.brandBackgroundColor, 0xff0c1118u);
  layout.accentArgb = compositor::parseHexColorRgba(overlay.brandColor, 0xff44c1a1u);
  layout.accentSecondaryArgb = compositor::parseHexColorRgba(overlay.brandAccentColor, 0xfff0a85cu);
  layout.fontFamily = overlay.fontFamily;
  constexpr uint32_t kTextColor = 0xfff4f7fau;

  // Accent bar: top edge for captions, left edge for lower-thirds.
  if (overlay.isCaption) {
    layout.accentBar = {0.f, 0.f, width, height * 0.10f};
  } else {
    layout.accentBar = {0.f, 0.f, width * 0.04f, height};
  }

  // Image occupies the left portion when present; text starts after it.
  float textLeft = width * 0.06f;
  if (!overlay.imageUri.empty()) {
    layout.hasImage = true;
    const float imageW = width * 0.22f;
    const float imageH = height * 0.8f;
    layout.imageRect = {width * 0.04f, (height - imageH) * 0.5f, imageW, imageH};
    textLeft = layout.imageRect.x + layout.imageRect.width + width * 0.03f;
  }
  const float availableTextWidth = std::max(0.f, width * 0.88f - textLeft);

  if (overlay.isCaption) {
    // Caption: speaker attribution above the caption body. The speaker line
    // uses the SECONDARY brand accent so it stays visually distinct from the
    // top accent bar (the pre-tile raster relied on an incidental double-blend
    // for that distinction).
    if (!overlay.speaker.empty()) {
      layout.textLines.push_back(
          {overlay.speaker, {textLeft, height * 0.12f, availableTextWidth, height * 0.32f}, layout.accentSecondaryArgb});
    }
    layout.textLines.push_back(
        {overlay.text, {textLeft, height * 0.50f, availableTextWidth, height * 0.40f}, kTextColor});
    return layout;
  }

  // Lower-third: presenter NAME (bright, top) over their title/role (accent, below).
  // The NAME arrives in overlay.text and MUST be the primary line. Previously the layout
  // rendered overlay.title as primary and dropped the name whenever a title was present,
  // so a capture source showed its device kind ("UVC") and a participant showed only their
  // role -- never the actual name (owner: "doesn't honor the source name"). (2026-07-11)
  const std::string& name = !overlay.text.empty() ? overlay.text : overlay.title;
  if (!name.empty()) {
    layout.textLines.push_back(
        {name, {textLeft, height * 0.18f, availableTextWidth, height * 0.34f}, kTextColor});
  }
  const std::string& secondary =
      (!overlay.title.empty() && overlay.title != name) ? overlay.title : overlay.org;
  if (!secondary.empty()) {
    layout.textLines.push_back(
        {secondary, {textLeft, height * 0.56f, availableTextWidth, height * 0.28f}, layout.accentArgb});
  }
  return layout;
}

void drawOverlayTileTextLine(
    std::vector<uint8_t>& bgra,
    int widthPx,
    int heightPx,
    const OverlayTileTextLine& line) {
  if (line.text.empty() || widthPx <= 0 || heightPx <= 0) {
    return;
  }
  const float cellPxX = line.rect.width;
  const float cellPxY = line.rect.height;
  if (cellPxX <= 0.f || cellPxY <= 0.f) {
    return;
  }
  // Glyph cell: 6 columns (5 + 1 spacing) by 8 rows (7 + 1 spacing).
  const int maxGlyphsByWidth = std::max(1, static_cast<int>(cellPxX / 6.f));
  const int glyphCount = std::min(static_cast<int>(line.text.size()), maxGlyphsByWidth);
  if (glyphCount <= 0) {
    return;
  }
  const float glyphW = cellPxX / static_cast<float>(glyphCount);
  const float pixelW = glyphW / 6.f;
  const float pixelH = cellPxY / 8.f;
  for (int g = 0; g < glyphCount; ++g) {
    const uint8_t* rows = overlayGlyphRows5x7(line.text[static_cast<size_t>(g)]);
    const float glyphOriginX = line.rect.x + static_cast<float>(g) * glyphW;
    for (int row = 0; row < 7; ++row) {
      const uint8_t bits = rows[row];
      for (int col = 0; col < 5; ++col) {
        if ((bits & (0x10u >> col)) == 0) {
          continue;
        }
        const int px0 = static_cast<int>(glyphOriginX + static_cast<float>(col) * pixelW);
        const int px1 = static_cast<int>(glyphOriginX + static_cast<float>(col + 1) * pixelW);
        const int py0 = static_cast<int>(line.rect.y + static_cast<float>(row) * pixelH);
        const int py1 = static_cast<int>(line.rect.y + static_cast<float>(row + 1) * pixelH);
        for (int py = py0; py < std::max(py0 + 1, py1); ++py) {
          for (int px = px0; px < std::max(px0 + 1, px1); ++px) {
            setTilePixel(bgra, widthPx, px, py, line.colorArgb);
          }
        }
      }
    }
  }
}

bool rasterizeOverlayTileBgra(
    const CompositorOverlayContent& overlay,
    int widthPx,
    int heightPx,
    std::vector<uint8_t>& outBgra) {
  if (widthPx <= 0 || heightPx <= 0) {
    return false;
  }
  const auto layout = computeOverlayTileLayout(overlay, widthPx, heightPx);
  outBgra.assign(static_cast<size_t>(widthPx) * static_cast<size_t>(heightPx) * 4u, 0);

  // Background band covers the full tile (opaque).
  fillTileRect(outBgra, widthPx, heightPx, {0.f, 0.f, static_cast<float>(widthPx), static_cast<float>(heightPx)},
               layout.backgroundArgb);
  fillTileRect(outBgra, widthPx, heightPx, layout.accentBar, layout.accentArgb);
  if (layout.hasImage) {
    drawTileImagePlaceholder(outBgra, widthPx, heightPx, layout.imageRect, overlay.imageUri,
                             layout.accentSecondaryArgb);
  }
  for (const auto& line : layout.textLines) {
    drawOverlayTileTextLine(outBgra, widthPx, heightPx, line);
  }
  return true;
}

uint64_t overlayContentSignature(
    const CompositorOverlayContent& overlay, int widthPx, int heightPx) {
  uint64_t hash = 1469598103934665603ull;
  hashString(hash, overlay.title);
  hashString(hash, overlay.org);
  hashString(hash, overlay.text);
  hashString(hash, overlay.speaker);
  hashString(hash, overlay.imageUri);
  hashString(hash, overlay.brandColor);
  hashString(hash, overlay.brandAccentColor);
  hashString(hash, overlay.brandBackgroundColor);
  hashString(hash, overlay.fontFamily);
  const unsigned char caption = overlay.isCaption ? 1 : 0;
  hashBytes(hash, &caption, 1);
  hashBytes(hash, &widthPx, sizeof(widthPx));
  hashBytes(hash, &heightPx, sizeof(heightPx));
  return hash;
}

}  // namespace corevideo::modules
