#include "modules/OverlayTileRaster.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

using corevideo::modules::CompositorOverlayContent;
using corevideo::modules::computeOverlayTileLayout;
using corevideo::modules::overlayContentSignature;
using corevideo::modules::overlayGlyphRows5x7;
using corevideo::modules::rasterizeOverlayTileBgra;

constexpr int kTileW = 320;
constexpr int kTileH = 64;

CompositorOverlayContent makeLowerThird(const std::string& title, const std::string& org) {
  CompositorOverlayContent overlay;
  overlay.title = title;
  overlay.org = org;
  return overlay;
}

int distinctColors(const std::vector<uint8_t>& bgra) {
  std::set<uint32_t> colors;
  for (size_t offset = 0; offset + 3 < bgra.size(); offset += 4) {
    uint32_t packed = 0;
    std::memcpy(&packed, bgra.data() + offset, 4);
    colors.insert(packed);
  }
  return static_cast<int>(colors.size());
}

int countColor(const std::vector<uint8_t>& bgra, uint8_t b, uint8_t g, uint8_t r) {
  int count = 0;
  for (size_t offset = 0; offset + 3 < bgra.size(); offset += 4) {
    if (bgra[offset] == b && bgra[offset + 1] == g && bgra[offset + 2] == r) {
      ++count;
    }
  }
  return count;
}

// --- Font ---

TEST(OverlayTileRaster, FullAsciiFontHasDistinctGlyphs) {
  // Every printable ASCII glyph must be drawable, and non-identical characters
  // must produce non-identical shapes (the old 7-glyph subset rendered most
  // consonants as the same filled block).
  const uint8_t* block = overlayGlyphRows5x7(static_cast<char>(0x7f));
  int blockLookalikes = 0;
  for (char ch = 0x21; ch <= 0x7e; ++ch) {
    const uint8_t* rows = overlayGlyphRows5x7(ch);
    if (std::memcmp(rows, block, 7) == 0) {
      ++blockLookalikes;
    }
  }
  EXPECT_EQ(blockLookalikes, 0) << "printable glyphs must not fall back to the block";

  // Spot-check that visually distinct letters differ.
  EXPECT_NE(0, std::memcmp(overlayGlyphRows5x7('B'), overlayGlyphRows5x7('D'), 7));
  EXPECT_NE(0, std::memcmp(overlayGlyphRows5x7('H'), overlayGlyphRows5x7('W'), 7));
  EXPECT_NE(0, std::memcmp(overlayGlyphRows5x7('l'), overlayGlyphRows5x7('L'), 7));
  EXPECT_NE(0, std::memcmp(overlayGlyphRows5x7('0'), overlayGlyphRows5x7('O'), 7));
}

TEST(OverlayTileRaster, SpaceIsEmptyAndNonAsciiFallsBackToBlock) {
  const uint8_t* space = overlayGlyphRows5x7(' ');
  for (int row = 0; row < 7; ++row) {
    EXPECT_EQ(space[row], 0x00);
  }
  const uint8_t* nonAscii = overlayGlyphRows5x7(static_cast<char>(0xc3));
  for (int row = 0; row < 7; ++row) {
    EXPECT_EQ(nonAscii[row], 0x1f);
  }
}

// --- Tile raster ---

TEST(OverlayTileRaster, ConsonantOnlyTitlesRenderDistinctTiles) {
  // Regression for the 7-glyph font: "HRDWR" and "WLDCT" share zero glyphs
  // with the old A/E/I/O/T subset, so both previously rastered as identical
  // rows of blocks. With the full font the tiles must differ.
  std::vector<uint8_t> first;
  std::vector<uint8_t> second;
  ASSERT_TRUE(rasterizeOverlayTileBgra(makeLowerThird("HRDWR", ""), kTileW, kTileH, first));
  ASSERT_TRUE(rasterizeOverlayTileBgra(makeLowerThird("WLDCT", ""), kTileW, kTileH, second));
  EXPECT_NE(first, second);
}

TEST(OverlayTileRaster, TitleAndOrgLinesRenderTextPixels) {
  std::vector<uint8_t> tile;
  ASSERT_TRUE(rasterizeOverlayTileBgra(
      makeLowerThird("Ada Lovelace", "CoreVideo Pro"), kTileW, kTileH, tile));
  // Band + accent bar + bright title text + accent org text.
  EXPECT_GE(distinctColors(tile), 3);
  // Title text color is #f4f7fa -> BGRA fa f7 f4.
  EXPECT_GE(countColor(tile, 0xfa, 0xf7, 0xf4), 1);
}

TEST(OverlayTileRaster, BrandColorsStyleTheTile) {
  CompositorOverlayContent overlay = makeLowerThird("Name", "Org");
  overlay.brandColor = "#ff0000";
  overlay.brandBackgroundColor = "#0000ff";
  std::vector<uint8_t> tile;
  ASSERT_TRUE(rasterizeOverlayTileBgra(overlay, kTileW, kTileH, tile));
  // Background #0000ff -> BGRA ff 00 00; accent #ff0000 -> BGRA 00 00 ff.
  EXPECT_GE(countColor(tile, 0xff, 0x00, 0x00), 1);
  EXPECT_GE(countColor(tile, 0x00, 0x00, 0xff), 1);
}

TEST(OverlayTileRaster, CaptionRendersSpeakerAndBody) {
  CompositorOverlayContent overlay;
  overlay.isCaption = true;
  overlay.text = "The quick brown fox";
  overlay.speaker = "Grace";
  std::vector<uint8_t> withSpeaker;
  ASSERT_TRUE(rasterizeOverlayTileBgra(overlay, kTileW, kTileH, withSpeaker));
  overlay.speaker.clear();
  std::vector<uint8_t> withoutSpeaker;
  ASSERT_TRUE(rasterizeOverlayTileBgra(overlay, kTileW, kTileH, withoutSpeaker));
  EXPECT_NE(withSpeaker, withoutSpeaker);
  EXPECT_GE(distinctColors(withSpeaker), 3);
}

TEST(OverlayTileRaster, ImagePlaceholderIsDeterministicPerUri) {
  CompositorOverlayContent overlay = makeLowerThird("Name", "");
  overlay.imageUri = "file:///assets/logo-a.png";
  std::vector<uint8_t> first;
  std::vector<uint8_t> again;
  ASSERT_TRUE(rasterizeOverlayTileBgra(overlay, kTileW, kTileH, first));
  ASSERT_TRUE(rasterizeOverlayTileBgra(overlay, kTileW, kTileH, again));
  EXPECT_EQ(first, again);
  overlay.imageUri = "file:///assets/logo-b.png";
  std::vector<uint8_t> other;
  ASSERT_TRUE(rasterizeOverlayTileBgra(overlay, kTileW, kTileH, other));
  EXPECT_NE(first, other);
}

TEST(OverlayTileRaster, TileIsFullyOpaque) {
  std::vector<uint8_t> tile;
  ASSERT_TRUE(rasterizeOverlayTileBgra(makeLowerThird("Name", "Org"), kTileW, kTileH, tile));
  int transparentBytes = 0;
  for (size_t offset = 3; offset < tile.size(); offset += 4) {
    if (tile[offset] != 0xff) {
      ++transparentBytes;
    }
  }
  EXPECT_EQ(transparentBytes, 0);
}

TEST(OverlayTileRaster, RejectsEmptyTiles) {
  std::vector<uint8_t> tile;
  EXPECT_FALSE(rasterizeOverlayTileBgra(makeLowerThird("Name", ""), 0, kTileH, tile));
  EXPECT_FALSE(rasterizeOverlayTileBgra(makeLowerThird("Name", ""), kTileW, -1, tile));
}

// --- Layout ---

TEST(OverlayTileRaster, LayoutPlacesAccentBarByKind) {
  CompositorOverlayContent lowerThird = makeLowerThird("Name", "Org");
  const auto ltLayout = computeOverlayTileLayout(lowerThird, kTileW, kTileH);
  EXPECT_TRUE(std::fabs(ltLayout.accentBar.width - kTileW * 0.04f) < 1e-3f);
  EXPECT_TRUE(std::fabs(ltLayout.accentBar.height - static_cast<float>(kTileH)) < 1e-3f);

  CompositorOverlayContent caption;
  caption.isCaption = true;
  caption.text = "body";
  const auto capLayout = computeOverlayTileLayout(caption, kTileW, kTileH);
  EXPECT_TRUE(std::fabs(capLayout.accentBar.width - static_cast<float>(kTileW)) < 1e-3f);
  EXPECT_TRUE(std::fabs(capLayout.accentBar.height - kTileH * 0.10f) < 1e-3f);
}

TEST(OverlayTileRaster, LayoutShiftsTextAfterImage) {
  CompositorOverlayContent overlay = makeLowerThird("Name", "");
  const auto withoutImage = computeOverlayTileLayout(overlay, kTileW, kTileH);
  overlay.imageUri = "file:///logo.png";
  const auto withImage = computeOverlayTileLayout(overlay, kTileW, kTileH);
  ASSERT_TRUE(withImage.hasImage);
  ASSERT_FALSE(withoutImage.hasImage);
  ASSERT_FALSE(withImage.textLines.empty());
  ASSERT_FALSE(withoutImage.textLines.empty());
  EXPECT_TRUE(withImage.textLines[0].rect.x > withoutImage.textLines[0].rect.x);
}

// --- Signature ---

TEST(OverlayTileRaster, SignatureTracksContentNotAnimation) {
  CompositorOverlayContent overlay = makeLowerThird("Name", "Org");
  const uint64_t base = overlayContentSignature(overlay, kTileW, kTileH);

  // Animation state must NOT invalidate a cached tile.
  overlay.keyPhase = "building-in";
  overlay.keyProgress = 0.25f;
  overlay.keyPosition = "upper-left";
  overlay.keyer = "upstream";
  EXPECT_EQ(base, overlayContentSignature(overlay, kTileW, kTileH));

  // Content changes must.
  CompositorOverlayContent changedText = makeLowerThird("Other", "Org");
  EXPECT_NE(base, overlayContentSignature(changedText, kTileW, kTileH));
  CompositorOverlayContent changedBrand = makeLowerThird("Name", "Org");
  changedBrand.brandColor = "#123456";
  EXPECT_NE(base, overlayContentSignature(changedBrand, kTileW, kTileH));
  EXPECT_NE(base, overlayContentSignature(makeLowerThird("Name", "Org"), kTileW / 2, kTileH));
}

}  // namespace
